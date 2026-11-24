/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's riscv64 back end (README.md, "The RISC back ends"): RV64GC's registers and the operations risc.h writes
   every primitive from, after machdef.h and before risc.h. Each word is an encoder's of LLVM's RISC-V TableGen
   records (td/rv64_enc.inc, tdenc.py), so nothing about an instruction's bits is written here: only which
   instructions an operation is. The code is RV64IMA's alone (the base every riscv64 host has); Zba, Zbb and Zicond
   are encoded for when a host's features choose them.

   Registers: t0-t1 and a0-a7 hold values (a0 and a1 last: a call's results); t2 (risc.h's RK_S2), t3 and t4 (the
   virtual flags RK_FA and RK_FB), t5 (RK_S1) and t6 (RK_SA, the assembler's) are reserved; s1 holds the state's
   address; s2-s11 the pinned guest registers and the guest memory's base; s0 the frame, ra the link, sp the spill
   slots; gp and tp are the platform's and never touched */

#include "rv64_enc.inc"

static inline void rv_w(SgXBuf* b, SnxU32 w) { sgx_imm(b, (long long)w, 4); }

/* the host's features a candidate may need: none yet (the base ISA's code everywhere) */
enum { SGX_F_ZBB = 1 };

#define SGX_NREG 32
#define SGX_FIX 32
#define SGX_RET 10
#define SGX_RET2 11
#define SGX_FRAME 2 /* sp */
#define RV_ZERO 0
#define RV_RA 1
#define RV_SP 2
#define RV_S0 8
#define RK_SA 31
#define RK_S1 30
#define RK_S2 7
#define RK_FA 28
#define RK_FB 29
static const signed char sgx_pool[] = {5, 6, 17, 16, 15, 14, 13, 12, 11, 10};
#define SGX_CALLER_SAVED (1u << 5 | 1u << 6 | 0xFFu << 10)

static inline int rv_fits12(long long k) { return k >= -2048 && k < 2048; }
static inline SnxU32 rv_i12(long long k) { return (SnxU32)k & 0xfff; }

/* ---- the operations risc.h writes the primitives from ---- */

static inline void rk_mv(SgXBuf* b, int d, int a) {
    if (d != a) rv_w(b, sge_rv64_addi((SnxU32)d, (SnxU32)a, 0));
}
/* any 64-bit constant into d alone: LUI and ADDIW of a 32-bit one, else the upper bits made first (shifted down past
   their trailing zeros), shifted up and the low 12 added (LLVM's RISCVMatInt) */
static inline void rk_li(SgXBuf* b, int d, SnxU64 k) {
    long long v = (long long)k, lo = (long long)(k << 52) >> 52, hi;
    unsigned sh;
    if (v == (long long)(int)v) {
        SnxU32 hi20 = (SnxU32)(((v + 0x800) >> 12) & 0xfffff);
        if (hi20) {
            rv_w(b, sge_rv64_lui((SnxU32)d, hi20));
            if (lo) rv_w(b, sge_rv64_addiw((SnxU32)d, (SnxU32)d, rv_i12(lo)));
        } else
            rv_w(b, sge_rv64_addi((SnxU32)d, RV_ZERO, rv_i12(lo)));
        return;
    }
    hi = (v - lo) >> 12;
    sh = 12 + (unsigned)__builtin_ctzll((SnxU64)hi);
    hi >>= sh - 12;
    rk_li(b, d, (SnxU64)hi);
    rv_w(b, sge_rv64_slli((SnxU32)d, (SnxU32)d, sh));
    if (lo) rv_w(b, sge_rv64_addi((SnxU32)d, (SnxU32)d, rv_i12(lo)));
}
#define RV_RRR(name, i64, i32)                                                                                         \
    static inline void rk_##name(SgXBuf* b, int d, int a, int c, int sf) {                                             \
        rv_w(b, sf ? sge_rv64_##i64((SnxU32)d, (SnxU32)a, (SnxU32)c) : sge_rv64_##i32((SnxU32)d, (SnxU32)a, (SnxU32)c)); \
    }
RV_RRR(add, add, addw)
RV_RRR(sub, sub, subw)
RV_RRR(sll, sll, sllw)
RV_RRR(srl, srl, srlw)
RV_RRR(sra, sra, sraw)
RV_RRR(mul, mul, mulw)
#define RV_RR3(name, i)                                                                                                \
    static inline void rk_##name(SgXBuf* b, int d, int a, int c) { rv_w(b, sge_rv64_##i((SnxU32)d, (SnxU32)a, (SnxU32)c)); }
RV_RR3(and, and)
RV_RR3(or, or)
RV_RR3(xor, xor)
RV_RR3(mulhu, mulhu)
RV_RR3(mulh, mulh)
RV_RR3(divu, divu)
RV_RR3(div, div)
RV_RR3(sltu, sltu)
RV_RR3(slt, slt)
static inline void rk_neg(SgXBuf* b, int d, int a, int sf) { rk_sub(b, d, RV_ZERO, a, sf); }
static inline void rk_seqz(SgXBuf* b, int d, int a) { rv_w(b, sge_rv64_sltiu((SnxU32)d, (SnxU32)a, 1)); }
static inline void rk_snez(SgXBuf* b, int d, int a) { rv_w(b, sge_rv64_sltu((SnxU32)d, RV_ZERO, (SnxU32)a)); }
static inline void rk_addi(SgXBuf* b, int d, int a, long long k, int sf) {
    if (rv_fits12(k)) {
        rv_w(b, sf ? sge_rv64_addi((SnxU32)d, (SnxU32)a, rv_i12(k)) : sge_rv64_addiw((SnxU32)d, (SnxU32)a, rv_i12(k)));
        return;
    }
    rk_li(b, RK_SA, (SnxU64)k);
    rk_add(b, d, a, RK_SA, sf);
}
#define RV_LOGI(name, i, r)                                                                                            \
    static inline void rk_##name(SgXBuf* b, int d, int a, long long k) {                                               \
        if (rv_fits12(k)) {                                                                                            \
            rv_w(b, sge_rv64_##i((SnxU32)d, (SnxU32)a, rv_i12(k)));                                                    \
            return;                                                                                                    \
        }                                                                                                              \
        rk_li(b, RK_SA, (SnxU64)k);                                                                                    \
        rk_##r(b, d, a, RK_SA);                                                                                        \
    }
RV_LOGI(andi, andi, and)
RV_LOGI(ori, ori, or)
RV_LOGI(xori, xori, xor)
static inline void rk_slli(SgXBuf* b, int d, int a, int k, int sf) {
    rv_w(b, sf ? sge_rv64_slli((SnxU32)d, (SnxU32)a, (SnxU32)k) : sge_rv64_slliw((SnxU32)d, (SnxU32)a, (SnxU32)k));
}
static inline void rk_srli(SgXBuf* b, int d, int a, int k, int sf) {
    rv_w(b, sf ? sge_rv64_srli((SnxU32)d, (SnxU32)a, (SnxU32)k) : sge_rv64_srliw((SnxU32)d, (SnxU32)a, (SnxU32)k));
}
static inline void rk_srai(SgXBuf* b, int d, int a, int k, int sf) {
    rv_w(b, sf ? sge_rv64_srai((SnxU32)d, (SnxU32)a, (SnxU32)k) : sge_rv64_sraiw((SnxU32)d, (SnxU32)a, (SnxU32)k));
}
static inline void rk_sext(SgXBuf* b, int d, int a, int bits) {
    if (bits == 32) rv_w(b, sge_rv64_addiw((SnxU32)d, (SnxU32)a, 0));
    else {
        rv_w(b, sge_rv64_slli((SnxU32)d, (SnxU32)a, 64u - (SnxU32)bits));
        rv_w(b, sge_rv64_srai((SnxU32)d, (SnxU32)d, 64u - (SnxU32)bits));
    }
}
static inline void rk_zext(SgXBuf* b, int d, int a, int bits) {
    if (bits == 8) rv_w(b, sge_rv64_andi((SnxU32)d, (SnxU32)a, 255));
    else {
        rv_w(b, sge_rv64_slli((SnxU32)d, (SnxU32)a, 64u - (SnxU32)bits));
        rv_w(b, sge_rv64_srli((SnxU32)d, (SnxU32)d, 64u - (SnxU32)bits));
    }
}
/* a load or a store of bytes at base + index + disp: the address's upper part (and the index) added in RK_SA where the
   12-bit displacement doesn't hold it */
static inline void rk_ldst(SgXBuf* b, int load, int bytes, int sign, int rt, int base, int index, int disp) {
    long long lo = (long long)((SnxU64)(long long)disp << 52) >> 52;
    SnxU32 r = (SnxU32)rt, i12;
    if (index >= 0 || !rv_fits12(disp)) {
        long long hi = (long long)disp - lo;
        if (hi) {
            rk_li(b, RK_SA, (SnxU64)hi);
            rv_w(b, sge_rv64_add(RK_SA, RK_SA, (SnxU32)base));
        } else
            rk_mv(b, RK_SA, base);
        if (index >= 0) rv_w(b, sge_rv64_add(RK_SA, RK_SA, (SnxU32)index));
        base = RK_SA;
        disp = (int)lo;
    }
    i12 = rv_i12(disp);
    if (load)
        rv_w(b, bytes == 1   ? (sign ? sge_rv64_lb(r, (SnxU32)base, i12) : sge_rv64_lbu(r, (SnxU32)base, i12))
                : bytes == 2 ? (sign ? sge_rv64_lh(r, (SnxU32)base, i12) : sge_rv64_lhu(r, (SnxU32)base, i12))
                : bytes == 4 ? (sign ? sge_rv64_lw(r, (SnxU32)base, i12) : sge_rv64_lwu(r, (SnxU32)base, i12))
                             : sge_rv64_ld(r, (SnxU32)base, i12));
    else
        rv_w(b, bytes == 1   ? sge_rv64_sb(r, (SnxU32)base, i12)
                : bytes == 2 ? sge_rv64_sh(r, (SnxU32)base, i12)
                : bytes == 4 ? sge_rv64_sw(r, (SnxU32)base, i12)
                             : sge_rv64_sd(r, (SnxU32)base, i12));
}

/* ---- branches, calls and the frame (risc_asm.h's and risc_a64.h's) ----
   a local branch: its word's place, its offset patched once its target is known (rk_patch: 0, -1 out of reach) */
enum { RV_FIX_B, RV_FIX_JAL };
static inline size_t rk_bz(SgXBuf* b, int nonzero, int r) {
    size_t at = b->n;
    rv_w(b, nonzero ? sge_rv64_bne((SnxU32)r, RV_ZERO, 0) : sge_rv64_beq((SnxU32)r, RV_ZERO, 0));
    return at;
}
static inline size_t rk_j(SgXBuf* b) {
    size_t at = b->n;
    rv_w(b, sge_rv64_jal(RV_ZERO, 0));
    return at;
}
static inline SnxU32 rv_word_at(const SgXBuf* b, size_t at) {
    return (SnxU32)b->p[at] | (SnxU32)b->p[at + 1] << 8 | (SnxU32)b->p[at + 2] << 16 | (SnxU32)b->p[at + 3] << 24;
}
static inline void rv_word_put(SgXBuf* b, size_t at, SnxU32 w) {
    b->p[at] = (unsigned char)w, b->p[at + 1] = (unsigned char)(w >> 8), b->p[at + 2] = (unsigned char)(w >> 16);
    b->p[at + 3] = (unsigned char)(w >> 24);
}
/* the branch or JAL at at to the byte offset to: its immediate field rewritten (B's 13-bit, JAL's 21-bit offset) */
static inline int rk_patch(SgXBuf* b, size_t at, size_t to) {
    long long d = (long long)to - (long long)at;
    SnxU32 w;
    if (at + 4 > b->cap) return 0;
    w = rv_word_at(b, at);
    if ((w & 0x7f) == 0x6f) { /* JAL */
        if (d < -(1ll << 20) || d >= (1ll << 20)) return -1;
        w = sge_rv64_jal((w >> 7) & 31, (SnxU32)(d >> 1));
    } else {
        SnxU32 rs1 = (w >> 15) & 31, rs2 = (w >> 20) & 31, f3 = (w >> 12) & 7;
        if (d < -(1ll << 12) || d >= (1ll << 12)) return -1;
        w = f3 == 1 ? sge_rv64_bne(rs1, rs2, (SnxU32)(d >> 1)) : sge_rv64_beq(rs1, rs2, (SnxU32)(d >> 1));
    }
    rv_word_put(b, at, w);
    return 0;
}
static inline void rk_call_reg(SgXBuf* b, int r) { rv_w(b, sge_rv64_jalr(RV_RA, (SnxU32)r, 0)); }
static inline void rk_ret(SgXBuf* b) { rv_w(b, sge_rv64_jalr(RV_ZERO, RV_RA, 0)); }
/* a call by the object's relocation (R_RISCV_CALL_PLT: AUIPC and JALR of ra, the relocation at the AUIPC, the place
   this gives) */
static inline size_t rk_call_reloc(SgXBuf* b) {
    size_t at = b->n;
    rv_w(b, sge_rv64_auipc(RV_RA, 0));
    rv_w(b, sge_rv64_jalr(RV_RA, RV_RA, 0));
    return at;
}
/* a tail call by the object's relocation (R_RISCV_CALL_PLT of AUIPC t6 and JALR x0) */
static inline size_t rk_tail_reloc(SgXBuf* b) {
    size_t at = b->n;
    rv_w(b, sge_rv64_auipc(RK_SA, 0));
    rv_w(b, sge_rv64_jalr(RV_ZERO, RK_SA, 0));
    return at;
}
static inline void rk_jump_reg(SgXBuf* b, int r) { rv_w(b, sge_rv64_jalr(RV_ZERO, (SnxU32)r, 0)); }
/* a call's or tail call's AUIPC and JALR at at made to reach to (within 2 GiB): 0, -1 out of reach */
static inline int rk_patch_call(SgXBuf* b, size_t at, size_t to) {
    long long d = (long long)to - (long long)at, lo = (long long)((SnxU64)d << 52) >> 52;
    SnxU32 auipc, jalr;
    if (at + 8 > b->cap) return 0;
    if (d < -(1ll << 31) || d >= (1ll << 31) - 0x800) return -1;
    auipc = rv_word_at(b, at), jalr = rv_word_at(b, at + 4);
    rv_word_put(b, at, sge_rv64_auipc((auipc >> 7) & 31, (SnxU32)(((d - lo) >> 12) & 0xfffff)));
    rv_word_put(b, at + 4, sge_rv64_jalr((jalr >> 7) & 31, (jalr >> 15) & 31, rv_i12(lo)));
    return 0;
}
/* d = the address of an 8-byte absolute address in the code (its relocation's place, which this gives): JAL d over
   it, the link its address */
static inline size_t rk_literal(SgXBuf* b, int d) {
    size_t at;
    rv_w(b, sge_rv64_jal((SnxU32)d, 6)); /* 12 bytes on */
    at = b->n;
    sgx_imm(b, 0, 8);
    return at;
}
/* rk_literal where there's no frame (a module descriptor's functions): the link as it was */
static inline size_t rk_leaf_literal(SgXBuf* b, int d) { return rk_literal(b, d); }
/* a function's frame: ra, s0, s1 and (pinned) s2-s11 saved, s0 the caller's sp, s1 the state; then sp -= the slots'
   room, LUI and ADDIW of t6 patched (rk_fn_patch_frame; the place this gives); the epilogue gives sp back from s0 */
static inline size_t rk_fn_prologue(SgXBuf* b, int pinned) {
    size_t at;
    int k;
    rv_w(b, sge_rv64_addi(RV_SP, RV_SP, rv_i12(-112)));
    rv_w(b, sge_rv64_sd(RV_RA, RV_SP, 104));
    rv_w(b, sge_rv64_sd(RV_S0, RV_SP, 96));
    rv_w(b, sge_rv64_sd(9, RV_SP, 88));
    if (pinned)
        for (k = 0; k < 10; ++k) rv_w(b, sge_rv64_sd(18u + (SnxU32)k, RV_SP, 8u * (SnxU32)k));
    rv_w(b, sge_rv64_addi(RV_S0, RV_SP, 112));
    rk_mv(b, 9, 10);
    at = b->n;
    rv_w(b, sge_rv64_lui(RK_SA, 0));
    rv_w(b, sge_rv64_addiw(RK_SA, RK_SA, 0));
    rv_w(b, sge_rv64_sub(RV_SP, RV_SP, RK_SA));
    return at;
}
static inline void rk_fn_epilogue(SgXBuf* b, int pinned) {
    int k;
    rv_w(b, sge_rv64_addi(RV_SP, RV_S0, rv_i12(-112)));
    if (pinned)
        for (k = 0; k < 10; ++k) rv_w(b, sge_rv64_ld(18u + (SnxU32)k, RV_SP, 8u * (SnxU32)k));
    rv_w(b, sge_rv64_ld(9, RV_SP, 88));
    rv_w(b, sge_rv64_ld(RV_S0, RV_SP, 96));
    rv_w(b, sge_rv64_ld(RV_RA, RV_SP, 104));
    rv_w(b, sge_rv64_addi(RV_SP, RV_SP, 112));
}
static inline int rk_fn_patch_frame(SgXBuf* b, size_t at, long long slots) {
    long long size = (slots + 15) & ~15ll, lo = (long long)((SnxU64)size << 52) >> 52;
    if (size >= (1ll << 31) - 0x800) return 0;
    if (at + 8 > b->cap) return 1;
    rv_word_put(b, at, sge_rv64_lui(RK_SA, (SnxU32)(((size + 0x800) >> 12) & 0xfffff)));
    rv_word_put(b, at + 4, sge_rv64_addiw(RK_SA, RK_SA, rv_i12(lo)));
    return 1;
}
/* FENCE RW, RW (every barrier: RISC-V's loads and stores are no acquires or releases) */
static inline void rk_fence(SgXBuf* b) { rv_w(b, sge_rv64_fence(3, 3)); }
static inline void rk_nop(SgXBuf* b) { rv_w(b, sge_rv64_addi(RV_ZERO, RV_ZERO, 0)); }
/* the reservation's load and the conditional store of 4 or 8 bytes at [a] (LR and SC, both .aqrl: sequentially
   consistent); the store's status 0 where it stored */
static inline void rk_lr(SgXBuf* b, int d, int a, int bytes) {
    rv_w(b, bytes == 8 ? sge_rv64_lr_d((SnxU32)d, (SnxU32)a) : sge_rv64_lr_w((SnxU32)d, (SnxU32)a));
}
static inline void rk_sc(SgXBuf* b, int status, int a, int v, int bytes) {
    rv_w(b, bytes == 8 ? sge_rv64_sc_d((SnxU32)status, (SnxU32)v, (SnxU32)a)
                       : sge_rv64_sc_w((SnxU32)status, (SnxU32)v, (SnxU32)a));
}
/* the reservation's load of 4 bytes is sign extended (LR.W): rk_lr's value compared at 32 bits must be too */
#define RK_LR32_SEXT 1

/* risc_a64.h's inline sequences' fixed registers: the guest memory's base a1, the address a2, the values a3 and a4;
   a5 to a7 their own */
#define RK_M_BASE 11
#define RK_M_ADDR 12
#define RK_M_V1 13
#define RK_M_V2 14
#define RK_M_T1 15
#define RK_M_T2 16
#define RK_M_T3 17

/* the state's address (s1, the callee's), a call's arguments from the state's (a0) on */
#define SGX_BASE 9
static const signed char sgx_arg_regs[6] = {10, 11, 12, 13, 14, 15};
/* AArch32's calls' arguments after the state's (a1-a4), and its exclusive store's address, value and the memory's
   base (a1, a2, a3), a4 and a5 its own */
static const signed char sgx_a32_args[4] = {11, 12, 13, 14};
static const signed char sgx_a32_strex[3] = {11, 12, 13};
#define SGX_A32_STREX_CLOBBER (1u << 14 | 1u << 15)
