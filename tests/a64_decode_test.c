/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The decoder tools/decodetree generates from a64.decode and a64.profile (src/common/cpu/aarch64/decode/): hand-encoded
   words, one or more per argument set and format family, with the op, the arguments, the flow and the features
   they need checked against the Arm ARM (DDI 0487, chapter C4 "A64 Instruction Set Encoding" for the encoding
   tables, C6 and C7 for each instruction's fields); the hint group's first match; feature gating between the
   Switch's target and "max"; the profile's predicates (reserved encodings inside a pattern); branch immediates
   sign-extended and scaled; and the bucket index against a scan of every row. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "a64_decode.h"
#include "a64_decode.inc"

#define SW  SNX_A64_TARGET_SWITCH
#define MAX SNX_A64_TARGET_MAX
#define ANDROID SNX_A64_TARGET_ANDROID

static struct {
    int failures;
    SnxA64Insn d;
} g_test;

static void eq(long long got, long long want, const char* what, int line) {
    if (got == want) return;
    fprintf(stderr, "a64_decode_test.c:%d: %s is %lld, expected %lld\n", line, what, got, want);
    g_test.failures++;
}

/* decodes word for the target, expecting op; the result is in g_test.d for the field checks */
static void dec(uint32_t word, uint64_t target, SnxA64Op op, int line) {
    SnxA64Op got = snx_a64_decode(word, target, &g_test.d);
    if (got != op || g_test.d.op != op) {
        fprintf(stderr, "a64_decode_test.c:%d: 0x%08x decodes as %s, expected %s\n", line, (unsigned)word,
                snx_a64_op_name(got), snx_a64_op_name(op));
        g_test.failures++;
    }
}

#define DEC(word, target, op) dec(word, target, SNX_A64_OP_##op, __LINE__)
#define UNDEF(word, target)   dec(word, target, SNX_A64_OP_UNDEF, __LINE__)
#define A(field, want)        eq(g_test.d.args.field, want, #field, __LINE__)
#define FLOW(want)            eq(g_test.d.flow, SNX_A64_FLOW_##want, "flow", __LINE__)
#define NOP(want)             eq(g_test.d.nop, want, "nop", __LINE__)
#define NEEDS(want)           eq((long long)g_test.d.feature, (long long)(want), "feature", __LINE__)

/* C4.1.2 Data Processing -- Immediate */
static void data_immediate(void) {
    /* ADR x3, #16: op=0 immlo=00 10000 immhi=4 Rd=3 (C6.2.10) */
    DEC(0x10000083u, SW, ADR);
    A(rd, 3), A(imm, 16), FLOW(PLAIN), NOP(0), NEEDS(SNX_A64_FEAT_V8_0);
    /* ADR x0, #-4: immhi all ones, immlo 00 */
    DEC(0x10ffffe0u, SW, ADR);
    A(imm, -4);
    /* ADD x1, x2, #0x123: sf=1 op=0 S=0 100010 sh=0 imm12 Rn Rd (C6.2.5) */
    DEC(0x91048c41u, SW, ADD_i);
    A(sf, 1), A(imm, 0x123), A(rn, 2), A(rd, 1);
    /* ADD x1, x2, #1, LSL #12: sh=1, the immediate already shifted */
    DEC(0x91400441u, SW, ADD_i);
    A(imm, 4096);
    /* ADDG x0, x1, #16, #3: 1 0 0 100011 0 uimm6=1 00 uimm4=3 (C6.2.6), FEAT_MTE */
    UNDEF(0x91810c20u, SW);
    DEC(0x91810c20u, MAX, ADDG_i);
    A(uimm6, 1), A(uimm4, 3), A(rn, 1), A(rd, 0), NEEDS(SNX_A64_FEAT_MTE);
    /* AND x0, x1, #0xff: sf=1 00 100100 N=1 immr=0 imms=000111 (C6.2.12) */
    DEC(0x92401c20u, SW, AND_i);
    A(sf, 1), A(dbm, 0x1007);
    /* N=1 imms=111111: no element size leaves imms below all ones, reserved */
    UNDEF(0x9240fc20u, MAX);
    /* AND w0, w1, #1: sf=0 N=0 immr=0 imms=0 */
    DEC(0x12000020u, SW, AND_i);
    A(sf, 0), A(dbm, 0);
    /* sf=0 N=0 imms=111111: NOT(imms) has no set bit, reserved */
    UNDEF(0x1200fc20u, MAX);
    /* MOVZ x5, #0x1234, LSL #32: sf=1 opc=10 100101 hw=10 (C6.2.248) */
    DEC(0xd2c24685u, SW, MOVZ);
    A(sf, 1), A(hw, 2), A(imm, 0x1234), A(rd, 5);
    /* MOVK with sf=0 and hw=1x is unallocated: @movw_32 fixes bit 22 to 0 */
    UNDEF(0x72c24685u, MAX);
    /* SBFM x0, x1, #2, #5: sf=1 opc=00 100110 N=1 immr imms (C6.2.282) */
    DEC(0x93421420u, SW, SBFM);
    A(sf, 1), A(immr, 2), A(imms, 5);
    /* EXTR x0, x1, x2, #7: sf=1 00 100111 N=1 0 Rm imms (C6.2.120) */
    DEC(0x93c21c20u, SW, EXTR);
    A(sf, 1), A(rm, 2), A(imm, 7);
    /* sf=0 with imms >= 32 is reserved: the 32-bit pattern fixes bit 15 */
    UNDEF(0x13828420u, MAX);
}

/* C4.1.3 Branches, Exception Generating and System instructions */
static void branches(void) {
    /* B #-8: 0 00101 imm26=-2, the offset scaled by 4 (C6.2.26) */
    DEC(0x17fffffeu, SW, B);
    A(imm, -8), FLOW(B);
    /* BL #0x100 (C6.2.35) */
    DEC(0x94000040u, SW, BL);
    A(imm, 0x100), FLOW(BL);
    /* CBNZ w3, #-12: sf=0 011010 op=1 imm19=-3 Rt (C6.2.48) */
    DEC(0x35ffffa3u, SW, CBZ);
    A(sf, 0), A(nz, 1), A(rt, 3), A(imm, -12), FLOW(BC);
    /* TBNZ x5, #40, #8: b5=1 011011 op=1 b40=01000 imm14=2 (C6.2.372) */
    DEC(0xb7400045u, SW, TBZ);
    A(nz, 1), A(bitpos, 40), A(imm, 8), A(rt, 5), FLOW(BC);
    /* B.NE #0x40: 0101010 0 imm19=0x10 o0=0 cond=0001 (C6.2.27) */
    DEC(0x54000201u, SW, B_cond);
    A(cond, 1), A(c, 0), A(imm, 0x40), FLOW(BC), NEEDS(SNX_A64_FEAT_V8_0);
    /* BC.NE: o0=1 needs FEAT_HBC */
    UNDEF(0x54000211u, SW);
    DEC(0x54000211u, MAX, B_cond);
    A(c, 1), NEEDS(SNX_A64_FEAT_V8_0 | SNX_A64_FEAT_HBC);
    /* CBGT x1, x2, #-8: sf=1 1110100 cc=000 Rm 00 imm9=-2 Rt (CB<cc> (register), FEAT_CMPBR) */
    UNDEF(0xf4023fc1u, SW);
    DEC(0xf4023fc1u, MAX, CB_cond);
    A(cc, 0), A(rm, 2), A(rt, 1), A(esz, 3), A(imm, -8), FLOW(BC);
    /* cc=100 is reserved */
    UNDEF(0xf4823fc1u, MAX);
    /* CBEQ w7, #9, #12: sf=0 1110101 cc=110 imm6=9 0 imm9=3 Rt (CB<cc> (immediate)) */
    DEC(0x75c48067u, MAX, CB_cond_imm);
    A(sf, 0), A(cc, 6), A(imm6, 9), A(imm9, 12), A(rt, 7), FLOW(BC);
    /* BR x17, BLR x8, RET: 1101011 opc 11111 000000 Rn 00000 (C4.1.3, unconditional branch (register)) */
    DEC(0xd61f0220u, SW, BR);
    A(rn, 17), FLOW(BR);
    DEC(0xd63f0100u, SW, BLR);
    A(rn, 8), FLOW(BLR);
    DEC(0xd65f03c0u, SW, RET);
    A(rn, 30), FLOW(RET);
    /* BRAAZ x3: op2=11111 op3=00001 M=0 Rn Rm=11111 (C6.2.38), FEAT_PAuth */
    UNDEF(0xd61f087fu, SW);
    DEC(0xd61f087fu, MAX, BRAZ);
    A(m, 0), A(rn, 3), FLOW(BR);
    /* RETAB: M=1 (C6.2.271) */
    DEC(0xd65f0fffu, MAX, RETA);
    A(m, 1), FLOW(RET);
    /* BRAA x1, x2: opc=1000 (C6.2.38) */
    DEC(0xd71f0822u, MAX, BRA);
    A(m, 0), A(rn, 1), A(rm, 2), FLOW(BR);
    /* ERET (C6.2.117): UNDEF at EL0, so on every user-space target */
    UNDEF(0xd69f03e0u, MAX);
    DEC(0xd69f03e0u, MAX | SNX_A64_FEAT_EL1, ERET);
    FLOW(ERET);
    /* SVC #0x45, BRK #1, HVC #0: 11010100 opc imm16 op2=000 LL (C4.1.3, exception generation) */
    DEC(0xd40008a1u, SW, SVC);
    A(imm, 0x45), FLOW(SVC);
    DEC(0xd4200020u, SW, BRK);
    A(imm, 1), FLOW(BRK);
    UNDEF(0xd4000002u, MAX);
    DEC(0xd4000002u, SW | SNX_A64_FEAT_EL1, HVC);
    FLOW(EXC);
    /* SMC #0 and HLT #0 are UNDEF at EL0 too */
    UNDEF(0xd4000003u, MAX);
    UNDEF(0xd4400000u, MAX);
}

/* C4.1.3 Hints: the overlapping group, first match wins and the rest of the space is NOP */
static void hints(void) {
    /* YIELD: CRm=0000 op2=001 (C6.2.395) */
    DEC(0xd503203fu, SW, YIELD);
    NOP(1), FLOW(PLAIN);
    /* NOP: CRm=0000 op2=000 */
    DEC(0xd503201fu, SW, NOP);
    NOP(1);
    /* HINT #0x7f: unallocated hint space executes as NOP */
    DEC(0xd5032fffu, MAX, NOP);
    /* XPACLRI (CRm=0000 op2=111) and PACIASP (CRm=0011 op2=001) are NOP without FEAT_PAuth */
    DEC(0xd50320ffu, SW, NOP);
    DEC(0xd50320ffu, MAX, XPACLRI);
    NOP(0), NEEDS(SNX_A64_FEAT_PAUTH);
    DEC(0xd503233fu, SW, NOP);
    DEC(0xd503233fu, MAX, PACIASP);
    /* an Android phone's target has no FEAT_PAuth: PACIASP and AUTIASP (CRm=0011 op2=101), which an app's arm64
       libraries sign their returns with, are NOPs as on a phone without it, while its atomics decode */
    DEC(0xd503233fu, ANDROID, NOP);
    NOP(1);
    DEC(0xd50323bfu, ANDROID, NOP);
    NOP(1);
    DEC(0xd50323bfu, MAX, AUTIASP);
    DEC(0xf8e00041u, ANDROID, LDADD);
    /* DMB ISH: CRm=1011 (domain=10 types=11) op2=101 (C6.2.95) */
    DEC(0xd5033bbfu, SW, DSB_DMB);
    A(domain, 2), A(types, 3), NOP(0);
    /* ISB, CLREX (CRm=1111) */
    DEC(0xd5033fdfu, SW, ISB);
    DEC(0xd5033f5fu, SW, CLREX);
    /* SB: FEAT_SB */
    UNDEF(0xd50330ffu, SW);
    DEC(0xd50330ffu, MAX, SB);
    /* MSR DAIFSet, #2: op1=011 CRn=0100 CRm=imm op2=110 (C6.2.250), trapped at EL0 (SCTLR_EL1.UMA is 0); MSR
       SPSel, #1 (op1=000 op2=101) is UNDEFINED at EL0 */
    UNDEF(0xd50342dfu, MAX);
    DEC(0xd50342dfu, SW | SNX_A64_FEAT_EL1, MSR_i_DAIFSET);
    A(imm, 2), NOP(0);
    UNDEF(0xd50041bfu, MAX);
    DEC(0xd50041bfu, SW | SNX_A64_FEAT_EL1, MSR_i_SPSEL);
    /* MSR DIT, #1 (op1=011 op2=010, FEAT_DIT) is EL0's */
    DEC(0xd503415fu, MAX, MSR_i_DIT);
    /* SMSTART: MSR SVCR with mask=11 imm=1, FEAT_SME; mask=00 is reserved */
    DEC(0xd503477fu, MAX, MSR_i_SVCR);
    A(mask, 3), A(imm, 1);
    UNDEF(0xd503417fu, MAX);
    /* MRS x0, TPIDR_EL0: L=1 op0=11 op1=011 CRn=1101 CRm=0000 op2=010 (C6.2.245) */
    DEC(0xd53bd040u, SW, SYS);
    A(l, 1), A(op0, 3), A(op1, 3), A(crn, 13), A(crm, 0), A(op2, 2), A(rt, 0);
    /* WFET x3: FEAT_WFxT */
    UNDEF(0xd5031003u, SW);
    DEC(0xd5031003u, MAX, WFET);
    A(rd, 3);
    /* RMIF x1, #7, #9: sf=1 op=0 S=1 11010000 imm6 00001 Rn o2=0 mask (C6.2.272), FEAT_FlagM */
    DEC(0xba038429u, MAX, RMIF);
    A(imm, 7), A(rn, 1), A(mask, 9);
}

/* C4.1.4 Loads and Stores */
static void loads_stores(void) {
    /* STLXR w1, x2, [x3]: size=11 001000 o2=0 L=0 o1=0 Rs o0=1 Rt2=11111 (C6.2.346) */
    DEC(0xc801fc62u, SW, STXR);
    A(sz, 3), A(lasr, 1), A(rs, 1), A(rt2, 31), A(rn, 3), A(rt, 2);
    /* STXP w1, x2, x4, [x3]: sz=1 in bit 30 is 64-bit, sz = 2 + bit 30 (C6.2.367) */
    DEC(0xc8211062u, SW, STXP);
    A(sz, 3), A(rs, 1), A(rt2, 4), A(lasr, 0);
    /* STLR x2, [x3]: o0=1 (C6.2.325) */
    DEC(0xc89ffc62u, SW, STLR);
    A(sz, 3), A(lasr, 1), NEEDS(SNX_A64_FEAT_V8_0);
    /* STLLR x2, [x3]: o0=0 needs FEAT_LOR */
    UNDEF(0xc89f7c62u, SW);
    DEC(0xc89f7c62u, MAX, STLR);
    A(lasr, 0), NEEDS(SNX_A64_FEAT_V8_0 | SNX_A64_FEAT_LOR);
    /* CASAL x0, x1, [x2]: FEAT_LSE, undefined on the Switch's A57 (C6.2.44) */
    UNDEF(0xc8e0fc41u, SW);
    DEC(0xc8e0fc41u, MAX, CAS);
    A(sz, 3), A(rs, 0), A(rt, 1), A(rn, 2), NEEDS(SNX_A64_FEAT_LSE);
    /* CASP with an odd Rs is reserved (C6.2.45) */
    UNDEF(0x48217c44u, MAX);
    /* LDR x1, #8 (literal): opc=01 011 V=0 00 imm19=2 (C6.2.167) */
    DEC(0x58000041u, SW, LD_lit);
    A(sz, 3), A(sign, 0), A(imm, 8), A(rt, 1);
    /* LDRSW x1, #-4 (literal): opc=10 */
    DEC(0x98ffffe1u, SW, LD_lit);
    A(sz, 2), A(sign, 1), A(imm, -4);
    /* LDR q2, #16 (literal): opc=10 V=1 */
    DEC(0x9c000082u, SW, LD_lit_v);
    A(sz, 4), A(imm, 16);
    /* PRFM (literal): opc=11 is a NOP */
    DEC(0xd8000040u, SW, NOP);
    NOP(1);
    /* LDP x1, x2, [sp, #-16]!: opc=10 101 V=0 011 L=1 imm7=-2, unscaled in the argument (C6.2.178) */
    DEC(0xa9ff0be1u, SW, LDP);
    A(sz, 3), A(sign, 0), A(p, 0), A(w, 1), A(imm, -2), A(rt, 1), A(rt2, 2), A(rn, 31);
    /* LDPSW x1, x2, [x3, #8]: opc=01, signed offset */
    DEC(0x69410861u, SW, LDP);
    A(sz, 2), A(sign, 1), A(p, 0), A(w, 0), A(imm, 2);
    /* LDR x0, [x1], #-8: post-index, imm9 (C6.2.166) */
    DEC(0xf85f8420u, SW, LDR_i);
    A(sz, 3), A(p, 1), A(w, 1), A(unpriv, 0), A(imm, -8);
    /* LDR x0, [x1, #16]: unsigned offset, imm12=2 scaled by 8 (uimm_scaled) */
    DEC(0xf9400820u, SW, LDR_i);
    A(sz, 3), A(p, 0), A(w, 0), A(imm, 16), A(ext, 0);
    /* LDR q0, [x1, #32]: size=00 V=1 opc=11, scaled by 16 */
    DEC(0x3dc00820u, SW, LDR_v_i);
    A(sz, 4), A(imm, 32);
    /* LDRSB w0, [x1, #3]!: size=00 opc=11 pre-index, sign-extended to 32 bits */
    DEC(0x38c03c20u, SW, LDR_i);
    A(sz, 0), A(sign, 1), A(ext, 1), A(p, 0), A(w, 1), A(imm, 3);
    /* LDTR x0, [x1, #1]: unprivileged */
    DEC(0xf8401820u, SW, LDR_i);
    A(unpriv, 1), A(imm, 1);
    /* LDR x0, [x1, x2, LSL #3]: option=011 S=1 (C6.2.168) */
    DEC(0xf8627820u, SW, LDR);
    A(opt, 3), A(s, 1), A(rm, 2), A(sz, 3);
    /* option=001 is reserved (option<1> must be 1) */
    UNDEF(0xf8623820u, MAX);
    /* LDADDAL x0, x1, [x2]: A=1 R=1, FEAT_LSE (C6.2.136) */
    UNDEF(0xf8e00041u, SW);
    DEC(0xf8e00041u, MAX, LDADD);
    A(a, 1), A(r, 1), A(rs, 0), A(rt, 1), A(rn, 2), A(sz, 3);
    /* LDAPR x0, [x1]: FEAT_LRCPC (C6.2.155) */
    DEC(0xf8bfc020u, MAX, LDAPR);
    A(sz, 3), NEEDS(SNX_A64_FEAT_LRCPC);
    /* LDCLRP x0, x1, [x2]: FEAT_LSE128 */
    DEC(0x19211040u, MAX, LDCLRP);
    A(rt2, 1), A(rn, 2), A(rt, 0), A(a, 0), A(r, 0);
    /* LDRAA x0, [x1, #-8]: M=0 S=1 imm9=0x1ff W=0, S:imm9 signed and scaled by 8 (C6.2.175) */
    DEC(0xf87ff420u, MAX, LDRA);
    A(m, 0), A(w, 0), A(imm, -8);
    /* STLUR w0, [x1, #-1]: FEAT_LRCPC2 (C6.2.326) */
    DEC(0x991ff020u, MAX, STLR_i);
    A(sz, 2), A(imm, -1);
    /* ST1 {v0.16b}, [x1]: Q=1 L=0 opcode=0111 size=00 (C7.2.323) */
    DEC(0x4c007020u, SW, ST_mult);
    A(q, 1), A(p, 0), A(rm, 0), A(sz, 0), A(rpt, 1), A(selem, 1), A(rn, 1), A(rt, 0);
    /* LD1 {v0.4s}, [x1], #16: post-index, Rm=11111 */
    DEC(0x4cdf7820u, SW, LD_mult);
    A(p, 1), A(rm, 31), A(sz, 2);
    /* LD4 {v0.1d-v3.1d}: size=11 Q=0 with more than one element is reserved */
    UNDEF(0x0c400c20u, MAX);
    /* no post-index (bit 23 = 0) but Rm != 0 is unallocated */
    UNDEF(0x4c437820u, MAX);
    /* LD1 {v0.s}[3], [x1]: Q=1 S=1 opcode=100 size=00, index = Q:S (C7.2.169) */
    DEC(0x4d409020u, SW, LD_single);
    A(scale, 2), A(selem, 1), A(index, 3), A(p, 0);
    /* LD1R {v0.4s}, [x1]: opcode=110 size=10 (C7.2.170) */
    DEC(0x4d40c820u, SW, LD_single_repl);
    A(q, 1), A(scale, 2), A(selem, 1);
    /* STG x0, [x1, #32]: signed offset, imm9 scaled by the 16-byte tag granule, FEAT_MTE (C6.2.329) */
    DEC(0xd9202820u, MAX, STG);
    A(imm, 32), A(p, 0), A(w, 0);
    /* SETP [x0]!, x1!, x2: FEAT_MOPS (C6.2.304); Rd == Rn is reserved */
    UNDEF(0x19c20420u, SW);
    DEC(0x19c20420u, MAX, SETP);
    A(rs, 2), A(rn, 1), A(rd, 0), A(nontemp, 0), A(unpriv, 0);
    UNDEF(0x19c20400u, MAX);
    /* CPYFP [x0]!, [x1]!, x2!: Rd destination, Rs source, Rn size (C6.2.73) */
    DEC(0x19010440u, MAX, CPYFP);
    A(rd, 0), A(rs, 1), A(rn, 2), A(options, 0);
    /* GCSSTR x0, [x1]: FEAT_GCS */
    DEC(0xd91f0c20u, MAX, GCSSTR);
    A(unpriv, 0), A(rn, 1), A(rt, 0);
}

/* C4.1.5 Data Processing -- Register */
static void data_register(void) {
    /* UDIV w0, w1, w2 (C6.2.388) */
    DEC(0x1ac20820u, SW, UDIV);
    A(sf, 0), A(rm, 2), A(rn, 1), A(rd, 0);
    /* CRC32CX w0, w1, x2: sf=1 C=1 sz=11, FEAT_CRC32, which the A57 has (C6.2.78) */
    DEC(0x9ac25c20u, SW, CRC32C);
    A(esz, 3), NEEDS(SNX_A64_FEAT_CRC32);
    /* SUBP x0, x1, x2: FEAT_MTE */
    DEC(0x9ac20020u, MAX, SUBP);
    A(rm, 2), A(rn, 1), A(rd, 0);
    /* RBIT w0, w1 and REV x0, x1 (C6.2.262, C6.2.265) */
    DEC(0x5ac00020u, SW, RBIT);
    A(sf, 0);
    DEC(0xdac00c20u, SW, REV64);
    A(rn, 1);
    /* PACIZA x3: Z=1 needs Rn=11111 (C6.2.233) */
    DEC(0xdac123e3u, MAX, PACIA);
    A(z, 1), A(rn, 31), A(rd, 3);
    UNDEF(0xdac12083u, MAX);
    /* AND w0, w1, w2, LSL #31 (C6.2.13); imm6 >= 32 with sf=0 is reserved */
    DEC(0x0a027c20u, SW, AND_r);
    A(sf, 0), A(st, 0), A(sa, 31), A(n, 0);
    UNDEF(0x0a028020u, MAX);
    /* ORN x0, x1, x2, ROR #3: opc=01 N=1 shift=11 */
    DEC(0xaae20c20u, SW, ORR_r);
    A(st, 3), A(n, 1), A(sa, 3);
    /* ADD x0, x1, x2, ASR #5 (C6.2.7); shift=11 is reserved for add/subtract */
    DEC(0x8b821420u, SW, ADD_r);
    A(st, 2), A(sa, 5), A(sf, 1);
    UNDEF(0x8bc21420u, MAX);
    /* ADD x0, sp, w1, UXTW #2 (C6.2.4); a shift above 4 is reserved */
    DEC(0x8b214be0u, SW, ADD_ext);
    A(st, 2), A(sa, 2), A(rn, 31);
    UNDEF(0x8b2157e0u, MAX);
    /* CCMP x1, #3, #4, NE: sf=1 op=1 S=1 imm5 cond o2=0 Rn o3=0 nzcv (C6.2.57) */
    DEC(0xfa431824u, SW, CCMP);
    A(sf, 1), A(op, 1), A(y, 3), A(cond, 1), A(imm, 1), A(rn, 1), A(nzcv, 4);
    /* CSINC w0, w1, w2, EQ (C6.2.86) */
    DEC(0x1a820420u, SW, CSEL);
    A(else_inv, 0), A(else_inc, 1), A(cond, 0);
    /* MADD x0, x1, x2, x3 (C6.2.226) */
    DEC(0x9b020c20u, SW, MADD_x);
    A(ra, 3), A(rm, 2), A(rn, 1), A(rd, 0);
}

/* C4.1.6 and C4.1.7: cryptographic, SIMD and floating-point */
static void simd_fp(void) {
    /* AESE v0.16b, v1.16b: Rn is the second operand, rn = rd (C7.2.7) */
    DEC(0x4e284820u, SW, AESE);
    A(rd, 0), A(rn, 0), A(rm, 1), A(q, 1), A(esz, 0), NEEDS(SNX_A64_FEAT_AES);
    /* SHA256H q0, q1, v2.4s (C7.2.270), SHA1H s0, s1 (C7.2.264) */
    DEC(0x5e024020u, SW, SHA256H);
    A(rm, 2), A(rn, 1);
    DEC(0x5e280820u, SW, SHA1H);
    /* SHA512H, EOR3, SM3TT1A, XAR: FEAT_SHA512, FEAT_SHA3, FEAT_SM3, not on the A57 */
    UNDEF(0xce628020u, SW);
    DEC(0xce628020u, MAX, SHA512H);
    DEC(0xce020c20u, MAX, EOR3);
    A(ra, 3), A(rm, 2), A(esz, 3);
    DEC(0xce42a020u, MAX, SM3TT1A);
    A(imm, 2), A(rm, 2);
    DEC(0xce822420u, MAX, XAR);
    A(imm, 9), A(rm, 2);
    /* PMULL2 v0.1q, v1.2d, v2.2d: FEAT_PMULL, which the A57 has (C7.2.215) */
    DEC(0x4ee2e020u, SW, PMULL_p64);
    /* SDOT (FEAT_DotProd) and FMLAL (FEAT_FHM) are later than the A57 */
    UNDEF(0x4e829420u, SW);
    DEC(0x4e829420u, MAX, SDOT_v);
    DEC(0x4e22ec20u, MAX, FMLAL_v);
    /* MOV s0, v1.s[2] (DUP (element), scalar): imm5=10100 (C7.2.38) */
    DEC(0x5e140420u, SW, DUP_element_s);
    A(imm, 20);
    /* MOV w0, v1.s[3] and MOV x0, v1.d[1] (UMOV, C7.2.371); the D form needs Q=1 */
    DEC(0x0e1c3c20u, SW, UMOV);
    A(imm, 28), A(q, 0);
    DEC(0x4e183c20u, SW, UMOV);
    A(imm, 24), A(q, 1);
    UNDEF(0x0e183c20u, MAX);
    /* SMOV x0, v1.s[1] (C7.2.279); SMOV w0 from an S element is reserved */
    DEC(0x4e0c2c20u, SW, SMOV);
    UNDEF(0x0e0c2c20u, MAX);
    /* MOV v0.s[1], v1.s[2] (INS (element), C7.2.176): imm5 = 01100, imm4 = 1000 */
    DEC(0x6e0c4420u, SW, INS_element);
    A(di, 12), A(si, 8);
    /* DUP v0.8h, w1 (C7.2.39) */
    DEC(0x4e020c20u, SW, DUP_general);
    A(imm, 2), A(q, 1);
    /* FADD d0, d1, d2: ftype=01 (C7.2.50); ftype=11 (half) needs FEAT_FP16; ftype=10 is reserved */
    DEC(0x1e622820u, SW, FADD_s);
    A(esz, 3), NEEDS(SNX_A64_FEAT_V8_0);
    UNDEF(0x1ee22820u, SW);
    DEC(0x1ee22820u, MAX, FADD_s);
    A(esz, 1), NEEDS(SNX_A64_FEAT_V8_0 | SNX_A64_FEAT_FP16);
    UNDEF(0x1ea22820u, MAX);
    /* FMULX h0, h1, h2: the half-precision row, FEAT_FP16 (C7.2.119) */
    UNDEF(0x5e421c20u, SW);
    DEC(0x5e421c20u, MAX, FMULX_s);
    A(esz, 1);
    /* SQRDMLAH s0, s1, s2: FEAT_RDM (C7.2.259) */
    UNDEF(0x7e828420u, SW);
    DEC(0x7e828420u, MAX, SQRDMLAH_s);
    A(esz, 2);
    /* SQDMULL s0, h1, h2: scalar x scalar decoded as indexed with idx=0 (C7.2.250) */
    DEC(0x5e62d020u, SW, SQDMULL_si);
    A(esz, 1), A(idx, 0), A(rm, 2);
    /* ADD v0.2d, v1.2d, v2.2d (C7.2.3); 1D (size=11, Q=0) is reserved */
    DEC(0x4ee28420u, SW, ADD_v);
    A(q, 1), A(esz, 3);
    UNDEF(0x0ee28420u, MAX);
    /* SUQADD v3.4s, v4.4s: the accumulator is Rd, rn = rd (C7.2.336) */
    DEC(0x4ea03883u, SW, SUQADD_v);
    A(rd, 3), A(rn, 3), A(rm, 4), A(esz, 2);
    /* FMUL v0.4s, v1.4s, v2.s[3]: index H:L (C7.2.110) */
    DEC(0x4fa29820u, SW, FMUL_vi);
    A(esz, 2), A(idx, 3), A(rm, 2), A(q, 1);
    /* FMUL v0.8h, v1.8h, v2.h[7]: index H:L:M, Rm 4 bits, FEAT_FP16 */
    UNDEF(0x4f329820u, SW);
    DEC(0x4f329820u, MAX, FMUL_vi);
    A(esz, 1), A(idx, 7), A(rm, 2);
    /* FMUL d0, d1, v2.d[1]: index H (C7.2.110, scalar) */
    DEC(0x5fc29820u, SW, FMUL_si);
    A(esz, 3), A(idx, 1);
    /* MUL v0.4h, v1.4h, v15.h[5]: H:L:M = 101, Rm 4 bits (C7.2.205) */
    DEC(0x0f5f8820u, SW, MUL_vi);
    A(esz, 1), A(idx, 5), A(rm, 15), A(q, 0);
    /* FMLALB v0.8h, v1.16b, v2.16b: FEAT_FP8FMA, &rxx */
    DEC(0x0ec2fc20u, MAX, FMLAL_hb_v);
    A(idxn, 0), A(idxm, 0), A(rm, 2);
    /* FMLALLBT v0.4s, v1.16b, v2.b[13] (indexed): idxn = Q:bit22, idxm = H:L:M:bit19 */
    DEC(0x6f6a8820u, MAX, FMLALL_sb_vi);
    A(idxn, 3), A(idxm, 13), A(rm, 2), NEEDS(SNX_A64_FEAT_FP8FMA);
    /* FCMLA v0.4s, v1.4s, v2.4s, #90 (C7.2.62) and FCMLA v0.4s, v1.4s, v2.s[1], #180: FEAT_FCMA */
    UNDEF(0x6e82cc20u, SW);
    DEC(0x6e82cc20u, MAX, FCMLA_v);
    A(rot, 1), A(esz, 2), A(q, 1);
    DEC(0x6f825820u, MAX, FCMLA_vi);
    A(rot, 2), A(idx, 1), A(esz, 2);
    /* MOVI v0.16b, #0xab: op=0 cmode=1110 abc:defgh (C7.2.206) */
    DEC(0x4f05e560u, SW, Vimm);
    A(q, 1), A(op, 0), A(cmode, 14), A(abcdefgh, 0xab);
    /* FMOV (vector, double) with Q=0 is reserved */
    UNDEF(0x2f03f600u, MAX);
    /* SSHR v0.4s, v1.4s, #3: immh:immb = 64 - 3 within 32-bit lanes (C7.2.312) */
    DEC(0x4f3d0420u, SW, SSHR_v);
    A(esz, 2), A(imm, 3), A(q, 1);
    /* SHL v0.2d, v1.2d, #5: immh:immb = 64 + 5 (C7.2.270) */
    DEC(0x4f455420u, SW, SHL_v);
    A(esz, 3), A(imm, 5);
    /* a D-lane shift with Q=0 is reserved */
    UNDEF(0x0f450420u, MAX);
    /* SSHR d0, d1, #64: immh:immb = 1000000 (scalar) */
    DEC(0x5f400420u, SW, SSHR_s);
    A(esz, 3), A(imm, 64);
    /* FCVTZS w0, s1, #3: scale = 64 - fbits (C7.2.95); sf=0 needs scale >= 32 */
    DEC(0x1e18f420u, SW, FCVTZS_g);
    A(sf, 0), A(esz, 2), A(shift, 3);
    UNDEF(0x1e187420u, MAX);
    /* SCVTF d0, x1 (C7.2.236) */
    DEC(0x9e620020u, SW, SCVTF_g);
    A(sf, 1), A(esz, 3), A(shift, 0);
    /* FMOV x0, d1, and FMOV x0, h1, which needs FEAT_FP16 (C7.2.131) */
    DEC(0x9e660020u, SW, FMOV_xd);
    UNDEF(0x9ee60020u, SW);
    DEC(0x9ee60020u, MAX, FMOV_xh);
    /* FCVT d0, s1 (C7.2.69) */
    DEC(0x1e22c020u, SW, FCVT_s_ds);
    /* FMOV d0, #1.0: imm8=0x70 (C7.2.132) */
    DEC(0x1e6e1000u, SW, FMOVI_s);
    A(imm, 0x70), A(esz, 3);
    /* FCMP s0, #0.0 (C7.2.66) */
    DEC(0x1e202008u, SW, FCMP);
    A(z, 1), A(e, 0), A(esz, 2);
    /* FCCMP d1, d2, #3, GE (C7.2.58) and FCSEL s0, s1, s2, LT (C7.2.67) */
    DEC(0x1e62a423u, SW, FCCMP);
    A(cond, 10), A(nzcv, 3), A(e, 0), A(rn, 1), A(rm, 2), A(esz, 3);
    DEC(0x1e22bc20u, SW, FCSEL);
    A(cond, 11), A(esz, 2);
    /* FMADD d0, d1, d2, d3 (C7.2.98) */
    DEC(0x1f420c20u, SW, FMADD);
    A(ra, 3), A(esz, 3);
    /* EXT v0.16b, v1.16b, v2.16b, #9 (C7.2.43) */
    DEC(0x6e024820u, SW, EXT_q);
    A(imm, 9);
    /* TBX v0.8b, {v1.16b, v2.16b}, v3.8b: len=01 op=1 (C7.2.358) */
    DEC(0x0e033020u, SW, TBL_TBX);
    A(len, 1), A(tbx, 1), A(q, 0), A(rm, 3);
    /* ADDV s0, v1.4s (C7.2.6); ADDV of two S lanes is reserved */
    DEC(0x4eb1b820u, SW, ADDV);
    A(esz, 2), A(q, 1);
    UNDEF(0x0eb1b820u, MAX);
    /* FCVTL2 v0.2d, v1.4s (C7.2.72) and FCVTN v0.4h, v1.4s (C7.2.79) */
    DEC(0x4e617820u, SW, FCVTL_v);
    A(esz, 3), A(q, 1);
    DEC(0x0e216820u, SW, FCVTN_v);
    A(esz, 1), A(q, 0);
    /* FMAXNMV s0, v1.4s and FMAXNMV h0, v1.8h, which needs FEAT_FP16 (C7.2.103) */
    DEC(0x6e30c820u, SW, FMAXNMV_s);
    A(esz, 2), A(q, 1);
    UNDEF(0x4e30c820u, SW);
    DEC(0x4e30c820u, MAX, FMAXNMV_h);
    /* SCVTF v0.4s, v1.4s, #5 (fixed point, C7.2.235) and SCVTF d0, d1, #10 */
    DEC(0x4f3be420u, SW, SCVTF_vf);
    A(esz, 2), A(shift, 5), A(q, 1);
    DEC(0x5f76e420u, SW, SCVTF_f);
    A(esz, 3), A(shift, 10), A(sf, 0);
    /* SQXTN b0, h1 (C7.2.267), REV32 v0.8h, v1.8h (C7.2.226), CNT v0.8b, v1.8b (C7.2.27) */
    DEC(0x5e214820u, SW, SQXTN_s);
    A(esz, 0);
    DEC(0x6e600820u, SW, REV32_v);
    A(esz, 1), A(q, 1);
    DEC(0x0e205820u, SW, CNT_v);
    A(esz, 0), A(q, 0);
    /* LUTI2 v0.16b, {v1.16b}, v2[3]: FEAT_LUT */
    DEC(0x4e827020u, MAX, LUTI2_1b);
    A(idx, 3), A(rm, 2), A(esz, 0);
}

/* every N and imms of a logical immediate, against the Arm ARM's DecodeBitMasks (J1.1.4): the element size is the
   highest set bit of N:NOT(imms), at least 2 bits, and imms is not all ones within it; a 32-bit form has N 0 */
static void logical_immediates(void) {
    unsigned sf, n, imms;
    for (sf = 0; sf < 2; sf++)
        for (n = 0; n < 2; n++)
            for (imms = 0; imms < 64; imms++) {
                /* AND x0/w0, x1/w1, #imm with immr 5 */
                uint32_t word = 0x12050020u | (uint32_t)sf << 31 | (uint32_t)n << 22 | (uint32_t)imms << 10;
                unsigned x = n << 6 | (~imms & 63), levels;
                int len = 6, valid;
                while (len >= 0 && !((x >> len) & 1)) len--;
                levels = len >= 1 ? (1u << len) - 1 : 0;
                valid = len >= 1 && (imms & levels) != levels && (sf || !n);
                dec(word, MAX, valid ? SNX_A64_OP_AND_i : SNX_A64_OP_UNDEF, __LINE__);
                if (valid) eq(g_test.d.args.dbm, (long long)(n << 12 | 5u << 6 | imms), "dbm", __LINE__);
            }
}

/* the index (buckets on bits [31:21], split where crowded) finds what a scan of every row in order finds, each row
   tried alone (snx_a64_row with no row after it), and fills the instruction the same; an UNDEF's is all zero */
static void index_matches_scan(void) {
    static const uint64_t targets[2] = {SW, MAX};
    uint32_t x = 0x12345678u;
    long i;
    int t;
    for (i = 0; i < 400000; i++) {
        uint32_t word;
        x = x * 1664525u + 1013904223u;
        /* every top-11-bit bucket, then random words */
        word = i < 2048 * 16 ? ((uint32_t)(i & 2047) << 21) | ((x >> 11) & 0x1fffffu) : x;
        for (t = 0; t < 2; t++) {
            SnxA64Insn a, b;
            SnxA64Op want = SNX_A64_OP_UNDEF;
            unsigned r;
            memset(&a, 0xa5, sizeof a);
            memset(&b, 0, sizeof b);
            for (r = 0; r < sizeof snx_a64_rows / sizeof snx_a64_rows[0]; r++) {
                const SnxA64Row* row = &snx_a64_rows[r];
                uint16_t one = (uint16_t)r;
                if ((word & row->mask) != row->value || (row->feature & ~targets[t])) continue;
                if (snx_a64_row(&one, &one + 1, word, targets[t], &b) == SNX_A64_OP_UNDEF) continue;
                want = (SnxA64Op)row->op;
                break;
            }
            if (snx_a64_decode(word, targets[t], &a) != want) {
                fprintf(stderr, "a64_decode_test.c: 0x%08x decodes as %s, a scan finds %s\n", (unsigned)word,
                        snx_a64_op_name(a.op), snx_a64_op_name(want));
                g_test.failures++;
                return;
            }
            /* both were zeroed whole before their fields were set, so the bytes compare */
            if (memcmp(&a, &b, sizeof a)) {
                fprintf(stderr, "a64_decode_test.c: 0x%08x (%s): the decode's instruction differs from its row's\n",
                        (unsigned)word, snx_a64_op_name(want));
                g_test.failures++;
                return;
            }
        }
    }
}

int main(void) {
    data_immediate();
    logical_immediates();
    branches();
    hints();
    loads_stores();
    data_register();
    simd_fp();
    index_matches_scan();
    eq(SNX_A64_DECODE_HASH != 0, 1, "SNX_A64_DECODE_HASH", __LINE__);
    if (g_test.failures) {
        fprintf(stderr, "a64_decode_test: %d failures\n", g_test.failures);
        return 1;
    }
    printf("a64_decode_test: ok\n");
    return 0;
}
