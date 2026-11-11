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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if defined(__linux__)
#include <sys/resource.h>
#endif
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <signal.h>
#include <sys/wait.h>
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
    unsigned char* m = plat_mem_reserve(2 * PAGE, 0);
    if (!m || plat_mem_commit(m, PAGE)) {
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
    plat_mem_release(m, 2 * PAGE);

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

/* ---- the bridge's own forms of bionic's functions ---- */

/* strlcpy, basename, strsep and strtok_r, which the host's C library lacks or has in another form, and fseek and
   ftell of 64-bit offsets (Windows' long is 32 bits): each through the adapter of its own type */
static void test_own_forms(void) {
    char out[8];
    expect_int("strlcpy gives the source's length", (int64_t)CALL("strlcpy", P(out), P("hello"), 3), 5);
    expect_text("and cuts to the room", out, "he");
    expect_text("basename drops trailing slashes", (const char*)(uintptr_t)CALL("basename", P("/a/b//")), "b");
    char list[] = "one,two";
    char* rest = list;
    expect_text("strsep's first field", (const char*)(uintptr_t)CALL("strsep", P(&rest), P(",")), "one");
    expect_text("and the rest", rest, "two");
    char words[] = "  x y";
    char* saved = NULL;
    expect_text("strtok_r's first word", (const char*)(uintptr_t)CALL("strtok_r", P(words), P(" "), P(&saved)), "x");
    expect_text("its second", (const char*)(uintptr_t)CALL("strtok_r", 0, P(" "), P(&saved)), "y");
    char* part = (char*)(uintptr_t)CALL("strndup", P("abcdef"), 3);
    expect_text("strndup takes n bytes", part ? part : "(null)", "abc");
    free(part);
    part = (char*)(uintptr_t)CALL("strndup", P("ab"), 5);
    expect_text("and stops at the end", part ? part : "(null)", "ab");
    free(part);
    FILE* f = fopen("android_bionic_seek.txt", "w+b");
    if (!f) {
        fprintf(stderr, "android_bionic: cannot write android_bionic_seek.txt in the working folder\n");
        exit(1);
    }
    fputs("0123456789", f);
    static const char* const seeks[2] = {"fseek", "fseeko"};
    static const char* const tells[2] = {"ftell", "ftello"};
    for (int k = 0; k < 2; ++k) {
        expect_int(seeks[k], (int64_t)CALL(seeks[k], P(f), 3 + (uint64_t)k, 0), 0); /* SEEK_SET */
        expect_int(tells[k], (int64_t)CALL(tells[k], P(f)), 3 + k);
    }
    fclose(f);
    remove("android_bionic_seek.txt");
}

/* ---- what the NDK's _FORTIFY_SOURCE calls ---- */

/* send(), recv() and readlink() of a buffer whose size the compiler knows, as __sendto_chk, __recvfrom_chk and
   __readlink_chk: over a TCP connection on loopback, which every host has (Windows has no socketpair) */
static void test_fortified(void) {
    /* first, a descriptor that is no socket: ENOTSOCK, Winsock's own error once the HLE has started it */
    FILE* file = fopen("android_bionic_notsock.txt", "w+b");
    if (!file) {
        fprintf(stderr, "android_bionic: cannot write android_bionic_notsock.txt in the working folder\n");
        exit(1);
    }
    expect_int("__sendto_chk on a file", (int64_t)CALL("__sendto_chk", I32(fileno(file)), P("abc"), 3, 4, 0, 0, 0), -1);
    expect_int("and ENOTSOCK", *hle_errno_location(), 88);
    fclose(file);
    remove("android_bionic_notsock.txt");

    /* AF_INET (2), SOCK_STREAM (1), and Linux's sockaddr_in of 127.0.0.1, port 0 */
    unsigned char address[16] = {2, 0, 0, 0, 127, 0, 0, 1};
    uint32_t length = sizeof address;
    int32_t server = (int32_t)CALL("socket", 2, 1, 0), client = (int32_t)CALL("socket", 2, 1, 0);
    expect_int("bind, listen and getsockname on loopback",
               (int64_t)CALL("bind", I32(server), P(address), sizeof address) |
                   (int64_t)CALL("listen", I32(server), 1) |
                   (int64_t)CALL("getsockname", I32(server), P(address), P(&length)),
               0);
    expect_int("connect", (int64_t)CALL("connect", I32(client), P(address), sizeof address), 0);
    int32_t accepted = (int32_t)CALL("accept", I32(server), 0, 0);
    expect_int("__sendto_chk, send's FORTIFY form", (int64_t)CALL("__sendto_chk", I32(client), P("abc"), 3, 4, 0, 0, 0),
               3);
    char got[8] = "";
    expect_int("__recvfrom_chk, recv's (MSG_WAITALL)",
               (int64_t)CALL("__recvfrom_chk", I32(accepted), P(got), 3, sizeof got, 0x100, 0, 0), 3);
    expect_text("what came through", got, "abc");
    CALL("close", I32(accepted));
    CALL("close", I32(client));
    CALL("close", I32(server));
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

/* ---- the ones edenprobe's library imports that the HLE once lacked ---- */

/* name called with two doubles or two floats: the result's bits */
static uint64_t call_dd(const char* name, double a, double b) {
    call(name, NULL, 0, (double[]){a, b}, 2);
    uint64_t bits;
    memcpy(&bits, &g_t.cpu.v[0].df[0], 8);
    return bits;
}
static uint32_t call_ff(const char* name, float a, float b) {
    g_t.cpu = (CPU){0};
    memcpy(&g_t.cpu.v[0].f[0], &a, 4);
    memcpy(&g_t.cpu.v[1].f[0], &b, 4);
    const HleSymbol* s = find(name);
    s->handler(&g_t.cpu, s->data);
    uint32_t bits;
    memcpy(&bits, &g_t.cpu.v[0].f[0], 4);
    return bits;
}
#ifndef _WIN32
/* name called with x in a child, which must end on SIGABRT with want in what it printed */
static void expect_abort(const char* name, const uint64_t* x, size_t nx, const char* want) {
    int out[2];
    fflush(NULL);
    if (pipe(out)) exit(1);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(out[1], 2);
        close(out[0]);
        call(name, x, nx, NULL, 0);
        _exit(0);
    }
    close(out[1]);
    char said[512];
    size_t got = 0;
    ssize_t n;
    while (got < sizeof said - 1 && (n = read(out[0], said + got, sizeof said - 1 - got)) > 0) got += (size_t)n;
    said[got] = 0;
    close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT || !strstr(said, want)) {
        fprintf(stderr, "android_bionic: %s: wanted an abort that says \"%s\", got \"%s\"\n", name, want, said);
        g_t.failures++;
    }
}
#define ABORTS(name, want, ...)                                                                                        \
    expect_abort(name, (uint64_t[]){__VA_ARGS__}, sizeof((uint64_t[]){__VA_ARGS__}) / 8, want)
