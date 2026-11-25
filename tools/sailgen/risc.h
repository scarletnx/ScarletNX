/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's machine back ends for hosts without Arm's flags (README.md, "The RISC back ends"): riscv64, loongarch64,
   mips64, powerpc64 and s390x. After the machine's own header (rv64.h...), which gives its registers and a small set
   of operations in its encodings (rk_*, below), and before mach.h: every primitive mach.h selects is written here once
   from those operations, so a machine's part is its encodings and ABI alone.

   Virtual flags. mach.h keeps Arm's NZCV as the flags an instruction left, read through its selection's map, as x86's
   and arm64's are. Here the flags are two reserved registers, RK_FA and RK_FB, and the selection that set them says in
   its condition codes what they hold (bits 4-5 of a code: its kind; bit 6: of 32 bits):
     RK_K_SUB    a compare's or a subtraction's operands a and b (of 32 bits: both sign extended, which keeps their
                 order signed and unsigned): every Arm condition is a compare of the two (or, MI and VS, their
                 difference's sign and overflow)
     RK_K_LOGIC  a logical result (of 32 bits sign extended): N its sign, Z whether 0, C and V clear
     RK_K_NZCV   NZCV itself in RK_FA's bits 3-0, which an addition (ADDS, ADCS, SBCS) works out
   A condition's reader (a select, a set, a jump, a flag's store) makes it 0 or 1 in a register from them (risc_cond).
   Nothing but a setter writes RK_FA and RK_FB, and a call may change them as it does a machine's flags (mach.h makes
   what is read after a call 0 or 1 first).

   The operations a machine gives (d, a, c host registers, any of them the same one; sf 1 for 64 bits, 0 for the low
   32, the result's bits above them anything; an immediate any value unless said, the machine making it in RK_SA where
   its encoding can't hold it, and leaving every other register as it was):
     rk_mv(b, d, a)             rk_li(b, d, k)            (k's 64 bits exactly; d alone written)
     rk_add, rk_sub(b, d, a, c, sf)   rk_neg(b, d, a, sf)
     rk_and, rk_or, rk_xor(b, d, a, c)     rk_addi(b, d, a, k, sf), rk_andi, rk_ori, rk_xori(b, d, a, k)
     rk_sll, rk_srl, rk_sra(b, d, a, c, sf)   (the amount modulo the width)
     rk_slli, rk_srli, rk_srai(b, d, a, k, sf)   (0 <= k < the width)
     rk_mul(b, d, a, c, sf), rk_mulhu, rk_mulh(b, d, a, c)   (the high doubleword of the 128-bit product)
     rk_divu, rk_div(b, d, a, c)   (64 bits; mach.h keeps the divisor from 0 and -1 under the most negative value)
     rk_sltu, rk_slt(b, d, a, c)   (64 bits: 1 where a < c)       rk_seqz, rk_snez(b, d, a)
     rk_sext, rk_zext(b, d, a, bits)   (the low 8, 16 or 32 bits extended to 64)
     rk_ldst(b, load, bytes, sign, rt, base, index, disp)   (index -1 for none; a load's value zero extended, or sign
                                                            extended where sign; the address made in RK_SA if need be)
   and RK_SA (the assembler's own), RK_S1 and RK_S2 (the sequences' here, which an operation never uses), RK_FA and
   RK_FB. A small immediate (-2048 to 2047) never takes RK_SA. */

/* ---- conditions: Arm's codes, and the virtual flags' kinds ---- */

static const char* const sgx_cc_name[16] = {"EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC",
                                            "HI", "LS", "GE", "LT", "GT", "LE", "AL", "NV"};
static const signed char sgx_flag_cc[4] = {4, 0, 2, 6};
static const signed char sgx_flag_ncc[4] = {5, 1, 3, 7};
enum { SGX_ARM_EQ, SGX_ARM_NE, SGX_ARM_CS, SGX_ARM_CC, SGX_ARM_MI, SGX_ARM_PL, SGX_ARM_VS, SGX_ARM_VC, SGX_ARM_HI,
       SGX_ARM_LS, SGX_ARM_GE, SGX_ARM_LT, SGX_ARM_GT, SGX_ARM_LE, SGX_ARM_COUNT };
enum { RK_K_SUB = 0x00, RK_K_LOGIC = 0x10, RK_K_NZCV = 0x20, RK_W32 = 0x40 };
/* a flag's code (mach.h's sgx_arm_flag_cc) carries the kind and width of the selection that set the flags */
#define SGX_CC_EXTRA(fsel) ((fsel)->cc_of[0] & 0x70)

#define SGX_F_GPR 0
#define SGX_F_AVX 0
#define SGX_CARRY 2
#define SGX_DIV_ARM 0
#define SGX_DIV_2OP 1
#define SGX_FP_HELPERS 1 /* mach_a64.h's floating point */
#define SGX_IMPLICIT_X0 0
#define SGX_HAS_VREGS 0
#define SGX_NXREG 1 /* none for values (SGX_HAS_VREGS): mach.h's arrays of them have one */
#define SGX_XPIN 64
#define SGX_MEMR(r) (64 + (r))
#define SGX_POOLR 127
#define RK_ISMEM(v) ((v) >= 64)

/* the condition cc on the flags, 0 or 1 into d (not RK_S2, RK_FA or RK_FB; RK_S2 its own) */
static inline void risc_cond(SgXBuf* b, int d, int cc) {
    int cond = cc & 15, kind = cc & 0x30, w32 = !!(cc & RK_W32), base = cond & ~1;
    if (cond >= 14) {
        rk_li(b, d, 1);
        return;
    }
    if (kind == RK_K_SUB) {
        switch (base) {
        case SGX_ARM_EQ: rk_xor(b, d, RK_FA, RK_FB); rk_seqz(b, d, d); break;
        case SGX_ARM_CS: rk_sltu(b, d, RK_FA, RK_FB); rk_xori(b, d, d, 1); break;
        case SGX_ARM_MI:
            rk_sub(b, d, RK_FA, RK_FB, 1);
            rk_srli(b, d, d, w32 ? 31 : 63, 1);
            if (w32) rk_andi(b, d, d, 1);
            break;
        case SGX_ARM_VS:
            if (w32) {
                /* the exact difference of two 32-bit values outside 32 bits */
                rk_sub(b, d, RK_FA, RK_FB, 1);
                rk_sext(b, RK_S2, d, 32);
                rk_xor(b, d, d, RK_S2);
                rk_snez(b, d, d);
            } else {
                rk_xor(b, RK_S2, RK_FA, RK_FB);
                rk_sub(b, d, RK_FA, RK_FB, 1);
                rk_xor(b, d, RK_FA, d);
                rk_and(b, d, d, RK_S2);
                rk_srli(b, d, d, 63, 1);
            }
            break;
        case SGX_ARM_HI: rk_sltu(b, d, RK_FB, RK_FA); break;
        case SGX_ARM_GE: rk_slt(b, d, RK_FA, RK_FB); rk_xori(b, d, d, 1); break;
        default: rk_slt(b, d, RK_FB, RK_FA); break; /* GT */
        }
    } else if (kind == RK_K_LOGIC) {
        switch (base) {
        case SGX_ARM_EQ: rk_seqz(b, d, RK_FA); break;
        case SGX_ARM_CS:
        case SGX_ARM_VS:
        case SGX_ARM_HI: rk_li(b, d, 0); break;
        case SGX_ARM_MI: rk_srli(b, d, RK_FA, 63, 1); break;
        case SGX_ARM_GE: rk_srli(b, d, RK_FA, 63, 1); rk_xori(b, d, d, 1); break;
        default: rk_li(b, RK_S2, 0); rk_slt(b, d, RK_S2, RK_FA); break; /* GT: a result above 0 */
        }
    } else {
        switch (base) {
        case SGX_ARM_EQ: rk_srli(b, d, RK_FA, 2, 1); rk_andi(b, d, d, 1); break;
        case SGX_ARM_CS: rk_srli(b, d, RK_FA, 1, 1); rk_andi(b, d, d, 1); break;
        case SGX_ARM_MI: rk_srli(b, d, RK_FA, 3, 1); rk_andi(b, d, d, 1); break;
        case SGX_ARM_VS: rk_andi(b, d, RK_FA, 1); break;
        case SGX_ARM_HI: /* Z:C == 0:1 */
            rk_srli(b, d, RK_FA, 1, 1);
            rk_andi(b, d, d, 3);
            rk_addi(b, d, d, -1, 1);
            rk_seqz(b, d, d);
            break;
        case SGX_ARM_GE: /* N == V */
            rk_srli(b, d, RK_FA, 3, 1);
            rk_xor(b, d, d, RK_FA);
            rk_andi(b, d, d, 1);
            rk_xori(b, d, d, 1);
            break;
        default: /* GT: Z clear and N == V; LE is N != V or Z */
            rk_srli(b, d, RK_FA, 3, 1);
            rk_xor(b, d, d, RK_FA);
            rk_andi(b, d, d, 1);
            rk_srli(b, RK_S2, RK_FA, 2, 1);
            rk_andi(b, RK_S2, RK_S2, 1);
            rk_or(b, d, d, RK_S2);
            rk_xori(b, d, d, 1);
            break;
        }
    }
    if (cond & 1) rk_xori(b, d, d, 1);
}

/* the flags of a + b (+ the carry RK_FA's C, cin; b NOT'd first, inv: SBCS) as NZCV in RK_FA (RK_K_NZCV); the sum
   into d (-1 for none). b is a register, or the constant k where breg < 0 */
static inline void risc_addc(SgXBuf* b, int d, int ra, int breg, SnxU64 k, int w32, int cin, int inv) {
    if (cin) {
        rk_srli(b, RK_S1, RK_FA, 1, 1);
        rk_andi(b, RK_S1, RK_S1, 1);
    }
    if (w32) {
        rk_zext(b, RK_FA, ra, 32);
        if (breg < 0) rk_li(b, RK_FB, k & 0xffffffffull);
        else
            rk_zext(b, RK_FB, breg, 32);
        if (inv) rk_xori(b, RK_FB, RK_FB, 0xffffffffll);
        rk_add(b, RK_S2, RK_FA, RK_FB, 1);
        if (cin) rk_add(b, RK_S2, RK_S2, RK_S1, 1);
        rk_srli(b, RK_SA, RK_S2, 32, 1); /* C: the 33rd bit of the exact sum */
        rk_xor(b, RK_S1, RK_S2, RK_FA);
        rk_xor(b, RK_FA, RK_S2, RK_FB);
        rk_and(b, RK_FA, RK_FA, RK_S1);
        rk_srli(b, RK_FA, RK_FA, 31, 1);
        rk_andi(b, RK_FA, RK_FA, 1); /* V */
        rk_slli(b, RK_SA, RK_SA, 1, 1);
        rk_or(b, RK_FA, RK_FA, RK_SA);
        rk_slli(b, RK_S1, RK_S2, 32, 1);
        rk_seqz(b, RK_S1, RK_S1);
        rk_slli(b, RK_S1, RK_S1, 2, 1);
        rk_or(b, RK_FA, RK_FA, RK_S1); /* Z */
        rk_srli(b, RK_S1, RK_S2, 31, 1);
        rk_andi(b, RK_S1, RK_S1, 1);
        rk_slli(b, RK_S1, RK_S1, 3, 1);
        rk_or(b, RK_FA, RK_FA, RK_S1); /* N */
    } else {
        rk_mv(b, RK_FA, ra);
        if (breg < 0) rk_li(b, RK_FB, k);
        else
            rk_mv(b, RK_FB, breg);
        if (inv) rk_xori(b, RK_FB, RK_FB, -1);
        rk_add(b, RK_S2, RK_FA, RK_FB, 1);
        rk_sltu(b, RK_SA, RK_S2, RK_FA); /* the carry of a + b */
        if (cin) {
            rk_add(b, RK_S2, RK_S2, RK_S1, 1);
            rk_sltu(b, RK_S1, RK_S2, RK_S1); /* and of + cin */
            rk_or(b, RK_SA, RK_SA, RK_S1);
        }
        rk_xor(b, RK_S1, RK_S2, RK_FA);
        rk_xor(b, RK_FA, RK_S2, RK_FB);
        rk_and(b, RK_FA, RK_FA, RK_S1);
        rk_srli(b, RK_FA, RK_FA, 63, 1); /* V */
        rk_slli(b, RK_SA, RK_SA, 1, 1);
        rk_or(b, RK_FA, RK_FA, RK_SA);
        rk_seqz(b, RK_S1, RK_S2);
        rk_slli(b, RK_S1, RK_S1, 2, 1);
        rk_or(b, RK_FA, RK_FA, RK_S1); /* Z */
        rk_srli(b, RK_S1, RK_S2, 63, 1);
        rk_slli(b, RK_S1, RK_S1, 3, 1);
        rk_or(b, RK_FA, RK_FA, RK_S1); /* N */
    }
    if (d >= 0) rk_mv(b, d, RK_S2);
}

/* a compare's operands as RK_K_SUB's flags: a register, or the constant k where breg < 0 */
static inline void risc_cmp(SgXBuf* b, int ra, int breg, SnxU64 k, int w32) {
    if (w32) {
        rk_sext(b, RK_FA, ra, 32);
        if (breg < 0) rk_li(b, RK_FB, (SnxU64)(long long)(int)(SnxU32)k);
        else
            rk_sext(b, RK_FB, breg, 32);
    } else {
        rk_mv(b, RK_FA, ra);
        if (breg < 0) rk_li(b, RK_FB, k);
        else
            rk_mv(b, RK_FB, breg);
    }
}

/* ---- the encoders: r[] the operands' registers (SGX_MEMR(base) for memory, with b->index and b->disp), imm the
   immediate, cc the condition ---- */

#define RK_E(name) static void rk_e_##name(SgXBuf* b, const unsigned char* r, long long imm, int cc)
#define RK_SX32(imm) ((SnxU64)(long long)(int)(SnxU32)(SnxU64)(imm))

#define RK_E2(name, op)                                                                                                \
    RK_E(name##_64) { (void)imm, (void)cc, op(b, r[0], r[1], r[2], 1); }                                               \
    RK_E(name##_32) { (void)imm, (void)cc, op(b, r[0], r[1], r[2], 0); }
#define RK_E2L(name, op)                                                                                               \
    RK_E(name##_64) { (void)imm, (void)cc, op(b, r[0], r[1], r[2]); }                                                  \
    RK_E(name##_32) { (void)imm, (void)cc, op(b, r[0], r[1], r[2]); }
RK_E2(add, rk_add)
RK_E2(sub, rk_sub)
RK_E2L(and, rk_and)
RK_E2L(or, rk_or)
RK_E2L(xor, rk_xor)
RK_E2(mul, rk_mul)
RK_E2(lslv, rk_sll)
RK_E2(lsrv, rk_srl)
RK_E2(asrv, rk_sra)
RK_E(umulh_64) { (void)imm, (void)cc, rk_mulhu(b, r[0], r[1], r[2]); }
RK_E(smulh_64) { (void)imm, (void)cc, rk_mulh(b, r[0], r[1], r[2]); }
RK_E(udiv_64) { (void)imm, (void)cc, rk_divu(b, r[0], r[1], r[2]); }
RK_E(sdiv_64) { (void)imm, (void)cc, rk_div(b, r[0], r[1], r[2]); }
RK_E(neg_64) { (void)imm, (void)cc, rk_neg(b, r[0], r[1], 1); }
RK_E(neg_32) { (void)imm, (void)cc, rk_neg(b, r[0], r[1], 0); }
RK_E(not_64) { (void)imm, (void)cc, rk_xori(b, r[0], r[1], -1); }
RK_E(not_32) { (void)imm, (void)cc, rk_xori(b, r[0], r[1], -1); }
RK_E(bic_64) {
    (void)imm, (void)cc;
    rk_xori(b, RK_S1, r[2], -1);
    rk_and(b, r[0], r[1], RK_S1);
}
RK_E(bic_32) { rk_e_bic_64(b, r, imm, cc); }
/* rotations: the two shifts ORed (an amount of 0 the value itself) */
static inline void risc_rorv(SgXBuf* b, int d, int a, int c, int sf) {
    rk_srl(b, RK_S1, a, c, sf);
    rk_neg(b, RK_S2, c, 1);
    rk_sll(b, RK_S2, a, RK_S2, sf);
    rk_or(b, d, RK_S1, RK_S2);
}
RK_E(rorv_64) { (void)imm, (void)cc, risc_rorv(b, r[0], r[1], r[2], 1); }
RK_E(rorv_32) { (void)imm, (void)cc, risc_rorv(b, r[0], r[1], r[2], 0); }
static inline void risc_shift_i(SgXBuf* b, const unsigned char* r, long long imm, int kind, int sf) {
    int w = sf ? 64 : 32, k = (int)(imm & (w - 1));
    if (kind == 0) rk_slli(b, r[0], r[1], k, sf);
    else if (kind == 1)
        rk_srli(b, r[0], r[1], k, sf);
    else if (kind == 2)
        rk_srai(b, r[0], r[1], k, sf);
    else if (!k)
        rk_mv(b, r[0], r[1]);
    else {
        rk_srli(b, RK_S1, r[1], k, sf);
        rk_slli(b, RK_S2, r[1], w - k, sf);
        rk_or(b, r[0], RK_S1, RK_S2);
    }
}
RK_E(lsl_i_64) { (void)cc, risc_shift_i(b, r, imm, 0, 1); }
RK_E(lsl_i_32) { (void)cc, risc_shift_i(b, r, imm, 0, 0); }
RK_E(lsr_i_64) { (void)cc, risc_shift_i(b, r, imm, 1, 1); }
RK_E(lsr_i_32) { (void)cc, risc_shift_i(b, r, imm, 1, 0); }
RK_E(asr_i_64) { (void)cc, risc_shift_i(b, r, imm, 2, 1); }
RK_E(asr_i_32) { (void)cc, risc_shift_i(b, r, imm, 2, 0); }
RK_E(ror_i_64) { (void)cc, risc_shift_i(b, r, imm, 3, 1); }
RK_E(ror_i_32) { (void)cc, risc_shift_i(b, r, imm, 3, 0); }
/* the bytes reversed: each halfword's, then each word's halfwords, then (64 bits) the words */
static inline void risc_rev(SgXBuf* b, int d, int a, int sf) {
    if (sf) rk_mv(b, RK_S1, a);
    else
        rk_zext(b, RK_S1, a, 32);
    rk_li(b, RK_S2, sf ? 0x00ff00ff00ff00ffull : 0x00ff00ffull);
    rk_srli(b, RK_SA, RK_S1, 8, 1);
    rk_and(b, RK_SA, RK_SA, RK_S2);
    rk_and(b, RK_S1, RK_S1, RK_S2);
    rk_slli(b, RK_S1, RK_S1, 8, 1);
    rk_or(b, RK_S1, RK_S1, RK_SA);
    if (sf) {
        rk_li(b, RK_S2, 0x0000ffff0000ffffull);
        rk_srli(b, RK_SA, RK_S1, 16, 1);
        rk_and(b, RK_SA, RK_SA, RK_S2);
        rk_and(b, RK_S1, RK_S1, RK_S2);
        rk_slli(b, RK_S1, RK_S1, 16, 1);
        rk_or(b, RK_S1, RK_S1, RK_SA);
    }
    rk_srli(b, RK_SA, RK_S1, sf ? 32 : 16, 1);
    rk_slli(b, RK_S1, RK_S1, sf ? 32 : 16, 1);
    rk_or(b, d, RK_S1, RK_SA);
}
RK_E(rev_64) { (void)imm, (void)cc, risc_rev(b, r[0], r[1], 1); }
RK_E(rev_32) { (void)imm, (void)cc, risc_rev(b, r[0], r[1], 0); }

/* the flags' forms */
RK_E(cmp_64) { (void)imm, (void)cc, risc_cmp(b, r[1], r[2], 0, 0); }
RK_E(cmp_32) { (void)imm, (void)cc, risc_cmp(b, r[1], r[2], 0, 1); }
RK_E(cmp_i_64) { (void)cc, risc_cmp(b, r[1], -1, RK_SX32(imm), 0); }
RK_E(cmp_i_32) { (void)cc, risc_cmp(b, r[1], -1, RK_SX32(imm), 1); }
RK_E(subs_64) { (void)imm, (void)cc, risc_cmp(b, r[1], r[2], 0, 0), rk_sub(b, r[0], RK_FA, RK_FB, 1); }
RK_E(subs_32) { (void)imm, (void)cc, risc_cmp(b, r[1], r[2], 0, 1), rk_sub(b, r[0], RK_FA, RK_FB, 1); }
RK_E(subs_i_64) { (void)cc, risc_cmp(b, r[1], -1, RK_SX32(imm), 0), rk_sub(b, r[0], RK_FA, RK_FB, 1); }
RK_E(subs_i_32) { (void)cc, risc_cmp(b, r[1], -1, RK_SX32(imm), 1), rk_sub(b, r[0], RK_FA, RK_FB, 1); }
static inline void risc_logic_flags(SgXBuf* b, int d, int a, int c, int w32) {
    rk_and(b, RK_FA, a, c);
    if (w32) rk_sext(b, RK_FA, RK_FA, 32);
    if (d >= 0) rk_mv(b, d, RK_FA);
}
RK_E(tst_64) { (void)imm, (void)cc, risc_logic_flags(b, -1, r[1], r[2], 0); }
RK_E(tst_32) { (void)imm, (void)cc, risc_logic_flags(b, -1, r[1], r[2], 1); }
RK_E(ands_64) { (void)imm, (void)cc, risc_logic_flags(b, r[0], r[1], r[2], 0); }
RK_E(ands_32) { (void)imm, (void)cc, risc_logic_flags(b, r[0], r[1], r[2], 1); }
RK_E(adds_64) { (void)imm, (void)cc, risc_addc(b, r[0], r[1], r[2], 0, 0, 0, 0); }
RK_E(adds_32) { (void)imm, (void)cc, risc_addc(b, r[0], r[1], r[2], 0, 1, 0, 0); }
RK_E(adds_i_64) { (void)cc, risc_addc(b, r[0], r[1], -1, RK_SX32(imm), 0, 0, 0); }
RK_E(adds_i_32) { (void)cc, risc_addc(b, r[0], r[1], -1, RK_SX32(imm), 1, 0, 0); }
/* ADCS and SBCS: the carry in is the C of the flags mach.h's carry setter (adds_i) left, which are RK_K_NZCV's */
RK_E(adcs_64) { (void)imm, (void)cc, risc_addc(b, r[0], r[1], r[2], 0, 0, 1, 0); }
RK_E(adcs_32) { (void)imm, (void)cc, risc_addc(b, r[0], r[1], r[2], 0, 1, 1, 0); }
RK_E(sbcs_64) { (void)imm, (void)cc, risc_addc(b, r[0], r[1], r[2], 0, 0, 1, 1); }
RK_E(sbcs_32) { (void)imm, (void)cc, risc_addc(b, r[0], r[1], r[2], 0, 1, 1, 1); }

/* immediates: an addition's 32-bit immediate sign extended, a logical operation's of the width */
RK_E(add_i_64) { (void)cc, rk_addi(b, r[0], r[1], (long long)RK_SX32(imm), 1); }
RK_E(add_i_32) { (void)cc, rk_addi(b, r[0], r[1], (long long)RK_SX32(imm), 0); }
RK_E(and_i_64) { (void)cc, rk_andi(b, r[0], r[1], imm); }
RK_E(and_i_32) { (void)cc, rk_andi(b, r[0], r[1], (long long)RK_SX32(imm)); }
RK_E(or_i_64) { (void)cc, rk_ori(b, r[0], r[1], imm); }
RK_E(or_i_32) { (void)cc, rk_ori(b, r[0], r[1], (long long)RK_SX32(imm)); }
RK_E(xor_i_64) { (void)cc, rk_xori(b, r[0], r[1], imm); }
RK_E(xor_i_32) { (void)cc, rk_xori(b, r[0], r[1], (long long)RK_SX32(imm)); }

/* guest memory's accesses (the moves' and extensions' memory operands): the machine's rk_gldst where its loads and
   stores must be aligned and Arm's needn't be, or where the host is big-endian (RK_GLDST; the guest's memory is
   little-endian, its numbers byte-reversed), else its plain ones. A big-endian machine has rk_bswap too:

     rk_bswap(b, d, a, bytes)   d = the low 2, 4 or 8 bytes of a reversed, zero extended (RK_SA and RK_S2 its own,
                                and RK_S1 for 8); RK_BSWAP where it has one */
#ifndef RK_BIG_ENDIAN
#define RK_BIG_ENDIAN 0
#endif
#ifndef RK_GLDST
#if RK_BIG_ENDIAN
#error "a big-endian machine's guest memory wants its rk_gldst"
#endif
#define rk_gldst rk_ldst
#endif
#ifndef RK_BSWAP
#if RK_BIG_ENDIAN
#error "a big-endian machine's atomics want its rk_bswap"
#endif
static inline void rk_bswap(SgXBuf* b, int d, int a, int bytes) {
    (void)b, (void)d, (void)a, (void)bytes;
}
#endif
/* moves: a load where operand 1 is memory, a store where operand 0 is, else the register's copy */
static inline void risc_mov_form(SgXBuf* b, const unsigned char* r, int bytes) {
    if (RK_ISMEM(r[1])) rk_gldst(b, 1, bytes, 0, r[0], r[1] - 64, b->index, b->disp);
    else if (RK_ISMEM(r[0]))
        rk_gldst(b, 0, bytes, 0, r[1], r[0] - 64, b->index, b->disp);
    else
        rk_mv(b, r[0], r[1]);
}
RK_E(mov_64) { (void)imm, (void)cc, risc_mov_form(b, r, 8); }
RK_E(mov_32) { (void)imm, (void)cc, risc_mov_form(b, r, 4); }
RK_E(mov_16) { (void)imm, (void)cc, risc_mov_form(b, r, 2); }
RK_E(mov_8) { (void)imm, (void)cc, risc_mov_form(b, r, 1); }
static inline void risc_ext(SgXBuf* b, const unsigned char* r, int bits, int sign) {
    if (RK_ISMEM(r[1])) rk_gldst(b, 1, bits / 8, sign, r[0], r[1] - 64, b->index, b->disp);
    else if (sign)
        rk_sext(b, r[0], r[1], bits);
    else
        rk_zext(b, r[0], r[1], bits);
}
RK_E(zext8) { (void)imm, (void)cc, risc_ext(b, r, 8, 0); }
RK_E(zext16) { (void)imm, (void)cc, risc_ext(b, r, 16, 0); }
RK_E(zext32) { (void)imm, (void)cc, risc_ext(b, r, 32, 0); }
RK_E(sext8) { (void)imm, (void)cc, risc_ext(b, r, 8, 1); }
RK_E(sext16) { (void)imm, (void)cc, risc_ext(b, r, 16, 1); }
RK_E(sext32) { (void)imm, (void)cc, risc_ext(b, r, 32, 1); }
RK_E(movi_64) { (void)cc, rk_li(b, r[0], (SnxU64)imm); }
RK_E(movi_32) { (void)cc, rk_li(b, r[0], (SnxU64)(SnxU32)imm); }
/* CSEL: the condition's 0 or 1 made a mask, B ^ ((A ^ B) & mask); CSET the condition */
static inline void risc_csel(SgXBuf* b, int d, int a, int c, int cc) {
    risc_cond(b, RK_S1, cc);
    rk_neg(b, RK_S2, RK_S1, 1);
    rk_xor(b, RK_S1, a, c);
    rk_and(b, RK_S1, RK_S1, RK_S2);
    rk_xor(b, d, c, RK_S1);
}
RK_E(csel_64) { (void)imm, risc_csel(b, r[0], r[1], r[2], cc); }
RK_E(csel_32) { (void)imm, risc_csel(b, r[0], r[1], r[2], cc); }
RK_E(cset_64) { (void)imm, risc_cond(b, r[0], cc); }
RK_E(cset_32) { (void)imm, risc_cond(b, r[0], cc); }

/* ---- the selection: each primitive's one candidate ----
   operand 0 the result, 1 and 2 the inputs. A 32-bit result's bits above 32 are anything (clean_out 0: mach.h zero
   extends it where something reads them); a 64-bit one is exact. An input is read whole (clean) where the result's
   low bits depend on its bits above the value's, as arm64.h's are; the 32-bit operations here read the low 32 bits
   of a register alone. The flags' forms' condition codes carry their kind and width */
#define RK_CC(x) {0 | (x), 1 | (x), 2 | (x), 3 | (x), 4 | (x), 5 | (x), 6 | (x), 7 | (x), 8 | (x), 9 | (x), 10 | (x), 11 | (x), 12 | (x), 13 | (x), -1, -1}
#define RK_SEL(e, i0, i1, cl, im, fb, fk, o, fl, co, ccx)                                                             \
    {.enc = rk_e_##e, .rm = -1, .in = {i0, i1, -1, -1}, .clean = cl, .imm = im, .fit_bits = fb, .fit_kind = fk,     \
     .out = o, .clean_out = co, .flags = fl, .flag_of = {0, 1, 2, 3}, .cc_of = RK_CC(ccx)}
#define RK_CO(w) ((w) == 64)
#define RK_RR(name, w, cl) static const SgXSel sgx_sel_##name##_##w[1] = {RK_SEL(name##_##w, 1, 2, cl, 0, 0, '-', 0, 0, RK_CO(w), 0)};
#define RK_RRF(name, w, cl, k) static const SgXSel sgx_sel_##name##_##w[1] = {RK_SEL(name##_##w, 1, 2, cl, 0, 0, '-', 0, 0xF, RK_CO(w), (k) | ((w) == 32 ? RK_W32 : 0))};
#define RK_R(name, w, cl) static const SgXSel sgx_sel_##name##_##w[1] = {RK_SEL(name##_##w, 1, -1, cl, 0, 0, '-', 0, 0, RK_CO(w), 0)};
#define RK_RI(name, w, fb, fk, cl) static const SgXSel sgx_sel_##name##_##w[1] = {RK_SEL(name##_##w, 1, -1, cl, 2, fb, fk, 0, 0, RK_CO(w), 0)};
#define RK_RIF(name, w, cl, k) static const SgXSel sgx_sel_##name##_##w[1] = {RK_SEL(name##_##w, 1, -1, cl, 2, 0, '-', 0, 0xF, RK_CO(w), (k) | ((w) == 32 ? RK_W32 : 0))};
#define RK_N1(name, w) enum { sgx_sel_##name##_##w##_n = 1 };
#define RK_NS(name) RK_N1(name, 64) RK_N1(name, 32)
#define RK_PAIR(m, name, ...) m(name, 64 __VA_OPT__(,) __VA_ARGS__) m(name, 32 __VA_OPT__(,) __VA_ARGS__) RK_NS(name)

RK_PAIR(RK_RR, add, 0)
RK_PAIR(RK_RR, sub, 0)
RK_PAIR(RK_RRF, adds, 3, RK_K_NZCV)
RK_PAIR(RK_RRF, subs, 3, RK_K_SUB)
RK_PAIR(RK_RRF, adcs, 3, RK_K_NZCV)
RK_PAIR(RK_RRF, sbcs, 3, RK_K_NZCV)
RK_PAIR(RK_RR, and, 0)
RK_PAIR(RK_RR, or, 0)
RK_PAIR(RK_RR, xor, 0)
RK_PAIR(RK_RR, bic, 0)
RK_PAIR(RK_RRF, ands, 3, RK_K_LOGIC)
RK_PAIR(RK_RR, mul, 0)
RK_PAIR(RK_R, neg, 0)
RK_PAIR(RK_R, not, 0)
RK_PAIR(RK_R, rev, 1)
RK_RR(umulh, 64, 3)
RK_RR(smulh, 64, 3)
RK_RR(udiv, 64, 3)
RK_RR(sdiv, 64, 3)
RK_N1(umulh, 64)
RK_N1(smulh, 64)
RK_N1(udiv, 64)
RK_N1(sdiv, 64)
/* the flags alone */
static const SgXSel sgx_sel_cmp_64[1] = {RK_SEL(cmp_64, 1, 2, 3, 0, 0, '-', -1, 0xF, 1, RK_K_SUB)};
static const SgXSel sgx_sel_cmp_32[1] = {RK_SEL(cmp_32, 1, 2, 3, 0, 0, '-', -1, 0xF, 1, RK_K_SUB | RK_W32)};
static const SgXSel sgx_sel_tst_64[1] = {RK_SEL(tst_64, 1, 2, 3, 0, 0, '-', -1, 0xF, 1, RK_K_LOGIC)};
static const SgXSel sgx_sel_tst_32[1] = {RK_SEL(tst_32, 1, 2, 3, 0, 0, '-', -1, 0xF, 1, RK_K_LOGIC | RK_W32)};
static const SgXSel sgx_sel_cmp_i_64[1] = {RK_SEL(cmp_i_64, 1, -1, 1, 2, 0, '-', -1, 0xF, 1, RK_K_SUB)};
static const SgXSel sgx_sel_cmp_i_32[1] = {RK_SEL(cmp_i_32, 1, -1, 1, 2, 0, '-', -1, 0xF, 1, RK_K_SUB | RK_W32)};
RK_NS(cmp)
RK_NS(tst)
RK_NS(cmp_i)
RK_RI(lsl_i, 64, 6, 'U', 0)
RK_RI(lsl_i, 32, 5, 'U', 0)
RK_RI(lsr_i, 64, 6, 'U', 1)
RK_RI(lsr_i, 32, 5, 'U', 1)
RK_RI(asr_i, 64, 6, 'U', 1)
RK_RI(asr_i, 32, 5, 'U', 1)
RK_RI(ror_i, 64, 6, 'U', 1)
RK_RI(ror_i, 32, 5, 'U', 1)
RK_NS(lsl_i)
RK_NS(lsr_i)
RK_NS(asr_i)
RK_NS(ror_i)
RK_PAIR(RK_RR, lslv, 0)
RK_PAIR(RK_RR, lsrv, 1)
RK_PAIR(RK_RR, asrv, 1)
RK_PAIR(RK_RR, rorv, 1)
RK_PAIR(RK_RI, add_i, 0, '-', 0)
RK_PAIR(RK_RIF, adds_i, 1, RK_K_NZCV)
RK_PAIR(RK_RIF, subs_i, 1, RK_K_SUB)
RK_PAIR(RK_RI, and_i, 0, '-', 0)
RK_PAIR(RK_RI, or_i, 0, '-', 0)
RK_PAIR(RK_RI, xor_i, 0, '-', 0)
#define sgx_sel_umulh_32 ((const SgXSel*)0)
#define sgx_sel_umulh_32_n 0u
#define sgx_sel_smulh_32 ((const SgXSel*)0)
#define sgx_sel_smulh_32_n 0u
#define sgx_sel_udiv_32 ((const SgXSel*)0)
#define sgx_sel_udiv_32_n 0u
#define sgx_sel_sdiv_32 ((const SgXSel*)0)
#define sgx_sel_sdiv_32_n 0u
#define sgx_sel_zext32_32 ((const SgXSel*)0)
#define sgx_sel_zext32_32_n 0u
#define sgx_sel_sext32_32 ((const SgXSel*)0)
#define sgx_sel_sext32_32_n 0u
#define sgx_sel_movi_s32_32 ((const SgXSel*)0)
#define sgx_sel_movi_s32_32_n 0u
#define sgx_sel_lea_64 sgx_sel_add_64
#define sgx_sel_lea_32 sgx_sel_add_32
#define sgx_sel_leai_64 sgx_sel_add_i_64
#define sgx_sel_leai_32 sgx_sel_add_i_32
#define sgx_sel_lea_64_n 1u
#define sgx_sel_lea_32_n 1u
#define sgx_sel_leai_64_n 1u
#define sgx_sel_leai_32_n 1u
/* CLZ and CTZ: none, mach.h's compositions (a machine with its own defines RK_CLZ and gives rk_clz) */
static const SgXSel sgx_sel_clz_64[1] = {RK_SEL(rev_64, 1, -1, 1, 0, 0, '-', 0, 0, 1, 0)};
static const SgXSel sgx_sel_clz_32[1] = {RK_SEL(rev_32, 1, -1, 1, 0, 0, '-', 0, 0, 0, 0)};
enum { sgx_sel_clz_64_n = 0, sgx_sel_clz_32_n = 0 };
#define sgx_sel_ctz_64 sgx_sel_clz_64
#define sgx_sel_ctz_32 sgx_sel_clz_32
enum { sgx_sel_ctz_64_n = 0, sgx_sel_ctz_32_n = 0 };
/* extensions: of a register, or a load of memory operand 1 */
#define RK_EXT(name, e, w, co)                                                                                         \
    static const SgXSel sgx_sel_##name##_##w[1] = {{.enc = rk_e_##e, .rm = 1, .in = {1, -1, -1, -1}, .out = 0,       \
                                                    .clean_out = co, .flag_of = {0, 1, 2, 3}, .cc_of = RK_CC(0)}};
RK_EXT(zext8, zext8, 32, 1)
RK_EXT(zext8, zext8, 64, 1)
RK_EXT(zext16, zext16, 32, 1)
RK_EXT(zext16, zext16, 64, 1)
RK_EXT(zext32, zext32, 64, 1)
RK_EXT(sext8, sext8, 64, 1)
RK_EXT(sext8, sext8, 32, 0)
RK_EXT(sext16, sext16, 64, 1)
RK_EXT(sext16, sext16, 32, 0)
RK_EXT(sext32, sext32, 64, 1)
RK_NS(zext8)
RK_NS(zext16)
RK_N1(zext32, 64)
RK_NS(sext8)
RK_NS(sext16)
RK_N1(sext32, 64)
/* moves: the first a load where operand 1 is memory (zero extending), the second a store where operand 0 is */
#define RK_MOV(w, e)                                                                                                   \
    static const SgXSel sgx_sel_mov_##w[2] = {                                                                         \
        {.enc = rk_e_##e, .rm = 1, .in = {1, -1, -1, -1}, .out = 0, .clean_out = 1, .flag_of = {0, 1, 2, 3},          \
         .cc_of = RK_CC(0)},                                                                                           \
        {.enc = rk_e_##e, .rm = 0, .in = {1, -1, -1, -1}, .out = 0, .clean_out = 1, .flag_of = {0, 1, 2, 3},          \
         .cc_of = RK_CC(0)}};                                                                                          \
    enum { sgx_sel_mov_##w##_n = 2 };
RK_MOV(64, mov_64)
RK_MOV(32, mov_32)
RK_MOV(16, mov_16)
RK_MOV(8, mov_8)
static const SgXSel sgx_sel_movi_64[1] = {{.enc = rk_e_movi_64, .rm = -1, .in = {-1, -1, -1, -1}, .imm = 1, .out = 0,
                                          .clean_out = 1, .flag_of = {0, 1, 2, 3}, .cc_of = RK_CC(0)}};
static const SgXSel sgx_sel_movi_32[1] = {{.enc = rk_e_movi_32, .rm = -1, .in = {-1, -1, -1, -1}, .imm = 1, .out = 0,
                                          .clean_out = 1, .flag_of = {0, 1, 2, 3}, .cc_of = RK_CC(0)}};
#define sgx_sel_movi_s32_64 sgx_sel_movi_64
enum { sgx_sel_movi_64_n = 1, sgx_sel_movi_32_n = 1, sgx_sel_movi_s32_64_n = 0 };
static const SgXSel sgx_sel_csel_64[1] = {RK_SEL(csel_64, 1, 2, 0, 0, 0, '-', 0, 0, 1, 0)};
static const SgXSel sgx_sel_csel_32[1] = {RK_SEL(csel_32, 1, 2, 0, 0, 0, '-', 0, 0, 0, 0)};
static const SgXSel sgx_sel_cset_64[1] = {{.enc = rk_e_cset_64, .rm = -1, .in = {-1, -1, -1, -1}, .out = 0,
                                          .clean_out = 1, .flag_of = {0, 1, 2, 3}, .cc_of = RK_CC(0)}};
static const SgXSel sgx_sel_cset_32[1] = {{.enc = rk_e_cset_32, .rm = -1, .in = {-1, -1, -1, -1}, .out = 0,
                                          .clean_out = 1, .flag_of = {0, 1, 2, 3}, .cc_of = RK_CC(0)}};
RK_NS(csel)
RK_NS(cset)

/* ---- vector forms: none (SGX_HAS_VREGS 0), each a failure where something asks for one ---- */
static void rk_v_none(SgXBuf* b, const unsigned char* r, long long imm, int cc) {
    (void)b, (void)r, (void)imm, (void)cc;
    sg_fail("a vector form on a machine without vector registers", 0, 0);
}
static const SgXVForm sgx_v_none = {rk_v_none, 0, 2, 0, 1, 2, 1, 0, NULL};
#define SGX_VF_LD16 (&sgx_v_none)
#define SGX_VF_ST16 (&sgx_v_none)
#define SGX_VF_ZERO (&sgx_v_none)
#define SGX_VF_X2G32 (&sgx_v_none)
#define SGX_VF_X2G64 (&sgx_v_none)
#define SGX_VF_G2X32 (&sgx_v_none)
#define SGX_VF_G2X64 (&sgx_v_none)
#define SGX_VF_XHI2G (&sgx_v_none)
#define SGX_VF_INS64 (&sgx_v_none)
#define SGX_VF_LD8 (&sgx_v_none)
#define SGX_VF_ST8 (&sgx_v_none)
#define SGX_VF_LD4 (&sgx_v_none)
#define SGX_VF_ST4 (&sgx_v_none)
