/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The A32 decoder: QEMU's a32.decode and a32-uncond.decode (decode/), which tools/decodetree makes into
   decode-a32.inc and decode-a32-uncond.inc, sort a word into its instruction, and each instruction's trans_ function
   gives its SnxA32Kind, or SNX_A32_UNDEFINED for an encoding the backend refuses (UNPREDICTABLE operands, and the
   instructions it does not run). VFP and Advanced SIMD words are vector_decode.c's, which the decode tries first.
   tests/arm_decode.c checks every word against the hand-written decoder this one replaced
   (tests/support/arm_decode_ref_a32.c). */
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"

typedef struct {
    uint32_t w;
    SnxA32Kind kind;
} DisasContext;

#include "common/cpu/arm/decodetree.h"

/* the generated extractors take a context and arguments that some formats leave unused, as QEMU's do */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "decode-a32.inc"
#include "decode-a32-uncond.inc"
#pragma GCC diagnostic pop

/* the register fields of most A32 encodings */
#define RN(w) (((w) >> 16) & 15)
#define RD(w) (((w) >> 12) & 15)
#define RS(w) (((w) >> 8) & 15)
#define RM(w) ((w) & 15)
#define BIT(w, b) (((w) >> (b)) & 1)

/* a trans_ function that gives check(word): a kind, or SNX_A32_UNDEFINED. It always answers true, so a { } group
   never tries a later pattern for a word one has refused. Inlined, so the decoder leaves out the fields it ignores */
#define KIND(name, check)                                                                                              \
    static inline __attribute__((always_inline)) bool trans_##name(DisasContext* ctx, arg_##name* a) {                 \
        (void)a;                                                                                                       \
        ctx->kind = check(ctx->w);                                                                                     \
        return true;                                                                                                   \
    }

static SnxA32Kind undefined(uint32_t w) { return (void)w, SNX_A32_UNDEFINED; }
static SnxA32Kind always_branch(uint32_t w) { return (void)w, SNX_A32_BRANCH; }
static SnxA32Kind always_svc(uint32_t w) { return (void)w, SNX_A32_SVC; }
static SnxA32Kind always_blx(uint32_t w) { return (void)w, SNX_A32_BLX_IMM; }

/* data processing: a register-shifted register can't be the PC; TST, TEQ, CMP and CMN set the flags into no Rd; an
   S form writing the PC is an exception return; MOV and MVN have no Rn */
static SnxA32Kind data(uint32_t w) {
    unsigned op = (w >> 21) & 15;
    if (!BIT(w, 25) && BIT(w, 4) && (BIT(w, 7) || RS(w) == 15 || RM(w) == 15 || RN(w) == 15)) return SNX_A32_UNDEFINED;
    if (op >= 8 && op <= 11) {
        if (!BIT(w, 20) || RD(w)) return SNX_A32_UNDEFINED;
    } else if (RD(w) == 15 && BIT(w, 20))
        return SNX_A32_UNDEFINED;
    if ((op == 13 || op == 15) && RN(w)) return SNX_A32_UNDEFINED;
    return SNX_A32_DATA;
}

static SnxA32Kind movw(uint32_t w) { return RD(w) != 15 ? SNX_A32_MOVW : SNX_A32_UNDEFINED; }
static SnxA32Kind movt(uint32_t w) { return RD(w) != 15 ? SNX_A32_MOVT : SNX_A32_UNDEFINED; }

/* the multiplies: Rd (RdHi) 19:16, Ra (RdLo) 15:12, Rm 11:8, Rn 3:0. MUL's Ra is zero */
static SnxA32Kind mul(uint32_t w) {
    if (RN(w) == 15 || RM(w) == 15 || RS(w) == 15) return SNX_A32_UNDEFINED;
    return (BIT(w, 21) ? RD(w) != 15 : RD(w) == 0) ? SNX_A32_MUL : SNX_A32_UNDEFINED;
}
static SnxA32Kind mull(uint32_t w) {
    return RN(w) != 15 && RD(w) != 15 && RM(w) != 15 && RS(w) != 15 && RN(w) != RD(w) ? SNX_A32_MULL
                                                                                      : SNX_A32_UNDEFINED;
}
static SnxA32Kind mls(uint32_t w) {
    return RN(w) != 15 && RD(w) != 15 && RM(w) != 15 && RS(w) != 15 ? SNX_A32_MLS : SNX_A32_UNDEFINED;
}

