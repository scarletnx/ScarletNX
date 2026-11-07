/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Thumb decoder: QEMU's t16.decode and t32.decode (decode/), which tools/decodetree makes into decode-t16.inc and
   decode-t32.inc, sort an instruction into its pattern, and each pattern's trans_ function fills the SnxThumb the
   emitter reads (arm.h), or refuses the encoding (len 0). A T32 instruction that is an A32 one in another encoding is
   built as that A32 word, which a32.c's decoder judges (TH_A32). The coprocessor, VFP and Advanced SIMD space is the
   A32 vector decoder's, which the decode tries first. tests/arm_decode.c checks every T16 halfword and every T32 pair
   against the hand-written decoder this one replaced (tests/support/arm_decode_ref_thumb.c). */
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"

typedef struct {
    uint16_t a, b;   /* the first halfword, and the second of a T32 instruction */
    SnxThumb* o;
    unsigned len;    /* what the decode answers: TH_F16, TH_F32, or 0 */
    int t16;         /* disas_t16's: the names t16.decode shares with t32.decode take their T16 form */
} DisasContext;

#include "common/cpu/arm/decodetree.h"

/* the S bit of a T16 data-processing instruction. Outside an IT block it sets the flags; the emitter knows the IT
   state and passes TH_S as a flag-setting request it takes back inside one */
static inline int t16_setflags(DisasContext* s) { return (void)s, 1; }

/* the argument sets a32.decode and a32-uncond.decode define, which t32.decode and t16.decode name !extern, and the
   generated extractors, which take a context and arguments that some formats leave unused, as QEMU's do */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "decode-a32-args.inc"
#include "decode-a32-uncond-args.inc"
#include "decode-t32.inc"
#include "decode-t16.inc"
#pragma GCC diagnostic pop

#define T16(c) ((c)->t16)

static bool undef(DisasContext* c) {
    c->len = 0;
    return true;
}

static bool shape(DisasContext* c, SnxThumbShape s) {
    c->o->shape = s;
    return true;
}

/* an A32 twin: the A32 word (condition AL) and its kind, unless the A32 decoder refuses it */
static bool twin_kind(DisasContext* c, uint32_t word, SnxA32Kind kind, unsigned pair) {
    if (kind == SNX_A32_UNDEFINED) return undef(c);
    c->o->shape = TH_A32;
    c->o->imm = word;
    c->o->op = kind;
    c->o->ra = pair;
    c->o->rd = 0;
    return true;
}
static bool twin(DisasContext* c, uint32_t word) { return twin_kind(c, word, snx_a32_decode(word), 0); }

/* =========================================================================================== T16 */

/* LSL, LSR, ASR Rd, Rm, #imm5 */
static bool shift_imm16(DisasContext* c, arg_s_rrr_shi* a) {
    c->o->op = a->shty == 0 ? ALU_LSL : a->shty == 1 ? ALU_LSR : ALU_ASR;
    c->o->rd = a->rd;
    c->o->rm = a->rm;
    c->o->imm = a->shim;
    c->o->flags = TH_S;
    return shape(c, TH_SHIFT_IMM);
}

/* ADD and SUB Rd, Rn, Rm or #imm3 */
static bool addsub16(DisasContext* c, unsigned op, int rd, int rn, int rm, int imm, int reg) {
    c->o->op = op;
    c->o->rd = rd;
    c->o->rn = rn;
    if (reg) {
        c->o->rm = rm;
        c->o->flags = TH_REG;
    } else
        c->o->imm = imm;
    c->o->flags |= TH_S;
    return shape(c, TH_ADDSUB);
}

/* MOV, CMP, ADD, SUB Rd, #imm8 */
static bool imm8(DisasContext* c, unsigned op) {
    c->o->op = op;
    c->o->rd = (c->a >> 8) & 7;
    c->o->imm = c->a & 255;
    return shape(c, TH_IMM8);
}

/* the data-processing instructions on two low registers: AND .. MVN, op the halfword's bits 9:6 */
static bool alu16(DisasContext* c) {
    c->o->op = (c->a >> 6) & 15;
    c->o->rd = c->a & 7;
    c->o->rn = c->a & 7;
    c->o->rm = (c->a >> 3) & 7;
    c->o->flags = TH_S;
    return shape(c, TH_ALU);
}

/* ADD, CMP, MOV with high registers: op 0, 1, 2. ADD reads Rd as its first source */
static bool hi16(DisasContext* c, unsigned op) {
    c->o->op = op;
    c->o->rd = (c->a & 7) | ((c->a >> 4) & 8);
    c->o->rm = (c->a >> 3) & 15;
    c->o->rn = c->o->rd;
    return shape(c, TH_HI);
}

/* ADR, ADD Rd, SP, #imm8 * 4, ADD and SUB SP, SP, #imm7 * 4 */
static bool addr16(DisasContext* c, unsigned op, unsigned rd, unsigned rn, uint32_t imm) {
    c->o->op = op;
    c->o->rd = rd;
    c->o->rn = rn;
    c->o->imm = imm;
    return shape(c, TH_ADDR);
}

/* the loads and stores: Rt, Rn, and the offset register or immediate (scaled) */
static bool ldst16_rr(DisasContext* c, arg_ldst_rr* a, int load, unsigned size) {
    c->o->rd = a->rt;
    c->o->rn = a->rn;
    c->o->rm = a->rm;
    c->o->flags = TH_REG | TH_UP | size;
    return shape(c, load ? TH_LOAD : TH_STORE);
}
static bool ldst16_ri(DisasContext* c, arg_ldst_ri* a, int load, unsigned size) {
    c->o->rd = a->rt;
    c->o->rn = a->rn;
    c->o->imm = a->imm;
    c->o->flags = size | TH_UP | TH_PRE | (a->rn == 13 ? TH_SPB : a->rn == 15 ? TH_PCB : 0);
    return shape(c, load ? TH_LOAD : TH_STORE);
}

static bool branch16(DisasContext* c, unsigned op, uint32_t imm) {
    c->o->op = op;
    c->o->imm = imm;
    return shape(c, TH_BRANCH);
}

/* =========================================================================================== T32 */

/* ThumbExpandImm: the 12-bit modified immediate of T32 data processing */
static uint32_t thumb_expand_imm(unsigned imm12) {
    unsigned imm8 = imm12 & 255;
    if ((imm12 & 0xC00) == 0) {
        switch ((imm12 >> 8) & 3) {
            case 0: return imm8;
            case 1: return imm8 | (imm8 << 16);
            case 2: return (imm8 << 8) | (imm8 << 24);
            default: return imm8 * 0x01010101u;
        }
    }
    unsigned rot = (imm12 >> 7) & 31;
    uint32_t v = 0x80u | (imm12 & 127);
    return (v >> rot) | (v << ((32 - rot) & 31));
}

