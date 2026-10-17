/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate.c, vec_helper.c, vfp_helper.c, crypto_helper.c),
   LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd.
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org> */
/* Included by llvm.c. Thumb16/Thumb32 emission reusing the A32 primitives
   (readmem/writemem/bounds/arithmetic/shift/nz/condition). PC reads are the
   Thumb pipeline value: instruction address + 4. Literal and ADR bases are
   Align(PC, 4), which their emitters make (tpcalign). The A57 path used to align
   every read, and Thumb PIC code's ADD Rd, PC at an address 2 mod 4 got an
   address 2 bytes off. */

static LLVMValueRef treg(ArmIR* a, unsigned r) {
    return r == 15 ? c32(a, a->pc + 4) : get(a, offsetof(SnxArmCPU, r) + r * 4, a->i32);
}
static LLVMValueRef tpcalign(ArmIR* a) { return c32(a, (a->pc + 4) & ~3u); } /* Align(PC, 4): literals, ADR */
static LLVMValueRef tpcbase(ArmIR* a) { return c32(a, a->pc + 4); } /* unaligned, for branches */

/* rd = x - y (SUB) or x + y (ADD), with the supplied flag behaviour. */
static LLVMValueRef taddsub(ArmIR* a, LLVMValueRef x, LLVMValueRef y, int is_sub, int flags) {
    return is_sub ? arithmetic(a, x, LLVMBuildNot(a->b, y, ""), c32(a, 1), flags)
                  : arithmetic(a, x, y, c32(a, 0), flags);
}

static void thumb_ldst(ArmIR* a, const SnxThumb* t) {
    LLVMValueRef base, offset, addr, adjusted, value;
    unsigned bytes = (t->flags & TH_B) ? 1 : (t->flags & TH_H) ? 2 : 4;
    int is_load = t->shape == TH_LOAD;
    int sign = !!(t->flags & TH_SIGN);
    if (t->flags & TH_REG) {
        offset = treg(a, t->rm);
        if (t->len == TH_F32 && t->imm) offset = shl(a, offset, t->imm); /* wide: shifted offset */
    } else {
        offset = c32(a, t->imm);
    }
    base = t->rn == 15 ? tpcalign(a) : treg(a, t->rn); /* LDR (literal) */
    if (t->flags & TH_SPB) base = treg(a, 13);
    if (t->flags & TH_PCB) base = tpcalign(a);
    adjusted = (t->flags & TH_UP) ? add(a, base, offset) : sub(a, base, offset);
    if (t->flags & TH_REG) addr = adjusted; /* register-offset forms are non-writeback */
    else if (t->flags & TH_PRE) addr = adjusted;
    else addr = base;
    transfer_begin(a, addr, bytes, 1, !is_load, 0); /* Normal memory, unaligned enabled */
    if (is_load) {
        value = readmem(a, addr, bytes, sign);
        if (t->rd == 15) branch_exchange(a, value);
        else setreg(a, t->rd, value);
    } else {
        writemem(a, addr, bytes, treg(a, t->rd));
        transfer_commit(a);
    }
    if (t->flags & TH_WB) setreg(a, t->rn, adjusted);
}

/* LDRD/STRD: any two registers, unlike A32's even pair, and a word-aligned offset up to 1020. Both words are read
   before either register is written, and both checked before either is stored */
static void thumb_dual(ArmIR* a, const SnxThumb* t) {
    LLVMValueRef base = t->rn == 15 ? tpcalign(a) : treg(a, t->rn), adjusted, addr, first, second;
    adjusted = (t->flags & TH_UP) ? add(a, base, c32(a, t->imm)) : sub(a, base, c32(a, t->imm));
    addr = (t->flags & TH_PRE) ? adjusted : base;
    transfer_begin(a, addr, 8, 4, !t->op, 1);
    if (t->op) {
        first = readmem(a, addr, 4, 0);
        second = readmem(a, add(a, addr, c32(a, 4)), 4, 0);
        setreg(a, t->rd, first);
        setreg(a, t->rm, second);
    } else {
        writemem(a, addr, 4, treg(a, t->rd));
        writemem(a, add(a, addr, c32(a, 4)), 4, treg(a, t->rm));
        transfer_commit(a);
    }
    if (t->flags & TH_WB) setreg(a, t->rn, adjusted);
}

