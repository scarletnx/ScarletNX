/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's C library as the native bridge gives it to an app's code (src/android/bridge/hle_libc.c, hle_posix.c,
   hle_signal.c and args.c): each function through its HLE handler, with the arguments in a CPU state as a guest's
   call has them, against what bionic gives on Android 16 */
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/host.h"
#include <locale.h>
#include <math.h>
#if defined(__APPLE__)
#include <xlocale.h> /* uselocale */
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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

/* ---- errno's texts ---- */

/* bionic's own words for each errno (not the host C library's, nor in the user's language), "Unknown error N" past
   them, in strerror, strerror_r, GNU's strerror_r, perror and %m */
static void test_error_texts(void) {
    static const struct {
        int error;
        const char* text;
    } texts[] = {
        {0, "Success"},
        {2, "No such file or directory"},
        {11, "Try again"},
        {38, "Function not implemented"},
        {41, "Unknown error 41"}, /* a number Linux leaves out */
        {133, "Memory page has hardware error"},
        {134, "Unknown error 134"},
        {9999, "Unknown error 9999"},
        {-1, "Unknown error -1"},
    };
    for (size_t i = 0; i < sizeof texts / sizeof *texts; ++i) {
        char what[64];
        snprintf(what, sizeof what, "strerror(%d)", texts[i].error);
        expect_text(what, (const char*)(uintptr_t)CALL("strerror", I32(texts[i].error)), texts[i].text);
    }
    expect_text("strerror_l(11)", (const char*)(uintptr_t)CALL("strerror_l", 11, 0), "Try again");
    char buffer[64];
    expect_int("strerror_r of a text that fits", (int64_t)CALL("strerror_r", 2, P(buffer), sizeof buffer), 0);
    expect_text("its text", buffer, "No such file or directory");
    expect_int("strerror_r of a text cut short: ERANGE", (int64_t)CALL("strerror_r", 11, P(buffer), 4), 34);
    expect_text("its cut text", buffer, "Try");
    expect_int("GNU's strerror_r gives its buffer", (int64_t)CALL("__gnu_strerror_r", I32(9999), P(buffer), 8),
               (int64_t)(uintptr_t)buffer);
    expect_text("its cut text", buffer, "Unknown");
    hle_set_errno(11);
    CALL("snprintf", P(buffer), sizeof buffer, P("%m|%d"), I32(5));
    expect_text("%m", buffer, "Try again|5");
#ifndef _WIN32
    /* perror's line on stderr */
    static const struct {
        const char* message;
        const char* line;
    } lines[] = {{"open", "open: No such file or directory\n"},
                 {"", "No such file or directory\n"},
                 {NULL, "No such file or directory\n"}};
    for (size_t i = 0; i < sizeof lines / sizeof *lines; ++i) {
        fflush(stderr);
        FILE* f = tmpfile();
        int saved = dup(2);
        if (!f || saved < 0 || dup2(fileno(f), 2) < 0) {
            fprintf(stderr, "android_bionic: stderr cannot be read back for perror\n");
            exit(1);
        }
        hle_set_errno(2);
        CALL("perror", P(lines[i].message));
        fflush(stderr);
        dup2(saved, 2);
        close(saved);
        rewind(f);
        size_t n = fread(buffer, 1, sizeof buffer - 1, f);
        buffer[n] = 0;
        fclose(f);
        expect_text("perror's line", buffer, lines[i].line);
    }
#endif
}

/* ---- what the NDK's _FORTIFY_SOURCE calls ---- */

/* send(), recv() and readlink() of a buffer whose size the compiler knows, as __sendto_chk, __recvfrom_chk and
   __readlink_chk */
static void test_fortified(void) {
    int32_t fds[2] = {-1, -1};
    expect_int("socketpair(AF_UNIX, SOCK_STREAM)", (int64_t)CALL("socketpair", 1, 1, 0, P(fds)), 0);
    expect_int("__sendto_chk, send's FORTIFY form", (int64_t)CALL("__sendto_chk", I32(fds[0]), P("abc"), 3, 4, 0, 0, 0),
               3);
    char got[8] = "";
    expect_int("__recvfrom_chk, recv's (MSG_WAITALL)",
               (int64_t)CALL("__recvfrom_chk", I32(fds[1]), P(got), 3, sizeof got, 0x100, 0, 0), 3);
    expect_text("what came through", got, "abc");
    CALL("close", I32(fds[0]));
    CALL("close", I32(fds[1]));
    char link[256];
    int64_t n = (int64_t)CALL("__readlink_chk", P("/proc/self/exe"), P(link), sizeof link - 1, sizeof link);
    link[n > 0 ? n : 0] = 0;
    expect_text("__readlink_chk of /proc/self/exe", link, "/system/bin/app_process64");
}

