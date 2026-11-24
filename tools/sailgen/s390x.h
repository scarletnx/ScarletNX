/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's s390x back end (README.md, "The RISC back ends"): z/Architecture's registers under the s390x ELF ABI and
   the operations risc.h writes every primitive from, after machdef.h and before risc.h. Each instruction is an
   encoder's of LLVM's SystemZ TableGen records (td/s390x_enc.inc, tdenc.py); z196's (distinct operands) and before.
   Big-endian: the guest's memory and its vector registers' bytes are read and written byte-reversed (LRVG, STRVG...).

   Sixteen registers: r2-r9 hold values (r2 and r3 last: a call's results); r0 (the even half of MLGR's, DLGR's and
   DSGR's pair, and CS's compare value), r1 (RK_SA, the odd half), r10 and r11 (the virtual flags), r12 (RK_S1) and
   r14 (RK_S2: the link, saved by the frame and free between calls) are reserved; r13 holds the state's address and
   r15 is the stack: no register is left for pinned guest registers. The spill slots are past the 160 bytes of
   register save area a callee may write and the 8 of a call's sixth argument (the ABI's: its stack slot) */

#include "s390x_enc.inc"

#ifndef RK_BIG_ENDIAN
#define RK_BIG_ENDIAN 1
#endif
#if !RK_BIG_ENDIAN
#error "s390x is big-endian"
#endif

/* an instruction of 2, 4 or 6 bytes (the encoder's value in its low ones; an opcode's first byte is never 0 in a 4-
   or 6-byte one), the high byte first */
static inline void zs_w(SgXBuf* b, SnxU64 w) {
    int n = w >> 32 ? 6 : w >> 16 ? 4 : 2;
    for (int k = n - 1; k >= 0; --k) sgx_byte(b, (unsigned char)(w >> (8 * k)));
}

enum { SGX_F_NONE_ZS = 1 };

#define SGX_NREG 16
#define SGX_FIX 16
#define SGX_RET 2
#define SGX_RET2 3
#define SGX_FRAME 15 /* r15: its slots at ZS_SLOT0 past it (rk_ldst) */
#define ZS_R0 0
#define ZS_LINK 14
#define ZS_SP 15
#define ZS_SLOT0 168
#define RK_SA 1
#define RK_S1 12
#define RK_S2 14
#define RK_TAIL 4 /* a tail call's target past the epilogue, which restores r12: r2 and r3 are its arguments */
#define RK_FA 11
#define RK_FB 10
static const signed char sgx_pool[] = {9, 8, 7, 6, 5, 4, 3, 2};
/* the scratch of code before the frame (risc_arm.inc's entry checks): r12 is the callee's and r14 the link, so r0
   and r1 */
#define RK_ENTRY_S1 0
#define RK_ENTRY_S2 1
#define SGX_CALLER_SAVED 0x3Cu /* r2-r5; r6-r9 are the callee's */

static inline int zs_fits16(long long k) { return k >= -32768 && k < 32768; }
static inline int zs_fits20(long long k) { return k >= -(1ll << 19) && k < (1ll << 19); }
static inline int zs_fits32(long long k) { return k == (long long)(int)k; }
#define U(r) ((SnxU32)(r))
#define M(k) ((SnxU32)(k))

/* ---- the operations risc.h writes the primitives from ---- */

