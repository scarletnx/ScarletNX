/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The C library's wide string functions a game imports from its SDK (hooks.hook's libc_wide family), run by the
   host's on guest memory: the wcs and wmem functions, the wide character classes and case, and the conversions of
   numbers from wide text. The SDK's wchar_t is 32 bits, as the host's is on FreeBSD, Linux and macOS (Windows, whose
   is 16, leaves the family off).

   The SDK's C library is musl, whose wide character classes and case are Unicode's whatever the locale (its iswalpha
   reads its Unicode tables). The host's are its locale's, so the handlers take them from a C.UTF-8 locale of their
   own (iswalpha_l and the rest, made at the first call); a host without one leaves the family off (libc_wide_ready) */
#define _GNU_SOURCE
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#if !defined(_WIN32)
#include <locale.h>
#include <pthread.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <xlocale.h>
#endif
#endif

#define W(a)  ((wchar_t*)hook_host(c, a))
#define G(p)  ((u64)((const u8*)(p) - (const u8*)hook_host(c, 0)))
#define GN(p) ((p) ? G(p) : 0)

#if defined(_WIN32) || WCHAR_MAX <= 0xFFFF
int libc_wide_ready(void) { return 0; }
#define LOC 0
#define WIDE_OFF 1
#else
static locale_t g_utf8;
static void utf8_make(void) {
    g_utf8 = newlocale(LC_ALL_MASK, "C.UTF-8", (locale_t)0);
    if (!g_utf8) g_utf8 = newlocale(LC_ALL_MASK, "en_US.UTF-8", (locale_t)0);
}
int libc_wide_ready(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, utf8_make);
    return g_utf8 != (locale_t)0;
}
#define LOC g_utf8
#define WIDE_OFF 0
#endif

/* ---- wide strings and memory */
#if !WIDE_OFF
u64 libc_wcslen(CPU* c, u64 s) { return wcslen(W(s)); }
u64 libc_wcsnlen(CPU* c, u64 s, u64 n) { return wcsnlen(W(s), (size_t)n); }
i32 libc_wcscmp(CPU* c, u64 a, u64 b) { return wcscmp(W(a), W(b)); }
i32 libc_wcsncmp(CPU* c, u64 a, u64 b, u64 n) { return wcsncmp(W(a), W(b), (size_t)n); }
/* towlower of each character compared, as wcscasecmp does (FreeBSD has no wcscasecmp_l) */
i32 libc_wcscasecmp(CPU* c, u64 a, u64 b) {
    const wchar_t *x = W(a), *y = W(b);
    for (;; x++, y++) {
        wint_t p = towlower_l((wint_t)*x, LOC), q = towlower_l((wint_t)*y, LOC);
        if (p != q || !*x) return p < q ? -1 : p > q;
    }
}
u64 libc_wcscpy(CPU* c, u64 d, u64 s) {
    wcscpy(W(d), W(s));
    return d;
}
u64 libc_wcsncpy(CPU* c, u64 d, u64 s, u64 n) {
    wcsncpy(W(d), W(s), (size_t)n);
    return d;
}
u64 libc_wcscat(CPU* c, u64 d, u64 s) {
    wcscat(W(d), W(s));
    return d;
}
u64 libc_wcsncat(CPU* c, u64 d, u64 s, u64 n) {
    wcsncat(W(d), W(s), (size_t)n);
    return d;
}
u64 libc_wcschr(CPU* c, u64 s, u32 ch) { return GN(wcschr(W(s), (wchar_t)ch)); }
u64 libc_wcsrchr(CPU* c, u64 s, u32 ch) { return GN(wcsrchr(W(s), (wchar_t)ch)); }
u64 libc_wcsstr(CPU* c, u64 s, u64 sub) { return GN(wcsstr(W(s), W(sub))); }
u64 libc_wcstok(CPU* c, u64 s, u64 delim, u64 save) {
    u64 at = snx_ld_le64(hook_host(c, save));
    wchar_t* p = at ? W(at) : NULL;
    wchar_t* t = wcstok(s ? W(s) : NULL, W(delim), &p);
    snx_st_le64(hook_host(c, save), GN(p));
    return GN(t);
}
u64 libc_wmemchr(CPU* c, u64 s, u32 ch, u64 n) { return n ? GN(wmemchr(W(s), (wchar_t)ch, (size_t)n)) : 0; }
i32 libc_wmemcmp(CPU* c, u64 a, u64 b, u64 n) { return n ? wmemcmp(W(a), W(b), (size_t)n) : 0; }
u64 libc_wmemcpy(CPU* c, u64 d, u64 s, u64 n) {
    if (n) wmemcpy(W(d), W(s), (size_t)n);
    return d;
}
u64 libc_wmemmove(CPU* c, u64 d, u64 s, u64 n) {
    if (n) wmemmove(W(d), W(s), (size_t)n);
    return d;
}
u64 libc_wmemset(CPU* c, u64 d, u32 ch, u64 n) {
    if (n) wmemset(W(d), (wchar_t)ch, (size_t)n);
    return d;
}

