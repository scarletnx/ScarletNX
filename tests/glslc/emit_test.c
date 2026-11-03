/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_glslc_compile (src/glslc/glslc.h) as a backend calls it: the module's header and its size when the buffer is
   too small, the same words from the same source, the interface it reports (locations, bindings, the default
   uniform block's offsets), and the bounds of the SPIR-V it writes. tests/glslc/glslc_test.py runs SPIRV-Tools'
   validator on whole shaders */
#include "glslc/glslc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GlslcWorkspace workspace; /* every compilation's, as a caller keeps one per thread */
    GlslcResult result, again;
    uint32_t words[SNX_GLSLC_MAX_WORDS];
    uint32_t words2[SNX_GLSLC_MAX_WORDS];
    char big[70000];
} EmitTest;

static EmitTest g_emit_test;

static int compile(GlslcStage stage, const char* source) {
    return snx_glslc_compile(&g_emit_test.workspace, stage, source, strlen(source), g_emit_test.words,
                             SNX_GLSLC_MAX_WORDS, &g_emit_test.result);
}

static void must_compile(GlslcStage stage, const char* source) {
    if (compile(stage, source)) {
        fprintf(stderr, "%s\n%u:%u: %s\n", source, g_emit_test.result.line, g_emit_test.result.column,
                g_emit_test.result.error);
        exit(1);
    }
}

static const GlslcReflect* find(const char* source, const char* name, uint8_t kind) {
    const GlslcResult* r = &g_emit_test.result;
    for (uint32_t i = 0; i < r->reflect_count; i++) {
        const GlslcReflect* e = &r->reflect[i];
        if (e->kind == kind && e->name.len == strlen(name) && !memcmp(source + e->name.pos, name, e->name.len))
            return e;
    }
    fprintf(stderr, "no reflection entry %s of kind %u\n", name, kind);
    exit(1);
}

/* the operands of the first instruction op in the module whose operand at is value (any operand when at is -1), or
   NULL */
static const uint32_t* instruction_with(uint32_t op, int at, uint32_t value) {
    const uint32_t* w = g_emit_test.words;
    for (uint32_t i = 5; i < g_emit_test.result.words;) {
        uint32_t len = w[i] >> 16;
        if ((w[i] & 0xffffu) == op && (at < 0 || (len > (uint32_t)at + 1 && w[i + 1 + at] == value))) return &w[i + 1];
        i += len ? len : 1;
    }
    return NULL;
}

static const uint32_t* instruction(uint32_t op) { return instruction_with(op, -1, 0); }

static void module(void) {
    static const char source[] = "#version 310 es\nlayout(local_size_x = 8, local_size_y = 4) in;\n"
                                 "layout(std430, binding = 0) buffer B { uint v[]; };\n"
                                 "void main() { v[gl_GlobalInvocationID.x] = gl_WorkGroupSize.y; }\n";
    GlslcResult* r = &g_emit_test.result;
    must_compile(SNX_GLSLC_COMPUTE, source);
    const uint32_t* w = g_emit_test.words;
    assert(w[0] == 0x07230203u && w[1] == 0x00010300u && w[3] > 1 && w[4] == 0);
    /* the buffer block: an OpVariable (59) of the StorageBuffer storage class (12), its struct decorated (71) Block (2),
       and no BufferBlock (3) left from SPIR-V 1.0 */
    assert(instruction_with(59, 2, 12) && instruction_with(71, 1, 2) && !instruction_with(71, 1, 3));
    assert(r->version == 310 && r->error[0] == 0 && r->line == 0 && r->warnings == 0);
    const uint32_t* mode = instruction(16); /* OpExecutionMode main LocalSize 8 4 1 */
    assert(mode && mode[1] == 17 && mode[2] == 8 && mode[3] == 4 && mode[4] == 1);
    /* the same words again */
    uint32_t n = r->words;
    memcpy(g_emit_test.words2, g_emit_test.words, n * sizeof(uint32_t));
    must_compile(SNX_GLSLC_COMPUTE, source);
    assert(r->words == n && !memcmp(g_emit_test.words, g_emit_test.words2, n * sizeof(uint32_t)));
    /* a buffer too small: -1, and the size it needs */
    GlslcResult* small = &g_emit_test.again;
    assert(snx_glslc_compile(&g_emit_test.workspace, SNX_GLSLC_COMPUTE, source, sizeof source - 1, g_emit_test.words2,
                             7, small) == -1);
    assert(small->words == n && small->line == 0);
    char want[128];
    snprintf(want, sizeof want, "the SPIR-V module has %u words, and the buffer holds 7", n);
    assert(!strcmp(small->error, want));
    assert(snx_glslc_compile(&g_emit_test.workspace, SNX_GLSLC_COMPUTE, source, sizeof source - 1, NULL, 0, small) ==
               -1 &&
           small->words == n);
    /* what the API refuses before compiling */
    assert(snx_glslc_compile(&g_emit_test.workspace, (GlslcStage)3, source, sizeof source - 1, g_emit_test.words2, 7,
                             small) == -1);
    assert(!strcmp(small->error, "stage 3 is not one glslc compiles: vertex (0), fragment (1) or compute (2)"));
    assert(snx_glslc_compile(&g_emit_test.workspace, SNX_GLSLC_VERTEX, NULL, 4, g_emit_test.words2, 7, small) == -1);
    assert(!strcmp(small->error, "the source is NULL with a size of 4 bytes"));
    /* an error's place and the warning's */
    assert(compile(SNX_GLSLC_FRAGMENT, "#version 300 es\n#extension GL_FOO_x : warn\nprecision highp float;\n"
                                       "out vec4 c;\nvoid main() { c = 1.0; }\n") == -1);
    assert(r->line == 5 && r->column == 17 && strstr(r->error, "cannot assign float to vec4"));
    assert(r->warnings == 1 && strstr(r->warning, "line 2, column 12") && strstr(r->warning, "GL_FOO_x"));
}

