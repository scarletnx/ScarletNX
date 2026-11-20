/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Not a test: a game's functions recompiled on their own, to time the recompile and read what it makes of them, a
   piece at a time. DIR is a module dump (scarletnx --dump-modules PROGRAM DIR), ADDR the guest addresses of the
   functions (hex, the F_<address> of a game module's symbols), or @FILE a file of them, one per line. Only those
   functions are compiled: a call out of them is left to the dispatch table.
     snx-llvm-pieces DIR [-O LEVEL] [--keep-ir FOLDER] [--each] [--volatile] ADDR... | @FILE
   Prints the seconds the recompile took, or with --each one line per function, its seconds and address. --volatile
   makes the guest's loads and stores volatile, as the setting volatile_memory does */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint64_t* hints;
    size_t count;
} Only;

/* every call target but the functions asked for is someone else's */
static int skip(void* context, uint64_t address) {
    const Only* only = (const Only*)context;
    size_t i;
    for (i = 0; i < only->count; ++i)
        if (only->hints[i] == address) return 0;
    return 1;
}

/* the dump's modules, as code ranges at their guest addresses */
static size_t load_dump(const char* dir, struct snx_code_range* ranges, size_t cap) {
    char *path = snx_path_join(dir, "modules.txt"), line[1024], name[256];
    FILE* f = fopen(path, "r");
    size_t n = 0;
    free(path);
    if (!f) return 0;
    while (n < cap && fgets(line, sizeof line, f)) {
        unsigned long long base, size, image;
        char* bin;
        size_t length = 0;
        if (sscanf(line, "module %251s %llx %llx %llx", name, &base, &size, &image) != 4) continue;
        strcat(name, ".bin"); /* which the 251 leave room for */
        bin = snx_path_join(dir, name);
        ranges[n].code = snx_read_file(bin, &length);
        free(bin);
        if (!ranges[n].code) continue;
        ranges[n].address = base;
        ranges[n].size = length & ~(size_t)3;
        ++n;
    }
    fclose(f);
    return n;
}

static int add_address(uint64_t** list, size_t* count, size_t* cap, const char* text) {
    char* end;
    unsigned long long v = strtoull(text, &end, 16);
    if (end == text) return -1;
    if (*count == *cap) {
        *cap = *cap ? 2 * *cap : 64;
        *list = (uint64_t*)realloc(*list, *cap * sizeof(**list));
    }
    (*list)[(*count)++] = v;
    return 0;
}

static double compile(struct snx_recompiler* r, const struct snx_code_range* ranges, size_t nranges,
                      const uint64_t* hints, size_t count) {
    Only only;
    struct snx_recompiled* m;
    double start = snx_seconds();
    only.hints = hints;
    only.count = count;
    m = snx_recompile_from(r, ranges, nranges, hints, count, (size_t)-1, skip, &only);
    if (!m) {
        fprintf(stderr, "llvm_pieces: %s\n", snx_recompiler_error(r));
        return -1;
    }
    start = snx_seconds() - start;
    snx_recompiled_free(m);
    return start;
}

int main(int argc, char** argv) {
    struct snx_code_range ranges[64];
    struct snx_recompiler* r;
    uint64_t* hints = NULL;
    size_t nranges, count = 0, cap = 0, i;
    const char* keep_ir = NULL;
    char opt = '2';
    int each = 0, a;
    if (argc < 3) return 2;
    nranges = load_dump(argv[1], ranges, sizeof ranges / sizeof *ranges);
    if (!nranges) return snx_error("no modules in %s", argv[1]);
    for (a = 2; a < argc; ++a) {
        if (!strcmp(argv[a], "-O") && a + 1 < argc)
            opt = argv[++a][0];
        else if (!strcmp(argv[a], "--keep-ir") && a + 1 < argc)
            keep_ir = argv[++a];
        else if (!strcmp(argv[a], "--each"))
            each = 1;
        else if (!strcmp(argv[a], "--volatile"))
            snx_ir_volatile_memory(1);
        else if (argv[a][0] == '@') {
            char line[128];
            FILE* f = fopen(argv[a] + 1, "r");
            if (!f) return snx_error("cannot read %s", argv[a] + 1);
            while (fgets(line, sizeof line, f)) (void)add_address(&hints, &count, &cap, line);
            fclose(f);
        } else if (add_address(&hints, &count, &cap, argv[a]))
            return snx_error("not an address: %s", argv[a]);
    }
    if (!count) return snx_error("no functions");
    r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    if (!r) return snx_error("no LLVM backend");
    snx_recompiler_set_optimization(r, opt);
    snx_recompiler_set_jobs(r, 1);
    if (keep_ir && snx_recompiler_set_keep_ir(r, keep_ir)) return snx_error("%s", snx_recompiler_error(r));
    if (each) {
        for (i = 0; i < count; ++i) {
            double t = compile(r, ranges, nranges, &hints[i], 1);
            if (t < 0) return 1;
            printf("%.4f %llx\n", t, (unsigned long long)hints[i]);
            fflush(stdout);
        }
    } else {
        double t = compile(r, ranges, nranges, hints, count);
        if (t < 0) return 1;
        printf("%.3f s for %lu functions\n", t, (unsigned long)count);
    }
    snx_recompiler_free(r);
    for (i = 0; i < nranges; ++i) free((void*)ranges[i].code);
    free(hints);
    return 0;
}