/* QADD, QSUB, QDADD, QDSUB: Rn 19:16, Rd 15:12, Rm 3:0, bits 11:8 zero */
static SnxA32Kind qarith(uint32_t w) {
    return !RS(w) && RN(w) != 15 && RD(w) != 15 && RM(w) != 15 ? SNX_A32_QARITH : SNX_A32_UNDEFINED;
}

/* the halfword multiplies. SMUL<x><y> and SMULW<y> have no Ra; SMLAL<x><y>'s RdHi and RdLo differ */
static SnxA32Kind half_mul(uint32_t w) {
    unsigned op = (w >> 21) & 3;
    if (RN(w) == 15 || RS(w) == 15 || RM(w) == 15) return SNX_A32_UNDEFINED;
    if (op == 3 || (op == 1 && (w & 0x20))) return !RD(w) ? SNX_A32_HALF_MUL : SNX_A32_UNDEFINED;
    return RD(w) != 15 && (op != 2 || RD(w) != RN(w)) ? SNX_A32_HALF_MUL : SNX_A32_UNDEFINED;
}

/* NOP, YIELD, WFE, WFI, SEV, SEVL, ESB, and the unallocated hints ARMv8 runs as NOP, with their should-be fields */
static SnxA32Kind hint(uint32_t w) { return (w & 0x0FFFFF00) == 0x0320F000 ? SNX_A32_HINT : SNX_A32_UNDEFINED; }

/* MSR to CPSR: at EL0 the f and s fields write NZCVQ and GE, x writes only E and c nothing. SPSR needs privilege */
static SnxA32Kind msr(uint32_t w) {
    if ((w & 0x0FF0FFF0) != 0x0120F000 && (w & 0x0FF0F000) != 0x0320F000) return SNX_A32_UNDEFINED;
    return RN(w) && (BIT(w, 25) || RM(w) != 15) ? SNX_A32_MSR : SNX_A32_UNDEFINED;
}
static SnxA32Kind mrs(uint32_t w) {
    return (w & 0x0FBF0FFF) == 0x010F0000 && !BIT(w, 22) && RD(w) != 15 ? SNX_A32_MRS : SNX_A32_UNDEFINED;
}

/* CRC32{C}: unconditional only, sizes B, H and W */
static SnxA32Kind crc32(uint32_t w) {
    if ((w >> 28) != 14 || ((w >> 21) & 3) == 3 || (w & 0xD00) || RN(w) == 15 || RD(w) == 15 || RM(w) == 15)
        return SNX_A32_UNDEFINED;
    return SNX_A32_CRC32;
}

static SnxA32Kind bx(uint32_t w) {
    if ((w & 0x0FFFFFF0) != 0x012FFF10 && (w & 0x0FFFFFF0) != 0x012FFF30) return SNX_A32_UNDEFINED;
    return RM(w) != 15 ? SNX_A32_BX : SNX_A32_UNDEFINED;
}
static SnxA32Kind clz(uint32_t w) {
    return (w & 0x0FFF0FF0) == 0x016F0F10 && RD(w) != 15 && RM(w) != 15 ? SNX_A32_CLZ : SNX_A32_UNDEFINED;
}
static SnxA32Kind bkpt(uint32_t w) { return (w >> 28) == 14 ? SNX_A32_BKPT : SNX_A32_UNDEFINED; }

/* the halfword, signed byte and doubleword loads and stores. A PC base is UNPREDICTABLE only with writeback, as for
   the word and byte forms; LDRD and STRD take an even pair below R14 */
