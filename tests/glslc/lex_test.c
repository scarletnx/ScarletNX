/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* glslc's preprocessor and tokens (src/glslc/lexer.c): what comes out of a source, the errors it gives with their
   places, and the bounds it keeps (glslc.h), each refused with the bound and how much the source needed */
#include "glslc/internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GlslcResult result;
    char text[4096]; /* the tokens, each followed by a space */
    char big[70000]; /* a source past a bound */
    void* workspace; /* one for every compilation of the test, as a caller keeps one per thread */
    size_t workspace_size;
} LexTest;

static LexTest g_lex_test;

/* the tokens source preprocesses to, or "ERROR line:column: message" */
static const char* lex(GlslcStage stage, const char* source, size_t size) {
    GlslcResult* r = &g_lex_test.result;
    memset(r, 0, sizeof *r);
    Glslc* g = glslc_open(g_lex_test.workspace, g_lex_test.workspace_size, stage, source, size, r);
    assert(g);
    size_t w = 0;
    if (glslc_run(g, GLSLC_PASS_LEX, NULL, 0)) {
        snprintf(g_lex_test.text, sizeof g_lex_test.text, "ERROR %u:%u: %s", r->line, r->column, r->error);
    } else {
        g_lex_test.text[0] = 0;
        for (uint32_t i = 0; i + 1 < g->ntoks && w + 1 < sizeof g_lex_test.text; i++) {
            const GlslcToken* t = &g->toks[i];
            int k = snprintf(g_lex_test.text + w, sizeof g_lex_test.text - w, "%.*s ", (int)t->text.len,
                             glslc_token_text(g, t));
            if (k < 0) break;
            w += (size_t)k;
        }
    }
    return g_lex_test.text;
}

static void expect(const char* source, const char* want) {
    const char* got = lex(SNX_GLSLC_FRAGMENT, source, strlen(source));
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "source:\n%s\nwant: [%s]\ngot:  [%s]\n", source, want, got);
        exit(1);
    }
}

/* an error whose message holds want, at line:column */
static void expect_error(GlslcStage stage, const char* source, size_t size, uint32_t line, uint32_t column,
                         const char* want) {
    const char* got = lex(stage, source, size);
    const GlslcResult* r = &g_lex_test.result;
    if (strncmp(got, "ERROR", 5) != 0 || !strstr(r->error, want) || r->line != line || r->column != column) {
        fprintf(stderr, "source:\n%.200s\nwant: ERROR %u:%u: ...%s...\ngot:  %s\n", source, line, column, want, got);
        exit(1);
    }
}

static void tokens(void) {
    expect("#version 300 es\nfloat x = 1.5e3f + .5 + 2. + 0x1Fu + 017;\n",
           "float x = 1.5e3f + .5 + 2. + 0x1Fu + 017 ; ");
    expect("#version 300 es\na<<=b>>=c^^d&&e||f!=g==h<=i>=j++--~!%=%\n",
           "a <<= b >>= c ^^ d && e || f != g == h <= i >= j ++ -- ~ ! %= % ");
    /* comments, a line continuation between tokens (3.00), and the white space GLSL ES has: \v and \f too */
    expect("#version 300 es\na /* b\n c */ d // e\\\n f\n\vg\fh\r\ni \\\nj\n", "a d g h i j ");
    /* a carriage return alone ends a line: the directive after it is one */
    expect("#version 300 es\r#define A 1\rA\r", "1 ");
}

static void macros(void) {
    expect(
        "#version 300 es\n#define N 4\n#define SQ(x) ((x) * (x))\n#define MAD(a, b, c) ((a) * (b) + (c))\n"
        "#define TWICE(x) MAD(x, 2, 0)\nSQ(N) TWICE(SQ(1)) SQ(SQ(2))\n",
        "( ( 4 ) * ( 4 ) ) ( ( ( ( 1 ) * ( 1 ) ) ) * ( 2 ) + ( 0 ) ) ( ( ( ( 2 ) * ( 2 ) ) ) * ( ( ( 2 ) * ( 2 ) ) ) ) ");
    /* a macro is off inside its own expansion */
    expect("#version 300 es\n#define R R + 1\n#define A B\n#define B A\nR A\n", "R + 1 A ");
    /* a function-like name without ( is a name; ( after a space is still a call */
    expect("#version 300 es\n#define F(x) [x]\nF F (1) F()\n", "F [ 1 ] [ ] ");
    /* the same definition again changes nothing; #undef, then a new one */
    expect("#version 300 es\n#define A 1 + 2\n#define A 1 + 2\nA\n#undef A\n#define A 3\nA\n", "1 + 2 3 ");
    expect("#version 300 es\nint l = __LINE__; int v = __VERSION__;\n#line 41\nint m = __LINE__ + GL_ES;\n",
           "int l = 2 ; int v = 300 ; int m = 41 + 1 ; ");
    expect("int v = __VERSION__;\n", "int v = 100 ; ");
}