/* ---- libm ---- */

/* the distance in units in the last place between two doubles of one sign */
static int64_t ulps(double a, double b) {
    int64_t x, y;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    return x > y ? x - y : y - x;
}
/* a libm function of one double through its handler, against the correctly rounded result's bits: within 1 ulp */
static void expect_math(const char* name, double x, uint64_t bits) {
    double want, got;
    memcpy(&want, &bits, 8);
    CALLD(name, x, 0);
    memcpy(&got, &g_t.cpu.v[0].df[0], 8);
    if (ulps(got, want) > 1) {
        fprintf(stderr, "android_bionic: %s(%g): got %.17g, %lld ulp from %.17g\n", name, x, got,
                (long long)ulps(got, want), want);
        g_t.failures++;
    }
}
static void expect_mathf(const char* name, float x, float (*host)(float)) {
    float got;
    g_t.cpu = (CPU){0};
    memcpy(&g_t.cpu.v[0].f[0], &x, 4);
    const HleSymbol* s = find(name);
    s->handler(&g_t.cpu, s->data);
    memcpy(&got, &g_t.cpu.v[0].f[0], 4);
    if (got != host(x)) {
        fprintf(stderr, "android_bionic: %s(%g): got %.9g, the host's %.9g\n", name, x, got, host(x));
        g_t.failures++;
    }
}

/* the functions of bionic's libm that apps call, past the ones every app has (the probe's correctly rounded values,
   from tools/math-expected.py of androidprobe) */
static void test_libm(void) {
    expect_math("cbrt", 2.0, 0x3ff428a2f98d728bull);
    expect_math("expm1", 1e-10, 0x3ddb7cdfd9dda4e3ull);
    expect_math("log1p", 1e-10, 0x3ddb7cdfd9d1d693ull);
    expect_mathf("cbrtf", 2.0f, cbrtf);
    expect_mathf("expm1f", 1e-5f, expm1f);
    expect_mathf("log1pf", 1e-5f, log1pf);
}

/* ---- files ---- */

/* getline and getdelim into a buffer they make and grow, to the end of the file */
static void test_getline(void) {
    const char* path = "android_bionic_getline.txt";
    FILE* f = fopen(path, "w+b");
    if (!f) {
        fprintf(stderr, "android_bionic: cannot write %s in the working folder\n", path);
        exit(1);
    }
    fputs("one\ntwo,three\nlast", f);
    rewind(f);
    char* line = NULL;
    size_t size = 0;
    static const char* const want[] = {"one\n", "two,three\n", "last"};
    for (int i = 0; i < 3; ++i) {
        int64_t n = (int64_t)CALL("getline", P(&line), P(&size), P(f));
        expect_int("getline's length", n, (int64_t)strlen(want[i]));
        expect_text("getline's line", n > 0 ? line : "", want[i]);
    }
    expect_int("getline at the end of the file", (int64_t)CALL("getline", P(&line), P(&size), P(f)), -1);
    rewind(f);
    expect_int("getdelim to a comma", (int64_t)CALL("getdelim", P(&line), P(&size), ',', P(f)), 8);
    expect_text("its text", line, "one\ntwo,");
    expect_int("a room that holds it", size >= 9, 1);
    free(line);
    fclose(f);
    remove(path);
}

/* ---- signals ---- */

/* sigismember of a set the other set functions made, and the 64-bit forms, which are the same on LP64 */
static void test_signal_sets(void) {
    static const char* const forms[2] = {"", "64"};
    for (int k = 0; k < 2; ++k) {
        char name[32];
        uint64_t set = ~0ull;
        snprintf(name, sizeof name, "sigemptyset%s", forms[k]);
        CALL(name, P(&set));
        snprintf(name, sizeof name, "sigaddset%s", forms[k]);
        CALL(name, P(&set), 10);
        CALL(name, P(&set), 64);
        snprintf(name, sizeof name, "sigismember%s", forms[k]);
        expect_int("SIGUSR1 (10) added", (int64_t)CALL(name, P(&set), 10), 1);
        expect_int("signal 64 added", (int64_t)CALL(name, P(&set), 64), 1);
        expect_int("SIGUSR2 (12) not", (int64_t)CALL(name, P(&set), 12), 0);
        expect_int("signal 0: -1", (int64_t)CALL(name, P(&set), 0), -1);
        expect_int("and EINVAL", *hle_errno_location(), 22);
        expect_int("signal 65: -1", (int64_t)CALL(name, P(&set), 65), -1);
        snprintf(name, sizeof name, "sigdelset%s", forms[k]);
        CALL(name, P(&set), 10);
        snprintf(name, sizeof name, "sigismember%s", forms[k]);
        expect_int("SIGUSR1 taken out", (int64_t)CALL(name, P(&set), 10), 0);
        snprintf(name, sizeof name, "sigfillset%s", forms[k]);
        CALL(name, P(&set));
        snprintf(name, sizeof name, "sigismember%s", forms[k]);
        expect_int("SIGUSR2 in a full set", (int64_t)CALL(name, P(&set), 12), 1);
    }
}

