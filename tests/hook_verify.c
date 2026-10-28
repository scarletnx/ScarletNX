/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Verify mode (src/nx/hle/hook/verify.c) on functions of the test's own, standing in for the game's recompiled code:
   the buffers' copies in scratch memory (overlapping ones as one, at their alignment), the function's own code run on
   them and through the dispatch loop when it leaves early, the CPU put back for the handler, and each kind of
   difference: a result, a callee-saved register, a byte. Also the table hookgen makes of tests/hook/verify.hook's
   buffers */
#include "nx/hle/hook/hook.h"
#include "nx/hle/hook/verify.h"
#include "hle/hook/hooks.hook.h"
#include "hle/hook/hooks.hook.inc"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define MEM     (1u << 22)
#define SCRATCH 0x300000u
#define CODE    0x1000u /* the code range the dispatch table covers */

/* the handlers */
u64 t_copy(CPU* c, u64 dst, u64 src, u64 n) {
    memmove(c->mem + dst, c->mem + src, (size_t)n);
    return dst;
}
void t_fill(CPU* c, u64 dst, u32 value, u64 n) {
    u64 i;
    for (i = 0; i < n; i++) snx_st_le32(c->mem + dst + 4 * i, value);
}
u16 t_sum(CPU* c, u64 n, u64 p) {
    u32 s = 0;
    u64 i;
    for (i = 0; i <= n; i++) s += snx_ld_le16(c->mem + p + 2 * i);
    return (u16)s;
}
f64 t_half(CPU* c, f64 x) {
    (void)c;
    return x / 2;
}
u64 t_copy_wrong(CPU* c, u64 dst, u64 src, u64 n) { /* drops the last byte */
    if (n) memmove(c->mem + dst, c->mem + src, (size_t)(n - 1));
    return dst;
}
u64 t_tick(CPU* c) {
    (void)c;
    return 1;
}

/* the functions' own code, as recompiled code behaves: it reads its arguments from the registers, clobbers the
   caller-saved ones, and returns with the pc at x30 */
static int clobber_x19, leave_early;
static void own_copy(CPU* RESTRICT c, u64 entry) { /* memcpy's forward loop: an overlap smears */
    u64 i;
    (void)entry;
    for (i = 0; i < c->x[2]; i++) c->mem[c->x[0] + i] = c->mem[c->x[1] + i];
    c->x[9] = 0xDEAD, c->x[2] = 0;
    memset(&c->v[3], 0x5A, sizeof c->v[3]);
    if (clobber_x19) c->x[19] = 0xBAD;
    c->pc = c->x[30];
}
/* fill, in two pieces: the first returns to the dispatch loop at CODE + 8 as a branch out of a function would */
static void own_fill_rest(CPU* RESTRICT c, u64 entry) {
    u64 i;
    (void)entry;
    for (i = c->x[2] / 2; i < c->x[2]; i++) snx_st_le32(c->mem + c->x[0] + 4 * i, (u32)c->x[1]);
    c->pc = c->x[30];
}
static void own_fill(CPU* RESTRICT c, u64 entry) {
    u64 i;
    (void)entry;
    for (i = 0; i < c->x[2] / 2; i++) snx_st_le32(c->mem + c->x[0] + 4 * i, (u32)c->x[1]);
    if (leave_early) {
        c->pc = CODE + 8;
        return;
    }
    own_fill_rest(c, CODE + 8);
}
static void own_sum(CPU* RESTRICT c, u64 entry) {
    u64 i, s = 0x50000; /* bits above the u16 result, which no caller reads */
    (void)entry;
    for (i = 0; i <= c->x[0]; i++) s += snx_ld_le16(c->mem + c->x[1] + 2 * i);
    c->x[0] = s;
    c->pc = c->x[30];
}
static void own_half(CPU* RESTRICT c, u64 entry) {
    f64 x = hook_f64(c, 0) * 0.5;
    u64 bits;
    (void)entry;
    memcpy(&bits, &x, sizeof bits);
    SNX_VSET_D(c->v[0], 0, bits);
    SNX_VSET_D(c->v[0], 1, 0x1234); /* the upper half, which isn't the result */
    c->pc = c->x[30];
}

static int index_of(const char* symbol) {
    int i;
    for (i = 0; i < HOOK_COUNT; i++)
        if (!strcmp(hook_symbol[i], symbol)) return i;
    assert(!"no such hook");
    return -1;
}

static CPU c;
static HookVerifyResult r;

/* a call of symbol from 0x81234560 with the stack at 0x200000, checked by verify mode against own */
static int verify(const char* symbol, GuestFn own) {
    int k = index_of(symbol), what;
    c.x[30] = 0x81234560, c.x[31] = 0x200000, c.x[19] = 0x1919, c.pc = 0;
    memset(c.mem + SCRATCH, 0xEE, 0x10000);
    what = hook_verify_call(&c, 0x2000, own, hook_fn[k], &hook_verify[k], SCRATCH, 0x10000, &r);
    assert(what == r.what);
    if (what < HOOK_SKIP_SIZE) assert(c.pc == 0x81234560 && c.x[19] == 0x1919 && c.x[31] == 0x200000);
    return what;
}

