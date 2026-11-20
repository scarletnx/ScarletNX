/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen: ScarletNX's instruction emitters generated from the Sail of the Arm architecture (rems-project's sail-arm,
   the Arm ASL translated, BSD 3-Clause Clear), in strict C23 with no dependency, so a cross build can build it for
   the machine it runs on, as tools/decodetree.

     sailgen --extract ROOTS -o OUT INPUT...
         copies the definitions of INPUT (sail-arm's .sail files) that ROOTS reaches into OUT (extract.c)
     sailgen --generate ROOTS --backend FILE... [--profile PROFILE] -o OUT INPUT... [--overlay FILE]...
         writes OUT, C that builds each decode function ROOTS names as LLVM IR (gen.c), from INPUT and the overlays,
         whose definitions take the place of INPUT's of the same name (a platform's fixups), with the back end's files
         (core.h, the lowering of Sail's builtins, then a machine's state: llvm.h for A64, llvm_arm.h for AArch32)
         copied at its top in their order. A feature the profile's feature lines do not name is one no guest has:
         IsFeatureImplemented of it is false when sailgen runs; its since lines give an encoding's architecture

   OUT is only rewritten when it changes. tools/sailgen/README.md describes the generated code */
#include "sailgen.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the output, and the strings every part of sailgen names things by (an offset into strings; 0 is "") */
static struct {
    char* text;
    size_t length, cap;
    char* strings;
    size_t string_size, string_cap;
} g_main;

void* sg_alloc(size_t size) {
    void* p = calloc(1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "sailgen: out of memory for %zu bytes\n", size);
        exit(1);
    }
    return p;
}

char* sg_read(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    long n;
    char* text;
    if (!f) {
        fprintf(stderr, "sailgen: cannot open %s\n", path);
        exit(1);
    }
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "sailgen: cannot size %s\n", path);
        exit(1);
    }
    text = (char*)sg_alloc((size_t)n + 1);
    if (fread(text, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "sailgen: cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    *size = (size_t)n;
    return text;
}

const char* sg_str(unsigned offset) { return g_main.strings + offset; }

unsigned sg_intern(const char* text, size_t length) {
    size_t at;
    if (!g_main.strings) {
        g_main.string_cap = 1u << 16;
        g_main.strings = (char*)sg_alloc(g_main.string_cap);
        g_main.string_size = 1; /* offset 0 is the empty string */
    }
    if (g_main.string_size + length + 1 > g_main.string_cap) {
        while (g_main.string_size + length + 1 > g_main.string_cap) g_main.string_cap *= 2;
        g_main.strings = (char*)realloc(g_main.strings, g_main.string_cap);
        if (!g_main.strings) {
            fprintf(stderr, "sailgen: out of memory for the strings\n");
            exit(1);
        }
    }
    at = g_main.string_size;
    memcpy(g_main.strings + at, text, length);
    g_main.strings[at + length] = 0;
    g_main.string_size += length + 1;
    return (unsigned)at;
}

void sg_out(const char* fmt, ...) {
    va_list ap;
    int n;
    for (;;) {
        size_t room = g_main.cap - g_main.length;
        va_start(ap, fmt);
        n = vsnprintf(g_main.text ? g_main.text + g_main.length : NULL, room, fmt, ap);
        va_end(ap);
        if (n < 0) {
            fprintf(stderr, "sailgen: cannot format the output\n");
            exit(1);
        }
        if ((size_t)n < room) break;
        g_main.cap = (g_main.length + (size_t)n + 1) * 2;
        g_main.text = (char*)realloc(g_main.text, g_main.cap);
        if (!g_main.text) {
            fprintf(stderr, "sailgen: out of memory for the output\n");
            exit(1);
        }
    }
    g_main.length += (size_t)n;
}

/* written only when it changes, so what includes it recompiles only then (Ninja's restat) */
int sg_flush(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) {
        char* old = (char*)sg_alloc(g_main.length + 1);
        size_t got = fread(old, 1, g_main.length + 1, f);
        int same = got == g_main.length && !memcmp(old, g_main.text, g_main.length);
        fclose(f);
        free(old);
        if (same) return 0;
    }
    f = fopen(path, "wb");
    if (!f || fwrite(g_main.text, 1, g_main.length, f) != g_main.length || fclose(f)) {
        fprintf(stderr, "sailgen: cannot write %s\n", path);
        return 1;
    }
    return 0;
}

static int usage(const char* why) {
    fprintf(stderr,
            "sailgen: %s\n"
            "usage: sailgen --extract ROOTS -o OUT INPUT...\n"
            "       sailgen --generate ROOTS --backend FILE [--backend FILE]... [--profile PROFILE] -o OUT INPUT...\n"
            "               [--overlay FILE]...\n",
            why);
    return 2;
}

int main(int argc, char** argv) {
    const char* mode = NULL;
    const char* roots = NULL;
    const char* out = NULL;
    const char* backends[4];
    int backend_count = 0;
    const char* profile = NULL;
    const char* inputs[16];
    int overlays[16], count = 0, i;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--extract") || !strcmp(argv[i], "--generate")) {
            if (mode) return usage("--extract and --generate are one or the other");
            if (i + 1 >= argc) return usage("--extract and --generate take the roots file");
            mode = argv[i];
            roots = argv[++i];
        } else if (!strcmp(argv[i], "--backend")) {
            if (i + 1 >= argc) return usage("--backend takes the back end's file");
            if (backend_count == 4) return usage("more than 4 back end files");
            backends[backend_count++] = argv[++i];
        } else if (!strcmp(argv[i], "--profile")) {
            if (i + 1 >= argc) return usage("--profile takes the decoder's profile (a64.profile)");
            profile = argv[++i];
        } else if (!strcmp(argv[i], "-o")) {
            if (i + 1 >= argc) return usage("-o takes the output file");
            out = argv[++i];
        } else if (!strcmp(argv[i], "--overlay") || argv[i][0] != '-') {
            int overlay = argv[i][0] == '-';
            if (overlay && i + 1 >= argc) return usage("--overlay takes a file");
            if (count == 16) return usage("more than 16 input files");
            overlays[count] = overlay;
            inputs[count++] = overlay ? argv[++i] : argv[i];
        } else {
            fprintf(stderr, "sailgen: unknown option %s\n", argv[i]);
            return usage("see the options below");
        }
    }
    if (!mode) return usage("give --extract or --generate");
    if (!out) return usage("give -o OUT");
    if (!count) return usage("give the input files");
    if (!strcmp(mode, "--extract")) {
        for (i = 0; i < count; ++i)
            if (overlays[i]) return usage("--extract reads sail-arm's files, not overlays");
        if (profile) return usage("--profile is --generate's");
        return sg_extract(roots, out, inputs, count);
    }
    if (!backend_count) return usage("--generate needs the back end's files (--backend core.h --backend llvm.h)");
    for (i = 0; i < count; ++i) sg_parse_file(inputs[i], overlays[i]);
    return sg_generate(roots, backends, backend_count, profile, out);
}
