/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The C library's functions a game imports from its SDK (hooks.hook's libc_str family), run by the host's C library on
   guest memory: the string and memory functions, the character classes, the conversions of numbers, abs and div, the
   floating point helpers, the byte order of the network, the pseudo-random numbers, qsort and bsearch. A function
   that returns a pointer into its argument returns the guest address of the same byte; one that sets errno sets the
   guest's, which __errno_location, the SDK's own, finds for the calling thread.

   The character classes and conversions are the C locale's, which the SDK's libc is in and the host is too (the
   runtime never calls setlocale). rand and srand are the host's random and srandom, whose RAND_MAX is the guest's
   (2^31 - 1), with a state of their own apart from the runtime's. qsort and bsearch call the game's comparison
   function for each comparison the host's makes (hook_guest_call). The family is off on Windows, whose C library
   lacks half of these and has another RAND_MAX */
#define _GNU_SOURCE /* glibc's strcasestr */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/kernel/kernel.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
/* what the handlers call, for the table to link: the family is off on Windows (hook.c's family_unready) */
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#define strtok_r    strtok_s
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
static long random(void) { return rand(); }
static void srandom(unsigned s) { srand(s); }
static char* initstate(unsigned s, char* state, size_t n) {
    (void)state, (void)n;
    srand(s);
    return NULL;
}
static long nrand48(unsigned short x[3]) {
    (void)x;
    return rand();
}
static int rand_r(unsigned* s) {
    (void)s;
    return rand();
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

/* errno of the calling guest thread, through the SDK's __errno_location: e when it is set, otherwise nothing */
static void set_errno(CPU* c, int e) {
    static u64 fn;
    if (!e) return;
    if (!fn) fn = kernel_find_symbol_exact("__errno_location");
    if (!fn) return;
    hook_guest_call(c, fn, 0, 0, 0, 0);
    if (c->x[0]) snx_st_le32(hook_host(c, c->x[0]), (u32)e); /* POSIX's numbers, the SDK's and the host's alike */
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
/* strtok's place in the string it was last given, a guest address, as the SDK's keeps one for every thread alike */
static u64 g_strtok;
u64 libc_strtok(CPU* c, u64 s, u64 delim) {
    char* save = g_strtok ? S(g_strtok) : NULL;
    char* t = strtok_r(s ? S(s) : NULL, S(delim), &save);
    g_strtok = GN(save);
    return GN(t);
}
u64 libc_strtok_r(CPU* c, u64 s, u64 delim, u64 save) {
    u64 at = snx_ld_le64(hook_host(c, save));
    char* p = at ? S(at) : NULL;
    char* t = strtok_r(s ? S(s) : NULL, S(delim), &p);
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
        set_errno(c, errno);                                                                                           \
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
    set_errno(c, errno);
    if (end) snx_st_le64(hook_host(c, end), G(e));
    return v;
}
f32 libc_strtof(CPU* c, u64 s, u64 end) {
    char* e;
    errno = 0;
    f32 v = strtof(S(s), &e);
    set_errno(c, errno);
    if (end) snx_st_le64(hook_host(c, end), G(e));
    return v;
}

/* ---- integer arithmetic. div's div_t (two ints) comes back in x0, ldiv's ldiv_t (two longs) in x0 and x1 */
i32 libc_abs(CPU* c, i32 v) {
    (void)c;
    return abs(v);
}
i64 libc_labs(CPU* c, i64 v) {
    (void)c;
    return labs((long)v);
}
i64 libc_llabs(CPU* c, i64 v) {
    (void)c;
    return llabs((long long)v);
}
u64 libc_div(CPU* c, i32 n, i32 d) {
    (void)c;
    div_t r = div(n, d);
    return (u32)r.quot | (u64)(u32)r.rem << 32;
}
i64 libc_ldiv(CPU* c, i64 n, i64 d) {
    ldiv_t r = ldiv((long)n, (long)d);
    c->x[1] = (u64)r.rem;
    return r.quot;
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

/* ---- the network's byte order, big-endian, from the guest's little-endian */
u32 libc_htonl(CPU* c, u32 v) {
    (void)c;
    return __builtin_bswap32(v);
}
u32 libc_htons(CPU* c, u32 v) {
    (void)c;
    return __builtin_bswap16((u16)v);
}

/* ---- pseudo-random numbers: the host's random() and nrand48, states of their own */
static unsigned short g_rand48[3] = {0x330e, 0, 0};
static char g_random_state[256];
static int g_random_init;
static void random_init(void) {
    if (!g_random_init) { /* seed 1, as a program's that never calls srand starts */
        initstate(1, g_random_state, sizeof g_random_state);
        g_random_init = 1;
    }
}
i32 libc_rand(CPU* c) {
    (void)c;
    random_init();
    return (i32)random();
}
void libc_srand(CPU* c, u32 seed) {
    (void)c;
    if (!g_random_init) random_init();
    srandom(seed);
}
i32 libc_rand_r(CPU* c, u64 seed) {
    unsigned s = snx_ld_le32(hook_host(c, seed));
    int r = rand_r(&s);
    snx_st_le32(hook_host(c, seed), s);
    return r;
}
i64 libc_lrand48(CPU* c) {
    (void)c;
    return nrand48(g_rand48);
}
void libc_srand48(CPU* c, i64 seed) {
    (void)c;
    g_rand48[0] = 0x330e;
    g_rand48[1] = (unsigned short)seed;
    g_rand48[2] = (unsigned short)(seed >> 16);
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
