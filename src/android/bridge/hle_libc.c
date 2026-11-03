/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's libc and libm as HLE (hle.h). A guest pointer is a host pointer and the LP64 layouts agree, so most of
   libc is the host's own function, called with the guest's registers by one adapter (PASS). The functions whose types
   differ between bionic on AArch64 and the host's C library have their own adapters here or in hle_posix.c */
#define _GNU_SOURCE
#include "android/bridge/args.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include <ctype.h>
#include <fenv.h>
#include <malloc.h>
#include <unistd.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

/* ---- the generic adapters ---- */

/* a host function of up to 8 integer or pointer arguments and an integer, a pointer or no result. x86-64 and AArch64
   pass those in registers in the same order, and a callee ignores registers it doesn't take */
typedef uint64_t (*Pass8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
void hle_pass(CPU* c, void* fn) {
    X(0) = ((Pass8)fn)(X(0), X(1), X(2), X(3), X(4), X(5), X(6), X(7));
}

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "hle_call passes the guest's registers as the host's: an x86-64 or AArch64 host"
#endif
typedef uint64_t (*CallInt)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, double, double, double, double,
                            double, double, double, double);
typedef double (*CallFloat)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, double, double, double, double,
                            double, double, double, double);
void hle_call(CPU* c, void* fn, char kind) {
    double v[8];
    for (int i = 0; i < 8; ++i) memcpy(&v[i], &c->v[i].d[0], sizeof v[i]);
    if (kind == 'F' || kind == 'D') {
        double r = ((CallFloat)fn)(X(0), X(1), X(2), X(3), X(4), X(5), v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        memset(&c->v[0], 0, sizeof c->v[0]);
        /* a float result is the register's low half: the double's bits carry it */
        memcpy(&c->v[0].d[0], &r, sizeof r);
        if (kind == 'F') memset(&c->v[0].b[4], 0, 4);
        return;
    }
    X(0) = ((CallInt)fn)(X(0), X(1), X(2), X(3), X(4), X(5), v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}

typedef uint64_t (*CallWide)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, double, double, double, double, double, double, double, double);
void hle_call_wide(CPU* c, void* fn, char kind) {
    if (kind == 'F' || kind == 'D') {
        hle_call(c, fn, kind);
        return;
    }
    /* the integers past x7 are the guest's stack slots, in order; the host's past its sixth are its stack's, in
       order: the floating ones stay in registers on both */
    uint64_t s[4];
    memcpy(s, (const void*)(uintptr_t)c->x[31], sizeof s);
    double v[8];
    for (int i = 0; i < 8; ++i) memcpy(&v[i], &c->v[i].d[0], sizeof v[i]);
    X(0) = ((CallWide)fn)(X(0), X(1), X(2), X(3), X(4), X(5), X(6), X(7), s[0], s[1], s[2], s[3], v[0], v[1], v[2],
                          v[3], v[4], v[5], v[6], v[7]);
}

double hle_d(const CPU* c, unsigned n) {
    double v;
    memcpy(&v, &c->v[n].df[0], sizeof v);
    return v;
}
float hle_f(const CPU* c, unsigned n) {
    float v;
    memcpy(&v, &c->v[n].f[0], sizeof v);
    return v;
}
void hle_set_d(CPU* c, double value) {
    memset(&c->v[0], 0, sizeof c->v[0]);
    memcpy(&c->v[0].df[0], &value, sizeof value);
}
void hle_set_f(CPU* c, float value) {
    memset(&c->v[0], 0, sizeof c->v[0]);
    memcpy(&c->v[0].f[0], &value, sizeof value);
}

/* math by its type: d for double, f for float, i for int, p for a pointer */
static void d_d(CPU* c, void* fn) { hle_set_d(c, ((double (*)(double))fn)(hle_d(c, 0))); }
static void d_dd(CPU* c, void* fn) { hle_set_d(c, ((double (*)(double, double))fn)(hle_d(c, 0), hle_d(c, 1))); }
static void f_f(CPU* c, void* fn) { hle_set_f(c, ((float (*)(float))fn)(hle_f(c, 0))); }
static void f_ff(CPU* c, void* fn) { hle_set_f(c, ((float (*)(float, float))fn)(hle_f(c, 0), hle_f(c, 1))); }
static void d_di(CPU* c, void* fn) { hle_set_d(c, ((double (*)(double, int))fn)(hle_d(c, 0), (int)X(0))); }
static void d_dp(CPU* c, void* fn) { hle_set_d(c, ((double (*)(double, void*))fn)(hle_d(c, 0), PTR(0))); }
static void f_fp(CPU* c, void* fn) { hle_set_f(c, ((float (*)(float, void*))fn)(hle_f(c, 0), PTR(0))); }
static void d_p(CPU* c, void* fn) { hle_set_d(c, ((double (*)(void*))fn)(PTR(0))); }
static void d_pp(CPU* c, void* fn) { hle_set_d(c, ((double (*)(void*, void*))fn)(PTR(0), PTR(1))); }
static void f_pp(CPU* c, void* fn) { hle_set_f(c, ((float (*)(void*, void*))fn)(PTR(0), PTR(1))); }
static void sincos_d(CPU* c, void* fn) {
    (void)fn;
    double x = hle_d(c, 0), s = sin(x), co = cos(x);
    memcpy(PTR(0), &s, sizeof s);
    memcpy(PTR(1), &co, sizeof co);
}
static void sincos_f(CPU* c, void* fn) {
    (void)fn;
    float x = hle_f(c, 0), s = sinf(x), co = cosf(x);
    memcpy(PTR(0), &s, sizeof s);
    memcpy(PTR(1), &co, sizeof co);
}
/* time_t difftime(time_t, time_t): a double of two integers */
static void difftime_(CPU* c, void* fn) {
    (void)fn;
    hle_set_d(c, difftime((time_t)X(0), (time_t)X(1)));
}

/* ---- the C++ runtime's hooks in libc ---- */

/* the destructors of a library's objects, run as the process ends: the process ends with the JVM, which runs none */
static void cxa_atexit(CPU* c, void* data) {
    (void)data;
    X(0) = 0;
}
static void nothing(CPU* c, void* data) {
    (void)c;
    (void)data;
}
static void zero(CPU* c, void* data) {
    (void)data;
    X(0) = 0;
}

/* ---- the stack protector, assertions, aborts ---- */

static uint64_t stack_chk_guard;
static void stack_chk_fail(CPU* c, void* data) {
    (void)data;
    fprintf(stderr, "scarletnx-android: stack corruption detected (the guest's __stack_chk_fail, from 0x%llx)\n",
            (unsigned long long)X(30));
    abort();
}
static void assert2(CPU* c, void* data) {
    (void)data;
    fprintf(stderr, "scarletnx-android: %s:%d: %s: assertion \"%s\" failed\n", STR(0), (int)X(1), STR(2), STR(3));
    abort();
}
static void set_abort_message(CPU* c, void* data) {
    (void)data;
    fprintf(stderr, "scarletnx-android: abort message: %s\n", STR(0));
}
static void guest_exit(CPU* c, void* data) {
    (void)data;
    fprintf(stderr, "scarletnx-android: the guest called exit(%d)\n", (int)X(0));
    exit((int)X(0));
}

/* ---- errno: the host's, whose numbers are Linux's on both ---- */
static void errno_location(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)&errno;
}

