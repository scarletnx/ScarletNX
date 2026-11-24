/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's loongarch64 back end (README.md, "The RISC back ends"): LA64's registers under the LP64D ABI and the
   operations risc.h writes every primitive from, after machdef.h and before risc.h. Each word is an encoder's of
   LLVM's LoongArch TableGen records (td/la64_enc.inc, tdenc.py). The 32-bit operations (ADD.W, SLL.W and the like)
   sign extend their results, as RISC-V's do; loads and stores may be unaligned (Linux's LoongArch hosts all allow it).

   Registers: t0-t3 and a0-a7 hold values (a0 and a1 last: a call's results); t4 and t5 (the virtual flags RK_FB and
   RK_FA), t6 (RK_S2), t7 (RK_S1) and t8 (RK_SA, the assembler's) are reserved; s0 holds the state's address, s1-s8
   the pinned guest registers and the guest memory's base; fp the frame's anchor, sp the spill slots, ra the link; tp
   and r21 are the platform's and never touched */

#include "la64_enc.inc"

static inline void la_w(SgXBuf* b, SnxU32 w) { sgx_imm(b, (long long)w, 4); }

enum { SGX_F_NONE_LA = 1 };

#define SGX_NREG 32
#define SGX_FIX 32
#define SGX_RET 4
#define SGX_RET2 5
#define SGX_FRAME 3 /* sp */
#define LA_ZERO 0
#define LA_RA 1
#define LA_SP 3
#define LA_FP 22
#define RK_SA 20
#define RK_S1 19
#define RK_S2 18
#define RK_FA 17
#define RK_FB 16
static const signed char sgx_pool[] = {12, 13, 14, 15, 11, 10, 9, 8, 7, 6, 5, 4};
#define SGX_CALLER_SAVED 0xFFF0u /* a0-t3 */

static inline int la_fits12(long long k) { return k >= -2048 && k < 2048; }
static inline SnxU32 la_i12(long long k) { return (SnxU32)k & 0xfff; }
#define U(r) ((SnxU32)(r))

/* ---- the operations risc.h writes the primitives from ---- */

static inline void rk_mv(SgXBuf* b, int d, int a) {
    if (d != a) la_w(b, sge_la64_or(U(d), U(a), LA_ZERO));
}
/* any 64-bit constant into d alone: ADDI.D or ORI of 12 bits; else LU12I.W (bits 31-12, sign extended) and ORI of
   the low 32, then LU32I.D (bits 51-32, sign extended) and LU52I.D (63-52) where they differ from that extension */
static inline void rk_li(SgXBuf* b, int d, SnxU64 k) {
    long long v = (long long)k;
    SnxU32 hi20 = (SnxU32)(k >> 12) & 0xfffff;
    if (la_fits12(v)) {
        la_w(b, sge_la64_addi_d(U(d), LA_ZERO, la_i12(v)));
        return;
    }
    if (hi20) {
        la_w(b, sge_la64_lu12i_w(U(d), hi20));
        if (k & 0xfff) la_w(b, sge_la64_ori(U(d), U(d), (SnxU32)k & 0xfff));
    } else
        la_w(b, sge_la64_ori(U(d), LA_ZERO, (SnxU32)k & 0xfff));
    {
        SnxU64 cur = (SnxU64)(long long)(int)(SnxU32)k; /* what d holds */
        if (((cur >> 32) & 0xfffff) != ((k >> 32) & 0xfffff)) {
            la_w(b, sge_la64_lu32i_d(U(d), (SnxU32)(k >> 32) & 0xfffff));
            cur = (cur & 0xffffffffu) | (SnxU64)((long long)(k << 12) >> 44) << 32;
        }
        if (cur != k) la_w(b, sge_la64_lu52i_d(U(d), U(d), (SnxU32)(k >> 52)));
    }
}
#define LA_RRR(name, i64, i32)                                                                                         \
    static inline void rk_##name(SgXBuf* b, int d, int a, int c, int sf) {                                             \
        la_w(b, sf ? sge_la64_##i64(U(d), U(a), U(c)) : sge_la64_##i32(U(d), U(a), U(c)));                             \
    }
LA_RRR(add, add_d, add_w)
LA_RRR(sub, sub_d, sub_w)
LA_RRR(sll, sll_d, sll_w)
LA_RRR(srl, srl_d, srl_w)
LA_RRR(sra, sra_d, sra_w)
LA_RRR(mul, mul_d, mul_w)
#define LA_RR3(name, i)                                                                                                \
    static inline void rk_##name(SgXBuf* b, int d, int a, int c) { la_w(b, sge_la64_##i(U(d), U(a), U(c))); }
LA_RR3(and, and)
LA_RR3(or, or)
LA_RR3(xor, xor)
LA_RR3(mulhu, mulh_du)
LA_RR3(mulh, mulh_d)
LA_RR3(divu, div_du)
LA_RR3(div, div_d)
LA_RR3(sltu, sltu)
LA_RR3(slt, slt)
static inline void rk_neg(SgXBuf* b, int d, int a, int sf) { rk_sub(b, d, LA_ZERO, a, sf); }
static inline void rk_seqz(SgXBuf* b, int d, int a) { la_w(b, sge_la64_sltui(U(d), U(a), 1)); }
static inline void rk_snez(SgXBuf* b, int d, int a) { la_w(b, sge_la64_sltu(U(d), LA_ZERO, U(a))); }
static inline void rk_addi(SgXBuf* b, int d, int a, long long k, int sf) {
    if (la_fits12(k)) {
        la_w(b, sf ? sge_la64_addi_d(U(d), U(a), la_i12(k)) : sge_la64_addi_w(U(d), U(a), la_i12(k)));
        return;
    }
    rk_li(b, RK_SA, (SnxU64)k);
    rk_add(b, d, a, RK_SA, sf);
}
/* ANDI, ORI and XORI of 12 bits (zero extended); XOR of all ones NOR; else the constant in RK_SA */
#define LA_LOGI(name, i, r)                                                                                            \
    static inline void rk_##name(SgXBuf* b, int d, int a, long long k) {                                               \
        if (k >= 0 && k < 4096) {                                                                                      \
            la_w(b, sge_la64_##i(U(d), U(a), (SnxU32)k));                                                              \
            return;                                                                                                    \
        }                                                                                                              \
        rk_li(b, RK_SA, (SnxU64)k);                                                                                    \
        rk_##r(b, d, a, RK_SA);                                                                                        \
    }
LA_LOGI(andi, andi, and)
LA_LOGI(ori, ori, or)
static inline void rk_xori(SgXBuf* b, int d, int a, long long k) {
    if (k == -1) {
        la_w(b, sge_la64_nor(U(d), U(a), LA_ZERO));
        return;
    }
    if (k >= 0 && k < 4096) {
        la_w(b, sge_la64_xori(U(d), U(a), (SnxU32)k));
        return;
    }
    rk_li(b, RK_SA, (SnxU64)k);
    rk_xor(b, d, a, RK_SA);
}
static inline void rk_slli(SgXBuf* b, int d, int a, int k, int sf) {
    la_w(b, sf ? sge_la64_slli_d(U(d), U(a), (SnxU32)k) : sge_la64_slli_w(U(d), U(a), (SnxU32)k));
}
static inline void rk_srli(SgXBuf* b, int d, int a, int k, int sf) {
    la_w(b, sf ? sge_la64_srli_d(U(d), U(a), (SnxU32)k) : sge_la64_srli_w(U(d), U(a), (SnxU32)k));
}
static inline void rk_srai(SgXBuf* b, int d, int a, int k, int sf) {
    la_w(b, sf ? sge_la64_srai_d(U(d), U(a), (SnxU32)k) : sge_la64_srai_w(U(d), U(a), (SnxU32)k));
}
static inline void rk_sext(SgXBuf* b, int d, int a, int bits) {
    la_w(b, bits == 8    ? sge_la64_ext_w_b(U(d), U(a))
            : bits == 16 ? sge_la64_ext_w_h(U(d), U(a))
                         : sge_la64_addi_w(U(d), U(a), 0));
}
static inline void rk_zext(SgXBuf* b, int d, int a, int bits) {
    la_w(b, sge_la64_bstrpick_d(U(d), U(a), (SnxU32)bits - 1, 0));
}
/* a load or a store of bytes at base + index + disp: LDX and STX where there's an index alone, else the address's
   upper part (and the index) added in RK_SA where the 12-bit displacement doesn't hold it */
static inline void rk_ldst(SgXBuf* b, int load, int bytes, int sign, int rt, int base, int index, int disp) {
    long long lo = (long long)((SnxU64)(long long)disp << 52) >> 52;
    SnxU32 r = U(rt), i12;
    if (index >= 0 && !disp) {
        SnxU32 j = U(base), k = U(index);
        if (load)
            la_w(b, bytes == 1   ? (sign ? sge_la64_ldx_b(r, j, k) : sge_la64_ldx_bu(r, j, k))
                    : bytes == 2 ? (sign ? sge_la64_ldx_h(r, j, k) : sge_la64_ldx_hu(r, j, k))
                    : bytes == 4 ? (sign ? sge_la64_ldx_w(r, j, k) : sge_la64_ldx_wu(r, j, k))
                                 : sge_la64_ldx_d(r, j, k));
        else
            la_w(b, bytes == 1   ? sge_la64_stx_b(r, j, k)
                    : bytes == 2 ? sge_la64_stx_h(r, j, k)
                    : bytes == 4 ? sge_la64_stx_w(r, j, k)
                                 : sge_la64_stx_d(r, j, k));
        return;
    }
    if (index >= 0 || !la_fits12(disp)) {
        long long hi = (long long)disp - lo;
        if (hi) {
            rk_li(b, RK_SA, (SnxU64)hi);
            la_w(b, sge_la64_add_d(RK_SA, RK_SA, U(base)));
        } else
            rk_mv(b, RK_SA, base);
        if (index >= 0) la_w(b, sge_la64_add_d(RK_SA, RK_SA, U(index)));
        base = RK_SA;
        disp = (int)lo;
    }
    i12 = la_i12(disp);
    if (load)
        la_w(b, bytes == 1   ? (sign ? sge_la64_ld_b(r, U(base), i12) : sge_la64_ld_bu(r, U(base), i12))
                : bytes == 2 ? (sign ? sge_la64_ld_h(r, U(base), i12) : sge_la64_ld_hu(r, U(base), i12))
                : bytes == 4 ? (sign ? sge_la64_ld_w(r, U(base), i12) : sge_la64_ld_wu(r, U(base), i12))
                             : sge_la64_ld_d(r, U(base), i12));
    else
        la_w(b, bytes == 1   ? sge_la64_st_b(r, U(base), i12)
                : bytes == 2 ? sge_la64_st_h(r, U(base), i12)
                : bytes == 4 ? sge_la64_st_w(r, U(base), i12)
                             : sge_la64_st_d(r, U(base), i12));
}

/* ---- branches, calls and the frame (risc_asm.h's and risc_a64.h's) ----
   a local branch: its word's place, its offset patched once its target is known (rk_patch: 0, -1 out of reach) */
static inline size_t rk_bz(SgXBuf* b, int nonzero, int r) {
    size_t at = b->n;
    la_w(b, nonzero ? sge_la64_bnez(U(r), 0) : sge_la64_beqz(U(r), 0));
    return at;
}
static inline size_t rk_j(SgXBuf* b) {
    size_t at = b->n;
    la_w(b, sge_la64_b(0));
    return at;
}
static inline SnxU32 la_word_at(const SgXBuf* b, size_t at) {
    return (SnxU32)b->p[at] | (SnxU32)b->p[at + 1] << 8 | (SnxU32)b->p[at + 2] << 16 | (SnxU32)b->p[at + 3] << 24;
}
static inline void la_word_put(SgXBuf* b, size_t at, SnxU32 w) {
    b->p[at] = (unsigned char)w, b->p[at + 1] = (unsigned char)(w >> 8), b->p[at + 2] = (unsigned char)(w >> 16);
    b->p[at + 3] = (unsigned char)(w >> 24);
}
/* the branch at at to the byte offset to, its offset of words from itself rewritten: B's 26 bits (128 MiB), BEQZ's
   and BNEZ's 21 (4 MiB) */
static inline int rk_patch(SgXBuf* b, size_t at, size_t to) {
    long long d = ((long long)to - (long long)at) >> 2;
    SnxU32 w;
    if (at + 4 > b->cap) return 0;
    w = la_word_at(b, at);
    if ((w & 0xFC000000u) == sge_la64_b(0)) {
        if (d < -(1ll << 25) || d >= (1ll << 25)) return -1;
        w = sge_la64_b((SnxU32)d);
    } else {
        if (d < -(1ll << 20) || d >= (1ll << 20)) return -1;
        w = (w & 0xFC000000u) == sge_la64_bnez(0, 0) ? sge_la64_bnez((w >> 5) & 31, (SnxU32)d)
                                                      : sge_la64_beqz((w >> 5) & 31, (SnxU32)d);
    }
    la_word_put(b, at, w);
    return 0;
}
static inline void rk_call_reg(SgXBuf* b, int r) { la_w(b, sge_la64_jirl(LA_RA, U(r), 0)); }
static inline void rk_jump_reg(SgXBuf* b, int r) { la_w(b, sge_la64_jirl(LA_ZERO, U(r), 0)); }
static inline void rk_ret(SgXBuf* b) { rk_jump_reg(b, LA_RA); }
/* a call by the object's relocation (R_LARCH_CALL36: PCADDU18I and JIRL of ra, the relocation at the PCADDU18I, the
   place this gives); a tail call's of t8 and JIRL zero */
static inline size_t rk_call_reloc(SgXBuf* b) {
    size_t at = b->n;
    la_w(b, sge_la64_pcaddu18i(LA_RA, 0));
    la_w(b, sge_la64_jirl(LA_RA, LA_RA, 0));
    return at;
}
static inline size_t rk_tail_reloc(SgXBuf* b) {
    size_t at = b->n;
    la_w(b, sge_la64_pcaddu18i(RK_SA, 0));
    la_w(b, sge_la64_jirl(LA_ZERO, RK_SA, 0));
    return at;
}
/* a call's or tail call's PCADDU18I and JIRL at at made to reach to (128 GiB either way): 0, -1 out of reach */
static inline int rk_patch_call(SgXBuf* b, size_t at, size_t to) {
    long long d = (long long)to - (long long)at, hi = (d + 0x20000) >> 18;
    SnxU32 pc, jirl;
    if (at + 8 > b->cap) return 0;
    if (hi < -(1ll << 19) || hi >= (1ll << 19)) return -1;
    pc = la_word_at(b, at), jirl = la_word_at(b, at + 4);
    la_word_put(b, at, sge_la64_pcaddu18i(pc & 31, (SnxU32)hi & 0xfffff));
    la_word_put(b, at + 4, sge_la64_jirl(jirl & 31, (jirl >> 5) & 31, (SnxU32)((d - (hi << 18)) >> 2) & 0xffff));
    return 0;
}
/* d = the address of an 8-byte absolute address in the code (its relocation's place, which this gives): PCADDI d of
   it and B past it, the literal 8-aligned */
static inline size_t rk_literal(SgXBuf* b, int d) {
    size_t at;
    if ((b->n + 8) % 8) la_w(b, sge_la64_andi(LA_ZERO, LA_ZERO, 0));
    la_w(b, sge_la64_pcaddi(U(d), 2));
    la_w(b, sge_la64_b(3));
    at = b->n;
    sgx_imm(b, 0, 8);
    return at;
}
/* rk_literal where there's no frame (a module descriptor's functions): the link as it was */
static inline size_t rk_leaf_literal(SgXBuf* b, int d) { return rk_literal(b, d); }
/* a function's frame: ra, fp and s0-s8 stored below sp (ADDI.D sp of their room first), fp the frame's anchor, s0
   the state; then sp less the slots' room (LU12I.W and ORI of t8 patched: rk_fn_patch_frame, the place this gives).
   The epilogue takes sp back from fp */
#define LA_SAVED 96
static inline size_t rk_fn_prologue(SgXBuf* b, int pinned) {
    size_t at;
    SnxU32 k;
    (void)pinned;
    la_w(b, sge_la64_addi_d(LA_SP, LA_SP, la_i12(-LA_SAVED)));
    la_w(b, sge_la64_st_d(LA_RA, LA_SP, 80));
    la_w(b, sge_la64_st_d(LA_FP, LA_SP, 72));
    for (k = 0; k < 9; ++k) la_w(b, sge_la64_st_d(23 + k, LA_SP, 8 * k));
    rk_mv(b, LA_FP, LA_SP);
    rk_mv(b, 23, 4);
    at = b->n;
    la_w(b, sge_la64_lu12i_w(RK_SA, 0));
    la_w(b, sge_la64_ori(RK_SA, RK_SA, 0));
    la_w(b, sge_la64_sub_d(LA_SP, LA_SP, RK_SA));
    return at;
}
static inline void rk_fn_epilogue(SgXBuf* b, int pinned) {
    SnxU32 k;
    (void)pinned;
    rk_mv(b, LA_SP, LA_FP);
    for (k = 0; k < 9; ++k) la_w(b, sge_la64_ld_d(23 + k, LA_SP, 8 * k));
    la_w(b, sge_la64_ld_d(LA_FP, LA_SP, 72));
    la_w(b, sge_la64_ld_d(LA_RA, LA_SP, 80));
    la_w(b, sge_la64_addi_d(LA_SP, LA_SP, LA_SAVED));
}
static inline int rk_fn_patch_frame(SgXBuf* b, size_t at, long long slots) {
    long long size = (slots + 15) & ~15ll;
    if (size >= (1ll << 31)) return 0;
    if (at + 8 > b->cap) return 1;
    la_word_put(b, at, sge_la64_lu12i_w(RK_SA, (SnxU32)(size >> 12) & 0xfffff));
    la_word_put(b, at + 4, sge_la64_ori(RK_SA, RK_SA, (SnxU32)size & 0xfff));
    return 1;
}
/* DBAR 0, a full barrier (every one) */
static inline void rk_fence(SgXBuf* b) { la_w(b, sge_la64_dbar(0)); }
static inline void rk_nop(SgXBuf* b) { la_w(b, sge_la64_andi(LA_ZERO, LA_ZERO, 0)); }
/* LL and SC of 4 or 8 bytes at [a], a full barrier before the LL and after the SC (Arm's atomics' acquire and release
   at most); SC writes its register 1 where it stored, so the value goes through RK_SA and the status is its inverse */
static inline void rk_lr(SgXBuf* b, int d, int a, int bytes) {
    rk_fence(b);
    la_w(b, bytes == 8 ? sge_la64_ll_d(U(d), U(a), 0) : sge_la64_ll_w(U(d), U(a), 0));
}
static inline void rk_sc(SgXBuf* b, int status, int a, int v, int bytes) {
    rk_mv(b, RK_SA, v);
    la_w(b, bytes == 8 ? sge_la64_sc_d(RK_SA, U(a), 0) : sge_la64_sc_w(RK_SA, U(a), 0));
    la_w(b, sge_la64_xori(U(status), RK_SA, 1));
    rk_fence(b);
}
#define RK_LR32_SEXT 1

/* risc_a64.h's inline sequences' fixed registers: the guest memory's base a1, the address a2, the values a3 and a4;
   a5 to a7 their own */
#define RK_M_BASE 5
#define RK_M_ADDR 6
#define RK_M_V1 7
#define RK_M_V2 8
#define RK_M_T1 9
#define RK_M_T2 10
#define RK_M_T3 11

/* the state's address (s0, the callee's), a call's arguments from the state's (a0) on */
#define SGX_BASE 23
static const signed char sgx_arg_regs[6] = {4, 5, 6, 7, 8, 9};
static const signed char sgx_a32_args[4] = {5, 6, 7, 8};
static const signed char sgx_a32_strex[3] = {5, 6, 7};
#define SGX_A32_STREX_CLOBBER (1u << 8 | 1u << 9)
#undef U