static void thumb_alu(ArmIR* a, const SnxThumb* t, int hi) {
    LLVMValueRef x, y, r;
    int flags = hi ? t->op == 1 /* CMP hi sets flags */ : 1;
    x = hi ? treg(a, t->rn) : treg(a, t->rd); /* most 16-bit ALU ops read rd as first source */
    y = treg(a, t->rm);
    if (t->op == ALU_ADD || t->op == ALU_SUB) {
        r = taddsub(a, x, y, t->op == ALU_SUB, flags);
        setreg(a, t->rd, r);
        return;
    }
    switch (t->op) {
        case ALU_AND: r = band(a, x, y); setreg(a, t->rd, r); if (flags) nz(a, r, NULL, NULL); break;
        case ALU_EOR: r = bxor(a, x, y); setreg(a, t->rd, r); if (flags) nz(a, r, NULL, NULL); break;
        case ALU_ORR: r = bor(a, x, y); setreg(a, t->rd, r); if (flags) nz(a, r, NULL, NULL); break;
        case ALU_BIC: r = band(a, x, LLVMBuildNot(a->b, y, "")); setreg(a, t->rd, r); if (flags) nz(a, r, NULL, NULL); break;
        case ALU_ORN: r = bor(a, x, LLVMBuildNot(a->b, y, "")); setreg(a, t->rd, r); if (flags) nz(a, r, NULL, NULL); break;
        case ALU_MVN: r = LLVMBuildNot(a->b, y, ""); setreg(a, t->rd, r); if (flags) nz(a, r, NULL, NULL); break;
        case ALU_TST: r = band(a, x, y); nz(a, r, NULL, NULL); break;
        case ALU_NEG: r = taddsub(a, c32(a, 0), y, 1, flags); setreg(a, t->rd, r); break;
        case ALU_CMP: taddsub(a, x, y, 1, flags); break;
        case ALU_CMN: taddsub(a, x, y, 0, flags); break;
        case ALU_MUL: r = LLVMBuildMul(a->b, x, y, ""); setreg(a, t->rd, r); if (flags) nz(a, r, NULL, NULL); break;
        case ALU_LSL: case ALU_LSR: case ALU_ASR: case ALU_ROR: {
            LLVMValueRef carry;
            unsigned type = t->op == ALU_LSL ? 0 : t->op == ALU_LSR ? 1 : t->op == ALU_ASR ? 2 : 3;
            r = shift(a, x, type, y, 0, 0, &carry); /* register amount */
            setreg(a, t->rd, r);
            nz(a, r, carry, NULL);
            break;
        }
        case ALU_ADC: r = arithmetic(a, x, y, flag(a, 29), flags); setreg(a, t->rd, r); break;
        case ALU_SBC: r = arithmetic(a, x, LLVMBuildNot(a->b, y, ""), flag(a, 29), flags); setreg(a, t->rd, r); break;
        case ALU_RSB: r = taddsub(a, y, x, 1, flags); setreg(a, t->rd, r); break;
        default: trap(a, SNX_ARM_UNDEFINED, c32(a, a->word)); return;
    }
    (void)flags;
}

static void thumb_imm8(ArmIR* a, const SnxThumb* t) {
    LLVMValueRef imm = c32(a, t->imm), r;
    switch (t->op) {
        case I8_MOV: r = imm; setreg(a, t->rd, r); nz(a, r, NULL, NULL); break;
        case I8_CMP: taddsub(a, treg(a, t->rd), imm, 1, 1); break;
        case I8_ADD: r = taddsub(a, treg(a, t->rd), imm, 0, 1); setreg(a, t->rd, r); break;
        case I8_SUB: r = taddsub(a, treg(a, t->rd), imm, 1, 1); setreg(a, t->rd, r); break;
        default: trap(a, SNX_ARM_UNDEFINED, c32(a, a->word)); break;
    }
}