/* ---- the fortified functions (_FORTIFY_SOURCE): the plain function, with bionic's check of the size ---- */
static void fortify_fail(const char* what) {
    fprintf(stderr, "scarletnx-android: FORTIFY: %s: buffer overflow\n", what);
    abort();
}
static void memcpy_chk(CPU* c, void* data) {
    (void)data;
    if (X(2) > X(3)) fortify_fail("memcpy");
    X(0) = (uint64_t)(uintptr_t)memcpy(PTR(0), PTR(1), X(2));
}
static void memmove_chk(CPU* c, void* data) {
    (void)data;
    if (X(2) > X(3)) fortify_fail("memmove");
    X(0) = (uint64_t)(uintptr_t)memmove(PTR(0), PTR(1), X(2));
}
static void memset_chk(CPU* c, void* data) {
    (void)data;
    if (X(2) > X(3)) fortify_fail("memset");
    X(0) = (uint64_t)(uintptr_t)memset(PTR(0), (int)X(1), X(2));
}
static void strlen_chk(CPU* c, void* data) {
    (void)data;
    size_t n = strlen(STR(0));
    if (n >= X(1)) fortify_fail("strlen");
    X(0) = n;
}
static void strcpy_chk(CPU* c, void* data) {
    (void)data;
    if (strlen(STR(1)) >= X(2)) fortify_fail("strcpy");
    X(0) = (uint64_t)(uintptr_t)strcpy(PTR(0), STR(1));
}
static void strcat_chk(CPU* c, void* data) {
    (void)data;
    if (strlen(STR(0)) + strlen(STR(1)) >= X(2)) fortify_fail("strcat");
    X(0) = (uint64_t)(uintptr_t)strcat(PTR(0), STR(1));
}
static void strncpy_chk2(CPU* c, void* data) {
    (void)data;
    if (X(2) > X(3)) fortify_fail("strncpy");
    X(0) = (uint64_t)(uintptr_t)strncpy(PTR(0), STR(1), X(2));
}
static void strchr_chk(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)strchr(STR(0), (int)X(1));
}
static void strrchr_chk(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)strrchr(STR(0), (int)X(1));
}
static void read_chk(CPU* c, void* data) {
    (void)data;
    if (X(2) > X(3)) fortify_fail("read");
    X(0) = (uint64_t)read((int)X(0), PTR(1), X(2));
}
static void fd_set_chk(CPU* c, void* data) {
    (void)data;
    if ((int)X(0) < 0 || (int)X(0) >= 1024) fortify_fail("FD_SET");
    ((unsigned long*)PTR(1))[X(0) / 64] |= 1ul << (X(0) % 64);
}
static void fd_isset_chk(CPU* c, void* data) {
    (void)data;
    if ((int)X(0) < 0 || (int)X(0) >= 1024) fortify_fail("FD_ISSET");
    X(0) = (((const unsigned long*)PTR(1))[X(0) / 64] >> (X(0) % 64)) & 1;
}

