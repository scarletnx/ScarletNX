/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* glslc on shader files, for the tests: each file's stage from its extension (.vert, .frag, .comp), and with -o
   (one file only) its module written to OUT. Prints FILE:LINE:COLUMN: ERROR for each file refused, the first warning
   of each file with warnings, and exits 1 when a file was refused.

       snx-glslc-cli [-o OUT.spv] FILE... */
#include "glslc/glslc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char source[SNX_GLSLC_MAX_SOURCE + 2];
    uint32_t words[SNX_GLSLC_MAX_WORDS];
    GlslcResult result;
    GlslcWorkspace workspace; /* every file's */
} Cli;

static Cli g_cli;

/* 0 compiled, 1 refused, 2 not read or written */
static int compile_file(const char* path, const char* out) {
    const char* ext = strrchr(path, '.');
    GlslcStage stage;
    if (ext && !strcmp(ext, ".vert")) {
        stage = SNX_GLSLC_VERTEX;
    } else if (ext && !strcmp(ext, ".frag")) {
        stage = SNX_GLSLC_FRAGMENT;
    } else if (ext && !strcmp(ext, ".comp")) {
        stage = SNX_GLSLC_COMPUTE;
    } else {
        fprintf(stderr, "%s: the extension is .vert, .frag or .comp, and this one is %s\n", path, ext ? ext : "none");
        return 2;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 2;
    }
    size_t size = fread(g_cli.source, 1, sizeof g_cli.source, f);
    fclose(f);
    GlslcResult* r = &g_cli.result;
    if (snx_glslc_compile(&g_cli.workspace, stage, g_cli.source, size, g_cli.words, SNX_GLSLC_MAX_WORDS, r)) {
        printf("%s:%u:%u: %s\n", path, r->line, r->column, r->error);
        return 1;
    }
    if (r->warnings) printf("%s: %u warnings, the first: %s\n", path, r->warnings, r->warning);
    if (out) {
        FILE* o = fopen(out, "wb");
        if (!o || fwrite(g_cli.words, sizeof g_cli.words[0], r->words, o) != r->words || fclose(o)) {
            perror(out);
            return 2;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* out = NULL;
    int first = 1;
    if (argc > 2 && !strcmp(argv[1], "-o")) {
        out = argv[2];
        first = 3;
    }
    if (first >= argc || (out && argc - first != 1)) {
        fprintf(stderr, "usage: %s [-o OUT.spv] FILE... (-o with one file)\n", argv[0]);
        return 2;
    }
    size_t workspace_size = snx_glslc_workspace_size();
    if (snx_glslc_workspace_init(&g_cli.workspace, malloc(workspace_size), workspace_size, &g_cli.result)) {
        fprintf(stderr, "%s\n", g_cli.result.error);
        return 2;
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        int r = compile_file(argv[i], out);
        if (r > status) status = r;
    }
    free(g_cli.workspace.memory);
    return status;
}