static inline void rk_mv(SgXBuf* b, int d, int a) {
    if (d != a) zs_w(b, sge_s390x_lgr(U(d), U(a)));
}
/* any 64-bit constant into d alone: LGHI, LGFI or LLILF of one that fits; else LLIHF of the high word, OILF the low */
static inline void rk_li(SgXBuf* b, int d, SnxU64 k) {
    long long v = (long long)k;
    if (zs_fits16(v)) zs_w(b, sge_s390x_lghi(U(d), M(v) & 0xffff));
    else if (zs_fits32(v))
        zs_w(b, sge_s390x_lgfi(U(d), M(v)));
    else if (!(k >> 32))
        zs_w(b, sge_s390x_llilf(U(d), M(k)));
    else {
        zs_w(b, sge_s390x_llihf(U(d), M(k >> 32)));
        if ((SnxU32)k) zs_w(b, sge_s390x_oilf(U(d), M(k)));
    }
}
static inline void rk_add(SgXBuf* b, int d, int a, int c, int sf) {
    (void)sf;
    zs_w(b, sge_s390x_agrk(U(d), U(a), U(c)));
}
static inline void rk_sub(SgXBuf* b, int d, int a, int c, int sf) {
    (void)sf;
    zs_w(b, sge_s390x_sgrk(U(d), U(a), U(c)));
}
static inline void rk_neg(SgXBuf* b, int d, int a, int sf) {
    (void)sf;
    zs_w(b, sge_s390x_lcgr(U(d), U(a)));
}
static inline void rk_and(SgXBuf* b, int d, int a, int c) { zs_w(b, sge_s390x_ngrk(U(d), U(a), U(c))); }
static inline void rk_or(SgXBuf* b, int d, int a, int c) { zs_w(b, sge_s390x_ogrk(U(d), U(a), U(c))); }
static inline void rk_xor(SgXBuf* b, int d, int a, int c) { zs_w(b, sge_s390x_xgrk(U(d), U(a), U(c))); }
static inline void rk_addi(SgXBuf* b, int d, int a, long long k, int sf) {
    (void)sf;
    if (zs_fits16(k)) zs_w(b, sge_s390x_aghik(U(d), U(a), M(k) & 0xffff));
    else if (zs_fits32(k)) {
        rk_mv(b, d, a);
        zs_w(b, sge_s390x_agfi(U(d), M(k)));
    } else {
        rk_li(b, RK_SA, (SnxU64)k);
        rk_add(b, d, a, RK_SA, 1);
    }
}
/* the logical immediates on d's halves and quarters (NILL, OILL, XILF... leave the rest as it was), so a constant
   whose other bits are all ones (AND) or zeros (OR, XOR) takes one or two; a small positive AND zero extends after.
   Else the constant in RK_SA */
static inline void rk_andi(SgXBuf* b, int d, int a, long long k) {
    if (k >= 0 && k < 65536) {
        rk_mv(b, d, a);
        zs_w(b, sge_s390x_nill(U(d), M(k)));
        zs_w(b, sge_s390x_llghr(U(d), U(d)));
    } else if (k < 0 && k >= -65536) {
        rk_mv(b, d, a);
        zs_w(b, sge_s390x_nill(U(d), M(k) & 0xffff));
    } else if (k < 0 && zs_fits32(k)) {
        rk_mv(b, d, a);
        zs_w(b, sge_s390x_nilf(U(d), M(k)));
    } else {
        rk_li(b, RK_SA, (SnxU64)k);
        rk_and(b, d, a, RK_SA);
    }
}
static inline void rk_ori(SgXBuf* b, int d, int a, long long k) {
    if (k >= 0 && k < 65536) {
        rk_mv(b, d, a);
        zs_w(b, sge_s390x_oill(U(d), M(k)));
    } else if (k >= 0 && !((SnxU64)k >> 32)) {
        rk_mv(b, d, a);
        zs_w(b, sge_s390x_oilf(U(d), M(k)));
    } else if (k < 0 && zs_fits32(k)) {
        rk_mv(b, d, a);
        zs_w(b, sge_s390x_oihf(U(d), 0xffffffffu));
        zs_w(b, sge_s390x_oilf(U(d), M(k)));
    } else {
        rk_li(b, RK_SA, (SnxU64)k);
        rk_or(b, d, a, RK_SA);
    }
}
static inline void rk_xori(SgXBuf* b, int d, int a, long long k) {
    if (zs_fits32(k) || !((SnxU64)k >> 32)) {
        rk_mv(b, d, a);
        if ((SnxU32)k) zs_w(b, sge_s390x_xilf(U(d), M(k)));
        if (k < 0) zs_w(b, sge_s390x_xihf(U(d), 0xffffffffu));
    } else {
        rk_li(b, RK_SA, (SnxU64)k);
        rk_xor(b, d, a, RK_SA);
    }
}
static inline void rk_sext(SgXBuf* b, int d, int a, int bits) {
    zs_w(b, bits == 8    ? sge_s390x_lgbr(U(d), U(a))
            : bits == 16 ? sge_s390x_lghr(U(d), U(a))
                         : sge_s390x_lgfr(U(d), U(a)));
}
static inline void rk_zext(SgXBuf* b, int d, int a, int bits) {
    zs_w(b, bits == 8    ? sge_s390x_llgcr(U(d), U(a))
            : bits == 16 ? sge_s390x_llghr(U(d), U(a))
                         : sge_s390x_llgfr(U(d), U(a)));
}
/* shifts: SLLG, SRLG and SRAG by the low 6 bits of an address (a register's, or the constant's alone); of 32 bits the
   amount MOD 32 first (NILL's of a copy in RK_SA: its low 6 bits are all the shift reads), the value zero (right) or
   sign (arithmetic) extended */
