/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate.c, vec_helper.c, vfp_helper.c, crypto_helper.c),
   LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd.
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org> */
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"

static unsigned thumb16(uint16_t h, SnxThumb* o) {
    unsigned g = h >> 11;
    o->len = TH_F16;
    o->flags = 0;
    o->op = 0;
    o->ra = 0;
    if (g <= 2) { /* LSL/LSR/ASR Rd, Rm, #imm5 */
        o->shape = TH_SHIFT_IMM;
        o->op = g == 0 ? ALU_LSL : g == 1 ? ALU_LSR : ALU_ASR;
        o->rd = h & 7;
        o->rm = (h >> 3) & 7;
        o->imm = (h >> 6) & 31;
        o->flags = TH_S;
        return TH_F16;
    }
    if (g == 3) { /* ADD/SUB Rd, Rn, Rm|#imm3 */
        o->shape = TH_ADDSUB;
        o->op = (h >> 9) & 1 ? ALU_SUB : ALU_ADD;
        o->rn = (h >> 3) & 7;
        o->rd = h & 7;
        if ((h >> 10) & 1)
            o->imm = (h >> 6) & 7;
        else {
            o->rm = (h >> 6) & 7;
            o->flags = TH_REG;
        }
        o->flags |= TH_S;
        return TH_F16;
    }
    if (g >= 4 && g <= 7) { /* MOV/CMP/ADD/SUB Rd, #imm8 */
        o->shape = TH_IMM8;
        o->op = g - 4;
        o->rd = (h >> 8) & 7;
        o->imm = h & 255;
        return TH_F16;
    }
    if ((h >> 10) == 16) { /* data-processing register */
        o->shape = TH_ALU;
        o->op = (h >> 6) & 15;
        o->rd = h & 7;
        o->rn = h & 7;
        o->rm = (h >> 3) & 7;
        o->flags = TH_S;
        return TH_F16;
    }
    if ((h >> 10) == 17) { /* special data / branch exchange */
        unsigned op = (h >> 8) & 3, hd = (h >> 7) & 1, hs = (h >> 6) & 1;
        if (op == 3) { /* BX / BLX register: Rm in bits[6:3], L in bit 7 */
            if (h & 7) return 0;
            o->shape = TH_BRANCH;
            o->op = (h >> 7) & 1 ? BR_BLX_REG : BR_BX;
            o->rm = (h >> 3) & 15;
            return TH_F16;
        }
        o->shape = TH_HI;
        o->op = op; /* 0=ADD 1=CMP 2=MOV */
        o->rd = (h & 7) | (hd << 3);
        o->rm = ((h >> 3) & 7) | (hs << 3); /* Rm */
        o->rn = o->rd;                      /* ADD reads Rd as its first source */
        return TH_F16;
    }
    if (g == 9) { /* load literal: PC-relative word load (Thumb format 9, needed by every Thumb ROM) */
        o->shape = TH_LOAD;
        o->rd = (h >> 8) & 7;
        o->rn = 15;
        o->flags = TH_PCB | TH_UP | TH_PRE;
        o->imm = (h & 255) * 4;
        return TH_F16;
    }
    if (g == 10 || g == 11) { /* load/store register offset */
        unsigned sub = (h >> 9) & 7;
        o->shape = (sub >= 3) ? TH_LOAD : TH_STORE;
        o->rd = h & 7;
        o->rn = (h >> 3) & 7;
        o->rm = (h >> 6) & 7;
        o->flags = TH_REG | TH_UP;
        switch (sub) {
            case 1: o->flags |= TH_H; break;
            case 2: o->flags |= TH_B; break;
            case 3: o->flags |= TH_B | TH_SIGN; break;
            case 5: o->flags |= TH_H; break;
            case 6: o->flags |= TH_B; break;
            case 7: o->flags |= TH_H | TH_SIGN; break;
        }
        return TH_F16;
    }
    if (g >= 12 && g <= 15) { /* load/store word/byte immediate */
        o->shape = (h >> 11) & 1 ? TH_LOAD : TH_STORE;
        o->rd = h & 7;
        o->rn = (h >> 3) & 7;
        o->flags = ((h >> 12) & 1 ? TH_B : 0) | TH_UP | TH_PRE;
        o->imm = ((h >> 6) & 31) * (o->flags & TH_B ? 1 : 4);
        return TH_F16;
    }
    if (g == 16 || g == 17) { /* load/store halfword immediate */
        o->shape = g == 17 ? TH_LOAD : TH_STORE;
        o->rd = h & 7;
        o->rn = (h >> 3) & 7;
        o->flags = TH_H | TH_UP | TH_PRE;
        o->imm = ((h >> 6) & 31) * 2;
        return TH_F16;
    }
    if (g == 18 || g == 19) { /* SP-relative load/store */
        o->shape = g == 19 ? TH_LOAD : TH_STORE;
        o->rd = (h >> 8) & 7;
        o->rn = 13;
        o->flags = TH_SPB | TH_UP | TH_PRE;
        o->imm = (h & 255) * 4;
        return TH_F16;
    }
    if (g == 20 || g == 21) { /* load address */
        o->shape = TH_ADDR;
        o->op = ALU_ADD;
        o->rd = (h >> 8) & 7;
        o->rn = g == 21 ? 13 : 15;
        o->imm = (h & 255) * 4;
        return TH_F16;
    }
    if (g == 22 || g == 23) { /* misc 0xB0-0xBF */
        unsigned top = h >> 8;
        if (top == 0xB0) { /* ADD/SUB SP, #imm7 */
            o->shape = TH_ADDR;
            o->op = (h >> 7) & 1 ? ALU_SUB : ALU_ADD;
            o->rd = 13;
            o->rn = 13;
            o->imm = (h & 127) * 4;
            return TH_F16;
        }
        if (top == 0xB2) { /* sign/zero extend */
            o->shape = TH_EXTEND;
            o->op = (h >> 6) & 3;
            o->rd = h & 7;
            o->rm = (h >> 3) & 7;
            return TH_F16;
        }
        if (top == 0xB4 || top == 0xB5 || top == 0xBC || top == 0xBD) { /* PUSH/POP */
            o->shape = TH_PUSHPOP;
            o->op = (top == 0xBC || top == 0xBD) ? 1 : 0;
            o->imm = h & 255;
            o->ra = (h >> 8) & 1;
            return TH_F16;
        }
        if (top == 0xB1 || top == 0xB3 || top == 0xB9 || top == 0xBB) { /* CBZ/CBNZ */
            o->shape = TH_BRANCH;
            o->op = (top == 0xB9 || top == 0xBB) ? BR_CBNZ : BR_CBZ;
            o->rn = h & 7;
            o->imm = (((h >> 9) & 1) << 5) | ((h >> 3) & 31);
            o->imm <<= 1;
            return TH_F16;
        }
        if (top == 0xBF) { /* hints and IT */
            unsigned firstcond = (h >> 4) & 15, mask = h & 15;
            if (!mask) {
                if (firstcond <= 4) {
                    o->shape = TH_HINT;
                    o->op = firstcond;
                    return TH_F16;
                }
                return 0;
            }
            o->shape = TH_IT;
            o->imm = (firstcond << 4) | mask;
            return TH_F16;
        }
        if (top == 0xBA && ((h >> 6) & 3) != 2) { /* REV, REV16, REVSH (.op 0, 1, 3, as TH_REV takes them) */
            o->shape = TH_REV;
            o->op = (h >> 6) & 3;
            o->rd = h & 7;
            o->rm = (h >> 3) & 7;
            return TH_F16;
        }
        if (top == 0xBE) { /* BKPT #imm8 */
            o->shape = TH_BRANCH;
            o->op = BR_BKPT;
            o->imm = h & 255;
            return TH_F16;
        }
        return 0;
    }
    if (g == 24 || g == 25) { /* STMIA/LDMIA */
        o->shape = TH_LDMSTM;
        o->op = g == 25 ? 1 : 0;
        o->rn = (h >> 8) & 7;
        o->imm = h & 255;
        o->flags = TH_WB;
        return TH_F16;
    }
    if (g == 26 || g == 27) { /* conditional branch / SVC */
        unsigned cond = (h >> 8) & 15;
        if (cond == 15) {
            o->shape = TH_BRANCH;
            o->op = BR_SVC;
            o->imm = h & 255;
            return TH_F16;
        }
        if (cond == 14) return 0;
        o->shape = TH_BRANCH;
        o->op = BR_BCOND;
        o->ra = cond;
        o->imm = h & 255;
        return TH_F16;
    }
    if (g == 28) { /* unconditional branch */
        o->shape = TH_BRANCH;
        o->op = BR_B;
        o->imm = h & 0x7FF;
        return TH_F16;
    }
    o->shape = TH_UNDEF;
    o->len = 0;
    return 0;
}