static void thumb_extend(ArmIR* a, const SnxThumb* t) {
    LLVMValueRef x = treg(a, t->rm), r;
    /* op bit0: 0=half 1=byte; bit1: 0=signed 1=unsigned */
    LLVMTypeRef ty = (t->op & 1) ? a->i8 : a->i16;
    LLVMValueRef narrow = LLVMBuildTrunc(a->b, x, ty, "");
    r = (t->op & 2) ? (LLVMValueRef)LLVMBuildZExt(a->b, narrow, a->i32, "")
                    : (LLVMValueRef)LLVMBuildSExt(a->b, narrow, a->i32, "");
    setreg(a, t->rd, r);
}

static void thumb_pushpop(ArmIR* a, const SnxThumb* t) {
    unsigned list = t->imm, count = 0, i;
    for (i = 0; i < 8; ++i) count += (list >> i) & 1;
    count += t->ra;
    if (!count && a->memory == SNX_ARM_MEMORY_CHECKED) { trap(a, SNX_ARM_UNDEFINED, c32(a, a->word)); return; }
    LLVMValueRef sp = treg(a, 13);
    if (t->op == 0) { /* PUSH */
        LLVMValueRef start = sub(a, sp, c32(a, count * 4));
        transfer_begin(a, start, count * 4, 4, 1, 1);
        unsigned index = 0;
        for (i = 0; i < 8; ++i) if (list & (1u << i)) writemem(a, add(a, start, c32(a, index++ * 4)), 4, treg(a, i));
        if (t->ra) writemem(a, add(a, start, c32(a, index * 4)), 4, treg(a, 14));
        transfer_commit(a);
        setreg(a, 13, start);
    } else { /* POP */
        LLVMValueRef start = sp, value;
        transfer_begin(a, start, count * 4, 4, 0, 1);
        unsigned index = 0;
        for (i = 0; i < 8; ++i) if (list & (1u << i)) setreg(a, i, readmem(a, add(a, start, c32(a, index++ * 4)), 4, 0));
        if (t->ra) {
            value = readmem(a, add(a, start, c32(a, index * 4)), 4, 0);
            branch_exchange(a, value);
        }
        setreg(a, 13, add(a, start, c32(a, count * 4)));
    }
}

static void thumb_ldmstm(ArmIR* a, const SnxThumb* t) {
    unsigned list = t->imm, count = 0, i;
    for (i = 0; i < 8; ++i) count += (list >> i) & 1;
    if (!count) { trap(a, SNX_ARM_UNDEFINED, c32(a, a->word)); return; }
    LLVMValueRef base = treg(a, t->rn), values[8];
    transfer_begin(a, base, count * 4, 4, !t->op, 1);
    unsigned index = 0;
    if (t->op == 1) { /* LDMIA: snapshot before writeback */
        for (i = 0; i < 8; ++i) if (list & (1u << i)) values[i] = readmem(a, add(a, base, c32(a, index++ * 4)), 4, 0);
        for (i = 0; i < 8; ++i) if (list & (1u << i)) if (i != t->rn || !(t->flags & TH_WB)) setreg(a, i, values[i]);
    } else { /* STMIA */
        for (i = 0; i < 8; ++i) if (list & (1u << i)) writemem(a, add(a, base, c32(a, index++ * 4)), 4, treg(a, i));
        transfer_commit(a);
    }
    if (t->flags & TH_WB) setreg(a, t->rn, add(a, base, c32(a, count * 4)));
}

static void thumb_bitfield(ArmIR* a, const SnxThumb* t) {
    unsigned lsb = t->imm;
    LLVMValueRef x = treg(a, t->rn), r, mask;
    if (t->op == 0 || t->op == 1) { /* SBFX / UBFX: .ra is width-1 */
        unsigned width = t->ra + 1;
        mask = c32(a, width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1));
        r = band(a, lsr(a, x, lsb), mask);
        if (t->op == 0 && width < 32) r = LLVMBuildAShr(a->b, shl(a, r, 32 - width), c32(a, 32 - width), "");
        setreg(a, t->rd, r);
    } else { /* BFI / BFC: .ra is the msb */
        unsigned width = t->ra - lsb + 1;
        uint32_t m = (width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1)) << lsb;
        LLVMValueRef dst = treg(a, t->rd);
        LLVMValueRef src = t->op == 3 ? c32(a, 0) : treg(a, t->rn); /* BFC inserts zero */
        r = bor(a, band(a, dst, c32(a, ~m)), band(a, shl(a, src, lsb), c32(a, m)));
        setreg(a, t->rd, r);
    }
}