/* ---- the guest's printf family ---- */

static void format_args(CPU* c, unsigned first, char** text) {
    GuestArgs a;
    args_from_registers(&a, c, first + 1, 0);
    char small[1024];
    int n = guest_format(small, sizeof small, STR(first), &a);
    *text = malloc((size_t)n + 1);
    if (!*text) return;
    if ((size_t)n < sizeof small) {
        memcpy(*text, small, (size_t)n + 1);
        return;
    }
    args_from_registers(&a, c, first + 1, 0);
    guest_format(*text, (size_t)n + 1, STR(first), &a);
}
static void format_va(CPU* c, unsigned first, char** text) {
    GuestArgs a;
    args_from_va_list(&a, X(first + 1));
    char small[1024];
    int n = guest_format(small, sizeof small, STR(first), &a);
    *text = malloc((size_t)n + 1);
    if (!*text) return;
    if ((size_t)n < sizeof small) {
        memcpy(*text, small, (size_t)n + 1);
        return;
    }
    args_from_va_list(&a, X(first + 1));
    guest_format(*text, (size_t)n + 1, STR(first), &a);
}
static void g_snprintf(CPU* c, void* data) {
    (void)data;
    GuestArgs a;
    args_from_registers(&a, c, 3, 0);
    X(0) = (uint64_t)(int64_t)guest_format(PTR(0), X(1), STR(2), &a);
}
static void g_vsnprintf(CPU* c, void* data) {
    (void)data;
    GuestArgs a;
    args_from_va_list(&a, X(3));
    X(0) = (uint64_t)(int64_t)guest_format(PTR(0), X(1), STR(2), &a);
}
/* __vsnprintf_chk(buf, size, flags, slen, fmt, va) */
static void g_vsnprintf_chk(CPU* c, void* data) {
    (void)data;
    if (X(1) > X(3)) fortify_fail("vsnprintf");
    GuestArgs a;
    args_from_va_list(&a, X(5));
    X(0) = (uint64_t)(int64_t)guest_format(PTR(0), X(1), STR(4), &a);
}
static void g_sprintf(CPU* c, void* data) {
    (void)data;
    GuestArgs a;
    args_from_registers(&a, c, 2, 0);
    X(0) = (uint64_t)(int64_t)guest_format(PTR(0), SIZE_MAX, STR(1), &a);
}
static void g_vsprintf(CPU* c, void* data) {
    (void)data;
    GuestArgs a;
    args_from_va_list(&a, X(2));
    X(0) = (uint64_t)(int64_t)guest_format(PTR(0), SIZE_MAX, STR(1), &a);
}
/* __vsprintf_chk(buf, flags, slen, fmt, va) */
static void g_vsprintf_chk(CPU* c, void* data) {
    (void)data;
    GuestArgs a;
    args_from_va_list(&a, X(4));
    int n = guest_format(PTR(0), X(2), STR(3), &a);
    if ((uint64_t)n >= X(2)) fortify_fail("vsprintf");
    X(0) = (uint64_t)(int64_t)n;
}
static void g_vasprintf(CPU* c, void* data) {
    (void)data;
    char* text;
    format_va(c, 1, &text);
    memcpy(PTR(0), &text, sizeof text);
    X(0) = text ? strlen(text) : (uint64_t)-1;
}
static void g_printf(CPU* c, void* data) {
    (void)data;
    char* text;
    format_args(c, 0, &text);
    X(0) = text ? (uint64_t)fputs(text, stdout) : (uint64_t)-1;
    if (text) X(0) = strlen(text);
    free(text);
}
static void g_vprintf(CPU* c, void* data) {
    (void)data;
    char* text;
    format_va(c, 0, &text);
    if (text) fputs(text, stdout);
    X(0) = text ? strlen(text) : (uint64_t)-1;
    free(text);
}
static void g_fprintf(CPU* c, void* data) {
    (void)data;
    char* text;
    format_args(c, 1, &text);
    if (text) fputs(text, hle_file(X(0)));
    X(0) = text ? strlen(text) : (uint64_t)-1;
    free(text);
}
static void g_vfprintf(CPU* c, void* data) {
    (void)data;
    char* text;
    format_va(c, 1, &text);
    if (text) fputs(text, hle_file(X(0)));
    X(0) = text ? strlen(text) : (uint64_t)-1;
    free(text);
}
static void g_syslog(CPU* c, void* data) {
    (void)data;
    char* text;
    format_args(c, 1, &text);
    if (text) fprintf(stderr, "syslog: %s\n", text);
    free(text);
}

