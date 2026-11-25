/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Included by mach_arm.inc. Thumb blocks (what cpu/arm/llvm_thumb.inc's thumb_block makes, for any profile and memory
   mode): the 16-bit instructions are sailgen's emitters of the AArch32 Sail (sg_decode_t16), their condition the IT
   block's while one is open, as the Sail's CurrentCond has it (before Thumb-2, on the ARM11, the ARM946E-S and the
   ARM7TDMI, every halfword is one, BL's two halves among them); the 32-bit ones
   the Thumb decoder's shapes (thumb.c), made here over the same SSA, and those that are an A32 instruction in another
   encoding its emitter's (TH_A32: an A32 word whose PC reads Thumb's). ITSTATE is known as the block is made: it
   advances with each instruction, and an SVC, a BKPT or a block that ends inside one leaves it in CPSR */

/* R[r] as a Thumb operand: the PC reads as the instruction's address plus 4, unaligned (Align(PC, 4) of literals and ADR
   is tpcalign) */
static SgD treg(SgCtx* g, unsigned r) { return r == 15 ? sgx_const(32, (SnxU32)g->pc + 4) : sg_GPR_read(g, r); }
static SgD tpcalign(SgCtx* g) { return sgx_const(32, ((SnxU32)g->pc + 4) & ~3u); }
/* rd = v, but R15, which an instruction that doesn't branch leaves as the next one's address */
static void tset(SgCtx* g, unsigned r, SgD v) {
    if (r != 15) sg_GPR_write(g, r, v);
}

/* x + y, or x - y, with their NZCV where flags (llvm_thumb.inc's taddsub) */
static SgD taddsub(SgCtx* g, SgD x, SgD y, int sub, int flags) {
    if (!flags) return sub ? sg_d_sub(g, x, y) : sg_d_add(g, x, y);
    SgD r, nzcv;
    sg_add_with_carry(g, x, sub ? sg_d_not(g, y) : y, sgx_const(1, (SnxU64)sub), &r, &nzcv);
    sg_PSTATE_write(g, SG_PSTATE_N, 4, nzcv);
    return r;
}
/* x + y + C, or x + NOT(y) + C, with their NZCV where flags (llvm.c's arithmetic of flag 29) */
static SgD tadc(SgCtx* g, SgD x, SgD y, int sub, int flags) {
    SgD c = sg_PSTATE_read(g, SG_PSTATE_C), r, nzcv;
    if (!flags) return sg_d_add(g, sg_d_add(g, x, sub ? sg_d_not(g, y) : y), sg_d_zext(g, c, 32));
    sg_add_with_carry(g, x, sub ? sg_d_not(g, y) : y, c, &r, &nzcv);
    sg_PSTATE_write(g, SG_PSTATE_N, 4, nzcv);
    return r;
}
/* the flags of a logical result: N and Z, and C the shifter's carry where it has one (NULL: C as it was) */
static void tlogic_flags(SgCtx* g, SgD r, const SgD* carry) {
    SgD c = carry ? *carry : sgx_const(1, 0);
    nz(g, r, 32);
    if (carry) sg_PSTATE_write(g, SG_PSTATE_C, 1, c);
}
/* v shifted by an immediate (llvm.c's shift with immediate set): its carry in *carry where it sets one, 0 where C stays
   as it was (LSL #0) */
static SgD tshift_imm(SgCtx* g, SgD v, unsigned type, unsigned imm, SgD* carry, int* has_carry) {
    SgD r;
    *has_carry = 1;
    if (!imm && type == 0) {
        *has_carry = 0;
        return v;
    }
    if (!imm && type == 3) { /* RRX */
        *carry = sg_d_slice(g, v, 0, 0);
        return sg_d_or(g, sg_d_lsl(g, sg_d_zext(g, sg_PSTATE_read(g, SG_PSTATE_C), 32), 31), sg_d_lsr(g, v, 1));
    }
    sg_d_shift_c(g, v, imm ? imm : 32, (int)type, &r, carry);
    return r;
}
/* v shifted by a register's low byte (llvm.c's shift): an amount of 0 leaves v and C */
static SgD tshift_reg(SgCtx* g, SgD v, unsigned type, SgD amount, SgD* carry) {
    SgD c = sg_PSTATE_read(g, SG_PSTATE_C), r;
    SgI n = sg_i_uint(g, sg_d_slice(g, amount, 7, 0));
    SgB zero = sg_i_cmp(g, 0, n, sg_i_const(g, 0));
    sg_d_shift_c_dyn(g, v, n, (int)type, &r, carry);
    *carry = sg_d_select(g, zero, c, *carry);
    return r; /* an amount of 0 shifts by none: the value is v */
}

