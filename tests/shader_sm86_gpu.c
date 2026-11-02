/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The SM86 translator (src/nx/gpu/shader/sm86_translate.c) on a Vulkan device against the CPU reference
   (tests/support/sm86_ref.c): three synthetic compute programs, the integer, float and control flow instructions of
   the captured compute programs, run on 64 invocations of chosen inputs, every output word compared. It takes a CPU
   device (lavapipe) when the loader has one, else the first device. Opt-in with SNX_TEST_VULKAN, as vulkan_headless.
   No word here comes from a game. */
#include "nx/gpu/shader/sm86.h"
#include "support/sm86_ref.h"
#include <volk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL       32u
#define GROUPS      2u
#define INVOCATIONS (LOCAL * GROUPS)
#define MAX_OUT     32u
#define VA_IN       0x100000000ull
#define VA_OUT      0x200001000ull
#define FILL        0xdeadbeefu

/* ---- the programs ---- */

typedef struct {
    u8 code[16 * 256];
    u32 n;
    u32 outputs;     /* words each invocation writes */
    u32 approx_from; /* the outputs from here on are MUFU's, which a tolerance compares */
    u32 denorm_mask; /* the outputs of float instructions without FTZ, which a subnormal input makes the device's
                        own (the module declares no DenormPreserve, as spirv_translate.c's do not) */
} Prog;
static u32 emit(Prog* p, Sm86Word w) {
    sm86_put(p->code, p->n, w);
    return p->n++;
}
static Sm86Word alu(u32 op9, u32 form, u32 dst, u32 a) { return sm86_alu(op9, form, dst, a); }
static void f(Sm86Word* w, unsigned lo, unsigned n, u64 v) { sm86_set(w, lo, n, v); }
/* the second source: a register at 32 (form 1) */
static Sm86Word rr(u32 op9, u32 dst, u32 a, u32 b) {
    Sm86Word w = alu(op9, 1, dst, a);
    f(&w, 32, 8, b);
    f(&w, 64, 8, 255);
    return w;
}
/* an immediate second source (form 4), c a register at 64 */
static Sm86Word ri(u32 op9, u32 dst, u32 a, u32 imm, u32 c) {
    Sm86Word w = alu(op9, 4, dst, a);
    f(&w, 32, 32, imm);
    f(&w, 64, 8, c);
    return w;
}
/* R0 = TID.X, R1 = CTAID.X, R2 = the invocation, UR4:UR5 and UR6:UR7 the buffers' addresses from c[0][0x20] and
   c[0][0x28], R4:R5 = the invocation's two input words, R6 = the byte offset of its outputs */
static void prologue(Prog* p) {
    Sm86Word w = sm86_fix(0x919);
    f(&w, 72, 8, 0x21);
    emit(p, w);
    w = sm86_fix(0x919);
    f(&w, 16, 8, 1);
    f(&w, 72, 8, 0x25);
    emit(p, w);
    emit(p, ri(0x024, 2, 1, LOCAL, 0)); /* IMAD R2, R1, 32, R0 */
    for (u32 k = 0; k < 2; k++) {
        w = sm86_fix(0xab9); /* ULDC.64 */
        f(&w, 16, 6, 4 + 2 * k);
        sm86_cbuf(&w, 0, 0x20 + 8 * k);
        f(&w, 73, 3, 5);
        emit(p, w);
    }
    w = rr(0x011, 3, 2, 255); /* LEA R3, R2, RZ, 3 */
    f(&w, 75, 5, 3);
    f(&w, 81, 3, 7);
    emit(p, w);
    w = sm86_fix(0x981); /* LDG.E.64 R4, [R3.U32 + UR4] */
    f(&w, 16, 8, 4);
    f(&w, 24, 8, 3);
    f(&w, 32, 6, 4);
    f(&w, 72, 1, 1);
    f(&w, 73, 3, 5);
    f(&w, 81, 3, 7);
    f(&w, 84, 3, 1);
    f(&w, 91, 1, 1);
    emit(p, w);
    emit(p, ri(0x024, 6, 2, p->outputs * 4, 255)); /* IMAD R6, R2, outputs * 4, RZ */
}
static void store(Prog* p, u32 k, u32 reg) {
    Sm86Word w = sm86_fix(0x986); /* STG.E [R6.U32 + UR6 + 4k], reg */
    f(&w, 24, 8, 6);
    f(&w, 32, 8, reg);
    f(&w, 40, 24, 4 * k);
    f(&w, 64, 6, 6);
    f(&w, 72, 1, 1);
    f(&w, 73, 3, 4);
    f(&w, 84, 3, 1);
    f(&w, 91, 1, 1);
    emit(p, w);
}
static void epilogue(Prog* p) {
    emit(p, sm86_exit());
    emit(p, sm86_bra(p->n, p->n, 7, 0));
}
/* the integer instructions: IADD3 (negated, carries, .X), IMAD, LOP3, SHF, LEA.HI, IMNMX, ISETP, SEL, IABS, R2P,
   PLOP3 */