/* data processing with a modified immediate: op the first halfword's bits 8:5, Rn 15 for MOV and MVN */
static bool wdata_imm(DisasContext* c) {
    unsigned a = c->a, b = c->b;
    c->o->op = (a >> 5) & 15;
    c->o->rn = a & 15;
    c->o->rd = (b >> 8) & 15;
    c->o->imm = thumb_expand_imm((((a >> 10) & 1) << 11) | (((b >> 12) & 7) << 8) | (b & 255));
    c->o->flags = (a & 0x10 ? TH_S : 0) | TH_MODIMM | ((a & 0x400) || (b & 0x4000) ? TH_ROTC : 0);
    return shape(c, TH_WDATA);
}

/* data processing with a register shifted by an immediate (bit 15 of the second halfword zero) */
static bool wdata_shift(DisasContext* c) {
    unsigned a = c->a, b = c->b;
    if (b & 0x8000) return undef(c);
    c->o->op = (a >> 5) & 15;
    c->o->flags = (a & 0x10) ? TH_S : 0;
    c->o->rn = a & 15;
    c->o->rd = (b >> 8) & 15;
    c->o->rm = b & 15;
    c->o->ra = (b >> 4) & 3;                             /* the shift type */
    c->o->imm = (((b >> 12) & 7) << 2) | ((b >> 6) & 3); /* the shift amount */
    return shape(c, TH_WDATA);
}

/* MOVW, MOVT, ADDW, SUBW (and ADR, their Rn 15 forms): the plain 12- and 16-bit immediates */
static bool wimm(DisasContext* c) {
    unsigned a = c->a, b = c->b, imm12 = (((a >> 10) & 1) << 11) | (((b >> 12) & 7) << 8) | (b & 255);
    c->o->rd = (b >> 8) & 15;
    if ((a & 0xFBF0) == 0xF240 || (a & 0xFBF0) == 0xF2C0) {
        c->o->op = (a & 0x80) ? 1 : 0;
        c->o->imm = ((a & 15) << 12) | imm12;
    } else {
        c->o->op = (a & 0x80) ? ALU_SUB : ALU_ADD;
        c->o->rn = a & 15;
        c->o->imm = imm12;
    }
    return shape(c, TH_WIMM);
}

/* LDM and STM IA (op 1) or DB (op 2), with Thumb's stricter lists: two registers at least, no SP, no PC in a store,
   not both LR and PC in a load, and the base not in the list with writeback */
static bool ldm_stm32(DisasContext* c, arg_ldst_block* a) {
    unsigned b = c->b, n = (unsigned)a->rn, op1 = (c->a >> 7) & 3, load = (c->a >> 4) & 1, wb = (unsigned)a->w;
    unsigned count = 0;
    for (unsigned i = 0; i < 16; ++i) count += (b >> i) & 1;
    if (n == 15 || count < 2 || (b & 0x2000) || (load ? (b & 0xC000) == 0xC000 : (b & 0x8000)) ||
        (wb && ((b >> n) & 1)))
        return undef(c);
    return twin(c, 0xE8000000u | (op1 == 2 ? 1u << 24 : 1u << 23) | ((uint32_t)wb << 21) | ((uint32_t)load << 20) |
                       ((uint32_t)n << 16) | b);
}

/* LDRD and STRD: their own shape, the pair any two registers but SP and PC */
static bool dual32(DisasContext* c, arg_ldst_ri2* a, unsigned load) {
    unsigned n = (unsigned)a->rn, t = (unsigned)a->rt, t2 = (unsigned)a->rt2, wb = (unsigned)a->w;
    if (t == 13 || t == 15 || t2 == 13 || t2 == 15 || (load && t == t2) || (wb && (n == t || n == t2)) ||
        (n == 15 && (wb || !load)))
        return undef(c);
    c->o->op = load;
    c->o->rn = n;
    c->o->rd = t;
    c->o->rm = t2;
    c->o->imm = (c->b & 0xFF) * 4;
    c->o->flags = (a->p ? TH_PRE : 0) | (a->u ? TH_UP : 0) | (wb ? TH_WB : 0);
    return shape(c, TH_DUAL);
}

/* STREX and LDREX (T1): their imm8 * 4 offset, which A32's lack, in .rd */
static bool strex_t1(DisasContext* c, arg_strex* a) {
    unsigned n = (unsigned)a->rn, d = (unsigned)a->rd, t = (unsigned)a->rt;
    bool r = d == n || d == t ? undef(c) : twin(c, 0xE1800F90u | (uint32_t)n << 16 | (uint32_t)d << 12 | t);
    c->o->rd = (c->b & 0xFF) * 4;
    return r;
}
static bool ldrex_t1(DisasContext* c, arg_ldrex* a) {
    unsigned n = (unsigned)a->rn, t = (unsigned)a->rt;
    bool r = ((c->b >> 8) & 15) == 15 ? twin(c, 0xE1900F9Fu | (uint32_t)n << 16 | (uint32_t)t << 12) : undef(c);
    c->o->rd = (c->b & 0xFF) * 4;
    return r;
}

/* the byte, halfword and doubleword exclusives (op3 01ss), LDA/STL (10ss) and LDAEX/STLEX (11ss), ss the size: B,
   H, word, D. A32's have the size in bits 22:21 (word 00, D 01, B 10, H 11) and the kind in bits 11:8. A Thumb
   doubleword pair is any two registers: the A32 emitter takes the second from .ra, A32's being Rt + 1 */
static bool exclusive32(DisasContext* c) {
    static const unsigned size32[4] = {2, 3, 0, 1};
    static const unsigned kind32[4] = {0, 0xF, 0xC, 0xE};
    unsigned a = c->a, b = c->b, n = a & 15, t = b >> 12, t2 = (b >> 8) & 15, d = b & 15, op3 = (b >> 4) & 15;
    unsigned size = op3 & 3, kind = op3 >> 2, load = (a >> 4) & 1, pair = 0;
    if (!kind || (kind == 1 && size == 2) || (kind == 2 && size == 3) || n == 15) return undef(c);
    if (size == 3) {
        if (t == 13 || t == 15 || t2 == 13 || t2 == 15 || (load && (t == t2 || d != 15)) ||
            (!load && (d == 13 || d == 15 || d == n || d == t || d == t2)))
            return undef(c);
        pair = t2 + 1;
    } else if (t2 != 15 || (load && d != 15))
        return undef(c);
    uint32_t word = 0xE1800090u | (uint32_t)size32[size] << 21 | (uint32_t)load << 20 | (uint32_t)n << 16 |
                    (uint32_t)kind32[kind] << 8;
    if (load)
        word |= (uint32_t)t << 12 | 0xF;
    else
        word |= (uint32_t)(kind == 2 ? 15 : d) << 12 | t;
    if (pair) return twin_kind(c, word, SNX_A32_EXCLUSIVE, pair);
    return twin(c, word);
}

