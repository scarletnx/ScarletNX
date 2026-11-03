/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* glslc's parser and type checks (src/glslc/parser.c): the bounds it keeps (glslc.h), each refused with the bound,
   float literals read to the nearest float whatever the C locale, and constant expressions folded where array sizes
   need them. tests/glslc/errors.glsl holds the language's error cases, compared with glslangValidator's */
#include "glslc/internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GlslcResult result;
    char big[70000];
} ParseTest;

static ParseTest g_parse_test;

/* 0 when source parses; otherwise the error, which must hold want at line:column */
static void expect_error(GlslcStage stage, const char* source, size_t size, uint32_t line, uint32_t column,
                         const char* want) {
    GlslcResult* r = &g_parse_test.result;
    memset(r, 0, sizeof *r);
    Glslc* g = glslc_open(stage, source, size, r);
    assert(g);
    int failed = glslc_run(g, GLSLC_PASS_PARSE, NULL, 0) != 0;
    glslc_close(g);
    if (!failed || !strstr(r->error, want) || r->line != line || r->column != column) {
        fprintf(stderr, "source: %.160s...\nwant: %u:%u: ...%s...\ngot:  %s%u:%u: %s\n", source, line, column, want,
                failed ? "" : "(no error) ", r->line, r->column, r->error);
        exit(1);
    }
}

/* the bits of the float constant the literal text becomes */
static uint32_t literal_bits(const char* text) {
    char source[256];
    int n = snprintf(source, sizeof source,
                     "#version 300 es\nprecision highp float;\nconst float x = %s;\nvoid main() {}\n", text);
    GlslcResult* r = &g_parse_test.result;
    memset(r, 0, sizeof *r);
    Glslc* g = glslc_open(SNX_GLSLC_FRAGMENT, source, (size_t)n, r);
    assert(g && glslc_run(g, GLSLC_PASS_PARSE, NULL, 0) == 0);
    const GlslcNode* decl = &g->nodes[g->first_decl];
    assert(decl->kind == GLSLC_N_GLOBAL && decl->b);
    const GlslcNode* init = &g->nodes[decl->b];
    assert(init->kind == GLSLC_N_CONST);
    uint32_t bits = init->a | ((uint32_t)init->b << 16);
    glslc_close(g);
    return bits;
}

static void literals(void) {
    static const struct {
        const char* text;
        uint32_t bits;
    } cases[] = {
        {"1.5", 0x3fc00000u},
        {"0.1", 0x3dcccccdu},
        {".5e1", 0x40a00000u},
        {"3.4028235e38", 0x7f7fffffu},
        {"1e39", 0x7f800000u},
        {"1e-45", 0x00000001u},
        {"0.0", 0x00000000u},
        {"16777217.0", 0x4b800000u},
        {"0.333333333333333333333333", 0x3eaaaaabu},
        {"123456789012345678901234567890.0", 0x6fc77488u},
        {"2.5e-3", 0x3b23d70au},
        {"-1.0", 0xbf800000u},
        {"1e-50", 0x00000000u},
    };
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t bits = literal_bits(cases[i].text);
        if (bits != cases[i].bits) {
            fprintf(stderr, "%s: bits 0x%08x, want 0x%08x\n", cases[i].text, bits, cases[i].bits);
            exit(1);
        }
    }
}

static void folding(void) {
    static const char source[] = "#version 310 es\nprecision highp float;\nconst int N = 3 * 4 - (7 >> 1);\n"
                                 "const ivec2 S = ivec2(2, N);\nconst uint U = 0xffffffffu / 3u;\n"
                                 "float a[N + S.y + int(U % 7u) + int(-2.5) + int(true)];\nvoid main() {}\n";
    GlslcResult* r = &g_parse_test.result;
    memset(r, 0, sizeof *r);
    Glslc* g = glslc_open(SNX_GLSLC_FRAGMENT, source, sizeof source - 1, r);
    assert(g);
    if (glslc_run(g, GLSLC_PASS_PARSE, NULL, 0)) {
        fprintf(stderr, "folding: %u:%u: %s\n", r->line, r->column, r->error);
        exit(1);
    }
    /* N = 9, S.y = 9, U % 7 = 1431655765 % 7 = 1, int(-2.5) = -2, int(true) = 1 */
    uint32_t a = 0;
    for (uint32_t d = g->first_decl; d; d = g->nodes[d].next) {
        if (g->nodes[d].kind == GLSLC_N_GLOBAL && glslc_view_is(g, g->syms[g->nodes[d].a].name, "a")) a = g->nodes[d].a;
    }
    assert(a && g->types[g->syms[a].type].kind == GLSLC_T_ARRAY);
    if (g->types[g->syms[a].type].len != 18) {
        fprintf(stderr, "folding: a has %u elements, want 18\n", g->types[g->syms[a].type].len);
        exit(1);
    }
    glslc_close(g);
}