static void integer_program(Prog* p) {
    p->outputs = 15;
    p->approx_from = 15;
    prologue(p);
    Sm86Word w = alu(0x010, 2, 8, 4); /* IADD3 R8, R4, -R5, 0x1234 */
    f(&w, 64, 8, 5);
    f(&w, 75, 1, 1);
    f(&w, 32, 32, 0x1234);
    f(&w, 81, 6, 0x3f);
    f(&w, 87, 4, 0xf);
    f(&w, 77, 4, 0xf);
    emit(p, w);
    w = rr(0x010, 9, 4, 5); /* IADD3 R9, P0, P1, R4, R5, R5 */
    f(&w, 64, 8, 5);
    f(&w, 81, 3, 0);
    f(&w, 84, 3, 1);
    f(&w, 87, 4, 0xf);
    f(&w, 77, 4, 0xf);
    emit(p, w);
    w = rr(0x010, 10, 255, 255); /* IADD3.X R10, RZ, RZ, RZ, P0, P1 */
    f(&w, 74, 1, 1);
    f(&w, 87, 4, 0);
    f(&w, 77, 4, 1);
    f(&w, 81, 6, 0x3f);
    emit(p, w);
    w = rr(0x024, 11, 4, 5); /* IMAD R11, R4, R5, R4 */
    f(&w, 64, 8, 4);
    emit(p, w);
    for (u32 k = 0; k < 2; k++) { /* LOP3.LUT R12, R4, R5, imm, 0x96 (xor3) and 0xe8 (majority) */
        w = alu(0x012, 2, 12 + k, 4);
        f(&w, 64, 8, 5);
        f(&w, 32, 32, k ? 0x00ff00ffu : 0x0f0f0f0fu);
        f(&w, 72, 8, k ? 0xe8 : 0x96);
        f(&w, 81, 3, 7);
        f(&w, 87, 4, 0xf);
        emit(p, w);
    }
    w = rr(0x019, 14, 4, 5); /* SHF.L.W.U32 R14, R4, R5, RZ */
    f(&w, 73, 2, 3);
    f(&w, 75, 1, 1);
    emit(p, w);
    w = ri(0x019, 15, 255, 5, 4); /* SHF.R.S32.HI R15, RZ, 5, R4 */
    f(&w, 73, 2, 2);
    f(&w, 76, 1, 1);
    f(&w, 80, 1, 1);
    emit(p, w);
    w = rr(0x011, 16, 4, 5); /* LEA.HI R16, R4, R5, R5, 4 */
    f(&w, 64, 8, 5);
    f(&w, 75, 5, 4);
    f(&w, 80, 1, 1);
    f(&w, 81, 3, 7);
    emit(p, w);
    w = rr(0x017, 17, 4, 5); /* IMNMX R17, R4, R5, PT (signed min) */
    f(&w, 73, 1, 1);
    f(&w, 87, 4, 7);
    emit(p, w);
    w = rr(0x017, 18, 4, 5); /* IMNMX.U32 R18, R4, R5, !PT (max) */
    f(&w, 87, 4, 0xf);
    emit(p, w);
    w = rr(0x00c, 0, 4, 5); /* ISETP.LT.AND P2, PT, R4, R5, PT */
    f(&w, 73, 1, 1);
    f(&w, 76, 3, 1);
    f(&w, 81, 3, 2);
    f(&w, 84, 3, 7);
    f(&w, 87, 4, 7);
    f(&w, 68, 4, 7);
    emit(p, w);
    w = rr(0x007, 19, 4, 5); /* SEL R19, R4, R5, P2 */
    f(&w, 87, 4, 2);
    emit(p, w);
    emit(p, rr(0x013, 20, 255, 4)); /* IABS R20, R4 */
    w = ri(0x004, 0, 5, 0x7f, 255); /* R2P PR, R5.B1, 0x7f */
    f(&w, 76, 2, 1);
    emit(p, w);
    w = ri(0x007, 21, 4, 0x44, 255); /* SEL R21, R4, 0x44, P3 */
    f(&w, 87, 4, 3);
    emit(p, w);
    w = sm86_fix(0x81c); /* PLOP3.LUT P5, PT, P0, P1, P6, 0xe8, 0 */
    f(&w, 87, 4, 0);
    f(&w, 77, 4, 1);
    f(&w, 68, 4, 6);
    f(&w, 64, 3, 0xe8 & 7);
    f(&w, 72, 5, 0xe8 >> 3);
    f(&w, 16, 8, 0);
    f(&w, 81, 3, 5);
    f(&w, 84, 3, 7);
    emit(p, w);
    w = rr(0x007, 22, 5, 4); /* SEL R22, R5, R4, P5 */
    f(&w, 87, 4, 5);
    emit(p, w);
    for (u32 k = 0; k < p->outputs; k++) store(p, k, 8 + k);
    epilogue(p);
}
/* the float instructions: FADD, FMUL and FFMA with the modifiers, the rounding modes, FTZ, .SAT and .D2, FMNMX,
   FSETP, FSEL, FSET, F2I, I2FP, FRND, then MUFU */