/* LDR, STR and their byte, halfword and signed forms, of any offset and writeback (llvm_thumb.inc's thumb_ldst): 1 where
   it loads the PC */
static int tldst(SgCtx* g, const SnxThumb* t) {
    unsigned bytes = t->flags & TH_B ? 1 : t->flags & TH_H ? 2 : 4;
    int load = t->shape == TH_LOAD;
    SgD offset, base = t->rn == 15 ? tpcalign(g) : treg(g, t->rn), addr;
    if (t->flags & TH_REG) {
        offset = treg(g, t->rm);
        if (t->len == TH_F32 && t->imm) offset = sg_d_lsl(g, offset, t->imm);
    } else
        offset = sgx_const(32, t->imm);
    if (t->flags & TH_SPB) base = treg(g, 13);
    if (t->flags & TH_PCB) base = tpcalign(g);
    SgD adjusted = t->flags & TH_UP ? sg_d_add(g, base, offset) : sg_d_sub(g, base, offset);
    addr = t->flags & (TH_REG | TH_PRE) ? adjusted : base;
    sgx_a32_commit(g);
    sgx_reg(sgx(g), &addr);
    sgx_a32_transfer(g, addr, bytes, 1, !load, 0);
    if (load) {
        SgD value = sgx_a32_read(g, addr, (int)bytes, 0, !!(t->flags & TH_SIGN));
        if (t->rd == 15)
            write_pc(g, value);
        else
            tset(g, t->rd, value);
    } else {
        sgx_a32_write(g, addr, (int)bytes, 0, treg(g, t->rd), 0);
        sgx_a32_written(g, addr, bytes);
    }
    if (t->flags & TH_WB) tset(g, t->rn, adjusted);
    return load && t->rd == 15;
}

/* LDRD and STRD (llvm_thumb.inc's thumb_dual): word aligned, both words read or checked before either is written */
static void tdual(SgCtx* g, const SnxThumb* t) {
    SgD base = t->rn == 15 ? tpcalign(g) : treg(g, t->rn);
    SgD adjusted =
        t->flags & TH_UP ? sg_d_add(g, base, sgx_const(32, t->imm)) : sg_d_sub(g, base, sgx_const(32, t->imm));
    SgD addr = t->flags & TH_PRE ? adjusted : base;
    sgx_a32_commit(g);
    sgx_reg(sgx(g), &addr);
    sgx_a32_transfer(g, addr, 8, 4, !t->op, 1);
    if (t->op) {
        SgD first = sgx_a32_read(g, addr, 4, 0, 0), second = sgx_a32_read(g, addr, 4, 4, 0);
        tset(g, t->rd, first);
        tset(g, t->rm, second);
    } else {
        sgx_a32_write(g, addr, 4, 0, treg(g, t->rd), 0);
        sgx_a32_write(g, addr, 4, 4, treg(g, t->rm), 0);
        sgx_a32_written(g, addr, 8);
    }
    if (t->flags & TH_WB) tset(g, t->rn, adjusted);
}

/* the 32-bit data processing (llvm_thumb.inc's thumb_wdata): op AND, BIC, ORR, ORN, EOR, ADD, ADC, SBC, SUB, RSB, Rn 15
   MOV, MVN and their kin, Rd 15 with the flags the compares */