/* the branches and the miscellaneous control instructions where a B<c>.W would have condition 111x: MSR, the hints,
   CLREX and the barriers, MRS. What user mode can't run is refused */
static bool control32(DisasContext* c) {
    unsigned a = c->a, b = c->b, op = (a >> 4) & 0x7F, n = a & 15;
    c->o->rn = a; /* both halfwords, for the branches' emitter, which the other forms leave there */
    c->o->rm = b;
    if (b & 0x5000) return undef(c);
    if (((a >> 7) & 7) != 7) { /* B<c>.W (T3): S:J2:J1:imm6:imm11, in halfwords */
        c->o->op = BR_BCOND_W;
        c->o->ra = (a >> 6) & 15;
        c->o->imm = (((a >> 10) & 1) << 19) | (((b >> 11) & 1) << 18) | (((b >> 13) & 1) << 17) | ((a & 0x3F) << 11) |
                    (b & 0x7FF);
        return shape(c, TH_BRANCH);
    }
    if (op == 0x38) /* MSR APSR_<mask>, Rn: mask in the second halfword's bits 11:10 (nzcvq, g) */
        return (b & 0x3FF) ? undef(c) : twin(c, 0xE120F000u | (uint32_t)((b >> 10) & 3) << 18 | n);
    if (op == 0x3A) /* NOP.W, YIELD.W, WFE.W, WFI.W, SEV.W, SEVL.W */
        return n == 15 && (b & 0x7FF) <= 5 ? twin(c, 0xE320F000u | (b & 0xFF)) : undef(c);
    if (op == 0x3B && n == 15 && (b & 0xF00) == 0xF00) { /* CLREX (option 2 here, 1 in A32), DSB, DMB, ISB */
        if (((b >> 4) & 15) == 2) return (b & 15) == 15 ? twin(c, 0xF57FF01Fu) : undef(c);
        return twin(c, 0xF57FF000u | (b & 0xFF));
    }
    if (op == 0x3E && n == 15 && !(b & 0xFF)) return twin(c, 0xE10F0000u | (uint32_t)((b >> 8) & 15) << 12); /* MRS */
    return undef(c);
}

/* the byte, halfword and word loads and stores, and the memory hints. The first halfword's bit 7 selects imm12 (or,
   with Rn the PC, the literal's U); without it the second halfword's bit 11 selects imm8 with P, U and W, else a
   register offset */
static bool load_store32(DisasContext* c) {
    unsigned a = c->a, b = c->b, n = a & 15, t = b >> 12, sign = (a >> 8) & 1, size = (a >> 5) & 3,
             load = (a >> 4) & 1;
    SnxThumb* o = c->o;
    if (size == 3 || (sign && (!load || size == 2))) return undef(c);
    o->rn = n;
    o->rd = t;
    o->flags = (size == 0 ? TH_B : size == 1 ? TH_H : 0) | (sign ? TH_SIGN : 0);
    if (n == 15) { /* literal */
        if (!load) return undef(c);
        o->flags |= TH_PCB | TH_PRE | (a & 0x80 ? TH_UP : 0);
        o->imm = b & 0xFFF;
    } else if (a & 0x80) {
        o->flags |= TH_PRE | TH_UP;
        o->imm = b & 0xFFF;
    } else if (b & 0x800) { /* 1 P U W imm8; P U !W (LDRT, STRT) at the user privilege it runs at anyway */
        if (!(b & 0x500)) return undef(c);
        o->flags |= (b & 0x400 ? TH_PRE : 0) | (b & 0x200 ? TH_UP : 0) | (b & 0x100 ? TH_WB : 0);
        o->imm = b & 0xFF;
        if ((o->flags & TH_WB) && (n == t)) return undef(c);
    } else if (!(b & 0x7C0)) {
        o->flags |= TH_REG | TH_UP;
        o->rm = b & 15;
        o->imm = (b >> 4) & 3;
        if (o->rm == 13 || o->rm == 15) return undef(c);
    } else
        return undef(c);
    if (t == 15 && size < 2) /* PLD, PLDW, PLI and the unallocated hints: no access */
        return load ? shape(c, TH_HINT) : undef(c);
    if (!load && t == 15) return undef(c);
    return shape(c, load ? TH_LOAD : TH_STORE);
}

/* SBFX, UBFX (op 1), BFI (op 2), BFC (op 3, Rn 15): .imm the lsb, .ra the msb or the extract's width - 1 */
static bool bitfield32(DisasContext* c) {
    unsigned a = c->a, b = c->b, lsb = (((b >> 12) & 7) << 2) | ((b >> 6) & 3), msb = b & 31;
    if ((b & 0x8020) || (a & 0x20 ? msb < lsb : lsb + msb > 31)) return undef(c);
    c->o->rd = (b >> 8) & 15;
    c->o->rn = a & 15;
    c->o->op = !(a & 0x20) ? ((a & 0x80) ? 1 : 0) : (a & 15) == 15 ? 3 : 2;
    c->o->imm = lsb;
    c->o->ra = msb;
    return shape(c, TH_BITFIELD);
}

/* SSAT and USAT (with a shift), SSAT16 and USAT16 (sh, no shift) as A32's */
static bool saturate32(DisasContext* c) {
    unsigned a = c->a, b = c->b, sh = (a >> 5) & 1, shift = (((b >> 12) & 7) << 2) | ((b >> 6) & 3), u = (a >> 7) & 1;
    uint32_t regs = (uint32_t)((b >> 8) & 15) << 12 | (a & 15);
    if (b & 0x20) return undef(c);
    if (sh && !shift) {
        if (b & 0x30) return undef(c);
        return twin(c, (u ? 0xE6E00F30u : 0xE6A00F30u) | (uint32_t)(b & 15) << 16 | regs);
    }
    return twin(c, (u ? 0xE6E00010u : 0xE6A00010u) | (uint32_t)(b & 31) << 16 | (uint32_t)shift << 7 |
                       (uint32_t)sh << 6 | regs);
}