static SnxA32Kind extra(uint32_t w) {
    unsigned op = (w >> 5) & 3, load = BIT(w, 20), wb = !BIT(w, 24) || BIT(w, 21), n = RN(w), d = RD(w);
    if (!BIT(w, 22) && (RS(w) || RM(w) == 15)) return SNX_A32_UNDEFINED;
    if ((wb && (n == 15 || n == d)) || d == 15) return SNX_A32_UNDEFINED;
    if (!load && op >= 2 && ((d & 1) || d >= 14 || (wb && n == d + 1) || (!BIT(w, 24) && BIT(w, 21))))
        return SNX_A32_UNDEFINED;
    return SNX_A32_EXTRA;
}

/* the word and byte loads and stores. A PC base is UNPREDICTABLE only with writeback, for stores as for loads:
   STR r1, [pc, r0] reads PC+8 */
static SnxA32Kind single(uint32_t w) {
    unsigned wb = !BIT(w, 24) || BIT(w, 21), n = RN(w), d = RD(w);
    if (BIT(w, 25) && (BIT(w, 4) || RM(w) == 15)) return SNX_A32_UNDEFINED;
    if ((wb && (n == 15 || n == d)) || (BIT(w, 22) && d == 15)) return SNX_A32_UNDEFINED;
    return SNX_A32_SINGLE;
}

/* the exclusives and acquire/release: bits 11:8 1100 is LDA/STL (ARMv8, without the monitor), 1110 and 1111 the
   exclusives. A doubleword pair is even, below R14 */
static SnxA32Kind exclusive(uint32_t w) {
    unsigned op = (w >> 20) & 7, load = op & 1, n = RN(w), d = RD(w), m = RM(w), s = RS(w);
    if ((w & 0x0F8000F0) != 0x01800090) return SNX_A32_UNDEFINED;
    if (s == 12) {
        if (n == 15 || (op >> 1) == 1 || (load ? d == 15 || m != 15 : d != 15 || m == 15)) return SNX_A32_UNDEFINED;
        return SNX_A32_ACQUIRE_RELEASE;
    }
    if (n == 15 || d == 15 || (s != 15 && s != 14) || (load && m != 15)) return SNX_A32_UNDEFINED;
    if (!load && (m == 15 || d == n || d == m)) return SNX_A32_UNDEFINED;
    if ((op >> 1) == 1) {
        unsigned t = load ? d : m;
        if ((t & 1) || t >= 14 || (!load && d == t + 1)) return SNX_A32_UNDEFINED;
    }
    return SNX_A32_EXCLUSIVE;
}

/* USAD8, USADA8: Rd 19:16, Ra 15:12 */
static SnxA32Kind abs_diff(uint32_t w) {
    return RN(w) != 15 && RS(w) != 15 && RM(w) != 15 ? SNX_A32_ABS_DIFF : SNX_A32_UNDEFINED;
}

/* SBFX and UBFX within the register; BFI and BFC with msb at or above lsb */
static SnxA32Kind extract(uint32_t w) {
    return RD(w) != 15 && RM(w) != 15 && ((w >> 7) & 31) + ((w >> 16) & 31) < 32 ? SNX_A32_BITFIELD
                                                                                : SNX_A32_UNDEFINED;
}
static SnxA32Kind insert(uint32_t w) {
    return RD(w) != 15 && ((w >> 16) & 31) >= ((w >> 7) & 31) ? SNX_A32_BITFIELD : SNX_A32_UNDEFINED;
}

/* the ARMv6 parallel add and subtract family; its should-be-one field and PC operands are UNPREDICTABLE */
static SnxA32Kind parallel(uint32_t w) {
    unsigned op = (w >> 5) & 7;
    if (!(w & (3u << 20)) || op == 5 || op == 6 || RS(w) != 15 || RN(w) == 15 || RD(w) == 15 || RM(w) == 15)
        return SNX_A32_UNDEFINED;
    return SNX_A32_PARALLEL;
}

static SnxA32Kind pack(uint32_t w) {
    return RN(w) != 15 && RD(w) != 15 && RM(w) != 15 ? SNX_A32_PACK : SNX_A32_UNDEFINED;
}
static SnxA32Kind select_bytes(uint32_t w) {
    return (w & 0x0FF00FF0) == 0x06800FB0 && RN(w) != 15 && RD(w) != 15 && RM(w) != 15 ? SNX_A32_SELECT
                                                                                         : SNX_A32_UNDEFINED;
}
static SnxA32Kind saturate(uint32_t w) { return RD(w) != 15 && RM(w) != 15 ? SNX_A32_SATURATE : SNX_A32_UNDEFINED; }