int main(void) {
    static GuestFn disp[64];
    u8* mem = calloc(MEM, 1);
    int i, k;
    assert(mem);
    c.mem = mem;
    c.disp = disp;
    c.code_lo = CODE, c.code_hi = CODE + sizeof disp / sizeof *disp * 4;
    disp[2] = own_fill_rest;

    /* the table: each pure hook's buffers (register, use, the length's register, factor and addend) and result */
    k = index_of("copy");
    assert(hook_verify[k].pure && hook_verify[k].ret == HOOK_RET_ADDR && hook_verify[k].nbufs == 2);
    assert(hook_verify[k].buf[0].reg == 0 && hook_verify[k].buf[0].mode == HOOK_OUT && hook_verify[k].buf[0].len_reg == 2 &&
           hook_verify[k].buf[0].len_mul == 1 && hook_verify[k].buf[0].len_add == 0);
    assert(hook_verify[k].buf[1].reg == 1 && hook_verify[k].buf[1].mode == HOOK_IN);
    k = index_of("fill");
    assert(hook_verify[k].ret == HOOK_RET_VOID && hook_verify[k].buf[0].len_mul == 4);
    k = index_of("sum");
    assert(hook_verify[k].ret == HOOK_RET_X16 && hook_verify[k].buf[0].reg == 1 && hook_verify[k].buf[0].len_reg == 0 &&
           hook_verify[k].buf[0].len_mul == 2 && hook_verify[k].buf[0].len_add == 2);
    assert(hook_verify[index_of("half")].ret == HOOK_RET_F64 && !hook_verify[index_of("half")].nbufs);
    assert(!hook_verify[index_of("tick")].pure);

    /* a copy between separate buffers agrees, its result (the copy's address in the function's own run) mapped back
       to the live buffer, and the live buffer holds the handler's bytes */
    for (i = 0; i < 100; i++) mem[0x10003 + i] = (u8)(i * 7);
    memset(mem + 0x20005, 0x11, 100);
    c.x[0] = 0x20005, c.x[1] = 0x10003, c.x[2] = 100;
    assert(verify("copy", own_copy) == HOOK_AGREE);
    assert(c.x[0] == 0x20005 && !memcmp(mem + 0x20005, mem + 0x10003, 100));
    assert(c.x[2] == 100 && c.x[9] == 0); /* the handler's call: the own code's clobbers are gone */
    /* its copies kept the buffers' alignment modulo 64 */
    assert(r.what == HOOK_AGREE);
    for (i = 0; i < 64 && mem[SCRATCH + i] == 0xEE; i++) {
    }
    assert(i == 0x05 || i == 0x03);

    /* overlapping buffers are one copy, so memcpy's forward loop smears where memmove doesn't: the bytes differ */
    for (i = 0; i < 64; i++) mem[0x30000 + i] = (u8)i;
    c.x[0] = 0x30001, c.x[1] = 0x30000, c.x[2] = 32;
    assert(verify("copy", own_copy) == HOOK_DIFFER_BYTES && r.offset == 1);
    for (i = 0; i < 32; i++) assert(mem[0x30001 + i] == (u8)i); /* the live memory is the handler's memmove */

    /* a handler that writes one byte too few */
    c.x[0] = 0x20005, c.x[1] = 0x10003, c.x[2] = 100;
    memset(mem + 0x20005, 0, 100);
    assert(verify("wrong", own_copy) == HOOK_DIFFER_BYTES && r.buf == 0 && r.offset == 99);

    /* the function's own code leaving a callee-saved register changed */
    clobber_x19 = 1;
    c.x[0] = 0x20005, c.x[1] = 0x10003, c.x[2] = 16;
    assert(verify("copy", own_copy) == HOOK_DIFFER_REG && r.reg == 19 && r.b == 0xBAD);
    clobber_x19 = 0;

    /* a length times a factor; the function's own code going on through the dispatch loop */
    memset(mem + 0x40000, 0, 64);
    c.x[0] = 0x40000, c.x[1] = 0xA5A5A5A5, c.x[2] = 10;
    leave_early = 1;
    assert(verify("fill", own_fill) == HOOK_AGREE);
    leave_early = 0;
    for (i = 0; i < 10; i++) assert(snx_ld_le32(mem + 0x40000 + 4 * i) == 0xA5A5A5A5);
    assert(mem[0x40000 + 40] == 0);

    /* a narrow result compares its own bits only; a length with an addend */
    for (i = 0; i < 9; i++) snx_st_le16(mem + 0x50000 + 2 * i, (u16)(1000 * i));
    c.x[0] = 8, c.x[1] = 0x50000;
    assert(verify("sum", own_sum) == HOOK_AGREE && c.x[0] == 36000);

    /* a float result: lane 0 of v0 */
    memset(&c.v[0], 0, sizeof c.v[0]);
    SNX_VSET_D(c.v[0], 0, 0x4024000000000000ull); /* 10.0 */
    assert(verify("half", own_half) == HOOK_AGREE && hook_f64(&c, 0) == 5.0);

    /* buffers larger than the scratch memory: nothing runs, the caller runs the handler */
    memset(mem + 0x20005, 0x77, 16);
    c.x[0] = 0x20005, c.x[1] = 0x100000, c.x[2] = 0x20000;
    assert(verify("copy", own_copy) == HOOK_SKIP_SIZE && mem[0x20005] == 0x77 && c.x[0] == 0x20005);
    /* a length past the address space */
    c.x[0] = 0x20005, c.x[1] = 0x100000, c.x[2] = ~0ull - 4;
    assert(verify("copy", own_copy) == HOOK_SKIP_RANGE);

    free(mem);
    puts("hook verify: ok");
    return 0;
}