/* the long multiplies (second halfword RdLo RdHi 0000 Rm) and their accumulating halfword and dual forms */
static uint32_t long_regs(DisasContext* c) {
    return (uint32_t)((c->b >> 8) & 15) << 16 | (uint32_t)(c->b >> 12) << 12 | (uint32_t)(c->b & 15) << 8 |
           (c->a & 15);
}

/* the 32-bit multiplies past MUL, MLA and MLS: second halfword Ra Rd 00 op2 Rm, Ra 15 for the forms without an
   accumulate */
static uint32_t mul_regs(DisasContext* c) {
    return (uint32_t)((c->b >> 8) & 15) << 16 | (uint32_t)(c->b & 15) << 8 | (c->a & 15);
}
static uint32_t mul_acc(DisasContext* c) { return (uint32_t)(c->b >> 12) << 12; }

/* =========================================================================================== trans */

/* the data processing names: T16's two-low-register forms (TH_ALU), and T32's shifted register and modified
   immediate forms */
#define T32_SHIFT(name)                                                                                                \
    static bool trans_##name(DisasContext* c, arg_##name* a) { return (void)a, wdata_shift(c); }
#define T32_IMM(name)                                                                                                  \
    static bool trans_##name(DisasContext* c, arg_##name* a) { return (void)a, wdata_imm(c); }

static bool trans_AND_rrri(DisasContext* c, arg_AND_rrri* a) { return (void)a, T16(c) ? alu16(c) : wdata_shift(c); }
static bool trans_EOR_rrri(DisasContext* c, arg_EOR_rrri* a) { return (void)a, T16(c) ? alu16(c) : wdata_shift(c); }
static bool trans_ADC_rrri(DisasContext* c, arg_ADC_rrri* a) { return (void)a, T16(c) ? alu16(c) : wdata_shift(c); }
static bool trans_SBC_rrri(DisasContext* c, arg_SBC_rrri* a) { return (void)a, T16(c) ? alu16(c) : wdata_shift(c); }
static bool trans_TST_xrri(DisasContext* c, arg_TST_xrri* a) { return (void)a, T16(c) ? alu16(c) : wdata_shift(c); }
static bool trans_CMN_xrri(DisasContext* c, arg_CMN_xrri* a) { return (void)a, T16(c) ? alu16(c) : wdata_shift(c); }
static bool trans_ORR_rrri(DisasContext* c, arg_ORR_rrri* a) { return (void)a, T16(c) ? alu16(c) : wdata_shift(c); }
static bool trans_BIC_rrri(DisasContext* c, arg_BIC_rrri* a) { return (void)a, T16(c) ? alu16(c) : wdata_shift(c); }
static bool trans_MVN_rxri(DisasContext* c, arg_MVN_rxri* a) { return (void)a, T16(c) ? alu16(c) : wdata_shift(c); }
static bool trans_MOV_rxrr(DisasContext* c, arg_MOV_rxrr* a) {
    if (T16(c)) return alu16(c);
    /* LSL, LSR, ASR, ROR.W Rd, Rn, Rm: MOV (ORR with Rn 15) with a register-shifted operand */
    if ((c->b & 0xF0F0) != 0xF000) return undef(c);
    c->o->op = 2;
    c->o->flags = (c->a & 0x10 ? TH_S : 0) | TH_RSHIFT;
    c->o->rd = (unsigned)a->rd;
    c->o->rn = 15;
    c->o->rm = (unsigned)a->rm;
    c->o->imm = (unsigned)a->rs; /* the shift register */
    c->o->ra = (unsigned)a->shty;
    return shape(c, TH_WDATA);
}
static bool trans_MUL(DisasContext* c, arg_MUL* a) {
    if (T16(c)) return alu16(c);
    c->o->rn = (unsigned)a->rn;
    c->o->rd = (unsigned)a->rd;
    c->o->rm = (unsigned)a->rm;
    c->o->ra = (c->b >> 12) & 15;
    c->o->op = 0;
    return shape(c, TH_MUL);
}
static bool trans_RSB_rri(DisasContext* c, arg_RSB_rri* a) { return (void)a, T16(c) ? alu16(c) : wdata_imm(c); }

static bool trans_CMP_xrri(DisasContext* c, arg_CMP_xrri* a) {
    (void)a;
    if (!T16(c)) return wdata_shift(c);
    return (c->a >> 10) == 17 ? hi16(c, 1) : alu16(c);
}
static bool trans_ADD_rrri(DisasContext* c, arg_ADD_rrri* a) {
    if (!T16(c)) return wdata_shift(c);
    if ((c->a >> 10) == 17) return hi16(c, 0);
    return addsub16(c, ALU_ADD, a->rd, a->rn, a->rm, 0, 1);
}
static bool trans_SUB_rrri(DisasContext* c, arg_SUB_rrri* a) {
    return T16(c) ? addsub16(c, ALU_SUB, a->rd, a->rn, a->rm, 0, 1) : wdata_shift(c);
}
static bool trans_MOV_rxri(DisasContext* c, arg_MOV_rxri* a) {
    if (!T16(c)) return wdata_shift(c);
    return (c->a >> 10) == 17 ? hi16(c, 2) : shift_imm16(c, a);
}
T32_SHIFT(ORN_rrri) T32_SHIFT(TEQ_xrri) T32_SHIFT(RSB_rrri)
/* the MVE shifts of the ORR space: what the A-profile has there is ORR (MOV) with a shifted register */
T32_SHIFT(UQSHL_ri) T32_SHIFT(LSLL_ri) T32_SHIFT(UQSHLL_ri) T32_SHIFT(URSHR_ri) T32_SHIFT(LSRL_ri)
T32_SHIFT(URSHRL_ri) T32_SHIFT(SRSHR_ri) T32_SHIFT(ASRL_ri) T32_SHIFT(SRSHRL_ri) T32_SHIFT(SQSHL_ri)
T32_SHIFT(SQSHLL_ri) T32_SHIFT(UQRSHL_rr) T32_SHIFT(LSLL_rr) T32_SHIFT(UQRSHLL64_rr) T32_SHIFT(SQRSHR_rr)
T32_SHIFT(ASRL_rr) T32_SHIFT(SQRSHRL64_rr) T32_SHIFT(UQRSHLL48_rr) T32_SHIFT(SQRSHRL48_rr)

T32_IMM(TST_xri) T32_IMM(AND_rri) T32_IMM(BIC_rri) T32_IMM(ORR_rri) T32_IMM(MVN_rxi) T32_IMM(ORN_rri)
T32_IMM(TEQ_xri) T32_IMM(EOR_rri) T32_IMM(CMN_xri) T32_IMM(ADC_rri) T32_IMM(SBC_rri)

