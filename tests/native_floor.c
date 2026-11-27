/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The hosts below a native back end's floor (recompiler/mach/native.h's snx_native_floor):
   - the line of a host that lacks some of x86-64's features, for each of their subsets: those needed and those the
     host has; of an ELFv1 program on big-endian PowerPC64, and which of those the kernel names have LDBRX, on any
     host;
   - on a host with a floor, its features taken out one by one and all at once (the mask its back end has for tests): no
     back end, of A64 functions nor of A32 blocks, and the line names what was taken;
   - there the JIT, the A32 executor, the blocks made ahead of time and the library are refused with that line;
   - without what the back end's fallback does, the library's line on stdout says so;
   - a host with all of it is refused nothing.

     snx-test-native-floor [--without=NAME,...]

   --without: the line of this host without the features named, alone, on stdout. A name the host's back end has no
   floor of is an error */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/arm/aot.h"
#include "common/cpu/arm/exec.h"
#include "common/cpu/recompiler/jit/jit_service.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/ppc64/ppc64be_codegen.h"
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/platform/file.h"
#include "common/runtime/interface.h"
#include "common/utilities/common.h"
#include "sail_x64_emit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* what a line of the launcher's run holds of one (gui/steps.h's GuiNote), past "scarletnx: " */
#define NOTE_MAX (111 - 11)

#define CODE    0x1000u
#define CODE_HI 0x2000u
static struct {
    int failures;
    /* the JIT's: the guest's memory, its dispatch table, the image of it */
    unsigned char memory[0x4000];
    _Atomic GuestFn dispatch[(CODE_HI - CODE) / 4];
    SnxCodeImage image;
} g_floor;
static void check(int ok, const char* what, const char* of) {
    if (ok) return;
    ++g_floor.failures;
    fprintf(stderr, "FAIL: %s: %s\n", of, what);
}
static void check_line(const char* got, const char* want, const char* of) {
    if (!strcmp(got, want)) return;
    ++g_floor.failures;
    fprintf(stderr, "FAIL: %s: \"%s\", not \"%s\"\n", of, got, want);
}

/* ---- x86-64's features, in the order its line names them ---- */

static const SnxNativeFeature k_x64[] = {{"SSE2", SNX_X64_SSE2}};
/* and what it does without, through its helpers */
static const SnxNativeFeature k_x64_done[] = {
    {"SSSE3", SNX_X64_SSSE3}, {"SSE4.1", SNX_X64_SSE41}, {"SSE4.2", SNX_X64_SSE42}, {"PCLMULQDQ", SNX_X64_PCLMUL}};
enum {
    X64_FEATURES = sizeof k_x64 / sizeof *k_x64,
    X64_DONE = sizeof k_x64_done / sizeof *k_x64_done,
    X64_ALL = SNX_X64_SSE2
};

/* "A, B and C" of k_x64's in bits, "none" for no one */
static void names_of(unsigned bits, char* out, size_t size) {
    const char* names[X64_FEATURES];
    unsigned n = 0;
    for (unsigned k = 0; k < X64_FEATURES; ++k)
        if (bits & k_x64[k].bit) names[n++] = k_x64[k].name;
    snprintf(out, size, "%s", n ? names[0] : "none");
    for (unsigned k = 1; k < n; ++k) {
        size_t at = strlen(out);
        snprintf(out + at, size - at, "%s%s", k + 1 < n ? ", " : " and ", names[k]);
    }
}
static void line_of(unsigned has, char* out, size_t size) {
    char needs[128], got[128];
    names_of(X64_ALL & ~has, needs, sizeof needs);
    names_of(has & X64_ALL, got, sizeof got);
    snprintf(out, size, "cpu: needs %s, this host has %s", needs, got);
}

