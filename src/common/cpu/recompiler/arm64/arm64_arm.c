/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The arm64 back end's AArch32 blocks: mach/mach_arm.inc over sailgen's arm64 emitters of the AArch32 Sail
   (a32_arm64.inc), and its glue here: the frame (X29, X30 and X19 saved, the spill slots below), the entry checks on
   X0, the chaining's tail jumps (JUMP26) and its poll (ADRP and LDR of snx_arm_poll_offset), and the entry stub of a
   block another back end makes. The VFP's arithmetic is snx_arm_fp's, its helper (no hot path yet). It builds on any
   host: its blocks are arm64's code, which the tests run under Unicorn. On Windows the object is COFF, a frame's code one
   entry of its function table */
#include "common/cpu/recompiler/arm64/arm64_arm.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) || defined(__aarch64__) /* as arm64_codegen.c */
/* the AArch64 ELF relocations the blocks have (the ELF for the Arm 64-bit Architecture's numbers) */
enum {
    A64_EM = 183, /* EM_AARCH64 */
    A64_ADR_PREL_PG_HI21 = 275,
    A64_JUMP26 = 282,
    A64_CALL26 = 283,
    A64_LDST64_ABS_LO12_NC = 286
};
static unsigned arm64_arm_features(void) {
    return (snx_arm64_available() ? snx_arm64_features() : SNX_ARM64_F_ALL) |
           (snx_arm64_windows() ? SNX_ARM64_F_WIN : 0);
}
#define MG_ARM_INC         "a32_arm64.inc"
#define MG_ARM             SnxArm64Arm
#define MG_ARM_NEXT        SnxArm64ArmNext
#define MG_ARM_API(name)   snx_arm64_arm_##name
#define MG_ARM_AVAILABLE() snx_arm64_available()
#define MG_ARM_MAKES()     1
#define MG_ARM_FEATURES()  arm64_arm_features()
#define MG_ARM_EM          A64_EM
#define MG_ARM_CALL_RELOC  A64_CALL26
#define MG_ARM_LOAD_FENCE  1 /* an LDR is no acquire: a DMB after it, as LLVM's LDA lowering's order */
#define MG_ARM_FP_HOT      0
#define MG_ARM_ABI          SNX_ARM64_F_WIN
/* the code of a frame one range, the one exit that has none past it */
#define MG_ARM_EXIT_LAST(c) windows(c)
#define MG_ARM_COFF         snx_arm64_windows()

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

static int windows(const SnxArm64Arm* c);
#include "common/cpu/recompiler/mach/mach_arm.inc"

/* ---- the arm64 glue ---- */

/* the block is made for Windows */
static int windows(const SnxArm64Arm* c) { return (c->features & SNX_ARM64_F_WIN) != 0; }

/* every operation snx_arm_fp's (its helper's) */
static SgD fp_call(SgCtx* g, SnxArmVectorOp op, unsigned width, SnxU32 param, SgD xv, SgD yv, SgD zv) {
    return fp_helper(g, op, width, param, xv, yv, zv);
}

/* the frame's room: SUB or ADD SP of the slots' size, patched once the block is done (sgx_patch_frame), on Windows the
   SUB past a probe of one of a page or more */
static void frame_here(MG_ARM* c, size_t at) {
    if (c->a.nframe >= 256) sg_fail("a block of too many epilogues", c->a.nframe, 256);
    c->a.frame_at[c->a.nframe++] = at;
}
static void frame(MG_ARM* c, int add) {
    SgXBuf* b = &c->b;
    if (!add && c->big) frame_here(c, sga_stack_probe(b));
    frame_here(c, b->n);
    sga_w(b, add ? sge_a64_add_addsub_imm(31, 31, 0, 0, 1) : sge_a64_sub_addsub_imm(31, 31, 0, 0, 1));
}
/* 1 where the frame is a page or more and has no probe: made again with it */
static int mg_patch_frame(MG_ARM* c) {
    if (windows(c) && !c->big && SGA_FRAME_PROBED(sga_frame_size(&c->a))) return 1;
    sgx_patch_frame(&c->a);
    return 0;
}
/* the entry (block_begin's checks: R15 the block's address, CPSR.T and E clear, a Thumb block's T set and its ITSTATE 0;
   then exit and syndrome cleared), on X0, before the frame; where a check fails, a jump to bad (entry_exit). Then the
   entry the block's own jumps from another of the arm64 back end's take (X and the block's name), past them: X19 the
   state, the frame. A hosted block's entry is that one alone (codegen.h) */