static void float_program(Prog* p) {
    p->outputs = 26;
    p->approx_from = 20;
    p->denorm_mask = 0x3ffffffu & ~(7u << 15) & ~(3u << 8); /* not I2FP, FADD.FTZ or FMUL.FTZ */
    prologue(p);
    Sm86Word w = rr(0x021, 8, 4, 5); /* FADD R8, R4, -|R5| */
    f(&w, 62, 2, 3);
    emit(p, w);
    static const u8 modes[3] = {3, 1, 2}; /* FADD.RZ, .RM, .RP */
    for (u32 k = 0; k < 3; k++) {
        w = rr(0x021, 9 + k, 4, 5);
        f(&w, 78, 2, modes[k]);
        emit(p, w);
    }
    w = rr(0x020, 12, 4, 5); /* FMUL.RZ R12, R4, R5 */
    f(&w, 78, 2, 3);
    f(&w, 84, 3, 4);
    emit(p, w);
    w = rr(0x020, 13, 4, 5); /* FMUL.D2 R13, R4, R5 */
    f(&w, 84, 3, 3);
    emit(p, w);
    w = alu(0x023, 2, 14, 4); /* FFMA R14, R4, R5, 1.0 */
    f(&w, 64, 8, 5);
    f(&w, 32, 32, 0x3f800000u);
    emit(p, w);
    w = rr(0x023, 15, 4, 5); /* FFMA.RP R15, R4, R5, R4 */
    f(&w, 64, 8, 4);
    f(&w, 78, 2, 2);
    emit(p, w);
    w = rr(0x021, 16, 4, 5); /* FADD.FTZ R16, R4, R5 */
    f(&w, 80, 1, 1);
    emit(p, w);
    w = rr(0x020, 17, 4, 5); /* FMUL.FTZ.SAT R17, R4, R5 */
    f(&w, 77, 1, 1);
    f(&w, 80, 1, 1);
    f(&w, 84, 3, 4);
    emit(p, w);
    w = rr(0x009, 18, 4, 5); /* FMNMX R18, R4, R5, PT */
    f(&w, 87, 4, 7);
    emit(p, w);
    w = rr(0x00b, 0, 4, 5); /* FSETP.GTU.AND P0, PT, R4, R5, PT */
    f(&w, 76, 4, 12);
    f(&w, 81, 3, 0);
    f(&w, 84, 3, 7);
    f(&w, 87, 4, 7);
    emit(p, w);
    w = rr(0x008, 19, 4, 5); /* FSEL R19, R4, R5, P0 */
    f(&w, 87, 4, 0);
    emit(p, w);
    w = rr(0x00a, 20, 4, 5); /* FSET.BF.LT.AND R20, R4, R5, PT */
    f(&w, 76, 4, 1);
    f(&w, 87, 4, 7);
    emit(p, w);
    for (u32 k = 0; k < 2; k++) { /* F2I.TRUNC (signed), F2I.U32.FLOOR */
        w = rr(0x105, 21 + k, 255, 4);
        f(&w, 72, 1, k ? 0 : 1);
        f(&w, 75, 2, 2);
        f(&w, 78, 2, k ? 1 : 3);
        f(&w, 84, 2, 2);
        emit(p, w);
    }
    static const u8 i2f_signed[3] = {1, 0, 1}, i2f_mode[3] = {0, 3, 2}; /* .S32, .U32.RZ, .S32.RP */
    for (u32 k = 0; k < 3; k++) {
        w = rr(0x045, 23 + k, 255, 5);
        f(&w, 74, 1, i2f_signed[k]);
        f(&w, 75, 2, 2);
        f(&w, 78, 2, i2f_mode[k]);
        f(&w, 84, 2, 2);
        emit(p, w);
    }
    for (u32 k = 0; k < 2; k++) { /* FRND.FLOOR, FRND */
        w = rr(0x107, 26 + k, 255, 4);
        f(&w, 75, 2, 2);
        f(&w, 78, 2, k ? 0 : 1);
        f(&w, 84, 2, 2);
        emit(p, w);
    }
    /* MUFU: RCP R5, SQRT |R4|, RSQ |R4|, EX2 R4, LG2 |R5|, and SIN of R4 clamped to half a turn */
    static const u8 fn[5] = {4, 8, 5, 2, 3}, src_reg[5] = {5, 4, 4, 4, 5}, abs_bit[5] = {0, 1, 1, 0, 1};
    for (u32 k = 0; k < 5; k++) {
        w = rr(0x108, 28 + k, 255, src_reg[k]);
        f(&w, 62, 1, abs_bit[k]);
        f(&w, 74, 6, fn[k]);
        emit(p, w);
    }
    w = ri(0x009, 33, 4, 0x3f000000u, 255); /* FMNMX R33, R4, 0.5, PT */
    f(&w, 87, 4, 7);
    emit(p, w);
    w = ri(0x009, 33, 33, 0xbf000000u, 255); /* FMNMX R33, R33, -0.5, !PT */
    f(&w, 87, 4, 0xf);
    emit(p, w);
    w = rr(0x108, 34, 255, 33); /* MUFU.SIN R34, R33 */
    f(&w, 74, 6, 1);
    emit(p, w);
    for (u32 k = 0; k < 20; k++) store(p, k, 8 + k);
    for (u32 k = 20; k < 26; k++) store(p, k, 28 + (k - 20) + (k == 25)); /* R28-R32, then R34 */
    epilogue(p);
}
/* control flow: a loop that BRA closes, a BSSY and BSYNC around an if, CS2R, shared memory through STS and LDS with
   a scaled offset, and an EXIT that some invocations take */