/* ThumbExpandImm: the 12-bit modified immediate shared by T32 data-processing. */
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
    {
        unsigned rot = (imm12 >> 7) & 31;
        uint32_t v = 0x80u | (imm12 & 127);
        return (v >> rot) | (v << ((32 - rot) & 31));
    }
}

/* A Thumb32 instruction that is an A32 one with its fields elsewhere (condition AL): run by the A32 emitter as that
   (TH_A32). kind is what the A32 decoder makes of word, unless the caller checked the Thumb rules itself; 0 when the
   A32 decoder refuses it */
static unsigned twin_kind(SnxThumb* o, uint32_t word, SnxA32Kind kind, unsigned pair) {
    if (kind == SNX_A32_UNDEFINED) return 0;
    o->shape = TH_A32;
    o->imm = word;
    o->op = kind;
    o->ra = pair;
    return TH_F32;
}
static unsigned twin(SnxThumb* o, uint32_t word) { return twin_kind(o, word, snx_a32_decode(word), 0); }

/* Coprocessor, VFP and the v8 FP instructions: the A32 word with the condition in the first halfword's top nibble.
   Advanced SIMD data processing and element/structure transfers move their top bits (Arm ARM, "Advanced SIMD and
   floating-point instruction encoding") */
static unsigned coprocessor(unsigned a, unsigned b, SnxThumb* o) {
    uint32_t word;
    SnxA32Kind kind;
    if ((a & 0xEF00) == 0xEF00)
        word = 0xF2000000u | ((uint32_t)(a & 0x1000) << 12) | ((uint32_t)(a & 0xFF) << 16) | b;
    else if ((a & 0xFF10) == 0xF900)
        word = 0xF4000000u | ((uint32_t)(a & 0xFF) << 16) | b;
    else
        word = ((uint32_t)a << 16) | b;
    kind = snx_a32_decode(word);
    if (kind == SNX_A32_VECTOR) { /* Thumb's VSTR, VLDM and VSTM can't take the PC as their base, as A32's can */
        SnxArmVector v;
        snx_arm_vector_decode(word, &v);
        if (v.op == AV_MEM && v.n == 15 && (!(v.flags & AV_LOAD) || !(v.flags & AV_PRE) || (v.flags & AV_WB)))
            return 0;
    }
    return kind == SNX_A32_VECTOR || kind == SNX_A32_COPROC ? twin_kind(o, word, kind, 0) : 0;
}

