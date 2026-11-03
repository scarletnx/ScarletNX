/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A short, deterministic fuzz of glslc: each seed shader mutated (bytes changed, ranges cut or repeated, tokens
   put in) and compiled, many times, with a fixed pseudo-random sequence. A compilation either works, giving a module
   that starts with SPIR-V's magic, or fails with a message; any crash is the test failing, and a sanitized build
   (SNX_TEST_SANITIZERS) turns a bad read or write into one. With --keep DIR, every module a mutant compiled to is
   written there, for spirv-val.

       snx-test-glslc-fuzz [--iterations N] [--seed S] [--keep DIR] SHADER... */
#include "glslc/glslc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INPUTS 64
#define BUFFER     (SNX_GLSLC_MAX_SOURCE + 1024)

typedef struct {
    uint64_t state;
    char* seeds[MAX_INPUTS];
    size_t sizes[MAX_INPUTS];
    GlslcStage stages[MAX_INPUTS];
    uint32_t count;
    char work[BUFFER];
    uint32_t words[SNX_GLSLC_MAX_WORDS];
    GlslcResult result;
} Fuzz;

static Fuzz g_fuzz;

static uint64_t next(void) {
    uint64_t x = g_fuzz.state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_fuzz.state = x;
    return x * 0x2545f4914f6cdd1dull;
}

static size_t below(size_t n) { return n ? (size_t)(next() % n) : 0; }

static const char* const pieces[] = {
    "(",
    ")",
    "{",
    "}",
    "[",
    "]",
    ";",
    ",",
    ".",
    "=",
    "+=",
    "<<=",
    "?",
    ":",
    "&&",
    "||",
    "++",
    "--",
    "#define A(x) x x\n",
    "#if 1\n",
    "#endif\n",
    "#else\n",
    "#undef A\n",
    "A(",
    "__LINE__",
    "defined",
    "float",
    "vec4",
    "mat3",
    "int",
    "uint",
    "bool",
    "struct",
    "uniform",
    "in",
    "out",
    "inout",
    "const",
    "layout(location = 1)",
    "layout(std430, binding = 2)",
    "buffer",
    "shared",
    "precision",
    "highp",
    "return",
    "discard",
    "break",
    "continue",
    "for",
    "while",
    "do",
    "switch",
    "case 1:",
    "default:",
    "if",
    "else",
    "true",
    "1.0",
    "0x7fffffff",
    "4294967295u",
    "1e39",
    "gl_Position",
    "gl_FragColor",
    "gl_FragCoord",
    "gl_GlobalInvocationID",
    "texture",
    "texture2D",
    "texelFetch",
    "imageStore",
    "atomicAdd",
    "barrier()",
    "length()",
    ".xyzw",
    ".rgba",
    "[0]",
    "[-1]",
    "main",
    "void main() {}",
    "#version 300 es\n",
    "#version 310 es\n",
    "#extension GL_OES_standard_derivatives : enable\n",
    "/*",
    "*/",
    "//",
    "\\\n",
    "\n",
    " ",
    "@",
    "\"",
};

static void mutate(char* s, size_t* size) {
    size_t n = *size;
    switch (below(7)) {
        case 0: /* one byte, to anything */
            if (n) s[below(n)] = (char)below(256);
            break;
        case 1: { /* a range cut */
            if (!n) break;
            size_t at = below(n), len = below(n - at < 64 ? n - at + 1 : 65);
            memmove(s + at, s + at + len, n - at - len);
            n -= len;
            break;
        }
        case 2: { /* a range repeated */
            if (!n) break;
            size_t at = below(n), len = below(n - at < 128 ? n - at + 1 : 129);
            if (n + len >= BUFFER) break;
            memmove(s + at + len, s + at, n - at);
            n += len;
            break;
        }
        case 3: { /* a piece of GLSL put in */
            const char* p = pieces[below(sizeof pieces / sizeof pieces[0])];
            size_t len = strlen(p), at = below(n + 1);
            if (n + len >= BUFFER) break;
            memmove(s + at + len, s + at, n - at);
            memcpy(s + at, p, len);
            n += len;
            break;
        }
        case 4: /* cut short */ n = below(n + 1); break;
        case 5: { /* two bytes swapped */
            if (n < 2) break;
            size_t a = below(n), b = below(n);
            char c = s[a];
            s[a] = s[b];
            s[b] = c;
            break;
        }
        default: { /* a digit made larger */
            for (size_t k = 0, at = below(n + 1); k < n; k++) {
                size_t i = (at + k) % n;
                if (s[i] >= '0' && s[i] <= '9') {
                    s[i] = '9';
                    break;
                }
            }
        }
    }
    *size = n;
}

