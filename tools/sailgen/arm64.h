/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's arm64 back end: the host's own A64, after machdef.h and before mach.h (README.md, "The arm64 back end"),
   which the guest's part follows (arm64_asm.h, then arm64_a64.h). Every word it writes is made by an encoder of the
   A64 Sail's decode clauses (a64_enc.inc, sailgen --encoders): the same Sail the guest's emitters are generated from,
   so nothing about an instruction's bits is written here. What is written here is which instruction each of
   mach.h's primitives is, and those are their own names: mach.h's primitives are the Arm ARM's definitions (x64.ops
   writes them from AddWithCarry, ShiftReg, CountLeadingZeroBits), so SUBS is SUBS (shifted register) and HI is HI.
   The host's flags are the guest's NZCV (each condition its own code), its division is Arm's, and its immediates are
   A64's: an addition's imm12 (or imm12 shifted by 12) of the value or its negation, a logical operation's bitmask
   (DecodeBitMasks' inverse, sga_bitmask). The tests run what it writes (sail_arm64, under Unicorn on other hosts).

   Registers: X0-X15 hold values (X0 and X1 last: a call's results); X16 and X17 are the assembler's own (an
   address or a constant it has to make, a flag's byte); X18 is the platform's on Apple and Windows and is never
   touched; X19 holds the state's address; X20-X28 the pinned guest registers and the guest memory's base; X29 and
   X30 the frame and the link; SP the spill slots. V0-V7 and V16-V31 hold vector values, every one caller-saved; V8-V15
   hold none, as their low halves are the callee's to keep (SGX_XCALLEE) */

#include "a64_enc.inc"

static inline void sga_w(SgXBuf* b, SnxU32 w) { sgx_imm(b, (long long)w, 4); }

/* the host's features a candidate may need (ID_AA64ISAR0_EL1's): the LSE atomics, CRC32, FEAT_CSSC's CTZ */
enum { SGX_F_LSE = 1, SGX_F_CRC32 = 2, SGX_F_CSSC = 4 };
#define SGX_F_AVX 0 /* no vector form has a VEX twin here */

/* Arm's conditions: the host's own codes, and each flag's (N 0, Z 1, C 2, V 3) set and clear ones */
static const char* const sgx_cc_name[16] = {"EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC",
                                            "HI", "LS", "GE", "LT", "GT", "LE", "AL", "NV"};
static const signed char sgx_flag_cc[4] = {4, 0, 2, 6};
static const signed char sgx_flag_ncc[4] = {5, 1, 3, 7};
enum { SGX_ARM_EQ, SGX_ARM_NE, SGX_ARM_CS, SGX_ARM_CC, SGX_ARM_MI, SGX_ARM_PL, SGX_ARM_VS, SGX_ARM_VC, SGX_ARM_HI,
       SGX_ARM_LS, SGX_ARM_GE, SGX_ARM_LT, SGX_ARM_GT, SGX_ARM_LE, SGX_ARM_COUNT };

/* ---- what mach.h asks of the machine ---- */
#define SGX_NREG 32
#define SGX_NXREG 32
#define SGX_FIX 32
#define SGX_MEMR(r) (64 + (r))
#define SGX_XPIN 32
#define SGX_RET 0
#define SGX_RET2 1
#define SGX_F_GPR 0
#define SGX_CARRY 2
#define SGX_DIV_ARM 1
#define SGX_FP_HELPERS 1 /* mach_a64.h's floating point */
/* no vector operand is implicit: every one is a field's */
#define SGX_IMPLICIT_X0 0
#define SGX_FRAME 31 /* SP, as a load's or a store's base */
#define SGA_X16 16
#define SGA_X17 17
#define SGA_ZR 31
static const signed char sgx_pool[] = {9, 10, 11, 12, 13, 14, 15, 8, 7, 6, 5, 4, 3, 2, 1, 0};
#define SGX_CALLER_SAVED 0xFFFFu /* X0-X15 of the pool; X16 and X17 hold nothing across an instruction */
#define SGX_XCALLEE(a)   0xFF00u /* V8-V15, whose low halves a callee keeps: no prologue saves them, no value has them */

/* ---- immediates ---- */

/* k of w bits as A64's bitmask immediate, DecodeBitMasks(N, imms, immr, TRUE, w)'s wmask inverted: the smallest
   element of 2 to w bits the value repeats, which must be ROR(Ones(S + 1), R) of its esize: rotated right by r it is
   S + 1 ones from bit 0, so R is esize - r; imms is NOT(esize - 1) << 1 with S below it, N whether esize is 64. 0
   where there is none (0 and all ones never are) */
static inline int sga_bitmask(SnxU64 k, unsigned w, unsigned* n, unsigned* immr, unsigned* imms) {
    unsigned e, rot, ones;
    SnxU64 m = w == 64 ? ~0ull : (1ull << w) - 1, el, emask;
    k &= m;
    if (!k || k == m) return 0;
    for (e = 2; e < w; e *= 2) {
        SnxU64 x = k & ((1ull << e) - 1), rep = 0;
        unsigned i;
        for (i = 0; i < w; i += e) rep |= x << i;
        if (rep == k) break;
    }
    emask = e == 64 ? ~0ull : (1ull << e) - 1;
    el = k & emask;
    for (rot = 0; rot < e; ++rot) {
        SnxU64 v = rot ? ((el >> rot) | (el << (e - rot))) & emask : el;
        if (v & (v + 1)) continue;
        for (ones = 0; ones < e && (v >> ones & 1); ++ones) {}
        *n = e == 64;
        *immr = (e - rot) % e;
        *imms = ((~(e - 1) << 1) & 0x3f) | (ones - 1);
        return 1;
    }
    return 0;
}

/* an addition of k (w bits, the primitives' 32-bit immediate sign extended) as ADD or SUB of an imm12, shifted by 12
   or not: 1 and the parts where it is one */
static inline int sga_addimm(SnxU64 k, unsigned w, int* sub, unsigned* imm12, unsigned* sh) {
    long long v = (long long)(int)(SnxU32)k, a;
    SnxU64 u;
    (void)w;
    *sub = v < 0;
    a = v < 0 ? -v : v;
    u = (SnxU64)a;
    if (u < 4096) {
        *imm12 = (unsigned)u;
        *sh = 0;
        return 1;
    }
    if (!(u & 0xfff) && u < (4096ull << 12)) {
        *imm12 = (unsigned)(u >> 12);
        *sh = 1;
        return 1;
    }
    return 0;
}

/* ---- the encoders: r[] the operands' registers (64 + a base register for memory: SGX_MEMR, with b->index and
   b->disp), imm the immediate, cc the condition ---- */

#define SGA_E(name) static void sga_e_##name(SgXBuf* b, const unsigned char* r, long long imm, int cc)
#define SGA_ISMEM(v) ((v) >= 64)

SGA_E(add_64) { (void)imm, (void)cc; sga_w(b, sge_a64_add_addsub_shift(r[0], r[1], 0, r[2], 0, 1)); }
SGA_E(add_32) { (void)imm, (void)cc; sga_w(b, sge_a64_add_addsub_shift(r[0], r[1], 0, r[2], 0, 0)); }
SGA_E(sub_64) { (void)imm, (void)cc; sga_w(b, sge_a64_sub_addsub_shift(r[0], r[1], 0, r[2], 0, 1)); }
SGA_E(sub_32) { (void)imm, (void)cc; sga_w(b, sge_a64_sub_addsub_shift(r[0], r[1], 0, r[2], 0, 0)); }
SGA_E(adds_64) { (void)imm, (void)cc; sga_w(b, sge_a64_adds_addsub_shift(r[0], r[1], 0, r[2], 0, 1)); }
SGA_E(adds_32) { (void)imm, (void)cc; sga_w(b, sge_a64_adds_addsub_shift(r[0], r[1], 0, r[2], 0, 0)); }
SGA_E(subs_64) { (void)imm, (void)cc; sga_w(b, sge_a64_subs_addsub_shift(r[0], r[1], 0, r[2], 0, 1)); }
SGA_E(subs_32) { (void)imm, (void)cc; sga_w(b, sge_a64_subs_addsub_shift(r[0], r[1], 0, r[2], 0, 0)); }
SGA_E(cmp_64) { (void)imm, (void)cc; sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, r[1], 0, r[2], 0, 1)); }
SGA_E(cmp_32) { (void)imm, (void)cc; sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, r[1], 0, r[2], 0, 0)); }
SGA_E(adcs_64) { (void)imm, (void)cc; sga_w(b, sge_a64_adcs(r[0], r[1], r[2], 1)); }
SGA_E(adcs_32) { (void)imm, (void)cc; sga_w(b, sge_a64_adcs(r[0], r[1], r[2], 0)); }
SGA_E(sbcs_64) { (void)imm, (void)cc; sga_w(b, sge_a64_sbcs(r[0], r[1], r[2], 1)); }
SGA_E(sbcs_32) { (void)imm, (void)cc; sga_w(b, sge_a64_sbcs(r[0], r[1], r[2], 0)); }
SGA_E(neg_64) { (void)imm, (void)cc; sga_w(b, sge_a64_sub_addsub_shift(r[0], SGA_ZR, 0, r[1], 0, 1)); }
SGA_E(neg_32) { (void)imm, (void)cc; sga_w(b, sge_a64_sub_addsub_shift(r[0], SGA_ZR, 0, r[1], 0, 0)); }
SGA_E(not_64) { (void)imm, (void)cc; sga_w(b, sge_a64_orn_log_shift(r[0], SGA_ZR, 0, r[1], 0, 1)); }
SGA_E(not_32) { (void)imm, (void)cc; sga_w(b, sge_a64_orn_log_shift(r[0], SGA_ZR, 0, r[1], 0, 0)); }
SGA_E(and_64) { (void)imm, (void)cc; sga_w(b, sge_a64_and_log_shift(r[0], r[1], 0, r[2], 0, 1)); }
SGA_E(and_32) { (void)imm, (void)cc; sga_w(b, sge_a64_and_log_shift(r[0], r[1], 0, r[2], 0, 0)); }
SGA_E(or_64) { (void)imm, (void)cc; sga_w(b, sge_a64_orr_log_shift(r[0], r[1], 0, r[2], 0, 1)); }
SGA_E(or_32) { (void)imm, (void)cc; sga_w(b, sge_a64_orr_log_shift(r[0], r[1], 0, r[2], 0, 0)); }
SGA_E(xor_64) { (void)imm, (void)cc; sga_w(b, sge_a64_eor_log_shift(r[0], r[1], 0, r[2], 0, 1)); }
SGA_E(xor_32) { (void)imm, (void)cc; sga_w(b, sge_a64_eor_log_shift(r[0], r[1], 0, r[2], 0, 0)); }
SGA_E(bic_64) { (void)imm, (void)cc; sga_w(b, sge_a64_bic_log_shift(r[0], r[1], 0, r[2], 0, 1)); }
SGA_E(bic_32) { (void)imm, (void)cc; sga_w(b, sge_a64_bic_log_shift(r[0], r[1], 0, r[2], 0, 0)); }
SGA_E(ands_64) { (void)imm, (void)cc; sga_w(b, sge_a64_ands_log_shift(r[0], r[1], 0, r[2], 0, 1)); }
SGA_E(ands_32) { (void)imm, (void)cc; sga_w(b, sge_a64_ands_log_shift(r[0], r[1], 0, r[2], 0, 0)); }
SGA_E(tst_64) { (void)imm, (void)cc; sga_w(b, sge_a64_ands_log_shift(SGA_ZR, r[1], 0, r[2], 0, 1)); }
SGA_E(tst_32) { (void)imm, (void)cc; sga_w(b, sge_a64_ands_log_shift(SGA_ZR, r[1], 0, r[2], 0, 0)); }
SGA_E(mul_64) { (void)imm, (void)cc; sga_w(b, sge_a64_madd(r[0], r[1], SGA_ZR, r[2], 1)); }
SGA_E(mul_32) { (void)imm, (void)cc; sga_w(b, sge_a64_madd(r[0], r[1], SGA_ZR, r[2], 0)); }
SGA_E(umulh_64) { (void)imm, (void)cc; sga_w(b, sge_a64_umulh(r[0], r[1], r[2])); }
SGA_E(smulh_64) { (void)imm, (void)cc; sga_w(b, sge_a64_smulh(r[0], r[1], r[2])); }
SGA_E(udiv_64) { (void)imm, (void)cc; sga_w(b, sge_a64_udiv(r[0], r[1], r[2], 1)); }
SGA_E(sdiv_64) { (void)imm, (void)cc; sga_w(b, sge_a64_sdiv(r[0], r[1], r[2], 1)); }
/* LSL #k is UBFM #(-k MOD w), #(w-1-k); LSR UBFM #k, #(w-1); ASR SBFM #k, #(w-1); ROR EXTR of the register twice */
static inline void sga_shift_i(SgXBuf* b, const unsigned char* r, long long imm, int kind, int sf) {
    unsigned w = sf ? 64 : 32, k = (unsigned)imm & (w - 1);
    if (kind == 0) sga_w(b, sge_a64_ubfm(r[0], r[1], w - 1 - k, (w - k) & (w - 1), (SnxU32)sf, (SnxU32)sf));
    else if (kind == 1)
        sga_w(b, sge_a64_ubfm(r[0], r[1], w - 1, k, (SnxU32)sf, (SnxU32)sf));
    else if (kind == 2)
        sga_w(b, sge_a64_sbfm(r[0], r[1], w - 1, k, (SnxU32)sf, (SnxU32)sf));
    else
        sga_w(b, sge_a64_extr(r[0], r[1], k, r[1], (SnxU32)sf, (SnxU32)sf));
}
SGA_E(lsl_i_64) { (void)cc, sga_shift_i(b, r, imm, 0, 1); }
SGA_E(lsl_i_32) { (void)cc, sga_shift_i(b, r, imm, 0, 0); }
SGA_E(lsr_i_64) { (void)cc, sga_shift_i(b, r, imm, 1, 1); }
SGA_E(lsr_i_32) { (void)cc, sga_shift_i(b, r, imm, 1, 0); }
SGA_E(asr_i_64) { (void)cc, sga_shift_i(b, r, imm, 2, 1); }
SGA_E(asr_i_32) { (void)cc, sga_shift_i(b, r, imm, 2, 0); }
SGA_E(ror_i_64) { (void)cc, sga_shift_i(b, r, imm, 3, 1); }
SGA_E(ror_i_32) { (void)cc, sga_shift_i(b, r, imm, 3, 0); }
SGA_E(lslv_64) { (void)imm, (void)cc; sga_w(b, sge_a64_lslv(r[0], r[1], r[2], 1)); }
SGA_E(lslv_32) { (void)imm, (void)cc; sga_w(b, sge_a64_lslv(r[0], r[1], r[2], 0)); }
SGA_E(lsrv_64) { (void)imm, (void)cc; sga_w(b, sge_a64_lsrv(r[0], r[1], r[2], 1)); }
SGA_E(lsrv_32) { (void)imm, (void)cc; sga_w(b, sge_a64_lsrv(r[0], r[1], r[2], 0)); }
SGA_E(asrv_64) { (void)imm, (void)cc; sga_w(b, sge_a64_asrv(r[0], r[1], r[2], 1)); }
SGA_E(asrv_32) { (void)imm, (void)cc; sga_w(b, sge_a64_asrv(r[0], r[1], r[2], 0)); }
SGA_E(rorv_64) { (void)imm, (void)cc; sga_w(b, sge_a64_rorv(r[0], r[1], r[2], 1)); }
SGA_E(rorv_32) { (void)imm, (void)cc; sga_w(b, sge_a64_rorv(r[0], r[1], r[2], 0)); }
SGA_E(clz_64) { (void)imm, (void)cc; sga_w(b, sge_a64_clz_int(r[0], r[1], 1)); }
SGA_E(clz_32) { (void)imm, (void)cc; sga_w(b, sge_a64_clz_int(r[0], r[1], 0)); }
SGA_E(rev_64) { (void)imm, (void)cc; sga_w(b, sge_a64_rev(r[0], r[1], 3, 1)); }
SGA_E(rev_32) { (void)imm, (void)cc; sga_w(b, sge_a64_rev(r[0], r[1], 2, 0)); }
SGA_E(rbit_64) { (void)imm, (void)cc; sga_w(b, sge_a64_rbit_int(r[0], r[1], 1)); }
SGA_E(rbit_32) { (void)imm, (void)cc; sga_w(b, sge_a64_rbit_int(r[0], r[1], 0)); }