#endif

static void test_imports_once_missing(void) {
    expect_int("nextafter(1, 2)", (int64_t)call_dd("nextafter", 1.0, 2.0), 0x3ff0000000000001ll);
    expect_int("nextafterf(1, 2)", call_ff("nextafterf", 1.0f, 2.0f), 0x3f800001);
    expect_int("fmax(-0.5, 2)", (int64_t)call_dd("fmax", -0.5, 2.0), 0x4000000000000000ll);
    expect_int("fmin(-0.5, 2)", (int64_t)call_dd("fmin", -0.5, 2.0), (int64_t)0xbfe0000000000000ull);
    expect_int("fmaxf(3, -1)", call_ff("fmaxf", 3.0f, -1.0f), 0x40400000);
    expect_int("fminf(3, -1)", call_ff("fminf", 3.0f, -1.0f), 0xbf800000);

    const char* path = "android_bionic_once_missing.txt";
    FILE* f = fopen(path, "w+b");
    if (!f) {
        fprintf(stderr, "android_bionic: cannot write %s in the working folder\n", path);
        exit(1);
    }
    fputs("xline\nrest", f);
    rewind(f);
    expect_int("fgetc", (int64_t)CALL("fgetc", P(f)), 'x');
    char buffer[16] = "";
    expect_int("__fgets_chk gives the buffer", (int64_t)CALL("__fgets_chk", P(buffer), 16, P(f), 16),
               (int64_t)(uintptr_t)buffer);
    expect_text("__fgets_chk reads the line", buffer, "line\n");
    const char* hello = "hello";
    expect_int("__memchr_chk", (int64_t)CALL("__memchr_chk", P(hello), 'l', 5, 8), (int64_t)(uintptr_t)(hello + 2));
    char copy[8];
    memset(copy, 'z', sizeof copy);
    CALL("__strncpy_chk", P(copy), P("hi"), 4, 8);
    expect_int("__strncpy_chk pads with NULs", !memcmp(copy, "hi\0\0zzzz", 8), 1);
#ifndef _WIN32
    ABORTS("__fgets_chk", "fgets: prevented 16-byte write into 8-byte buffer", P(buffer), 16, P(f), 8);
    ABORTS("__fgets_chk", "fgets: buffer size -1 < 0", P(buffer), I32(-1), P(f), 8);
    ABORTS("__memchr_chk", "memchr: prevented 9-byte read from 8-byte buffer", P(hello), 'z', 9, 8);
    ABORTS("__strncpy_chk", "strncpy: prevented 9-byte write into 8-byte buffer", P(copy), P("hi"), 9, 8);
#endif
    fclose(f);
    remove(path);

    char folder[] = "android_bionic_dXXXXXX";
    expect_int("mkdtemp gives its template", (int64_t)CALL("mkdtemp", P(folder)), (int64_t)(uintptr_t)folder);
    struct stat st;
    expect_int("mkdtemp makes a folder", !strstr(folder, "XXXXXX") && !stat(folder, &st), 1);
#ifdef _WIN32
    _rmdir(folder);
#else
    rmdir(folder);
#endif
}