static int load(const char* path) {
    const char* ext = strrchr(path, '.');
    GlslcStage stage;
    if (ext && !strcmp(ext, ".vert")) {
        stage = SNX_GLSLC_VERTEX;
    } else if (ext && !strcmp(ext, ".frag")) {
        stage = SNX_GLSLC_FRAGMENT;
    } else if (ext && !strcmp(ext, ".comp")) {
        stage = SNX_GLSLC_COMPUTE;
    } else {
        return 0; /* not a shader */
    }
    if (g_fuzz.count == MAX_INPUTS) {
        fprintf(stderr, "more than %d seed shaders: %s is one too many\n", MAX_INPUTS, path);
        return -1;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }
    char* data = malloc(BUFFER);
    size_t size = data ? fread(data, 1, BUFFER - 1, f) : 0;
    fclose(f);
    if (!data) {
        fprintf(stderr, "out of memory reading %s\n", path);
        return -1;
    }
    g_fuzz.seeds[g_fuzz.count] = data;
    g_fuzz.sizes[g_fuzz.count] = size;
    g_fuzz.stages[g_fuzz.count] = stage;
    g_fuzz.count++;
    return 0;
}

int main(int argc, char** argv) {
    unsigned long iterations = 2000;
    unsigned long long seed = 0x5eed;
    const char* keep = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iterations") && i + 1 < argc) {
            iterations = strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--keep") && i + 1 < argc) {
            keep = argv[++i];
        } else if (load(argv[i])) {
            return 2;
        }
    }
    if (!g_fuzz.count) {
        fprintf(stderr, "usage: %s [--iterations N] [--seed S] [--keep DIR] SHADER...\n", argv[0]);
        return 2;
    }
    g_fuzz.state = seed ? seed : 1;
    unsigned long worked = 0, failed = 0;
    for (unsigned long it = 0; it < iterations; it++) {
        uint32_t k = (uint32_t)below(g_fuzz.count);
        size_t size = g_fuzz.sizes[k];
        memcpy(g_fuzz.work, g_fuzz.seeds[k], size);
        /* mostly one change, so that many mutants still compile and reach the emitter */
        for (size_t m = below(4) ? 1 : 2 + below(7); m > 0; m--) mutate(g_fuzz.work, &size);
        int r =
            snx_glslc_compile(g_fuzz.stages[k], g_fuzz.work, size, g_fuzz.words, SNX_GLSLC_MAX_WORDS, &g_fuzz.result);
        if (r == 0) {
            if (g_fuzz.result.words < 5 || g_fuzz.words[0] != 0x07230203u) {
                fprintf(stderr, "iteration %lu: compiled to %u words without SPIR-V's magic\n", it,
                        g_fuzz.result.words);
                return 1;
            }
            worked++;
            if (keep) {
                char path[4096];
                static const char* const ext[] = {"vert", "frag", "comp"};
                snprintf(path, sizeof path, "%s/%06lu.%s.spv", keep, it, ext[g_fuzz.stages[k]]);
                FILE* o = fopen(path, "wb");
                if (o) {
                    fwrite(g_fuzz.words, 4, g_fuzz.result.words, o);
                    fclose(o);
                }
                snprintf(path, sizeof path, "%s/%06lu.%s", keep, it, ext[g_fuzz.stages[k]]);
                o = fopen(path, "wb");
                if (o) {
                    fwrite(g_fuzz.work, 1, size, o);
                    fclose(o);
                }
            }
        } else {
            if (!g_fuzz.result.error[0]) {
                fprintf(stderr, "iteration %lu: failed without a message\n", it);
                return 1;
            }
            failed++;
        }
    }
    printf("%lu mutants of %u shaders: %lu compiled, %lu refused with a message\n", iterations, g_fuzz.count, worked,
           failed);
    for (uint32_t i = 0; i < g_fuzz.count; i++) free(g_fuzz.seeds[i]);
    return 0;
}