static inline SnxU32 zs_amount32(SgXBuf* b, int c) {
    rk_mv(b, RK_SA, c);
    zs_w(b, sge_s390x_nill(RK_SA, 31));
    return RK_SA;
}
static inline void rk_sll(SgXBuf* b, int d, int a, int c, int sf) {
    SnxU32 n = sf ? U(c) : zs_amount32(b, c);
    zs_w(b, sge_s390x_sllg(U(d), U(a), n, 0));
}
static inline void rk_srl(SgXBuf* b, int d, int a, int c, int sf) {
    SnxU32 n = sf ? U(c) : zs_amount32(b, c);
    if (!sf) {
        rk_zext(b, d, a, 32);
        a = d;
    }
    zs_w(b, sge_s390x_srlg(U(d), U(a), n, 0));
}
static inline void rk_sra(SgXBuf* b, int d, int a, int c, int sf) {
    SnxU32 n = sf ? U(c) : zs_amount32(b, c);
    if (!sf) {
        rk_sext(b, d, a, 32);
        a = d;
    }
    zs_w(b, sge_s390x_srag(U(d), U(a), n, 0));
}
static inline void rk_slli(SgXBuf* b, int d, int a, int k, int sf) {
    (void)sf;
    zs_w(b, sge_s390x_sllg(U(d), U(a), 0, M(k)));
}
static inline void rk_srli(SgXBuf* b, int d, int a, int k, int sf) {
    if (!sf) {
        rk_zext(b, d, a, 32);
        a = d;
    }
    zs_w(b, sge_s390x_srlg(U(d), U(a), 0, M(k)));
}
static inline void rk_srai(SgXBuf* b, int d, int a, int k, int sf) {
    if (!sf) {
        rk_sext(b, d, a, 32);
        a = d;
    }
    zs_w(b, sge_s390x_srag(U(d), U(a), 0, M(k)));
}
/* MSGR's low product; MLGR's 128-bit one in r0 (high) and r1 (low), the multiplicand r1's; the signed high from the
   unsigned less each operand where the other is negative; DSGR's and DLGR's quotient in r1, the dividend r1's (r0:r1
   for DLGR's) */
static inline void rk_mul(SgXBuf* b, int d, int a, int c, int sf) {
    (void)sf;
    if (d == c) c = a, a = d;
    rk_mv(b, d, a);
    zs_w(b, sge_s390x_msgr(U(d), U(c)));
}
static inline void rk_mulhu(SgXBuf* b, int d, int a, int c) {
    rk_mv(b, RK_SA, a);
    zs_w(b, sge_s390x_mlgr(ZS_R0, U(c)));
    rk_mv(b, d, ZS_R0);
}
static inline void rk_mulh(SgXBuf* b, int d, int a, int c) {
    rk_mv(b, RK_SA, a);
    zs_w(b, sge_s390x_mlgr(ZS_R0, U(c)));
    zs_w(b, sge_s390x_srag(RK_SA, U(a), 0, 63));
    zs_w(b, sge_s390x_ngr(RK_SA, U(c)));
    zs_w(b, sge_s390x_sgr(ZS_R0, RK_SA));
    zs_w(b, sge_s390x_srag(RK_SA, U(c), 0, 63));
    zs_w(b, sge_s390x_ngr(RK_SA, U(a)));
    zs_w(b, sge_s390x_sgr(ZS_R0, RK_SA));
    rk_mv(b, d, ZS_R0);
}
static inline void rk_divu(SgXBuf* b, int d, int a, int c) {
    zs_w(b, sge_s390x_lghi(ZS_R0, 0));
    rk_mv(b, RK_SA, a);
    zs_w(b, sge_s390x_dlgr(ZS_R0, U(c)));
    rk_mv(b, d, RK_SA);
}
static inline void rk_div(SgXBuf* b, int d, int a, int c) {
    rk_mv(b, RK_SA, a);
    zs_w(b, sge_s390x_dsgr(ZS_R0, U(c)));
    rk_mv(b, d, RK_SA);
}
/* d = 1 where the compare's condition code is in mask, else 0: the compare first (d may be an operand), LGHI keeps
   the code, BRC past the 0 */