/* the extends with add (and, Rn 15, without): XTB16 included, bits 9:8 zero */
static SnxA32Kind extend(uint32_t w) {
    unsigned op = (w >> 20) & 7;
    return op != 1 && op != 5 && !(w & 0x300) && RD(w) != 15 && RM(w) != 15 ? SNX_A32_EXTEND : SNX_A32_UNDEFINED;
}

/* REV, REV16, RBIT, REVSH with their should-be-one fields */
static SnxA32Kind rev(uint32_t w) {
    uint32_t c = w & 0x0FFF0FF0;
    if (c != 0x06BF0F30 && c != 0x06BF0FB0 && c != 0x06FF0FB0 && c != 0x06FF0F30) return SNX_A32_UNDEFINED;
    return RD(w) != 15 && RM(w) != 15 ? SNX_A32_REV : SNX_A32_UNDEFINED;
}

/* SMLAD, SMUAD, SMLSD, SMUSD, SMLALD, SMLSLD, SMMLA, SMMUL, SMMLS: Rd (RdHi) 19:16, Ra (RdLo) 15:12, Rm 11:8 */
static SnxA32Kind dual_mul(uint32_t w) {
    unsigned op = (w >> 5) & 7, n = RN(w), d = RD(w);
    if (n == 15 || RS(w) == 15 || RM(w) == 15) return SNX_A32_UNDEFINED;
    switch ((w >> 20) & 7) {
        case 0: return op < 4 ? SNX_A32_DUAL_MUL : SNX_A32_UNDEFINED;
        case 4: return op < 4 && d != 15 && d != n ? SNX_A32_DUAL_MUL : SNX_A32_UNDEFINED;
        case 5: return op < 2 || (op >= 6 && d != 15) ? SNX_A32_DUAL_MUL : SNX_A32_UNDEFINED;
        default: return SNX_A32_UNDEFINED;
    }
}

static SnxA32Kind divide(uint32_t w) {
    return (w & 0x0FD0F0F0) == 0x0710F010 && RN(w) != 15 && RM(w) != 15 && RS(w) != 15 ? SNX_A32_DIV
                                                                                        : SNX_A32_UNDEFINED;
}

/* LDM and STM: a register list, a base other than the PC, and no user-bank (^) form. Base-in-list with writeback
   is UNPREDICTABLE in the original ARMv6 text, but ARM11 MPCore runs it (and guest code relies on it): STM stores the
   old base, LDM's loaded base wins over writeback, as Unicorn's ARM11 MPCore model does */
static SnxA32Kind multiple(uint32_t w) {
    return !(uint16_t)w || RN(w) == 15 || BIT(w, 22) ? SNX_A32_UNDEFINED : SNX_A32_MULTIPLE;
}

/* MRC, MCR, MRRC and MCRR to coprocessors 14 and 15: which registers exist, and what they hold, is the backend's
   (SnxArmRt.coproc). Coprocessors 10 and 11 are VFP's, which vector_decode.c takes first */
static SnxA32Kind coproc(uint32_t w) { return RS(w) == 14 || RS(w) == 15 ? SNX_A32_COPROC : SNX_A32_UNDEFINED; }

static SnxA32Kind clrex(uint32_t w) { return w == 0xF57FF01F ? SNX_A32_CLREX : SNX_A32_UNDEFINED; }

/* ISB SY only. DMB and DSB with valid shareability and type options */
static SnxA32Kind barrier(uint32_t w) {
    unsigned op = (w >> 4) & 15;
    return (op == 6 && (w & 15) == 15) || ((op == 4 || op == 5) && (w & 3)) ? SNX_A32_BARRIER : SNX_A32_UNDEFINED;
}
static SnxA32Kind setend(uint32_t w) { return (w & 0xFFFFFDFF) == 0xF1010000 ? SNX_A32_SETEND : SNX_A32_UNDEFINED; }

