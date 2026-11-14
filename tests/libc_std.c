/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The libc_str family's handlers (src/nx/hle/hook/libc_std.c) on guest memory: a pointer into an argument comes back
   as the guest address of the same byte, an end pointer is written as one, strtok keeps its place between calls,
   div packs div_t into x0 and ldiv puts ldiv_t's second long in x1, errno goes to the address __errno_location gives
   (here a guest call answered by the test), and qsort and bsearch compare through the guest's function (the test's
   comparison of ints) */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define ERRNO_AT 0x100u /* where the test's __errno_location says errno is */
#define CMP_FN   0x40u  /* the guest's comparison function's address, as the test answers it */
static u8 g_mem[0x10000];
static CPU g_cpu;

u64 kernel_find_symbol_exact(const char* name) { return !strcmp(name, "__errno_location") ? 0x80 : 0; }
/* the guest calls the handlers make: __errno_location, and a comparison of two little-endian ints */
void hook_guest_call(CPU* c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a2, (void)a3;
    if (fn == 0x80) {
        c->x[0] = ERRNO_AT;
        return;
    }
    assert(fn == CMP_FN);
    i32 a = (i32)snx_ld_le32(hook_host(c, a0)), b = (i32)snx_ld_le32(hook_host(c, a1));
    c->x[0] = (u64)(i64)((a > b) - (a < b));
}
static u64 put(u64 at, const char* s) {
    memcpy(g_mem + at, s, strlen(s) + 1);
    return at;
}

int main(void) {
    CPU* c = &g_cpu;
    c->mem = g_mem;
    /* pointers into the argument, as guest addresses */
    u64 s = put(0x1000, "hello, world");
    assert(libc_strchr(c, s, 'w') == s + 7 && libc_strchr(c, s, 'z') == 0 && libc_strrchr(c, s, 'o') == s + 8);
    assert(libc_strstr(c, s, put(0x2000, "world")) == s + 7 && libc_strpbrk(c, s, put(0x2100, ",")) == s + 5);
    assert(libc_memchr(c, s, 'l', 12) == s + 2 && libc_memchr(c, s, 'l', 0) == 0);
    assert(libc_strlen(c, s) == 12 && libc_strcmp(c, s, s) == 0 && libc_strcasecmp(c, s, put(0x2200, "HELLO, WORLD")) == 0);
    assert(libc_stpcpy(c, 0x3000, s) == 0x3000 + 12 && !strcmp((char*)g_mem + 0x3000, "hello, world"));
    assert(libc_strlcpy(c, 0x3100, s, 6) == 12 && !strcmp((char*)g_mem + 0x3100, "hello"));
    /* strtok's place, and strtok_r's in the guest's pointer */
    u64 t = put(0x4000, "a b  c");
    assert(libc_strtok(c, t, put(0x2300, " ")) == t && libc_strtok(c, 0, 0x2300) == t + 2 &&
           libc_strtok(c, 0, 0x2300) == t + 5 && libc_strtok(c, 0, 0x2300) == 0);
    t = put(0x4100, "x,y");
    assert(libc_strtok_r(c, t, put(0x2400, ","), 0x4200) == t && libc_strtok_r(c, 0, 0x2400, 0x4200) == t + 2);
    /* numbers: the end pointer, and errno on overflow */
    u64 n = put(0x5000, "  -42xyz");
    assert(libc_strtol(c, n, 0x5100, 10) == -42 && snx_ld_le64(g_mem + 0x5100) == n + 5);
    assert(libc_strtoul(c, put(0x5200, "ff"), 0, 16) == 255 && snx_ld_le32(g_mem + ERRNO_AT) == 0);
    libc_strtol(c, put(0x5300, "99999999999999999999999"), 0, 10);
    assert(snx_ld_le32(g_mem + ERRNO_AT) == 34); /* ERANGE */
    assert(libc_strtod(c, put(0x5400, "1.5e2!"), 0x5500) == 150.0 && snx_ld_le64(g_mem + 0x5500) == 0x5400 + 5);
    assert(libc_atoi(c, put(0x5600, "123")) == 123 && libc_isdigit(c, '7') && !libc_isdigit(c, 300));
    assert(libc_tolower(c, 'Q') == 'q' && libc_tolower(c, -1) == -1);
    /* div_t packed in x0, ldiv_t's remainder in x1 */
    assert(libc_div(c, 7, -2) == ((u32)-3 | (u64)1 << 32));
    assert(libc_ldiv(c, 17, 5) == 3 && c->x[1] == 2);
    assert(libc_htonl(c, 0x11223344) == 0x44332211 && libc_htons(c, 0x1122) == 0x2211);
    /* qsort and bsearch through the guest's comparison */
    i32 v[6] = {5, -1, 3, 9, 0, 3};
    for (int i = 0; i < 6; i++) snx_st_le32(g_mem + 0x6000 + 4 * i, (u32)v[i]);
    libc_qsort(c, 0x6000, 6, 4, CMP_FN);
    for (int i = 1; i < 6; i++)
        assert((i32)snx_ld_le32(g_mem + 0x6000 + 4 * i) >= (i32)snx_ld_le32(g_mem + 0x6000 + 4 * (i - 1)));
    snx_st_le32(g_mem + 0x6100, 9);
    assert(libc_bsearch(c, 0x6100, 0x6000, 6, 4, CMP_FN) == 0x6000 + 20);
    snx_st_le32(g_mem + 0x6100, 4);
    assert(libc_bsearch(c, 0x6100, 0x6000, 6, 4, CMP_FN) == 0);
    /* rand's range is the guest's RAND_MAX, and srand repeats its sequence */
    libc_srand(c, 7);
    i32 r0 = libc_rand(c);
    libc_srand(c, 7);
    assert(libc_rand(c) == r0 && r0 >= 0);
    /* wide strings, 32-bit characters: guest addresses back, and Unicode's classes and case as musl's */
    if (libc_wide_ready()) {
        static const u32 w[] = {'h', 'i', 0x4E00, ' ', '4', '2', 0};
        for (int i = 0; i < 7; i++) snx_st_le32(g_mem + 0x7000 + 4 * i, w[i]);
        assert(libc_wcslen(c, 0x7000) == 6 && libc_wcschr(c, 0x7000, 0x4E00) == 0x7008);
        assert(libc_iswalpha(c, 0x4E00) && !libc_iswalpha(c, '4') && libc_towlower(c, 0xC9) == 0xE9);
        assert(libc_towupper(c, 0x3B1) == 0x391); /* Greek alpha */
        assert(libc_wcstol(c, 0x7010, 0x7100, 10) == 42 && snx_ld_le64(g_mem + 0x7100) == 0x7018);
        static const u32 a[] = {'A', 0xC9, 0}, b[] = {'a', 0xE9, 0};
        for (int i = 0; i < 3; i++) snx_st_le32(g_mem + 0x7200 + 4 * i, a[i]), snx_st_le32(g_mem + 0x7300 + 4 * i, b[i]);
        assert(libc_wcscasecmp(c, 0x7200, 0x7300) == 0 && libc_wcscmp(c, 0x7200, 0x7300) < 0);
    } else
        puts("SKIP the wide functions: no UTF-8 locale on this host");
    assert(libc_fpclassifyf(c, 0.0f) == 2 && libc_fpclassifyf(c, 1.0f) == 4 && libc_fpclassifyf(c, __builtin_nanf("")) == 0);
    puts("PASS the C library's string, number and sorting functions on guest memory");
    return 0;
}