static void thumb_wimm(ArmIR* a, const SnxThumb* t) {
    LLVMValueRef r;
    if (t->op == 0) { r = c32(a, t->imm); setreg(a, t->rd, r); } /* MOVW */
    else if (t->op == 1) { /* MOVT */
        r = bor(a, band(a, treg(a, t->rd), c32(a, 0xFFFF)), c32(a, t->imm << 16));
        setreg(a, t->rd, r);
    } else { /* ADDW / SUBW */
        r = taddsub(a, t->rn == 15 ? tpcalign(a) : treg(a, t->rn), c32(a, t->imm), t->op == ALU_SUB, 0); /* ADR.W */
        setreg(a, t->rd, r);
    }
}

static void thumb_wdata(ArmIR* a, const SnxThumb* t) {
    LLVMValueRef x = treg(a, t->rn), rmv = treg(a, t->rm), y, carry = NULL, r = NULL;
    int flags = !!(t->flags & TH_S);
    int compare = (t->rd == 15 && flags);
    /* the shifter's carry, which the logical operations set C to: ThumbExpandImm_C's for a rotated immediate, an
       unrotated one leaving C as it is */
    if (t->flags & TH_MODIMM) {
        y = c32(a, t->imm);
        if (t->flags & TH_ROTC) carry = c32(a, t->imm >> 31);
    } else if (t->flags & TH_RSHIFT) y = shift(a, rmv, t->ra, treg(a, t->imm), 0, 0, &carry);
    else y = shift(a, rmv, t->ra, NULL, 1, t->imm, &carry);
    switch (t->op) {
        case 0: r = band(a, x, y); if (flags) nz(a, r, carry, NULL); if (!compare) setreg(a, t->rd, r); break; /* AND/TST */
        case 1: r = band(a, x, LLVMBuildNot(a->b, y, "")); if (flags) nz(a, r, carry, NULL); setreg(a, t->rd, r); break; /* BIC */
        case 2: r = t->rn == 15 ? y : bor(a, x, y); if (flags) nz(a, r, carry, NULL); setreg(a, t->rd, r); break; /* ORR/MOV */
        case 3: r = t->rn == 15 ? LLVMBuildNot(a->b, y, "") : bor(a, x, LLVMBuildNot(a->b, y, ""));
                if (flags) nz(a, r, carry, NULL); setreg(a, t->rd, r); break; /* ORN/MVN */
        case 4: r = bxor(a, x, y); if (flags) nz(a, r, carry, NULL); if (!compare) setreg(a, t->rd, r); break; /* EOR/TEQ */
        case 8: /* ADD/CMN */
            if (compare) taddsub(a, x, y, 0, 1);
            else { r = taddsub(a, x, y, 0, flags); setreg(a, t->rd, r); }
            break;
        case 10: /* ADC/CMP */
            if (compare) taddsub(a, x, y, 1, 1);
            else { r = arithmetic(a, x, y, flag(a, 29), flags); setreg(a, t->rd, r); }
            break;
        case 11: /* SBC/CMN */
            if (compare) taddsub(a, x, y, 0, 1);
            else { r = arithmetic(a, x, LLVMBuildNot(a->b, y, ""), flag(a, 29), flags); setreg(a, t->rd, r); }
            break;
        case 13: /* SUB/CMP */
            if (compare) taddsub(a, x, y, 1, 1);
            else { r = taddsub(a, x, y, 1, flags); setreg(a, t->rd, r); }
            break;
        case 14: r = taddsub(a, y, x, 1, flags); setreg(a, t->rd, r); break; /* RSB */
        default: trap(a, SNX_ARM_UNDEFINED, c32(a, a->word)); break;
    }
}