static void conditionals(void) {
    expect("#version 300 es\n#define N 4\n#if defined(N) && N > 3 && !defined(FOO) && (7 % 4) == 3 && (1 << 4) == 16\n"
           "yes\n#else\nno\n#endif\n",
           "yes ");
    /* an undefined name is fine where && or || leaves it out */
    expect("#version 300 es\n#if defined(FOO) && FOO > 1\nno\n#elif 0 || 1\nelif\n#else\nelse\n#endif\n", "elif ");
    expect("#version 300 es\n#ifdef GL_ES\na\n#endif\n#ifndef GL_ES\nb\n#else\nc\n#endif\n", "a c ");
    /* what a left-out group holds is not looked at, but its #if nesting is */
    expect("#version 300 es\n#if 0\n@ ' \"\n#if 1\nx\n#else\ny\n#endif\n#unknown\n#endif\nz\n", "z ");
    expect("#version 300 es\n#extension GL_OES_standard_derivatives : enable\n#pragma optimize(off)\n#pragma "
           "STDGL invariant(all)\nx\n",
           "x ");
}

static void keywords(void) {
    /* the table is sorted, for the binary search */
    for (uint32_t i = 1; i < glslc_keyword_count; i++)
        assert(strcmp(glslc_keywords[i - 1].name, glslc_keywords[i].name) < 0);
    for (uint32_t i = 0; i < glslc_keyword_count; i++) {
        const GlslcKeyword* k = glslc_find_keyword(glslc_keywords[i].name, (uint32_t)strlen(glslc_keywords[i].name));
        assert(k == &glslc_keywords[i]);
    }
    assert(!glslc_find_keyword("vec5", 4) && !glslc_find_keyword("", 0) && !glslc_find_keyword("zzz", 3));
    /* uint is a name in 1.00 and a type from 3.00; attribute is reserved from 3.00; switch is reserved in 1.00 */
    GlslcResult* r = &g_lex_test.result;
    static const struct {
        const char* source;
        uint8_t kind;
    } cases[] = {
        {"uint", TK_IDENT},
        {"#version 300 es\nuint", TK_TYPE + GT_UINT - 1},
        {"#version 300 es\nattribute", TK_RESERVED},
        {"attribute", TK_ATTRIBUTE},
        {"switch", TK_RESERVED},
        {"#version 300 es\nswitch", TK_SWITCH},
        {"#version 300 es\nbuffer", TK_IDENT},
        {"#version 310 es\nbuffer", TK_BUFFER},
        {"#version 310 es\nsamplerBuffer", TK_RESERVED},
        {"#version 310 es\n#extension GL_EXT_texture_buffer : enable\nsamplerBuffer",
         TK_TYPE + GT_SAMPLER_FIRST + 21 - 1},
        {"sampler3D", TK_RESERVED},
        {"#extension GL_OES_texture_3D : enable\nsampler3D", TK_TYPE + GT_SAMPLER_FIRST + 2 - 1},
        {"#version 300 es\nmat2x2", TK_TYPE + GT_MAT2 - 1},
        {"#version 320 es\ngoto", TK_RESERVED},
    };
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memset(r, 0, sizeof *r);
        Glslc* g = glslc_open(g_lex_test.workspace, g_lex_test.workspace_size, SNX_GLSLC_FRAGMENT, cases[i].source,
                              strlen(cases[i].source), r);
        assert(g && glslc_run(g, GLSLC_PASS_LEX, NULL, 0) == 0);
        if (g->toks[0].kind != cases[i].kind) {
            fprintf(stderr, "%s: kind %u, want %u\n", cases[i].source, g->toks[0].kind, cases[i].kind);
            exit(1);
        }
    }
}