static bool trans_MOV_rxi(DisasContext* c, arg_MOV_rxi* a) { return (void)a, T16(c) ? imm8(c, I8_MOV) : wdata_imm(c); }
static bool trans_CMP_xri(DisasContext* c, arg_CMP_xri* a) { return (void)a, T16(c) ? imm8(c, I8_CMP) : wdata_imm(c); }
/* ADD and SUB with an immediate: T16's imm3, imm8 and SP forms; T32's modified immediate, and ADDW and SUBW (the
   first halfword's bit 9). SUBS PC, LR, #imm (ERET's space) needs privilege */
static bool trans_ADD_rri(DisasContext* c, arg_ADD_rri* a) {
    if (!T16(c)) return (c->a & 0x200) ? wimm(c) : wdata_imm(c);
    if (c->a < 0x2000) return addsub16(c, ALU_ADD, a->rd, a->rn, 0, a->imm, 0);
    if (c->a < 0x4000) return imm8(c, I8_ADD);
    if ((c->a >> 11) == 21) return addr16(c, ALU_ADD, (c->a >> 8) & 7, 13, (c->a & 255) * 4);
    return addr16(c, ALU_ADD, 13, 13, (c->a & 127) * 4);
}
static bool trans_SUB_rri(DisasContext* c, arg_SUB_rri* a) {
    if (!T16(c)) {
        if ((c->a & 0xF800) == 0xF000 && (c->b & 0x8000)) return control32(c);
        return (c->a & 0x200) ? wimm(c) : wdata_imm(c);
    }
    if (c->a < 0x2000) return addsub16(c, ALU_SUB, a->rd, a->rn, 0, a->imm, 0);
    if (c->a < 0x4000) return imm8(c, I8_SUB);
    return addr16(c, ALU_SUB, 13, 13, (c->a & 127) * 4);
}
static bool trans_ADR(DisasContext* c, arg_ADR* a) {
    (void)a;
    return T16(c) ? addr16(c, ALU_ADD, (c->a >> 8) & 7, 15, (c->a & 255) * 4) : wimm(c);
}
static bool trans_MOVW(DisasContext* c, arg_MOVW* a) { return (void)a, wimm(c); }
static bool trans_MOVT(DisasContext* c, arg_MOVT* a) { return (void)a, wimm(c); }

static bool trans_SSAT(DisasContext* c, arg_SSAT* a) { return (void)a, saturate32(c); }
static bool trans_USAT(DisasContext* c, arg_USAT* a) { return (void)a, saturate32(c); }
static bool trans_SSAT16(DisasContext* c, arg_SSAT16* a) { return (void)a, saturate32(c); }
static bool trans_USAT16(DisasContext* c, arg_USAT16* a) { return (void)a, saturate32(c); }
static bool trans_SBFX(DisasContext* c, arg_SBFX* a) { return (void)a, bitfield32(c); }
static bool trans_UBFX(DisasContext* c, arg_UBFX* a) { return (void)a, bitfield32(c); }
static bool trans_BFCI(DisasContext* c, arg_BFCI* a) { return (void)a, bitfield32(c); }

/* PKHBT, PKHTB: A32's, tb in bit 6 */
static bool trans_PKH(DisasContext* c, arg_PKH* a) {
    unsigned n = (unsigned)a->rn, d = (unsigned)a->rd, m = (unsigned)a->rm, b = c->b;
    if ((b & 0x8010) || n == 13 || n == 15 || d == 13 || d == 15 || m == 13 || m == 15) return undef(c);
    return twin(c, 0xE6800010u | (uint32_t)n << 16 | (uint32_t)d << 12 | ((b >> 12) & 7) << 9 | ((b >> 6) & 3) << 7 |
                       ((b >> 5) & 1) << 6 | m);
}

/* MLA, MLS: TH_MUL (op 1, 2) */
static bool trans_MLA(DisasContext* c, arg_MLA* a) {
    c->o->rn = (unsigned)a->rn;
    c->o->rd = (unsigned)a->rd;
    c->o->rm = (unsigned)a->rm;
    c->o->ra = (unsigned)a->ra;
    c->o->op = 1;
    return shape(c, TH_MUL);
}
static bool trans_MLS(DisasContext* c, arg_MLS* a) {
    c->o->rn = (unsigned)a->rn;
    c->o->rd = (unsigned)a->rd;
    c->o->rm = (unsigned)a->rm;
    c->o->ra = (unsigned)a->ra;
    c->o->op = 2;
    return shape(c, TH_MUL);
}

/* the long multiplies and divides, as A32's */
#define LONG(name, base)                                                                                               \
    static bool trans_##name(DisasContext* c, arg_##name* a) { return (void)a, twin(c, (base) | long_regs(c)); }
LONG(SMULL, 0xE0C00090u) LONG(UMULL, 0xE0800090u) LONG(SMLAL, 0xE0E00090u) LONG(UMLAL, 0xE0A00090u)
LONG(UMAAL, 0xE0400090u)
/* SMLAL<x><y>: N in bit 5, M in bit 4 of A32's; SMLALD, SMLSLD: X in bit 5 */
LONG(SMLALBB, 0xE1400080u) LONG(SMLALBT, 0xE14000C0u) LONG(SMLALTB, 0xE14000A0u) LONG(SMLALTT, 0xE14000E0u)
LONG(SMLALD, 0xE7400010u) LONG(SMLALDX, 0xE7400030u) LONG(SMLSLD, 0xE7400050u) LONG(SMLSLDX, 0xE7400070u)
static bool trans_SDIV(DisasContext* c, arg_SDIV* a) {
    return (void)a, twin(c, 0xE710F010u | (uint32_t)((c->b >> 8) & 15) << 16 | (uint32_t)(c->b & 15) << 8 | (c->a & 15));
}
static bool trans_UDIV(DisasContext* c, arg_UDIV* a) {
    return (void)a, twin(c, 0xE730F010u | (uint32_t)((c->b >> 8) & 15) << 16 | (uint32_t)(c->b & 15) << 8 | (c->a & 15));
}

/* the halfword, dual and most significant word multiplies and USAD8: Ra 15 for the forms without the accumulate */
#define MULACC(name, base, base_noacc)                                                                                 \
    static bool trans_##name(DisasContext* c, arg_##name* a) {                                                         \
        (void)a;                                                                                                       \
        return twin(c, ((c->b >> 12) == 15 ? (base_noacc) : (base) | mul_acc(c)) | mul_regs(c));                       \
    }