/* E8xx/E9xx: LDM/STM (and PUSH.W/POP.W), LDRD/STRD, the exclusives and acquire/release, TBB/TBH */
static unsigned multiple_dual(unsigned a, unsigned b, SnxThumb* o) {
    unsigned n = a & 15, op1 = (a >> 7) & 3, op2 = (a >> 4) & 3, op3 = (b >> 4) & 15;
    unsigned t = b >> 12, t2 = (b >> 8) & 15, d = b & 15, i, count = 0;
    if (!(a & 0x40)) { /* LDM/STM IA (op1 1) or DB (op1 2), with Thumb's stricter lists */
        unsigned load = (a >> 4) & 1, wb = (a >> 5) & 1;
        if (op1 != 1 && op1 != 2) return 0;
        for (i = 0; i < 16; ++i) count += (b >> i) & 1;
        if (n == 15 || count < 2 || (b & 0x2000) || (load ? (b & 0xC000) == 0xC000 : (b & 0x8000)) ||
            (wb && ((b >> n) & 1)))
            return 0;
        return twin(o, 0xE8000000u | (op1 == 2 ? 1u << 24 : 1u << 23) | ((uint32_t)wb << 21) | ((uint32_t)load << 20) |
                           ((uint32_t)n << 16) | b);
    }
    if (op1 >= 2 || op2 >= 2) { /* LDRD/STRD: P = op1 bit 1, U = op1 bit 0, W = op2 bit 1, L = op2 bit 0 */
        unsigned load = op2 & 1, wb = op2 >> 1;
        if (t == 13 || t == 15 || t2 == 13 || t2 == 15 || (load && t == t2) || (wb && (n == t || n == t2)) ||
            (n == 15 && (wb || !load)))
            return 0;
        o->shape = TH_DUAL;
        o->op = load;
        o->rn = n;
        o->rd = t;
        o->rm = t2;
        o->imm = (b & 0xFF) * 4;
        o->flags = (op1 & 2 ? TH_PRE : 0) | (op1 & 1 ? TH_UP : 0) | (wb ? TH_WB : 0);
        return TH_F32;
    }
    if (op1 == 0) { /* STREX/LDREX (T1): A32's have no offset */
        if (b & 0xFF) return 0;
        if (op2 == 0) return twin(o, 0xE1800F90u | (uint32_t)n << 16 | (uint32_t)t2 << 12 | t); /* Rd in hw2[11:8] */
        return t2 == 15 ? twin(o, 0xE1900F9Fu | (uint32_t)n << 16 | (uint32_t)t << 12) : 0;
    }
    if (op2 == 1 && op3 < 2) {                /* TBB / TBH */
        if ((b & 0xFFE0) != 0xF000) return 0; /* 1111 0000 000H Rm */
        o->shape = TH_TABLE;
        o->op = op3;
        o->rn = n;
        o->rm = b & 15;
        return TH_F32;
    }
    /* op1 1: byte, halfword and doubleword exclusives (op3 01ss), LDA/STL (10ss) and LDAEX/STLEX (11ss), ss the size:
       B, H, word, D. A32's have the size in bits 22:21 (word 00, D 01, B 10, H 11) and the kind in bits 11:8 */
    {
        static const unsigned size32[4] = {2, 3, 0, 1};
        static const unsigned kind32[4] = {0, 0xF, 0xC, 0xE};
        unsigned size = op3 & 3, kind = op3 >> 2, load = op2 & 1, pair = 0;
        uint32_t word;
        if (!kind || (kind == 1 && size == 2) || (kind == 2 && size == 3) || n == 15) return 0;
        if (size == 3) { /* the second register is any one: the A32 emitter takes it from .ra, A32's being Rt + 1 */
            if (t == 13 || t == 15 || t2 == 13 || t2 == 15 || (load && (t == t2 || d != 15)) ||
                (!load && (d == 13 || d == 15 || d == n || d == t || d == t2)))
                return 0;
            pair = t2 + 1;
        } else if (t2 != 15 || (load && d != 15))
            return 0;
        word = 0xE1800090u | (uint32_t)size32[size] << 21 | (uint32_t)load << 20 | (uint32_t)n << 16 |
               (uint32_t)kind32[kind] << 8;
        if (load)
            word |= (uint32_t)t << 12 | 0xF;
        else
            word |= (uint32_t)(kind == 2 ? 15 : d) << 12 | t;
        if (pair) return twin_kind(o, word, SNX_A32_EXCLUSIVE, pair);
        return twin(o, word);
    }
}