static void x64_lines(void) {
    /* processors with none, some and all but one of them */
    static const struct {
        const char* name;
        unsigned has;
        const char* line;
    } hosts[] = {
        {"a test's without SSE2", SNX_X64_SSE3 | SNX_X64_POPCNT, "cpu: needs SSE2, this host has none"},
    };
    char line[256], want[256], cut[24];
    for (size_t k = 0; k < sizeof hosts / sizeof *hosts; ++k) {
        check(snx_native_lacks(k_x64, X64_FEATURES, hosts[k].has, line, sizeof line), "is below it", hosts[k].name);
        check_line(line, hosts[k].line, hosts[k].name);
    }
    for (unsigned subset = 0; subset < 1u << X64_FEATURES; ++subset) {
        unsigned has = SNX_X64_AVX | SNX_X64_BMI1; /* features that are no part of it */
        for (unsigned k = 0; k < X64_FEATURES; ++k)
            if (subset & (1u << k)) has |= k_x64[k].bit;
        const int below = (has & X64_ALL) != X64_ALL;
        if (below)
            line_of(has, want, sizeof want);
        else
            want[0] = 0;
        snprintf(line, sizeof line, "what was here");
        check(snx_native_lacks(k_x64, X64_FEATURES, has, line, sizeof line) == below, "whether it is below it", want);
        check_line(line, want, "a subset of the features");
        check(strlen(line) <= NOTE_MAX, "fits the launcher's line of it", line);
        check(snx_native_lacks(k_x64, X64_FEATURES, has, NULL, 0) == below, "without a line", want);
    }
    snx_native_lacks(k_x64, X64_FEATURES, 0, cut, sizeof cut);
    check_line(cut, "cpu: needs SSE2, this h", "a line cut to its room");
    /* what it does without: an Android x86_64 device with what its ABI promises alone (developer.android.com/ndk/
       guides/abis: SSE3 to SSE4.2 and POPCNT, no PCLMULQDQ), a Nehalem */
    const unsigned nehalem = X64_ALL | SNX_X64_SSSE3 | SNX_X64_SSE41 | SNX_X64_SSE42;
    check(snx_native_done(k_x64_done, X64_DONE, nehalem, line, sizeof line), "has no PCLMULQDQ", "a Nehalem");
    check_line(line, "cpu: this host has no PCLMULQDQ, done by the fallback", "a Nehalem");
    check(snx_native_done(k_x64_done, X64_DONE, X64_ALL, line, sizeof line), "has none of them", "a K10");
    check_line(line, "cpu: this host has no SSSE3, SSE4.1, SSE4.2 and PCLMULQDQ, done by the fallback", "a K10");
    check(!snx_native_lacks(k_x64, X64_FEATURES, X64_ALL, line, sizeof line), "is above the floor", "a K10");
    check(!snx_native_done(k_x64_done, X64_DONE, nehalem | SNX_X64_PCLMUL, line, sizeof line) && !line[0], "has it all",
          "a Westmere");
}

/* ---- big-endian PowerPC64 ---- */

#if !defined(_WIN32)
static void ppc64_lines(void) {
    /* Linux 6.12's cpu_user_features of each (arch/powerpc/kernel/cpu_specs_book3s_64.h and cpu_specs_e500mc.h), what
       AT_HWCAP starts from; power4 is the entry of a PVR it doesn't know. LDBRX where ISA 2.06 or the Cell is, two words
       elsewhere (ppc64.h's SGX_F_LDBRX): no floor */
    static const struct {
        const char* platform;
        unsigned long hwcap;
        unsigned features;
    } hosts[] = {
        {"power4", 0xcc000000, 0},      {"ppc970", 0xdc080000, 0},   {"power5", 0xcc046000, 0},
        {"power5+", 0xcc026000, 0},     {"power6", 0xcc007042, 0},   {"power6x", 0xcc007242, 0},
        {"pa6t", 0xdc000802, 0},        {"ppce5500", 0xcc008000, 0}, {"ppce6500", 0xdc008000, 0},
        {"ppc-cell-be", 0xdc014000, 1}, {"power7", 0xcc006142, 1},   {"power8", 0xcc006142, 1},
        {"power10", 0xcc006142, 1},
    };
    char line[256];
    for (size_t k = 0; k < sizeof hosts / sizeof *hosts; ++k)
        check(snx_ppc64be_features_of(hosts[k].hwcap) == hosts[k].features, "LDBRX as the processor has it",
              hosts[k].platform);
    /* an ELFv1 program, whatever the processor, is the one floor */
    check(snx_ppc64be_floor_of(1, line, sizeof line), "ELFv1", line);
    check_line(line, "abi: needs ELFv2, this build has ELFv1", "ELFv1");
    check(strlen(line) <= NOTE_MAX, "fits the launcher's line of it", line);
    check(!snx_ppc64be_floor_of(2, line, sizeof line) && !line[0], "ELFv2", line);
}
#else
static void ppc64_lines(void) {} /* the ppc64 back ends are not built there */
#endif

/* ---- this host's floor, masked ---- */