/* ---- the second batch: the file calls, *at, pipe2, links, sessions, fma and lround ---- */

static uint64_t call_ddd(const char* name, double a, double b, double c) {
    call(name, NULL, 0, (double[]){a, b, c}, 3);
    uint64_t bits;
    memcpy(&bits, &g_t.cpu.v[0].df[0], 8);
    return bits;
}
static uint32_t call_fff(const char* name, float a, float b, float c) {
    g_t.cpu = (CPU){0};
    memcpy(&g_t.cpu.v[0].f[0], &a, 4);
    memcpy(&g_t.cpu.v[1].f[0], &b, 4);
    memcpy(&g_t.cpu.v[2].f[0], &c, 4);
    const HleSymbol* s = find(name);
    s->handler(&g_t.cpu, s->data);
    uint32_t bits;
    memcpy(&bits, &g_t.cpu.v[0].f[0], 4);
    return bits;
}
static uint64_t call_d(const char* name, double a) { return call(name, NULL, 0, (double[]){a}, 1); }
static uint64_t call_f(const char* name, float a) {
    g_t.cpu = (CPU){0};
    memcpy(&g_t.cpu.v[0].f[0], &a, 4);
    const HleSymbol* s = find(name);
    s->handler(&g_t.cpu, s->data);
    return g_t.cpu.x[0];
}

static void test_fma_and_lround(void) {
    expect_int("fma(2, 3, 4)", (int64_t)call_ddd("fma", 2.0, 3.0, 4.0), 0x4024000000000000ll); /* 10 */
    /* (2^27+1)^2 less 2^54 is 2^28+1; a product rounded before the sum gives 2^28 */
    double big = 134217729.0;
    double want = 268435457.0;
    uint64_t want_bits;
    memcpy(&want_bits, &want, 8);
    expect_int("fma keeps the product whole", (int64_t)call_ddd("fma", big, big, -18014398509481984.0),
               (int64_t)want_bits);
    expect_int("fmaf(2, 3, 4)", call_fff("fmaf", 2.0f, 3.0f, 4.0f), 0x41200000); /* 10 */
    expect_int("lround(2.5) goes away from zero", (int64_t)call_d("lround", 2.5), 3);
    expect_int("lround(-2.5)", (int64_t)call_d("lround", -2.5), -3);
    expect_int("llround(10000000000.5) past 32 bits", (int64_t)call_d("llround", 10000000000.5), 10000000001ll);
    expect_int("lround past 32 bits (long is 64)", (int64_t)call_d("lround", -5000000000.25), -5000000000ll);
    expect_int("lroundf(-0.5)", (int64_t)call_f("lroundf", -0.5f), -1);
    expect_int("llroundf(2.4)", (int64_t)call_f("llroundf", 2.4f), 2);
}

/* a descriptor from name through the HLE, which opens it as the guest does */
enum {
    G_O_RDWR = 2,
    G_O_CREAT = 64,
    G_O_TRUNC = 512,
    G_O_NONBLOCK = 2048,
    G_O_DIRECTORY = 040000,
    G_O_NOFOLLOW = 0100000,
    G_O_CLOEXEC = 524288,
    G_AT_FDCWD = -100
};
#define SAID_ENOSYS(what, r)                                                                                           \
    (expect_int(what, (int64_t)(r), -1), expect_int(what " sets ENOSYS", *hle_errno_location(), 38))

