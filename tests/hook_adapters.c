/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The adapters tools/hookgen generates (tests/hook/test.hook): each reads its function's arguments from the registers
   AAPCS64 gives them (integers in x0-x7, floats in v0-v7, each counted apart), hands them to its handler, puts the
   result in x0 (narrow ones extended to 32 bits) or v0 (the rest of v0 zeroed), and returns to x30. The adapters for a
   32-bit program (hookgen --a32) read them where AAPCS32 with VFP arguments puts them (r0-r3, an even pair of them for
   64 bits, s and d registers apart, the rest on the stack) and return to lr in the state its bit 0 says */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "hle/hook/hooks.hook.inc"
#include <assert.h>
#include <stdio.h>

static u64 seen[8];
static f64 seen_f[2];
static int calls;

i64 test_mixed(CPU* c, u64 self, i32 a, f32 b, u32 c2, f64 d) {
    (void)c;
    seen[0] = self, seen[1] = (u64)(i64)a, seen[2] = c2;
    seen_f[0] = b, seen_f[1] = d;
    return -2;
}
i8 test_narrow_i8(CPU* c, i8 a) {
    (void)c;
    return (i8)(a - 1);
}
u16 test_narrow_u16(CPU* c, u16 a) {
    (void)c;
    return (u16)(a + 1);
}
int test_flag(CPU* c, int on) {
    (void)c;
    return !on;
}
f64 test_half(CPU* c, f64 x) {
    (void)c;
    return x / 2;
}
f32 test_single(CPU* c, f32 x) {
    (void)c;
    return x * 2;
}
void test_nothing(CPU* c) {
    (void)c;
    calls++;
}
u64 test_eight(CPU* c, u64 a, u64 b, u64 c3, u64 d, u64 e, u64 f, u64 g, u64 h) {
    (void)c;
    return a + b + c3 + d + e + f + g + h;
}
i64 test_sizes(CPU* c, u64 n, i64 d, u64 w) {
    (void)c;
    seen[0] = n, seen[1] = (u64)d, seen[2] = w;
    return d - 1;
}

/* the 32-bit side: the thread's CPU the handlers get, and a stack in a guest memory of its own */
static CPU g_cpu32;
static u8 g_mem32[0x1000];
CPU* hook_a32_cpu(SnxArmCPU* arm) {
    (void)arm;
    return &g_cpu32;
}
static SnxArmBlock fn32(const char* symbol) {
    int i;
    for (i = 0; i < HOOK_COUNT; i++)
        if (!strcmp(hook_symbol[i], symbol)) return hook_a32[i];
    assert(!"no such hook");
    return NULL;
}
/* a call from Thumb code, as a BLX leaves it: lr the return address with bit 0 set, and the adapter must go back to
   it in Thumb state */
static void call32(SnxArmCPU* a, const char* symbol) {
    a->r[14] = 0x8001;
    a->r[15] = 0x4000;
    a->cpsr = 0x10;
    fn32(symbol)(a);
    assert(a->r[15] == 0x8000 && (a->cpsr & SNX_ARM_T));
}
static void put_dword(u32 at, u64 v) {
    snx_st_le32(g_mem32 + at, (u32)v);
    snx_st_le32(g_mem32 + at + 4, (u32)(v >> 32));
}
static u64 bits64(f64 f) {
    u64 b;
    memcpy(&b, &f, sizeof b);
    return b;
}
static u32 bits32(f32 f) {
    u32 b;
    memcpy(&b, &f, sizeof b);
    return b;
}
static void test_a32(void) {
    static SnxArmCPU a;
    int i;
    memset(&a, 0, sizeof a);
    a.mem = g_mem32;
    a.mem_size = sizeof g_mem32;
    a.r[13] = 0x800;
    /* self, a and c2 in r0-r2; b in s0 and d in d1, after it: a double takes the first even pair */
    a.r[0] = 0x1000, a.r[1] = (u32)-5, a.r[2] = 7, a.r[3] = 0xDEAD;
    a.d[0] = 0xAAAAAAAA00000000ull | bits32(1.5f);
    a.d[1] = bits64(2.25);
    call32(&a, "_ZN4test5mixedEifjd");
    assert(seen[0] == 0x1000 && seen[1] == (u64)-5 && seen[2] == 7 && seen_f[0] == 1.5 && seen_f[1] == 2.25);
    assert(a.r[0] == 0xFFFFFFFE && a.r[1] == 0xFFFFFFFF); /* an i64 result in r0 and r1 */
    /* a and b in r0-r3, the other six on the stack, each 8-aligned */
    for (i = 0; i < 4; i++) a.r[i] = i & 1 ? 0 : 1u << (i / 2);
    for (i = 2; i < 8; i++) put_dword(0x800 + 8 * (u32)(i - 2), (u64)1 << i | (u64)1 << (32 + i));
    call32(&a, "eight");
    assert(a.r[0] == 0xFF && a.r[1] == 0xFC);
    /* a size_t zero-extended, a ptrdiff_t sign-extended, each a word; the u64 in the pair r2 and r3 */
    a.r[0] = 0xFFFFFFFF, a.r[1] = 0xFFFFFFFE, a.r[2] = 0x89ABCDEF, a.r[3] = 0x01234567;
    call32(&a, "sizes");
    assert(seen[0] == 0xFFFFFFFFull && (i64)seen[1] == -2 && seen[2] == 0x0123456789ABCDEFull);
    assert(a.r[0] == 0xFFFFFFFD); /* an isize result: its word */
    /* a float result in s0, s1 kept; a double in d0 */
    a.d[0] = 0x5555555500000000ull | bits32(2.5f);
    call32(&a, "single");
    assert(a.d[0] == (0x5555555500000000ull | bits32(5.0f)));
    a.d[0] = bits64(7.0);
    call32(&a, "half");
    assert(a.d[0] == bits64(3.5));
    /* a bool's low byte, extended */
    a.r[0] = 0x100;
    call32(&a, "flag");
    assert(a.r[0] == 1);
}