/* ---- locales ---- */

/* bionic's locales: the C locale alone, a name setting only MB_CUR_MAX, and the host's own locale never set */
static void test_bionic_locales(void) {
    char host[256];
    snprintf(host, sizeof host, "%s", setlocale(LC_ALL, NULL));
    expect_text("setlocale(LC_ALL, NULL)", (const char*)(uintptr_t)CALL("setlocale", 6, 0), "C.UTF-8");
    expect_int("MB_CUR_MAX from the start", (int64_t)CALL("__ctype_get_mb_cur_max", 0), 4);
    expect_text("setlocale(LC_ALL, \"\")", (const char*)(uintptr_t)CALL("setlocale", 6, P("")), "C.UTF-8");
    expect_int("setlocale of a locale bionic lacks", (int64_t)CALL("setlocale", 6, P("pt_BR.UTF-8")), 0);
    expect_int("and ENOENT", *hle_errno_location(), 2);
    expect_int("setlocale of category 13", (int64_t)CALL("setlocale", 13, P("C")), 0);
    expect_int("and EINVAL", *hle_errno_location(), 22);
    expect_text("setlocale(LC_ALL, \"POSIX\")", (const char*)(uintptr_t)CALL("setlocale", 6, P("POSIX")), "C");
    expect_int("MB_CUR_MAX in C", (int64_t)CALL("__ctype_get_mb_cur_max", 0), 1);
    expect_text("setlocale(LC_CTYPE, \"en_US.UTF-8\")", (const char*)(uintptr_t)CALL("setlocale", 0, P("en_US.UTF-8")),
                "C.UTF-8");
    expect_int("MB_CUR_MAX in C.UTF-8", (int64_t)CALL("__ctype_get_mb_cur_max", 0), 4);
    expect_text("the host's locale, after", setlocale(LC_ALL, NULL), host);

    /* a thread's locale_t: bionic's, which the _l functions take */
    uint64_t c_locale = CALL("newlocale", 0x1fbf, P("C"), 0);
    expect_int("newlocale(LC_ALL_MASK, \"C\") made one", c_locale != 0, 1);
    expect_int("the thread's first uselocale gives LC_GLOBAL_LOCALE", (int64_t)CALL("uselocale", c_locale), -1);
    expect_int("MB_CUR_MAX in it", (int64_t)CALL("__ctype_get_mb_cur_max", 0), 1);
    expect_int("uselocale(0) asks", (int64_t)CALL("uselocale", 0), (int64_t)c_locale);
    expect_int("strtoll_l in it", (int64_t)CALL("strtoll_l", P("-123"), 0, 10, c_locale), -123);
    expect_int("back to the global locale", (int64_t)CALL("uselocale", (uint64_t)-1), (int64_t)c_locale);
    expect_int("MB_CUR_MAX there", (int64_t)CALL("__ctype_get_mb_cur_max", 0), 4);
    uint64_t copy = CALL("duplocale", c_locale);
    CALL("uselocale", copy);
    expect_int("MB_CUR_MAX of a copy", (int64_t)CALL("__ctype_get_mb_cur_max", 0), 1);
    CALL("uselocale", (uint64_t)-1);
    CALL("freelocale", copy);
    CALL("freelocale", c_locale);
    expect_int("newlocale of a locale bionic lacks", (int64_t)CALL("newlocale", 0x1fbf, P("de_DE.UTF-8"), 0), 0);
    expect_int("and ENOENT", *hle_errno_location(), 2);
    expect_int("newlocale of LC_ALL's own bit", (int64_t)CALL("newlocale", 0x40, P("C"), 0), 0);
    expect_int("and EINVAL", *hle_errno_location(), 22);
}

/* ---- calls from guest code ---- */