/* a load or a store of a general-purpose register at [base + index + disp] (b's), of bytes 1, 2, 4 or 8: LDR (and
   LDRB, LDRH) of an unsigned offset the size scales, LDUR of a signed 9-bit one, the register offset for an index of
   no displacement, else the address made in X16 first */
static inline void sga_ldst(SgXBuf* b, int load, int bytes, int rt, int base, int index, int disp) {
    unsigned size = bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : 3;
    if (index >= 0 && disp) {
        sga_w(b, sge_a64_add_addsub_shift(SGA_X16, (SnxU32)base, 0, (SnxU32)index, 0, 1));
        base = SGA_X16;
        index = -1;
    }
    if (index >= 0) {
        /* option LSL (UXTX, 011), unscaled (S 0) */
        if (bytes == 1)
            sga_w(b, load ? sge_a64_ldrb_reg((SnxU32)rt, (SnxU32)base, 0, 3, (SnxU32)index)
                          : sge_a64_strb_reg((SnxU32)rt, (SnxU32)base, 0, 3, (SnxU32)index));
        else if (bytes == 2)
            sga_w(b, load ? sge_a64_ldrh_reg((SnxU32)rt, (SnxU32)base, 0, 3, (SnxU32)index)
                          : sge_a64_strh_reg((SnxU32)rt, (SnxU32)base, 0, 3, (SnxU32)index));
        else
            sga_w(b, load ? sge_a64_ldr_reg_gen((SnxU32)rt, (SnxU32)base, 0, 3, (SnxU32)index, size)
                          : sge_a64_str_reg_gen((SnxU32)rt, (SnxU32)base, 0, 3, (SnxU32)index, size));
        return;
    }
    if (disp >= 0 && !(disp & (bytes - 1)) && disp / bytes < 4096) {
        SnxU32 i12 = (SnxU32)(disp / bytes);
        if (bytes == 1)
            sga_w(b, load ? sge_a64_ldrb_imm_immediate_unsigned((SnxU32)rt, (SnxU32)base, i12)
                          : sge_a64_strb_imm_immediate_unsigned((SnxU32)rt, (SnxU32)base, i12));
        else if (bytes == 2)
            sga_w(b, load ? sge_a64_ldrh_imm_immediate_unsigned((SnxU32)rt, (SnxU32)base, i12)
                          : sge_a64_strh_imm_immediate_unsigned((SnxU32)rt, (SnxU32)base, i12));
        else
            sga_w(b, load ? sge_a64_ldr_imm_gen_immediate_unsigned((SnxU32)rt, (SnxU32)base, i12, size)
                          : sge_a64_str_imm_gen_immediate_unsigned((SnxU32)rt, (SnxU32)base, i12, size));
        return;
    }
    if (disp >= -256 && disp < 256) {
        SnxU32 i9 = (SnxU32)disp & 0x1ff;
        if (bytes == 1) sga_w(b, load ? sge_a64_ldurb((SnxU32)rt, (SnxU32)base, i9) : sge_a64_sturb((SnxU32)rt, (SnxU32)base, i9));
        else if (bytes == 2)
            sga_w(b, load ? sge_a64_ldurh((SnxU32)rt, (SnxU32)base, i9) : sge_a64_sturh((SnxU32)rt, (SnxU32)base, i9));
        else
            sga_w(b, load ? sge_a64_ldur_gen((SnxU32)rt, (SnxU32)base, i9, size)
                          : sge_a64_stur_gen((SnxU32)rt, (SnxU32)base, i9, size));
        return;
    }
    /* X17 = the displacement (MOVZ/MOVN and MOVK), the register offset (X16 may be the base) */
    {
        SnxU64 d = (SnxU64)(long long)disp;
        int neg = disp < 0;
        sga_w(b, neg ? sge_a64_movn(SGA_X17, (SnxU32)(~d & 0xffff), 0, 1) : sge_a64_movz(SGA_X17, (SnxU32)(d & 0xffff), 0, 1));
        if (((d >> 16) & 0xffff) != (neg ? 0xffffu : 0u)) sga_w(b, sge_a64_movk(SGA_X17, (SnxU32)((d >> 16) & 0xffff), 1, 1));
        sga_ldst(b, load, bytes, rt, base, SGA_X17, 0);
    }
}
/* a move whose operand rm may be memory (mach.h's memory operand: SGX_MEMR(base), b->index, b->disp): the load or the
   store of that many bytes, else the register's copy (ORR of the zero register) */