static void mg_prologue(MG_ARM* c, SnxU32 pc, int* bad) {
    SgXBuf* b = &c->b;
    SgCtx* g = &c->g;
    *bad = sgx_a32_label(g);
    if (!sgx_a32_hosted(g)) {
        sga_ldst(b, 1, 4, SGA_X16, 0, -1, (int)(offsetof(SnxArmCPU, r) + 60));
        sga_movi(b, SGA_X17, pc, 0);
        sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, SGA_X16, 0, SGA_X17, 0, 0)); /* CMP W16, W17 */
        sgx_a32_jcc(g, b, 1, *bad);                                             /* B.NE */
        sga_ldst(b, 1, 4, SGA_X16, 0, -1, (int)offsetof(SnxArmCPU, cpsr));
        if (g->thumb) { /* of T, E and ITSTATE's bits, T alone: AND W16, W16, W17; CMP W16, #T */
            sga_movi(b, SGA_X17, SNX_ARM_T | SNX_ARM_E | SG_IT_BITS, 0);
            sga_w(b, sge_a64_and_log_shift(SGA_X16, SGA_X16, 0, SGA_X17, 0, 0));
            sga_w(b, sge_a64_subs_addsub_imm(SGA_ZR, SGA_X16, SNX_ARM_T, 0, 0));
        } else {
            sga_movi(b, SGA_X17, SNX_ARM_T | SNX_ARM_E, 0);
            sga_w(b, sge_a64_ands_log_shift(SGA_ZR, SGA_X16, 0, SGA_X17, 0, 0)); /* TST W16, W17 */
        }
        sgx_a32_jcc(g, b, 1, *bad);
        sga_ldst(b, 0, 4, SGA_ZR, 0, -1, (int)offsetof(SnxArmCPU, exit));
        sga_ldst(b, 0, 4, SGA_ZR, 0, -1, (int)offsetof(SnxArmCPU, syndrome));
    }
    c->own = b->n;
    sga_w(b, sge_a64_stp_gen_pre_idx(29, 31, 30, (SnxU32)-4 & 0x7F, 2)); /* STP X29, X30, [SP, #-32]! */
    sga_w(b, sge_a64_add_addsub_imm(29, 31, 0, 0, 1));                    /* MOV X29, SP */
    sga_w(b, sge_a64_stp_gen_general_offset(19, 31, 20, 2, 2));           /* STP X19, X20, [SP, #16] */
    sga_state_in(b);
    frame(c, 0);
}
/* where a check of the entry failed: W1 the block's address, W2 its first word (a Thumb block's halfword), and
   snx_a64_a32_entry_exit or snx_a64_thumb_entry (the object's own, which set the exit) */
static void mg_entry_exit(MG_ARM* c, SnxU32 pc, SnxU32 first, int bad) {
    SgXBuf* b = &c->b;
    sgx_a32_bind(&c->g, b, bad);
    sga_movi(b, 1, pc, 0);
    sga_movi(b, 2, first, 0);
    add_rel(c, b->n, c->g.thumb ? "snx_a64_thumb_entry" : "snx_a64_a32_entry_exit", A64_JUMP26, 0);
    sga_w(b, sge_a64_b_uncond(0));
}
/* the frame given back and X19, X29 and X30 the caller's */
static void epilogue(MG_ARM* c) {
    SgXBuf* b = &c->b;
    frame(c, 1);
    sga_w(b, sge_a64_ldp_gen_general_offset(19, 31, 20, 2, 2));
    sga_w(b, sge_a64_ldp_gen_post_idx(29, 31, 30, 4, 2));
}
static void mg_return(MG_ARM* c) {
    epilogue(c);
    sga_w(&c->b, sge_a64_ret(30));
}
/* where the block ends with R15 key's address in key's state: a jump to the block's own entry past its checks (X and
   its name) where key is one of the successors, after the poll where polled (key at or before the block's start); else
   R15 stored and the return. public: the block's checked entry instead, R15 stored */