/* name called as guest code calls it, through the bridge's export and its host call (bridge_call): the floating
   arguments in v0 and on, set before */
static uint64_t guest(const char* name, const uint64_t* x, unsigned n) {
    uint64_t f = hle_symbol(name);
    if (!f) {
        fprintf(stderr, "android_bionic: the HLE has no %s\n", name);
        exit(1);
    }
    return bridge_call(f, x, n);
}
#define GUEST(name, ...) guest(name, (uint64_t[]){__VA_ARGS__}, sizeof((uint64_t[]){__VA_ARGS__}) / 8)
static void set_v(unsigned n, double value) {
    memset(&bridge_cpu()->v[n], 0, sizeof bridge_cpu()->v[n]);
    memcpy(&bridge_cpu()->v[n].df[0], &value, sizeof value);
}

/* the host C library's functions that a guest's call reaches take bionic's C locale, whatever the process's (the
   user's, which the JVM sets): its numbers, its days' and months' names and UTF-8 characters. The thread's own locale
   is back once the call returns */
static void test_guest_locale(void) {
    char error[256];
    if (!bridge_init(NULL, error, sizeof error)) {
        fprintf(stderr, "android_bionic: the bridge does not start: %s\n", error);
        exit(1);
    }
    const char* comma = NULL;
#ifndef _WIN32
    static const char* const commas[] = {"pt_BR.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8", "es_ES.UTF-8",
                                         "it_IT.UTF-8", "ru_RU.UTF-8", "nl_NL.UTF-8"};
    for (size_t i = 0; i < sizeof commas / sizeof *commas && !comma; ++i)
        if (setlocale(LC_ALL, commas[i]) && !strcmp(localeconv()->decimal_point, ",")) comma = commas[i];
#endif
    if (!comma) {
        setlocale(LC_ALL, "C");
        printf("android_bionic: the host has no locale with a decimal comma: the guest's numbers and names are "
               "checked in the host's C locale alone\n");
    }
    /* multibyte characters of the C locale, ASCII on the host */
    setlocale(LC_CTYPE, "C");

    char out[64];
    set_v(0, 3.14159);
    set_v(1, 12345.678);
    GUEST("snprintf", P(out), sizeof out, P("%.2f|%e"));
    expect_text("snprintf of doubles", out, "3.14|1.234568e+04");
    double d = 0;
    expect_int("sscanf of a double", (int64_t)GUEST("sscanf", P("3.5 x"), P("%lf"), P(&d)), 1);
    expect_int("its value times 10", (int64_t)(d * 10), 35);
    char* end = NULL;
    GUEST("strtod", P("0x1.8p3"), P(&end));
    memcpy(&d, &bridge_cpu()->v[0].df[0], sizeof d);
    expect_int("strtod of a hex float", (int64_t)d, 12);
    expect_int("to its end", end && !*end, 1);
    GUEST("strtod", P("1.25"), 0);
    memcpy(&d, &bridge_cpu()->v[0].df[0], sizeof d);
    expect_int("strtod of 1.25, times 100", (int64_t)(d * 100), 125);
    struct tm tm = {.tm_year = 123, .tm_mon = 10, .tm_mday = 14, .tm_wday = 2, .tm_yday = 317}; /* 2023-11-14 */
    GUEST("strftime", P(out), sizeof out, P("%a %b %A %B"), P(&tm));
    expect_text("strftime's names", out, "Tue Nov Tuesday November");
    const char* point = *(const char* const*)(uintptr_t)GUEST("localeconv", 0);
    expect_text("localeconv's decimal point", point, ".");
    uint32_t wide = 0;
    expect_int("mbrtowc of a two-byte character", (int64_t)GUEST("mbrtowc", P(&wide), P("\xc3\xa7"), 2, 0), 2);
    expect_int("its code point", wide, 0xe7);

    /* the thread's own locale again */
    expect_text("the host's decimal point after", localeconv()->decimal_point, comma ? "," : ".");
#ifndef _WIN32
    expect_int("the thread on the process's locale after", uselocale((locale_t)0) == LC_GLOBAL_LOCALE, 1);
#endif
    setlocale(LC_ALL, "C");
}