static inline void sga_mov_form(SgXBuf* b, const unsigned char* r, int bytes, int sf) {
    if (SGA_ISMEM(r[1])) sga_ldst(b, 1, bytes, r[0], r[1] - 64, b->index, b->disp);
    else if (SGA_ISMEM(r[0]))
        sga_ldst(b, 0, bytes, r[1], r[0] - 64, b->index, b->disp);
    else
        sga_w(b, sge_a64_orr_log_shift(r[0], SGA_ZR, 0, r[1], 0, (SnxU32)sf));
}
SGA_E(mov_64) { (void)imm, (void)cc, sga_mov_form(b, r, 8, 1); }
SGA_E(mov_32) { (void)imm, (void)cc, sga_mov_form(b, r, 4, 0); }
SGA_E(mov_16) { (void)imm, (void)cc, sga_mov_form(b, r, 2, 0); }
SGA_E(mov_8) { (void)imm, (void)cc, sga_mov_form(b, r, 1, 0); }
/* UXTB, UXTH (UBFM #0, #7 and #15; LDRB and LDRH of memory), the 32-bit move that zero extends; SXTB, SXTH, SXTW */
SGA_E(zext8) {
    (void)imm, (void)cc;
    if (SGA_ISMEM(r[1])) sga_ldst(b, 1, 1, r[0], r[1] - 64, b->index, b->disp);
    else
        sga_w(b, sge_a64_ubfm(r[0], r[1], 7, 0, 0, 0));
}
SGA_E(zext16) {
    (void)imm, (void)cc;
    if (SGA_ISMEM(r[1])) sga_ldst(b, 1, 2, r[0], r[1] - 64, b->index, b->disp);
    else
        sga_w(b, sge_a64_ubfm(r[0], r[1], 15, 0, 0, 0));
}
SGA_E(zext32) { (void)imm, (void)cc, sga_mov_form(b, r, 4, 0); }
SGA_E(sext8_64) { (void)imm, (void)cc; sga_w(b, sge_a64_sbfm(r[0], r[1], 7, 0, 1, 1)); }
SGA_E(sext8_32) { (void)imm, (void)cc; sga_w(b, sge_a64_sbfm(r[0], r[1], 7, 0, 0, 0)); }
SGA_E(sext16_64) { (void)imm, (void)cc; sga_w(b, sge_a64_sbfm(r[0], r[1], 15, 0, 1, 1)); }
SGA_E(sext16_32) { (void)imm, (void)cc; sga_w(b, sge_a64_sbfm(r[0], r[1], 15, 0, 0, 0)); }
SGA_E(sext32_64) { (void)imm, (void)cc; sga_w(b, sge_a64_sbfm(r[0], r[1], 31, 0, 1, 1)); }