static inline void zs_set(SgXBuf* b, int d, SnxU32 mask) {
    zs_w(b, sge_s390x_lghi(U(d), 1));
    zs_w(b, sge_s390x_brc(mask, 4)); /* 8 bytes on: past the LGHI of 0 */
    zs_w(b, sge_s390x_lghi(U(d), 0));
}
static inline void rk_sltu(SgXBuf* b, int d, int a, int c) {
    zs_w(b, sge_s390x_clgr(U(a), U(c)));
    zs_set(b, d, 4); /* CC 1: the first low */
}
static inline void rk_slt(SgXBuf* b, int d, int a, int c) {
    zs_w(b, sge_s390x_cgr(U(a), U(c)));
    zs_set(b, d, 4);
}
static inline void rk_seqz(SgXBuf* b, int d, int a) {
    zs_w(b, sge_s390x_cghi(U(a), 0));
    zs_set(b, d, 8); /* CC 0: equal */
}
static inline void rk_snez(SgXBuf* b, int d, int a) {
    zs_w(b, sge_s390x_cghi(U(a), 0));
    zs_set(b, d, 6); /* CC 1 or 2 */
}
/* a load or a store of bytes at base + index + disp: RXY's 20-bit displacement and index register; where the
   displacement doesn't fit, it in RK_SA as the index (the index added to it). A spill slot's (base r15) is ZS_SLOT0
   past the frame's start */
static inline void zs_addr(SgXBuf* b, int base, int* index, int* disp) {
    if (base == SGX_FRAME) *disp += ZS_SLOT0;
    if (!zs_fits20(*disp)) {
        rk_li(b, RK_SA, (SnxU64)(long long)*disp);
        if (*index >= 0) rk_add(b, RK_SA, RK_SA, *index, 1);
        *index = RK_SA;
        *disp = 0;
    }
}
static inline void rk_ldst(SgXBuf* b, int load, int bytes, int sign, int rt, int base, int index, int disp) {
    SnxU32 r = U(rt), bb = U(base), x, dd;
    zs_addr(b, base, &index, &disp);
    x = index >= 0 ? U(index) : 0, dd = M(disp) & 0xfffff;
    if (load)
        zs_w(b, bytes == 1   ? (sign ? sge_s390x_lgb(r, bb, dd, x) : sge_s390x_llgc(r, bb, dd, x))
                : bytes == 2 ? (sign ? sge_s390x_lgh(r, bb, dd, x) : sge_s390x_llgh(r, bb, dd, x))
                : bytes == 4 ? (sign ? sge_s390x_lgf(r, bb, dd, x) : sge_s390x_llgf(r, bb, dd, x))
                             : sge_s390x_lg(r, bb, dd, x));
    else
        zs_w(b, bytes == 1   ? sge_s390x_stcy(r, bb, dd, x)
                : bytes == 2 ? sge_s390x_sthy(r, bb, dd, x)
                : bytes == 4 ? sge_s390x_sty(r, bb, dd, x)
                             : sge_s390x_stg(r, bb, dd, x));
}
/* d = the low bytes of a reversed: LRVGR, LRVR (the low word's alone), zero extended */
#define RK_BSWAP 1
static inline void rk_bswap(SgXBuf* b, int d, int a, int bytes) {
    if (bytes == 8) {
        zs_w(b, sge_s390x_lrvgr(U(d), U(a)));
        return;
    }
    zs_w(b, sge_s390x_lrvr(U(d), U(a)));
    rk_zext(b, d, d, 32);
    if (bytes == 2) rk_srli(b, d, d, 16, 1);
}
/* guest memory's loads and stores (risc.h's moves): LRVG, LRV and LRVH (into the register's low bytes, the rest as
   it was: extended after) and their stores, a byte's the plain one's */