static void thumb_mul(ArmIR* a, const SnxThumb* t) {
    LLVMValueRef x = treg(a, t->rn), y = treg(a, t->rm), r;
    r = LLVMBuildMul(a->b, x, y, "");
    if (t->op == 0) setreg(a, t->rd, r); /* MUL */
    else if (t->op == 1) setreg(a, t->rd, add(a, r, treg(a, t->ra))); /* MLA */
    else setreg(a, t->rd, sub(a, treg(a, t->ra), r)); /* MLS */
}

static void thumb_table(ArmIR* a, const SnxThumb* t) {
    LLVMValueRef base = add(a, treg(a, t->rn), treg(a, t->rm));
    LLVMValueRef index = t->op ? readmem(a, base, 2, 0) : readmem(a, base, 1, 0);
    LLVMValueRef target = add(a, shl(a, index, 1), tpcbase(a));
    setreg(a, 15, target);
}

/* Offset decode for BL/BLX/B.W from the raw halfwords (o->rn=hw1, o->rm=hw2). */
static uint32_t thumb_branch_offset(const SnxThumb* t, uint32_t pc, unsigned* thumb_state) {
    unsigned hw1 = t->rn, hw2 = t->rm;
    unsigned S = (hw1 >> 10) & 1, J1 = (hw2 >> 13) & 1, J2 = (hw2 >> 11) & 1;
    unsigned I1 = !(J1 ^ S), I2 = !(J2 ^ S);
    *thumb_state = 1;
    if (t->op == BR_BLX_IMM) {
        unsigned imm10 = hw1 & 0x3FF, imm10L = (hw2 >> 1) & 0x3FF;
        uint32_t imm24 = (S << 23) | (I1 << 22) | (I2 << 21) | (imm10 << 11) | (imm10L << 1);
        if (imm24 & 0x800000) imm24 |= 0xFF000000u;
        *thumb_state = 0;
        return ((pc + 4) & ~3u) + (imm24 << 1);
    }
    {
        unsigned imm10 = hw1 & 0x3FF, imm11 = hw2 & 0x7FF;
        uint32_t imm24 = (S << 23) | (I1 << 22) | (I2 << 21) | (imm10 << 11) | imm11;
        if (imm24 & 0x800000) imm24 |= 0xFF000000u;
        return (pc + 4) + (imm24 << 1);
    }
}

