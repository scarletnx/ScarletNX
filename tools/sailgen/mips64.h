/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's mips64 back end (README.md, "The RISC back ends"): MIPS64 release 2's registers under the n64 ABI and the
   operations risc.h writes every primitive from, after machdef.h and before risc.h. Each word is an encoder's of
   LLVM's MIPS TableGen records (td/mips64_enc.inc, tdenc.py). Little-endian here (mips64el); a big-endian host's
   guest memory byte-reversed where RK_BIG_ENDIAN says so.

   A 32-bit operation of MIPS64 wants inputs sign extended, so the 32-bit primitives are 64-bit operations (their low
   32 bits are the same) or take their input extended first; the multiplies and divides go through HI and LO. Every
   branch and jump has its delay slot, a NOP.

   Registers: a0-a7, t0, t1, v0 and v1 hold values (v0 and v1 last: a call's results); at (RK_S1), t8 (RK_S2), t2 and
   t3 (the virtual flags) and t9 (RK_SA, which also carries a callee's address, as n64's PIC calls want) are reserved;
   s0 holds the state's address, s1-s7 and gp the pinned guest registers and the guest memory's base; fp the frame's
   anchor, sp the spill slots, ra the link; k0 and k1 are the kernel's */

#include "mips64_enc.inc"

#ifndef RK_BIG_ENDIAN
#define RK_BIG_ENDIAN 0
#endif

static inline void mips_w(SgXBuf* b, SnxU32 w) {
    if (RK_BIG_ENDIAN) {
        sgx_byte(b, (unsigned char)(w >> 24)), sgx_byte(b, (unsigned char)(w >> 16));
        sgx_byte(b, (unsigned char)(w >> 8)), sgx_byte(b, (unsigned char)w);
    } else
        sgx_imm(b, (long long)w, 4);
}
#define MIPS_NOP 0u /* sll zero, zero, 0 */

enum { SGX_F_NONE_MIPS = 1 };

#define SGX_NREG 32
#define SGX_FIX 32
#define SGX_RET 2
#define SGX_RET2 3
#define SGX_FRAME 29 /* sp */
#define MIPS_ZERO 0
#define MIPS_SP 29
#define MIPS_FP 30
#define MIPS_RA 31
#define RK_SA 25
#define RK_S1 1
#define RK_S2 24
#define RK_FA 14
#define RK_FB 15
static const signed char sgx_pool[] = {12, 13, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2};
#define SGX_CALLER_SAVED 0x3FFCu /* v0-t1 */

static inline int mips_fits16(long long k) { return k >= -32768 && k < 32768; }
static inline SnxU32 mips_i(long long k) { return (SnxU32)k & 0xffff; }
#define U(r) ((SnxU32)(r))

/* ---- the operations risc.h writes the primitives from ---- */

static inline void rk_mv(SgXBuf* b, int d, int a) {
    if (d != a) mips_w(b, sge_mips64_or(U(d), U(a), MIPS_ZERO));
}
/* any 64-bit constant into d alone: DADDIU or ORI of 16 bits, LUI and ORI of 32; else the high word's, shifted up
   past the low word's halves ORed in */
static inline void rk_li(SgXBuf* b, int d, SnxU64 k) {
    long long v = (long long)k;
    SnxU32 lo = (SnxU32)k;
    if (mips_fits16(v)) {
        mips_w(b, sge_mips64_daddiu(U(d), MIPS_ZERO, mips_i(v)));
        return;
    }
    if (k < 65536) {
        mips_w(b, sge_mips64_ori(U(d), MIPS_ZERO, (SnxU32)k));
        return;
    }
    if (v == (long long)(int)lo) {
        mips_w(b, sge_mips64_lui(U(d), lo >> 16));
        if (lo & 0xffff) mips_w(b, sge_mips64_ori(U(d), U(d), lo & 0xffff));
        return;
    }
    rk_li(b, d, (SnxU64)(long long)(int)(SnxU32)(k >> 32));
    mips_w(b, sge_mips64_dsll(U(d), U(d), 16));
    if (lo >> 16) mips_w(b, sge_mips64_ori(U(d), U(d), lo >> 16));
    mips_w(b, sge_mips64_dsll(U(d), U(d), 16));
    if (lo & 0xffff) mips_w(b, sge_mips64_ori(U(d), U(d), lo & 0xffff));
}
static inline void rk_add(SgXBuf* b, int d, int a, int c, int sf) {
    (void)sf;
    mips_w(b, sge_mips64_daddu(U(d), U(a), U(c)));
}
static inline void rk_sub(SgXBuf* b, int d, int a, int c, int sf) {
    (void)sf;
    mips_w(b, sge_mips64_dsubu(U(d), U(a), U(c)));
}
static inline void rk_neg(SgXBuf* b, int d, int a, int sf) { rk_sub(b, d, MIPS_ZERO, a, sf); }
static inline void rk_and(SgXBuf* b, int d, int a, int c) { mips_w(b, sge_mips64_and(U(d), U(a), U(c))); }
static inline void rk_or(SgXBuf* b, int d, int a, int c) { mips_w(b, sge_mips64_or(U(d), U(a), U(c))); }
static inline void rk_xor(SgXBuf* b, int d, int a, int c) { mips_w(b, sge_mips64_xor(U(d), U(a), U(c))); }
static inline void rk_addi(SgXBuf* b, int d, int a, long long k, int sf) {
    (void)sf;
    if (mips_fits16(k)) {
        mips_w(b, sge_mips64_daddiu(U(d), U(a), mips_i(k)));
        return;
    }
    rk_li(b, RK_SA, (SnxU64)k);
    rk_add(b, d, a, RK_SA, 1);
}
/* ANDI, ORI and XORI of 16 bits (zero extended); XOR of all ones NOR; else the constant in RK_SA */
#define MIPS_LOGI(name, op, rr)                                                                                        \
    static inline void rk_##name(SgXBuf* b, int d, int a, long long k) {                                               \
        if (k >= 0 && k < 65536) {                                                                                     \
            mips_w(b, sge_mips64_##op(U(d), U(a), (SnxU32)k));                                                         \
            return;                                                                                                    \
        }                                                                                                              \
        rk_li(b, RK_SA, (SnxU64)k);                                                                                    \
        rk_##rr(b, d, a, RK_SA);                                                                                       \
    }
MIPS_LOGI(andi, andi, and)
MIPS_LOGI(ori, ori, or)
static inline void rk_xori(SgXBuf* b, int d, int a, long long k) {
    if (k == -1) {
        mips_w(b, sge_mips64_nor(U(d), U(a), MIPS_ZERO));
        return;
    }
    if (k >= 0 && k < 65536) {
        mips_w(b, sge_mips64_xori(U(d), U(a), (SnxU32)k));
        return;
    }
    rk_li(b, RK_SA, (SnxU64)k);
    rk_xor(b, d, a, RK_SA);
}
static inline void rk_sext(SgXBuf* b, int d, int a, int bits) {
    mips_w(b, bits == 8    ? sge_mips64_seb(U(d), U(a))
              : bits == 16 ? sge_mips64_seh(U(d), U(a))
                           : sge_mips64_sll(U(d), U(a), 0));
}
static inline void rk_zext(SgXBuf* b, int d, int a, int bits) {
    mips_w(b, sge_mips64_dext(U(d), U(a), 0, (SnxU32)bits - 1));
}
/* shifts by a register: DSLLV, DSRLV and DSRAV read six bits (Arm's MOD 64); of 32 bits the amount MOD 32 first, the
   value zero (right) or sign (arithmetic) extended */
static inline void rk_sll(SgXBuf* b, int d, int a, int c, int sf) {
    if (!sf) mips_w(b, sge_mips64_andi(RK_SA, U(c), 31)), c = RK_SA;
    mips_w(b, sge_mips64_dsllv(U(d), U(a), U(c)));
}
static inline void rk_srl(SgXBuf* b, int d, int a, int c, int sf) {
    if (!sf) {
        mips_w(b, sge_mips64_andi(RK_SA, U(c), 31));
        rk_zext(b, d, a, 32);
        mips_w(b, sge_mips64_dsrlv(U(d), U(d), RK_SA));
        return;
    }
    mips_w(b, sge_mips64_dsrlv(U(d), U(a), U(c)));
}
static inline void rk_sra(SgXBuf* b, int d, int a, int c, int sf) {
    if (!sf) {
        mips_w(b, sge_mips64_andi(RK_SA, U(c), 31));
        rk_sext(b, d, a, 32);
        mips_w(b, sge_mips64_srav(U(d), U(d), RK_SA));
        return;
    }
    mips_w(b, sge_mips64_dsrav(U(d), U(a), U(c)));
}
static inline void rk_slli(SgXBuf* b, int d, int a, int k, int sf) {
    (void)sf;
    mips_w(b, k < 32 ? sge_mips64_dsll(U(d), U(a), (SnxU32)k) : sge_mips64_dsll32(U(d), U(a), (SnxU32)k - 32));
}
static inline void rk_srli(SgXBuf* b, int d, int a, int k, int sf) {
    if (!sf) {
        mips_w(b, sge_mips64_dext(U(d), U(a), (SnxU32)k, 31u - (SnxU32)k));
        return;
    }
    mips_w(b, k < 32 ? sge_mips64_dsrl(U(d), U(a), (SnxU32)k) : sge_mips64_dsrl32(U(d), U(a), (SnxU32)k - 32));
}
static inline void rk_srai(SgXBuf* b, int d, int a, int k, int sf) {
    if (!sf) {
        rk_sext(b, d, a, 32);
        mips_w(b, sge_mips64_sra(U(d), U(d), (SnxU32)k));
        return;
    }
    mips_w(b, k < 32 ? sge_mips64_dsra(U(d), U(a), (SnxU32)k) : sge_mips64_dsra32(U(d), U(a), (SnxU32)k - 32));
}
/* products and quotients through HI and LO */
static inline void rk_mul(SgXBuf* b, int d, int a, int c, int sf) {
    (void)sf;
    mips_w(b, sge_mips64_dmultu(U(a), U(c)));
    mips_w(b, sge_mips64_mflo(U(d)));
}
static inline void rk_mulhu(SgXBuf* b, int d, int a, int c) {
    mips_w(b, sge_mips64_dmultu(U(a), U(c)));
    mips_w(b, sge_mips64_mfhi(U(d)));
}
static inline void rk_mulh(SgXBuf* b, int d, int a, int c) {
    mips_w(b, sge_mips64_dmult(U(a), U(c)));
    mips_w(b, sge_mips64_mfhi(U(d)));
}
static inline void rk_divu(SgXBuf* b, int d, int a, int c) {
    mips_w(b, sge_mips64_ddivu(U(a), U(c)));
    mips_w(b, sge_mips64_mflo(U(d)));
}
static inline void rk_div(SgXBuf* b, int d, int a, int c) {
    mips_w(b, sge_mips64_ddiv(U(a), U(c)));
    mips_w(b, sge_mips64_mflo(U(d)));
}
static inline void rk_sltu(SgXBuf* b, int d, int a, int c) { mips_w(b, sge_mips64_sltu(U(d), U(a), U(c))); }
static inline void rk_slt(SgXBuf* b, int d, int a, int c) { mips_w(b, sge_mips64_slt(U(d), U(a), U(c))); }
static inline void rk_seqz(SgXBuf* b, int d, int a) { mips_w(b, sge_mips64_sltiu(U(d), U(a), 1)); }
static inline void rk_snez(SgXBuf* b, int d, int a) { mips_w(b, sge_mips64_sltu(U(d), MIPS_ZERO, U(a))); }
/* a load or a store of bytes at base + index + disp: the 16-bit offset where it fits, else the address made in RK_SA */
static inline void rk_ldst(SgXBuf* b, int load, int bytes, int sign, int rt, int base, int index, int disp) {
    if (index >= 0 || !mips_fits16(disp)) {
        /* RK_SA = the displacement past 16 bits (LUI's), then the base and the index added */
        long long lo = (long long)(short)(disp & 0xffff), hi = (long long)disp - lo;
        if (hi) {
            rk_li(b, RK_SA, (SnxU64)hi);
            mips_w(b, sge_mips64_daddu(RK_SA, RK_SA, U(base)));
        } else
            rk_mv(b, RK_SA, base);
        if (index >= 0) mips_w(b, sge_mips64_daddu(RK_SA, RK_SA, U(index)));
        base = RK_SA;
        disp = (int)lo;
    }
    {
        SnxU32 r = U(rt), a = U(base), o = mips_i(disp);
        if (load)
            mips_w(b, bytes == 1   ? (sign ? sge_mips64_lb(r, a, o) : sge_mips64_lbu(r, a, o))
                      : bytes == 2 ? (sign ? sge_mips64_lh(r, a, o) : sge_mips64_lhu(r, a, o))
                      : bytes == 4 ? (sign ? sge_mips64_lw(r, a, o) : sge_mips64_lwu(r, a, o))
                                   : sge_mips64_ld(r, a, o));
        else
            mips_w(b, bytes == 1   ? sge_mips64_sb(r, a, o)
                      : bytes == 2 ? sge_mips64_sh(r, a, o)
                      : bytes == 4 ? sge_mips64_sw(r, a, o)
                                   : sge_mips64_sd(r, a, o));
    }
}

/* a big-endian host's: d = the low bytes of a reversed (MIPS64r2's WSBH, DSBH and DSHD) */
#define RK_BSWAP 1
static inline void rk_bswap(SgXBuf* b, int d, int a, int bytes) {
    if (bytes == 8) {
        mips_w(b, sge_mips64_dsbh(U(d), U(a)));
        mips_w(b, sge_mips64_dshd(U(d), U(d)));
        return;
    }
    mips_w(b, sge_mips64_wsbh(U(d), U(a)));
    if (bytes == 4) {
        mips_w(b, sge_mips64_rotr(U(d), U(d), 16));
        rk_zext(b, d, d, 32);
    } else
        rk_zext(b, d, d, 16);
}
/* guest memory's loads and stores (risc.h's moves): MIPS faults where an access isn't aligned and Arm's don't, so
   LDL and LDR, LWL and LWR (and their stores) take a doubleword's or a word's two parts, a halfword's two bytes (the
   guest's low byte the lower address's, on either host); the address made in RK_SA first where it needs one or the
   loaded register is its base. A big-endian host's word and doubleword are byte-reversed after the load, before the
   store (into RK_S1). A move is the only operation around it: RK_S1 is its too */
#define RK_GLDST 1
static inline void rk_gldst(SgXBuf* b, int load, int bytes, int sign, int rt, int base, int index, int disp) {
    SnxU32 r = U(rt), a, lo, hi, first, last;
    if (bytes == 1) {
        rk_ldst(b, load, bytes, sign, rt, base, index, disp);
        return;
    }
    if (index >= 0 || !mips_fits16(disp) || !mips_fits16((long long)disp + bytes - 1) || (load && rt == base)) {
        if (index >= 0) mips_w(b, sge_mips64_daddu(RK_SA, U(base), U(index)));
        else
            rk_mv(b, RK_SA, base);
        if (disp) {
            rk_li(b, RK_S1, (SnxU64)(long long)disp);
            mips_w(b, sge_mips64_daddu(RK_SA, RK_SA, RK_S1));
        }
        base = RK_SA;
        disp = 0;
    }
    a = U(base), lo = mips_i(disp), hi = mips_i((long long)disp + bytes - 1);
    /* LDR/LWR take the least significant end: the lower address's on a little-endian host, the higher's on a big */
    first = RK_BIG_ENDIAN ? hi : lo, last = RK_BIG_ENDIAN ? lo : hi;
    if (bytes == 2) {
        if (load) {
            mips_w(b, sign ? sge_mips64_lb(RK_S1, a, hi) : sge_mips64_lbu(RK_S1, a, hi));
            mips_w(b, sge_mips64_lbu(r, a, lo));
            mips_w(b, sge_mips64_dsll(RK_S1, RK_S1, 8));
            mips_w(b, sge_mips64_or(r, r, RK_S1));
        } else {
            mips_w(b, sge_mips64_sb(r, a, lo));
            mips_w(b, sge_mips64_dsrl(RK_S1, r, 8));
            mips_w(b, sge_mips64_sb(RK_S1, a, hi));
        }
        return;
    }
    if (load) {
        if (bytes == 8) {
            mips_w(b, sge_mips64_ldr(r, a, first));
            mips_w(b, sge_mips64_ldl(r, a, last));
            if (RK_BIG_ENDIAN) rk_bswap(b, rt, rt, 8);
        } else {
            mips_w(b, sge_mips64_lwr(r, a, first));
            mips_w(b, sge_mips64_lwl(r, a, last));
            if (RK_BIG_ENDIAN) rk_bswap(b, rt, rt, 4);
            if (sign) rk_sext(b, rt, rt, 32);
            else if (!RK_BIG_ENDIAN)
                rk_zext(b, rt, rt, 32);
        }
        return;
    }
    if (RK_BIG_ENDIAN) {
        rk_bswap(b, RK_S1, rt, bytes);
        r = RK_S1;
    }
    if (bytes == 8) {
        mips_w(b, sge_mips64_sdr(r, a, first));
        mips_w(b, sge_mips64_sdl(r, a, last));
    } else {
        mips_w(b, sge_mips64_swr(r, a, first));
        mips_w(b, sge_mips64_swl(r, a, last));
    }
}

/* ---- branches, calls and the frame ---- */

static inline SnxU32 mips_word_at(const SgXBuf* b, size_t at) {
    return RK_BIG_ENDIAN ? (SnxU32)b->p[at] << 24 | (SnxU32)b->p[at + 1] << 16 | (SnxU32)b->p[at + 2] << 8 | (SnxU32)b->p[at + 3]
                         : (SnxU32)b->p[at] | (SnxU32)b->p[at + 1] << 8 | (SnxU32)b->p[at + 2] << 16 |
                               (SnxU32)b->p[at + 3] << 24;
}
static inline void mips_word_put(SgXBuf* b, size_t at, SnxU32 w) {
    size_t n = b->n;
    b->n = at;
    mips_w(b, w);
    b->n = n;
}
/* a branch on r: BEQ or BNE of it and zero, its delay slot's NOP */
static inline size_t rk_bz(SgXBuf* b, int nonzero, int r) {
    size_t at = b->n;
    mips_w(b, nonzero ? sge_mips64_bne(U(r), MIPS_ZERO, 0) : sge_mips64_beq(U(r), MIPS_ZERO, 0));
    mips_w(b, MIPS_NOP);
    return at;
}
/* B (BEQ zero, zero: 128 KiB either way, the code's position its own) */
static inline size_t rk_j(SgXBuf* b) { return rk_bz(b, 0, MIPS_ZERO); }
/* the branch at at to the byte offset to: its 16-bit offset of words from the delay slot */
static inline int rk_patch(SgXBuf* b, size_t at, size_t to) {
    long long d = ((long long)to - (long long)at - 4) / 4;
    SnxU32 w;
    if (at + 4 > b->cap) return 0;
    if (d < -32768 || d >= 32768) return -1;
    w = mips_word_at(b, at);
    mips_word_put(b, at, (w & 0xFFFF0000u) | ((SnxU32)d & 0xffff));
    return 0;
}
/* a jump past rk_j's reach, within 2 GiB: t9 the distance (LUI, in BAL's delay slot, and DADDIU, patched) from BAL's
   link, the word after that slot (ra is the frame's, saved) */
static inline size_t rk_j_far(SgXBuf* b) {
    size_t at = b->n;
    mips_w(b, sge_mips64_bal(1));
    mips_w(b, sge_mips64_lui(RK_SA, 0));
    mips_w(b, sge_mips64_daddiu(RK_SA, RK_SA, 0));
    mips_w(b, sge_mips64_daddu(RK_SA, RK_SA, MIPS_RA));
    mips_w(b, sge_mips64_jr(RK_SA));
    mips_w(b, MIPS_NOP);
    return at;
}
static inline int rk_patch_far(SgXBuf* b, size_t at, size_t to) {
    long long d = (long long)to - (long long)at - 8, hi = (d + 0x8000) >> 16;
    if (at + 12 > b->cap) return 0;
    if (hi < -32768 || hi >= 32768) return -1;
    mips_word_put(b, at + 4, sge_mips64_lui(RK_SA, (SnxU32)hi & 0xffff));
    mips_word_put(b, at + 8, sge_mips64_daddiu(RK_SA, RK_SA, mips_i(d - (hi << 16))));
    return 0;
}
/* calls and jumps through t9 (an n64 PIC callee's address) */
static inline void rk_call_reg(SgXBuf* b, int r) {
    rk_mv(b, RK_SA, r);
    mips_w(b, sge_mips64_jalr(MIPS_RA, RK_SA));
    mips_w(b, MIPS_NOP);
}
static inline void rk_jump_reg(SgXBuf* b, int r) {
    rk_mv(b, RK_SA, r);
    mips_w(b, sge_mips64_jr(RK_SA));
    mips_w(b, MIPS_NOP);
}
static inline void rk_ret(SgXBuf* b) {
    mips_w(b, sge_mips64_jr(MIPS_RA));
    mips_w(b, MIPS_NOP);
}
/* d = the address of a literal in the code: BAL past it (the link its address: ra is the frame's, saved), 8-aligned;
   the place of its relocation. It is the distance of a symbol from it (R_MIPS_PC32, in its first word; a module's code
   holds no address, which would be a text relocation), or the symbol's address in a JIT unit (R_MIPS_64, b->absolute) */
static inline size_t rk_literal(SgXBuf* b, int d) {
    size_t at;
    if ((b->n + 8) % 8) mips_w(b, MIPS_NOP);
    mips_w(b, sge_mips64_bal(3)); /* to the delay slot + 12: past the literal */
    mips_w(b, MIPS_NOP);
    at = b->n;
    sgx_imm(b, 0, 8);
    rk_mv(b, d, MIPS_RA);
    return at;
}
/* rk_literal where there's no frame (a module descriptor's functions): ra, which its BAL writes, kept in at around
   it */
static inline size_t rk_leaf_literal(SgXBuf* b, int d) {
    size_t at;
    rk_mv(b, RK_S1, MIPS_RA);
    at = rk_literal(b, d);
    rk_mv(b, MIPS_RA, RK_S1);
    return at;
}
/* d = the address the literal at lit (rk_literal's d) stands for, t a scratch (d, where lit isn't) */
static inline void rk_literal_value(SgXBuf* b, int d, int lit, int t) {
    if (b->absolute) {
        rk_ldst(b, 1, 8, 0, d, lit, -1, 0);
        return;
    }
    rk_ldst(b, 1, 4, 1, t, lit, -1, 0);
    rk_add(b, d, lit, t, 1);
}
/* a call by the object's relocation: the callee's literal (the place this gives), made its address in t9; a tail call
   keeps ra, the caller's, in RK_S1 around the BAL */
static inline size_t rk_call_reloc(SgXBuf* b) {
    size_t at = rk_literal(b, RK_SA);
    rk_literal_value(b, RK_SA, RK_SA, RK_S2);
    rk_call_reg(b, RK_SA);
    return at;
}
static inline size_t rk_tail_reloc(SgXBuf* b) {
    size_t at;
    rk_mv(b, RK_S1, MIPS_RA);
    at = rk_literal(b, RK_SA);
    rk_mv(b, MIPS_RA, RK_S1);
    rk_literal_value(b, RK_SA, RK_SA, RK_S2);
    rk_jump_reg(b, RK_SA);
    return at;
}
static inline int rk_patch_call(SgXBuf* b, size_t at, size_t to) {
    (void)b, (void)at, (void)to;
    return -1;
}
static inline void rk_fence(SgXBuf* b) { mips_w(b, sge_mips64_sync(0)); }
static inline void rk_nop(SgXBuf* b) { mips_w(b, MIPS_NOP); }
/* LL or LLD and SC or SCD at [a]: SC writes its register 1 where it stored, so the value goes through RK_SA and the
   status is its inverse */
static inline void rk_lr(SgXBuf* b, int d, int a, int bytes) {
    mips_w(b, bytes == 8 ? sge_mips64_lld(U(d), U(a), 0) : sge_mips64_ll(U(d), U(a), 0));
}
static inline void rk_sc(SgXBuf* b, int status, int a, int v, int bytes) {
    rk_mv(b, RK_SA, v);
    mips_w(b, bytes == 8 ? sge_mips64_scd(RK_SA, U(a), 0) : sge_mips64_sc(RK_SA, U(a), 0));
    mips_w(b, sge_mips64_xori(U(status), RK_SA, 1));
}
#define RK_LR32_SEXT 1
/* the frame: ra, gp, fp and s0-s7 stored below sp (DADDIU sp of their room first), fp the frame's anchor, s0 the
   state; then sp less the slots' room (DADDIU patched, its place this gives). The epilogue takes sp back from fp */
#define MIPS_SAVED 96
static inline size_t rk_fn_prologue(SgXBuf* b, int pinned) {
    size_t at;
    int k;
    static const SnxU32 regs[11] = {31, 28, 30, 16, 17, 18, 19, 20, 21, 22, 23};
    (void)pinned;
    mips_w(b, sge_mips64_daddiu(MIPS_SP, MIPS_SP, mips_i(-MIPS_SAVED)));
    for (k = 0; k < 11; ++k) mips_w(b, sge_mips64_sd(regs[k], MIPS_SP, (SnxU32)(8 * k)));
    rk_mv(b, MIPS_FP, MIPS_SP);
    rk_mv(b, 16, 4);
    at = b->n;
    mips_w(b, sge_mips64_daddiu(MIPS_SP, MIPS_SP, 0));
    return at;
}
static inline void rk_fn_epilogue(SgXBuf* b, int pinned) {
    int k;
    static const SnxU32 regs[11] = {31, 28, 30, 16, 17, 18, 19, 20, 21, 22, 23};
    (void)pinned;
    rk_mv(b, MIPS_SP, MIPS_FP);
    for (k = 10; k >= 0; --k) mips_w(b, sge_mips64_ld(regs[k], MIPS_SP, (SnxU32)(8 * k)));
    mips_w(b, sge_mips64_daddiu(MIPS_SP, MIPS_SP, MIPS_SAVED));
}
static inline int rk_fn_patch_frame(SgXBuf* b, size_t at, long long slots) {
    long long size = (slots + 15) & ~15ll;
    if (size >= 32768) return 0;
    if (at + 4 > b->cap) return 1;
    mips_word_put(b, at, sge_mips64_daddiu(MIPS_SP, MIPS_SP, mips_i(-size)));
    return 1;
}

/* risc_a64.h's inline sequences' fixed registers: the guest memory's base a1, the address a2, the values a3 and a4;
   a5 to a7 their own */
#define RK_M_BASE 5
#define RK_M_ADDR 6
#define RK_M_V1 7
#define RK_M_V2 8
#define RK_M_T1 9
#define RK_M_T2 10
#define RK_M_T3 11

#define SGX_BASE 16
static const signed char sgx_arg_regs[6] = {4, 5, 6, 7, 8, 9};
static const signed char sgx_a32_args[4] = {5, 6, 7, 8};
static const signed char sgx_a32_strex[3] = {5, 6, 7};
#define SGX_A32_STREX_CLOBBER (1u << 8 | 1u << 9)
#undef U