/* mbstowcs and wcstombs in UTF-8, bionic's only multibyte encoding, whatever the host's locale (C here) */
static void test_multibyte_strings(void) {
    setlocale(LC_ALL, "C");
    const char* text = "a\xc3\xa7\xc3\xa3o"; /* "a", c cedilla, a tilde, "o" */
    expect_int("mbstowcs counts the characters", (int64_t)GUEST("mbstowcs", 0, P(text), 0), 4);
    uint32_t wide[8] = {0};
    expect_int("mbstowcs writes them", (int64_t)GUEST("mbstowcs", P(wide), P(text), 8), 4);
    expect_int("the second", wide[1], 0xe7);
    expect_int("the fourth", wide[3], 'o');
    expect_int("and a 0 after", wide[4], 0);
    expect_int("mbstowcs of a byte that starts no character", (int64_t)GUEST("mbstowcs", 0, P("a\xff"), 0), -1);
    expect_int("wcstombs counts the bytes", (int64_t)GUEST("wcstombs", 0, P(wide), 0), 6);
    char bytes[16] = "";
    expect_int("wcstombs writes them", (int64_t)GUEST("wcstombs", P(bytes), P(wide), sizeof bytes), 6);
    expect_text("as UTF-8", bytes, text);
    expect_int("wcstombs stops at a whole character that does not fit",
               (int64_t)GUEST("wcstombs", P(bytes), P(wide), 2), 1);
}

/* ---- names ---- */

/* bionic's struct addrinfo */
typedef struct GuestAddrinfo {
    int32_t flags, family, socktype, protocol;
    uint32_t addrlen;
    char* canonname;
    unsigned char* addr;
    struct GuestAddrinfo* next;
} GuestAddrinfo;

/* getaddrinfo's answer for a name, as text: "getaddrinfo N" for an error, else its addresses in order */
static void resolve(const char* name, int32_t family, char* out, size_t size) {
    GuestAddrinfo hints = {.family = family, .socktype = 1}, *list = NULL; /* SOCK_STREAM */
    int64_t r = (int32_t)CALL("getaddrinfo", P(name), 0, P(&hints), P(&list));
    if (r) {
        snprintf(out, size, "getaddrinfo %lld", (long long)r);
        return;
    }
    size_t n = 0;
    out[0] = 0;
    for (const GuestAddrinfo* a = list; a && n < size; a = a->next) {
        if (a->family == 2) /* AF_INET: port, then the address */
            n += (size_t)snprintf(out + n, size - n, "%s%u.%u.%u.%u", n ? " " : "", a->addr[4], a->addr[5], a->addr[6],
                                  a->addr[7]);
        else if (a->family == 10) /* AF_INET6 */
            n += (size_t)snprintf(out + n, size - n, "%sipv6", n ? " " : "");
    }
    CALL("freeaddrinfo", P(list));
}

/* offline, the Network setting's default, a name of another machine does not resolve: EAI_NODATA, as Android answers
   without a network */
static void test_offline_names(void) {
    char got[128];
    resolve("androidprobe.invalid", 0, got, sizeof got);
    expect_text("a name under .invalid", got, "getaddrinfo 7");
    resolve("example.com", 2, got, sizeof got);
    expect_text("another machine's name", got, "getaddrinfo 7");
    resolve("127.0.0.1", 0, got, sizeof got);
    expect_text("loopback's address", got, "127.0.0.1");
}

/* localhost is 127.0.0.1 alone, as Android's hosts file has it (::1 is ip6-localhost there), whatever the host's
   lists, in any case of its letters */
static void test_localhost(void) {
    char got[128];
    resolve("localhost", 0, got, sizeof got);
    expect_text("localhost", got, "127.0.0.1");
    resolve("LocalHost", 0, got, sizeof got);
    expect_text("LocalHost", got, "127.0.0.1");
    resolve("localhost", 2, got, sizeof got);
    expect_text("localhost of AF_INET", got, "127.0.0.1");
    GuestAddrinfo hints = {.flags = 2, .socktype = 1}, *list = NULL; /* AI_CANONNAME */
    expect_int("getaddrinfo of localhost's canonical name",
               (int32_t)CALL("getaddrinfo", P("localhost"), 0, P(&hints), P(&list)), 0);
    expect_text("its name", list && list->canonname ? list->canonname : "(none)", "localhost");
    CALL("freeaddrinfo", P(list));
}

int main(void) {
    test_printf_signed();
    test_sscanf_reads_its_pointers();
    test_error_texts();
    test_fortified();
    test_libm();
    test_getline();
    test_signal_sets();
    test_bionic_locales();
    test_offline_names();
    test_localhost();
    test_guest_locale();
    test_multibyte_strings();
    if (g_t.failures) {
        fprintf(stderr, "android_bionic: %d failed\n", g_t.failures);
        return 1;
    }
    puts("android_bionic: all passed");
    return 0;
}