/* the scanf family: every argument after the format is a pointer, so the host's takes them as they are */
typedef int (*Scan)(const char*, const char*, ...);
static int scan_pointers(const char* in, const char* format, GuestArgs* a) {
    uint64_t p[24];
    for (int i = 0; i < 24; ++i) p[i] = args_int(a);
    return sscanf(in, format, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13],
                  p[14], p[15], p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23]);
}
static void g_sscanf(CPU* c, void* data) {
    (void)data;
    GuestArgs a;
    args_from_registers(&a, c, 2, 0);
    X(0) = (uint64_t)(int64_t)scan_pointers(STR(0), STR(1), &a);
}
static void g_vsscanf(CPU* c, void* data) {
    (void)data;
    GuestArgs a;
    args_from_va_list(&a, X(2));
    X(0) = (uint64_t)(int64_t)scan_pointers(STR(0), STR(1), &a);
}
static void g_fscanf(CPU* c, void* data) {
    (void)data;
    /* a line at a time: what the engine reads this way (proc files, configs) is line-oriented */
    char line[4096];
    if (!fgets(line, sizeof line, hle_file(X(0)))) {
        X(0) = (uint64_t)(int64_t)EOF;
        return;
    }
    GuestArgs a;
    args_from_registers(&a, c, 2, 0);
    X(0) = (uint64_t)(int64_t)scan_pointers(line, STR(1), &a);
}