static void twdata(SgCtx* g, const SnxThumb* t) {
    SgD x = treg(g, t->rn), m = treg(g, t->rm), y, carry = sgx_const(1, 0);
    int flags = !!(t->flags & TH_S), compare = t->rd == 15 && flags, has_carry = 0;
    SgD r;
    if (t->flags & TH_MODIMM) {
        y = sgx_const(32, t->imm);
        if (t->flags & TH_ROTC) {
            carry = sgx_const(1, t->imm >> 31);
            has_carry = 1;
        }
    } else if (t->flags & TH_RSHIFT) {
        y = tshift_reg(g, m, t->ra, treg(g, t->imm), &carry);
        has_carry = 1;
    } else
        y = tshift_imm(g, m, t->ra, t->imm, &carry, &has_carry);
    switch (t->op) {
        case 0: /* AND, TST */
            r = sg_d_and(g, x, y);
            if (flags) tlogic_flags(g, r, has_carry ? &carry : NULL);
            tset(g, t->rd, r); /* TST: Rd 15 */
            break;
        case 1: /* BIC */
            r = sg_d_and(g, x, sg_d_not(g, y));
            if (flags) tlogic_flags(g, r, has_carry ? &carry : NULL);
            tset(g, t->rd, r);
            break;
        case 2: /* ORR, MOV */
            r = t->rn == 15 ? y : sg_d_or(g, x, y);
            if (flags) tlogic_flags(g, r, has_carry ? &carry : NULL);
            tset(g, t->rd, r);
            break;
        case 3: /* ORN, MVN */
            r = t->rn == 15 ? sg_d_not(g, y) : sg_d_or(g, x, sg_d_not(g, y));
            if (flags) tlogic_flags(g, r, has_carry ? &carry : NULL);
            tset(g, t->rd, r);
            break;
        case 4: /* EOR, TEQ */
            r = sg_d_xor(g, x, y);
            if (flags) tlogic_flags(g, r, has_carry ? &carry : NULL);
            tset(g, t->rd, r); /* TEQ: Rd 15 */
            break;
        case 8: /* ADD, CMN */
            if (compare)
                taddsub(g, x, y, 0, 1);
            else
                tset(g, t->rd, taddsub(g, x, y, 0, flags));
            break;
        case 10: /* ADC, CMP */
            if (compare)
                taddsub(g, x, y, 1, 1);
            else
                tset(g, t->rd, tadc(g, x, y, 0, flags));
            break;
        case 11: /* SBC, CMN */
            if (compare)
                taddsub(g, x, y, 0, 1);
            else
                tset(g, t->rd, tadc(g, x, y, 1, flags));
            break;
        case 13: /* SUB, CMP */
            if (compare)
                taddsub(g, x, y, 1, 1);
            else
                tset(g, t->rd, taddsub(g, x, y, 1, flags));
            break;
        case 14: tset(g, t->rd, taddsub(g, y, x, 1, flags)); break; /* RSB */
        default: sg_undefined(g); break;
    }
}

/* MUL, MLA, MLS: the 32-bit product, with Ra or taken from it, no flags */
static void tmul(SgCtx* g, const SnxThumb* t) {
    SgD r = SGX_OP(sgx(g), mul, 32, treg(g, t->rn), treg(g, t->rm));
    if (t->op == 1)
        r = sg_d_add(g, treg(g, t->ra), r);
    else if (t->op == 2)
        r = sg_d_sub(g, treg(g, t->ra), r);
    tset(g, t->rd, r);
}

/* SBFX, UBFX, BFI, BFC (llvm_thumb.inc's thumb_bitfield) */
static void tbitfield(SgCtx* g, const SnxThumb* t) {
    unsigned lsb = t->imm;
    SgD x = treg(g, t->rn), r;
    if (t->op < 2) { /* SBFX (0), UBFX (1): .ra the width - 1 */
        r = sg_d_slice(g, x, lsb + t->ra, lsb);
        tset(g, t->rd, t->op ? sg_d_zext(g, r, 32) : sg_d_sext(g, r, 32));
        return;
    }
    unsigned width = t->ra - lsb + 1;
    SnxU32 mask = (width >= 32 ? UINT32_MAX : (1u << width) - 1) << lsb;
    SgD src = t->op == 3 ? sgx_const(32, 0) : x, dst = treg(g, t->rd);
    r = sg_d_or(g, sg_d_and(g, dst, sgx_const(32, ~mask)), sg_d_and(g, sg_d_lsl(g, src, lsb), sgx_const(32, mask)));
    tset(g, t->rd, r);
}

/* MOVW, MOVT, ADDW, SUBW (and ADR.W, whose base is Align(PC, 4)), no flags */
static void twimm(SgCtx* g, const SnxThumb* t) {
    if (t->op == 0)
        tset(g, t->rd, sgx_const(32, t->imm));
    else if (t->op == 1)
        tset(g, t->rd, sg_d_or(g, sg_d_and(g, treg(g, t->rd), sgx_const(32, 0xFFFF)), sgx_const(32, t->imm << 16)));
    else
        tset(g, t->rd,
             taddsub(g, t->rn == 15 ? tpcalign(g) : treg(g, t->rn), sgx_const(32, t->imm), t->op == ALU_SUB, 0));
}