#ifndef _WIN32
/* the host's dirfd, which test_files_second_batch's folder descriptor hides */
static int stream_descriptor(DIR* stream) { return dirfd(stream); }
#endif

static void test_files_second_batch(void) {
    const char* path = "android_bionic_more.txt";
    const char* second = "android_bionic_more_2.txt";
    const char* folder = "android_bionic_more_dir";
    remove(path);
    remove(second);
    CALL("unlinkat", I32(G_AT_FDCWD), P(folder), 0x200); /* a failed run's leftover */
    int64_t fd = (int64_t)CALL("open64", P(path), G_O_RDWR | G_O_CREAT | G_O_TRUNC, 0600);
    expect_int("open64 made a file", fd >= 0, 1);
    if (fd < 0) return;
    expect_int("pwrite", (int64_t)CALL("pwrite", I32(fd), P("abcdef"), 6, 0), 6);
    expect_int("pwrite64 at 2", (int64_t)CALL("pwrite64", I32(fd), P("XY"), 2, 2), 2);
    char back[8] = "";
    expect_int("pread64", (int64_t)CALL("pread64", I32(fd), P(back), 6, 0), 6);
    expect_text("pwrite's bytes", back, "abXYef");
    expect_int("ftruncate64", (int64_t)CALL("ftruncate64", I32(fd), 3), 0);
    expect_int("fdatasync", (int64_t)CALL("fdatasync", I32(fd)), 0);
    memset(back, 0, sizeof back);
    expect_int("pread64 of the cut file", (int64_t)CALL("pread64", I32(fd), P(back), 6, 0), 3);
    expect_text("what is left", back, "abX");
    expect_int("pwrite to -1", (int64_t)CALL("pwrite", I32(-1), P("a"), 1, 0), -1);
    expect_int("pwrite to -1: EBADF", *hle_errno_location(), 9);
    CALL("close", I32(fd));

    /* creat: write only, truncating */
    fd = (int64_t)CALL("creat", P(second), 0600);
    expect_int("creat", fd >= 0, 1);
    CALL("close", I32(fd));
    struct stat st;
    expect_int("creat made the file", stat(second, &st), 0);
    expect_int("and it is empty", (int64_t)st.st_size, 0);

    /* openat and __openat_2 against the working folder, faccessat and unlinkat */
    fd = (int64_t)CALL("openat", I32(G_AT_FDCWD), P(path), 0, 0);
    expect_int("openat(AT_FDCWD)", fd >= 0, 1);
    CALL("close", I32(fd));
    fd = (int64_t)CALL("__openat_2", I32(G_AT_FDCWD), P(path), 0);
    expect_int("__openat_2", fd >= 0, 1);
    CALL("close", I32(fd));
    expect_int("openat of nothing", (int64_t)CALL("openat", I32(G_AT_FDCWD), P("android_bionic_none"), 0, 0), -1);
    expect_int("and ENOENT", *hle_errno_location(), 2);
    expect_int("faccessat", (int64_t)CALL("faccessat", I32(G_AT_FDCWD), P(path), 4, 0), 0);
    expect_int("faccessat of nothing", (int64_t)CALL("faccessat", I32(G_AT_FDCWD), P("android_bionic_none"), 0, 0), -1);
    expect_int("and ENOENT", *hle_errno_location(), 2);
    expect_int("faccessat of a flag it lacks", (int64_t)CALL("faccessat", I32(G_AT_FDCWD), P(path), 0, 0x1000), -1);
    expect_int("and EINVAL", *hle_errno_location(), 22);
    expect_int("unlinkat of a flag it lacks", (int64_t)CALL("unlinkat", I32(G_AT_FDCWD), P(second), 0x1000), -1);
    expect_int("and EINVAL", *hle_errno_location(), 22);
    expect_int("unlinkat", (int64_t)CALL("unlinkat", I32(G_AT_FDCWD), P(second), 0), 0);
    expect_int("it is gone", stat(second, &st), -1);
    expect_int("mkdir", (int64_t)CALL("mkdir", P(folder), 0700), 0);
    expect_int("unlinkat of a folder without AT_REMOVEDIR", (int64_t)CALL("unlinkat", I32(G_AT_FDCWD), P(folder), 0),
               -1);
    expect_int("unlinkat AT_REMOVEDIR", (int64_t)CALL("unlinkat", I32(G_AT_FDCWD), P(folder), 0x200), 0);
    expect_int("the folder is gone", stat(folder, &st), -1);
#ifndef _WIN32
    int dirfd = open(".", O_RDONLY);
    fd = (int64_t)CALL("openat", I32(dirfd), P(path), 0, 0);
    expect_int("openat of a folder descriptor", fd >= 0, 1);
    CALL("close", I32(fd));
    expect_int("faccessat of a folder descriptor", (int64_t)CALL("faccessat", I32(dirfd), P(path), 0, 0), 0);
    expect_int("fchdir", (int64_t)CALL("fchdir", I32(dirfd)), 0);
    close(dirfd);
    expect_int("chown to our own ids", (int64_t)CALL("chown", P(path), I32(getuid()), I32(getgid())), 0);
    ABORTS("__openat_2", "open(O_CREAT) called without specifying a mode", I32(G_AT_FDCWD), P(path), G_O_CREAT);
#else
    SAID_ENOSYS("openat of a folder descriptor", CALL("openat", I32(7), P("relative"), 0, 0));
    SAID_ENOSYS("fchdir", CALL("fchdir", 0));
    SAID_ENOSYS("chown", CALL("chown", P(path), 0, 0));
#endif

    /* fstatat: the guest's struct stat (st_size at 48), of a path and of a descriptor itself (AT_EMPTY_PATH) */
    unsigned char guest_stat[128];
    uint64_t size = 0;
    memset(guest_stat, 0, sizeof guest_stat);
    expect_int("fstatat", (int64_t)CALL("fstatat", I32(G_AT_FDCWD), P(path), P(guest_stat), 0), 0);
    memcpy(&size, guest_stat + 48, sizeof size);
    expect_int("fstatat's size", (int64_t)size, 3);
    expect_int("fstatat of nothing", (int64_t)CALL("fstatat", I32(G_AT_FDCWD), P("android_bionic_none"), P(guest_stat), 0),
               -1);
    expect_int("and ENOENT", *hle_errno_location(), 2);
    expect_int("fstatat of a flag it lacks", (int64_t)CALL("fstatat", I32(G_AT_FDCWD), P(path), P(guest_stat), 1), -1);
    expect_int("and EINVAL", *hle_errno_location(), 22);
    fd = (int64_t)CALL("openat", I32(G_AT_FDCWD), P(path), 0, 0);
    memset(guest_stat, 0, sizeof guest_stat);
    expect_int("fstatat AT_EMPTY_PATH", (int64_t)CALL("fstatat64", I32(fd), P(""), P(guest_stat), 0x1000), 0);
    memcpy(&size, guest_stat + 48, sizeof size);
    expect_int("the descriptor's size", (int64_t)size, 3);
    expect_int("an empty path without it", (int64_t)CALL("fstatat", I32(fd), P(""), P(guest_stat), 0), -1);
    expect_int("is ENOENT", *hle_errno_location(), 2);
    CALL("close", I32(fd));
#ifndef _WIN32
    DIR* folder_stream = opendir(".");
    int stream_fd = folder_stream ? stream_descriptor(folder_stream) : -1;
    expect_int("dirfd", (int64_t)CALL("dirfd", P(folder_stream)), stream_fd);
    memset(guest_stat, 0, sizeof guest_stat);
    expect_int("fstatat of a folder descriptor", (int64_t)CALL("fstatat", I32(stream_fd), P(path), P(guest_stat), 0x100),
               0);
    memcpy(&size, guest_stat + 48, sizeof size);
    expect_int("its size", (int64_t)size, 3);
    if (folder_stream) closedir(folder_stream);
#else
    SAID_ENOSYS("dirfd", CALL("dirfd", 0));
#endif

    /* links: a hard link reads the same bytes, a second one is EEXIST */
    expect_int("link", (int64_t)CALL("link", P(path), P(second)), 0);
    FILE* linked = fopen(second, "rb");
    expect_int("the link opens", linked != NULL, 1);
    if (linked) {
        memset(back, 0, sizeof back);
        expect_int("and holds the bytes", fread(back, 1, 3, linked), 3);
        expect_text("the same", back, "abX");
        fclose(linked);
    }
    expect_int("link over a file", (int64_t)CALL("link", P(path), P(second)), -1);
    expect_int("and EEXIST", *hle_errno_location(), 17);
    remove(second);
#ifndef _WIN32
    expect_int("symlink", (int64_t)CALL("symlink", P(path), P(second)), 0);
    /* the guest's flags (AArch64's values, which x86-64's differ from) reach the host as its own */
    fd = (int64_t)CALL("openat", I32(G_AT_FDCWD), P("."), G_O_DIRECTORY, 0);
    expect_int("openat of a folder with O_DIRECTORY", fd >= 0, 1);
    CALL("close", I32(fd));
    expect_int("openat of a file with O_DIRECTORY", (int64_t)CALL("openat", I32(G_AT_FDCWD), P(path), G_O_DIRECTORY, 0),
               -1);
    expect_int("is ENOTDIR", *hle_errno_location(), 20);
    expect_int("openat of a symlink with O_NOFOLLOW",
               (int64_t)CALL("openat", I32(G_AT_FDCWD), P(second), G_O_NOFOLLOW, 0), -1);
    expect_int("is ELOOP", *hle_errno_location(), 40);
    char target[64] = "";
    expect_int("readlink of it", (int64_t)CALL("readlink", P(second), P(target), sizeof target - 1),
               (int64_t)strlen(path));
    expect_text("symlink's target", target, path);
    remove(second);
#else
    SAID_ENOSYS("symlink", CALL("symlink", P(path), P(second)));
#endif
    remove(path);

    /* mkstemp */
    char temp[] = "android_bionic_mXXXXXX";
    fd = (int64_t)CALL("mkstemp", P(temp));
    expect_int("mkstemp", fd >= 0, 1);
    expect_int("it made the name", !strstr(temp, "XXXXXX") && !stat(temp, &st), 1);
    CALL("close", I32(fd));
    remove(temp);
    /* bionic's rule, 6 X's or more at the end, whatever the host's C library takes (macOS' makes the name as it is) */
    char bad[] = "android_bionic_mYYYYYY";
    expect_int("mkstemp of a template without XXXXXX", (int64_t)CALL("mkstemp", P(bad)), -1);
    expect_int("and EINVAL", *hle_errno_location(), 22);
    expect_int("nothing made of it", stat(bad, &st), -1);
    char five[] = "android_bionic_mXXXXX";
    expect_int("mkstemp of 5 X's", (int64_t)CALL("mkstemp", P(five)), -1);
    expect_int("and EINVAL", *hle_errno_location(), 22);
    char folder_five[] = "android_bionic_dXXXXX";
    expect_int("mkdtemp of 5 X's", (int64_t)CALL("mkdtemp", P(folder_five)), 0);
    expect_int("and EINVAL", *hle_errno_location(), 22);
    expect_int("nothing made of it", stat(folder_five, &st), -1);
}