/* ---- ctype: bionic's table of the BSD flags, indexed by the character plus one (EOF is -1) ---- */
static unsigned char ctype_table[1 + 256];
enum { CT_U = 0x01, CT_L = 0x02, CT_N = 0x04, CT_S = 0x08, CT_P = 0x10, CT_C = 0x20, CT_X = 0x40, CT_B = 0x80 };
static void init_ctype(void) {
    for (int ch = 0; ch < 128; ++ch) {
        unsigned char f = 0;
        if (ch >= 'A' && ch <= 'Z') f |= CT_U;
        if (ch >= 'a' && ch <= 'z') f |= CT_L;
        if (ch >= '0' && ch <= '9') f |= CT_N;
        if (ch == ' ' || (ch >= '\t' && ch <= '\r')) f |= CT_S;
        if (ch > ' ' && ch < 127 && !(f & (CT_U | CT_L | CT_N))) f |= CT_P;
        if (ch < ' ' || ch == 127) f |= CT_C;
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) f |= CT_X;
        if (ch == ' ' || ch == '\t') f |= CT_B;
        ctype_table[1 + ch] = f;
    }
}
static const unsigned char* ctype_pointer = ctype_table;
static void ctype_get_mb_cur_max(CPU* c, void* data) {
    (void)data;
    X(0) = 4; /* UTF-8, bionic's only multibyte encoding */
}

/* ---- the system properties ---- */
static const struct {
    const char *name, *value;
} properties[] = {
    {"ro.build.version.sdk", "36"},          {"ro.build.version.release", "16"},
    {"ro.product.cpu.abi", "arm64-v8a"},     {"ro.product.manufacturer", "ScarletNX"},
    {"ro.product.model", "ScarletNX"},       {"ro.product.brand", "scarletnx"},
    {"ro.hardware", "scarletnx"},            {"ro.build.type", "user"},
    {"ro.debuggable", "0"},                  {"ro.secure", "1"},
    {"ro.build.tags", "release-keys"},       {"ro.board.platform", "scarletnx"},
    {"ro.kernel.qemu", ""},                  {"ro.boot.qemu", ""},
};
/* __system_property_get(name, value): the value's length, "" (0) for one the device doesn't have */
static void system_property_get(CPU* c, void* data) {
    (void)data;
    char* out = PTR(1);
    out[0] = 0;
    for (size_t i = 0; i < sizeof properties / sizeof *properties; ++i)
        if (!strcmp(properties[i].name, STR(0))) {
            snprintf(out, 92, "%s", properties[i].value); /* PROP_VALUE_MAX */
            break;
        }
    X(0) = strlen(out);
}

/* ---- the device ---- */
static void get_device_api_level(CPU* c, void* data) {
    (void)data;
    X(0) = 36;
}

/* ---- liblog ---- */
static const char* priority(uint64_t p) {
    static const char* const letters[] = {"?", "?", "V", "D", "I", "W", "E", "F", "S"};
    return p < 9 ? letters[p] : "?";
}
static void log_line(uint64_t prio, const char* tag, const char* text) {
    fprintf(stderr, "%s/%s: %s\n", priority(prio), tag ? tag : "", text ? text : "");
}
static void android_log_write(CPU* c, void* data) {
    (void)data;
    log_line(X(0), STR(1), STR(2));
    X(0) = 1;
}
static void android_log_print(CPU* c, void* data) {
    (void)data;
    char text[4096];
    GuestArgs a;
    args_from_registers(&a, c, 3, 0);
    guest_format(text, sizeof text, STR(2), &a);
    log_line(X(0), STR(1), text);
    X(0) = 1;
}
static void android_log_vprint(CPU* c, void* data) {
    (void)data;
    char text[4096];
    GuestArgs a;
    args_from_va_list(&a, X(3));
    guest_format(text, sizeof text, STR(2), &a);
    log_line(X(0), STR(1), text);
    X(0) = 1;
}