static void flow_program(Prog* p) {
    p->outputs = 3;
    p->approx_from = 3;
    prologue(p);
    Sm86Word w = ri(0x012, 7, 4, 15, 255); /* LOP3 R7 = R4 & 15 (0xc0) */
    f(&w, 72, 8, 0xc0);
    f(&w, 81, 3, 7);
    f(&w, 87, 4, 0xf);
    emit(p, w);
    w = sm86_fix(0x805); /* CS2R R8, SRZ */
    f(&w, 16, 8, 8);
    f(&w, 72, 8, 0xff);
    emit(p, w);
    w = ri(0x002, 9, 255, 0, 255); /* MOV R9, 0 */
    f(&w, 72, 4, 0xf);
    emit(p, w);
    u32 head = p->n;
    w = rr(0x00c, 0, 9, 7); /* ISETP.GE.U32.AND P0, PT, R9, R7, PT */
    f(&w, 76, 3, 6);
    f(&w, 81, 3, 0);
    f(&w, 84, 3, 7);
    f(&w, 87, 4, 7);
    f(&w, 68, 4, 7);
    emit(p, w);
    u32 exit_branch = emit(p, sm86_fix(0x918)); /* @P0 BRA past the loop, patched below */
    w = rr(0x024, 8, 9, 5);                     /* IMAD R8, R9, R5, R8 */
    f(&w, 64, 8, 8);
    emit(p, w);
    w = ri(0x010, 9, 9, 1, 255); /* IADD3 R9, R9, 1, RZ */
    f(&w, 81, 6, 0x3f);
    f(&w, 87, 4, 0xf);
    f(&w, 77, 4, 0xf);
    emit(p, w);
    emit(p, sm86_bra(p->n, head, 7, 0));
    sm86_put(p->code, exit_branch, sm86_bra(exit_branch, p->n, 0, 0));
    store(p, 0, 8);
    u32 bssy = emit(p, sm86_fix(0x918)); /* BSSY B0 to the BSYNC, patched below */
    w = ri(0x012, 10, 5, 1, 255);        /* LOP3 P1, R10 = R5 & 1 */
    f(&w, 72, 8, 0xc0);
    f(&w, 81, 3, 1);
    f(&w, 87, 4, 0xf);
    emit(p, w);
    u32 skip = emit(p, sm86_fix(0x918)); /* @!P1 BRA to the BSYNC, patched below */
    w = ri(0x010, 8, 8, 0x100, 255);     /* IADD3 R8, R8, 0x100, RZ */
    f(&w, 81, 6, 0x3f);
    f(&w, 87, 4, 0xf);
    f(&w, 77, 4, 0xf);
    emit(p, w);
    w = sm86_fix(0x941); /* BSYNC B0 */
    f(&w, 87, 3, 7);
    u32 bsync = emit(p, w);
    sm86_put(p->code, skip, sm86_bra(skip, bsync, 1, 1));
    w = sm86_fix(0x945);
    f(&w, 34, 30, (u64)(bsync - bssy - 1) * 4);
    f(&w, 87, 3, 7);
    sm86_put(p->code, bssy, w);
    w = sm86_fix(0x388); /* STS [R0.X4], R8 */
    f(&w, 32, 8, 8);
    f(&w, 73, 3, 4);
    f(&w, 78, 2, 1);
    emit(p, w);
    w = sm86_fix(0x984); /* LDS R11, [R0.X4 + URZ] */
    f(&w, 16, 8, 11);
    f(&w, 32, 6, 63);
    f(&w, 73, 3, 4);
    f(&w, 78, 2, 1);
    f(&w, 91, 1, 1);
    emit(p, w);
    store(p, 1, 11);
    w = ri(0x012, 12, 5, 2, 255); /* LOP3 P2, R12 = R5 & 2 */
    f(&w, 72, 8, 0xc0);
    f(&w, 81, 3, 2);
    f(&w, 87, 4, 0xf);
    emit(p, w);
    w = sm86_exit(); /* @P2 EXIT */
    f(&w, 12, 3, 2);
    emit(p, w);
    store(p, 2, 4);
    epilogue(p);
}