/* PLD, PLDW and PLI: hints, not loads, so even an unmapped address must not data-abort */
static SnxA32Kind preload(uint32_t w) {
    uint32_t op = w & 0xFD70F000;
    if (op != 0xF550F000 && op != 0xF510F000 && op != 0xF450F000) return SNX_A32_UNDEFINED;
    if (BIT(w, 25) && (BIT(w, 4) || RM(w) == 15)) return SNX_A32_UNDEFINED;
    if (op == 0xF510F000 && RN(w) == 15) return SNX_A32_UNDEFINED;
    return SNX_A32_HINT;
}

#define DATA_OPS(X)                                                                                                    \
    X(AND) X(EOR) X(SUB) X(RSB) X(ADD) X(ADC) X(SBC) X(RSC) X(ORR) X(BIC)
#define DATA3(op) KIND(op##_rrri, data) KIND(op##_rrrr, data) KIND(op##_rri, data)
DATA_OPS(DATA3)
#define COMPARE(op) KIND(op##_xrri, data) KIND(op##_xrrr, data) KIND(op##_xri, data)
COMPARE(TST) COMPARE(TEQ) COMPARE(CMP) COMPARE(CMN)
#define MOVES(op) KIND(op##_rxri, data) KIND(op##_rxrr, data) KIND(op##_rxi, data)
MOVES(MOV) MOVES(MVN)

KIND(MOVW, movw) KIND(MOVT, movt)
KIND(MUL, mul) KIND(MLA, mul) KIND(MLS, mls)
KIND(UMAAL, mull) KIND(UMULL, mull) KIND(UMLAL, mull) KIND(SMULL, mull) KIND(SMLAL, mull)
KIND(QADD, qarith) KIND(QSUB, qarith) KIND(QDADD, qarith) KIND(QDSUB, qarith)

#define HALF(X)                                                                                                        \
    X(SMLABB) X(SMLABT) X(SMLATB) X(SMLATT) X(SMLAWB) X(SMULWB) X(SMLAWT) X(SMULWT) X(SMLALBB) X(SMLALBT) X(SMLALTB)    \
    X(SMLALTT) X(SMULBB) X(SMULBT) X(SMULTB) X(SMULTT)
#define HALF_KIND(name) KIND(name, half_mul)
HALF(HALF_KIND)

KIND(YIELD, hint) KIND(WFE, hint) KIND(WFI, hint) KIND(SEV, hint) KIND(SEVL, hint) KIND(ESB, hint) KIND(NOP, hint)
KIND(MSR_imm, msr) KIND(MSR_reg, msr) KIND(MRS_reg, mrs)
KIND(CRC32B, crc32) KIND(CRC32H, crc32) KIND(CRC32W, crc32) KIND(CRC32CB, crc32) KIND(CRC32CH, crc32)
KIND(CRC32CW, crc32)
KIND(BX, bx) KIND(BLX_r, bx) KIND(CLZ, clz) KIND(BKPT, bkpt)

/* what user mode can't run, or the backend doesn't: banked registers, Jazelle, exception returns and calls to
   higher levels, SWP (gone in ARMv8), UDF, SB */
KIND(MRS_bank, undefined) KIND(MSR_bank, undefined) KIND(BXJ, undefined) KIND(ERET, undefined)
KIND(HLT, undefined) KIND(HVC, undefined) KIND(SMC, undefined) KIND(SWP, undefined) KIND(SWPB, undefined)
KIND(UDF, undefined) KIND(RFE, undefined) KIND(SRS, undefined) KIND(CPS, undefined) KIND(SB, undefined)

#define EXTRA(X)                                                                                                       \
    X(STRH_rr) X(LDRD_rr) X(STRD_rr) X(LDRH_rr) X(LDRSB_rr) X(LDRSH_rr) X(STRHT_rr) X(LDRHT_rr) X(LDRSBT_rr)           \
    X(LDRSHT_rr) X(STRH_ri) X(LDRD_ri_a32) X(STRD_ri_a32) X(LDRH_ri) X(LDRSB_ri) X(LDRSH_ri) X(STRHT_ri) X(LDRHT_ri)   \
    X(LDRSBT_ri) X(LDRSHT_ri)