/* ---- libandroid: the thermal manager, of a device that is never warm ---- */
static uint64_t thermal_manager;
static void thermal_acquire(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)&thermal_manager;
}
/* the headroom to severe throttling: NaN, as a device that does not forecast it reports */
static void thermal_headroom(CPU* c, void* data) {
    (void)data;
    hle_set_f(c, NAN);
}

/* ---- bionic's forms of functions the host's C library has in other forms ---- */

size_t hle_strlcpy(char* dst, const char* src, size_t size) {
    size_t n = strlen(src);
    if (size) {
        size_t copy = n < size - 1 ? n : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = 0;
    }
    return n;
}
/* POSIX's strerror_r, which returns an error number: glibc's of that form is __xpg_strerror_r */
int hle_strerror_r(int error, char* buffer, size_t size) {
    const char* text = strerror(error);
    size_t n = strlen(text);
    if (!size) return ERANGE;
    size_t copy = n < size - 1 ? n : size - 1;
    memcpy(buffer, text, copy);
    buffer[copy] = 0;
    return copy < n ? ERANGE : 0;
}
/* bionic's basename leaves its argument as it is, as GNU's does, but drops trailing slashes as POSIX's */
char* hle_basename(const char* path) {
    static _Thread_local char out[4096];
    if (!path || !*path) return strcpy(out, ".");
    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/') --end;
    size_t start = end;
    while (start > 0 && path[start - 1] != '/') --start;
    if (end == 1 && path[0] == '/') return strcpy(out, "/");
    size_t n = end - start < sizeof out - 1 ? end - start : sizeof out - 1;
    memcpy(out, path + start, n);
    out[n] = 0;
    return out;
}

/* the host functions that PASS calls: GNU C takes a function's address as a data pointer */
#define P(name)          {#name, hle_pass, (void*)name}
#define PH(name, host)   {#name, hle_pass, (void*)host}
#define T(name, adapter) {#name, adapter, (void*)name}