/* F0xx-F7xx with hw2 bit 15 set: branches, and the miscellaneous control instructions where a B<c>.W would have
   condition 111x */
static unsigned branch_control(unsigned a, unsigned b, SnxThumb* o) {
    unsigned op = (a >> 4) & 0x7F, n = a & 15;
    o->rn = a;
    o->rm = b;
    if ((b & 0xC000) == 0xC000) { /* BL / BLX */
        o->shape = TH_BRANCH;
        o->op = (b & 0x1000) ? BR_BL : BR_BLX_IMM;
        return TH_F32;
    }
    if ((b & 0xD000) == 0x9000) { /* B.W */
        o->shape = TH_BRANCH;
        o->op = BR_BW;
        return TH_F32;
    }
    if (b & 0x5000) return 0;
    if (((a >> 7) & 7) != 7) { /* B<c>.W (T3) */
        o->shape = TH_BRANCH;
        o->op = BR_BCOND_W;
        o->ra = (a >> 6) & 15;
        o->imm = (((a >> 10) & 1) << 19) | (((b >> 11) & 1) << 18) | (((b >> 13) & 1) << 17) | ((a & 0x3F) << 11) |
                 (b & 0x7FF); /* S:J2:J1:imm6:imm11, in halfwords */
        return TH_F32;
    }
    if (op == 0x38) /* MSR APSR_<mask>, Rn: mask in hw2[11:10] (nzcvq, g) */
        return (b & 0x3FF) ? 0 : twin(o, 0xE120F000u | (uint32_t)((b >> 10) & 3) << 18 | n);
    if (op == 0x3A) /* hints: NOP.W, YIELD.W, WFE.W, WFI.W, SEV.W, SEVL.W */
        return n == 15 && (b & 0x7FF) <= 5 ? twin(o, 0xE320F000u | (b & 0xFF)) : 0;
    if (op == 0x3B && n == 15 && (b & 0xF00) == 0xF00) /* CLREX (op 2 here, 1 in A32), DSB, DMB, ISB */
        return twin(o, ((b >> 4) & 15) == 2 ? ((b & 15) == 15 ? 0xF57FF01Fu : 0) : 0xF57FF000u | (b & 0xFF));
    if (op == 0x3E && n == 15 && !(b & 0xFF)) return twin(o, 0xE10F0000u | (uint32_t)((b >> 8) & 15) << 12); /* MRS */
    return 0;
}