#define EXTRA_KIND(name) KIND(name, extra)
EXTRA(EXTRA_KIND)

#define SINGLE(X)                                                                                                      \
    X(STR_rr) X(STRB_rr) X(LDR_rr) X(LDRB_rr) X(STRT_rr) X(STRBT_rr) X(LDRT_rr) X(LDRBT_rr) X(STR_ri) X(STRB_ri)       \
    X(LDR_ri) X(LDRB_ri) X(STRT_ri) X(STRBT_ri) X(LDRT_ri) X(LDRBT_ri)
#define SINGLE_KIND(name) KIND(name, single)
SINGLE(SINGLE_KIND)

#define EXCLUSIVE(X)                                                                                                   \
    X(STREX) X(STREXD_a32) X(STREXB) X(STREXH) X(STLEX) X(STLEXD_a32) X(STLEXB) X(STLEXH) X(STL) X(STLB) X(STLH)       \
    X(LDREX) X(LDREXD_a32) X(LDREXB) X(LDREXH) X(LDAEX) X(LDAEXD_a32) X(LDAEXB) X(LDAEXH) X(LDA) X(LDAB) X(LDAH)
#define EXCLUSIVE_KIND(name) KIND(name, exclusive)
EXCLUSIVE(EXCLUSIVE_KIND)

KIND(USADA8, abs_diff) KIND(SBFX, extract) KIND(UBFX, extract) KIND(BFCI, insert)

#define PARALLEL(p)                                                                                                    \
    KIND(p##ADD16, parallel) KIND(p##ASX, parallel) KIND(p##SAX, parallel) KIND(p##SUB16, parallel)                    \
        KIND(p##ADD8, parallel) KIND(p##SUB8, parallel)
PARALLEL(S) PARALLEL(Q) PARALLEL(SH) PARALLEL(U) PARALLEL(UQ) PARALLEL(UH)

KIND(PKH, pack) KIND(SSAT, saturate) KIND(USAT, saturate) KIND(SSAT16, saturate) KIND(USAT16, saturate)
KIND(SXTAB16, extend) KIND(SXTAB, extend) KIND(SXTAH, extend) KIND(UXTAB16, extend) KIND(UXTAB, extend)
KIND(UXTAH, extend)
KIND(SEL, select_bytes) KIND(REV, rev) KIND(REV16, rev) KIND(REVSH, rev) KIND(RBIT, rev)

#define DUAL(X)                                                                                                        \
    X(SMLAD) X(SMLADX) X(SMLSD) X(SMLSDX) X(SMLALD) X(SMLALDX) X(SMLSLD) X(SMLSLDX) X(SMMLA) X(SMMLAR) X(SMMLS)        \
    X(SMMLSR)
#define DUAL_KIND(name) KIND(name, dual_mul)
DUAL(DUAL_KIND)

KIND(SDIV, divide) KIND(UDIV, divide) KIND(STM, multiple) KIND(LDM_a32, multiple)
KIND(B, always_branch) KIND(BL, always_branch) KIND(SVC, always_svc) KIND(BLX_i, always_blx)
KIND(MCRR, coproc) KIND(MRRC, coproc) KIND(MCR, coproc) KIND(MRC, coproc)
KIND(CLREX, clrex) KIND(DSB, barrier) KIND(DMB, barrier) KIND(ISB, barrier) KIND(SETEND, setend)
KIND(PLD, preload) KIND(PLDW, preload) KIND(PLI, preload)

SnxA32Kind snx_a32_decode(uint32_t w) {
    DisasContext ctx = {w, SNX_A32_UNDEFINED};
    SnxArmVector vector;
    if (snx_arm_vector_decode(w, &vector)) return SNX_A32_VECTOR;
    if ((w >> 28) == 15)
        disas_a32_uncond(&ctx, w);
    else
        disas_a32(&ctx, w);
    return ctx.kind;
}