/* global memory through register pairs, as the game's compute programs address it: MOV of the buffers' addresses
   from c[0], IMAD.WIDE to the invocation's words, LDG and STG without a uniform register, and IMAD.HI, SGXT and
   PRMT on the words */
static void wide_program(Prog* p) {
    p->outputs = 3;
    p->approx_from = 3;
    Sm86Word w = sm86_fix(0x919);
    f(&w, 72, 8, 0x21);
    emit(p, w);
    w = sm86_fix(0x919);
    f(&w, 16, 8, 1);
    f(&w, 72, 8, 0x25);
    emit(p, w);
    emit(p, ri(0x024, 2, 1, LOCAL, 0)); /* IMAD R2, R1, 32, R0 */
    for (u32 k = 0; k < 4; k++) {       /* MOV R8, R9, R18, R19 = c[0][0x20], [0x24], [0x28], [0x2c] */
        w = alu(0x002, 5, k < 2 ? 8 + k : 16 + k, 255);
        sm86_cbuf(&w, 0, 0x20 + 4 * k);
        f(&w, 72, 4, 0xf);
        emit(p, w);
    }
    emit(p, ri(0x025, 10, 2, 8, 8)); /* IMAD.WIDE.U32 R10, R2, 8, R8 */
    w = sm86_fix(0x381);             /* LDG.E.64 R12, [R10.64] */
    f(&w, 16, 8, 12);
    f(&w, 24, 8, 10);
    f(&w, 72, 1, 1);
    f(&w, 73, 3, 5);
    f(&w, 81, 3, 7);
    f(&w, 84, 3, 1);
    emit(p, w);
    emit(p, rr(0x027, 14, 12, 13)); /* IMAD.HI.U32 R14, R12, R13, RZ */
    w = ri(0x01a, 15, 12, 12, 255); /* SGXT R15, R12, 12 */
    f(&w, 73, 1, 1);
    emit(p, w);
    emit(p, ri(0x016, 16, 12, 0x7051, 13));        /* PRMT R16, R12, 0x7051, R13 */
    emit(p, ri(0x025, 20, 2, p->outputs * 4, 18)); /* IMAD.WIDE.U32 R20, R2, 12, R18 */
    for (u32 k = 0; k < 3; k++) {
        w = sm86_fix(0x386); /* STG.E [R20.64 + 4k], R14 + k */
        f(&w, 24, 8, 20);
        f(&w, 32, 8, 14 + k);
        f(&w, 40, 24, 4 * k);
        f(&w, 72, 1, 1);
        f(&w, 73, 3, 4);
        f(&w, 84, 3, 1);
        emit(p, w);
    }
    epilogue(p);
}

/* ---- the inputs and the comparison ---- */