/* F8xx/F9xx: loads and stores of a byte, halfword or word, and the memory hints. hw1 bit 7 selects imm12 (or, with
   Rn = PC, the literal's U); without it hw2 bit 11 selects imm8 with P, U and W, else a register offset */
static unsigned load_store(unsigned a, unsigned b, SnxThumb* o) {
    unsigned n = a & 15, t = b >> 12, sign = (a >> 8) & 1, size = (a >> 5) & 3, load = (a >> 4) & 1;
    if (size == 3 || (sign && (!load || size == 2))) return 0;
    o->rn = n;
    o->rd = t;
    o->flags = (size == 0 ? TH_B : size == 1 ? TH_H : 0) | (sign ? TH_SIGN : 0);
    if (n == 15) { /* literal */
        if (!load) return 0;
        o->flags |= TH_PCB | TH_PRE | (a & 0x80 ? TH_UP : 0);
        o->imm = b & 0xFFF;
    } else if (a & 0x80) {
        o->flags |= TH_PRE | TH_UP;
        o->imm = b & 0xFFF;
    } else if (b & 0x800) { /* 1 P U W imm8; P U !W (LDRT, STRT) at the user privilege it runs at anyway */
        if (!(b & 0x500)) return 0;
        o->flags |= (b & 0x400 ? TH_PRE : 0) | (b & 0x200 ? TH_UP : 0) | (b & 0x100 ? TH_WB : 0);
        o->imm = b & 0xFF;
        if ((o->flags & TH_WB) && (n == t)) return 0;
    } else if (!(b & 0x7C0)) {
        o->flags |= TH_REG | TH_UP;
        o->rm = b & 15;
        o->imm = (b >> 4) & 3;
        if (o->rm == 13 || o->rm == 15) return 0;
    } else
        return 0;
    if (t == 15 && size < 2) { /* PLD, PLDW, PLI and the unallocated hints: no access */
        if (!load) return 0;
        o->shape = TH_HINT;
        return TH_F32;
    }
    if (!load && t == 15) return 0;
    o->shape = load ? TH_LOAD : TH_STORE;
    return TH_F32;
}

