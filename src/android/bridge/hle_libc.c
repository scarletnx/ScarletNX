/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's libc and libm as HLE (hle.h). A guest pointer is a host pointer and the LP64 layouts agree, so most of
   libc is the host's own function, called with the guest's registers by one adapter (PASS). The functions whose types
   differ between bionic on AArch64 and the host's C library have their own adapters here or in hle_posix.c */
#define _GNU_SOURCE
#include "android/bridge/args.h"
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/host.h"
#include "common/platform/file.h"
#include "common/utilities/ascii.h"
#include "gnuabi/kernel/kernel.h"
#include <ctype.h>
#include <pthread.h>
#include <fenv.h>
#include <errno.h>
#if defined(__linux__)
#include <malloc.h> /* memalign, malloc_usable_size */
#elif defined(__FreeBSD__)
#include <malloc_np.h> /* malloc_usable_size */
#elif defined(__APPLE__)
#include <malloc/malloc.h> /* malloc_size */
#elif defined(_WIN32)
#include <malloc.h> /* _msize */
#endif
#ifdef _WIN32
#include <io.h> /* read */
#else
#include <unistd.h>
#endif
#include <locale.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <xlocale.h> /* newlocale, uselocale, strtoll_l */
#endif
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
#if defined(_WIN32) && defined(__x86_64__)
/* Windows x64 passes each argument by its position: the first four in RCX, RDX, R8 and R9, or in XMM0 to XMM3 when
   floating, the others on the stack, a slot each. The guest's arguments go to their positions by params (each one's
   class in order: 'I', 'F' or 'D'); without params they are integers alone, count of them. A float is the low half
   of its slot, as on the guest's stack and in an XMM register */
enum { WIN_SLOTS = 14 };
static unsigned win_slots(const CPU* c, const char* params, unsigned count, uint64_t slot[WIN_SLOTS]) {
    const uint64_t* stack = (const uint64_t*)(uintptr_t)c->x[31];
    unsigned xi = 0, vi = 0, si = 0, floating = 0;
    for (unsigned n = 0; n < WIN_SLOTS && (params ? params[n] != 0 : n < count); ++n) {
        char k = params ? params[n] : 'I';
        if (k == 'I') {
            slot[n] = xi < 8 ? c->x[xi++] : stack[si++];
            continue;
        }
        uint64_t bits = 0;
        if (vi < 8 && k == 'F') {
            uint32_t f;
            memcpy(&f, &c->v[vi++].f[0], sizeof f);
            bits = f;
        } else if (vi < 8)
            memcpy(&bits, &c->v[vi++].df[0], sizeof bits);
        else
            bits = stack[si++];
        slot[n] = bits;
        if (n < 4) floating |= 1u << n;
    }
    return floating;
}
static double bits_double(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}
typedef uint64_t U;
#define TY_I uint64_t
#define TY_D double
#define AR_I(i) s[i]
#define AR_D(i) bits_double(s[i])
#define WIN_TYPE(R, a, b, d, e) R (*)(TY_##a, TY_##b, TY_##d, TY_##e, U, U, U, U, U, U, U, U, U, U)
#define WIN_ARGS(a, b, d, e)                                                                                           \
    AR_##a(0), AR_##b(1), AR_##d(2), AR_##e(3), s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13]
#define WIN_CASE(m, a, b, d, e)                                                                                        \
    case m:                                                                                                            \
        if (fp) fr = ((WIN_TYPE(double, a, b, d, e))fn)(WIN_ARGS(a, b, d, e));                                         \
        else ir = ((WIN_TYPE(U, a, b, d, e))fn)(WIN_ARGS(a, b, d, e));                                                 \
        break;
static void win_call(CPU* c, void* fn, char kind, const char* params, unsigned count) {
    uint64_t s[WIN_SLOTS] = {0};
    unsigned floating = win_slots(c, params, count, s);
    bool fp = kind == 'F' || kind == 'D';
    double fr = 0;
    U ir = 0;
    switch (floating) {
        WIN_CASE(0, I, I, I, I) WIN_CASE(1, D, I, I, I) WIN_CASE(2, I, D, I, I) WIN_CASE(3, D, D, I, I)
        WIN_CASE(4, I, I, D, I) WIN_CASE(5, D, I, D, I) WIN_CASE(6, I, D, D, I) WIN_CASE(7, D, D, D, I)
        WIN_CASE(8, I, I, I, D) WIN_CASE(9, D, I, I, D) WIN_CASE(10, I, D, I, D) WIN_CASE(11, D, D, I, D)
        WIN_CASE(12, I, I, D, D) WIN_CASE(13, D, I, D, D) WIN_CASE(14, I, D, D, D) WIN_CASE(15, D, D, D, D)
    }
    if (fp) {
        memset(&c->v[0], 0, sizeof c->v[0]);
        memcpy(&c->v[0].d[0], &fr, sizeof fr); /* a float result is the low half: the double's bits carry it */
        if (kind == 'F') memset(&c->v[0].b[4], 0, 4);
    } else
        X(0) = ir;
}
#undef WIN_CASE
#undef WIN_ARGS
#undef WIN_TYPE
#endif

typedef uint64_t (*CallInt)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, double, double, double, double,
                            double, double, double, double);
typedef double (*CallFloat)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, double, double, double, double,
                            double, double, double, double);
void hle_call(CPU* c, void* fn, char kind, const char* params) {
#if defined(_WIN32) && defined(__x86_64__)
    win_call(c, fn, kind, params, 8);
#else
    (void)params; /* each class has its own registers, in order, on both sides */
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
#endif
}