/* TBB, TBH: a byte or a halfword of the table at Rn (+ Rm, or + Rm * 2), and PC plus twice it */
static void ttable(SgCtx* g, const SnxThumb* t) {
    SgD base = sg_d_add(g, treg(g, t->rn), t->op ? sg_d_lsl(g, treg(g, t->rm), 1) : treg(g, t->rm));
    unsigned bytes = t->op ? 2 : 1;
    sgx_a32_commit(g);
    sgx_reg(sgx(g), &base);
    sgx_a32_transfer(g, base, bytes, 1, 0, 0);
    SgD index = sgx_a32_read(g, base, (int)bytes, 0, 0);
    sg_branch_to(g, sg_d_add(g, sg_d_lsl(g, index, 1), sgx_const(32, (SnxU32)g->pc + 4)), 0);
}

/* the offset a B.W, BL or BLX (immediate) adds to the PC (llvm_thumb.inc's thumb_branch_offset) from the raw halfwords
   (.rn and .rm), BLX's base aligned: with BLX, A32 the target's state */
static SnxU32 tbranch_target(const SnxThumb* t, SnxU32 pc, int* thumb) {
    unsigned hw1 = t->rn, hw2 = t->rm;
    unsigned s = (hw1 >> 10) & 1, i1 = !(((hw2 >> 13) & 1) ^ s), i2 = !(((hw2 >> 11) & 1) ^ s);
    *thumb = t->op != BR_BLX_IMM;
    SnxU32 imm = (SnxU32)(s << 23 | i1 << 22 | i2 << 21 | (hw1 & 0x3FF) << 11);
    if (t->op == BR_BLX_IMM)
        imm |= (hw2 >> 1 & 0x3FF) << 1;
    else
        imm |= hw2 & 0x7FF;
    if (imm & 0x800000) imm |= 0xFF000000u;
    if (t->op == BR_BLX_IMM) return ((pc + 4) & ~3u) + (imm << 1);
    return pc + 4 + (imm << 1);
}

/* B<c>.W, B.W, BL, BLX (immediate): 1, the block's end. A B<c>.W in an IT block (UNPREDICTABLE) runs under both
   conditions */
static int tbranch(SgCtx* g, const SnxThumb* t) {
    SnxU32 pc = (SnxU32)g->pc;
    switch (t->op) {
        case BR_BCOND_W: {
            SnxU32 off = t->imm << 1;
            if (off & 0x100000) off |= 0xFFE00000u;
            SgD target = sgx_const(32, pc + 4 + off);
            if (g->when) { /* the IT block's condition: its skip made first, then its own */
                sgx_a32_commit(g);
                sg_branch_to(g, target, sgx_a32_condition(g, t->ra));
            } else {
                sg_when(g, sgx_a32_condition(g, t->ra));
                sg_branch_to(g, target, 0);
            }
            return 1;
        }
        case BR_BW:
        case BR_BL:
        case BR_BLX_IMM: {
            int thumb;
            SnxU32 target = tbranch_target(t, pc, &thumb);
            sgx_a32_commit(g);
            if (t->op != BR_BW) sg_GPR_write(g, 14, sgx_const(32, (pc + 4) | 1u));
            if (!thumb) sg_PSTATE_write(g, SG_PSTATE_T, 1, sgx_const(1, 0));
            sg_branch_to(g, sgx_const(32, target), 0);
            return 1;
        }
        default: sg_fail("a Thumb branch this back end does not make", t->op, 0);
    }
}

/* a 32-bit Thumb instruction (llvm_thumb.inc's thumb_instruction, its 32-bit shapes): 1 where the block ends after it */
static int thumb32(SgCtx* g, const SnxThumb* t) {
    if (sgx_a32_checked(g) && t->shape == TH_TABLE) {
        sgx_a32_trap(g, SNX_ARM_BACKEND_UNSUPPORTED);
        return 1;
    }
    switch (t->shape) {
        case TH_LOAD:
        case TH_STORE: return tldst(g, t);
        case TH_DUAL: tdual(g, t); return 0;
        case TH_WDATA: twdata(g, t); return 0;
        case TH_WIMM: twimm(g, t); return 0;
        case TH_MUL: tmul(g, t); return 0;
        case TH_BITFIELD: tbitfield(g, t); return 0;
        case TH_TABLE: ttable(g, t); return 1;
        case TH_HINT: return 0;
        case TH_A32: { /* the A32 emitter runs the twin, reading Thumb's PC (g->thumb) */
            g->word = t->imm;
            g->pair = t->ra;
            g->offset = t->rd;
            int stop = instruction(g, (SnxA32Kind)t->op);
            g->pair = 0;
            g->offset = 0;
            return stop;
        }
        case TH_BRANCH: return tbranch(g, t);
        default: sg_fail("a 32-bit Thumb instruction this back end does not make", t->shape, t->op);
    }
}

