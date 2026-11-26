/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx -r on a host below its native back end's floor (recompiler/mach/native.h), an NRO of two instructions made
   in one process, over the game made before on the same host whole:
   - the recompile fails with the line of what the host lacks, after the line the launcher starts its step with, and the
     game made already stays as it was, with its stamp;
   - a host that lacks only what the back end does by its fallback has the game made by the back end, and a line on
     stdout says what it lacks and that the fallback does it;
   - the host whole says none of them.
   Where the host's back end has no floor, it passes having said so (77, skipped) */
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include "nx/runtime/driver/recompile.h"
#include "sail_x64_emit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the host's floor without one of its features (1), or without features the back end's fallback does (2), and the
   line of each */
#if defined(__x86_64__)
static void take_out(int how) {
    snx_x64_without(how == 1 ? SNX_X64_SSE2 : how == 2 ? SNX_X64_SSE42 | SNX_X64_PCLMUL : 0);
}
#define LACKS "cpu: needs SSE2, this host has none"
#define DONE  "cpu: this host has no SSE4.2 and PCLMULQDQ, done by the fallback"
#else
static void take_out(int how) { (void)how; }
#define LACKS ""
#define DONE  ""
#endif

static struct {
    int failures;
    char said[4096]; /* snx_error's lines since the last recompile */
} g_test;
static void check(int ok, const char* what, const char* of) {
    if (ok) return;
    ++g_test.failures;
    fprintf(stderr, "FAIL: %s: %s\n%s", of, what, g_test.said);
}
static void sink(const char* line) {
    const size_t at = strlen(g_test.said);
    snprintf(g_test.said + at, sizeof g_test.said - at, "%s\n", line);
}

static void put32(unsigned char* p, unsigned v) {
    for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
}
/* an NRO of 0x100 bytes of code, ADD X0, X0, #1 and RET past the header's words, 0x80 of .rodata and of .data */
static int write_nro(const char* path) {
    unsigned char nro[0x200] = {0};
    put32(nro, 0x14000020u); /* B +0x80, over the header */
    memcpy(nro + 0x10, "NRO0", 4);
    put32(nro + 0x18, sizeof nro);
    put32(nro + 0x24, 0x100);
    put32(nro + 0x28, 0x100), put32(nro + 0x2c, 0x80);
    put32(nro + 0x30, 0x180), put32(nro + 0x34, 0x80);
    put32(nro + 0x80, 0x91000400u);
    put32(nro + 0x84, 0xD65F03C0u);
    return snx_write_file(path, nro, sizeof nro);
}

typedef struct {
    unsigned char* data;
    size_t size;
} File;
static File take(const char* path) {
    File f = {0};
    f.data = snx_is_file(path) ? (unsigned char*)snx_read_file(path, &f.size) : NULL;
    return f;
}
static int same(File a, File b) { return a.data && b.data && a.size == b.size && !memcmp(a.data, b.data, a.size); }

int main(int argc, char** argv) {
    char* folder;
    if (argc != 2) {
        fprintf(stderr, "usage: native_floor_recompile FOLDER\n");
        return 2;
    }
    if (!LACKS[0] || !snx_native_available()) {
        printf("native_floor_recompile: no floor to take this host below (%s)\n", snx_native_name());
        return 77;
    }
    folder = snx_absolute(argv[1]);
    if (!folder || (snx_is_dir(folder) && snx_remove_tree(folder)) || snx_mkdirs(folder)) return 1;
    char *nro = snx_path_join(folder, "floor.nro"), *out = snx_path_join(folder, "out"),
         *runtime = snx_path_join(folder, "runtime"), *printed = snx_path_join(folder, "stdout"),
         *base = snx_path_join(folder, "out/floor/floor"), *stamp = snx_format("%s.abi", base),
#if defined(_WIN32)
         *code = snx_format("%s_code.dll", base);
#else
         *code = snx_format("%s_code.so", base);
#endif
    if (write_nro(nro) || snx_write_file(runtime, "runtime", 7)) return 1;
    SnxPaths paths = {0};
    paths.repo = folder;
    paths.build = folder;
    paths.executable = runtime;
    File first_stamp = {0}, first_code = {0};
    static const struct {
        const char* of;
        int below, done;
    } runs[] = {{"the host whole", 0, 0}, {"below the floor", 1, 0}, {"without what the fallback does", 0, 1}};
    snx_error_sink(sink);
    for (unsigned k = 0; k < sizeof runs / sizeof *runs; ++k) {
        char* args[] = {nro, "--name", "floor", "--outdir", out, "--runtime", runtime};
        const char* of = runs[k].of;
        char line[256], refused[300];
        g_test.said[0] = 0;
        take_out(runs[k].done ? 2 : runs[k].below);
        check(snx_native_floor(line, sizeof line) == runs[k].below &&
                  (!runs[k].below || !strncmp(line, LACKS, strlen(LACKS))),
              "the host's line", of);
        check(runs[k].below || snx_native_fallback(line, sizeof line) == runs[k].done, "the fallback's line", of);
        fflush(stdout);
        if (!freopen(printed, "wb", stdout)) return 1;
        const int rc = snx_recompile_game(sizeof args / sizeof *args, args, &paths);
        fflush(stdout);
        File stamped = take(stamp), made = take(code), text = take(printed);
        char* lines = (char*)calloc(text.size + 1, 1);
        if (!lines) return 1;
        if (text.data) memcpy(lines, text.data, text.size);
        check(strstr(lines, "Recompiling floor (1 modules)\n") != NULL, "the line of the launcher's step", of);
        if (runs[k].below) {
            check(rc != 0, "fails", of);
            snprintf(refused, sizeof refused, "%s\n", line);
            check(!strcmp(g_test.said, refused), "says what the host lacks", of);
            check(same(stamped, first_stamp) && same(made, first_code), "leaves the game made before as it was", of);
        } else {
            check(rc == 0, "makes the game", of);
            check(stamped.data && made.data, "with its stamp and its module", of);
            check(!g_test.said[0], "says nothing else", of);
            check(!strstr(lines, "LLVM"), "says nothing of LLVM", of);
            check(!runs[k].done == !strstr(lines, DONE "\n"), "says what the fallback does, on stdout", of);
            check(runs[k].done || !strstr(lines, "fallback"), "and nothing of it otherwise", of);
        }
        if (!k)
            first_stamp = stamped, first_code = made;
        else
            free(stamped.data), free(made.data);
        free(text.data);
        free(lines);
    }
    snx_error_sink(NULL);
    take_out(0);
    free(first_stamp.data), free(first_code.data);
    free(nro), free(out), free(runtime), free(printed), free(base), free(stamp), free(code), free(folder);
    fprintf(stderr, "native_floor_recompile: %d failures\n", g_test.failures);
    return g_test.failures != 0;
}