/* ---- wide character classes and case: Unicode's, as musl's */
#define WCTYPE(name)                                                                                                   \
    i32 libc_##name(CPU* c, u32 wc) {                                                                                  \
        (void)c;                                                                                                       \
        return name##_l((wint_t)wc, LOC) ? 1 : 0;                                                                      \
    }
WCTYPE(iswalnum)
WCTYPE(iswalpha)
WCTYPE(iswdigit)
WCTYPE(iswgraph)
WCTYPE(iswlower)
WCTYPE(iswprint)
WCTYPE(iswspace)
WCTYPE(iswupper)
WCTYPE(iswxdigit)
#undef WCTYPE
u32 libc_towlower(CPU* c, u32 wc) {
    (void)c;
    return (u32)towlower_l((wint_t)wc, LOC);
}
u32 libc_towupper(CPU* c, u32 wc) {
    (void)c;
    return (u32)towupper_l((wint_t)wc, LOC);
}

/* ---- numbers from wide text: the end pointer back as a guest address, errno the guest's (libc_set_errno) */
#define WCSTO(name, T, call)                                                                                           \
    T libc_##name(CPU* c, u64 s, u64 end, i32 base) {                                                                  \
        wchar_t* e;                                                                                                    \
        errno = 0;                                                                                                     \
        T v = (T)call(W(s), &e, base);                                                                                 \
        libc_set_errno(c, errno);                                                                                      \
        if (end) snx_st_le64(hook_host(c, end), G(e));                                                                 \
        return v;                                                                                                      \
    }
WCSTO(wcstol, i64, wcstol)
WCSTO(wcstoul, u64, wcstoul)
WCSTO(wcstoll, i64, wcstoll)
WCSTO(wcstoull, u64, wcstoull)
#undef WCSTO
f64 libc_wcstod(CPU* c, u64 s, u64 end) {
    wchar_t* e;
    errno = 0;
    f64 v = wcstod(W(s), &e);
    libc_set_errno(c, errno);
    if (end) snx_st_le64(hook_host(c, end), G(e));
    return v;
}
f32 libc_wcstof(CPU* c, u64 s, u64 end) {
    wchar_t* e;
    errno = 0;
    f32 v = wcstof(W(s), &e);
    libc_set_errno(c, errno);
    if (end) snx_st_le64(hook_host(c, end), G(e));
    return v;
}
#else
/* the table names them, and the family is off on this host: never called */
#define STUB(T, name, ...)                                                                                             \
    T name(CPU* c, __VA_ARGS__) {                                                                                      \
        (void)c;                                                                                                       \
        return 0;                                                                                                      \
    }
#define STUB1(T, name, A)          STUB(T, name, A a)
#define STUB2(T, name, A, B)       STUB(T, name, A a, B b)
#define STUB3(T, name, A, B, C)    STUB(T, name, A a, B b, C d)
STUB1(u64, libc_wcslen, u64)
STUB2(u64, libc_wcsnlen, u64, u64)
STUB2(i32, libc_wcscmp, u64, u64)
STUB3(i32, libc_wcsncmp, u64, u64, u64)
STUB2(i32, libc_wcscasecmp, u64, u64)
STUB2(u64, libc_wcscpy, u64, u64)
STUB3(u64, libc_wcsncpy, u64, u64, u64)
STUB2(u64, libc_wcscat, u64, u64)
STUB3(u64, libc_wcsncat, u64, u64, u64)
STUB2(u64, libc_wcschr, u64, u32)
STUB2(u64, libc_wcsrchr, u64, u32)
STUB2(u64, libc_wcsstr, u64, u64)
STUB3(u64, libc_wcstok, u64, u64, u64)
STUB3(u64, libc_wmemchr, u64, u32, u64)
STUB3(i32, libc_wmemcmp, u64, u64, u64)
STUB3(u64, libc_wmemcpy, u64, u64, u64)
STUB3(u64, libc_wmemmove, u64, u64, u64)
STUB3(u64, libc_wmemset, u64, u32, u64)
STUB1(i32, libc_iswalnum, u32)
STUB1(i32, libc_iswalpha, u32)
STUB1(i32, libc_iswdigit, u32)
STUB1(i32, libc_iswgraph, u32)
STUB1(i32, libc_iswlower, u32)
STUB1(i32, libc_iswprint, u32)
STUB1(i32, libc_iswspace, u32)
STUB1(i32, libc_iswupper, u32)
STUB1(i32, libc_iswxdigit, u32)
STUB1(u32, libc_towlower, u32)
STUB1(u32, libc_towupper, u32)
STUB3(i64, libc_wcstol, u64, u64, i32)
STUB3(u64, libc_wcstoul, u64, u64, i32)
STUB3(i64, libc_wcstoll, u64, u64, i32)
STUB3(u64, libc_wcstoull, u64, u64, i32)
STUB2(f64, libc_wcstod, u64, u64)
STUB2(f32, libc_wcstof, u64, u64)
#endif