/* the code's halfword i (little-endian) */
static unsigned half_at(const unsigned char* code, size_t i) { return code[2 * i] | (unsigned)code[2 * i + 1] << 8; }

/* the block's halfwords, until an instruction ends it: how many, and ITSTATE after the last (0 where none is open) */
static size_t thumb_code(MG_ARM* c, const unsigned char* code, size_t count, SnxU32 pc, unsigned* it) {
    SgCtx* g = &c->g;
    SgX* x = &g->x;
    SgXBuf* b = &c->b;
    size_t used = 0;
    unsigned itstate = 0;
    /* before Thumb-2 every halfword is a 16-bit instruction, its BL's halves (11110, 11111, 11101) among them */
    unsigned wide = g->profile == SNX_ARM_PROFILE_A57 || g->profile == SNX_ARM_PROFILE_A9 ? 0x1D : 0x20;
    for (;;) {
        unsigned hw1 = half_at(code, used), hw2 = used + 1 < count ? half_at(code, used + 1) : 0;
        size_t len;
        int stop;
        if (x->n > SGX_MAXI - SGX_MAXI / 4 || x->nv > SGX_MAXV - SGX_MAXV / 4 || x->nb > SGX_MAXB - SGX_MAXB / 4)
            flush(c);
        g->pc = pc + 2 * (SnxU32)used;
        g->yields = 0;
        g->branched = g->undefined = 0;
        g->taken = 0;
        g->iset = 0;
        g->skip = -1;
        g->when = 0;
        g->it_set = 0;
        g->itstate = itstate;
        if ((hw1 >> 11) < wide) {
            /* a 16-bit instruction: the Sail's, its condition (the IT block's, a B<c>'s) its own */
            len = 1;
            g->word = hw1;
            g->next = (SnxU32)g->pc + 2;
            itstate = itstate & 7 ? (itstate & 0xE0) | ((itstate << 1) & 0x1F) : 0; /* ITAdvance */
            g->it_after = itstate;
            sgx_a32_fetch(g, hw1, 2);
            g->start = x->n;
            if (sg_decode_t16(g))
                stop = g->branched || (g->undefined && g->skip < 0);
            else { /* no instruction: UNDEFINED, whatever the IT block's condition */
                sg_undefined(g);
                stop = 1;
            }
            if (g->it_set) itstate = g->it_next;
        } else {
            SnxThumb t;
            unsigned decoded = snx_thumb_decode_profile(g->profile, (uint16_t)hw1, (uint16_t)hw2, &t), cond = 14;
            if (!decoded || used + 1 >= count) { /* UNDEFINED (the 32-bit word when its second half is here) */
                len = used + 1 < count ? 2 : 1;
                g->word = len == 2 ? (SnxU32)hw1 | (SnxU32)hw2 << 16 : hw1;
                g->next = (SnxU32)g->pc + 2 * (SnxU32)len;
                g->it_after = 0;
                sgx_a32_fetch(g, g->word, (unsigned)len * 2);
                sg_undefined(g);
                stop = 1;
            } else {
                len = 2;
                g->word = (SnxU32)hw1 | (SnxU32)hw2 << 16;
                g->next = (SnxU32)g->pc + 4;
                if (itstate) { /* a B<c>, CBZ or CBNZ, and BKPT, run as they are written, not under it */
                    if (t.shape != TH_BRANCH ||
                        (t.op != BR_BKPT && t.op != BR_BCOND && t.op != BR_CBZ && t.op != BR_CBNZ))
                        cond = itstate >> 4;
                    itstate = itstate & 7 ? (itstate & 0xE0) | ((itstate << 1) & 0x1F) : 0;
                }
                g->it_after = itstate;
                sgx_a32_fetch(g, g->word, 4);
                g->start = x->n;
                if (cond != 14) sg_when(g, sgx_a32_condition(g, cond));
                stop = thumb32(g, &t);
                if (g->undefined) stop = cond == 14; /* it stopped the block; under a condition the block goes on */
            }
        }
        if (x->unmade) sg_fail("a primitive this host has no form of", 0, 0);
        sgx_a32_finish(g);
        if (!stop) sgx_a32_yield_check(g);
        used += len;
        if (stop || used >= count) break;
        if (g->skip >= 0) {
            flush(c);
            sgx_a32_bind(g, b, g->skip);
        }
    }
    *it = itstate;
    return used;
}