static GuestFn fn(const char* symbol) {
    int i;
    for (i = 0; i < HOOK_COUNT; i++)
        if (!strcmp(hook_symbol[i], symbol)) return hook_fn[i];
    assert(!"no such hook");
    return NULL;
}
/* a call as a BL leaves it: x30 the return address, and the adapter must go back to it */
static void call(CPU* c, const char* symbol) {
    c->x[30] = 0x81234560;
    c->pc = 0;
    fn(symbol)(c, 0);
    assert(c->pc == 0x81234560);
}
static void set_f64(CPU* c, int r, f64 f) {
    u64 bits;
    memcpy(&bits, &f, sizeof bits);
    memset(&c->v[r], 0xAB, sizeof c->v[r]);
    SNX_VSET_D(c->v[r], 0, bits);
}
static void set_f32(CPU* c, int r, f32 f) {
    u32 bits;
    memcpy(&bits, &f, sizeof bits);
    memset(&c->v[r], 0xAB, sizeof c->v[r]);
    SNX_VSET_S(c->v[r], 0, bits);
}

int main(void) {
    static CPU c;
    int i;
    /* sorted by symbol, for hook.c's binary search */
    for (i = 1; i < HOOK_COUNT; i++) assert(strcmp(hook_symbol[i - 1], hook_symbol[i]) < 0);
    assert(HOOK_COUNT == 10 && hook_symbol[HOOK_COUNT] == NULL);
    /* the family of the lines that follow its family line */
    assert(HOOK_FAMILIES == 2 && !strcmp(hook_family_name[0], "types") && !strcmp(hook_family_name[1], "other"));
    for (i = 0; i < HOOK_COUNT; i++)
        assert(hook_family[i] == (!strncmp(hook_symbol[i], "nothing", 7) || !strncmp(hook_symbol[i], "eight", 5)));

    /* self, a, c2 in x0-x2; b and d in v0 and v1 */
    memset(&c, 0, sizeof c);
    c.x[0] = 0x8000001000ull, c.x[1] = 0xFFFFFFFF00000005ull, c.x[2] = 0x1234567800000007ull;
    set_f32(&c, 0, 1.5f);
    set_f64(&c, 1, -3.25);
    call(&c, "_ZN4test5mixedEifjd");
    assert(seen[0] == 0x8000001000ull && seen[1] == 5 && seen[2] == 7);
    assert(seen_f[0] == 1.5 && seen_f[1] == -3.25);
    assert(c.x[0] == 0xFFFFFFFFFFFFFFFEull);

    /* a narrow argument is the register's low bits; a narrow result is extended to 32 bits, signed or not */
    c.x[0] = 0x1234567800000080ull; /* -128 as an i8 */
    call(&c, "narrow_i8");
    assert(c.x[0] == 0x7F); /* -129 wraps to 127 */
    c.x[0] = 0xFFFFFFFFFFFF0000ull;
    call(&c, "narrow_i8");
    assert(c.x[0] == 0xFFFFFFFFull); /* -1, extended to 32 bits only */
    c.x[0] = 0xAAAAFFFF;
    call(&c, "narrow_u16");
    assert(c.x[0] == 0); /* 0xFFFF + 1 wraps */

    /* a bool is the low byte */
    c.x[0] = 0x100;
    call(&c, "flag");
    assert(c.x[0] == 1);
    c.x[0] = 0x101;
    call(&c, "flag");
    assert(c.x[0] == 0);

    /* a float result is lane 0 of v0, the rest of it zeroed */
    set_f64(&c, 0, 7.0);
    call(&c, "half");
    assert(hook_f64(&c, 0) == 3.5 && SNX_VD(c.v[0], 1) == 0);
    set_f32(&c, 0, 2.5f);
    call(&c, "single");
    assert(hook_f32(&c, 0) == 5.0f && SNX_VS(c.v[0], 1) == 0 && SNX_VD(c.v[0], 1) == 0);

    /* no arguments and no result: x0 stays */
    c.x[0] = 42;
    call(&c, "nothing");
    assert(calls == 1 && c.x[0] == 42);

    /* all eight x registers, and two hooks of one handler */
    for (i = 0; i < 8; i++) c.x[i] = (u64)1 << i;
    call(&c, "eight");
    assert(c.x[0] == 0xFF);
    for (i = 0; i < 8; i++) c.x[i] = (u64)1 << (i + 8);
    call(&c, "eight_again");
    assert(c.x[0] == 0xFF00);

    /* the sizes in x registers as the u64 and i64 they are under AAPCS64 */
    c.x[0] = 0xFFFFFFFFFFull, c.x[1] = (u64)-3, c.x[2] = 9;
    call(&c, "sizes");
    assert(seen[0] == 0xFFFFFFFFFFull && (i64)seen[1] == -3 && seen[2] == 9 && c.x[0] == (u64)-4);

    test_a32();
    puts("hook adapters: ok");
    return 0;
}