#define MULALL(name, base)                                                                                             \
    static bool trans_##name(DisasContext* c, arg_##name* a) { return (void)a, twin(c, (base) | mul_acc(c) | mul_regs(c)); }
MULACC(SMLABB, 0xE1000080u, 0xE1600080u) MULACC(SMLABT, 0xE10000C0u, 0xE16000C0u)
MULACC(SMLATB, 0xE10000A0u, 0xE16000A0u) MULACC(SMLATT, 0xE10000E0u, 0xE16000E0u)
MULACC(SMULBB, 0xE1000080u, 0xE1600080u) MULACC(SMULBT, 0xE10000C0u, 0xE16000C0u)
MULACC(SMULTB, 0xE10000A0u, 0xE16000A0u) MULACC(SMULTT, 0xE10000E0u, 0xE16000E0u)
MULACC(SMLAWB, 0xE1200080u, 0xE12000A0u) MULACC(SMLAWT, 0xE12000C0u, 0xE12000E0u)
MULACC(SMULWB, 0xE1200080u, 0xE12000A0u) MULACC(SMULWT, 0xE12000C0u, 0xE12000E0u)
MULALL(SMLAD, 0xE7000010u) MULALL(SMLADX, 0xE7000030u) MULALL(SMLSD, 0xE7000050u) MULALL(SMLSDX, 0xE7000070u)
MULALL(SMMLA, 0xE7500010u) MULALL(SMMLAR, 0xE7500030u) MULALL(SMMLS, 0xE75000D0u) MULALL(SMMLSR, 0xE75000F0u)
MULALL(USADA8, 0xE7800010u)

/* the register-only data processing of the FA space, as A32's: Rd Rn Rm from the fields */
#define RRR(name, base)                                                                                                \
    static bool trans_##name(DisasContext* c, arg_##name* a) {                                                         \
        return twin(c, (base) | (uint32_t)a->rn << 16 | (uint32_t)a->rd << 12 | (uint32_t)a->rm);                      \
    }
/* QADD, QSUB, QDADD, QDSUB take Rd, Rm, Rn in A32's order */
#define QRR(name, base)                                                                                                \
    static bool trans_##name(DisasContext* c, arg_##name* a) {                                                         \
        return twin(c, (base) | (uint32_t)a->rn << 16 | (uint32_t)a->rd << 12 | (uint32_t)a->rm);                      \
    }
QRR(QADD, 0xE1000050u) QRR(QSUB, 0xE1200050u) QRR(QDADD, 0xE1400050u) QRR(QDSUB, 0xE1600050u)
RRR(SEL, 0xE6800FB0u)
#define PAR(p, base)                                                                                                   \
    RRR(p##ADD16, (base) | 0x10) RRR(p##ASX, (base) | 0x30) RRR(p##SAX, (base) | 0x50) RRR(p##SUB16, (base) | 0x70)    \
        RRR(p##ADD8, (base) | 0x90) RRR(p##SUB8, (base) | 0xF0)
PAR(S, 0xE6100F00u) PAR(Q, 0xE6200F00u) PAR(SH, 0xE6300F00u) PAR(U, 0xE6500F00u) PAR(UQ, 0xE6600F00u)
PAR(UH, 0xE6700F00u)
/* CRC32 is A32 only here, as the decoder had it */
static bool trans_CRC32B(DisasContext* c, arg_CRC32B* a) { return (void)a, undef(c); }
static bool trans_CRC32H(DisasContext* c, arg_CRC32H* a) { return (void)a, undef(c); }
static bool trans_CRC32W(DisasContext* c, arg_CRC32W* a) { return (void)a, undef(c); }
static bool trans_CRC32CB(DisasContext* c, arg_CRC32CB* a) { return (void)a, undef(c); }
static bool trans_CRC32CH(DisasContext* c, arg_CRC32CH* a) { return (void)a, undef(c); }
static bool trans_CRC32CW(DisasContext* c, arg_CRC32CW* a) { return (void)a, undef(c); }

/* REV, REV16, RBIT, REVSH, CLZ: T16's REV forms (TH_REV, op the halfword's bits 7:6), T32's as A32's */
#define RR(name, base, t16)                                                                                            \
    static bool trans_##name(DisasContext* c, arg_##name* a) {                                                         \
        if (T16(c)) {                                                                                                  \
            c->o->op = (c->a >> 6) & 3;                                                                                \
            c->o->rd = (unsigned)a->rd;                                                                                \
            c->o->rm = (unsigned)a->rm;                                                                                \
            return t16 ? shape(c, TH_REV) : undef(c);                                                                  \
        }                                                                                                              \
        if ((c->a & 15) != (c->b & 15)) return undef(c);                                                              \
        return twin(c, (base) | (uint32_t)a->rd << 12 | (uint32_t)a->rm);                                              \
    }
RR(REV, 0xE6BF0F30u, 1) RR(REV16, 0xE6BF0FB0u, 1) RR(REVSH, 0xE6FF0FB0u, 1) RR(RBIT, 0xE6FF0F30u, 0)
RR(CLZ, 0xE16F0F10u, 0)

/* the extends with add (T32, Rn 15 without the add) and T16's SXTH, SXTB, UXTH, UXTB (TH_EXTEND, op the halfword's
   bits 7:6) */
#define XT(name, base)                                                                                                 \
    static bool trans_##name(DisasContext* c, arg_##name* a) {                                                         \
        if (T16(c)) {                                                                                                  \
            c->o->op = (c->a >> 6) & 3;                                                                                \
            c->o->rd = (unsigned)a->rd;                                                                                \
            c->o->rm = (unsigned)a->rm;                                                                                \
            return shape(c, TH_EXTEND);                                                                                \
        }                                                                                                              \
        return twin(c, (base) | (uint32_t)a->rn << 16 | (uint32_t)a->rd << 12 | (uint32_t)a->rot << 10 |               \
                           (uint32_t)a->rm);                                                                           \
    }
XT(SXTAH, 0xE6B00070u) XT(UXTAH, 0xE6F00070u) XT(SXTAB16, 0xE6800070u) XT(UXTAB16, 0xE6C00070u)
XT(SXTAB, 0xE6A00070u) XT(UXTAB, 0xE6E00070u)

/* the hints, barriers and CLREX of the control space, and the memory hint the NOP name also has (PLI's space) */
#define CONTROL(name)                                                                                                  \
    static bool trans_##name(DisasContext* c, arg_##name* a) { return (void)a, control32(c); }