/* FAxx: shifts by a register, extends (with add), and REV, REV16, RBIT, REVSH and CLZ. hw2[15:12] is 1111 */
static unsigned data_register(unsigned a, unsigned b, SnxThumb* o) {
    unsigned op1 = (a >> 4) & 15, op2 = (b >> 4) & 15, n = a & 15, d = (b >> 8) & 15, m = b & 15;
    if ((b & 0xF000) != 0xF000) return 0;
    if (!(op1 & 8) && op2 == 0) { /* LSL/LSR/ASR/ROR.W Rd, Rn, Rm: MOV with a register-shifted operand */
        o->shape = TH_WDATA;
        o->flags = (a & 0x10 ? TH_S : 0) | TH_RSHIFT;
        o->op = 2; /* ORR with Rn=15 => MOV */
        o->rd = d;
        o->rn = 15;
        o->rm = n;
        o->imm = m; /* the shift register */
        o->ra = (a >> 5) & 3;
        return TH_F32;
    }
    if (!(op1 & 8) && (op2 & 0xC) == 8 && op1 != 6 &&
        op1 != 7) { /* SXTAH..UXTAB, Rn 15 without the add: A32's op order */
        static const unsigned op32[6] = {3, 7, 0, 4, 2, 6};
        return twin(o, 0xE6800070u | (uint32_t)op32[op1] << 20 | (uint32_t)n << 16 | (uint32_t)d << 12 |
                           (uint32_t)((b >> 4) & 3) << 10 | m);
    }
    if (n != m || (op2 & 0xC) != 8) return 0;
    switch (op1 << 4 | (op2 & 3)) {
        case 0x90: return twin(o, 0xE6BF0F30u | (uint32_t)d << 12 | m); /* REV */
        case 0x91: return twin(o, 0xE6BF0FB0u | (uint32_t)d << 12 | m); /* REV16 */
        case 0x92: return twin(o, 0xE6FF0F30u | (uint32_t)d << 12 | m); /* RBIT */
        case 0x93: return twin(o, 0xE6FF0FB0u | (uint32_t)d << 12 | m); /* REVSH */
        case 0xB0: return twin(o, 0xE16F0F10u | (uint32_t)d << 12 | m); /* CLZ */
        default: return 0;
    }
}

/* FB8x-FBFx: SMULL, UMULL, SMLAL, UMLAL (hw2 = RdLo RdHi 0000 Rm), SDIV, UDIV (hw2 = 1111 Rd 1111 Rm) */
static unsigned multiply_long(unsigned a, unsigned b, SnxThumb* o) {
    unsigned op1 = (a >> 4) & 7, op2 = (b >> 4) & 15, n = a & 15, lo = b >> 12, hi = (b >> 8) & 15, m = b & 15;
    static const uint32_t longs[8] = {0xE0C00090u, 0, 0xE0800090u, 0, 0xE0E00090u, 0, 0xE0A00090u, 0};
    if (op2 == 0 && longs[op1])
        return twin(o, longs[op1] | (uint32_t)hi << 16 | (uint32_t)lo << 12 | (uint32_t)m << 8 | n);
    if ((op1 == 1 || op1 == 3) && op2 == 15 && lo == 15)
        return twin(o, (op1 == 1 ? 0xE710F010u : 0xE730F010u) | (uint32_t)hi << 16 | (uint32_t)m << 8 | n);
    return 0;
}