const HleSymbol hle_libc[] = {
    /* liblog and libandroid */
    {"AThermal_acquireManager", thermal_acquire, NULL},
    {"AThermal_getCurrentThermalStatus", zero, NULL},
    {"AThermal_getThermalHeadroom", thermal_headroom, NULL},
    {"AThermal_releaseManager", nothing, NULL},
    {"__android_log_print", android_log_print, NULL},
    {"__android_log_vprint", android_log_vprint, NULL},
    {"__android_log_write", android_log_write, NULL},
    {"android_get_device_api_level", get_device_api_level, NULL},
    {"android_set_abort_message", set_abort_message, NULL},
    {"__system_property_get", system_property_get, NULL},
    /* the C++ runtime's hooks */
    {"__cxa_atexit", cxa_atexit, NULL},
    {"__cxa_finalize", nothing, NULL},
    {"__cxa_thread_atexit_impl", zero, NULL},
    {"__register_atfork", zero, NULL},
    /* checks and aborts */
    {"__stack_chk_fail", stack_chk_fail, NULL},
    {"__stack_chk_guard", NULL, &stack_chk_guard},
    {"__assert2", assert2, NULL},
    {"abort", stack_chk_fail, NULL},
    {"exit", guest_exit, NULL},
    {"_exit", guest_exit, NULL},
    {"__errno", errno_location, NULL},
    /* fortified */
    {"__memcpy_chk", memcpy_chk, NULL},
    {"__memmove_chk", memmove_chk, NULL},
    {"__memset_chk", memset_chk, NULL},
    {"__strlen_chk", strlen_chk, NULL},
    {"__strcpy_chk", strcpy_chk, NULL},
    {"__strcat_chk", strcat_chk, NULL},
    {"__strncpy_chk2", strncpy_chk2, NULL},
    {"__strchr_chk", strchr_chk, NULL},
    {"__strrchr_chk", strrchr_chk, NULL},
    {"__read_chk", read_chk, NULL},
    {"__FD_SET_chk", fd_set_chk, NULL},
    {"__FD_ISSET_chk", fd_isset_chk, NULL},
    {"__vsnprintf_chk", g_vsnprintf_chk, NULL},
    {"__vsprintf_chk", g_vsprintf_chk, NULL},
    /* printf and scanf */
    {"snprintf", g_snprintf, NULL},
    {"vsnprintf", g_vsnprintf, NULL},
    {"sprintf", g_sprintf, NULL},
    {"vsprintf", g_vsprintf, NULL},
    {"vasprintf", g_vasprintf, NULL},
    {"printf", g_printf, NULL},
    {"vprintf", g_vprintf, NULL},
    {"fprintf", g_fprintf, NULL},
    {"vfprintf", g_vfprintf, NULL},
    {"syslog", g_syslog, NULL},
    {"sscanf", g_sscanf, NULL},
    {"vsscanf", g_vsscanf, NULL},
    {"fscanf", g_fscanf, NULL},
    /* ctype */
    {"_ctype_", NULL, &ctype_pointer},
    {"__ctype_get_mb_cur_max", ctype_get_mb_cur_max, NULL},
    P(isspace), P(towlower), P(iswlower_l),
    /* memory */
    P(malloc), P(calloc), P(realloc), P(free), P(memalign), P(posix_memalign), P(malloc_usable_size),
    /* strings */
    P(memchr), P(memcmp), P(memcpy), P(memmove), P(memset), P(strcasecmp), P(strcat), P(strchr), P(strcmp),
    P(strcoll), P(strcpy), P(strcspn), P(strdup), P(strerror), P(strlen), P(strncasecmp), P(strncat), P(strncmp),
    P(strncpy), P(strpbrk), P(strrchr), P(strsep), P(strspn), P(strstr), P(strtok), P(strtok_r), P(strtol),
    P(strtoll), P(strtoul), P(strtoull), P(atoi), P(atol), P(atoll), P(bsearch),
    PH(strlcpy, hle_strlcpy), PH(strerror_r, hle_strerror_r), PH(basename, hle_basename),
    P(wcslen), P(wmemchr), P(wmemcmp), P(wmemcpy), P(mbrlen), P(mbrtowc), P(mbsnrtowcs), P(mbsrtowcs), P(mbtowc),
    P(wcrtomb), P(wcsnrtombs),
    /* locales: the host's C locale, the only one bionic has */
    P(setlocale), P(newlocale), P(freelocale), P(uselocale), P(localeconv), P(strtoll_l), P(strtoull_l),
    /* math */
    T(acos, d_d), T(asin, d_d), T(atan, d_d), T(cos, d_d), T(cosh, d_d), T(exp, d_d), T(exp2, d_d), T(log, d_d),
    T(log10, d_d), T(log2, d_d), T(sin, d_d), T(sinh, d_d), T(sqrt, d_d), T(tan, d_d), T(tanh, d_d),
    T(atan2, d_dd), T(fmod, d_dd), T(hypot, d_dd), T(pow, d_dd),
    T(acosf, f_f), T(asinf, f_f), T(atanf, f_f), T(cosf, f_f), T(expf, f_f), T(exp2f, f_f), T(logf, f_f),
    T(log10f, f_f), T(log2f, f_f), T(sinf, f_f), T(sqrtf, f_f), T(tanf, f_f),
    T(atan2f, f_ff), T(fmodf, f_ff), T(powf, f_ff), T(remainderf, f_ff),
    T(ldexp, d_di), T(frexp, d_dp), T(modf, d_dp), T(frexpf, f_fp), T(modff, f_fp),
    T(atof, d_p), T(strtod, d_pp), T(strtof, f_pp),
    {"sincos", sincos_d, NULL}, {"sincosf", sincos_f, NULL}, {"difftime", difftime_, NULL},
    P(div), P(rand), P(srand), P(feclearexcept), P(fetestexcept),
    {"", NULL, NULL},
};

void hle_libc_init(void) {
    stack_chk_guard = bridge_stack_guard();
    init_ctype();
}