static void inputs(u32* in) {
    static const u32 special[] = {
        0x00000000u, 0x00000001u, 0xffffffffu, 0x7fffffffu, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x40200000u,
        0x00800000u, 0x007fffffu, 0x00000010u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7f7fffffu,
        0x3dcccccdu, 0x3eaaaaabu, 0x4b800001u, 0xcf000000u, 0x4f800000u, 0x12345678u, 0x80000001u, 0x3effffffu,
        0x00000003u, 0x0000000fu, 0x00010000u, 0x447a0000u, 0xc2c80000u, 0x3a83126fu, 0x5f000000u, 0x2f800000u};
    u32 count = (u32)(sizeof special / sizeof *special), seed = 0x9e3779b9u;
    for (u32 i = 0; i < 2 * INVOCATIONS; i++) {
        seed = seed * 1664525u + 1013904223u;
        u32 a = special[(i / 2 + i * 7) % count];
        /* pairs of specials, pairs of nearby floats, and random words */
        in[i] = i < 2 * count ? a : i < 3 * count ? 0x3f800000u + (seed >> 9) : seed;
    }
}
static int is_nan(u32 v) { return (v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu); }
static int is_subnormal(u32 v) { return !(v & 0x7f800000u) && (v & 0x7fffffu); }
static int close_enough(u32 got, u32 want) {
    float g, w;
    memcpy(&g, &got, 4);
    memcpy(&w, &want, 4);
    if (got == want || (isnan(g) && isnan(w))) return 1;
    if (isinf(w) || fabsf(w) > 1e37f) return (isinf(g) || fabsf(g) > 1e37f) && signbit(g) == signbit(w);
    if (fabsf(w) < 1e-30f) return fabsf(g) < 1e-30f;
    /* Vulkan's bounds for these: 2.5 ULP for 1/x, 2 for inversesqrt, 3 + 2|x| for exp2, 2^-21 absolute for log2
       near 1. The bound here takes all of them */
    return fabsf(g - w) <= fabsf(w) * 1e-4f + 1e-5f;
}
static int close_sin(u32 got, u32 want) {
    float g, w;
    memcpy(&g, &got, 4);
    memcpy(&w, &want, 4);
    return (isnan(g) && isnan(w)) || fabsf(g - w) <= 1.0f / 2048.0f; /* Vulkan's sin bound, 2^-11 absolute */
}

/* ---- Vulkan ---- */

typedef struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    u32 family;
    VkPhysicalDeviceMemoryProperties mem;
    char name[256];
} Gpu;
static int gpu_open(Gpu* g) {
    if (volkInitialize() != VK_SUCCESS) return 0;
    VkApplicationInfo app = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "snx-test-shader-sm86-gpu", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &g->instance) != VK_SUCCESS) return 0;
    volkLoadInstance(g->instance);
    u32 count = 0;
    vkEnumeratePhysicalDevices(g->instance, &count, NULL);
    VkPhysicalDevice devices[16];
    if (count > 16) count = 16;
    vkEnumeratePhysicalDevices(g->instance, &count, devices);
    if (!count) return 0;
    g->phys = devices[0];
    for (u32 i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) g->phys = devices[i];
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g->phys, &props);
    snprintf(g->name, sizeof g->name, "%s", props.deviceName);
    if (props.apiVersion < VK_API_VERSION_1_3) return 0;
    vkGetPhysicalDeviceMemoryProperties(g->phys, &g->mem);
    VkQueueFamilyProperties families[16];
    u32 nf = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &nf, families);
    g->family = 0xffffffffu;
    for (u32 i = 0; i < nf && g->family == 0xffffffffu; i++)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) g->family = i;
    if (g->family == 0xffffffffu) return 0;
    VkPhysicalDeviceFeatures features = {0};
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(g->phys, &have);
    features.shaderFloat64 = have.shaderFloat64;
    features.shaderInt64 = have.shaderInt64;
    VkPhysicalDeviceVulkan13Features v13 = {0};
    v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    v13.shaderDemoteToHelperInvocation = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v13, features};
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, g->family, 1, &priority};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(g->phys, &dci, NULL, &g->device) != VK_SUCCESS) return 0;
    volkLoadDevice(g->device);
    vkGetDeviceQueue(g->device, g->family, 0, &g->queue);
    return have.shaderFloat64 && have.shaderInt64;
}
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* map;
    u32 bytes;
} Buf;
static int buf_make(Gpu* g, Buf* b, u32 bytes, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bci = {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g->device, &bci, NULL, &b->buffer) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g->device, b->buffer, &req);
    u32 type = 0xffffffffu;
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (u32 i = 0; i < g->mem.memoryTypeCount && type == 0xffffffffu; i++)
        if ((req.memoryTypeBits & (1u << i)) && (g->mem.memoryTypes[i].propertyFlags & want) == want) type = i;
    if (type == 0xffffffffu) return 0;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (vkAllocateMemory(g->device, &mai, NULL, &b->memory) != VK_SUCCESS) return 0;
    vkBindBufferMemory(g->device, b->buffer, b->memory, 0);
    if (vkMapMemory(g->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) return 0;
    b->bytes = bytes;
    memset(b->map, 0, bytes);
    return 1;
}
static void buf_free(Gpu* g, Buf* b) {
    if (b->memory) vkFreeMemory(g->device, b->memory, NULL);
    if (b->buffer) vkDestroyBuffer(g->device, b->buffer, NULL);
}
/* Runs the module with cbuf 0 and the global buffers that si names (each a buffer of the bytes at in or out): 0 when
   Vulkan fails */
