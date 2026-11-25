/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's powerpc64 back end (README.md, "The RISC back ends"): the 64-bit PowerPC's registers under the ELFv2 ABI
   and the operations risc.h writes every primitive from, after machdef.h and before risc.h. Each word is an encoder's
   of LLVM's PowerPC TableGen records (td/ppc64_enc.inc, tdenc.py). The code is POWER8's (ppc64le's baseline) without
   its vector unit; little-endian here, a big-endian host's guest memory byte-reversed where RK_BIG_ENDIAN says so. A
   big-endian host before ISA 2.06 (the 970, POWER4 to POWER6, the e5500) has no LDBRX and STDBRX: its doublewords of
   guest memory are two words each (SGX_F_LDBRX, the host's features in the buffer).

   Registers: r3-r8 and r14-r19 hold values (r3 and r4 last: a call's results); r0 (RK_S2: never a base or an addi's
   source, which read it as 0), r9 and r10 (the virtual flags), r11 (RK_S1) and r12 (RK_SA, which also carries a
   callee's address to its ELFv2 global entry) are reserved; r30 holds the state's address, r31 the spill slots'
   (r1 + 32, past the ABI's header); r20-r29 the pinned guest registers and the guest memory's base; r1 the stack, r2
   the caller's TOC (saved and given back: the code uses none) and r13 the thread's, never touched. A frame saves r2
   and r14-r31 in the ABI's red zone below r1 before its stdu, whose size is patched in at the end */

#include "ppc64_enc.inc"

#ifndef RK_BIG_ENDIAN
#define RK_BIG_ENDIAN 0
#endif

static inline void ppc_w(SgXBuf* b, SnxU32 w) {
    if (RK_BIG_ENDIAN) {
        sgx_byte(b, (unsigned char)(w >> 24)), sgx_byte(b, (unsigned char)(w >> 16));
        sgx_byte(b, (unsigned char)(w >> 8)), sgx_byte(b, (unsigned char)w);
    } else
        sgx_imm(b, (long long)w, 4);
}

/* the host's features a form may need: LDBRX and STDBRX, ISA 2.06's (POWER7) and the Cell's */
enum { SGX_F_LDBRX = 1 };

#define SGX_NREG 32
#define SGX_FIX 32
#define SGX_RET 3
#define SGX_RET2 4
#define SGX_FRAME 31
#define PPC_SP 1
#define PPC_TOC 2
#define RK_SA 12
#define RK_S1 11
#define RK_S2 0
#define RK_FA 9
#define RK_FB 10
static const signed char sgx_pool[] = {14, 15, 16, 17, 18, 19, 8, 7, 6, 5, 4, 3};
#define SGX_CALLER_SAVED (0x3Fu << 3) /* r3-r8 of the pool */

static inline int ppc_fits16(long long k) { return k >= -32768 && k < 32768; }
static inline SnxU32 ppc_d(long long k) { return (SnxU32)k & 0xffff; }
/* the condition fields of BCC (LLVM's BIBO: the CR field's bit << 5 | BO) on CR0 */
#define PPC_BEQ ((2u << 5) | 12u)
#define PPC_BNE ((2u << 5) | 4u)

/* ---- the operations risc.h writes the primitives from ---- */

static inline void rk_mv(SgXBuf* b, int d, int a) {
    if (d != a) ppc_w(b, sge_ppc64_or((SnxU32)d, (SnxU32)a, (SnxU32)a));
}
/* any 64-bit constant into d alone: LI, LIS and ORI of a 32-bit one; else the high word's, shifted up, and ORIS and
   ORI of the low */
static inline void rk_li(SgXBuf* b, int d, SnxU64 k) {
    long long v = (long long)k;
    SnxU32 hi = (SnxU32)(k >> 32), lo = (SnxU32)k;
    if (ppc_fits16(v)) {
        ppc_w(b, sge_ppc64_addi((SnxU32)d, 0, ppc_d(v)));
        return;
    }
    if (v == (long long)(int)lo) {
        ppc_w(b, sge_ppc64_addis((SnxU32)d, 0, lo >> 16));
        if (lo & 0xffff) ppc_w(b, sge_ppc64_ori((SnxU32)d, (SnxU32)d, lo & 0xffff));
        return;
    }
    rk_li(b, d, (SnxU64)(long long)(int)hi);
    ppc_w(b, sge_ppc64_rldicr((SnxU32)d, (SnxU32)d, 32, 31)); /* sldi d, d, 32 */
    if (lo >> 16) ppc_w(b, sge_ppc64_oris((SnxU32)d, (SnxU32)d, lo >> 16));
    if (lo & 0xffff) ppc_w(b, sge_ppc64_ori((SnxU32)d, (SnxU32)d, lo & 0xffff));
}
static inline void rk_add(SgXBuf* b, int d, int a, int c, int sf) {
    (void)sf;
    ppc_w(b, sge_ppc64_add((SnxU32)d, (SnxU32)a, (SnxU32)c));
}
static inline void rk_sub(SgXBuf* b, int d, int a, int c, int sf) {
    (void)sf;
    ppc_w(b, sge_ppc64_subf((SnxU32)d, (SnxU32)c, (SnxU32)a));
}
static inline void rk_neg(SgXBuf* b, int d, int a, int sf) {
    (void)sf;
    ppc_w(b, sge_ppc64_neg((SnxU32)d, (SnxU32)a));
}
static inline void rk_and(SgXBuf* b, int d, int a, int c) { ppc_w(b, sge_ppc64_and((SnxU32)d, (SnxU32)a, (SnxU32)c)); }
static inline void rk_or(SgXBuf* b, int d, int a, int c) { ppc_w(b, sge_ppc64_or((SnxU32)d, (SnxU32)a, (SnxU32)c)); }
static inline void rk_xor(SgXBuf* b, int d, int a, int c) { ppc_w(b, sge_ppc64_xor((SnxU32)d, (SnxU32)a, (SnxU32)c)); }
/* d = a + k: ADDI (and ADDIS) of a register other than r0, else the constant made in RK_SA and ADD */
static inline void rk_addi(SgXBuf* b, int d, int a, long long k, int sf) {
    (void)sf;
    if (a != 0 && d != 0 && ppc_fits16(k)) {
        ppc_w(b, sge_ppc64_addi((SnxU32)d, (SnxU32)a, ppc_d(k)));
        return;
    }
    rk_li(b, RK_SA, (SnxU64)k);
    rk_add(b, d, a, RK_SA, 1);
}
/* AND, OR and XOR of a constant: ANDI., ORI and XORI of 16 bits (ORIS and XORIS of the next 16), else the constant in
   RK_SA */
static inline void rk_andi(SgXBuf* b, int d, int a, long long k) {
    if (k >= 0 && k < 65536) {
        ppc_w(b, sge_ppc64_andi_rec((SnxU32)d, (SnxU32)a, (SnxU32)k));
        return;
    }
    rk_li(b, RK_SA, (SnxU64)k);
    rk_and(b, d, a, RK_SA);
}
static inline void rk_ori(SgXBuf* b, int d, int a, long long k) {
    if (k >= 0 && k < (1ll << 32)) {
        SnxU32 u = (SnxU32)k;
        if (u >> 16) ppc_w(b, sge_ppc64_oris((SnxU32)d, (SnxU32)a, u >> 16)), a = d;
        if ((u & 0xffff) || !(u >> 16)) ppc_w(b, sge_ppc64_ori((SnxU32)d, (SnxU32)a, u & 0xffff));
        return;
    }
    rk_li(b, RK_SA, (SnxU64)k);
    rk_or(b, d, a, RK_SA);
}
static inline void rk_xori(SgXBuf* b, int d, int a, long long k) {
    if (k == -1) {
        ppc_w(b, sge_ppc64_nor((SnxU32)d, (SnxU32)a, (SnxU32)a));
        return;
    }
    if (k >= 0 && k < (1ll << 32)) {
        SnxU32 u = (SnxU32)k;
        if (u >> 16) ppc_w(b, sge_ppc64_xoris((SnxU32)d, (SnxU32)a, u >> 16)), a = d;
        if ((u & 0xffff) || !(u >> 16)) ppc_w(b, sge_ppc64_xori((SnxU32)d, (SnxU32)a, u & 0xffff));
        return;
    }
    rk_li(b, RK_SA, (SnxU64)k);
    rk_xor(b, d, a, RK_SA);
}
/* shifts by a register: its amount modulo the width first (SLD and SLW read seven and six bits) */
static inline void ppc_amount(SgXBuf* b, int c, int sf) {
    if (sf) ppc_w(b, sge_ppc64_rldicl(RK_SA, (SnxU32)c, 0, 58));
    else
        ppc_w(b, sge_ppc64_rlwinm(RK_SA, (SnxU32)c, 0, 27, 31));
}
static inline void rk_sll(SgXBuf* b, int d, int a, int c, int sf) {
    ppc_amount(b, c, sf);
    ppc_w(b, sf ? sge_ppc64_sld((SnxU32)d, (SnxU32)a, RK_SA) : sge_ppc64_slw((SnxU32)d, (SnxU32)a, RK_SA));
}
static inline void rk_srl(SgXBuf* b, int d, int a, int c, int sf) {
    ppc_amount(b, c, sf);
    ppc_w(b, sf ? sge_ppc64_srd((SnxU32)d, (SnxU32)a, RK_SA) : sge_ppc64_srw((SnxU32)d, (SnxU32)a, RK_SA));
}
static inline void rk_sra(SgXBuf* b, int d, int a, int c, int sf) {
    ppc_amount(b, c, sf);
    ppc_w(b, sf ? sge_ppc64_srad((SnxU32)d, (SnxU32)a, RK_SA) : sge_ppc64_sraw((SnxU32)d, (SnxU32)a, RK_SA));
}
static inline void rk_slli(SgXBuf* b, int d, int a, int k, int sf) {
    if (sf) ppc_w(b, sge_ppc64_rldicr((SnxU32)d, (SnxU32)a, (SnxU32)k, 63u - (SnxU32)k));
    else
        ppc_w(b, sge_ppc64_rlwinm((SnxU32)d, (SnxU32)a, (SnxU32)k, 0, 31u - (SnxU32)k));
}
static inline void rk_srli(SgXBuf* b, int d, int a, int k, int sf) {
    if (sf) ppc_w(b, sge_ppc64_rldicl((SnxU32)d, (SnxU32)a, (64u - (SnxU32)k) & 63, (SnxU32)k));
    else
        ppc_w(b, sge_ppc64_rlwinm((SnxU32)d, (SnxU32)a, (32u - (SnxU32)k) & 31, (SnxU32)k, 31));
}
static inline void rk_srai(SgXBuf* b, int d, int a, int k, int sf) {
    ppc_w(b, sf ? sge_ppc64_sradi((SnxU32)d, (SnxU32)a, (SnxU32)k) : sge_ppc64_srawi((SnxU32)d, (SnxU32)a, (SnxU32)k));
}
static inline void rk_mul(SgXBuf* b, int d, int a, int c, int sf) {
    ppc_w(b, sf ? sge_ppc64_mulld((SnxU32)d, (SnxU32)a, (SnxU32)c) : sge_ppc64_mullw((SnxU32)d, (SnxU32)a, (SnxU32)c));
}
static inline void rk_mulhu(SgXBuf* b, int d, int a, int c) { ppc_w(b, sge_ppc64_mulhdu((SnxU32)d, (SnxU32)a, (SnxU32)c)); }
static inline void rk_mulh(SgXBuf* b, int d, int a, int c) { ppc_w(b, sge_ppc64_mulhd((SnxU32)d, (SnxU32)a, (SnxU32)c)); }
static inline void rk_divu(SgXBuf* b, int d, int a, int c) { ppc_w(b, sge_ppc64_divdu((SnxU32)d, (SnxU32)a, (SnxU32)c)); }
static inline void rk_div(SgXBuf* b, int d, int a, int c) { ppc_w(b, sge_ppc64_divd((SnxU32)d, (SnxU32)a, (SnxU32)c)); }
/* a < c: CMPLD or CMPD into CR0, its LT bit taken out of MFCR */
static inline void ppc_lt(SgXBuf* b, int d, int a, int c, int sign) {
    ppc_w(b, sign ? sge_ppc64_cmpd(0, (SnxU32)a, (SnxU32)c) : sge_ppc64_cmpld(0, (SnxU32)a, (SnxU32)c));
    ppc_w(b, sge_ppc64_mfcr((SnxU32)d));
    ppc_w(b, sge_ppc64_rlwinm((SnxU32)d, (SnxU32)d, 1, 31, 31));
}
static inline void rk_sltu(SgXBuf* b, int d, int a, int c) { ppc_lt(b, d, a, c, 0); }
static inline void rk_slt(SgXBuf* b, int d, int a, int c) { ppc_lt(b, d, a, c, 1); }
/* a == 0: CNTLZD's count is 64 only for 0; a != 0: the carry of a - 1 (ADDIC, SUBFE) */
static inline void rk_seqz(SgXBuf* b, int d, int a) {
    ppc_w(b, sge_ppc64_cntlzd((SnxU32)d, (SnxU32)a));
    ppc_w(b, sge_ppc64_rldicl((SnxU32)d, (SnxU32)d, 58, 6));
}
static inline void rk_snez(SgXBuf* b, int d, int a) {
    ppc_w(b, sge_ppc64_addic(RK_SA, (SnxU32)a, ppc_d(-1)));
    ppc_w(b, sge_ppc64_subfe((SnxU32)d, RK_SA, (SnxU32)a));
}
static inline void rk_sext(SgXBuf* b, int d, int a, int bits) {
    ppc_w(b, bits == 8    ? sge_ppc64_extsb((SnxU32)d, (SnxU32)a)
             : bits == 16 ? sge_ppc64_extsh((SnxU32)d, (SnxU32)a)
                          : sge_ppc64_extsw((SnxU32)d, (SnxU32)a));
}
static inline void rk_zext(SgXBuf* b, int d, int a, int bits) {
    ppc_w(b, sge_ppc64_rldicl((SnxU32)d, (SnxU32)a, 0, 64u - (SnxU32)bits));
}
/* a load or a store of bytes at base + index + disp: the D (or DS) form where the displacement fits it, else the
   address made in RK_SA first (and r0 as a base moved there: D forms read it as 0) */
static inline void rk_ldst(SgXBuf* b, int load, int bytes, int sign, int rt, int base, int index, int disp) {
    int ds = bytes == 8 || (load && bytes == 4 && sign);
    SnxU32 r = (SnxU32)rt;
    if (index >= 0 || base == 0 || !ppc_fits16(disp) || (ds && (disp & 3))) {
        if (index >= 0) ppc_w(b, sge_ppc64_add(RK_SA, (SnxU32)base, (SnxU32)index));
        else
            rk_mv(b, RK_SA, base);
        if (disp) {
            long long lo = (long long)(short)(disp & 0xffff), hi = ((long long)disp - lo) >> 16;
            if (hi) ppc_w(b, sge_ppc64_addis(RK_SA, RK_SA, ppc_d(hi)));
            if (lo) ppc_w(b, sge_ppc64_addi(RK_SA, RK_SA, ppc_d(lo)));
        }
        base = RK_SA;
        disp = 0;
    }
    {
        SnxU32 a = (SnxU32)base, dd = ppc_d(disp), dsv = ppc_d(disp) >> 2;
        if (load) {
            if (bytes == 1) {
                ppc_w(b, sge_ppc64_lbz(r, dd, a));
                if (sign) rk_sext(b, rt, rt, 8);
            } else if (bytes == 2)
                ppc_w(b, sign ? sge_ppc64_lha(r, dd, a) : sge_ppc64_lhz(r, dd, a));
            else if (bytes == 4)
                ppc_w(b, sign ? sge_ppc64_lwa(r, dsv, a) : sge_ppc64_lwz(r, dd, a));
            else
                ppc_w(b, sge_ppc64_ld(r, dsv, a));
        } else
            ppc_w(b, bytes == 1   ? sge_ppc64_stb(r, dd, a)
                     : bytes == 2 ? sge_ppc64_sth(r, dd, a)
                     : bytes == 4 ? sge_ppc64_stw(r, dd, a)
                                  : sge_ppc64_std(r, dsv, a));
    }
}

#if RK_BIG_ENDIAN
/* a big-endian host's: d = the low bytes of a reversed (no BRH/BRW/BRD before POWER10: rotates and inserts) */
static inline void ppc_bswap32(SgXBuf* b, int d, int a) { /* d != a, d's high half zero after */
    ppc_w(b, sge_ppc64_rlwinm((SnxU32)d, (SnxU32)a, 8, 0, 31));
    ppc_w(b, sge_ppc64_rlwimi((SnxU32)d, (SnxU32)a, 24, 0, 7));
    ppc_w(b, sge_ppc64_rlwimi((SnxU32)d, (SnxU32)a, 24, 16, 23));
}
#define RK_BSWAP 1
static inline void rk_bswap(SgXBuf* b, int d, int a, int bytes) {
    if (bytes == 2) {
        ppc_w(b, sge_ppc64_rlwinm(RK_SA, (SnxU32)a, 8, 16, 23));
        ppc_w(b, sge_ppc64_rlwinm(RK_S2, (SnxU32)a, 24, 24, 31));
        rk_or(b, d, RK_SA, RK_S2);
    } else if (bytes == 4) {
        ppc_bswap32(b, RK_SA, a);
        rk_mv(b, d, RK_SA);
    } else {
        /* the low word's reversed into the high one, the high word's into the low */
        ppc_w(b, sge_ppc64_rldicl(RK_S2, (SnxU32)a, 32, 32));
        ppc_bswap32(b, RK_SA, a);
        ppc_w(b, sge_ppc64_rldicr(RK_SA, RK_SA, 32, 31));
        ppc_bswap32(b, RK_S1, RK_S2);
        rk_or(b, d, RK_SA, RK_S1);
    }
}
/* guest memory's loads and stores (risc.h's moves): LHBRX, LWBRX and LDBRX (and their stores) of base and RK_SA (the
   index and the displacement), a byte's the plain one's; a reversed load is zero extended, so sign extended after */
#define RK_GLDST 1
static inline void rk_gldst(SgXBuf* b, int load, int bytes, int sign, int rt, int base, int index, int disp) {
    SnxU32 r = (SnxU32)rt, ra = (SnxU32)base, rb;
    if (bytes == 1) {
        rk_ldst(b, load, bytes, sign, rt, base, index, disp);
        return;
    }
    if (index >= 0 && !disp) rb = (SnxU32)index;
    else {
        rk_li(b, RK_SA, (SnxU64)(long long)disp);
        if (index >= 0) rk_add(b, RK_SA, RK_SA, index, 1);
        rb = RK_SA;
    }
    if (ra == 0) { /* r0 as RA reads 0: the base added into RB instead */
        rk_add(b, RK_SA, (int)rb, 0, 1);
        rb = RK_SA;
    }
    if (bytes == 8 && !(b->features & SGX_F_LDBRX)) {
        /* the word at +4, guest memory's high one, in RK_S2 and the address in RK_S1; LLVM's form there too. Two
           accesses, as LLVM's are, where Arm's aligned doubleword is one */
        ppc_w(b, sge_ppc64_addi(RK_S1, rb, 4));
        if (load) {
            ppc_w(b, sge_ppc64_lwbrx(RK_S2, ra, RK_S1));
            ppc_w(b, sge_ppc64_lwbrx(r, ra, rb));
            ppc_w(b, sge_ppc64_rldicr(RK_S2, RK_S2, 32, 31)); /* sldi */
            rk_or(b, rt, rt, RK_S2);
        } else {
            ppc_w(b, sge_ppc64_rldicl(RK_S2, r, 32, 32)); /* srdi */
            ppc_w(b, sge_ppc64_stwbrx(r, ra, rb));
            ppc_w(b, sge_ppc64_stwbrx(RK_S2, ra, RK_S1));
        }
        return;
    }
    if (load) {
        ppc_w(b, bytes == 2 ? sge_ppc64_lhbrx(r, ra, rb) : bytes == 4 ? sge_ppc64_lwbrx(r, ra, rb) : sge_ppc64_ldbrx(r, ra, rb));
        if (sign && bytes < 8) rk_sext(b, rt, rt, 8 * bytes);
    } else
        ppc_w(b, bytes == 2 ? sge_ppc64_sthbrx(r, ra, rb) : bytes == 4 ? sge_ppc64_stwbrx(r, ra, rb) : sge_ppc64_stdbrx(r, ra, rb));
}
#endif

/* ---- branches, calls and the frame ---- */

static inline SnxU32 ppc_word_at(const SgXBuf* b, size_t at) {
    return RK_BIG_ENDIAN ? (SnxU32)b->p[at] << 24 | (SnxU32)b->p[at + 1] << 16 | (SnxU32)b->p[at + 2] << 8 | (SnxU32)b->p[at + 3]
                         : (SnxU32)b->p[at] | (SnxU32)b->p[at + 1] << 8 | (SnxU32)b->p[at + 2] << 16 |
                               (SnxU32)b->p[at + 3] << 24;
}
static inline void ppc_word_put(SgXBuf* b, size_t at, SnxU32 w) {
    size_t n = b->n;
    b->n = at;
    ppc_w(b, w);
    b->n = n;
}
/* a branch on r: CMPDI and BEQ (BNE where nonzero) to be patched */
static inline size_t rk_bz(SgXBuf* b, int nonzero, int r) {
    size_t at;
    ppc_w(b, sge_ppc64_cmpdi(0, (SnxU32)r, 0));
    at = b->n;
    ppc_w(b, sge_ppc64_bcc(nonzero ? PPC_BNE : PPC_BEQ, 0, 0));
    return at;
}
static inline size_t rk_j(SgXBuf* b) {
    size_t at = b->n;
    ppc_w(b, sge_ppc64_b(0));
    return at;
}
/* the B or BC at at to the byte offset to: its LI (26 bits) or BD (16 bits) of words rewritten */
static inline int rk_patch(SgXBuf* b, size_t at, size_t to) {
    long long d = (long long)to - (long long)at;
    SnxU32 w;
    if (at + 4 > b->cap) return 0;
    w = ppc_word_at(b, at);
    if ((w >> 26) == 18) {
        if (d < -(1ll << 25) || d >= (1ll << 25)) return -1;
        w = (w & ~0x03FFFFFCu) | ((SnxU32)d & 0x03FFFFFCu);
    } else {
        if (d < -(1ll << 15) || d >= (1ll << 15)) return -1;
        w = (w & ~0xFFFCu) | ((SnxU32)d & 0xFFFCu);
    }
    ppc_word_put(b, at, w);
    return 0;
}
/* calls and jumps through CTR, the target in r12 (an ELFv2 global entry's) */
static inline void rk_call_reg(SgXBuf* b, int r) {
    rk_mv(b, RK_SA, r);
    ppc_w(b, sge_ppc64_mtctr(RK_SA));
    ppc_w(b, sge_ppc64_bctrl());
}
static inline void rk_jump_reg(SgXBuf* b, int r) {
    rk_mv(b, RK_SA, r);
    ppc_w(b, sge_ppc64_mtctr(RK_SA));
    ppc_w(b, sge_ppc64_bctr());
}
static inline void rk_ret(SgXBuf* b) { ppc_w(b, sge_ppc64_blr()); }
/* d = the address of 8 bytes in the code (BL over them, the link their address: LR is the frame's, saved), a symbol's
   distance from them (R_PPC64_REL64; a module's code holds no address, which would be a text relocation); the place
   of their relocation */
static inline size_t rk_literal(SgXBuf* b, int d) {
    size_t at;
    ppc_w(b, sge_ppc64_bl(3)); /* 12 bytes on */
    at = b->n;
    sgx_imm(b, 0, 8);
    ppc_w(b, sge_ppc64_mflr((SnxU32)d));
    return at;
}
/* rk_literal where there's no frame (a module descriptor's functions): LR, which its BL writes, kept in RK_S1 around
   it */
static inline size_t rk_leaf_literal(SgXBuf* b, int d) {
    size_t at;
    ppc_w(b, sge_ppc64_mflr(RK_S1));
    at = rk_literal(b, d);
    ppc_w(b, sge_ppc64_mtlr(RK_S1));
    return at;
}
/* d = the address the literal at lit (rk_literal's d) stands for, t a scratch (d, where lit isn't) */
static inline void rk_literal_value(SgXBuf* b, int d, int lit, int t) {
    rk_ldst(b, 1, 8, 0, t, lit, -1, 0);
    rk_add(b, d, lit, t, 1);
}
/* a call by the object's relocation: the callee's distance from a literal (R_PPC64_REL64: the place this gives), made
   its address in r12 and called through CTR; no veneer, which would want a TOC in r2. A tail call keeps LR, the
   caller's, in RK_S1 around the BL that finds the literal */
static inline size_t rk_call_reloc(SgXBuf* b) {
    size_t at = rk_literal(b, RK_SA);
    rk_literal_value(b, RK_SA, RK_SA, RK_S2);
    rk_call_reg(b, RK_SA);
    return at;
}
static inline size_t rk_tail_reloc(SgXBuf* b) {
    size_t at;
    ppc_w(b, sge_ppc64_mflr(RK_S1));
    at = rk_literal(b, RK_SA);
    ppc_w(b, sge_ppc64_mtlr(RK_S1));
    rk_literal_value(b, RK_SA, RK_SA, RK_S2);
    rk_jump_reg(b, RK_SA);
    return at;
}
/* an absolute call's address resolved here: its literal made the target's (the unit's own: a relocation it stays) */
static inline int rk_patch_call(SgXBuf* b, size_t at, size_t to) {
    (void)b, (void)at, (void)to;
    return -1;
}
/* SYNC (every barrier: the full one) */
static inline void rk_fence(SgXBuf* b) { ppc_w(b, sge_ppc64_sync(0)); }
static inline void rk_nop(SgXBuf* b) { ppc_w(b, sge_ppc64_nop()); }
/* LWARX or LDARX and STWCX. or STDCX. at [a]: the store's status 0 where it stored (CR0's EQ, inverted) */
static inline void rk_lr(SgXBuf* b, int d, int a, int bytes) {
    ppc_w(b, bytes == 8 ? sge_ppc64_ldarx((SnxU32)d, 0, (SnxU32)a) : sge_ppc64_lwarx((SnxU32)d, 0, (SnxU32)a));
}
static inline void rk_sc(SgXBuf* b, int status, int a, int v, int bytes) {
    ppc_w(b, bytes == 8 ? sge_ppc64_stdcx((SnxU32)v, 0, (SnxU32)a) : sge_ppc64_stwcx((SnxU32)v, 0, (SnxU32)a));
    ppc_w(b, sge_ppc64_mfcr((SnxU32)status));
    ppc_w(b, sge_ppc64_rlwinm((SnxU32)status, (SnxU32)status, 3, 31, 31));
    ppc_w(b, sge_ppc64_xori((SnxU32)status, (SnxU32)status, 1));
}
/* the frame: r2 and r14-r31 stored in the red zone below r1, LR at 16(r1), then STDU r1 of the frame's size (patched:
   its place this gives); r30 the state, r31 the spill slots (r1 + 32). The epilogue takes r1 back from its back
   chain. The saved registers are at the top of the frame, so its size counts them */
#define PPC_SAVED (8 * 19)
static inline size_t rk_fn_prologue(SgXBuf* b, int pinned) {
    size_t at;
    int k;
    (void)pinned;
    ppc_w(b, sge_ppc64_std(PPC_TOC, ppc_d(-8) >> 2, PPC_SP));
    for (k = 14; k <= 31; ++k) ppc_w(b, sge_ppc64_std((SnxU32)k, ppc_d(-8 * (32 - k) - 8) >> 2, PPC_SP));
    ppc_w(b, sge_ppc64_mflr(0));
    ppc_w(b, sge_ppc64_std(0, 16 >> 2, PPC_SP));
    at = b->n;
    ppc_w(b, sge_ppc64_stdu(PPC_SP, ppc_d(-(32 + PPC_SAVED)) >> 2, PPC_SP));
    ppc_w(b, sge_ppc64_addi(SGX_FRAME, PPC_SP, 32));
    rk_mv(b, 30, 3);
    return at;
}
static inline void rk_fn_epilogue(SgXBuf* b, int pinned) {
    int k;
    (void)pinned;
    ppc_w(b, sge_ppc64_ld(PPC_SP, 0, PPC_SP));
    ppc_w(b, sge_ppc64_ld(0, 16 >> 2, PPC_SP));
    ppc_w(b, sge_ppc64_mtlr(0));
    for (k = 14; k <= 31; ++k) ppc_w(b, sge_ppc64_ld((SnxU32)k, ppc_d(-8 * (32 - k) - 8) >> 2, PPC_SP));
    ppc_w(b, sge_ppc64_ld(PPC_TOC, ppc_d(-8) >> 2, PPC_SP));
}
/* the STDU's size: the header, the slots and the saved registers, 16-aligned */
static inline int rk_fn_patch_frame(SgXBuf* b, size_t at, long long slots) {
    long long size = (32 + slots + PPC_SAVED + 15) & ~15ll;
    if (size >= 32768) return 0;
    if (at + 4 > b->cap) return 1;
    ppc_word_put(b, at, sge_ppc64_stdu(PPC_SP, ppc_d(-size) >> 2, PPC_SP));
    return 1;
}

/* risc_a64.h's inline sequences' fixed registers: the guest memory's base r4, the address r5, the values r6 and r7;
   r8, r14 and r15 their own */
#define RK_M_BASE 4
#define RK_M_ADDR 5
#define RK_M_V1 6
#define RK_M_V2 7
#define RK_M_T1 8
#define RK_M_T2 14
#define RK_M_T3 15

/* the state's address (r30, the callee's), a call's arguments from the state's (r3) on */
#define SGX_BASE 30
static const signed char sgx_arg_regs[6] = {3, 4, 5, 6, 7, 8};
static const signed char sgx_a32_args[4] = {4, 5, 6, 7};
static const signed char sgx_a32_strex[3] = {4, 5, 6};
#define SGX_A32_STREX_CLOBBER (1u << 7 | 1u << 8)