#define RK_GLDST 1
static inline void rk_gldst(SgXBuf* b, int load, int bytes, int sign, int rt, int base, int index, int disp) {
    SnxU32 r = U(rt), bb = U(base), x, dd;
    if (bytes == 1) {
        rk_ldst(b, load, bytes, sign, rt, base, index, disp);
        return;
    }
    zs_addr(b, base, &index, &disp);
    x = index >= 0 ? U(index) : 0, dd = M(disp) & 0xfffff;
    if (load) {
        zs_w(b, bytes == 2   ? sge_s390x_lrvh(r, bb, dd, x)
                : bytes == 4 ? sge_s390x_lrv(r, bb, dd, x)
                             : sge_s390x_lrvg(r, bb, dd, x));
        if (bytes < 8) (sign ? rk_sext : rk_zext)(b, rt, rt, 8 * bytes);
    } else
        zs_w(b, bytes == 2   ? sge_s390x_strvh(r, bb, dd, x)
                : bytes == 4 ? sge_s390x_strv(r, bb, dd, x)
                             : sge_s390x_strvg(r, bb, dd, x));
}

/* ---- branches, calls and the frame ---- */

/* a branch on r: LTGR, then BRCL of CC 0 (zero) or not (4 GiB either way, its offset of halfwords from itself) */
static inline size_t rk_bz(SgXBuf* b, int nonzero, int r) {
    size_t at;
    zs_w(b, sge_s390x_ltgr(U(r), U(r)));
    at = b->n;
    zs_w(b, sge_s390x_brcl(nonzero ? 7 : 8, 0));
    return at;
}
static inline size_t rk_j(SgXBuf* b) {
    size_t at = b->n;
    zs_w(b, sge_s390x_brcl(15, 0));
    return at;
}
/* the BRCL at at to the byte offset to: its 32-bit offset of halfwords, the high byte first */
static inline int rk_patch(SgXBuf* b, size_t at, size_t to) {
    long long d = ((long long)to - (long long)at) / 2;
    if (at + 6 > b->cap) return 0;
    if (!zs_fits32(d)) return -1;
    for (int k = 0; k < 4; ++k) b->p[at + 2 + k] = (unsigned char)((SnxU64)d >> (24 - 8 * k));
    return 0;
}
/* a call's sixth argument: the ABI's at 160 past the stack pointer, whichever call has one (every call is in a frame,
   whose room it is) */
static inline void zs_arg6(SgXBuf* b) { zs_w(b, sge_s390x_stg(7, ZS_SP, 160, 0)); }
static inline void rk_call_reg(SgXBuf* b, int r) {
    zs_arg6(b);
    zs_w(b, sge_s390x_basr(ZS_LINK, U(r)));
}
static inline void rk_jump_reg(SgXBuf* b, int r) { zs_w(b, sge_s390x_bcr(15, U(r))); }
static inline void rk_ret(SgXBuf* b) { rk_jump_reg(b, ZS_LINK); }
/* d = the address of an 8-byte absolute address in the code (its relocation's place, which this gives): LARL of it
   and BRC past it, the literal 8-aligned (NOPR's two bytes before it where it isn't) */
static inline size_t rk_literal(SgXBuf* b, int d) {
    size_t larl = b->n, brc, at;
    zs_w(b, sge_s390x_larl(U(d), 0));
    brc = b->n;
    zs_w(b, sge_s390x_brc(15, 0));
    while (b->n % 8) zs_w(b, sge_s390x_bcr(0, 0));
    at = b->n;
    sgx_imm(b, 0, 8);
    if (b->n <= b->cap) {
        SgXBuf l = *b;
        l.n = larl;
        zs_w(&l, sge_s390x_larl(U(d), M((at - larl) / 2)));
        l.n = brc;
        zs_w(&l, sge_s390x_brc(15, M((b->n - brc) / 2) & 0xffff));
    }
    return at;
}
/* rk_literal where there's no frame (a module descriptor's functions): the link as it was */
static inline size_t rk_leaf_literal(SgXBuf* b, int d) { return rk_literal(b, d); }
/* a call by the object's relocation: the callee's absolute address in the code (R_390_64, the place this gives)
   loaded into r1 and called with BASR; a tail call's jumps (the epilogue's r14 the caller's, kept) */