/* a constant into a register: MOVZ or MOVN of its one halfword that isn't 0 (or all ones), ORR of the zero register
   with a bitmask, else MOVZ (or MOVN) and a MOVK for each other halfword */
static inline void sga_movi(SgXBuf* b, int d, SnxU64 k, int sf) {
    unsigned w = sf ? 64 : 32, hw = w / 16, i, zeros = 0, ones = 0, n, immr, imms, first = 1;
    int inv;
    if (!sf) k &= 0xffffffffull;
    for (i = 0; i < hw; ++i) {
        unsigned h = (unsigned)(k >> (16 * i)) & 0xffff;
        zeros += h == 0;
        ones += h == 0xffff;
    }
    if (zeros >= hw - 1) {
        for (i = 0; i < hw && !((k >> (16 * i)) & 0xffff); ++i) {}
        if (i == hw) i = 0;
        sga_w(b, sge_a64_movz((SnxU32)d, (SnxU32)((k >> (16 * i)) & 0xffff), i, (SnxU32)sf));
        return;
    }
    if (ones >= hw - 1) {
        for (i = 0; i < hw && ((k >> (16 * i)) & 0xffff) == 0xffff; ++i) {}
        if (i == hw) i = 0;
        sga_w(b, sge_a64_movn((SnxU32)d, (SnxU32)(~(k >> (16 * i)) & 0xffff), i, (SnxU32)sf));
        return;
    }
    if (sga_bitmask(k, w, &n, &immr, &imms)) {
        sga_w(b, sge_a64_orr_log_imm((SnxU32)d, SGA_ZR, imms, immr, n, (SnxU32)sf));
        return;
    }
    inv = ones > zeros;
    for (i = 0; i < hw; ++i) {
        unsigned h = (unsigned)(k >> (16 * i)) & 0xffff;
        if (h == (inv ? 0xffffu : 0u)) continue;
        if (first) sga_w(b, inv ? sge_a64_movn((SnxU32)d, ~h & 0xffff, i, (SnxU32)sf) : sge_a64_movz((SnxU32)d, h, i, (SnxU32)sf));
        else
            sga_w(b, sge_a64_movk((SnxU32)d, h, i, (SnxU32)sf));
        first = 0;
    }
}
SGA_E(movi_64) { (void)cc, sga_movi(b, r[0], (SnxU64)imm, 1); }
SGA_E(movi_32) { (void)cc, sga_movi(b, r[0], (SnxU64)imm, 0); }

