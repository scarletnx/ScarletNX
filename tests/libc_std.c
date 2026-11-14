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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
/* the log's last line (libc_fmt.c's standard output), and the formats left to the function's own code */
static char g_log[512];
static int g_own_runs;
void logf_(int level, const char* format, ...) {
    va_list args;
    (void)level;
    va_start(args, format);
    vsnprintf(g_log, sizeof g_log, format, args);
    va_end(args);
}
void fatal(const char* format, ...) {
    (void)format;
    abort();
}
void hook_run_original(CPU* c, u64 addr) {
    (void)addr;
    g_own_runs++;
    c->x[0] = 777;
}
static void set_f64(CPU* c, int r, f64 v) {
    u64 bits;
    memcpy(&bits, &v, 8);
    c->v[r].d[0] = SNX_GUEST64(bits);
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
    /* snprintf, a direct call: ints and a string in x3 on, a double in v0, the rest on the stack */
    u64 f = put(0x8000, "%d|%5s|%.2f|%x|%lld|%c|%s|%p");
    c->x[3] = (u64)-7, c->x[4] = put(0x8100, "ab"), c->x[5] = 255, c->x[6] = (u64)-123456789012ll, c->x[7] = 'Z';
    set_f64(c, 0, 3.14159);
    c->x[31] = 0x8800; /* the stack: a NULL string, then a pointer */
    snx_st_le64(g_mem + 0x8800, 0);
    snx_st_le64(g_mem + 0x8808, 0xdeadbeef);
    i32 n2 = libc_snprintf(c, 0x9000, 64, f);
    const char* want = "-7|   ab|3.14|ff|-123456789012|Z|(null)|0x00000000deadbeef";
    assert(n2 == (i32)strlen(want) && !strcmp((char*)g_mem + 0x9000, want));
    /* truncated to the size, the whole length returned */
    assert(libc_snprintf(c, 0x9100, 4, put(0x8200, "hello")) == 5 && !strcmp((char*)g_mem + 0x9100, "hel"));
    /* vsnprintf, through a va_list record: two ints from the general registers' save area, a double from the vector
       registers', a long from the stack */
    u64 ap = 0xA000, gr_top = 0xA200, vr_top = 0xA400;
    snx_st_le64(g_mem + ap, 0xA600);       /* __stack */
    snx_st_le64(g_mem + ap + 8, gr_top);   /* __gr_top */
    snx_st_le64(g_mem + ap + 16, vr_top);  /* __vr_top */
    snx_st_le32(g_mem + ap + 24, (u32)-16); /* __gr_offs: two left */
    snx_st_le32(g_mem + ap + 28, (u32)-16); /* __vr_offs: one left */
    snx_st_le64(g_mem + gr_top - 16, 11);
    snx_st_le64(g_mem + gr_top - 8, 22);
    f64 half = 0.5;
    memcpy(g_mem + vr_top - 16, &half, 8);
    snx_st_le64(g_mem + 0xA600, 33);
    assert(libc_vsnprintf(c, 0x9200, 64, put(0x8300, "%d %d %g %ld"), ap) == 12 &&
           !strcmp((char*)g_mem + 0x9200, "11 22 0.5 33"));
    /* %n through the guest's pointer, and a numbered argument left to the function's own code */
    c->x[3] = 0xB000;
    assert(libc_snprintf(c, 0x9300, 64, put(0x8400, "abc%n")) == 3 && snx_ld_le32(g_mem + 0xB000) == 3);
    assert(libc_snprintf(c, 0x9300, 64, put(0x8500, "%1$d")) == 777 && g_own_runs == 1);
    /* sscanf through the guest's pointers */
    c->x[2] = 0xB100, c->x[3] = 0xB200, c->x[4] = 0xB300, c->x[5] = 0xB400;
    assert(libc_sscanf(c, put(0x8600, " 42 hello 2.5 ff"), put(0x8700, "%d %s %lf %x")) == 4);
    f64 got;
    memcpy(&got, g_mem + 0xB300, 8);
    assert((i32)snx_ld_le32(g_mem + 0xB100) == 42 && !strcmp((char*)g_mem + 0xB200, "hello") && got == 2.5 &&
           snx_ld_le32(g_mem + 0xB400) == 255);
    c->x[2] = 0xB100;
    assert(libc_sscanf(c, put(0x8600, "x"), put(0x8700, "%d")) == 0 && libc_sscanf(c, put(0x8600, ""), 0x8700) == -1);
    /* printf: the log, a line at a time */
    c->x[1] = 5;
    assert(libc_printf(c, put(0x8800, "n=%d\n")) == 4 && !strcmp(g_log, "[game] n=5\n"));
    puts("PASS the C library's string, number, formatting and sorting functions on guest memory");
    return 0;
}