static void test_pipe2_umask_session(void) {
    int fds[2] = {-1, -1};
    expect_int("pipe2", (int64_t)CALL("pipe2", P(fds), 0), 0);
    expect_int("pipe2 wrote", (int64_t)CALL("write", I32(fds[1]), P("hi"), 2), 2);
    char got[4] = "";
    expect_int("pipe2 read", (int64_t)CALL("read", I32(fds[0]), P(got), 3), 2);
    expect_text("what the pipe held", got, "hi");
    CALL("close", I32(fds[0]));
    CALL("close", I32(fds[1]));
    expect_int("pipe2 of a flag it lacks", (int64_t)CALL("pipe2", P(fds), 0x1000), -1);
    expect_int("and EINVAL", *hle_errno_location(), 22);
#ifdef _WIN32
    SAID_ENOSYS("pipe2 with O_NONBLOCK", CALL("pipe2", P(fds), G_O_NONBLOCK));
#else
    expect_int("pipe2 with both flags", (int64_t)CALL("pipe2", P(fds), G_O_NONBLOCK | G_O_CLOEXEC), 0);
    for (int i = 0; i < 2; ++i) {
        expect_int("close-on-exec", fcntl(fds[i], F_GETFD) & FD_CLOEXEC, FD_CLOEXEC);
        expect_int("non-blocking", (fcntl(fds[i], F_GETFL) & O_NONBLOCK) != 0, 1);
    }
    expect_int("a read of an empty one", (int64_t)CALL("read", I32(fds[0]), P(got), 1), -1);
    expect_int("is EAGAIN", *hle_errno_location(), 11);
    CALL("close", I32(fds[0]));
    CALL("close", I32(fds[1]));
#endif

    uint64_t old = CALL("umask", 077);
    expect_int("umask gives the one set before", (int64_t)CALL("umask", old), 077);
    expect_int("umask back", (int64_t)CALL("umask", old), (int64_t)old);
    expect_int("__umask_chk is umask", (int64_t)CALL("__umask_chk", 077), (int64_t)old);
    expect_int("and gives it back", (int64_t)CALL("__umask_chk", old), 077);
#ifndef _WIN32
    ABORTS("__umask_chk", "umask: called with invalid mask", 01777);
#endif

#ifdef _WIN32
    expect_int("getpgrp is the process", (int64_t)CALL("getpgrp"), (int64_t)host_pid());
#else
    expect_int("getpgrp", (int64_t)CALL("getpgrp"), (int64_t)getpgrp());
#endif
    /* never the host's: a session of its own would take the emulator from its terminal */
    SAID_ENOSYS("setsid", CALL("setsid"));

#if defined(__linux__)
    int64_t event = (int64_t)CALL("eventfd", 5, G_O_NONBLOCK);
    expect_int("eventfd", event >= 0, 1);
    uint64_t count = 0;
    expect_int("eventfd read", (int64_t)CALL("read", I32(event), P(&count), 8), 8);
    expect_int("eventfd's count", (int64_t)count, 5);
    CALL("close", I32(event));
#else
    SAID_ENOSYS("eventfd", CALL("eventfd", 0, 0));
#endif
    /* memfd_create on every host: Linux's own, an unlinked temporary file elsewhere */
    int64_t memory = (int64_t)CALL("memfd_create", P("android_bionic"), 1);
    expect_int("memfd_create", memory >= 0, 1);
    expect_int("memfd write", (int64_t)CALL("write", I32(memory), P("mem"), 3), 3);
    char mem[4] = "";
    expect_int("memfd pread", (int64_t)CALL("pread64", I32(memory), P(mem), 3, 0), 3);
    expect_text("memfd's bytes", mem, "mem");
    CALL("close", I32(memory));
}