/* Returns 1 when the instruction ends the block (control transfer). */
static int thumb_instruction(ArmIR* a, const SnxThumb* t) {
    if (a->memory == SNX_ARM_MEMORY_CHECKED && t->shape == TH_TABLE) {
        trap(a, SNX_ARM_BACKEND_UNSUPPORTED, c32(a, a->word));
        return 1;
    }
    switch (t->shape) {
        case TH_LEGACY_BL: {
            if (!t->op) {
                uint32_t off = t->imm << 12;
                if (t->imm & 0x400) off |= 0xff800000u;
                setreg(a, 14, add(a, tpcbase(a), c32(a, off)));
                return 0;
            }
            LLVMValueRef target = add(a, treg(a, 14), c32(a, t->imm << 1));
            setreg(a, 14, c32(a, (a->pc + 2) | 1u));
            if (t->op == 2) PUT(a, cpsr, band(a, GET(a, cpsr), c32(a, ~SNX_ARM_T)));
            setreg(a, 15, band(a, target, c32(a, t->op == 2 ? ~3u : ~1u)));
            return 1;
        }
        case TH_REV: {
            LLVMValueRef x = treg(a, t->rm), r;
            if (!t->op) r = intrinsic(a, "llvm.bswap", 10, a->i32, &x, 1);
            else {
                r = bor(a, band(a, lsr(a, x, 8), c32(a, 0x00ff00ff)), shl(a, band(a, x, c32(a, 0x00ff00ff)), 8));
                if (t->op == 3) r = LLVMBuildSExt(a->b, LLVMBuildTrunc(a->b, r, a->i16, ""), a->i32, "");
            }
            setreg(a, t->rd, r);
            return 0;
        }
        case TH_SHIFT_IMM: {
            LLVMValueRef carry, r = shift(a, treg(a, t->rm), t->op == ALU_LSL ? 0 : t->op == ALU_LSR ? 1 : 2,
                                          NULL, 1, t->imm, &carry);
            setreg(a, t->rd, r);
            nz(a, r, carry, NULL);
            return 0;
        }
        case TH_ADDSUB: {
            LLVMValueRef y = (t->flags & TH_REG) ? treg(a, t->rm) : c32(a, t->imm);
            setreg(a, t->rd, taddsub(a, treg(a, t->rn), y, t->op == ALU_SUB, 1));
            return 0;
        }
        case TH_IMM8: thumb_imm8(a, t); return 0;
        case TH_ALU: thumb_alu(a, t, 0); return 0;
        case TH_HI:
            if (t->op == 3) { /* BX / BLX register */
                LLVMValueRef target = treg(a, t->rm);
                branch_exchange(a, target);
                if (t->op == BR_BLX_REG) setreg(a, 14, bor(a, tpcbase(a), c32(a, 1)));
                return 1;
            }
            if (t->op == 0) {
                LLVMValueRef value = add(a, treg(a, t->rn), treg(a, t->rm));
                if (t->rd == 15) { /* ALUWritePC: a branch, in Thumb state */
                    setreg(a, 15, band(a, value, c32(a, ~1u)));
                    return 1;
                }
                setreg(a, t->rd, value);
            } else if (t->op == 1) taddsub(a, treg(a, t->rn), treg(a, t->rm), 1, 1);
            else {
                if (t->rd == 15) { setreg(a, 15, band(a, treg(a, t->rm), c32(a, ~1u))); return 1; }
                setreg(a, t->rd, treg(a, t->rm));
            }
            return 0;
        case TH_LOAD: case TH_STORE:
            thumb_ldst(a, t);
            return t->shape == TH_LOAD && t->rd == 15; /* LoadWritePC: a branch, which ends the block */
        case TH_ADDR:
            if (t->rd == 13 && t->rn == 13) setreg(a, 13, taddsub(a, treg(a, 13), c32(a, t->imm), t->op == ALU_SUB, 0));
            else setreg(a, t->rd, add(a, t->rn == 15 ? c32(a, (a->pc + 4) & ~3u) : treg(a, t->rn), c32(a, t->imm)));
            return 0;
        case TH_EXTEND: thumb_extend(a, t); return 0;
        case TH_PUSHPOP: thumb_pushpop(a, t); return t->op == 1 && t->ra;
        case TH_LDMSTM: thumb_ldmstm(a, t); return 0;
        case TH_MUL: thumb_mul(a, t); return 0;
        case TH_BITFIELD: thumb_bitfield(a, t); return 0;
        case TH_WIMM: thumb_wimm(a, t); return 0;
        case TH_WDATA: thumb_wdata(a, t); return 0;
        case TH_TABLE: thumb_table(a, t); return 1;
        case TH_IT: case TH_HINT: return 0;
        case TH_A32: { /* the A32 emitter runs the twin, reading Thumb's PC (ArmIR.thumb) */
            uint32_t word = a->word;
            int stop;
            a->word = t->imm;
            a->pair = t->ra;
            stop = instruction(a, (SnxA32Kind)t->op);
            a->word = word;
            a->pair = 0;
            return stop;
        }
        case TH_DUAL: thumb_dual(a, t); return 0;
        case TH_BRANCH: {
            unsigned state;
            LLVMValueRef target;
            switch (t->op) {
                case BR_SVC: case BR_BKPT:
                    setreg(a, 15, c32(a, a->pc + 2));
                    trap(a, t->op == BR_SVC ? SNX_ARM_SVC : SNX_ARM_BREAKPOINT, c32(a, t->imm));
                    return 1;
                case BR_BX: case BR_BLX_REG:
                    target = treg(a, t->rm);
                    branch_exchange(a, target);
                    if (t->op == BR_BLX_REG) setreg(a, 14, bor(a, c32(a, a->pc + 2), c32(a, 1)));
                    return 1;
                case BR_B: case BR_BCOND: case BR_CBZ: case BR_CBNZ: case BR_BCOND_W: {
                    uint32_t off = t->imm;
                    LLVMValueRef cond = NULL;
                    if (t->op == BR_B) { off <<= 1; if (off & 0x800) off |= 0xFFFFF000u; }
                    if (t->op == BR_BCOND) { off <<= 1; if (off & 0x100) off |= 0xFFFFFE00u; }
                    if (t->op == BR_BCOND_W) { off <<= 1; if (off & 0x100000) off |= 0xFFE00000u; }
                    target = add(a, tpcbase(a), c32(a, off));
                    if (t->op == BR_BCOND || t->op == BR_BCOND_W) cond = condition(a, t->ra);
                    else if (t->op == BR_CBZ) cond = cmp(a, LLVMIntEQ, treg(a, t->rn), c32(a, 0));
                    else if (t->op == BR_CBNZ) cond = cmp(a, LLVMIntNE, treg(a, t->rn), c32(a, 0));
                    if (cond) {
                        LLVMBasicBlockRef taken = block(a, "tb"), nt = block(a, "nt"), j = block(a, "tj");
                        LLVMBuildCondBr(a->b, cond, taken, nt);
                        LLVMPositionBuilderAtEnd(a->b, taken);
                        setreg(a, 15, target);
                        LLVMBuildBr(a->b, j);
                        LLVMPositionBuilderAtEnd(a->b, nt);
                        setreg(a, 15, c32(a, a->next)); /* B<c>.W is 4 bytes */
                        LLVMBuildBr(a->b, j);
                        LLVMPositionBuilderAtEnd(a->b, j);
                    } else {
                        setreg(a, 15, target); /* B */
                    }
                    return 1;
                }
                case BR_BL: case BR_BLX_IMM: case BR_BW:
                    target = c32(a, thumb_branch_offset(t, a->pc, &state));
                    if (t->op != BR_BW) setreg(a, 14, bor(a, tpcbase(a), c32(a, 1)));
                    if (!state) PUT(a, cpsr, band(a, GET(a, cpsr), c32(a, ~SNX_ARM_T)));
                    setreg(a, 15, target);
                    return 1;
                default: trap(a, SNX_ARM_UNDEFINED, c32(a, a->word)); return 1;
            }
        }
        default: trap(a, SNX_ARM_UNDEFINED, c32(a, a->word)); return 1;
    }
}