static unsigned thumb32(uint16_t a, uint16_t b, SnxThumb* o) {
    o->len = TH_F32;
    o->flags = 0;
    o->op = 0;
    o->ra = 0;
    if ((a & 0xEC00) == 0xEC00 || (a & 0xFF10) == 0xF900) return coprocessor(a, b, o);
    if ((a & 0xFE00) == 0xE800) return multiple_dual(a, b, o);
    if ((a & 0xF800) == 0xF000 && (b & 0x8000)) return branch_control(a, b, o);
    if ((a >> 11) == 0x1E && ((a >> 9) & 1) == 0) { /* data-processing, modified immediate */
        o->shape = TH_WDATA;
        o->op = (a >> 5) & 15;
        o->flags = (a & 0x10) ? TH_S : 0;
        o->rn = a & 15;
        o->rd = (b >> 8) & 15;
        o->imm = thumb_expand_imm((((a >> 10) & 1) << 11) | (((b >> 12) & 7) << 8) | (b & 255));
        o->flags |= TH_MODIMM | ((a & 0x400) || (b & 0x4000) ? TH_ROTC : 0);
        return TH_F32;
    }
    if ((a & 0xFBF0) == 0xF240 || (a & 0xFBF0) == 0xF2C0) { /* MOVW / MOVT */
        o->shape = TH_WIMM;
        o->op = (a & 0x80) ? 1 : 0;
        o->rd = (b >> 8) & 15;
        o->imm = ((a & 15) << 12) | (((a >> 10) & 1) << 11) | (((b >> 12) & 7) << 8) | (b & 255);
        return TH_F32;
    }
    if ((a & 0xFBF0) == 0xF200 || (a & 0xFBF0) == 0xF2A0) { /* ADDW / SUBW */
        o->shape = TH_WIMM;
        o->op = (a & 0x80) ? ALU_SUB : ALU_ADD;
        o->rn = a & 15;
        o->rd = (b >> 8) & 15;
        o->imm = (((a >> 10) & 1) << 11) | (((b >> 12) & 7) << 8) | (b & 255);
        return TH_F32;
    }
    if ((a & 0xFFF0) == 0xF340 || (a & 0xFFF0) == 0xF360 || (a & 0xFFF0) == 0xF3C0) { /* SBFX, BFI/BFC, UBFX */
        unsigned lsb = (((b >> 12) & 7) << 2) | ((b >> 6) & 3);
        unsigned msb = b & 31;
        if (b & 0x20) return 0;
        o->shape = TH_BITFIELD;
        o->rd = (b >> 8) & 15;
        o->rn = a & 15;
        if (!(a & 0x20)) {              /* extract: bit5=0, bit7 selects sign */
            o->op = (a & 0x80) ? 1 : 0; /* UBFX : SBFX */
        } else {                        /* insert: bit5=1, Rn=15 is BFC */
            o->op = (a & 15) == 15 ? 3 : 2;
        }
        o->imm = lsb;
        o->ra = msb;
        return TH_F32;
    }
    if ((a & 0xFE00) == 0xF800) return load_store(a, b, o);
    if ((a >> 8) == 0xEA || (a >> 8) == 0xEB) { /* data-processing, immediate shift */
        o->shape = TH_WDATA;
        o->op = (a >> 5) & 15;
        o->flags = (a & 0x10) ? TH_S : 0;
        o->rn = a & 15;
        o->rd = (b >> 8) & 15;
        o->rm = b & 15;
        o->ra = (b >> 4) & 3;                             /* shift type */
        o->imm = (((b >> 12) & 7) << 2) | ((b >> 6) & 3); /* shift amount */
        return TH_F32;
    }
    if ((a >> 8) == 0xFA) return data_register(a, b, o);
    if ((a & 0xFF80) == 0xFB80) return multiply_long(a, b, o);
    if ((a >> 8) == 0xFB) { /* MUL / MLA / MLS */
        unsigned sub = (b >> 4) & 15;
        if (sub == 0 || sub == 1) {
            o->shape = TH_MUL;
            o->rn = a & 15;
            o->rd = (b >> 8) & 15;
            o->rm = b & 15;
            o->ra = (b >> 12) & 15;
            o->op = sub == 1 ? 2 : ((b >> 12) == 15 ? 0 : 1); /* MLS : MUL : MLA */
            return TH_F32;
        }
        return 0; /* the other multiplies (SMULxy, SMMUL, ...) */
    }
    o->shape = TH_UNDEF;
    o->len = 0;
    return 0;
}

unsigned snx_thumb_decode(uint16_t hw1, uint16_t hw2, SnxThumb* out) {
    out->shape = TH_UNDEF;
    out->len = 0;
    if ((hw1 >> 11) >= 0x1D) return thumb32(hw1, hw2, out);
    return thumb16(hw1, out);
}