typedef uint64_t (*CallWide)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, double, double, double, double, double, double, double, double);
void hle_call_wide(CPU* c, void* fn, char kind, const char* params) {
#if defined(_WIN32) && defined(__x86_64__)
    win_call(c, fn, kind, params, 12);
#else
    if (kind == 'F' || kind == 'D') {
        hle_call(c, fn, kind, params);
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
#endif
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
static void d_ddd(CPU* c, void* fn) {
    hle_set_d(c, ((double (*)(double, double, double))fn)(hle_d(c, 0), hle_d(c, 1), hle_d(c, 2)));
}
static void f_fff(CPU* c, void* fn) {
    hle_set_f(c, ((float (*)(float, float, float))fn)(hle_f(c, 0), hle_f(c, 1), hle_f(c, 2)));
}
/* lround and llround: bionic's long is 64 bits as long long is, which Windows' long is not */
static void l_d(CPU* c, void* fn) {
    (void)fn;
    X(0) = (uint64_t)llround(hle_d(c, 0));
}
static void l_f(CPU* c, void* fn) {
    (void)fn;
    X(0) = (uint64_t)llroundf(hle_f(c, 0));
}
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

/* ---- qsort and bsearch: the host's, with a comparator that calls the guest's ---- */

static _Thread_local struct {
    /* the guest comparator of this thread's sort or search: saved and restored, so a comparator can sort too */
    uint64_t compare;
    int error;              /* the guest's errno, where it is not the host's (hle_impl.h) */
    char unknown_error[32]; /* strerror's text of a number bionic has no text for */
    uint64_t locale;        /* the guest's uselocale, 0 before its first: bionic's LC_GLOBAL_LOCALE */
#ifndef _WIN32
    unsigned guest_calls; /* the host calls into guest code under way (hle_locale_enter) */
    locale_t host_locale; /* the thread's locale before the first of them */
#endif
} t_hle_libc;

static int compare_guest(const void* a, const void* b) {
    const uint64_t args[2] = {(uint64_t)(uintptr_t)a, (uint64_t)(uintptr_t)b};
    return (int32_t)bridge_call(t_hle_libc.compare, args, 2);
}
static void g_qsort(CPU* c, void* data) {
    (void)data;
    uint64_t saved = t_hle_libc.compare;
    t_hle_libc.compare = X(3);
    qsort(PTR(0), (size_t)X(1), (size_t)X(2), compare_guest);
    t_hle_libc.compare = saved;
}
static void g_bsearch(CPU* c, void* data) {
    (void)data;
    uint64_t saved = t_hle_libc.compare;
    t_hle_libc.compare = X(4);
    void* found = bsearch(PTR(0), PTR(1), (size_t)X(2), (size_t)X(3), compare_guest);
    t_hle_libc.compare = saved;
    X(0) = (uint64_t)(uintptr_t)found;
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

/* ---- errno: bionic's numbers are Linux's (hle_impl.h) ---- */
#if defined(__linux__)
int* hle_errno_location(void) {
    return &errno;
}
void hle_errno_begin(void) {}
void hle_errno_end(void) {}
void hle_set_errno(int linux_error) {
    errno = linux_error;
}
#else
int* hle_errno_location(void) {
    return &t_hle_libc.error;
}
void hle_errno_begin(void) {
    errno = 0;
}
/* what the call left in the host's errno, as the guest's number. A call that succeeds may leave one, as POSIX
   allows: the guest's errno then changes, as a host call's may */
void hle_errno_end(void) {
    if (errno) t_hle_libc.error = -gnu_host_errno(errno);
    errno = 0;
}
void hle_set_errno(int linux_error) {
    t_hle_libc.error = linux_error;
    errno = 0; /* not the host's, which hle_errno_end would take after it */
}
#endif
static void errno_location(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)hle_errno_location();
}
/* bionic's texts of the errno numbers, Linux's, by number (its libc/private/bionic_errdefs.h, the kernel headers'
   words): the host C library's are other words, and in the user's language */
static const char* const error_texts[] = {
    [0] = "Success",
    [1] = "Operation not permitted",
    [2] = "No such file or directory",
    [3] = "No such process",
    [4] = "Interrupted system call",
    [5] = "I/O error",
    [6] = "No such device or address",
    [7] = "Argument list too long",
    [8] = "Exec format error",
    [9] = "Bad file descriptor",
    [10] = "No child processes",
    [11] = "Try again",
    [12] = "Out of memory",
    [13] = "Permission denied",
    [14] = "Bad address",
    [15] = "Block device required",
    [16] = "Device or resource busy",
    [17] = "File exists",
    [18] = "Cross-device link",
    [19] = "No such device",
    [20] = "Not a directory",
    [21] = "Is a directory",
    [22] = "Invalid argument",
    [23] = "File table overflow",
    [24] = "Too many open files",
    [25] = "Inappropriate ioctl for device",
    [26] = "Text file busy",
    [27] = "File too large",
    [28] = "No space left on device",
    [29] = "Illegal seek",
    [30] = "Read-only file system",
    [31] = "Too many links",
    [32] = "Broken pipe",
    [33] = "Math argument out of domain of func",
    [34] = "Math result not representable",
    [35] = "Resource deadlock would occur",
    [36] = "File name too long",
    [37] = "No record locks available",
    [38] = "Function not implemented",
    [39] = "Directory not empty",
    [40] = "Too many symbolic links encountered",
    [42] = "No message of desired type",
    [43] = "Identifier removed",
    [44] = "Channel number out of range",
    [45] = "Level 2 not synchronized",
    [46] = "Level 3 halted",
    [47] = "Level 3 reset",
    [48] = "Link number out of range",
    [49] = "Protocol driver not attached",
    [50] = "No CSI structure available",
    [51] = "Level 2 halted",
    [52] = "Invalid exchange",
    [53] = "Invalid request descriptor",
    [54] = "Exchange full",
    [55] = "No anode",
    [56] = "Invalid request code",
    [57] = "Invalid slot",
    [59] = "Bad font file format",
    [60] = "Device not a stream",
    [61] = "No data available",
    [62] = "Timer expired",
    [63] = "Out of streams resources",
    [64] = "Machine is not on the network",
    [65] = "Package not installed",
    [66] = "Object is remote",
    [67] = "Link has been severed",
    [68] = "Advertise error",
    [69] = "Srmount error",
    [70] = "Communication error on send",
    [71] = "Protocol error",
    [72] = "Multihop attempted",
    [73] = "RFS specific error",
    [74] = "Not a data message",
    [75] = "Value too large for defined data type",
    [76] = "Name not unique on network",
    [77] = "File descriptor in bad state",
    [78] = "Remote address changed",
    [79] = "Can not access a needed shared library",
    [80] = "Accessing a corrupted shared library",
    [81] = ".lib section in a.out corrupted",
    [82] = "Attempting to link in too many shared libraries",
    [83] = "Cannot exec a shared library directly",
    [84] = "Illegal byte sequence",
    [85] = "Interrupted system call should be restarted",
    [86] = "Streams pipe error",
    [87] = "Too many users",
    [88] = "Socket operation on non-socket",
    [89] = "Destination address required",
    [90] = "Message too long",
    [91] = "Protocol wrong type for socket",
    [92] = "Protocol not available",
    [93] = "Protocol not supported",
    [94] = "Socket type not supported",
    [95] = "Operation not supported on transport endpoint",
    [96] = "Protocol family not supported",
    [97] = "Address family not supported by protocol",
    [98] = "Address already in use",
    [99] = "Cannot assign requested address",
    [100] = "Network is down",
    [101] = "Network is unreachable",
    [102] = "Network dropped connection because of reset",
    [103] = "Software caused connection abort",
    [104] = "Connection reset by peer",
    [105] = "No buffer space available",
    [106] = "Transport endpoint is already connected",
    [107] = "Transport endpoint is not connected",
    [108] = "Cannot send after transport endpoint shutdown",
    [109] = "Too many references: cannot splice",
    [110] = "Connection timed out",
    [111] = "Connection refused",
    [112] = "Host is down",
    [113] = "No route to host",
    [114] = "Operation already in progress",
    [115] = "Operation now in progress",
    [116] = "Stale NFS file handle",
    [117] = "Structure needs cleaning",
    [118] = "Not a XENIX named type file",
    [119] = "No XENIX semaphores available",
    [120] = "Is a named type file",
    [121] = "Remote I/O error",
    [122] = "Quota exceeded",
    [123] = "No medium found",
    [124] = "Wrong medium type",
    [125] = "Operation Canceled",
    [126] = "Required key not available",
    [127] = "Key has expired",
    [128] = "Key has been revoked",
    [129] = "Key was rejected by service",
    [130] = "Owner died",
    [131] = "State not recoverable",
    [132] = "Operation not possible due to RF-kill",
    [133] = "Memory page has hardware error",
};
const char* hle_error_text(int error, char* buffer, size_t size) {
    if (error >= 0 && error < (int)(sizeof error_texts / sizeof *error_texts) && error_texts[error])
        return error_texts[error];
    snprintf(buffer, size, "Unknown error %d", error);
    return buffer;
}
static void g_strerror(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)hle_error_text((int)X(0), t_hle_libc.unknown_error, sizeof t_hle_libc.unknown_error);
}
/* __gnu_strerror_r(error, buffer, size), GNU's strerror_r: the text cut to the buffer, which it returns */
static void gnu_strerror_r(CPU* c, void* data) {
    (void)data;
    hle_strerror_r((int)X(0), PTR(1), (size_t)X(2));
    X(0) = X(1);
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
/* bionic's __check_buffer_access: claim bytes read from or written into a buffer of actual bytes; a failure ends the
   app with the sizes */
static void check_access(const char* fn, const char* action, uint64_t claim, uint64_t actual) {
    if (claim <= actual) return;
    fprintf(stderr, "scarletnx-android: FORTIFY: %s: prevented %llu-byte %s %llu-byte buffer\n", fn,
            (unsigned long long)claim, action, (unsigned long long)actual);
    abort();
}
/* __memchr_chk(s, c, n, actual) and __strncpy_chk(dst, src, len, dst_len) */
static void memchr_chk(CPU* c, void* data) {
    (void)data;
    check_access("memchr", "read from", X(2), X(3));
    X(0) = (uint64_t)(uintptr_t)memchr(PTR(0), (int)X(1), X(2));
}
static void strncpy_chk(CPU* c, void* data) {
    (void)data;
    check_access("strncpy", "write into", X(2), X(3));
    X(0) = (uint64_t)(uintptr_t)strncpy(PTR(0), STR(1), X(2));
}
/* __strncat_chk(dst, src, len, dst_size): strncat that ends the app, as bionic's does, when dst's text and what is
   appended with its NUL would not fit in dst_size */
static void strncat_chk(CPU* c, void* data) {
    (void)data;
    char* dst = PTR(0);
    const char* src = STR(1);
    uint64_t len = X(2), room = X(3);
    if (len) {
        size_t used = strnlen(dst, room);
        if (used >= room) fortify_fail("strlen");
        room -= used;
        size_t take = strnlen(src, len);
        if (take >= room) {
            fprintf(stderr,
                    "scarletnx-android: FORTIFY: strncat: prevented write past end of buffer (%llu bytes and a NUL "
                    "into %llu)\n",
                    (unsigned long long)take, (unsigned long long)room);
            abort();
        }
        strncat(dst, src, len);
    }
    /* x0 still holds dst, which strncat returns */
}
static void strchr_chk(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)strchr(STR(0), (int)X(1));
}
static void strrchr_chk(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)strrchr(STR(0), (int)X(1));
}
/* __strlcpy_chk(dst, src, size, dst_length) */
static void strlcpy_chk(CPU* c, void* data) {
    (void)data;
    if (X(2) > X(3)) fortify_fail("strlcpy");
    X(0) = hle_strlcpy(PTR(0), STR(1), X(2));
}
static void read_chk(CPU* c, void* data) {
    (void)data;
    if (X(2) > X(3)) fortify_fail("read");
    X(0) = (uint64_t)read((int)X(0), PTR(1), X(2));
}
/* __pread_chk(fd, buffer, count, offset, buffer_size) and __pread64_chk: pread, fortified */
static void pread_chk(CPU* c, void* data) {
    (void)data;
    if (X(2) > X(4)) fortify_fail("pread");
    X(0) = (uint64_t)plat_pread_fd((int)X(0), PTR(1), (size_t)X(2), X(3));
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
enum { SCAN_MOST = 24 };
/* the pointers a scanf format stores through: one for each conversion but %% and the suppressed (%*d), %n's too, or
   the highest of their positions (%2$d). Only those are read: the guest's stack can end right after the last one */
static unsigned scan_count(const char* f) {
    unsigned count = 0, highest = 0;
    while (*f) {
        if (*f++ != '%') continue;
        if (*f == '%') {
            ++f;
            continue;
        }
        const char* digits = f;
        unsigned position = 0;
        while (*f >= '0' && *f <= '9') ++f;
        if (*f == '$' && f > digits) {
            position = (unsigned)strtoul(digits, NULL, 10);
            ++f;
        } else
            f = digits;
        bool stores = *f != '*';
        if (!stores) ++f;
        while (*f >= '0' && *f <= '9') ++f;
        while (*f && strchr("hlLjztqm", *f)) ++f;
        if (*f == '[') { /* a set, which can hold ']' first and '%' anywhere */
            ++f;
            if (*f == '^') ++f;
            if (*f == ']') ++f;
            while (*f && *f != ']') ++f;
        }
        if (*f) ++f;
        if (stores && !position) ++count;
        if (stores && position > highest) highest = position;
    }
    return count > highest ? count : highest;
}
static int scan_pointers(const char* in, const char* format, GuestArgs* a) {
    unsigned count = scan_count(format);
    if (count > SCAN_MOST) {
        fprintf(stderr,
                "scarletnx-android: a scanf format stores through %u pointers, the bridge passes %d at most: "
                "it returns -1 (EOF) for \"%s\"\n",
                count, SCAN_MOST, format);
        return EOF;
    }
    uint64_t p[SCAN_MOST] = {0};
    for (unsigned i = 0; i < count; ++i) p[i] = args_int(a);
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

/* ctype's functions of a locale: bionic's C locale (the only one it has) for each, ASCII's classes, whatever the
   locale_t */
#define CTYPE_L(F) F(isalnum, snx_is_alnum) F(isalpha, snx_is_alpha) F(isblank, snx_is_blank) F(isdigit, snx_is_digit) \
    F(islower, snx_is_lower) F(isspace, snx_is_space) F(isupper, snx_is_upper) F(isxdigit, snx_is_hex)              \
    F(tolower, snx_to_lower) F(toupper, snx_to_upper)
#define CTYPE_L_ADAPTER(name, ascii)                                                                                   \
    static void g_##name##_l(CPU* c, void* data) {                                                                     \
        (void)data;                                                                                                    \
        int ch = (int)X(0);                                                                                            \
        X(0) = (uint64_t)(int64_t)(ch >= 0 && ch < 128 ? ascii(ch) : (#name[0] == 't' ? ch : 0));                      \
    }
CTYPE_L(CTYPE_L_ADAPTER)
#define CTYPE_L_ENTRY(name, ascii) {#name "_l", g_##name##_l, NULL},

/* ---- bionic's locales: the C locale alone, its multibyte characters UTF-8 whatever the locale's name. A name only
   sets MB_CUR_MAX, 4 for UTF-8's and 1 for the others, and a locale_t is bionic's struct of it. The host's locale, the
   user's, is neither read nor set ---- */

/* bionic's LC_GLOBAL_LOCALE, and its LC_ALL_MASK: each category's bit, LC_CTYPE (0) to LC_IDENTIFICATION (12), but
   LC_ALL's (6) */
#define BIONIC_GLOBAL_LOCALE ((uint64_t)-1)
enum { BIONIC_LC_ALL = 6, BIONIC_LC_LAST = 12, BIONIC_LC_ALL_MASK = 0x1fbf };
typedef struct {
    uint64_t mb_cur_max; /* bionic's struct __locale_t: a size_t */
} BionicLocale;
static struct {
    int ascii; /* setlocale's last name was not UTF-8's: MB_CUR_MAX 1. bionic starts in C.UTF-8 */
#ifndef _WIN32
    /* the host's C locale that the guest's calls run in (hle_locale_enter), made once */
    pthread_once_t once;
    locale_t c;
#endif
} g_locale = {
    .ascii = 0,
#ifndef _WIN32
    .once = PTHREAD_ONCE_INIT,
#endif
};

static bool locale_known(const char* name) {
    return !strcmp(name, "") || !strcmp(name, "C") || !strcmp(name, "C.UTF-8") || !strcmp(name, "en_US.UTF-8") ||
           !strcmp(name, "POSIX");
}
static bool locale_utf8(const char* name) { return !name[0] || strstr(name, "UTF-8"); }
static uint64_t mb_cur_max_of(uint64_t locale) {
    if (locale == BIONIC_GLOBAL_LOCALE) return __atomic_load_n(&g_locale.ascii, __ATOMIC_RELAXED) ? 1 : 4;
    return ((const BionicLocale*)(uintptr_t)locale)->mb_cur_max;
}
static uint64_t current_locale(void) { return t_hle_libc.locale ? t_hle_libc.locale : BIONIC_GLOBAL_LOCALE; }
static uint64_t new_locale(uint64_t mb_cur_max) {
    BionicLocale* l = malloc(sizeof *l);
    if (!l) {
        hle_set_errno(GNU_ENOMEM);
        return 0;
    }
    l->mb_cur_max = mb_cur_max;
    return (uint64_t)(uintptr_t)l;
}
/* setlocale(category, name): "C.UTF-8" or "C"; NULL and ENOENT for a locale bionic lacks */
static void g_setlocale(CPU* c, void* data) {
    (void)data;
    int category = (int)X(0);
    if (category < 0 || category > BIONIC_LC_LAST) {
        hle_set_errno(GNU_EINVAL);
        X(0) = 0;
        return;
    }
    if (X(1) && !locale_known(STR(1))) {
        hle_set_errno(GNU_ENOENT);
        X(0) = 0;
        return;
    }
    if (X(1)) __atomic_store_n(&g_locale.ascii, !locale_utf8(STR(1)), __ATOMIC_RELAXED);
    X(0) = (uint64_t)(uintptr_t)(__atomic_load_n(&g_locale.ascii, __ATOMIC_RELAXED) ? "C" : "C.UTF-8");
}
/* newlocale(mask, name, base): base is left as it is, as bionic's leaves it */
static void g_newlocale(CPU* c, void* data) {
    (void)data;
    if (((int)X(0) & ~BIONIC_LC_ALL_MASK) || !X(1)) {
        hle_set_errno(GNU_EINVAL);
        X(0) = 0;
        return;
    }
    if (!locale_known(STR(1))) {
        hle_set_errno(GNU_ENOENT);
        X(0) = 0;
        return;
    }
    X(0) = new_locale(locale_utf8(STR(1)) ? 4 : 1);
}
static void g_duplocale(CPU* c, void* data) {
    (void)data;
    X(0) = new_locale(mb_cur_max_of(X(0)));
}
static void g_freelocale(CPU* c, void* data) {
    (void)data;
    if (X(0) != BIONIC_GLOBAL_LOCALE) free(PTR(0));
}
/* uselocale(locale): the thread's locale before, LC_GLOBAL_LOCALE before the first; 0 only asks */
static void g_uselocale(CPU* c, void* data) {
    (void)data;
    uint64_t old = current_locale();
    if (X(0)) t_hle_libc.locale = X(0);
    X(0) = old;
}
static void ctype_get_mb_cur_max(CPU* c, void* data) {
    (void)data;
    X(0) = mb_cur_max_of(current_locale());
}

/* The host's C library takes the user's locale (the JVM sets the process's from the environment), with a decimal
   comma, other names of days and months, and other words. The guest's calls into it run in the C locale: the thread
   takes it from the first host call into guest code until that one returns. On Linux it is C.UTF-8, whose multibyte
   functions are the guest's (PASS). Windows' C runtime keeps the C locale, as no part of the process sets another */
#ifndef _WIN32
static void make_guest_locale(void) {
#if defined(__linux__)
    g_locale.c = newlocale(LC_ALL_MASK, "C.UTF-8", (locale_t)0);
    if (!g_locale.c)
        fprintf(stderr, "scarletnx-android: the host has no C.UTF-8 locale: the guest's multibyte functions take the "
                        "C locale's ASCII alone\n");
#endif
    if (!g_locale.c) g_locale.c = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    if (!g_locale.c)
        fprintf(stderr, "scarletnx-android: no C locale (newlocale: %s): the guest's C library takes the user's\n",
                strerror(errno));
}
#endif
void hle_locale_enter(void) {
#ifndef _WIN32
    if (t_hle_libc.guest_calls++) return;
    pthread_once(&g_locale.once, make_guest_locale);
    t_hle_libc.host_locale = uselocale(g_locale.c);
#endif
}
void hle_locale_leave(void) {
#ifndef _WIN32
    if (--t_hle_libc.guest_calls) return;
    uselocale(t_hle_libc.host_locale);
#endif
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
/* POSIX's strerror_r, which returns an error number (Linux's) */
int hle_strerror_r(int error, char* buffer, size_t size) {
    char unknown[32];
    return hle_strlcpy(buffer, hle_error_text(error, unknown, sizeof unknown), size) >= size ? GNU_ERANGE : 0;
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

/* strsep and strtok_r as bionic's, which every host's C library does not have (Windows) */
static char* hle_strsep(char** string, const char* delimiters) {
    char* start = *string;
    if (!start) return NULL;
    char* end = start + strcspn(start, delimiters);
    *string = *end ? end + 1 : NULL;
    *end = 0;
    return start;
}
static char* hle_strtok_r(char* string, const char* delimiters, char** saved) {
    char* s = string ? string : *saved;
    if (!s) return NULL;
    s += strspn(s, delimiters);
    if (!*s) {
        *saved = s;
        return NULL;
    }
    char* end = s + strcspn(s, delimiters);
    if (*end) *end++ = 0;
    *saved = end;
    return s;
}

#if !defined(__linux__)
/* ---- bionic's wide characters: a wchar_t of 32 bits (Windows' is 16), and multibyte strings in UTF-8, bionic's only
   encoding, in every locale (the hosts' C locales are ASCII). A conversion keeps no state between calls: a sequence
   that a call cuts is incomplete to it ---- */

/* one character of UTF-8 from s (n bytes at most): its length, 0 for NUL, -2 incomplete, -1 invalid */
static int64_t utf8_decode(const unsigned char* s, size_t n, uint32_t* out) {
    if (!n) return -2;
    uint32_t c = s[0];
    size_t length = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
    if (!length) return -1;
    if (length > 1) c &= 0x3f >> (length - 1);
    for (size_t i = 1; i < length; ++i) {
        if (i >= n) return -2;
        if ((s[i] & 0xc0) != 0x80) return -1;
        c = c << 6 | (s[i] & 0x3fu);
    }
    static const uint32_t least[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (c < least[length] || c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) return -1;
    *out = c;
    return c ? (int64_t)length : 0;
}
/* c as UTF-8 into out (4 bytes): its length, 0 for one that is no character */
static size_t utf8_encode(uint32_t c, unsigned char* out) {
    if (c < 0x80) {
        out[0] = (unsigned char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (unsigned char)(0xc0 | c >> 6);
        out[1] = (unsigned char)(0x80 | (c & 0x3f));
        return 2;
    }
    if (c >= 0xd800 && c < 0xe000) return 0;
    if (c < 0x10000) {
        out[0] = (unsigned char)(0xe0 | c >> 12);
        out[1] = (unsigned char)(0x80 | (c >> 6 & 0x3f));
        out[2] = (unsigned char)(0x80 | (c & 0x3f));
        return 3;
    }
    if (c > 0x10ffff) return 0;
    out[0] = (unsigned char)(0xf0 | c >> 18);
    out[1] = (unsigned char)(0x80 | (c >> 12 & 0x3f));
    out[2] = (unsigned char)(0x80 | (c >> 6 & 0x3f));
    out[3] = (unsigned char)(0x80 | (c & 0x3f));
    return 4;
}
static uint32_t wide_at(uint64_t address, size_t i) {
    uint32_t c;
    memcpy(&c, (const char*)(uintptr_t)address + 4 * i, sizeof c);
    return c;
}
static void wide_put(uint64_t address, size_t i, uint32_t c) {
    memcpy((char*)(uintptr_t)address + 4 * i, &c, sizeof c);
}

static void g_wcslen(CPU* c, void* data) {
    (void)data;
    size_t n = 0;
    while (wide_at(X(0), n)) ++n;
    X(0) = n;
}
static void g_wmemchr(CPU* c, void* data) {
    (void)data;
    uint64_t found = 0;
    for (size_t i = 0; i < X(2) && !found; ++i)
        if (wide_at(X(0), i) == (uint32_t)X(1)) found = X(0) + 4 * i;
    X(0) = found;
}
static void g_wmemcmp(CPU* c, void* data) {
    (void)data;
    int r = 0;
    for (size_t i = 0; i < X(2) && !r; ++i) {
        int32_t a = (int32_t)wide_at(X(0), i), b = (int32_t)wide_at(X(1), i); /* bionic's wchar_t is signed */
        r = a < b ? -1 : a > b;
    }
    X(0) = (uint64_t)(int64_t)r;
}
static void g_wmemcpy(CPU* c, void* data) {
    (void)data;
    memcpy(PTR(0), PTR(1), X(2) * 4);
}
/* mbrtowc(pwc, s, n, ps), mbrlen(s, n, ps) and mbtowc(pwc, s, n) */
static uint64_t decode_one(CPU* c, uint64_t to, const char* s, size_t n, bool restartable) {
    if (!s) return 0;
    uint32_t w = 0;
    int64_t r = utf8_decode((const unsigned char*)s, n, &w);
    if (r == -1 || (r == -2 && !restartable)) {
        hle_set_errno(GNU_EILSEQ);
        return (uint64_t)-1;
    }
    if (r == -2) return (uint64_t)-2;
    if (to) wide_put(to, 0, w);
    (void)c;
    return (uint64_t)r;
}
static void g_mbrtowc(CPU* c, void* data) {
    (void)data;
    X(0) = decode_one(c, X(0), STR(1), X(2), true);
}
static void g_mbrlen(CPU* c, void* data) {
    (void)data;
    X(0) = decode_one(c, 0, STR(0), X(1), true);
}
static void g_mbtowc(CPU* c, void* data) {
    (void)data;
    X(0) = decode_one(c, X(0), STR(1), X(2), false);
}
/* mbsnrtowcs(dst, src, nms, len, ps): at most nms bytes of *src into at most len characters at dst (none: counted) */
static uint64_t decode_string(uint64_t dst, uint64_t src_at, size_t bytes, size_t most) {
    const char* s;
    memcpy(&s, (const void*)(uintptr_t)src_at, sizeof s);
    size_t count = 0, at = 0;
    while (!dst || count < most) {
        uint32_t w = 0;
        int64_t r = utf8_decode((const unsigned char*)s + at, bytes - at, &w);
        if (r == -1) {
            hle_set_errno(GNU_EILSEQ);
            if (dst) {
                const char* left = s + at;
                memcpy((void*)(uintptr_t)src_at, &left, sizeof left);
            }
            return (uint64_t)-1;
        }
        if (r == -2) break; /* the bytes end within a character, or at nms */
        if (dst) wide_put(dst, count, w);
        if (!r) {
            if (dst) {
                const char* none = NULL;
                memcpy((void*)(uintptr_t)src_at, &none, sizeof none);
            }
            return count;
        }
        at += (size_t)r;
        ++count;
    }
    if (dst) {
        const char* left = s + at;
        memcpy((void*)(uintptr_t)src_at, &left, sizeof left);
    }
    return count;
}
static void g_mbsnrtowcs(CPU* c, void* data) {
    (void)data;
    X(0) = decode_string(X(0), X(1), X(2), X(3));
}
static void g_mbsrtowcs(CPU* c, void* data) {
    (void)data;
    X(0) = decode_string(X(0), X(1), SIZE_MAX, X(2));
}
/* mbstowcs(dst, src, n): mbsrtowcs of a pointer of its own */
static void g_mbstowcs(CPU* c, void* data) {
    (void)data;
    uint64_t src = X(1);
    X(0) = decode_string(X(0), (uint64_t)(uintptr_t)&src, SIZE_MAX, X(2));
}
static void g_wcrtomb(CPU* c, void* data) {
    (void)data;
    unsigned char out[4];
    if (!X(0)) {
        X(0) = 1;
        return;
    }
    size_t n = utf8_encode((uint32_t)X(1), out);
    if (!n) {
        hle_set_errno(GNU_EILSEQ);
        X(0) = (uint64_t)-1;
        return;
    }
    memcpy(PTR(0), out, n);
    X(0) = n;
}
/* wcsnrtombs(dst, src, nwc, len, ps): at most nwc characters of *src into at most len bytes at dst */
static void g_wcsnrtombs(CPU* c, void* data) {
    (void)data;
    uint64_t dst = X(0), src_at = X(1);
    size_t most_chars = X(2), most_bytes = X(3), bytes = 0, i = 0;
    uint64_t s;
    memcpy(&s, (const void*)(uintptr_t)src_at, sizeof s);
    for (; i < most_chars; ++i) {
        uint32_t w = wide_at(s, i);
        unsigned char out[4];
        size_t n = utf8_encode(w, out);
        if (!n) {
            hle_set_errno(GNU_EILSEQ);
            X(0) = (uint64_t)-1;
            return;
        }
        if (dst && bytes + n > most_bytes) break;
        if (dst) memcpy((char*)(uintptr_t)dst + bytes, out, n);
        if (!w) {
            if (dst) memset((void*)(uintptr_t)src_at, 0, sizeof s);
            X(0) = bytes;
            return;
        }
        bytes += n;
    }
    if (dst) {
        uint64_t left = s + 4 * i;
        memcpy((void*)(uintptr_t)src_at, &left, sizeof left);
    }
    X(0) = bytes;
}
/* wcstombs(dst, src, n): wcsnrtombs of every character of a pointer of its own, at most n bytes */
static void g_wcstombs(CPU* c, void* data) {
    uint64_t src = X(1);
    X(3) = X(2);
    X(2) = SIZE_MAX;
    X(1) = (uint64_t)(uintptr_t)&src;
    g_wcsnrtombs(c, data);
}
/* the case of ASCII and Latin-1, as the C locale of bionic's has it */
static void g_towlower(CPU* c, void* data) {
    (void)data;
    uint32_t w = (uint32_t)X(0);
    X(0) = (w >= 'A' && w <= 'Z') || (w >= 0xc0 && w <= 0xde && w != 0xd7) ? w + 32 : w;
}
static void g_iswlower_l(CPU* c, void* data) {
    (void)data;
    uint32_t w = (uint32_t)X(0);
    X(0) = (w >= 'a' && w <= 'z') || (w >= 0xdf && w <= 0xff && w != 0xf7);
}
#define WIDE_FUNCTIONS(F)                                                                                              \
    F(wcslen)                                                                                                          \
    F(wmemchr) F(wmemcmp) F(wmemcpy) F(mbrlen) F(mbrtowc) F(mbsnrtowcs) F(mbsrtowcs) F(mbtowc) F(wcrtomb)              \
        F(wcsnrtombs) F(towlower) F(iswlower_l) F(mbstowcs) F(wcstombs)
#define WIDE_ENTRY(name) {#name, g_##name, NULL},
#endif

#if !defined(__linux__) && !defined(_WIN32)
/* ---- FreeBSD and macOS: memalign through posix_memalign, whose blocks free() takes, and malloc's size of a block
   (macOS calls it malloc_size) ---- */
static void g_memalign(CPU* c, void* data) {
    (void)data;
    void* p = NULL;
    size_t align = X(0) < sizeof(void*) ? sizeof(void*) : (size_t)X(0);
    X(0) = posix_memalign(&p, align, (size_t)X(1)) ? 0 : (uint64_t)(uintptr_t)p;
}
static void g_malloc_usable_size(CPU* c, void* data) {
    (void)data;
#if defined(__APPLE__)
    X(0) = X(0) ? malloc_size(PTR(0)) : 0;
#else
    X(0) = malloc_usable_size(PTR(0));
#endif
}
#endif

#ifdef _WIN32
/* ---- Windows: what its C runtime has in another form ---- */

/* The CRT's free() takes malloc's blocks alone, and malloc aligns to 16. A block aligned past that is malloc'd
   larger, and its address kept with the block's in a table that free(), realloc() and malloc_usable_size() look in
   first */
static struct {
    pthread_mutex_t lock;
    struct {
        void *aligned, *block;
        size_t size;
    } * items;
    size_t count, capacity;
} g_win = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0};

/* the table's entry of p, removed: its block, or NULL for a block malloc gave */
static void* aligned_take(void* p, size_t* size) {
    void* block = NULL;
    pthread_mutex_lock(&g_win.lock);
    for (size_t i = 0; i < g_win.count && !block; ++i)
        if (g_win.items[i].aligned == p) {
            block = g_win.items[i].block;
            if (size) *size = g_win.items[i].size;
            g_win.items[i] = g_win.items[--g_win.count];
        }
    pthread_mutex_unlock(&g_win.lock);
    return block;
}
static void* aligned_alloc_(size_t align, size_t size) {
    if (align <= 16) return malloc(size ? size : 1);
    if (align & (align - 1) || size > SIZE_MAX - align) return NULL;
    void* block = malloc(size + align);
    if (!block) return NULL;
    void* p = (void*)(((uintptr_t)block + align - 1) & ~(uintptr_t)(align - 1));
    pthread_mutex_lock(&g_win.lock);
    bool kept = false;
    if (g_win.count == g_win.capacity) {
        size_t capacity = g_win.capacity ? g_win.capacity * 2 : 64;
        void* grown = realloc(g_win.items, capacity * sizeof *g_win.items);
        if (grown) {
            g_win.items = grown;
            g_win.capacity = capacity;
        }
    }
    if (g_win.count < g_win.capacity) {
        g_win.items[g_win.count].aligned = p;
        g_win.items[g_win.count].block = block;
        g_win.items[g_win.count++].size = size;
        kept = true;
    }
    pthread_mutex_unlock(&g_win.lock);
    if (!kept) {
        free(block);
        return NULL;
    }
    return p;
}
static void g_free(CPU* c, void* data) {
    (void)data;
    void* block = X(0) ? aligned_take(PTR(0), NULL) : NULL;
    free(block ? block : PTR(0));
}
static void g_realloc(CPU* c, void* data) {
    (void)data;
    size_t size = 0;
    void* block = X(0) ? aligned_take(PTR(0), &size) : NULL;
    if (!block) {
        X(0) = (uint64_t)(uintptr_t)realloc(PTR(0), X(1));
        return;
    }
    /* an aligned block grows as malloc's: realloc keeps no alignment past malloc's */
    void* p = malloc(X(1) ? X(1) : 1);
    if (p) {
        memcpy(p, PTR(0), size < X(1) ? size : X(1));
        free(block);
    }
    X(0) = (uint64_t)(uintptr_t)p;
}
static void g_memalign(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)aligned_alloc_((size_t)X(0), (size_t)X(1));
}
static void g_posix_memalign(CPU* c, void* data) {
    (void)data;
    if (X(1) < sizeof(void*) || (X(1) & (X(1) - 1))) {
        X(0) = GNU_EINVAL;
        return;
    }
    void* p = aligned_alloc_((size_t)X(1), (size_t)X(2));
    if (p) memcpy(PTR(0), &p, sizeof p);
    X(0) = p ? 0 : GNU_ENOMEM;
}
static void g_malloc_usable_size(CPU* c, void* data) {
    (void)data;
    size_t size = 0;
    bool aligned = false;
    pthread_mutex_lock(&g_win.lock);
    for (size_t i = 0; i < g_win.count && !aligned; ++i)
        if (g_win.items[i].aligned == PTR(0)) {
            size = g_win.items[i].size;
            aligned = true;
        }
    pthread_mutex_unlock(&g_win.lock);
    X(0) = aligned ? size : X(0) ? _msize(PTR(0)) : 0;
}
/* bionic's rand() is random(): 31 bits, where Windows' RAND_MAX is 32767 */
#endif

/* ---- rand and random: bionic's sequence on every host ----
   bionic's rand() is random(), the BSD additive feedback generator: 31 words of state, the feedback 3 words apart,
   seeded by the Park-Miller recurrence (16807 x mod 2^31 - 1) and run 310 times. glibc's rand() is the same, but
   FreeBSD's and macOS's are other generators, and a game that seeds it with a constant to make a key (a decryption's
   table) needs this one's numbers. A seed of 0 is 1, as glibc's (the game's numbers were made on it) */
static struct {
    pthread_mutex_t lock;
    uint32_t state[31];
    int front, rear;
    bool seeded;
} g_random_state = {.lock = PTHREAD_MUTEX_INITIALIZER};

static int32_t park_miller(int32_t x) {
    int32_t hi = x / 127773, lo = x % 127773;
    x = 16807 * lo - 2836 * hi; /* Schrage's: no product passes 2^31 */
    return x < 0 ? x + 0x7fffffff : x;
}
static uint32_t next_random(void) {
    uint32_t* s = g_random_state.state;
    s[g_random_state.front] += s[g_random_state.rear];
    uint32_t v = (s[g_random_state.front] >> 1) & 0x7fffffff;
    if (++g_random_state.front == 31) g_random_state.front = 0;
    if (++g_random_state.rear == 31) g_random_state.rear = 0;
    return v;
}
static void seed_random(uint32_t seed) {
    uint32_t* s = g_random_state.state;
    s[0] = seed ? seed : 1;
    for (int i = 1; i < 31; ++i) s[i] = (uint32_t)park_miller((int32_t)s[i - 1]);
    g_random_state.front = 3;
    g_random_state.rear = 0;
    for (int i = 0; i < 310; ++i) next_random();
    g_random_state.seeded = true;
}
static void g_random(CPU* c, void* data) {
    (void)data;
    pthread_mutex_lock(&g_random_state.lock);
    if (!g_random_state.seeded) seed_random(1); /* as srand(1), before any call */
    X(0) = next_random();
    pthread_mutex_unlock(&g_random_state.lock);
}
static void g_srandom(CPU* c, void* data) {
    (void)data;
    pthread_mutex_lock(&g_random_state.lock);
    seed_random((uint32_t)X(0));
    pthread_mutex_unlock(&g_random_state.lock);
}

/* the bridge's own forms of bionic's functions (above), each called with its own type: hle_pass calls through a type
   of eight integers, which only the host C library's functions, built without -fsanitize=function, can take */
static void g_strlcpy(CPU* c, void* data) {
    (void)data;
    X(0) = hle_strlcpy(PTR(0), STR(1), (size_t)X(2));
}
static void g_strerror_r(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)hle_strerror_r((int)X(0), PTR(1), (size_t)X(2));
}
static void g_basename(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)hle_basename(STR(0));
}
/* strndup(s, n): the first n bytes of s at most, terminated, in malloc's memory (Windows' CRT has no strndup) */
static void g_strndup(CPU* c, void* data) {
    (void)data;
    size_t n = strnlen(STR(0), (size_t)X(1));
    char* copy = malloc(n + 1);
    if (copy) {
        memcpy(copy, STR(0), n);
        copy[n] = 0;
    }
    X(0) = (uint64_t)(uintptr_t)copy;
}
static void g_strsep(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)hle_strsep(PTR(0), STR(1));
}
static void g_strtok_r(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)hle_strtok_r(PTR(0), STR(1), PTR(2));
}

/* the host functions that PASS calls: GNU C takes a function's address as a data pointer */
#define P(name)          {#name, hle_pass, (void*)name}
#define PH(name, host)   {#name, hle_pass, (void*)host}
#define T(name, adapter) {#name, adapter, (void*)name}

/* reallocarray(p, count, size): realloc of count times size, ENOMEM for a product past SIZE_MAX */
static void g_reallocarray(CPU* c, void* data) {
    if (X(2) && X(1) > SIZE_MAX / X(2)) {
        hle_set_errno(12); /* ENOMEM */
        X(0) = 0;
        return;
    }
    X(1) *= X(2);
#ifdef _WIN32
    g_realloc(c, data);
#else
    (void)data;
    X(0) = (uint64_t)(uintptr_t)realloc(PTR(0), X(1));
#endif
}

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
    {"qsort", g_qsort, NULL},
    {"bsearch", g_bsearch, NULL},
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
    {"__strlcpy_chk", strlcpy_chk, NULL},
    {"__strcat_chk", strcat_chk, NULL},
    {"__strncpy_chk2", strncpy_chk2, NULL},
    {"__strncpy_chk", strncpy_chk, NULL},
    {"__memchr_chk", memchr_chk, NULL},
    {"__strncat_chk", strncat_chk, NULL},
    {"__strchr_chk", strchr_chk, NULL},
    {"__strrchr_chk", strrchr_chk, NULL},
    {"__read_chk", read_chk, NULL},
    {"__pread_chk", pread_chk, NULL},
    {"__pread64_chk", pread_chk, NULL},
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
    P(isspace), CTYPE_L(CTYPE_L_ENTRY)
    /* memory */
    {"reallocarray", g_reallocarray, NULL},
#if defined(__linux__)
    P(malloc), P(calloc), P(realloc), P(free), P(memalign), P(posix_memalign), P(malloc_usable_size),
#elif defined(_WIN32)
    P(malloc), P(calloc), {"realloc", g_realloc, NULL}, {"free", g_free, NULL}, {"memalign", g_memalign, NULL},
    {"posix_memalign", g_posix_memalign, NULL}, {"malloc_usable_size", g_malloc_usable_size, NULL},
#else
    P(malloc), P(calloc), P(realloc), P(free), P(posix_memalign), {"memalign", g_memalign, NULL},
    {"malloc_usable_size", g_malloc_usable_size, NULL},
#endif
    /* strings */
    P(memchr), P(memcmp), P(memcpy), P(memmove), P(memset), P(strcasecmp), P(strcat), P(strchr), P(strcmp),
    P(strcoll), P(strcpy), P(strcspn), P(strdup), P(strlen), P(strnlen), {"strerror", g_strerror, NULL}, P(strncasecmp), P(strncat), P(strncmp),
    {"strerror_l", g_strerror, NULL}, {"__gnu_strerror_r", gnu_strerror_r, NULL},
    P(strncpy), P(strpbrk), P(strrchr), P(strspn), P(strstr), P(strtok),
    P(strtoll), P(strtoull), P(atoi), P(atoll),
    {"strlcpy", g_strlcpy, NULL}, {"strerror_r", g_strerror_r, NULL}, {"basename", g_basename, NULL},
    {"strsep", g_strsep, NULL}, {"strtok_r", g_strtok_r, NULL}, {"strndup", g_strndup, NULL},
#ifdef _WIN32
    /* long is 32 bits on Windows, 64 in bionic */
    PH(strtol, strtoll), PH(strtoul, strtoull), PH(atol, atoll), PH(strtoll_l, strtoll), PH(strtoull_l, strtoull),
#else
    P(strtol), P(strtoul), P(atol), PH(strtoll_l, strtoll), PH(strtoull_l, strtoull), /* a locale_t is bionic's */
#endif
#if defined(__linux__)
    P(wcslen), P(wmemchr), P(wmemcmp), P(wmemcpy), P(mbrlen), P(mbrtowc), P(mbsnrtowcs), P(mbsrtowcs), P(mbtowc),
    P(wcrtomb), P(wcsnrtombs), P(towlower), PH(iswlower_l, iswlower), P(mbstowcs), P(wcstombs),
#else
    WIDE_FUNCTIONS(WIDE_ENTRY)
#endif
    /* locales: bionic's, the C locale alone */
    {"setlocale", g_setlocale, NULL}, {"newlocale", g_newlocale, NULL}, {"duplocale", g_duplocale, NULL},
    {"freelocale", g_freelocale, NULL}, {"uselocale", g_uselocale, NULL}, P(localeconv),
    /* math */
    T(acos, d_d), T(asin, d_d), T(atan, d_d), T(cos, d_d), T(cosh, d_d), T(exp, d_d), T(exp2, d_d), T(log, d_d),
    T(log10, d_d), T(log2, d_d), T(sin, d_d), T(sinh, d_d), T(sqrt, d_d), T(tan, d_d), T(tanh, d_d),
    T(cbrt, d_d), T(expm1, d_d), T(log1p, d_d), T(cbrtf, f_f), T(expm1f, f_f), T(log1pf, f_f),
    T(atan2, d_dd), T(fmod, d_dd), T(hypot, d_dd), T(pow, d_dd), T(nextafter, d_dd), T(fmax, d_dd), T(fmin, d_dd),
    T(fma, d_ddd), T(fmaf, f_fff), T(lround, l_d), T(llround, l_d), T(lroundf, l_f), T(llroundf, l_f),
    T(acosf, f_f), T(asinf, f_f), T(atanf, f_f), T(cosf, f_f), T(expf, f_f), T(exp2f, f_f), T(logf, f_f),
    T(log10f, f_f), T(log2f, f_f), T(sinf, f_f), T(sqrtf, f_f), T(tanf, f_f),
    T(atan2f, f_ff), T(fmodf, f_ff), T(powf, f_ff), T(remainderf, f_ff), T(nextafterf, f_ff), T(fmaxf, f_ff),
    T(fminf, f_ff),
    T(ldexp, d_di), T(frexp, d_dp), T(modf, d_dp), T(frexpf, f_fp), T(modff, f_fp),
    T(atof, d_p), T(strtod, d_pp), T(strtof, f_pp),
    {"sincos", sincos_d, NULL}, {"sincosf", sincos_f, NULL}, {"difftime", difftime_, NULL},
    {"rand", g_random, NULL}, {"random", g_random, NULL}, {"srand", g_srandom, NULL}, {"srandom", g_srandom, NULL},
    P(div), P(feclearexcept), P(fetestexcept),
    {"", NULL, NULL},
};

void hle_libc_init(void) {
    stack_chk_guard = bridge_stack_guard();
    init_ctype();
}