static size_t start(char* big, const char* head) { return (size_t)snprintf(big, sizeof g_parse_test.big, "%s", head); }

#define ADD(...) (n += (size_t)snprintf(big + n, sizeof g_parse_test.big - n, __VA_ARGS__))

static void bounds(void) {
    char* big = g_parse_test.big;
    GlslcStage f = SNX_GLSLC_FRAGMENT;
    size_t n;
    /* syntax nodes: no construct makes more than it has tokens, so a sum just under the token bound still parses
       (a node per operand, one per operator) */
    n = start(big, "#version 300 es\nprecision highp float;\nfloat a = 1.0;\nvoid main() { float b = a");
    while (n < 32700) ADD("+a"); /* a token per byte */
    ADD(";}\n");
    {
        GlslcResult* r = &g_parse_test.result;
        memset(r, 0, sizeof *r);
        Glslc* g = glslc_open(f, big, n, r);
        assert(g && glslc_run(g, GLSLC_PASS_PARSE, NULL, 0) == 0);
        assert(g->ntoks > 32600 && g->nnodes <= g->ntoks);
        glslc_close(g);
    }
    /* nesting: parentheses */
    n = start(big, "#version 300 es\nvoid main() { int b = ");
    for (uint32_t i = 0; i < 100; i++) ADD("(");
    ADD("1");
    for (uint32_t i = 0; i < 100; i++) ADD(")");
    ADD(";}\n");
    expect_error(f, big, n, 2, 23 + 95, "nest more than the 96 deep"); /* the statement and initializer take one each */
    /* scopes: the global one, the function's, then 62 blocks */
    n = start(big, "#version 300 es\nvoid main() ");
    for (uint32_t i = 0; i < 70; i++) ADD("{");
    for (uint32_t i = 0; i < 70; i++) ADD("}");
    ADD("\n");
    expect_error(f, big, n, 2, 13 + 63, "scopes nest more than the 64 deep");
    /* names */
    n = start(big, "#version 300 es\nprecision highp float;\n");
    for (uint32_t i = 0; i < 4100; i++) ADD("float v%u;\n", i);
    expect_error(f, big, n, 3 + 4095, 7, "more than the 4095 names");
    /* types: one array type per size */
    n = start(big, "#version 300 es\nprecision highp float;\n");
    for (uint32_t i = 1; i < 2100; i++) ADD("float a%u[%u];\n", i, i);
    expect_error(f, big, n, 2 + 2048 - 73, 12, "more than the 2048 types"); /* after the 74 built in, 0 included */
    /* the members of every struct */
    n = start(big, "#version 300 es\nprecision highp float;\nstruct S {\n");
    for (uint32_t i = 0; i < 2100; i++) ADD("float m%u;\n", i);
    ADD("};\n");
    expect_error(f, big, n, 4 + 2047, 7, "more than the 2047 members");
    /* the parameters of every function */
    n = start(big, "#version 300 es\nprecision highp float;\n");
    for (uint32_t i = 0; i < 60; i++) {
        ADD("void f%u(", i);
        for (uint32_t p = 0; p < 40; p++) ADD("%sfloat", p ? "," : "");
        ADD(");\n");
    }
    expect_error(f, big, n, 3 + 51, 10 + 8 * 6, "more than the 2048 parameters"); /* the 2049th: f51's 9th */
    /* an array's elements and dimensions */
    expect_error(f, "#version 310 es\nfloat a[65536];\n", 31, 2, 9, "an array of 65536 elements");
    expect_error(f, "#version 310 es\nfloat a[1][1][1][1][1][1][1][1][1];\n", 51, 2, 32, "more than the 8 dimensions");
    expect_error(f, "#version 300 es\nfloat a[1][1];\n", 30, 2, 11, "an array of arrays needs GLSL ES 3.10");
    expect_error(f, "#version 300 es\nfloat a[0];\n", 27, 2, 9, "an array of 0 elements");
}

int main(void) {
    literals();
    folding();
    bounds();
    printf("glslc_parse: literals, folding and bounds checked\n");
    return 0;
}