static void mg_go_on(MG_ARM* c, SnxU32 key, int polled, const SnxU32* next, size_t nnext, int public) {
    SgXBuf* b = &c->b;
    SgCtx* g = &c->g;
    size_t k = 0;
    while (k < nnext && next[k] != key) ++k;
    if (k == nnext || public) sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, key & ~1u);
    if (k == nnext) {
        sgx_a32_jmp(g, b, g->ret);
        return;
    }
    if (polled) {
        /* ADRP X16, snx_arm_poll_offset; LDR X16, [X16, :lo12:]; LDR W16, [X19, X16]; CBZ W16, go: a thread of higher
           priority waits, the return with R15 stored */
        int go = sgx_a32_label(g);
        add_rel(c, b->n, "snx_arm_poll_offset", A64_ADR_PREL_PG_HI21, 0);
        sga_w(b, sge_a64_adrp(SGA_X16, 0, 0));
        add_rel(c, b->n, "snx_arm_poll_offset", A64_LDST64_ABS_LO12_NC, 0);
        sga_w(b, sge_a64_ldr_imm_gen_immediate_unsigned(SGA_X16, SGA_X16, 0, 3));
        sga_ldst(b, 1, 4, SGA_X16, SGX_BASE, SGA_X16, 0);
        sga_a32_cbz(g, b, 0, SGA_X16, go);
        if (!public) sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, key & ~1u);
        sgx_a32_jmp(g, b, g->ret);
        sgx_a32_bind(g, b, go);
    }
    sga_state_out(b);
    epilogue(c);
    char sym[24];
    name_of(sym + (public ? 0 : 1), sizeof sym - 1, key);
    if (!public) sym[0] = 'X';
    add_rel(c, b->n, sym, A64_JUMP26, 0);
    sga_w(b, sge_a64_b_uncond(0));
}

/* the block's end in a tail call of the block at the address in reg: the state its argument, as it is this one's */
static void mg_tail(MG_ARM* c, int reg) {
    SgXBuf* b = &c->b;
    sga_w(b, sge_a64_orr_log_shift(SGA_X16, SGA_ZR, 0, (SnxU32)reg, 0, 1)); /* MOV X16, reg */
    sga_state_out(b);
    epilogue(c);
    sga_w(b, sge_a64_br(SGA_X16));
}

/* snx_a64_a32_entry_exit(state X0, the block's address W1, its first word W2), each object's own, where a block's
   entry check failed (llvm.c's block_begin, in its order): R15 not the address, BAD_ENTRY with the address; else
   CPSR.T set, BAD_ENTRY, else (E) ENDIAN_UNSUPPORTED, with the word; R15 left as it is */