static unsigned thumb_ctz(unsigned v) {
    unsigned n = 0;
    while (!(v & 1)) {
        v >>= 1;
        ++n;
    }
    return n;
}

/* Emits one Thumb block: 16-bit and 32-bit instructions from the halfword
   stream, honouring IT conditions, ending at a control transfer or the end. */
LLVMValueRef snx_thumb_llvm_block(LLVMModuleRef module, const char* name, const uint16_t* halfwords,
                                  size_t count, uint32_t pc, size_t* consumed) {
    return snx_thumb_llvm_block_ex(module, name, halfwords, count, pc, consumed, NULL);
}
LLVMValueRef snx_thumb_llvm_block_ex(LLVMModuleRef module, const char* name, const uint16_t* halfwords,
                                    size_t count, uint32_t pc, size_t* consumed, const SnxArmCodegenOptions* options) {
    ArmIR a = {0};
    LLVMTypeRef ft;
    size_t used = 0;
    unsigned it_len = 0, it_idx = 0, it_conds[4];
    if (consumed) *consumed = 0;
    if (!options_valid(options) || !module || !name || !*name || !halfwords || !count || count > 65536 || (pc & 1) ||
        LLVMGetNamedFunction(module, name) || LLVMGetNamedGlobal(module, name))
        return NULL;
    if (options) { a.profile = options->profile; a.memory = options->memory; }
    a.thumb = 1;
    a.ctx = LLVMGetModuleContext(module);
    a.mod = module;
    a.b = LLVMCreateBuilderInContext(a.ctx);
    a.i1 = LLVMInt1TypeInContext(a.ctx);
    a.i8 = LLVMInt8TypeInContext(a.ctx);
    a.i16 = LLVMInt16TypeInContext(a.ctx);
    a.i32 = LLVMInt32TypeInContext(a.ctx);
    a.i64 = LLVMInt64TypeInContext(a.ctx);
    a.ptr = LLVMPointerTypeInContext(a.ctx, 0);
    ft = LLVMFunctionType(LLVMVoidTypeInContext(a.ctx), &a.ptr, 1, 0);
    a.fn = LLVMAddFunction(module, name, ft);
    a.cpu = LLVMGetParam(a.fn, 0);
    LLVMPositionBuilderAtEnd(a.b, block(&a, "entry"));
    PUT(&a, exit, c32(&a, SNX_ARM_OK));
    PUT(&a, syndrome, c32(&a, 0));
    require(&a, cmp(&a, LLVMIntEQ, get(&a, offsetof(SnxArmCPU, r) + 60, a.i32), c32(&a, pc)), SNX_ARM_BAD_ENTRY,
            c32(&a, pc));
    require(&a, cmp(&a, LLVMIntNE, band(&a, GET(&a, cpsr), c32(&a, SNX_ARM_T)), c32(&a, 0)), SNX_ARM_BAD_ENTRY,
            c32(&a, halfwords[0]));
    require(&a, cmp(&a, LLVMIntEQ, band(&a, GET(&a, cpsr), c32(&a, SNX_ARM_E)), c32(&a, 0)), SNX_ARM_ENDIAN_UNSUPPORTED,
            c32(&a, halfwords[0]));
    memory_entry(&a);
    while (used < count) {
        SnxThumb t;
        uint16_t hw1 = halfwords[used], hw2 = (used + 1 < count) ? halfwords[used + 1] : 0;
        unsigned len = snx_thumb_decode_profile(a.profile, hw1, hw2, &t);
        uint32_t next_pc;
        unsigned cond = 14;
        int stop;
        if (!len || (len == TH_F32 && used + 1 >= count)) {
            /* the backend's (SnxArmRt.undefined): a Thumb32 one as hw1 | hw2 << 16, when its second half is here */
            unsigned units = (hw1 >> 11) >= 0x1D && used + 1 < count ? 2 : 1;
            a.pc = pc + (uint32_t)used * 2;
            a.next = a.pc + units * 2;
            a.word = units == 2 ? (uint32_t)hw1 | ((uint32_t)hw2 << 16) : hw1;
            setreg(&a, 15, c32(&a, a.pc));
            instruction_fetch(&a, a.word, units * 2);
            trap(&a, SNX_ARM_UNDEFINED, c32(&a, a.word));
            used += units;
            break;
        }
        a.pc = pc + (uint32_t)used * 2;
        a.word = hw1;
        next_pc = a.pc + len * 2;
        a.next = next_pc;
        setreg(&a, 15, c32(&a, a.pc));
        instruction_fetch(&a, len == TH_F32 ? (uint32_t)hw1 | ((uint32_t)hw2 << 16) : hw1, len * 2);
        if (it_idx < it_len) cond = it_conds[it_idx++];
        if (cond != 14) {
            LLVMBasicBlockRef execute = block(&a, "execute"), skip = block(&a, "skip"), next = block(&a, "next");
            LLVMBuildCondBr(a.b, condition(&a, cond), execute, skip);
            LLVMPositionBuilderAtEnd(a.b, execute);
            stop = thumb_instruction(&a, &t);
            if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(a.b))) stop = 1; /* trapped */
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(a.b))) {
                if (!stop) setreg(&a, 15, c32(&a, next_pc));
                LLVMBuildBr(a.b, next);
            }
            LLVMPositionBuilderAtEnd(a.b, skip);
            setreg(&a, 15, c32(&a, next_pc));
            LLVMBuildBr(a.b, next);
            LLVMPositionBuilderAtEnd(a.b, next);
        } else {
            stop = thumb_instruction(&a, &t);
            if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(a.b))) stop = 1; /* trapped */
            if (!stop) setreg(&a, 15, c32(&a, next_pc));
        }
        used += len;
        if (t.shape == TH_IT) {
            unsigned mask = t.imm & 15, firstcond = (t.imm >> 4) & 15, i;
            it_len = 4 - thumb_ctz(mask);
            it_idx = 0;
            for (i = 0; i < it_len; ++i)
                it_conds[i] = i == 0 ? firstcond : ((firstcond & 0xE) | ((mask >> (4 - i)) & 1));
        }
        if (stop) break;
    }
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(a.b))) LLVMBuildRetVoid(a.b);
    LLVMDisposeBuilder(a.b);
    if (consumed) *consumed = used;
    return a.fn;
}
