/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The C library's functions a game imports from its SDK (hooks.hook's libc_str family), run by the host's C library on
   guest memory: the string and memory functions, the character classes, the conversions of numbers, abs and div, the
   floating point helpers (__fpclassifyf with musl's numbers, the SDK's C library being musl's), the byte order of the
   network, the pseudo-random numbers, qsort and bsearch. A function
   that returns a pointer into its argument returns the guest address of the same byte; one that sets errno sets the
   guest's, which __errno_location, the SDK's own, finds for the calling thread.

   The character classes and conversions are the C locale's, which the SDK's libc is in and the host is too (the
   runtime never calls setlocale). rand, srand and rand_r are musl's generators, which the SDK's are, with a state of
   their own apart from the runtime's; srand48 and lrand48 stay the SDK's code, as drand48 and the rest share their
   seed. qsort and bsearch call the game's comparison function for each comparison the host's makes
   (hook_guest_call). The family is off on Windows, whose C library lacks half of these and has another RAND_MAX */
#define _GNU_SOURCE /* glibc's strcasestr */
#include "nx/hle/hook/hook.h"
#include "common/utilities/bits.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/kernel/kernel.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#if defined(_WIN32)
/* what the handlers call, for the table to link: the family is off on Windows (hook.c's family_unready) */
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#define ffs         __builtin_ffs
#define ffsl        __builtin_ffsl
static char* stpcpy(char* d, const char* s) {
    size_t n = strlen(s);
    memcpy(d, s, n + 1);
    return d + n;
}
static char* strcasestr(const char* s, const char* sub) {
    for (size_t n = strlen(sub); *s; s++)
        if (!_strnicmp(s, sub, n)) return (char*)s;
    return *sub ? NULL : (char*)s;
}
#else
#include <strings.h>
#endif

/* BSD's strlcpy and strlcat, which glibc has only since 2.38: the length of the string they tried to make */
static size_t lcpy(char* d, const char* s, size_t n) {
    size_t len = strlen(s);
    if (n) {
        size_t k = len < n - 1 ? len : n - 1;
        memcpy(d, s, k);
        d[k] = 0;
    }
    return len;
}
static size_t lcat(char* d, const char* s, size_t n) {
    size_t had = strnlen(d, n);
    return had == n ? n + strlen(s) : had + lcpy(d + had, s, n - had);
}

#define S(a)  ((char*)hook_host(c, a))
#define G(p)  ((u64)((const u8*)(p) - (const u8*)hook_host(c, 0)))
#define GN(p) ((p) ? G(p) : 0)

/* errno: musl numbers like Linux, as the net stack's guest errno values do (common/network/abi/errno.inc). The C
   library sets a few the net stack never reports; they are listed here. A number with no name passes through. */
#define LIBC_ERRNOS(X)                                                                                                 \
    X(NOEXEC, 8) X(XDEV, 18) X(TXTBSY, 26) X(MLINK, 31) X(DEADLK, 35) X(NOLCK, 37) X(NOTEMPTY, 39) X(IDRM, 43)       \
        X(NOLINK, 67) X(PROTO, 71) X(MULTIHOP, 72) X(BADMSG, 74) X(OWNERDEAD, 130) X(NOTRECOVERABLE, 131)
int libc_errno_musl(int host) {
#ifndef _WIN32
#define SNX_NET_ERRNO(name, value)                                                                                     \
    if (host == E##name) return value;
#include "common/network/abi/errno.inc"
    if (host == EWOULDBLOCK) return 11;
#define TO_MUSL(name, value)                                                                                           \
    if (host == E##name) return value;
    LIBC_ERRNOS(TO_MUSL)
#undef TO_MUSL
#endif
    return host;
}
int libc_errno_host(int musl) {
#ifndef _WIN32
#define SNX_NET_ERRNO(name, value)                                                                                     \
    if (musl == value) return E##name;
#include "common/network/abi/errno.inc"
#define TO_HOST(name, value)                                                                                           \
    if (musl == value) return E##name;
    LIBC_ERRNOS(TO_HOST)
#undef TO_HOST
#endif
    return musl;
}
/* the address of the calling guest thread's errno, through the SDK's own __errno_location; 0 without one */
static u64 errno_at(CPU* c) {
    static u64 fn;
    if (!fn) fn = kernel_find_symbol_exact("__errno_location");
    if (!fn) return 0;
    hook_guest_call(c, fn, 0, 0, 0, 0);
    return c->x[0];
}
void libc_set_errno(CPU* c, int e) {
    u64 at;
    if (e && (at = errno_at(c)) != 0) snx_st_le32(hook_host(c, at), (u32)libc_errno_musl(e));
}
int libc_get_errno(CPU* c) {
    u64 at = errno_at(c);
    return at ? libc_errno_host((int)snx_ld_le32(hook_host(c, at))) : 0;
}

/* ---- strings and memory */
i32 libc_memcmp(CPU* c, u64 a, u64 b, u64 n) { return n ? memcmp(S(a), S(b), (size_t)n) : 0; }
u64 libc_memchr(CPU* c, u64 s, i32 ch, u64 n) { return n ? GN(memchr(S(s), ch, (size_t)n)) : 0; }
u64 libc_strlen(CPU* c, u64 s) { return strlen(S(s)); }
u64 libc_strnlen(CPU* c, u64 s, u64 n) { return strnlen(S(s), (size_t)n); }
i32 libc_strcmp(CPU* c, u64 a, u64 b) { return strcmp(S(a), S(b)); }
i32 libc_strncmp(CPU* c, u64 a, u64 b, u64 n) { return strncmp(S(a), S(b), (size_t)n); }
i32 libc_strcasecmp(CPU* c, u64 a, u64 b) { return strcasecmp(S(a), S(b)); }
i32 libc_strncasecmp(CPU* c, u64 a, u64 b, u64 n) { return strncasecmp(S(a), S(b), (size_t)n); }
i32 libc_strncasecmp_l(CPU* c, u64 a, u64 b, u64 n, u64 locale) { /* every locale of the SDK's is C's */
    (void)locale;
    return strncasecmp(S(a), S(b), (size_t)n);
}
i32 libc_strcoll(CPU* c, u64 a, u64 b) { return strcmp(S(a), S(b)); } /* the C locale's collation */
u64 libc_strcpy(CPU* c, u64 d, u64 s) {
    strcpy(S(d), S(s));
    return d;
}
u64 libc_strncpy(CPU* c, u64 d, u64 s, u64 n) {
    strncpy(S(d), S(s), (size_t)n);
    return d;
}
u64 libc_stpcpy(CPU* c, u64 d, u64 s) { return G(stpcpy(S(d), S(s))); }
u64 libc_strcat(CPU* c, u64 d, u64 s) {
    strcat(S(d), S(s));
    return d;
}
u64 libc_strncat(CPU* c, u64 d, u64 s, u64 n) {
    strncat(S(d), S(s), (size_t)n);
    return d;
}
u64 libc_strlcpy(CPU* c, u64 d, u64 s, u64 n) { return lcpy(S(d), S(s), (size_t)n); }
u64 libc_strlcat(CPU* c, u64 d, u64 s, u64 n) { return lcat(S(d), S(s), (size_t)n); }
u64 libc_strchr(CPU* c, u64 s, i32 ch) { return GN(strchr(S(s), ch)); }
u64 libc_strrchr(CPU* c, u64 s, i32 ch) { return GN(strrchr(S(s), ch)); }
u64 libc_strstr(CPU* c, u64 s, u64 sub) { return GN(strstr(S(s), S(sub))); }
u64 libc_strcasestr(CPU* c, u64 s, u64 sub) { return GN(strcasestr(S(s), S(sub))); }
u64 libc_strpbrk(CPU* c, u64 s, u64 set) { return GN(strpbrk(S(s), S(set))); }
u64 libc_strspn(CPU* c, u64 s, u64 set) { return strspn(S(s), S(set)); }
u64 libc_strcspn(CPU* c, u64 s, u64 set) { return strcspn(S(s), S(set)); }
/* musl's strtok_r, which the SDK's strtok and strtok_r are: the next token of s, or of *p when s is NULL, and *p the
   place after it; NULL with no string to go on in, and *p NULL once the string is used up (glibc's would read *p
   then, and leave it at the string's end) */
static char* tok(char* s, const char* sep, char** p) {
    if (!s && !(s = *p)) return NULL;
    s += strspn(s, sep);
    if (!*s) return *p = NULL;
    *p = s + strcspn(s, sep);
    if (**p)
        *(*p)++ = 0;
    else
        *p = NULL;
    return s;
}
/* strtok's place in the string it was last given, a guest address, as the SDK's keeps one for every thread alike */
static u64 g_strtok;
u64 libc_strtok(CPU* c, u64 s, u64 delim) {
    char* save = g_strtok ? S(g_strtok) : NULL;
    char* t = tok(s ? S(s) : NULL, S(delim), &save);
    g_strtok = GN(save);
    return GN(t);
}
u64 libc_strtok_r(CPU* c, u64 s, u64 delim, u64 save) {
    u64 at = snx_ld_le64(hook_host(c, save));
    char* p = at ? S(at) : NULL;
    char* t = tok(s ? S(s) : NULL, S(delim), &p);
    snx_st_le64(hook_host(c, save), GN(p));
    return GN(t);
}
/* C11's memset_s: the bytes set always, and EINVAL (22) when more were asked than the destination holds */
i32 libc_memset_s(CPU* c, u64 d, u64 size, i32 ch, u64 n) {
    if (!d) return 22;
    memset(S(d), ch, (size_t)(n < size ? n : size));
    return n > size ? 22 : 0;
}

/* ---- character classes: an int that is EOF or an unsigned char's value, anything else none of them */
#define CTYPE(name)                                                                                                    \
    i32 libc_##name(CPU* c, i32 ch) {                                                                                  \
        (void)c;                                                                                                       \
        return ch >= -1 && ch <= 255 && name(ch) ? 1 : 0;                                                              \
    }
CTYPE(isalnum)
CTYPE(isalpha)
CTYPE(isblank)
CTYPE(iscntrl)
CTYPE(isdigit)
CTYPE(isgraph)
CTYPE(islower)
CTYPE(isprint)
CTYPE(ispunct)
CTYPE(isspace)
CTYPE(isupper)
CTYPE(isxdigit)
#undef CTYPE
i32 libc_tolower(CPU* c, i32 ch) {
    (void)c;
    return ch >= -1 && ch <= 255 ? tolower(ch) : ch;
}
i32 libc_toupper(CPU* c, i32 ch) {
    (void)c;
    return ch >= -1 && ch <= 255 ? toupper(ch) : ch;
}

/* ---- numbers from text: the end pointer back as a guest address, errno the guest's */
i32 libc_atoi(CPU* c, u64 s) { return atoi(S(s)); }
i64 libc_atol(CPU* c, u64 s) { return atol(S(s)); }
i64 libc_atoll(CPU* c, u64 s) { return atoll(S(s)); }
f64 libc_atof(CPU* c, u64 s) { return atof(S(s)); }
#define STRTO(name, T, call)                                                                                           \
    T libc_##name(CPU* c, u64 s, u64 end, i32 base) {                                                                  \
        char* e;                                                                                                       \
        errno = 0;                                                                                                     \
        T v = (T)call(S(s), &e, base);                                                                                 \
        libc_set_errno(c, errno);                                                                                           \
        if (end) snx_st_le64(hook_host(c, end), G(e));                                                                 \
        return v;                                                                                                      \
    }
STRTO(strtol, i64, strtol)
STRTO(strtoul, u64, strtoul)
STRTO(strtoll, i64, strtoll)
STRTO(strtoull, u64, strtoull)
#undef STRTO
f64 libc_strtod(CPU* c, u64 s, u64 end) {
    char* e;
    errno = 0;
    f64 v = strtod(S(s), &e);
    libc_set_errno(c, errno);
    if (end) snx_st_le64(hook_host(c, end), G(e));
    return v;
}
f32 libc_strtof(CPU* c, u64 s, u64 end) {
    char* e;
    errno = 0;
    f32 v = strtof(S(s), &e);
    libc_set_errno(c, errno);
    if (end) snx_st_le64(hook_host(c, end), G(e));
    return v;
}

/* ---- integer arithmetic. div's div_t (two ints) comes back in x0, ldiv's ldiv_t (two longs) in x0 and x1. Their
   quotient is the guest's sdiv's (cpu.h's sdiv32 and sdiv64): 0 for a divisor of 0, and the dividend back for the
   lowest value over -1, where the host's division traps */
/* The lowest value has no opposite and comes back as it is, as the guest's own code leaves it; the game's long is 64
   bits, which a host's may not be */
i32 libc_abs(CPU* c, i32 v) {
    (void)c;
    return (i32)(v < 0 ? 0u - (u32)v : (u32)v);
}
i64 libc_labs(CPU* c, i64 v) {
    (void)c;
    return (i64)(v < 0 ? 0u - (u64)v : (u64)v);
}
i64 libc_llabs(CPU* c, i64 v) { return libc_labs(c, v); }
u64 libc_div(CPU* c, i32 n, i32 d) {
    (void)c;
    i32 q = sdiv32(n, d);
    return (u32)q | (u64)((u32)n - (u32)q * (u32)d) << 32;
}
i64 libc_ldiv(CPU* c, i64 n, i64 d) {
    i64 q = sdiv64(n, d);
    c->x[1] = (u64)n - (u64)q * (u64)d;
    return q;
}
i32 libc_ffs(CPU* c, i32 v) {
    (void)c;
    return ffs(v);
}
i32 libc_ffsl(CPU* c, i64 v) {
    (void)c;
    return ffsl((long)v);
}

/* ---- floating point */
f64 libc_fabs(CPU* c, f64 v) {
    (void)c;
    return fabs(v);
}
f64 libc_ldexp(CPU* c, f64 v, i32 e) {
    (void)c;
    return ldexp(v, e);
}
f64 libc_frexp(CPU* c, f64 v, u64 e) {
    int x;
    f64 r = frexp(v, &x);
    snx_st_le32(hook_host(c, e), (u32)x);
    return r;
}
f64 libc_modf(CPU* c, f64 v, u64 ip) {
    double i;
    f64 r = modf(v, &i);
    u64 bits;
    memcpy(&bits, &i, 8);
    snx_st_le64(hook_host(c, ip), bits);
    return r;
}

/* musl's fpclassify values (FP_NAN 0, FP_INFINITE 1, FP_ZERO 2, FP_SUBNORMAL 3, FP_NORMAL 4), which other C
   libraries number otherwise */
i32 libc_fpclassifyf(CPU* c, f32 v) {
    (void)c;
    switch (fpclassify(v)) {
        case FP_NAN: return 0;
        case FP_INFINITE: return 1;
        case FP_ZERO: return 2;
        case FP_SUBNORMAL: return 3;
        default: return 4;
    }
}

/* ---- the network's byte order, big-endian, from the guest's little-endian */
u32 libc_htonl(CPU* c, u32 v) {
    (void)c;
    return snx_bswap32(v);
}
u32 libc_htons(CPU* c, u32 v) {
    (void)c;
    return snx_bswap16((u16)v);
}

/* ---- pseudo-random numbers, musl's: rand's 64-bit LCG, which srand seeds with seed - 1 and a program that never calls
   it starts at 0; rand_r's 32-bit one, tempered */
static _Atomic u64 g_rand;
i32 libc_rand(CPU* c) {
    (void)c;
    u64 s = atomic_load_explicit(&g_rand, memory_order_relaxed) * 6364136223846793005ull + 1;
    atomic_store_explicit(&g_rand, s, memory_order_relaxed);
    return (i32)(s >> 33);
}
void libc_srand(CPU* c, u32 seed) {
    (void)c;
    atomic_store_explicit(&g_rand, (u64)(seed - 1), memory_order_relaxed);
}
i32 libc_rand_r(CPU* c, u64 seed) {
    u32 x = snx_ld_le32(hook_host(c, seed)) * 1103515245u + 12345u;
    snx_st_le32(hook_host(c, seed), x);
    x ^= x >> 11;
    x ^= x << 7 & 0x9D2C5680u;
    x ^= x << 15 & 0xEFC60000u;
    x ^= x >> 18;
    return (i32)(x / 2);
}

/* ---- qsort and bsearch: the host's, comparing through the game's function. The thread's context for the
   comparison function, which takes no argument of its own */
typedef struct {
    CPU* c;
    u64 fn;
} CmpCtx;
static _Thread_local CmpCtx g_cmp;
static int cmp_guest(const void* a, const void* b) {
    CPU* c = g_cmp.c;
    hook_guest_call(c, g_cmp.fn, G(a), G(b), 0, 0);
    return (i32)c->x[0];
}
void libc_qsort(CPU* c, u64 base, u64 n, u64 size, u64 fn) {
    CmpCtx was = g_cmp; /* a comparison that sorts again */
    if (!n || !size) return;
    g_cmp = (CmpCtx){c, fn};
    qsort(S(base), (size_t)n, (size_t)size, cmp_guest);
    g_cmp = was;
}
u64 libc_bsearch(CPU* c, u64 key, u64 base, u64 n, u64 size, u64 fn) {
    CmpCtx was = g_cmp;
    if (!n || !size) return 0;
    g_cmp = (CmpCtx){c, fn};
    void* r = bsearch(S(key), S(base), (size_t)n, (size_t)size, cmp_guest);
    g_cmp = was;
    return GN(r);
}