/* ADD and SUB of an immediate (their flags' forms ADDS and SUBS; CMP and CMN to the zero register): the 32-bit
   immediate sign extended, as an imm12 of its magnitude (sga_addimm; sgx_imm_ok made sure). An addition of 0 stays an
   addition and a subtraction of 0 a subtraction: their C differ */
static inline void sga_addsub_i(SgXBuf* b, int d, int n, long long imm, int sf, int sub, int flags) {
    long long v = (long long)(int)(SnxU32)((SnxU64)imm & 0xffffffffull);
    int neg = v < 0, dummy;
    unsigned i12, sh;
    if (!sga_addimm((SnxU64)v, sf ? 64 : 32, &dummy, &i12, &sh)) sg_fail("an immediate no ADD or SUB takes", imm, sf);
    /* x + k with k negative is SUB of -k, and x - k ADD: the same result and, for k not 0, the same NZCV */
    sub ^= neg;
    if (flags)
        sga_w(b, sub ? sge_a64_subs_addsub_imm((SnxU32)d, (SnxU32)n, i12, sh, (SnxU32)sf)
                     : sge_a64_adds_addsub_imm((SnxU32)d, (SnxU32)n, i12, sh, (SnxU32)sf));
    else
        sga_w(b, sub ? sge_a64_sub_addsub_imm((SnxU32)d, (SnxU32)n, i12, sh, (SnxU32)sf)
                     : sge_a64_add_addsub_imm((SnxU32)d, (SnxU32)n, i12, sh, (SnxU32)sf));
}
SGA_E(add_i_64) { (void)cc, sga_addsub_i(b, r[0], r[1], imm, 1, 0, 0); }
SGA_E(add_i_32) { (void)cc, sga_addsub_i(b, r[0], r[1], imm, 0, 0, 0); }
SGA_E(adds_i_64) { (void)cc, sga_addsub_i(b, r[0], r[1], imm, 1, 0, 1); }
SGA_E(adds_i_32) { (void)cc, sga_addsub_i(b, r[0], r[1], imm, 0, 0, 1); }
SGA_E(subs_i_64) { (void)cc, sga_addsub_i(b, r[0], r[1], imm, 1, 1, 1); }
SGA_E(subs_i_32) { (void)cc, sga_addsub_i(b, r[0], r[1], imm, 0, 1, 1); }
SGA_E(cmp_i_64) { (void)cc, sga_addsub_i(b, SGA_ZR, r[1], imm, 1, 1, 1); }
SGA_E(cmp_i_32) { (void)cc, sga_addsub_i(b, SGA_ZR, r[1], imm, 0, 1, 1); }
/* AND, ORR and EOR of a bitmask immediate */
static inline void sga_logic_i(SgXBuf* b, const unsigned char* r, long long imm, int op, int sf) {
    unsigned n, immr, imms;
    if (!sga_bitmask((SnxU64)imm, sf ? 64 : 32, &n, &immr, &imms)) sg_fail("an immediate no bitmask encodes", imm, sf);
    sga_w(b, op == 0   ? sge_a64_and_log_imm(r[0], r[1], imms, immr, n, (SnxU32)sf)
             : op == 1 ? sge_a64_orr_log_imm(r[0], r[1], imms, immr, n, (SnxU32)sf)
                       : sge_a64_eor_log_imm(r[0], r[1], imms, immr, n, (SnxU32)sf));
}
SGA_E(and_i_64) { (void)cc, sga_logic_i(b, r, imm, 0, 1); }
SGA_E(and_i_32) { (void)cc, sga_logic_i(b, r, imm, 0, 0); }
SGA_E(or_i_64) { (void)cc, sga_logic_i(b, r, imm, 1, 1); }
SGA_E(or_i_32) { (void)cc, sga_logic_i(b, r, imm, 1, 0); }
SGA_E(xor_i_64) { (void)cc, sga_logic_i(b, r, imm, 2, 1); }
SGA_E(xor_i_32) { (void)cc, sga_logic_i(b, r, imm, 2, 0); }
/* CSEL of the condition; CSET is CSINC of the zero register under the inverse */
SGA_E(csel_64) { (void)imm; sga_w(b, sge_a64_csel(r[0], r[1], (SnxU32)cc, r[2], 1)); }
SGA_E(csel_32) { (void)imm; sga_w(b, sge_a64_csel(r[0], r[1], (SnxU32)cc, r[2], 0)); }
SGA_E(cset_64) { (void)imm; sga_w(b, sge_a64_csinc(r[0], SGA_ZR, (SnxU32)cc ^ 1, SGA_ZR, 1)); }
SGA_E(cset_32) { (void)imm; sga_w(b, sge_a64_csinc(r[0], SGA_ZR, (SnxU32)cc ^ 1, SGA_ZR, 0)); }

/* ---- the selection: each primitive's one candidate (README.md, "The arm64 back end") ----
   operand 0 the result, 1 and 2 the inputs; every result zero extended (a W register's write clears the top half);
   NZCV the host's own (flag_of identity). An input is read whole (clean: mach.h zero extends a narrower value first)
   where the result's bits, or the flags, depend on its bits above the value's: a compare's and the flags' forms', a
   shift's or rotation's to the right, a division's, CLZ's and REV's, the high halves of a product; not an addition's,
   a logical operation's or a shift's to the left, whose low bits are of the inputs' low bits alone */
#define SGA_CC {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, -1, -1}
#define SGA_SEL(e, i0, i1, cl, im, fb, fk, o, fl)                                                                     \
    {.enc = sga_e_##e, .rm = -1, .in = {i0, i1, -1, -1}, .clean = cl, .imm = im, .fit_bits = fb, .fit_kind = fk,    \
     .out = o, .clean_out = 1, .flags = fl, .flag_of = {0, 1, 2, 3}, .cc_of = SGA_CC}
#define SGA_RR(name, w, fl, cl) static const SgXSel sgx_sel_##name##_##w[1] = {SGA_SEL(name##_##w, 1, 2, cl, 0, 0, '-', 0, fl)};
#define SGA_R(name, w, cl) static const SgXSel sgx_sel_##name##_##w[1] = {SGA_SEL(name##_##w, 1, -1, cl, 0, 0, '-', 0, 0)};
#define SGA_RI(name, w, fb, fk, fl, cl) static const SgXSel sgx_sel_##name##_##w[1] = {SGA_SEL(name##_##w, 1, -1, cl, 2, fb, fk, 0, fl)};
#define SGA_N1(name, w) enum { sgx_sel_##name##_##w##_n = 1 };
#define SGA_NS(name) SGA_N1(name, 64) SGA_N1(name, 32)
#define SGA_PAIR(m, name, ...) m(name, 64 __VA_OPT__(,) __VA_ARGS__) m(name, 32 __VA_OPT__(,) __VA_ARGS__) SGA_NS(name)