static void interface(void) {
    static const char source[] =
        "#version 300 es\n"
        "layout(location = 2) in vec4 a;\n"
        "in vec2 b;\n"
        "in mat2 m;\n"
        "uniform float f;\n"
        "uniform vec3 v3;\n"
        "uniform mat4 m4;\n"
        "uniform float arr[2];\n"
        "uniform bool flag;\n"
        "uniform sampler2D s;\n"
        "uniform U { vec4 x; float y; } u;\n"
        "out vec3 o;\n"
        "void main() { o = v3 * f * arr[1] + vec3(float(flag)) + u.x.xyz * u.y + texture(s, b).rgb + m[0].xyy;\n"
        "  gl_Position = m4 * a; }\n";
    must_compile(SNX_GLSLC_VERTEX, source);
    /* explicit locations first, the others around them, a matrix taking a location per column */
    assert(find(source, "a", SNX_GLSLC_REFLECT_INPUT)->location == 2);
    assert(find(source, "b", SNX_GLSLC_REFLECT_INPUT)->location == 0);
    const GlslcReflect* e = find(source, "m", SNX_GLSLC_REFLECT_INPUT);
    assert(e->location == 3 && e->rows == 2 && e->cols == 2 && e->base == SNX_GLSLC_BASE_FLOAT);
    assert(find(source, "o", SNX_GLSLC_REFLECT_OUTPUT)->location == 0);
    /* the loose uniforms: the default uniform block, laid out as std140 */
    assert(find(source, "f", SNX_GLSLC_REFLECT_UNIFORM)->offset == 0);
    assert(find(source, "v3", SNX_GLSLC_REFLECT_UNIFORM)->offset == 16);
    assert(find(source, "m4", SNX_GLSLC_REFLECT_UNIFORM)->offset == 32);
    e = find(source, "arr", SNX_GLSLC_REFLECT_UNIFORM);
    assert(e->offset == 96 && e->array == 2);
    assert(find(source, "flag", SNX_GLSLC_REFLECT_UNIFORM)->offset == 128);
    /* bindings in declaration order, the default block's after the declared ones */
    assert(find(source, "s", SNX_GLSLC_REFLECT_SAMPLER)->location == 0);
    e = find(source, "U", SNX_GLSLC_REFLECT_UNIFORM_BLOCK);
    assert(e->location == 1 && e->set == 0 && e->offset == 32);
    e = find(source, "", SNX_GLSLC_REFLECT_UNIFORM_BLOCK);
    assert(e->location == 2 && e->offset == 144);
    /* explicit bindings, and one more around them */
    static const char frag[] =
        "#version 310 es\nprecision highp float;\n"
        "layout(binding = 0) uniform sampler2D t0;\nuniform sampler2D t1;\n"
        "layout(set = 1, binding = 0) uniform sampler2D t2;\nlayout(location = 0) out vec4 c;\n"
        "void main() { c = texture(t0, vec2(0.0)) + texture(t1, vec2(0.0)) + texture(t2, vec2(0.0)); }\n";
    must_compile(SNX_GLSLC_FRAGMENT, frag);
    assert(find(frag, "t0", SNX_GLSLC_REFLECT_SAMPLER)->location == 0);
    assert(find(frag, "t1", SNX_GLSLC_REFLECT_SAMPLER)->location == 1);
    e = find(frag, "t2", SNX_GLSLC_REFLECT_SAMPLER);
    assert(e->location == 0 && e->set == 1);
    assert(compile(SNX_GLSLC_FRAGMENT, "#version 310 es\nlayout(binding = 3) uniform highp sampler2D a;\n"
                                       "layout(binding = 3) uniform highp sampler2D b;\nvoid main() {}\n") == -1);
    assert(strstr(g_emit_test.result.error, "set 0 binding 3 is another resource's too"));
    assert(compile(SNX_GLSLC_VERTEX,
                   "#version 300 es\nlayout(location = 1) in vec4 a;\nlayout(location = 1) in vec4 b;\n"
                   "void main() {}\n") == -1);
    assert(strstr(g_emit_test.result.error, "location 1 of b is another variable's too"));
    /* gl_FragColor is output 0, and 1.00's attributes are numbered in their order */
    static const char old[] = "precision mediump float;\nuniform vec4 c;\nvoid main() { gl_FragColor = c; }\n";
    must_compile(SNX_GLSLC_FRAGMENT, old);
    assert(find(old, "gl_FragColor", SNX_GLSLC_REFLECT_OUTPUT)->location == 0);
}