/* the features of this host's floor a test takes out of it, none where its back end has no floor */
#if defined(__x86_64__)
static const SnxNativeFeature* const k_names = k_x64;
enum { NAMES = X64_FEATURES, DONE = SNX_X64_SSSE3 | SNX_X64_SSE41 | SNX_X64_SSE42 | SNX_X64_PCLMUL };
static void take_out(unsigned bits) { snx_x64_without(bits); }
static const char* const k_none = "cpu: needs SSE2, this host has none";
#else
static const SnxNativeFeature* const k_names = NULL;
enum { NAMES = 0, DONE = 0 };
static void take_out(unsigned bits) { (void)bits; }
static const char* const k_none = "";
#endif
static unsigned all_names(void) {
    unsigned bits = 0;
    for (unsigned k = 0; k < NAMES; ++k) bits |= k_names[k].bit;
    return bits;
}
/* whether this host has a back end whose floor the test can take it below */
static int has_floor(void) { return NAMES > 0 && snx_native_available(); }

static void masked(void) {
    char line[256], name[64];
    const char* real = snx_native_name();
    for (unsigned k = 0; k <= NAMES; ++k) {
        const unsigned bits = k < NAMES ? k_names[k].bit : all_names();
        snprintf(name, sizeof name, "this host without %s", k < NAMES ? k_names[k].name : "any of them");
        take_out(bits);
        check(!snx_native_available(), "has no back end", name);
        check(!strcmp(snx_native_name(), "none"), "its name is none", name);
        check(!snx_native_arm_available() && !snx_native_arm_new(), "nor one of A32 blocks", name);
        check(snx_native_floor(line, sizeof line), "is below the floor", name);
        check(k == NAMES || NAMES == 1 || strstr(line, k_names[k].name) != NULL, line, name);
#if defined(__x86_64__)
        char want[256];
        line_of(X64_ALL & ~bits, want, sizeof want);
        check_line(line, want, name);
#endif
        if (k == NAMES) check(!strncmp(line, k_none, strlen(k_none)), line, name);
        take_out(0);
    }
    check(snx_native_available() && !strcmp(snx_native_name(), real), "its back end again", "this host");
    check(!snx_native_floor(line, sizeof line) && !line[0], "no line", "this host");
}

/* ---- the refusal ---- */

static SnxJitService* start_jit(char* error, size_t size) {
    static const SnxU32 words[] = {0x91000400u, 0xD65F03C0u}; /* add x0, x0, #1; ret */
    SnxJitConfig config = {0};
    for (size_t i = 0; i < sizeof words / sizeof *words; ++i)
        for (size_t j = 0; j < 4; ++j) g_floor.memory[CODE + 4 * i + j] = (unsigned char)(words[i] >> (8 * j));
    for (size_t i = 0; i < sizeof g_floor.dispatch / sizeof *g_floor.dispatch; ++i) g_floor.dispatch[i] = NULL;
    g_floor.image.image = g_floor.memory;
    g_floor.image.image_size = sizeof g_floor.memory;
    g_floor.image.text_off = CODE;
    g_floor.image.text_size = CODE_HI - CODE;
    config.images = &g_floor.image;
    config.image_count = 1;
    config.dispatch = g_floor.dispatch;
    config.code_lo = CODE;
    config.code_hi = CODE_HI;
    config.batch = 1;
    config.threads = 1;
    return snx_jit_service_new(&config, error, size);
}
/* one function through the JIT: x0 after it, 0 where it made none */
static uint64_t run_jit(SnxJitService* s) {
    RtAPI rt;
    CPU c = {0};
    GuestFn f = snx_jit_wait(s, CODE);
    snx_runtime_defaults(&rt);
    c.mem = g_floor.memory;
    c.rt = &rt;
    c.x[0] = 41;
    if (!f) return 0;
    f(&c, CODE);
    return c.x[0];
}
static void refused(const char* of) {
    char line[256], error[512];
    check(snx_native_floor(line, sizeof line), "is below the floor", of);
    snprintf(error, sizeof error, "unset");
    check(snx_native_refused(error, sizeof error), "is refused", of);
    check_line(error, line, of);
    error[0] = 0;
    SnxJitService* jit = start_jit(error, sizeof error);
    check(!jit, "the JIT is not started", of);
    check_line(error, line, "snx_jit_service_new");
    snx_jit_service_free(jit);
    error[0] = 0;
    SnxArmExec* exec = snx_arm_exec_new(error, sizeof error);
    check(!exec, "the A32 JIT is not started", of);
    check_line(error, line, "snx_arm_exec_new");
    snx_arm_exec_free(exec);
    error[0] = 0;
    SnxArmAot* aot = snx_arm_aot_new(error, sizeof error);
    check(!aot, "no block is made ahead of time", of);
    check_line(error, line, "snx_arm_aot_new");
    snx_arm_aot_free(aot);
    check(!snx_recompiler_has(SNX_RECOMPILER_FLAG_LLVM), "the library says it has no back end", of);
    struct snx_recompiler* r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    check(!r, "the library's recompiler is not made", of);
    snx_recompiler_free(r);
}