SGA_PAIR(SGA_RR, add, 0, 0)
SGA_PAIR(SGA_RR, sub, 0, 0)
SGA_PAIR(SGA_RR, adds, 0xF, 3)
SGA_PAIR(SGA_RR, subs, 0xF, 3)
SGA_PAIR(SGA_RR, adcs, 0xF, 3)
SGA_PAIR(SGA_RR, sbcs, 0xF, 3)
SGA_PAIR(SGA_RR, and, 0, 0)
SGA_PAIR(SGA_RR, or, 0, 0)
SGA_PAIR(SGA_RR, xor, 0, 0)
SGA_PAIR(SGA_RR, bic, 0, 0)
SGA_PAIR(SGA_RR, ands, 0xF, 3)
SGA_PAIR(SGA_RR, mul, 0, 0)
SGA_PAIR(SGA_R, neg, 0)
SGA_PAIR(SGA_R, not, 0)
SGA_PAIR(SGA_R, clz, 1)
SGA_PAIR(SGA_R, rev, 1)
SGA_PAIR(SGA_R, rbit, 1)
SGA_RR(umulh, 64, 0, 3)
SGA_RR(smulh, 64, 0, 3)
SGA_RR(udiv, 64, 0, 3)
SGA_RR(sdiv, 64, 0, 3)
SGA_N1(umulh, 64)
SGA_N1(smulh, 64)
SGA_N1(udiv, 64)
SGA_N1(sdiv, 64)
/* the flags alone: CMP and TST, to the zero register */
static const SgXSel sgx_sel_cmp_64[1] = {SGA_SEL(cmp_64, 1, 2, 3, 0, 0, '-', -1, 0xF)};
static const SgXSel sgx_sel_cmp_32[1] = {SGA_SEL(cmp_32, 1, 2, 3, 0, 0, '-', -1, 0xF)};
static const SgXSel sgx_sel_tst_64[1] = {SGA_SEL(tst_64, 1, 2, 3, 0, 0, '-', -1, 0xF)};
static const SgXSel sgx_sel_tst_32[1] = {SGA_SEL(tst_32, 1, 2, 3, 0, 0, '-', -1, 0xF)};
static const SgXSel sgx_sel_cmp_i_64[1] = {SGA_SEL(cmp_i_64, 1, -1, 1, 2, 0, '-', -1, 0xF)};
static const SgXSel sgx_sel_cmp_i_32[1] = {SGA_SEL(cmp_i_32, 1, -1, 1, 2, 0, '-', -1, 0xF)};
SGA_N1(cmp, 64)
SGA_N1(cmp, 32)
SGA_N1(tst, 64)
SGA_N1(tst, 32)
SGA_N1(cmp_i, 64)
SGA_N1(cmp_i, 32)
/* shifts by a constant (its bits 'U'), by a register (its amount MOD w: the low bits alone read) */
SGA_RI(lsl_i, 64, 6, 'U', 0, 0)
SGA_RI(lsl_i, 32, 5, 'U', 0, 0)
SGA_RI(lsr_i, 64, 6, 'U', 0, 1)
SGA_RI(lsr_i, 32, 5, 'U', 0, 1)
SGA_RI(asr_i, 64, 6, 'U', 0, 1)
SGA_RI(asr_i, 32, 5, 'U', 0, 1)
SGA_RI(ror_i, 64, 6, 'U', 0, 1)
SGA_RI(ror_i, 32, 5, 'U', 0, 1)
SGA_NS(lsl_i)
SGA_NS(lsr_i)
SGA_NS(asr_i)
SGA_NS(ror_i)
#define SGA_V(name, w, cl) static const SgXSel sgx_sel_##name##_##w[1] = {SGA_SEL(name##_##w, 1, 2, cl, 0, 0, '-', 0, 0)};
SGA_PAIR(SGA_V, lslv, 0)
SGA_PAIR(SGA_V, lsrv, 1)
SGA_PAIR(SGA_V, asrv, 1)
SGA_PAIR(SGA_V, rorv, 1)
/* immediates: an addition's (and its flags'), a logical operation's bitmask */
SGA_PAIR(SGA_RI, add_i, 0, '-', 0, 0)
SGA_PAIR(SGA_RI, adds_i, 0, '-', 0xF, 1)
SGA_PAIR(SGA_RI, subs_i, 0, '-', 0xF, 1)
SGA_PAIR(SGA_RI, and_i, 0, '-', 0, 0)
SGA_PAIR(SGA_RI, or_i, 0, '-', 0, 0)
SGA_PAIR(SGA_RI, xor_i, 0, '-', 0, 0)
/* the widths a primitive has none of (mach.h's SGX_TABLE names both) */
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
/* LEA's place (mach.h): an addition that leaves the flags, which every A64 ADD is */
#define sgx_sel_lea_64 sgx_sel_add_64
#define sgx_sel_lea_32 sgx_sel_add_32
#define sgx_sel_leai_64 sgx_sel_add_i_64
#define sgx_sel_leai_32 sgx_sel_add_i_32
#define sgx_sel_lea_64_n 1u
#define sgx_sel_lea_32_n 1u
#define sgx_sel_leai_64_n 1u
#define sgx_sel_leai_32_n 1u
/* no CTZ without FEAT_CSSC: none (mach.h's compositions) */
static const SgXSel sgx_sel_ctz_64[1] = {SGA_SEL(clz_64, 1, -1, 1, 0, 0, '-', 0, 0)};
static const SgXSel sgx_sel_ctz_32[1] = {SGA_SEL(clz_32, 1, -1, 0, 0, 0, '-', 0, 0)};
enum { sgx_sel_ctz_64_n = 0, sgx_sel_ctz_32_n = 0 };
/* extensions: of a register, or (the zero extensions) a load of memory operand 1 */
static const SgXSel sgx_sel_zext8_32[1] = {{.enc = sga_e_zext8, .rm = 1, .in = {1, -1, -1, -1}, .out = 0, .clean_out = 1,
                                           .flag_of = {0, 1, 2, 3}, .cc_of = SGA_CC}};
static const SgXSel sgx_sel_zext8_64[1] = {{.enc = sga_e_zext8, .rm = 1, .in = {1, -1, -1, -1}, .out = 0, .clean_out = 1,
                                           .flag_of = {0, 1, 2, 3}, .cc_of = SGA_CC}};