#define ADD(...) (n += (size_t)snprintf(big + n, sizeof g_emit_test.big - n, __VA_ARGS__))

static void bounds(void) {
    /* one function past its code's words: an array copied out of a block, element by element */
    char* big = g_emit_test.big;
    size_t n = 0;
    ADD("#version 300 es\nprecision highp float;\nuniform U { float a[16000]; } u;\nout vec4 c;\nvoid main() {\n");
    for (uint32_t i = 0; i < 4; i++) ADD("  float a%u[16000] = u.a;\n", i);
    ADD("  c = vec4(a0[1] + a1[2] + a2[3] + a3[4]);\n}\n");
    assert(compile(SNX_GLSLC_FRAGMENT, big) == -1);
    if (!strstr(g_emit_test.result.error, "one function's code need more than the 131072 words")) {
        fprintf(stderr, "bounds: %s\n", g_emit_test.result.error);
        exit(1);
    }
    /* the interface's entries */
    n = 0;
    ADD("#version 300 es\n");
    for (uint32_t i = 0; i < 300; i++) ADD("uniform float u%u;\n", i);
    ADD("out float o;\nvoid main() { o = u0; }\n");
    assert(compile(SNX_GLSLC_VERTEX, big) == -1 && strstr(g_emit_test.result.error, "more than the 256 entries"));
}

/* the workspace: refused with its size or place, and reused without a trace of the compilation before */
static void workspace(void) {
    GlslcResult* r = &g_emit_test.again;
    GlslcWorkspace w;
    size_t need = snx_glslc_workspace_size();
    char want[256];
    assert(need > 1000000 && need < 8000000);
    assert(snx_glslc_workspace_init(&w, NULL, need, r) == -1 && !w.memory);
    snprintf(want, sizeof want, "the workspace's memory is NULL, and a compilation needs %zu bytes", need);
    assert(!strcmp(r->error, want));
    char* memory = g_emit_test.workspace.memory;
    assert(snx_glslc_workspace_init(&w, memory, need - 1, r) == -1);
    snprintf(want, sizeof want, "the workspace is %zu bytes, and a compilation needs %zu (snx_glslc_workspace_size)",
             need - 1, need);
    assert(!strcmp(r->error, want));
    assert(snx_glslc_workspace_init(&w, memory + 4, need, r) == -1 &&
           strstr(r->error, "is 4 bytes past a multiple of 16"));
    /* a compilation checks the workspace it is given too */
    w.memory = memory;
    w.size = 16;
    static const char frag[] =
        "#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n";
    assert(snx_glslc_compile(&w, SNX_GLSLC_FRAGMENT, frag, sizeof frag - 1, g_emit_test.words, SNX_GLSLC_MAX_WORDS,
                             r) == -1);
    snprintf(want, sizeof want, "the workspace is 16 bytes, and a compilation needs %zu (snx_glslc_workspace_size)",
             need);
    assert(!strcmp(r->error, want));
    /* one shader, another, then the first again over a workspace filled with another pattern: the same words */
    static const char comp[] = "#version 310 es\nlayout(local_size_x = 4) in;\n"
                               "layout(std430, binding = 0) buffer B { uint v[]; };\nvoid main() { v[0] = 7u; }\n";
    must_compile(SNX_GLSLC_FRAGMENT, frag);
    uint32_t n = g_emit_test.result.words;
    memcpy(g_emit_test.words2, g_emit_test.words, n * sizeof(uint32_t));
    must_compile(SNX_GLSLC_COMPUTE, comp);
    memset(memory, 0xa5, g_emit_test.workspace.size);
    must_compile(SNX_GLSLC_FRAGMENT, frag);
    assert(g_emit_test.result.words == n && !memcmp(g_emit_test.words, g_emit_test.words2, n * sizeof(uint32_t)));
    /* and an error the same way, its message and place */
    static const char bad[] = "#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = 1; }\n";
    assert(compile(SNX_GLSLC_FRAGMENT, bad) == -1);
    snprintf(want, sizeof want, "%u:%u: %s", g_emit_test.result.line, g_emit_test.result.column,
             g_emit_test.result.error);
    memset(memory, 0x5a, g_emit_test.workspace.size);
    assert(compile(SNX_GLSLC_FRAGMENT, bad) == -1);
    char again[300];
    snprintf(again, sizeof again, "%u:%u: %s", g_emit_test.result.line, g_emit_test.result.column,
             g_emit_test.result.error);
    assert(!strcmp(want, again) && strstr(want, "4:17: cannot assign int to vec4"));
}

int main(void) {
    size_t size = snx_glslc_workspace_size();
    assert(snx_glslc_workspace_init(&g_emit_test.workspace, malloc(size), size, &g_emit_test.result) == 0);
    workspace();
    module();
    interface();
    bounds();
    free(g_emit_test.workspace.memory);
    printf("glslc_emit: the workspace, the module, its interface and its bounds checked\n");
    return 0;
}