static inline size_t rk_call_reloc(SgXBuf* b) {
    size_t at = rk_literal(b, RK_SA);
    rk_ldst(b, 1, 8, 0, RK_SA, RK_SA, -1, 0);
    rk_call_reg(b, RK_SA);
    return at;
}
static inline size_t rk_tail_reloc(SgXBuf* b) {
    size_t at = rk_literal(b, RK_SA);
    rk_ldst(b, 1, 8, 0, RK_SA, RK_SA, -1, 0);
    rk_jump_reg(b, RK_SA);
    return at;
}
static inline int rk_patch_call(SgXBuf* b, size_t at, size_t to) {
    (void)b, (void)at, (void)to;
    return -1;
}
/* BCR 15,0: a serialization (every barrier) */
static inline void rk_fence(SgXBuf* b) { zs_w(b, sge_s390x_bcr(15, 0)); }
static inline void rk_nop(SgXBuf* b) { zs_w(b, sge_s390x_brc(0, 0)); }
/* no load-reserved and store-conditional: the load (sign extended, as LL's), its value kept in r0, and CS or CSG of
   that and the new value; the status 0 where it stored (CC 0). Arm's monitors are cpu.h's value-based ones anyway */
static inline void rk_lr(SgXBuf* b, int d, int a, int bytes) {
    rk_ldst(b, 1, bytes, 1, d, a, -1, 0);
    rk_mv(b, ZS_R0, d);
}
static inline void rk_sc(SgXBuf* b, int status, int a, int v, int bytes) {
    zs_w(b, bytes == 8 ? sge_s390x_csg(ZS_R0, U(v), U(a), 0) : sge_s390x_cs(ZS_R0, U(v), U(a), 0));
    zs_set(b, status, 7); /* CC 1: not stored */
}
#define RK_LR32_SEXT 1
/* the frame: r6-r15 stored in the caller's save area (STMG), sp less the frame's room (AGFI patched: the slots, the
   160 bytes of save area and the 8 of a sixth argument), the caller's sp stored at the new one's 0 (the back chain),
   r13 the state. The epilogue takes the registers back through the back chain */
static inline size_t rk_fn_prologue(SgXBuf* b, int pinned) {
    size_t at;
    (void)pinned;
    zs_w(b, sge_s390x_stmg(6, ZS_SP, ZS_SP, 48));
    rk_mv(b, RK_SA, ZS_SP);
    at = b->n;
    zs_w(b, sge_s390x_agfi(ZS_SP, M(-ZS_SLOT0)));
    zs_w(b, sge_s390x_stg(RK_SA, ZS_SP, 0, 0));
    rk_mv(b, 13, 2);
    return at;
}
static inline void rk_fn_epilogue(SgXBuf* b, int pinned) {
    (void)pinned;
    zs_w(b, sge_s390x_lg(RK_SA, ZS_SP, 0, 0));
    zs_w(b, sge_s390x_lmg(6, ZS_SP, RK_SA, 48));
}
static inline int rk_fn_patch_frame(SgXBuf* b, size_t at, long long slots) {
    long long size = ZS_SLOT0 + ((slots + 7) & ~7ll);
    SgXBuf l = *b;
    if (size >= (1ll << 31)) return 0;
    if (at + 6 > b->cap) return 1;
    l.n = at;
    zs_w(&l, sge_s390x_agfi(ZS_SP, M(-size)));
    return 1;
}

/* risc_a64.h's inline sequences' fixed registers: the guest memory's base r3, the address r4, the values r5 and r6;
   r7 to r9 their own */
#define RK_M_BASE 3
#define RK_M_ADDR 4
#define RK_M_V1 5
#define RK_M_V2 6
#define RK_M_T1 7
#define RK_M_T2 8
#define RK_M_T3 9

/* the state's address (r13), a call's arguments from the state's (r2) on: r2-r6, the sixth r7's (zs_arg6) */
#define SGX_BASE 13
static const signed char sgx_arg_regs[6] = {2, 3, 4, 5, 6, 7};
static const signed char sgx_a32_args[4] = {3, 4, 5, 6};
static const signed char sgx_a32_strex[3] = {3, 4, 5};
#define SGX_A32_STREX_CLOBBER (1u << 6 | 1u << 7)
#undef U
#undef M