/* getpriority, the kernel's system call: after the bridge started (test_guest_locale), as guest code always runs, for
   the thread's kernel state it takes */
static void test_getpriority(void) {
#if defined(__linux__)
    expect_int("getpriority of this process", (int64_t)CALL("getpriority", 0, 0), getpriority(PRIO_PROCESS, 0));
#else
    expect_int("getpriority of this process", (int64_t)CALL("getpriority", 0, 0), 0); /* the kernel gives nice 0 */
#endif
}

static void test_strncat_chk(void) {
    char buffer[16] = "ab";
    expect_int("__strncat_chk gives dst", (int64_t)CALL("__strncat_chk", P(buffer), P("cdefg"), 3, 16),
               (int64_t)(uintptr_t)buffer);
    expect_text("__strncat_chk appends len bytes", buffer, "abcde");
    CALL("__strncat_chk", P(buffer), P("xyz"), 0, 16);
    expect_text("and none for len 0", buffer, "abcde");
    CALL("__strncat_chk", P(buffer), P("12"), 9, 16);
    expect_text("and stops at src's NUL", buffer, "abcde12");
#ifndef _WIN32
    char tight[8] = "abc";
    ABORTS("__strncat_chk", "strncat: prevented write past end of buffer (5 bytes and a NUL into 5)", P(tight),
           P("defgh"), 5, 8);
    char unterminated[4] = {'a', 'a', 'a', 'a'};
    ABORTS("__strncat_chk", "strlen: buffer overflow", P(unterminated), P("b"), 1, 4);
#endif
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
    hle_posix_init(); /* as the HLE's first lookup does */
    test_printf_signed();
    test_sscanf_reads_its_pointers();
    test_error_texts();
    test_own_forms();
    test_fortified();
    test_libm();
    test_getline();
    test_imports_once_missing();
    test_fma_and_lround();
    test_files_second_batch();
    test_pipe2_umask_session();
    test_strncat_chk();
    test_signal_sets();
    test_bionic_locales();
    test_offline_names();
    test_localhost();
    test_guest_locale();
    test_getpriority();
    test_multibyte_strings();
    if (g_t.failures) {
        fprintf(stderr, "android_bionic: %d failed\n", g_t.failures);
        return 1;
    }
    puts("android_bionic: all passed");
    return 0;
}