CONTROL(YIELD) CONTROL(WFE) CONTROL(WFI) CONTROL(SEV) CONTROL(SEVL) CONTROL(ESB) CONTROL(CLREX) CONTROL(DSB)
CONTROL(DMB) CONTROL(ISB) CONTROL(SB) CONTROL(MSR_reg) CONTROL(MRS_reg) CONTROL(MSR_bank) CONTROL(MRS_bank)
CONTROL(MSR_v7m) CONTROL(MRS_v7m) CONTROL(BXJ) CONTROL(ERET) CONTROL(SMC) CONTROL(HVC)
static bool trans_NOP(DisasContext* c, arg_NOP* a) {
    (void)a;
    return (c->a & 0xFE00) == 0xF800 ? load_store32(c) : control32(c);
}
static bool trans_CPS(DisasContext* c, arg_CPS* a) {
    (void)a;
    if (!T16(c)) return control32(c);
    c->o->op = 0; /* what CPS changes isn't user mode's, which runs it as NOP */
    return shape(c, TH_HINT);
}
static bool trans_UDF(DisasContext* c, arg_UDF* a) { return (void)a, T16(c) ? undef(c) : control32(c); }

/* B<c>: T16's (TH_BRANCH BR_BCOND, .ra the condition), T32's B<c>.W or the control space */
static bool trans_B_cond_thumb(DisasContext* c, arg_B_cond_thumb* a) {
    if (!T16(c)) return control32(c);
    c->o->ra = (unsigned)a->cond;
    return branch16(c, BR_BCOND, c->a & 255);
}
/* B, BL, BLX: T16's B (imm11); T32's with both halfwords in .rn and .rm, which the emitter decodes */
static bool branch32(DisasContext* c, unsigned op) {
    c->o->rn = c->a;
    c->o->rm = c->b;
    c->o->op = op;
    return shape(c, TH_BRANCH);
}
static bool trans_B(DisasContext* c, arg_B* a) {
    (void)a;
    return T16(c) ? branch16(c, BR_B, c->a & 0x7FF) : branch32(c, BR_BW);
}
static bool trans_BL(DisasContext* c, arg_BL* a) { return (void)a, branch32(c, BR_BL); }
static bool trans_BLX_i(DisasContext* c, arg_BLX_i* a) { return (void)a, branch32(c, BR_BLX_IMM); }

/* the loads and stores: T16's register, immediate, SP- and PC-relative forms, T32's of every addressing */
#define LDST_RR(name, load, size)                                                                                      \
    static bool trans_##name(DisasContext* c, arg_##name* a) {                                                         \
        return T16(c) ? ldst16_rr(c, a, load, size) : load_store32(c);                                                 \
    }
#define LDST_RI(name, load, size)                                                                                      \
    static bool trans_##name(DisasContext* c, arg_##name* a) {                                                         \
        return T16(c) ? ldst16_ri(c, a, load, size) : load_store32(c);                                                 \
    }
#define LDST32(name)                                                                                                   \
    static bool trans_##name(DisasContext* c, arg_##name* a) { return (void)a, load_store32(c); }
LDST_RR(STR_rr, 0, 0) LDST_RR(STRH_rr, 0, TH_H) LDST_RR(STRB_rr, 0, TH_B) LDST_RR(LDRSB_rr, 1, TH_B | TH_SIGN)
LDST_RR(LDR_rr, 1, 0) LDST_RR(LDRH_rr, 1, TH_H) LDST_RR(LDRB_rr, 1, TH_B) LDST_RR(LDRSH_rr, 1, TH_H | TH_SIGN)
LDST_RI(STR_ri, 0, 0) LDST_RI(LDR_ri, 1, 0) LDST_RI(STRB_ri, 0, TH_B) LDST_RI(LDRB_ri, 1, TH_B)
LDST_RI(STRH_ri, 0, TH_H) LDST_RI(LDRH_ri, 1, TH_H)
LDST32(STRBT_ri) LDST32(STRHT_ri) LDST32(STRT_ri) LDST32(LDRBT_ri) LDST32(LDRHT_ri) LDST32(LDRT_ri)
LDST32(LDRSB_ri) LDST32(LDRSBT_ri) LDST32(LDRSH_ri) LDST32(LDRSHT_ri) LDST32(PLD) LDST32(PLDW) LDST32(PLI)

/* the E8 and E9 space: LDM and STM, LDRD and STRD, the exclusives and acquire/release, TBB and TBH. T16's STM and
   LDM_t16 are PUSH and POP (B4, BC: TH_PUSHPOP, .ra the extra register's bit) and STMIA and LDMIA (C0, C8: TH_LDMSTM,
   a load with its base in the list loads it, without writeback) */
static bool ldm_stm16(DisasContext* c, unsigned load) {
    if ((c->a >> 12) == 0xB) {
        c->o->op = load;
        c->o->imm = c->a & 255;
        c->o->ra = (c->a >> 8) & 1;
        return shape(c, TH_PUSHPOP);
    }
    c->o->op = load;
    c->o->rn = (c->a >> 8) & 7;
    c->o->imm = c->a & 255;
    c->o->flags = load && ((c->a & 255) >> c->o->rn & 1) ? 0 : TH_WB;
    return shape(c, TH_LDMSTM);
}
static bool trans_STM(DisasContext* c, arg_STM* a) { return (void)a, ldm_stm16(c, 0); }
static bool trans_LDM_t16(DisasContext* c, arg_LDM_t16* a) { return (void)a, ldm_stm16(c, 1); }
static bool trans_STM_t32(DisasContext* c, arg_STM_t32* a) { return ldm_stm32(c, a); }
static bool trans_LDM_t32(DisasContext* c, arg_LDM_t32* a) { return ldm_stm32(c, a); }
static bool trans_STRD_ri_t32(DisasContext* c, arg_STRD_ri_t32* a) { return dual32(c, a, 0); }
static bool trans_LDRD_ri_t32(DisasContext* c, arg_LDRD_ri_t32* a) { return dual32(c, a, 1); }
static bool trans_STREX(DisasContext* c, arg_STREX* a) { return strex_t1(c, a); }
static bool trans_LDREX(DisasContext* c, arg_LDREX* a) { return ldrex_t1(c, a); }
#define EXCL(name)                                                                                                     \
    static bool trans_##name(DisasContext* c, arg_##name* a) { return (void)a, exclusive32(c); }