static int gpu_run(Gpu* g, const u32* spv, size_t words, const ShaderInfo* si, const u32* cb0, const u32* in,
                   u32 in_bytes, u32* out, u32 out_bytes) {
    Buf cb = {0}, gb[2] = {{0}};
    int ok = buf_make(g, &cb, 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (ok) memcpy(cb.map, cb0, 256);
    for (int k = 0; k < si->ngbuf && ok; k++) {
        int is_out = si->gbuf_cbuf_off[k] == 0x28;
        ok = buf_make(g, &gb[k], is_out ? out_bytes : in_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (ok) memcpy(gb[k].map, is_out ? (const void*)out : (const void*)in, is_out ? out_bytes : in_bytes);
    }
    VkDescriptorSetLayoutBinding bindings[3];
    u32 nb = 0;
    bindings[nb++] = (VkDescriptorSetLayoutBinding){SNX_VKB_CB, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                    VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    for (int k = 0; k < si->ngbuf; k++)
        bindings[nb++] = (VkDescriptorSetLayoutBinding){SNX_VKB_SSBO + (u32)k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                        VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkShaderModule sm = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSetLayoutCreateInfo dlci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, nb, bindings};
    ok = ok && vkCreateDescriptorSetLayout(g->device, &dlci, NULL, &dsl) == VK_SUCCESS;
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
    ok = ok && vkCreatePipelineLayout(g->device, &plci, NULL, &pl) == VK_SUCCESS;
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, words * 4, spv};
    ok = ok && vkCreateShaderModule(g->device, &smci, NULL, &sm) == VK_SUCCESS;
    VkComputePipelineCreateInfo cpci = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        NULL,
        0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL},
        pl,
        VK_NULL_HANDLE,
        0};
    ok = ok && vkCreateComputePipelines(g->device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe) == VK_SUCCESS;
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, sizes};
    ok = ok && vkCreateDescriptorPool(g->device, &dpci, NULL, &pool) == VK_SUCCESS;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &dsl};
    ok = ok && vkAllocateDescriptorSets(g->device, &dsai, &set) == VK_SUCCESS;
    if (ok) {
        VkDescriptorBufferInfo infos[3];
        VkWriteDescriptorSet writes[3];
        for (u32 k = 0; k < nb; k++) {
            infos[k] = (VkDescriptorBufferInfo){k ? gb[k - 1].buffer : cb.buffer, 0, VK_WHOLE_SIZE};
            writes[k] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                               NULL,
                                               set,
                                               bindings[k].binding,
                                               0,
                                               1,
                                               bindings[k].descriptorType,
                                               NULL,
                                               &infos[k],
                                               NULL};
        }
        vkUpdateDescriptorSets(g->device, nb, writes, 0, NULL);
    }
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, g->family};
    ok = ok && vkCreateCommandPool(g->device, &cpi, NULL, &cpool) == VK_SUCCESS;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool,
                                        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    ok = ok && vkAllocateCommandBuffers(g->device, &cbai, &cmd) == VK_SUCCESS;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    ok = ok && vkCreateFence(g->device, &fci, NULL, &fence) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                       VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        vkBeginCommandBuffer(cmd, &bi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
        vkCmdDispatch(cmd, GROUPS, 1, 1);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT,
                              VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL,
                             0, NULL);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si2 = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL};
        ok = vkQueueSubmit(g->queue, 1, &si2, fence) == VK_SUCCESS &&
             vkWaitForFences(g->device, 1, &fence, VK_TRUE, 10000000000ull) == VK_SUCCESS;
    }
    for (int k = 0; k < si->ngbuf && ok; k++)
        if (si->gbuf_cbuf_off[k] == 0x28) memcpy(out, gb[k].map, out_bytes);
    if (fence) vkDestroyFence(g->device, fence, NULL);
    if (cpool) vkDestroyCommandPool(g->device, cpool, NULL);
    if (pool) vkDestroyDescriptorPool(g->device, pool, NULL);
    if (pipe) vkDestroyPipeline(g->device, pipe, NULL);
    if (sm) vkDestroyShaderModule(g->device, sm, NULL);
    if (pl) vkDestroyPipelineLayout(g->device, pl, NULL);
    if (dsl) vkDestroyDescriptorSetLayout(g->device, dsl, NULL);
    for (int k = 0; k < 2; k++) buf_free(g, &gb[k]);
    buf_free(g, &cb);
    return ok;
}

