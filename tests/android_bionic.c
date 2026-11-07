/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's C library as the native bridge gives it to an app's code (src/android/bridge/hle_libc.c, hle_posix.c,
   hle_signal.c and args.c): each function through its HLE handler, with the arguments in a CPU state as a guest's
   call has them, against what bionic gives on Android 16 */
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/host.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    CPU cpu;
    uint64_t stack[16]; /* the arguments past the eighth integer one */
    uint64_t sp;        /* the guest's stack pointer instead of stack, when not 0 */
    int failures;
} g_t;

static const HleSymbol* find(const char* name) {
    const HleSymbol* const tables[] = {hle_libc, hle_posix, hle_signal};
    for (size_t t = 0; t < sizeof tables / sizeof *tables; ++t)
        for (const HleSymbol* s = tables[t]; s->name[0]; ++s)
            if (!strcmp(s->name, name)) return s;
    fprintf(stderr, "android_bionic: the HLE has no %s\n", name);
    exit(1);
}

/* name called as a guest calls it: the integer arguments x (from the ninth on the stack) and the floating ones d, with
   the guest's errno taken around it as the bridge takes it. Its x0 */
static uint64_t call(const char* name, const uint64_t* x, size_t nx, const double* d, size_t nd) {
    const HleSymbol* s = find(name);
    g_t.cpu = (CPU){0};
    memset(g_t.stack, 0, sizeof g_t.stack);
    for (size_t i = 0; i < nx; ++i) {
        if (i < 8)
            g_t.cpu.x[i] = x[i];
        else
            g_t.stack[i - 8] = x[i];
    }
    g_t.cpu.x[31] = g_t.sp ? g_t.sp : (uint64_t)(uintptr_t)g_t.stack;
    for (size_t i = 0; i < nd; ++i) memcpy(&g_t.cpu.v[i].df[0], &d[i], sizeof d[i]);
    hle_errno_begin();
    s->handler(&g_t.cpu, s->data);
    hle_errno_end();
    return g_t.cpu.x[0];
}
#define P(p)            ((uint64_t)(uintptr_t)(p))
#define I32(v)          ((uint64_t)(uint32_t)(int32_t)(v)) /* an int as a guest passes it: w, the upper half zero */
#define CALL(name, ...) call(name, (uint64_t[]){__VA_ARGS__}, sizeof((uint64_t[]){__VA_ARGS__}) / 8, NULL, 0)
#define CALLD(name, d, ...)                                                                                            \
    call(name, (uint64_t[]){__VA_ARGS__}, sizeof((uint64_t[]){__VA_ARGS__}) / 8, (double[]){d},                        \
         sizeof((double[]){d}) / 8)

static void expect_text(const char* what, const char* got, const char* want) {
    if (strcmp(got, want)) {
        fprintf(stderr, "android_bionic: %s: got \"%s\", want \"%s\"\n", what, got, want);
        g_t.failures++;
    }
}
static void expect_int(const char* what, int64_t got, int64_t want) {
    if (got != want) {
        fprintf(stderr, "android_bionic: %s: got %lld, want %lld\n", what, (long long)got, (long long)want);
        g_t.failures++;
    }
}

/* ---- printf ---- */

/* a negative int of %d and %i, with flags and widths, and the other lengths' */
static void test_printf_signed(void) {
    char out[128];
    int64_t n = (int64_t)CALL("snprintf", P(out), sizeof out, P("%d|%i|%+05d|%-4d|%ld|%hd|%hhd|%u"), I32(-42), I32(-1),
                              I32(42), I32(7), (uint64_t)-5, I32(-3), I32(-2), I32(-1));
    expect_text("snprintf of negative ints", out, "-42|-1|+0042|7   |-5|-3|-2|4294967295");
    expect_int("snprintf's length", n, (int64_t)strlen("-42|-1|+0042|7   |-5|-3|-2|4294967295"));
}

/* ---- scanf ---- */

/* a guest whose stack ends right after its frame, as a native method called from Java has it: sscanf reads the
   pointers its format stores through, and no stack slot past them */
static void test_sscanf_reads_its_pointers(void) {
    enum { PAGE = 65536 }; /* a multiple of every host's page */
    unsigned char* m = host_reserve(0, 2 * PAGE, false);
    if (!m || !host_commit(m, PAGE)) {
        fprintf(stderr, "android_bionic: no memory for a stack that ends at a page without access\n");
        exit(1);
    }
    g_t.sp = (uint64_t)(uintptr_t)(m + PAGE);
    int i = 0, x = 0, n = -1, skipped = 0, a = 0, b = 0;
    double d = 0;
    char w[16] = "";
    int64_t r =
        (int64_t)CALL("sscanf", P("42 3.5 word 0x1f rest"), P("%d %lf %15s %x %n"), P(&i), P(&d), P(w), P(&x), P(&n));
    expect_int("sscanf's count", r, 4);
    char got[96];
    snprintf(got, sizeof got, "%d|%.3f|%s|%d|%d", i, d, w, x, n);
    expect_text("sscanf's values", got, "42|3.500|word|31|17");
    r = (int64_t)CALL("sscanf", P("7 8 9"), P("%*d %d%n"), P(&skipped), P(&n));
    expect_int("sscanf of a suppressed conversion", r, 1);
    expect_int("the value after it", skipped, 8);
    r = (int64_t)CALL("sscanf", P("ab%cd"), P("%[^%]%%%n"), P(w), P(&n));
    expect_int("sscanf of a set that holds %", r, 1);
    expect_int("the count after it", n, 3);
    r = (int64_t)CALL("sscanf", P("1 2"), P("%2$d %1$d"), P(&a), P(&b));
    expect_int("sscanf by position", r, 2);
    expect_int("the first pointer, which %1$d fills from the second number", a, 2);
    g_t.sp = 0;
    host_unmap(m, 2 * PAGE);

    /* more pointers than the bridge passes: EOF, and the message says so */
    char many[25 * 2 + 1] = "";
    for (int k = 0; k < 25; ++k) strcat(many, "%d");
    expect_int("sscanf of 25 conversions", (int64_t)CALL("sscanf", P("1"), P(many)), -1);
}

int main(void) {
    test_printf_signed();
    test_sscanf_reads_its_pointers();
    if (g_t.failures) {
        fprintf(stderr, "android_bionic: %d failed\n", g_t.failures);
        return 1;
    }
    puts("android_bionic: all passed");
    return 0;
}