static const SgXSel sgx_sel_zext16_32[1] = {{.enc = sga_e_zext16, .rm = 1, .in = {1, -1, -1, -1}, .out = 0,
                                            .clean_out = 1, .flag_of = {0, 1, 2, 3}, .cc_of = SGA_CC}};
static const SgXSel sgx_sel_zext16_64[1] = {{.enc = sga_e_zext16, .rm = 1, .in = {1, -1, -1, -1}, .out = 0,
                                            .clean_out = 1, .flag_of = {0, 1, 2, 3}, .cc_of = SGA_CC}};
static const SgXSel sgx_sel_zext32_64[1] = {{.enc = sga_e_zext32, .rm = 1, .in = {1, -1, -1, -1}, .out = 0,
                                            .clean_out = 1, .flag_of = {0, 1, 2, 3}, .cc_of = SGA_CC}};
SGA_NS(zext8)
SGA_NS(zext16)
SGA_N1(zext32, 64)
SGA_PAIR(SGA_R, sext8, 0)
SGA_PAIR(SGA_R, sext16, 0)
SGA_R(sext32, 64, 0)
SGA_N1(sext32, 64)
/* moves: the first a load where operand 1 is memory, the second a store where operand 0 is; a byte's and a
   halfword's only stores */
#define SGA_MOV(w, e)                                                                                                  \
    static const SgXSel sgx_sel_mov_##w[2] = {                                                                         \
        {.enc = sga_e_##e, .rm = 1, .in = {1, -1, -1, -1}, .out = 0, .clean_out = 1,                                   \
         .flag_of = {0, 1, 2, 3}, .cc_of = SGA_CC},                                                                    \
        {.enc = sga_e_##e, .rm = 0, .in = {1, -1, -1, -1}, .out = 0, .clean_out = 1,                                   \
         .flag_of = {0, 1, 2, 3}, .cc_of = SGA_CC}};                                                                   \
    enum { sgx_sel_mov_##w##_n = 2 };
SGA_MOV(64, mov_64)
SGA_MOV(32, mov_32)
SGA_MOV(16, mov_16)
SGA_MOV(8, mov_8)
/* constants: any of the width (MOVZ, MOVN, ORR, MOVK); none sign extended from 32 bits apart (movi_64 makes it) */
static const SgXSel sgx_sel_movi_64[1] = {{.enc = sga_e_movi_64, .rm = -1, .in = {-1, -1, -1, -1}, .imm = 1, .out = 0,
                                          .clean_out = 1, .flag_of = {0, 1, 2, 3}, .cc_of = SGA_CC}};
static const SgXSel sgx_sel_movi_32[1] = {{.enc = sga_e_movi_32, .rm = -1, .in = {-1, -1, -1, -1}, .imm = 1, .out = 0,
                                          .clean_out = 1, .flag_of = {0, 1, 2, 3}, .cc_of = SGA_CC}};
#define sgx_sel_movi_s32_64 sgx_sel_movi_64
enum { sgx_sel_movi_64_n = 1, sgx_sel_movi_32_n = 1, sgx_sel_movi_s32_64_n = 0 };
/* CSEL (A if the condition holds, else B) and CSET */
SGA_V(csel, 64, 0)
SGA_V(csel, 32, 0)
static const SgXSel sgx_sel_cset_64[1] = {{.enc = sga_e_cset_64, .rm = -1, .in = {-1, -1, -1, -1}, .out = 0,
                                          .clean_out = 1, .flag_of = {0, 1, 2, 3}, .cc_of = SGA_CC}};
static const SgXSel sgx_sel_cset_32[1] = {{.enc = sga_e_cset_32, .rm = -1, .in = {-1, -1, -1, -1}, .out = 0,
                                          .clean_out = 1, .flag_of = {0, 1, 2, 3}, .cc_of = SGA_CC}};
SGA_NS(csel)
SGA_NS(cset)

/* ---- vector forms ----
   mach.h's front makes vector values with these (SGX_VF_*: operand 0 the result, 1 the source; a memory operand
   SGX_MEMR(base) with b->index and b->disp, or the constant pool's SGX_POOLR, a literal load whose imm19 the pool
   patches), and arm64_asm.h's hooks with the others. reg, rm and vvvv are x86's ModRM roles: 0, 1 and 2 here, so
   mach.h's assembly finds no implicit operand */
#define SGX_POOLR 63
/* LDR and STR of a Q (16 bytes) or D (8) register at [base + index + disp]: an unsigned offset the size scales, else
   the address in X16 first */
static inline void sga_fp_ldst(SgXBuf* b, int load, int bytes, int rt, int base, int index, int disp) {
    SnxU32 size = bytes == 16 ? 0 : bytes == 8 ? 3 : 2, opc = bytes == 16 ? (load ? 3 : 2) : (load ? 1 : 0);
    if (index >= 0) {
        sga_w(b, sge_a64_add_addsub_shift(SGA_X16, (SnxU32)base, 0, (SnxU32)index, 0, 1));
        base = SGA_X16;
    }
    if (disp < 0 || disp & (bytes - 1) || disp / bytes >= 4096) {
        /* X16 = base + disp (a displacement an ADD of 12 bits shifted or not takes, else the constant made) */
        int sub;
        unsigned i12, sh;
        if (sga_addimm((SnxU64)(long long)disp, 64, &sub, &i12, &sh))
            sga_w(b, sub ? sge_a64_sub_addsub_imm(SGA_X16, (SnxU32)base, i12, sh, 1)
                         : sge_a64_add_addsub_imm(SGA_X16, (SnxU32)base, i12, sh, 1));
        else {
            sga_movi(b, SGA_X17, (SnxU64)(long long)disp, 1);
            sga_w(b, base == 31 ? sge_a64_add_addsub_ext(SGA_X16, 31, 0, 3, SGA_X17, 1)
                                : sge_a64_add_addsub_shift(SGA_X16, (SnxU32)base, 0, SGA_X17, 0, 1));
        }
        base = SGA_X16;
        disp = 0;
    }
    sga_w(b, load ? sge_a64_ldr_imm_fpsimd_immediate_unsigned((SnxU32)rt, (SnxU32)base, (SnxU32)(disp / bytes), opc, size)
                  : sge_a64_str_imm_fpsimd_immediate_unsigned((SnxU32)rt, (SnxU32)base, (SnxU32)(disp / bytes), opc, size));
}
#define SGA_VE(name) static void sga_v_##name(SgXBuf* b, const unsigned char* r, long long imm, int cc)
SGA_VE(ld16) {
    (void)imm, (void)cc;
    if (r[1] == SGX_POOLR) {
        b->rip_at = b->n;
        sga_w(b, sge_a64_ldr_lit_fpsimd(r[0], 0, 2));
    } else
        sga_fp_ldst(b, 1, 16, r[0], r[1] - 64, b->index, b->disp);
}
SGA_VE(st16) { (void)imm, (void)cc, sga_fp_ldst(b, 0, 16, r[1], r[0] - 64, b->index, b->disp); }
/* LDR and STR of a D or S register (a load zeroing the rest of it) */
SGA_VE(ld8) { (void)imm, (void)cc, sga_fp_ldst(b, 1, 8, r[0], r[1] - 64, b->index, b->disp); }
SGA_VE(st8) { (void)imm, (void)cc, sga_fp_ldst(b, 0, 8, r[1], r[0] - 64, b->index, b->disp); }
SGA_VE(ld4) { (void)imm, (void)cc, sga_fp_ldst(b, 1, 4, r[0], r[1] - 64, b->index, b->disp); }
SGA_VE(st4) { (void)imm, (void)cc, sga_fp_ldst(b, 0, 4, r[1], r[0] - 64, b->index, b->disp); }
/* MOVI Vd.2D, #0 */
SGA_VE(zero) { (void)imm, (void)cc; sga_w(b, sge_a64_movi_advsimd(r[0], 0, 0, 0, 0, 0, 14, 0, 0, 0, 1, 1)); }
/* FMOV between a general-purpose register and a vector one's low 32 or 64 bits (the rest of it made 0) */
SGA_VE(x2g32) { (void)imm, (void)cc; sga_w(b, sge_a64_fmov_float_gen(r[0], r[1], 6, 0, 0, 0)); }
SGA_VE(x2g64) { (void)imm, (void)cc; sga_w(b, sge_a64_fmov_float_gen(r[0], r[1], 6, 0, 1, 1)); }
SGA_VE(g2x32) { (void)imm, (void)cc; sga_w(b, sge_a64_fmov_float_gen(r[0], r[1], 7, 0, 0, 0)); }
SGA_VE(g2x64) { (void)imm, (void)cc; sga_w(b, sge_a64_fmov_float_gen(r[0], r[1], 7, 0, 1, 1)); }
/* UMOV Xd, Vn.D[imm]; INS Vd.D[imm], Xn */
SGA_VE(xhi2g) { (void)cc; sga_w(b, sge_a64_umov_advsimd(r[0], r[1], (SnxU32)(imm & 1) << 4 | 8, 1)); }
SGA_VE(ins64) { (void)cc; sga_w(b, sge_a64_ins_advsimd_gen(r[0], r[1], (SnxU32)(imm & 1) << 4 | 8)); }
/* DUP of an element to a scalar register (the rest of it 0): imm the element's bit position, 32-bit aligned or 64 */
SGA_VE(down) {
    (void)cc;
    sga_w(b, imm == 64 ? sge_a64_dup_advsimd_elt_sisd(r[0], r[1], 1 << 4 | 8)
                       : sge_a64_dup_advsimd_elt_sisd(r[0], r[1], (SnxU32)(imm / 32) << 3 | 4));
}
/* ZIP1 Vd.2D, Vlo.2D, Vhi.2D: two doublewords */
SGA_VE(concat) { (void)imm, (void)cc; sga_w(b, sge_a64_zip1_advsimd(r[0], r[1], r[2], 3, 1)); }
/* a value's bits imm and up made 0: FMOV Dd or Sd of itself, DUP of its low halfword or byte to a scalar */
SGA_VE(clear) {
    (void)cc;
    if (imm == 64) sga_w(b, sge_a64_fmov_float(r[0], r[1], 1));
    else if (imm == 32)
        sga_w(b, sge_a64_fmov_float(r[0], r[1], 0));
    else if (imm == 16)
        sga_w(b, sge_a64_dup_advsimd_elt_sisd(r[0], r[1], 2));
    else
        sga_w(b, sge_a64_dup_advsimd_elt_sisd(r[0], r[1], 1));
}
#define SGA_VF(name, n, xm) static const SgXVForm sgx_v_##name = {sga_v_##name, 0, n, 0, 1, 2, xm, 0, NULL};
/* a store's register operand is 1 and its memory 0 (x86's MOVDQU m128, xmm), as mach.h's callers take a store */
#define SGA_VFS(name, n, xm) static const SgXVForm sgx_v_##name = {sga_v_##name, 0, n, 1, 0, 2, xm, 0, NULL};
SGA_VF(ld16, 2, 1)
SGA_VFS(st16, 2, 2)
SGA_VF(ld8, 2, 1)
SGA_VFS(st8, 2, 2)
SGA_VF(ld4, 2, 1)
SGA_VFS(st4, 2, 2)
SGA_VF(zero, 2, 1)
SGA_VF(x2g32, 2, 2)
SGA_VF(x2g64, 2, 2)
SGA_VF(g2x32, 2, 1)
SGA_VF(g2x64, 2, 1)
SGA_VF(xhi2g, 2, 2)
SGA_VF(ins64, 2, 1)
SGA_VF(down, 2, 3)
SGA_VF(concat, 3, 7)
SGA_VF(clear, 2, 3)
#define SGX_VF_LD16 (&sgx_v_ld16)
#define SGX_VF_ST16 (&sgx_v_st16)
#define SGX_VF_ZERO (&sgx_v_zero)
#define SGX_VF_X2G32 (&sgx_v_x2g32)
#define SGX_VF_X2G64 (&sgx_v_x2g64)
#define SGX_VF_G2X32 (&sgx_v_g2x32)
#define SGX_VF_G2X64 (&sgx_v_g2x64)
#define SGX_VF_XHI2G (&sgx_v_xhi2g)
#define SGX_VF_INS64 (&sgx_v_ins64)
#define SGX_VF_LD8 (&sgx_v_ld8)
#define SGX_VF_ST8 (&sgx_v_st8)
#define SGX_VF_LD4 (&sgx_v_ld4)
#define SGX_VF_ST4 (&sgx_v_st4)

/* the state's address (X19, the callee's), and a call's arguments from the state's (X0) on */
#define SGX_BASE 19
static const signed char sgx_arg_regs[6] = {0, 1, 2, 3, 4, 5};
/* AArch32's calls' arguments after the state's (X1-X4), and its exclusive store's address, value and the memory's base
   (X1, X2, X3: mach_a32.h's SGX_A32_STREX), X4 and X5 its own */
static const signed char sgx_a32_args[4] = {1, 2, 3, 4};
static const signed char sgx_a32_strex[3] = {1, 2, 3};
#define SGX_A32_STREX_CLOBBER (1u << 4 | 1u << 5)