static int run_program(Gpu* g, const char* name, void (*build)(Prog*)) {
    static Prog p;
    memset(&p, 0, sizeof p);
    build(&p);
    static u32 in[2 * INVOCATIONS], ref[INVOCATIONS * MAX_OUT], got[INVOCATIONS * MAX_OUT];
    inputs(in);
    u32 out_bytes = INVOCATIONS * p.outputs * 4;
    for (u32 i = 0; i < INVOCATIONS * p.outputs; i++) ref[i] = got[i] = FILL;
    u32 cb0[64] = {0};
    cb0[0x20 / 4] = (u32)VA_IN;
    cb0[0x24 / 4] = (u32)(VA_IN >> 32);
    cb0[0x28 / 4] = (u32)VA_OUT;
    cb0[0x2c / 4] = (u32)(VA_OUT >> 32);
    /* the reference */
    Sm86RefEnv env = {0};
    env.cbuf[0] = cb0;
    env.cbuf_words[0] = 64;
    env.global[0] = (u8*)(void*)in;
    env.global_va[0] = VA_IN;
    env.global_bytes[0] = sizeof in;
    env.global[1] = (u8*)(void*)ref;
    env.global_va[1] = VA_OUT;
    env.global_bytes[1] = out_bytes;
    env.local[0] = LOCAL, env.local[1] = env.local[2] = 1;
    env.groups[0] = GROUPS, env.groups[1] = env.groups[2] = 1;
    env.shared_bytes = LOCAL * 4;
    const char* why = sm86_ref_run(p.code, p.n * 16, &env);
    if (why) {
        fprintf(stderr, "%s: the reference stops: %s\n", name, why);
        return 0;
    }
    /* the translation */
    ShaderInfo si = {0};
    si.cs_local[0] = LOCAL, si.cs_local[1] = si.cs_local[2] = 1;
    si.shared_size = (int)(LOCAL * 4);
    size_t words = 0;
    u32* spv = sm86_to_spirv(p.code, p.n * 16, ST_CS, NULL, &si, &words);
    if (!spv) {
        fprintf(stderr, "%s: not translated: %s\n", name, si.first_unsupported);
        return 0;
    }
    int ok = si.ngbuf == 2 && gpu_run(g, spv, words, &si, cb0, in, sizeof in, got, out_bytes);
    free(spv);
    if (!ok) {
        fprintf(stderr, "%s: Vulkan failed\n", name);
        return 0;
    }
    u32 bad = 0, skipped = 0, nans = 0;
    for (u32 i = 0; i < INVOCATIONS; i++)
        for (u32 k = 0; k < p.outputs; k++) {
            u32 want = ref[i * p.outputs + k], have = got[i * p.outputs + k];
            if (want != have && is_nan(want) && is_nan(have)) { /* SPIR-V promises no NaN's bits */
                nans++;
                continue;
            }
            if ((p.denorm_mask >> k & 1) &&
                (is_subnormal(in[2 * i]) || is_subnormal(in[2 * i + 1]) || is_subnormal(want))) {
                skipped += want != have;
                continue;
            }
            int same = k < p.approx_from                                 ? have == want
                       : k == p.outputs - 1 && p.approx_from < p.outputs ? close_sin(have, want)
                                                                         : close_enough(have, want);
            if (!same && bad++ < 24)
                fprintf(stderr, "%s: invocation %u output %u: GPU 0x%08x, reference 0x%08x (inputs 0x%08x 0x%08x)\n",
                        name, i, k, have, want, in[2 * i], in[2 * i + 1]);
        }
    printf("%s: %u instructions, %u outputs x %u invocations, %u differ, %u NaNs of other bits, %u not compared "
           "(a subnormal input or result without FTZ: they differ)\n",
           name, p.n, p.outputs, INVOCATIONS, bad, nans, skipped);
    return bad == 0;
}

int main(void) {
    Gpu g = {0};
    if (!gpu_open(&g)) {
        printf("no Vulkan 1.3 device with shaderFloat64 and shaderInt64: skipped\n");
        return 77;
    }
    printf("device: %s\n", g.name);
    int ok = run_program(&g, "integer", integer_program);
    ok &= run_program(&g, "float", float_program);
    ok &= run_program(&g, "flow", flow_program);
    ok &= run_program(&g, "wide", wide_program);
    vkDestroyDevice(g.device, NULL);
    vkDestroyInstance(g.instance, NULL);
    return ok ? 0 : 1;
}