static void errors(void) {
    GlslcStage f = SNX_GLSLC_FRAGMENT;
#define E(source, line, column, want) expect_error(f, source, sizeof source - 1, line, column, want)
    E("#version 300 es\n#if FOO\n#endif\n", 2, 5, "'FOO' is not a defined macro");
    E("#version 300 es\n#if 1\n", 2, 1, "has no #endif");
    E("#version 300 es\n#endif\n", 2, 1, "#endif without #if");
    E("#version 300 es\n#if 1\n#else\n#else\n#endif\n", 4, 1, "#else after #else");
    E("#version 300 es\n#if 1 / 0\n#endif\n", 2, 7, "divides by zero");
    E("#version 300 es\n#define F(a) a\nF(1, 2)\n", 3, 1, "takes 1 argument, and is given 2");
    E("#version 300 es\n#define F(a) a\nF(1\n", 3, 1, "has no ) before the end");
    E("#version 300 es\n#define A 1\n#define A 2\n", 3, 9, "defined again with another body");
    E("#version 300 es\n#define GL_X 1\n", 2, 9, "starting with GL_ are reserved");
    E("#version 300 es\n#undef __LINE__\n", 2, 8, "predefined macro");
    E("#version 300 es\n#define C(a, b) a ## b\n", 2, 19, "token pasting (##) is not in GLSL ES");
    E("#version 300 es\n#define S(a) #a\n", 2, 14, "the # operator (stringizing)");
    E("#version 300 es\n#error stop here  \n", 2, 1, "#error stop here");
    E("#version 300 es\n#extension GL_FOO_bar : require\n", 2, 12, "not one glslc supports");
    E("#version 300 es\n#extension all : enable\n", 2, 18, "takes warn or disable");
    E("#version 300 es\n#foo\n", 2, 2, "'#foo' is not a directive");
    E("#version 300 es\nint a; # x\n", 2, 8, "only a directive at the start of a line");
    E("int a;\n#version 300 es\n", 2, 1, "#version must come before anything");
    E("#version 330\n", 1, 10, "not a GLSL ES version");
    E("#version 300\n", 1, 10, "desktop GLSL");
    E("#version 100 es\n", 1, 14, "takes no profile");
    E("#version 200 es\n", 1, 10, "not a GLSL ES version");
    E("#version 300 es\n/* open\n", 2, 1, "has no */");
    E("#version 300 es\nint a = 019;\n", 2, 9, "it is octal");
    E("#version 300 es\nint a = 1x;\n", 2, 9, "'1x' is not a number");
    E("#version 300 es\nfloat a = 1e;\n", 2, 11, "has no digits");
    E("#version 300 es\nint a = @;\n", 2, 9, "the character '@'");
    E("int a = 1u;\n", 1, 9, "the suffix u needs GLSL ES 3.00");
    E("float a = 1.0f;\n", 1, 11, "the suffix f needs GLSL ES 3.00");
    E("#define A 1 \\\n + 2\n", 1, 13, "line continuation");
    E("#version 300 es\nint a = 1 \\\n;\nfl\\\noat b;\n", 4, 3, "a line continuation inside a name or number");
    E("#version 300 es\nint a = 1 \\ 2;\n", 2, 11, "a backslash is only a line continuation");
    E("#version 300 es\n#line 7\n\n#if X\n#endif\n", 8, 5, "'X' is not a defined macro");
    expect_error(SNX_GLSLC_COMPUTE, "#version 300 es\n", 16, 1, 1, "a compute shader needs #version 310 es");
#undef E
    /* a warning, with its place */
    lex(f, "#version 300 es\n#extension GL_FOO_bar : enable\n#define A__B 1\n", 63);
    assert(g_lex_test.result.warnings == 2 && strstr(g_lex_test.result.warning, "line 2, column 12") &&
           strstr(g_lex_test.result.warning, "GL_FOO_bar"));
}