EXCL(STREXB) EXCL(STREXH) EXCL(STREXD_t32) EXCL(STLEX) EXCL(STLEXB) EXCL(STLEXH) EXCL(STLEXD_t32) EXCL(STL)
EXCL(STLB) EXCL(STLH) EXCL(LDREXB) EXCL(LDREXH) EXCL(LDREXD_t32) EXCL(LDAEX) EXCL(LDAEXB) EXCL(LDAEXH)
EXCL(LDAEXD_t32) EXCL(LDA) EXCL(LDAB) EXCL(LDAH)
static bool table(DisasContext* c, arg_tbranch* a, unsigned half) {
    if ((c->b & 0xFFE0) != 0xF000) return undef(c);
    c->o->op = half;
    c->o->rn = (unsigned)a->rn;
    c->o->rm = (unsigned)a->rm;
    return shape(c, TH_TABLE);
}
static bool trans_TBB(DisasContext* c, arg_TBB* a) { return table(c, a, 0); }
static bool trans_TBH(DisasContext* c, arg_TBH* a) { return table(c, a, 1); }

/* T16's branches and the rest of its miscellaneous space */
static bool trans_BX(DisasContext* c, arg_BX* a) {
    c->o->rm = (unsigned)a->rm;
    return branch16(c, BR_BX, 0);
}
static bool trans_BLX_r(DisasContext* c, arg_BLX_r* a) {
    c->o->rm = (unsigned)a->rm;
    return branch16(c, BR_BLX_REG, 0);
}
static bool trans_CBZ(DisasContext* c, arg_CBZ* a) {
    c->o->rn = (unsigned)a->rn;
    return branch16(c, a->nz ? BR_CBNZ : BR_CBZ, (uint32_t)a->imm);
}
static bool trans_SVC(DisasContext* c, arg_SVC* a) { return branch16(c, BR_SVC, (uint32_t)a->imm); }
static bool trans_BKPT(DisasContext* c, arg_BKPT* a) { return branch16(c, BR_BKPT, (uint32_t)a->imm); }
/* NOP, YIELD, WFE, WFI, SEV (op 0 to 4); SEVL and the reserved hints behave as NOP */
static bool trans_MAYBE_UNDEF_T1_HINT(DisasContext* c, arg_MAYBE_UNDEF_T1_HINT* a) {
    unsigned hint = (c->a >> 4) & 15;
    (void)a;
    c->o->op = hint <= 4 ? hint : 0;
    return shape(c, TH_HINT);
}
static bool trans_IT(DisasContext* c, arg_IT* a) {
    c->o->imm = (uint32_t)a->cond_mask;
    return shape(c, TH_IT);
}

/* what the backend doesn't run: the M profile's and the privileged instructions, and the halves of ARMv4T's BL,
   which the ARM11 and ARM7TDMI profiles decode themselves (profile.c) */
#define UNDEF(name)                                                                                                    \
    static bool trans_##name(DisasContext* c, arg_##name* a) { return (void)a, undef(c); }
UNDEF(CSEL) UNDEF(SG) UNDEF(TT) UNDEF(CLRM) UNDEF(RFE) UNDEF(SRS) UNDEF(BF) UNDEF(DLS) UNDEF(WLS) UNDEF(LE)
UNDEF(LCTP) UNDEF(VCTP) UNDEF(BXNS) UNDEF(BLXNS) UNDEF(SETEND) UNDEF(CPS_v7m) UNDEF(HLT) UNDEF(BLX_suffix)
UNDEF(BL_BLX_prefix) UNDEF(BL_suffix) UNDEF(MCR) UNDEF(MRC) UNDEF(MCRR) UNDEF(MRRC)

/* =========================================================================================== decode */

/* Coprocessor, VFP and the v8 FP instructions: the A32 word with the condition in the first halfword's top nibble.
   Advanced SIMD data processing and element/structure transfers move their top bits (Arm ARM, "Advanced SIMD and
   floating-point instruction encoding") */
static unsigned coprocessor(DisasContext* c) {
    unsigned a = c->a, b = c->b;
    uint32_t word;
    if ((a & 0xEF00) == 0xEF00)
        word = 0xF2000000u | ((uint32_t)(a & 0x1000) << 12) | ((uint32_t)(a & 0xFF) << 16) | b;
    else if ((a & 0xFF10) == 0xF900)
        word = 0xF4000000u | ((uint32_t)(a & 0xFF) << 16) | b;
    else
        word = ((uint32_t)a << 16) | b;
    SnxA32Kind kind = snx_a32_decode(word);
    if (kind == SNX_A32_VECTOR) { /* Thumb's VSTR, VLDM and VSTM can't take the PC as their base, as A32's can */
        SnxArmVector v;
        snx_arm_vector_decode(word, &v);
        if (v.op == AV_MEM && v.n == 15 && (!(v.flags & AV_LOAD) || !(v.flags & AV_PRE) || (v.flags & AV_WB)))
            return 0;
    }
    if (kind != SNX_A32_VECTOR && kind != SNX_A32_COPROC) return 0;
    twin_kind(c, word, kind, 0);
    return TH_F32;
}

/* the encodings t32.decode has no pattern for that the backend runs: T32 data processing with the reserved ops, in
   the modified immediate and shifted register forms (they keep the op for the emitter, as the decoder that came
   before did), and the
   byte, halfword and word store-release forms with a nonzero Rd field */
static unsigned unmatched32(DisasContext* c) {
    if ((c->a & 0xFFF0) == 0xEAC0) return 0; /* PKH's space */
    if ((c->a >> 11) == 0x1E && !(c->a & 0x200) && !(c->b & 0x8000)) {
        c->len = TH_F32;
        wdata_imm(c);
        return c->len;
    }
    if ((c->a >> 9) == (0xEA00 >> 9)) {
        c->len = TH_F32;
        wdata_shift(c);
        return c->len;
    }
    if ((c->a & 0xFFF0) == 0xE8C0) {
        c->len = TH_F32;
        exclusive32(c);
        return c->len;
    }
    return 0;
}

unsigned snx_thumb_decode(uint16_t hw1, uint16_t hw2, SnxThumb* out) {
    DisasContext c;
    *out = (SnxThumb){0};
    c.o = out;
    c.a = hw1;
    c.t16 = (hw1 >> 11) < 0x1D;
    if ((hw1 >> 11) >= 0x1D) {
        c.b = hw2;
        c.len = TH_F32;
        out->len = TH_F32;
        if ((hw1 & 0xEC00) == 0xEC00 || (hw1 & 0xFF10) == 0xF900)
            c.len = coprocessor(&c);
        else if (!disas_t32(&c, (uint32_t)hw1 << 16 | hw2))
            c.len = unmatched32(&c);
    } else {
        c.b = 0;
        c.len = TH_F16;
        out->len = TH_F16;
        if (!disas_t16(&c, hw1)) c.len = 0;
    }
    if (!c.len) {
        *out = (SnxThumb){0};
        out->shape = TH_UNDEF;
    }
    return c.len;
}