static void entry_exit_fn(SnxXElf* o) {
    SgXBuf b = {0};
    unsigned char code[64];
    b.p = code;
    b.cap = sizeof code;
    sga_ldst(&b, 1, 4, SGA_X16, 0, -1, (int)(offsetof(SnxArmCPU, r) + 60));
    sga_w(&b, sge_a64_subs_addsub_shift(SGA_ZR, SGA_X16, 0, 1, 0, 0)); /* CMP W16, W1 */
    sga_w(&b, sge_a64_b_cond(0, 5));                                   /* B.EQ state (5 words on) */
    sga_movi(&b, SGA_X16, SNX_ARM_BAD_ENTRY, 0);
    sga_ldst(&b, 0, 4, SGA_X16, 0, -1, (int)offsetof(SnxArmCPU, exit));
    sga_ldst(&b, 0, 4, 1, 0, -1, (int)offsetof(SnxArmCPU, syndrome));
    sga_w(&b, sge_a64_ret(30));
    /* state: BAD_ENTRY where CPSR.T, else ENDIAN_UNSUPPORTED; the syndrome the word */
    sga_ldst(&b, 1, 4, SGA_X16, 0, -1, (int)offsetof(SnxArmCPU, cpsr));
    sga_movi(&b, SGA_X17, SNX_ARM_ENDIAN_UNSUPPORTED, 0);
    sga_w(&b, sge_a64_tbz(SGA_X16, 2, 5, 0)); /* TBZ W16, #5 (T), +2 */
    sga_movi(&b, SGA_X17, SNX_ARM_BAD_ENTRY, 0);
    sga_ldst(&b, 0, 4, SGA_X17, 0, -1, (int)offsetof(SnxArmCPU, exit));
    sga_ldst(&b, 0, 4, 2, 0, -1, (int)offsetof(SnxArmCPU, syndrome));
    sga_w(&b, sge_a64_ret(30));
    size_t at = snx_xelf_put(o, SNX_ELF_TEXT, code, b.n, 16);
    snx_xelf_symbol(o, "snx_a64_a32_entry_exit", SNX_ELF_TEXT, at, b.n, SNX_ELF_LOCAL | SNX_ELF_FUNC);
}
/* snx_a64_thumb_entry(state X0, the block's address W1, its first halfword W2), where a Thumb block's entry check failed
   (llvm_thumb.inc's, in its order): R15 not the address, BAD_ENTRY with the address; CPSR.T clear, BAD_ENTRY with the
   halfword; ITSTATE in CPSR, IT_ENTRY with it; else (E) ENDIAN_UNSUPPORTED with the halfword */