/* the library's lines on stdout, from here on in path, where the host lacks what the back end's fallback does (DONE):
   that line, and none of LLVM's making the code */
static void printed_done(const char* path, const char* of) {
    char line[256], want[300];
    size_t size = 0;
    struct snx_code_range range = {CODE, g_floor.memory + CODE, 8};
    const uint64_t entry = CODE;
    take_out(DONE);
    /* made once the host has its floor again: the library's code is the native back end's alone */
    struct snx_recompiler* r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    check(!snx_native_floor(line, sizeof line) && snx_native_fallback(line, sizeof line) && r &&
              freopen(path, "wb", stdout),
          "starts", of);
    if (r) {
        snx_recompiler_set_verbose(r, 1);
        snx_recompiled_free(snx_recompile_ranges(r, &range, 1, &entry, 1));
    }
    snx_recompiler_free(r);
    fflush(stdout);
    char* text = (char*)snx_read_file(path, &size);
    char* lines = (char*)calloc(size + 1, 1);
    if (text && lines) memcpy(lines, text, size);
    snprintf(want, sizeof want, "%s\n", line);
    check(lines && strstr(lines, want) != NULL, want, of);
    check(lines && !strstr(lines, "LLVM makes the code"), "says nothing of LLVM making it", of);
    free(text), free(lines);
}

/* the features named, "A,B", of this host's floor: their bits, or -1 for a name it has none of, said */
static int named(const char* names, unsigned* bits) {
    *bits = 0;
    while (*names) {
        const size_t n = strcspn(names, ",");
        unsigned k = 0;
        while (k < NAMES && (strlen(k_names[k].name) != n || strncmp(k_names[k].name, names, n))) ++k;
        if (k == NAMES) {
            fprintf(stderr, "--without=%.*s: this host's back end has no such feature in its floor%s", (int)n, names,
                    NAMES ? ": " : "\n");
            for (k = 0; k < NAMES; ++k) fprintf(stderr, "%s%s", k_names[k].name, k + 1 < NAMES ? ", " : "\n");
            return -1;
        }
        *bits |= k_names[k].bit;
        names += n + (names[n] == ',');
    }
    return 0;
}

int main(int argc, char** argv) {
    char line[256];
    if (argc == 2 && !strncmp(argv[1], "--without=", 10)) {
        unsigned bits;
        if (named(argv[1] + 10, &bits)) return 2;
        take_out(bits);
        printf("%s: %s\n", snx_native_name(), snx_native_floor(line, sizeof line) ? line : "above the floor");
        return 0;
    }
    if (argc > 1) {
        fprintf(stderr, "%s: not --without=NAME,...\n", argv[1]);
        return 2;
    }
    x64_lines();
    ppc64_lines();
    /* a host is refused where it is below its back end's floor, and nothing elsewhere */
    const int below = snx_native_floor(line, sizeof line);
    check(snx_native_refused(line, sizeof line) == below, "is refused where it is below the floor", "this host");
#if defined(__x86_64__)
    /* SSE2 is every x86-64 processor's: none is below the floor, and the back end's tests skip none for it */
    check(!snx_x64_floor(line, sizeof line), line, "this x86-64 host, above the floor");
#endif
    if (has_floor()) {
        char error[512] = "";
        masked();
        /* the back end makes the JIT's code, and nothing is refused */
        SnxJitService* jit = start_jit(error, sizeof error);
        check(jit && run_jit(jit) == 42, error, "the JIT, this host's back end's");
        snx_jit_service_free(jit);
        for (unsigned k = 0; k <= NAMES; ++k) {
            char of[64];
            snprintf(of, sizeof of, "without %s", k < NAMES ? k_names[k].name : "any of them");
            take_out(k < NAMES ? k_names[k].bit : all_names());
            refused(of);
        }
        printed_done("native_floor_done.txt", "the lines on stdout without what the fallback does");
        take_out(0);
    } else
        printf("native_floor: no floor to take this host below (%s%s): the lines alone\n", snx_native_name(),
               below ? ", below it already" : "");
    fprintf(stderr, "native_floor: %d failures\n", g_floor.failures);
    return g_floor.failures != 0;
}