/* each bound refused with the bound and what the source needed */
static void bounds(void) {
    char* big = g_lex_test.big;
    GlslcResult* r = &g_lex_test.result;
    /* the source itself, refused before the workspace is touched */
    memset(big, ' ', SNX_GLSLC_MAX_SOURCE + 1);
    memset(r, 0, sizeof *r);
    assert(!glslc_open(g_lex_test.workspace, g_lex_test.workspace_size, SNX_GLSLC_FRAGMENT, big,
                       SNX_GLSLC_MAX_SOURCE + 1, r));
    assert(
        !strcmp(r->error, "the source is 65536 bytes, more than the 65535 glslc takes (a token's place is 16 bits)"));
    memset(r, 0, sizeof *r);
    Glslc* g =
        glslc_open(g_lex_test.workspace, g_lex_test.workspace_size, SNX_GLSLC_FRAGMENT, big, SNX_GLSLC_MAX_SOURCE, r);
    assert(g && glslc_run(g, GLSLC_PASS_LEX, NULL, 0) == 0 && g->ntoks == 1);
    /* tokens: 32768 is the bound, the end of the source included */
    size_t n = (size_t)snprintf(big, sizeof g_lex_test.big, "#version 300 es\n");
    for (uint32_t i = 0; i < 32767; i++) big[n++] = ';'; /* one token per byte */
    big[n] = 0;
    assert(strncmp(lex(SNX_GLSLC_FRAGMENT, big, n), "ERROR", 5) != 0);
    big[n++] = ';';
    expect_error(SNX_GLSLC_FRAGMENT, big, n, 2, 32769, "has 32769 tokens after preprocessing, more than the 32768");
    /* a macro's expansion */
    n = (size_t)snprintf(big, sizeof g_lex_test.big,
                         "#version 300 es\n#define A a a a a a a a a\n#define B A A A A A A A "
                         "A\n#define C B B B B B B B B\n#define D C C C C C C C C\nD\n");
    expect_error(SNX_GLSLC_FRAGMENT, big, n, 6, 1, "expands to more than the 4096 tokens");
    /* macros inside macros */
    n = (size_t)snprintf(big, sizeof g_lex_test.big, "#version 300 es\n");
    for (uint32_t i = 0; i < 40; i++)
        n += (size_t)snprintf(big + n, sizeof g_lex_test.big - n, "#define M%u M%u\n", i, i + 1);
    n += (size_t)snprintf(big + n, sizeof g_lex_test.big - n, "M0\n");
    expect_error(SNX_GLSLC_FRAGMENT, big, n, 42, 1, "more than 32 deep");
    /* #if nesting */
    n = (size_t)snprintf(big, sizeof g_lex_test.big, "#version 300 es\n");
    for (uint32_t i = 0; i < 65; i++) n += (size_t)snprintf(big + n, sizeof g_lex_test.big - n, "#if 1\n");
    expect_error(SNX_GLSLC_FRAGMENT, big, n, 66, 1, "nest more than 64 deep");
    /* the #if expression's depth */
    n = (size_t)snprintf(big, sizeof g_lex_test.big, "#version 300 es\n#if ");
    for (uint32_t i = 0; i < 100; i++) big[n++] = '(';
    big[n++] = '1';
    big[n] = 0;
    expect_error(SNX_GLSLC_FRAGMENT, big, n, 2, 101, "nests more than 96 deep");
    /* #line directives */
    n = (size_t)snprintf(big, sizeof g_lex_test.big, "#version 300 es\n");
    for (uint32_t i = 0; i < 257; i++) n += (size_t)snprintf(big + n, sizeof g_lex_test.big - n, "#line %u\n", i + 10);
    expect_error(SNX_GLSLC_FRAGMENT, big, n, 265, 1, "more than the 256 #line directives"); /* numbered by the last */
    /* macro definitions */
    n = (size_t)snprintf(big, sizeof g_lex_test.big, "#version 300 es\n");
    for (uint32_t i = 0; i < 520; i++) n += (size_t)snprintf(big + n, sizeof g_lex_test.big - n, "#define M%u\n", i);
    expect_error(SNX_GLSLC_FRAGMENT, big, n, 2 + SNX_GLSLC_MAX_MACROS - 1 - 5 - glslc_extension_count, 9,
                 "more than the 511 macros");
}

int main(void) {
    g_lex_test.workspace_size = snx_glslc_workspace_size();
    g_lex_test.workspace = malloc(g_lex_test.workspace_size);
    assert(g_lex_test.workspace);
    tokens();
    macros();
    conditionals();
    keywords();
    errors();
    bounds();
    free(g_lex_test.workspace);
    printf("glslc_lex: tokens, macros, conditionals, keywords, errors and bounds checked\n");
    return 0;
}