static void thumb_entry_exit_fn(SnxXElf* o) {
    SgXBuf b = {0};
    unsigned char code[96];
    b.p = code;
    b.cap = sizeof code;
    sga_ldst(&b, 1, 4, SGA_X16, 0, -1, (int)(offsetof(SnxArmCPU, r) + 60));
    sga_w(&b, sge_a64_eor_log_shift(SGA_X16, SGA_X16, 0, 1, 0, 0)); /* EOR W16, W16, W1 */
    size_t wrong = b.n;
    sga_w(&b, sge_a64_cbnz(SGA_X16, 0, 0));
    sga_ldst(&b, 1, 4, SGA_X16, 0, -1, (int)offsetof(SnxArmCPU, cpsr));
    sga_movi(&b, SGA_X17, SNX_ARM_BAD_ENTRY, 0);
    sga_w(&b, sge_a64_ubfm(1, SGA_X16, 5, 5, 0, 0)); /* UBFX W1, W16, #5 (T), #1 */
    size_t clear = b.n;
    sga_w(&b, sge_a64_cbz(1, 0, 0));
    sga_w(&b, sge_a64_ubfm(1, SGA_X16, 26, 25, 0, 0));        /* UBFX W1, W16, #25, #2 */
    sga_w(&b, sge_a64_ubfm(SGA_X16, SGA_X16, 15, 10, 0, 0));  /* UBFX W16, W16, #10, #6 */
    sga_w(&b, sge_a64_orr_log_shift(1, 1, 2, SGA_X16, 0, 0)); /* ORR W1, W1, W16, LSL #2 */
    sga_movi(&b, SGA_X17, SNX_ARM_ENDIAN_UNSUPPORTED, 0);
    size_t none = b.n;
    sga_w(&b, sge_a64_cbz(1, 0, 0));
    sga_movi(&b, SGA_X17, SNX_ARM_IT_ENTRY, 0);
    sga_w(&b, sge_a64_orr_log_shift(2, SGA_ZR, 0, 1, 0, 0)); /* MOV W2, W1 */
    sga_a32_put(&b, clear, (long long)(b.n - clear) / 4, SGA_FIX_BCOND);
    sga_a32_put(&b, none, (long long)(b.n - none) / 4, SGA_FIX_BCOND);
    sga_ldst(&b, 0, 4, SGA_X17, 0, -1, (int)offsetof(SnxArmCPU, exit));
    sga_ldst(&b, 0, 4, 2, 0, -1, (int)offsetof(SnxArmCPU, syndrome));
    sga_w(&b, sge_a64_ret(30));
    sga_a32_put(&b, wrong, (long long)(b.n - wrong) / 4, SGA_FIX_BCOND);
    sga_movi(&b, SGA_X17, SNX_ARM_BAD_ENTRY, 0);
    sga_ldst(&b, 0, 4, SGA_X17, 0, -1, (int)offsetof(SnxArmCPU, exit));
    sga_ldst(&b, 0, 4, 1, 0, -1, (int)offsetof(SnxArmCPU, syndrome));
    sga_w(&b, sge_a64_ret(30));
    if (b.n > b.cap) sg_fail("the Thumb entry's exit past its room", (long long)b.n, (long long)b.cap);
    size_t at = snx_xelf_put(o, SNX_ELF_TEXT, code, b.n, 16);
    snx_xelf_symbol(o, "snx_a64_thumb_entry", SNX_ELF_TEXT, at, b.n, SNX_ELF_LOCAL | SNX_ELF_FUNC);
}
static void mg_entry_exit_fn(SnxXElf* o) {
    entry_exit_fn(o);
    thumb_entry_exit_fn(o);
}
int snx_arm64_arm_entry(MG_ARM* c, uint32_t key) {
    SgXBuf b = {0};
    unsigned char code[16];
    b.p = code;
    b.cap = sizeof code;
    sga_movi(&b, SGA_X16, key & ~1u, 0);
    sga_ldst(&b, 0, 4, SGA_X16, 0, -1, (int)(offsetof(SnxArmCPU, r) + 60)); /* R15 = the address; B the block */
    Rel rel = {.at = b.n, .type = A64_JUMP26, .addend = 0};
    sga_w(&b, sge_a64_b_uncond(0));
    name_of(rel.sym, sizeof rel.sym, key);
    char name[24];
    snprintf(name, sizeof name, "X%s", rel.sym);
    return keep(c, code, b.n, &rel, 1, key & ~1u, name, (size_t)-1, 0) ? 0 : -1;
}
#else
int snx_arm64_arm_available(void) { return 0; }
SnxArm64Arm* snx_arm64_arm_new(void) { return NULL; }
void snx_arm64_arm_free(SnxArm64Arm* c) { (void)c; }
int snx_arm64_arm_options(SnxArm64Arm* c, const SnxArmCodegenOptions* options) {
    (void)c, (void)options;
    return -1;
}
int snx_arm64_arm_block(SnxArm64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                        size_t* consumed, SnxArm64ArmNext next, void* user) {
    (void)c, (void)name, (void)code, (void)count, (void)pc, (void)next, (void)user;
    *consumed = 0;
    return 0;
}
int snx_arm64_arm_thumb_block(SnxArm64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                              size_t* consumed, SnxArm64ArmNext next, void* user) {
    return snx_arm64_arm_block(c, name, code, count, pc, consumed, next, user);
}
const char* snx_arm64_arm_why(const SnxArm64Arm* c) {
    (void)c;
    return "no arm64 back end in this build";
}
int snx_arm64_arm_object(SnxArm64Arm* c, unsigned char** object, size_t* size, size_t* blocks) {
    (void)c;
    *object = NULL;
    *size = 0;
    if (blocks) *blocks = 0;
    return -1;
}
void snx_arm64_arm_limit(SnxArm64Arm* c, int pool, unsigned features) { (void)c, (void)pool, (void)features; }
int snx_arm64_arm_absorb(SnxArm64Arm* c, SnxArm64Arm* from) {
    (void)c, (void)from;
    return -1;
}
int snx_arm64_arm_entry(SnxArm64Arm* c, uint32_t key) {
    (void)c, (void)key;
    return -1;
}
#endif
