/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end (src/common/cpu/recompiler/x64) against LLVM, a function at a time, through the recompiler's
   own interface (snx_recompiler.h): random functions of a few words and a RET, made from encodings of what
   x64_codegen.c builds itself (the SIMD&FP loads and stores in every addressing mode, MOVI, MVNI, ORR, BIC and FMOV
   of an immediate, MRS, MSR and the cache maintenance) among the scalar floating point and integer words sailgen's
   emitters make. One program is recompiled twice, by a recompiler with the x86-64 back end and one with LLVM alone;
   each function runs from the same random state in both, and every register, the flags, FPCR, FPSR, TPIDR_EL0 and the
   data memory must match. The x86-64 back end must make the functions (snx_recompiled_x64_functions), or the test
   would compare LLVM with itself.
     snx-test-recompiler-backends [--functions N] [--states N] [--seed N] [--without MASK] [--target NAME] [--verbose]
         [--hook] [--words HEX...] [--made] [--ops OP...] [--refused]
   --without MASK: the x86-64 back end goes without these features of the host (sailgen's SGX_F_*: FMA 0x800, AVX 0x400,
   F16C 0x100, BMI1, BMI2 and LZCNT 7, AES-NI 0x1000, SHA-NI 0x2000), as on a host that lacks them; arm64's without its
   (SNX_ARM64_F_*)
   --target NAME: the guest's features, switch (the default), android, max or, with --refused, ounce
   --made: the back end must make --words' function (else, where LLVM makes it, there is nothing to compare)
   --ops OP...: every word of these ops of the decoder (llvm_qemu_words.c's samples, and each value of its small fields)
   is a function of its own with a RET, and the back end must make them all; they run from as many states as the rest,
   half of them with lanes of the values where an integer's or a float's arithmetic turns
   --refused: each of the target's ops that has words the back end leaves to LLVM, how many of its words, and stops
   there, failing where there is one
   On a host without the back end (not x86-64, or Windows, or built without SNX_SAIL_X64) it passes, saying so.

   Built with SNX_TEST_ARM64 or SNX_TEST_RV64 (snx-test-recompiler-arm64, -rv64), the same programs hold that native
   back end (src/common/cpu/recompiler/arm64, rv64) to LLVM on any host: its object, made for the program's
   functions, runs under Unicorn (support/host_unicorn.c), the native side's memory and state mapped into it, the
   helpers and the runtime's functions it calls trapped to this process's. --hook makes it with every call through
   CPU.disp (hook_direct_calls, as NVN's handlers want), which holds each function */
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "llvm_qemu_words.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(SNX_TEST_ARM64) || defined(SNX_TEST_RV64) || defined(SNX_TEST_PPC64) || defined(SNX_TEST_MIPS64) ||          \
    defined(SNX_TEST_LA64) || defined(SNX_TEST_PPC64BE) || defined(SNX_TEST_MIPS64BE) || defined(SNX_TEST_S390X)
#define SNX_TEST_NATIVE 1
#include "support/host_unicorn.h"
#define SNX_TEST_SWAP_CPU 1
#include "support/swap_state.h"
#if defined(SNX_TEST_ARM64)
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#define NATIVE_UC HOSTUC_ARM64
#define NATIVE_MIN_MADE 90 /* percent of the functions the back end must make */
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_arm64_object(cg, 0, (cg)->function_count, options, SNX_ARM64_F_ALL & ~g_without, made, object, size, error,    \
                     sizeof error)
#elif defined(SNX_TEST_LA64)
#include "common/cpu/recompiler/la64/la64_codegen.h"
#define NATIVE_UC HOSTUC_LA64
#define NATIVE_MIN_MADE 50 /* as riscv64's: the SIMD clauses sailgen doesn't make yet */
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_la64_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#elif defined(SNX_TEST_PPC64BE)
#include "common/cpu/recompiler/ppc64/ppc64be_codegen.h"
#define NATIVE_UC HOSTUC_PPC64
#define NATIVE_MIN_MADE 50 /* as riscv64's: the SIMD clauses sailgen doesn't make yet */
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_ppc64be_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#elif defined(SNX_TEST_S390X)
#include "common/cpu/recompiler/s390x/s390x_codegen.h"
#define NATIVE_UC HOSTUC_S390X
#define NATIVE_MIN_MADE 50 /* as riscv64's: the SIMD clauses sailgen doesn't make yet */
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_s390x_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#elif defined(SNX_TEST_MIPS64BE)
#include "common/cpu/recompiler/mips64/mips64be_codegen.h"
#define NATIVE_UC HOSTUC_MIPS64
#define NATIVE_MIN_MADE 50 /* as riscv64's: the SIMD clauses sailgen doesn't make yet */
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_mips64be_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#elif defined(SNX_TEST_MIPS64)
#include "common/cpu/recompiler/mips64/mips64_codegen.h"
#define NATIVE_UC HOSTUC_MIPS64EL
#define NATIVE_MIN_MADE 50 /* as riscv64's: the SIMD clauses sailgen doesn't make yet */
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_mips64_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#elif defined(SNX_TEST_PPC64)
#include "common/cpu/recompiler/ppc64/ppc64_codegen.h"
#define NATIVE_UC HOSTUC_PPC64LE
#define NATIVE_MIN_MADE 50 /* as riscv64's: the SIMD clauses sailgen doesn't make yet */
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_ppc64_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#else
#include "common/cpu/recompiler/rv64/rv64_codegen.h"
#define NATIVE_UC HOSTUC_RV64
/* the Advanced SIMD clauses sailgen doesn't make yet for a machine without vector registers (a64_risc.roots) leave
   their functions to LLVM */
#define NATIVE_MIN_MADE 50
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_rv64_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#endif
u64 snx_x64_crc32(u64 acc, u64 value, u32 desc);
u64 snx_x64_stxp(CPU* c, u64 a, u64 v1, u64 v2);
HostUcPair snx_x64_casp(CPU* c, u64 a, u64 e0, u64 e1, u64 d0, u64 d1);
#endif

#define CODE     0x10000u
#define SLOT     16u          /* words a function has room for, its RET included */
#define DATA     0x200000u    /* the loads' and stores' memory */
#define DATA_LEN 0x10000u
#define MEMORY   (DATA + DATA_LEN + 0x1000u)
#define STOP     0u

static int failures, verbose;
[[maybe_unused]] static unsigned g_without; /* --without's, for arm64's */
static uint64_t g_features = SNX_A64_TARGET_SWITCH; /* --target */
static uint64_t seed = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}
static unsigned pick(unsigned n) { return (unsigned)(rnd() % n); }

/* ---- the words ----
   The registers: x0-x15 any value, the integer words' and MRS's destinations; x20-x27 and SP addresses in the middle
   of the data (16-aligned), which the loads and stores use as their bases and their writebacks move by a few KiB at
   most; x28 a small offset, the register-offset forms' Rm. The SIMD&FP registers are all free */
static unsigned base(void) {
    unsigned r = 20 + pick(9);
    return r == 28 ? 31 : r; /* SP */
}
static unsigned lo(void) { return pick(16); }
static unsigned v(void) { return pick(32); }

/* LD1-LD4 and ST1-ST4 of multiple and single structures, LD1R-LD4R: no offset, or post-index by the size (Rm 31)
   or by x28 */
static uint32_t struct_ldst(void) {
    static const unsigned opc[] = {0, 2, 4, 6, 7, 8, 10};
    unsigned rt = v(), rn = base(), q = pick(2), l = pick(2), post = pick(2), rm = post ? (pick(2) ? 31 : 28) : 0;
    if (pick(2)) { /* multiple */
        unsigned size = pick(4), o = opc[pick(7)];
        if (size == 3 && !q && o != 2 && o != 6 && o != 7 && o != 10) size = 2;
        return q << 30 | 0x0C000000u | post << 23 | l << 22 | rm << 16 | o << 12 | size << 10 | rn << 5 | rt;
    }
    { /* single: opcode 000-101 an element (S and size its index), 110-111 LDnR */
        unsigned o = pick(8), r = pick(2), s = pick(2), size = pick(4);
        if (o >= 6) l = 1, s = 0;
        if ((o >> 1) == 1) size &= 2;
        if ((o >> 1) == 2) size = pick(2) ? 0 : 1, s = size ? 0 : s;
        return q << 30 | 0x0D000000u | post << 23 | l << 22 | r << 21 | rm << 16 | o << 13 | s << 12 | size << 10 |
               rn << 5 | rt;
    }
}
static uint32_t fp_ldst(void) {
    unsigned rt = v(), rn = base();
    if (pick(3) == 0) return struct_ldst();
    switch (pick(5)) {
    case 0: { /* unsigned offset */
        unsigned q = pick(5) == 0, size = q ? 0 : pick(4), opc = (q ? 2 : 0) | pick(2);
        return size << 30 | 0x3D000000u | opc << 22 | pick(64) << 10 | rn << 5 | rt;
    }
    case 1: { /* unscaled, post-index, pre-index */
        static const unsigned idx[] = {0, 1, 3};
        unsigned q = pick(5) == 0, size = q ? 0 : pick(4), opc = (q ? 2 : 0) | pick(2);
        return size << 30 | 0x3C000000u | opc << 22 | (pick(512) & 0x1FF) << 12 | idx[pick(3)] << 10 | rn << 5 | rt;
    }
    case 2: { /* register offset: UXTW, LSL, SXTW, SXTX of x28 */
        static const unsigned option[] = {2, 3, 6, 7};
        unsigned q = pick(5) == 0, size = q ? 0 : pick(4), opc = (q ? 2 : 0) | pick(2);
        return size << 30 | 0x3C200800u | opc << 22 | 28u << 16 | option[pick(4)] << 13 | pick(2) << 12 | rn << 5 |
               rt;
    }
    case 3: { /* the pairs: no-allocate, post, offset, pre */
        unsigned opc = pick(3), idx = pick(4), l = pick(2), rt2 = v();
        if (l && rt2 == rt) rt2 = (rt + 1) & 31;
        return opc << 30 | 0x2C000000u | idx << 23 | l << 22 | pick(128) << 15 | rt2 << 10 | rn << 5 | rt;
    }
    default: { /* literal: some of the code before it */
        unsigned imm19 = (0x80000u - 1 - pick(64)) & 0x7FFFF;
        return pick(3) << 30 | 0x1C000000u | imm19 << 5 | rt;
    }
    }
}

/* LDR, STR (unsigned offset, unscaled, post- and pre-index, register offset by x28) of bytes to doublewords, the
   sign-extending loads, LDP and STP of W and X: Rt from x0-x15, never the base (no writeback onto its own register) */
static uint32_t int_ldst(void) {
    unsigned rt = lo(), rn = base(), size = pick(4), opc = pick(2) ? pick(2) : (size < 2 ? 2 + pick(2) : (size == 2 ? 2 : pick(2)));
    switch (pick(4)) {
    case 0: return size << 30 | 0x39000000u | opc << 22 | pick(64) << 10 | rn << 5 | rt;
    case 1: {
        static const unsigned idx[] = {0, 1, 3};
        return size << 30 | 0x38000000u | opc << 22 | (pick(512) & 0x1FF) << 12 | idx[pick(3)] << 10 | rn << 5 | rt;
    }
    case 2: {
        static const unsigned option[] = {2, 3, 6, 7};
        return size << 30 | 0x38200800u | opc << 22 | 28u << 16 | option[pick(4)] << 13 | pick(2) << 12 | rn << 5 | rt;
    }
    default: { /* pairs of W (opc 00) or X (10), no-allocate, post, offset, pre */
        unsigned x = pick(2), rt2 = lo();
        if (rt2 == rt) rt2 = (rt + 1) & 15;
        return (x ? 2u : 0u) << 30 | 0x28000000u | pick(4) << 23 | pick(2) << 22 | pick(128) << 15 | rt2 << 10 | rn << 5 | rt;
    }
    }
}

static uint32_t simd_imm(void) {
    unsigned q = pick(2), op = pick(2), cmode = pick(16);
    if (op && cmode == 15 && !q) q = 1;
    return q << 30 | op << 29 | 0x0F000400u | pick(8) << 16 | cmode << 12 | pick(32) << 5 | v();
}

/* op0:op1:CRn:CRm:op2 of an MRS or MSR */
#define SYSREG(op1, crn, crm, op2) (3u << 19 | (op1) << 16 | (crn) << 12 | (crm) << 8 | (op2) << 5)
static uint32_t sys(void) {
    static const uint32_t reads[] = {SYSREG(3, 13, 0, 2), SYSREG(3, 13, 0, 3), SYSREG(3, 4, 4, 0),
                                     SYSREG(3, 4, 4, 1),  SYSREG(3, 4, 2, 0),  SYSREG(3, 0, 0, 1),
                                     SYSREG(3, 0, 0, 7),  SYSREG(3, 14, 0, 0)};
    static const uint32_t writes[] = {SYSREG(3, 13, 0, 2), SYSREG(3, 4, 4, 0), SYSREG(3, 4, 4, 1)};
    switch (pick(4)) {
    case 0:
    case 1: return 0xD5200000u | reads[pick(8)] | lo(); /* MRS */
    case 2: return 0xD5000000u | writes[pick(3)] | lo(); /* MSR */
    default: { /* DC ZVA, DC CVAC, DC CVAU, DC CIVAC, IC IVAU of a base */
        static const unsigned crm[] = {4, 10, 11, 14, 5};
        return 0xD5080000u | 3u << 16 | 7u << 12 | crm[pick(5)] << 8 | 1u << 5 | base();
    }
    }
}

static uint32_t fp(void) {
    unsigned t = pick(2), rd = v(), rn = v(), rm = v();
    switch (pick(17)) {
    case 0: return 0x1E202800u | t << 22 | rm << 16 | rn << 5 | rd; /* FADD */
    case 1: return 0x1E203800u | t << 22 | rm << 16 | rn << 5 | rd; /* FSUB */
    case 2: return 0x1E200800u | t << 22 | rm << 16 | rn << 5 | rd; /* FMUL */
    case 3: return 0x1E201800u | t << 22 | rm << 16 | rn << 5 | rd; /* FDIV */
    case 4: return 0x1F000000u | t << 22 | rm << 16 | v() << 10 | rn << 5 | rd; /* FMADD */
    case 5: return 0x1E202000u | t << 22 | rm << 16 | rn << 5 | pick(2) << 4; /* FCMP, FCMPE */
    case 6: return 0x1E200C00u | t << 22 | rm << 16 | pick(16) << 12 | rn << 5 | rd; /* FCSEL */
    case 7: return t ? 0x1E624000u | rn << 5 | rd : 0x1E22C000u | rn << 5 | rd; /* FCVT */
    case 8: return 0x1E220000u | pick(2) << 31 | t << 22 | lo() << 5 | rd; /* SCVTF (integer) */
    case 9: return 0x1E380000u | pick(2) << 31 | t << 22 | rn << 5 | lo(); /* FCVTZS (integer) */
    case 10: return 0x1E260000u | t << 22 | (t ? 1u << 31 : 0) | rn << 5 | lo(); /* FMOV W/X from S/D */
    case 11: return 0x1E270000u | t << 22 | (t ? 1u << 31 : 0) | lo() << 5 | rd; /* FMOV S/D from W/X */
    case 12: return 0x9EAF0000u | lo() << 5 | rd; /* FMOV Vd.D[1], Xn */
    case 13: return 0x1E201000u | t << 22 | pick(256) << 13 | rd; /* FMOV (scalar, immediate) */
    case 14: { /* SCVTF (fixed point): scale 64 - fbits, at least 32 from a W */
        unsigned sf = pick(2), scale = sf ? 1 + pick(63) : 32 + pick(32);
        return 0x1E020000u | sf << 31 | t << 22 | scale << 10 | lo() << 5 | rd;
    }
    case 15: return 0x1E214000u | t << 22 | rn << 5 | rd; /* FNEG */
    default: /* SCVTF, UCVTF, FCVTZS, FCVTZU (vector, integer) of a scalar */
        return (pick(2) ? 0x5E21D800u : 0x5EA1B800u) | pick(2) << 29 | t << 22 | rn << 5 | rd;
    }
}

/* SBFM, BFM, UBFM: the aliases' fields (LSR/ASR, LSL, SXTx/UXTx) a quarter of the time each, any fields else */
static uint32_t bitfield(void) {
    unsigned sf = pick(2), d = sf ? 64 : 32, imms = pick(d), immr = pick(d);
    switch (pick(4)) {
    case 0: imms = d - 1; break;
    case 1: immr = (imms + 1) % d; break;
    case 2: immr = 0, imms = pick(2) ? 7 : pick(2) ? 15 : 31; break;
    default: break;
    }
    return sf << 31 | pick(3) << 29 | 0x13000000u | sf << 22 | immr << 16 | imms << 10 | lo() << 5 | lo();
}
static uint32_t integer(void) {
    if (!pick(3)) return bitfield();
    if (!pick(3)) return int_ldst();
    switch (pick(4)) {
    case 0: return 0xAB000000u | lo() << 16 | lo() << 5 | lo();                 /* ADDS */
    case 1: return 0x9A800000u | lo() << 16 | pick(16) << 12 | lo() << 5 | lo(); /* CSEL */
    case 2: return 0xD2800000u | pick(4) << 21 | pick(65536) << 5 | lo();        /* MOVZ */
    default: return 0xEB000000u | lo() << 16 | lo() << 5 | lo();               /* SUBS */
    }
}

/* UMOV, SMOV, DUP (element, scalar and vector; general), INS (general, element), imm5 of any element size */
static uint32_t simd_copy(void) {
    unsigned size = pick(4), imm5 = (pick(32) << (size + 1) | 1u << size) & 31, q = pick(2);
    switch (pick(6)) {
    case 0: return 0x0E003C00u | (size == 3) << 30 | imm5 << 16 | v() << 5 | lo();          /* UMOV */
    case 1: return 0x0E002C00u | (size == 2 ? 1u : q) << 30 | (imm5 & 15 ? imm5 : 1) << 16 | v() << 5 | lo(); /* SMOV */
    case 2: return 0x5E000400u | imm5 << 16 | v() << 5 | v();                              /* DUP (element), scalar */
    case 3: return 0x0E000400u | (size == 3 ? 1u : q) << 30 | pick(2) << 11 | imm5 << 16 | (pick(2) ? lo() : v()) << 5 | v(); /* DUP */
    case 4: return 0x4E001C00u | imm5 << 16 | lo() << 5 | v();                             /* INS (general) */
    default: return 0x6E000400u | imm5 << 16 | (pick(16) << size & 15) << 11 | v() << 5 | v(); /* INS (element) */
    }
}

/* the AdvSIMD data processing x64_vec.c makes: words of the decoder's rows of these (llvm_qemu_words.c), registers
   and all as the rows' samples have them (no memory: any register will do) */
static const char* const k_vec_ops[] = {
    "AND_v", "BIC_v", "ORR_v", "ORN_v", "EOR_v", "BSL_v", "BIT_v", "BIF_v", "ADD_v", "SUB_v", "NEG_v", "ABS_v",
    "MUL_v", "MLA_v", "MLS_v", "MUL_vi", "MLA_vi", "MLS_vi", "CMEQ_v", "CMGT_v", "CMGE_v", "CMHI_v", "CMHS_v",
    "CMTST_v", "CMEQ0_v", "CMGT0_v", "CMGE0_v", "CMLE0_v", "CMLT0_v", "SMAX_v", "SMIN_v", "UMAX_v", "UMIN_v",
    "ADDP_v", "SMAXP_v", "SMINP_v", "UMAXP_v", "UMINP_v", "ADDV", "SMAXV", "SMINV", "UMAXV", "UMINV", "ZIP1", "ZIP2",
    "UZP1", "UZP2", "TRN1", "TRN2", "EXT_q", "REV16_v", "REV32_v", "REV64_v", "CNT_v", "TBL_TBX", "SHL_v", "USHR_v",
    "SSHR_v", "USRA_v", "SSRA_v", "SHL_s", "USHR_s", "SSHR_s", "SHRN_v", "XTN", "SSHLL_v", "USHLL_v", "SHLL_v",
    "USHL_v", "SSHL_v", "UADDW", "SADDW", "UMULL_v", "SMULL_v", "UMLAL_v", "SMLAL_v", "UMLSL_v", "SMLSL_v",
    "UMULL_vi", "SMULL_vi", "UMLAL_vi", "SMLAL_vi", "UMLSL_vi", "SMLSL_vi", "ADDHN", "SUBHN", "UHADD_v", "SHADD_v",
    "URHADD_v", "FADD_v", "FSUB_v", "FMUL_v", "FDIV_v", "FABD_v", "FABD_s", "FMAX_v", "FMIN_v", "FMAXNM_v",
    "FMINNM_v", "FMLA_v", "FMLS_v", "FMUL_vi", "FMLA_vi", "FMLS_vi", "FNEG_v", "FABS_v", "FSQRT_v", "FCMEQ_v",
    "FCMGE_v", "FCMGT_v", "FCMEQ_s", "FCMGE_s", "FCMGT_s", "FACGE_v", "FACGT_v", "FCMEQ0_v", "FCMGE0_v",
    "FCMGT0_v", "FCMLE0_v", "FCMLT0_v", "FCVTZS_vi", "FCVTZU_vi", "SCVTF_vi", "UCVTF_vi", "FCVTZS_vf",
    "FCVTZU_vf", "SCVTF_vf", "UCVTF_vf", "FCVTL_v", "FCVTN_v", "SCVTF_f", "UCVTF_f", "FCVTZS_f", "FCVTZU_f",
    "CMEQ0_s", "CMGT0_s", "CMGE0_s", "CMLE0_s", "CMLT0_s", "SQADD_v", "UQADD_v", "SQSUB_v", "UQSUB_v", "SQXTN_v",
    "UQXTN_v", "SQXTUN_v", "SQSHRN_v", "UQSHRN_v", "SQSHRUN_v", "SQDMULH_v", "SQRDMULH_v", "SQDMULH_vi",
    "SQRDMULH_vi", "SQSHL_v", "ADDP_s", "NOT_v", "EXT_d", "UADDLV", "SADDLV", "UADDLP_v", "SADDLP_v", "UADALP_v",
    "SADALP_v", "SHSUB_v", "UHSUB_v", "SRI_v", "SRI_s", "SLI_v", "SLI_s", "SRSHR_v", "URSHR_v", "SRSRA_v", "URSRA_v",
    "SRSHR_s", "URSHR_s", "SRSRA_s", "URSRA_s", "RSHRN_v", "UADDL_v", "SADDL_v", "USUBL_v", "SSUBL_v", "USUBW", "SSUBW",
    "UABD_v", "SABD_v", "UABA_v", "SABA_v", "UABDL_v", "SABDL_v", "UABAL_v", "SABAL_v", "CLZ_v", "CLS_v", "ADD_s",
    "SUB_s", "CMEQ_s", "CMGT_s", "CMGE_s", "CMHI_s", "CMHS_s", "CMTST_s", "SQADD_s", "UQADD_s", "SQSUB_s", "UQSUB_s",
    "SQXTN_s", "UQXTN_s", "SQXTUN_s", "SQSHRN_si", "UQSHRN_si", "SQSHRUN_si", "SQRSHRN_v", "UQRSHRN_v", "SQRSHRUN_v",
    "SQRSHRN_si", "UQRSHRN_si", "SQRSHRUN_si", "PMULL_p64", "FMUL_si", "FMLA_si", "FMLS_si", "FADDP_v", "FMAXP_v",
    "FMINP_v", "FMAXNMP_v", "FMINNMP_v", "FADDP_s", "FMAXP_s", "FMINP_s", "FMAXNMP_s", "FMINNMP_s", "FMAXV_s",
    "FMINV_s", "FMAXNMV_s", "FMINNMV_s", "FRECPE_v", "FRECPE_s", "FRSQRTE_v", "FRSQRTE_s", "FRECPS_v", "FRECPS_s",
    "FRSQRTS_v", "FRSQRTS_s", "FRINTN_v", "FRINTP_v", "FRINTM_v", "FRINTZ_v", "FRINTA_v", "AESE", "AESD", "AESMC",
    "AESIMC", "SHA1C", "SHA1P", "SHA1M", "SHA1SU0", "SHA1H", "SHA1SU1", "SHA256H", "SHA256H2", "SHA256SU0", "SHA256SU1",
    NULL};
static uint32_t* g_rows;
static size_t g_nrows;
static uint64_t rnd_for_rows(void) { return rnd(); }
/* the op the target decodes a word to, NULL where it is UNDEF there */
static const char* op_name(uint32_t w) {
    SnxA64Insn insn;
    return snx_a64_decode_word(w, g_features, &insn) != SNX_A64_OP_UNDEF ? snx_a64_op_text(insn.op) : NULL;
}
static void rows_init(void) {
    uint32_t* all;
    size_t n = lqd_row_words(6, rnd_for_rows, &all), i, k;
    g_rows = (uint32_t*)malloc((n + 1) * sizeof *g_rows);
    for (i = 0; i < n; ++i) {
        const char* name = op_name(all[i]);
        for (k = 0; name && k_vec_ops[k]; ++k)
            if (!strcmp(name, k_vec_ops[k])) {
                g_rows[g_nrows++] = all[i];
                break;
            }
    }
    free(all);
}
static uint32_t vec_row(void) { return g_nrows ? g_rows[pick((unsigned)g_nrows)] : 0xD503201Fu; /* NOP */ }

static uint32_t word(void) {
    switch (pick(12)) {
    case 9:
    case 10:
    case 11: return vec_row();
    case 8: return simd_copy();
    case 0:
    case 1:
    case 2: return fp_ldst();
    case 3: return simd_imm();
    case 4: return sys();
    case 5:
    case 6: return fp();
    default: return integer();
    }
}

/* ---- running ---- */

static void put32(uint8_t* mem, uint64_t a, uint32_t w) {
    mem[a] = (uint8_t)w, mem[a + 1] = (uint8_t)(w >> 8), mem[a + 2] = (uint8_t)(w >> 16), mem[a + 3] = (uint8_t)(w >> 24);
}

/* the start of a run: the registers as above, the SIMD&FP ones any bits or the floating-point values apart, FPCR's
   rounding mode, FZ, DN and AHP random in odd states */
static void start_state(CPU* c, unsigned s) {
    static const uint64_t fpv[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0x7ff0000000000000ull,
                                   0x7ff8000000000000ull, 0x7ff4000000000000ull, 0x000fffffffffffffull,
                                   0x3f8000007fc00000ull, 0x7f80000000000001ull, 0x41dfffffffc00000ull};
    unsigned r;
    for (r = 0; r < 16; ++r) c->x[r] = rnd();
    for (r = 20; r < 28; ++r) c->x[r] = DATA + 0x6000u + (rnd() % 0x4000u & ~15ull);
    c->x[31] = DATA + 0x6000u + (rnd() % 0x4000u & ~15ull);
    c->x[28] = rnd() % 256;
    for (r = 0; r < 32; ++r) {
        c->v[r].d[0] = rnd() & 1 ? rnd() : fpv[rnd() % 10];
        c->v[r].d[1] = rnd() & 1 ? rnd() : fpv[rnd() % 10];
    }
    c->n = rnd() & 1, c->z = rnd() & 1, c->cf = rnd() & 1, c->vf = rnd() & 1;
    c->fpcr = s & 1 ? (uint32_t)rnd() & (3u << 22 | 1u << 24 | 1u << 25 | 1u << 26) : 0;
    c->fpsr = (uint32_t)rnd() & 0x9f;
    c->tpidr = rnd();
    c->tpidrro = rnd();
}

/* a state of --ops: in three of four, the SIMD&FP registers' elements (of one size a register) are values where the
   integers' arithmetic turns, small ones (a shift by a register's amounts), or a float's: zeros, infinities, NaNs,
   denormals, the normals' ends, halfway cases, ordinary numbers; QC set in some, FZ16 random with FPCR's other bits */
static void ops_state(CPU* c, unsigned s) {
    static const uint64_t k32[] = {0, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x40000000u, 0x7f800000u, 0xff800000u,
                                   0x7fc00000u, 0xffc00001u, 0x7fa00000u, 0xff800001u, 0x00000001u, 0x80000001u,
                                   0x007fffffu, 0x00800000u, 0x80800000u, 0x7f7fffffu, 0xff7fffffu, 0x3f7fffffu,
                                   0x3f000000u, 0xbf000000u, 0x3fc00000u, 0x40200000u, 0x4b000001u, 0x4effffffu,
                                   0x4f000000u, 0xcf000000u, 0x5f000000u, 0x7e800000u, 0x7e800001u, 0x00200000u,
                                   0x40490fdbu, 0x33800000u, 0x7f000000u, 0x00400001u};
    static const uint64_t k64[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
                                   0x4000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
                                   0x7ff8000000000000ull, 0xfff8000000000001ull, 0x7ff4000000000000ull, 1,
                                   0x8000000000000001ull, 0x000fffffffffffffull, 0x0010000000000000ull,
                                   0x8010000000000000ull, 0x7fefffffffffffffull, 0xffefffffffffffffull,
                                   0x3fefffffffffffffull, 0x3fe0000000000000ull, 0xbfe0000000000000ull,
                                   0x3ff8000000000000ull, 0x4004000000000000ull, 0x4330000000000001ull,
                                   0x41dfffffffc00000ull, 0xc1e0000000000000ull, 0x43e0000000000000ull,
                                   0x7fd0000000000000ull, 0x7fd0000000000001ull, 0x0004000000000000ull,
                                   0x47efffffe0000000ull, 0x380fffffc0000000ull, 0x36a0000000000000ull};
    static const uint64_t k16[] = {0, 0x8000, 0x3c00, 0xbc00, 0x4000, 0x7c00, 0xfc00, 0x7e00, 0x7d00, 0xfe01,
                                   1, 0x8001, 0x3ff, 0x400, 0x8400, 0x7bff, 0xfbff, 0x3800, 0x3e00, 0x7800, 0x0100};
    unsigned r, i;
    start_state(c, s);
    if (s & 2) c->fpsr |= 1u << 27;
    if (s & 1) c->fpcr |= (uint32_t)rnd() & 1u << 19;
    if ((s >> 2 & 3) == 3) return;
    for (r = 0; r < 32; ++r) {
        unsigned size = (unsigned)(rnd() % 4), e = 8u << size, kind = (unsigned)(rnd() % 4);
        if (!kind) continue;
        for (i = 0; i < 128 / e; ++i) {
            uint64_t m = e == 64 ? ~0ull : (1ull << e) - 1, lane, top = 1ull << (e - 1);
            if (kind == 1) {
                static const int near[] = {0, 1, -1, 2, -2, 3};
                uint64_t base = rnd() % 3 ? (rnd() & 1 ? top : 0) : 1ull << (rnd() % e);
                lane = base + (uint64_t)(int64_t)near[rnd() % 6];
            } else if (kind == 2)
                lane = (uint64_t)((int64_t)(rnd() % (4 * e + 9)) - (int64_t)(2 * e + 4));
            else
                lane = e == 64 ? k64[rnd() % (sizeof k64 / sizeof *k64)]
                       : e == 32 ? k32[rnd() % (sizeof k32 / sizeof *k32)]
                       : e == 16 ? k16[rnd() % (sizeof k16 / sizeof *k16)]
                                 : rnd();
            lane &= m;
            c->v[r].d[i * e / 64] = (c->v[r].d[i * e / 64] & ~(m << (i * e % 64))) | lane << (i * e % 64);
        }
    }
}

static int differs(const CPU* a, const CPU* b) {
    return memcmp(a->x, b->x, sizeof a->x) || memcmp(a->v, b->v, sizeof a->v) || a->n != b->n || a->z != b->z ||
           a->cf != b->cf || a->vf != b->vf || a->fpcr != b->fpcr || a->fpsr != b->fpsr || a->tpidr != b->tpidr ||
           a->pc != b->pc;
}

static void report(const CPU* a, const CPU* b) {
    unsigned r;
    for (r = 0; r < 32; ++r)
        if (a->x[r] != b->x[r])
            fprintf(stderr, "  x%u: llvm %016llx x64 %016llx\n", r, (unsigned long long)a->x[r],
                    (unsigned long long)b->x[r]);
    for (r = 0; r < 32; ++r)
        if (memcmp(&a->v[r], &b->v[r], sizeof a->v[r]))
            fprintf(stderr, "  v%u: llvm %016llx:%016llx x64 %016llx:%016llx\n", r, (unsigned long long)a->v[r].d[1],
                    (unsigned long long)a->v[r].d[0], (unsigned long long)b->v[r].d[1],
                    (unsigned long long)b->v[r].d[0]);
    fprintf(stderr, "  nzcv llvm %u%u%u%u x64 %u%u%u%u; fpcr %08x %08x; fpsr %08x %08x; tpidr %llx %llx; pc %llx %llx\n",
            a->n, a->z, a->cf, a->vf, b->n, b->z, b->cf, b->vf, a->fpcr, b->fpcr, a->fpsr, b->fpsr,
            (unsigned long long)a->tpidr, (unsigned long long)b->tpidr, (unsigned long long)a->pc,
            (unsigned long long)b->pc);
}

/* an UNDEF or a BRK ends the run where it was (STOP's pc then: both back ends' the same) */
/* a fault in the code run: the function's words, for --words */
#include <signal.h>
static volatile uint64_t g_running;
static uint8_t* g_mem0;
static void on_fault(int sig) {
    char line[256];
    int n = snprintf(line, sizeof line, "FAULT %d in the function at %llx:", sig, (unsigned long long)g_running), k;
    for (k = 0; k < (int)SLOT - 1 && g_mem0; ++k) {
        uint64_t a = g_running + 4 * (uint64_t)k;
        uint32_t w = (uint32_t)g_mem0[a] | (uint32_t)g_mem0[a + 1] << 8 | (uint32_t)g_mem0[a + 2] << 16 |
                     (uint32_t)g_mem0[a + 3] << 24;
        n += snprintf(line + n, sizeof line - (size_t)n, " %08x", w);
        if (w == 0xD65F03C0u) break;
    }
    line[n++] = '\n';
    (void)!write(2, line, (size_t)n);
    _exit(3);
}
static void undef(CPU* c, uint64_t pc, uint32_t word) {
    (void)word, (void)pc;
    c->pc = STOP;
}
static void on_svc(CPU* c, uint32_t imm) { (void)c, (void)imm; }
#ifdef SNX_TEST_NATIVE
/* the native side's: a call no function takes (CALLDYN's miss) goes on at its target, which the run's loop finds */
static void on_miss(CPU* c, uint64_t entry) { c->pc = entry; }
static void on_poll(CPU* c) { (void)c; }
u64 snx_x64_fp_binary(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_fma(CPU* c, u64 add, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_unary(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_minmax(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_convert(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_round(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_round_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_to_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_from_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_jcvt(CPU* c, u64 a, u64 fpcr);
u64 snx_x64_fp_bfcvt(CPU* c, u64 a, u64 fpcr);
#endif
static void on_brk(CPU* c, uint64_t pc, uint32_t imm) {
    (void)imm, (void)pc;
    c->pc = STOP;
}

#ifdef SNX_TEST_NATIVE
/* what a big-endian machine's code shares with the test, turned over around its runs (host_unicorn.h's swapper): the
   state, the runtime's table and the dispatch table */
static struct {
    CPU* cpu;
    RtAPI* rt;
    uint64_t* disp;
    size_t ndisp;
} g_swap;
static void swap_shared(void* user) {
    (void)user;
    snx_test_swap_cpu(g_swap.cpu);
    snx_test_swap_rt(g_swap.rt);
    snx_test_swap_u64s(g_swap.disp, g_swap.ndisp);
}
#endif

/* the words of the decoder's ops named (NULL: every op of the target), in their ops' order, each once; with variants,
   each again with Rd its Rn and its Rm, where the word is still the op's */
typedef struct {
    const char* op;
    uint32_t word;
} OpWord;
static int by_op(const void* a, const void* b) {
    const OpWord *x = (const OpWord*)a, *y = (const OpWord*)b;
    int c = strcmp(x->op, y->op);
    return c ? c : x->word < y->word ? -1 : x->word > y->word;
}
static OpWord* op_words(char** ops, unsigned nops, int variants, size_t* count) {
    /* what the rows' samples of the system instructions hardly ever are: MRS and MSR of EL0's registers, DC ZVA, CVAC,
       CVAU and CIVAC, IC IVAU */
    static const uint32_t system[] = {
        0xD53BD041, 0xD53BD061, 0xD53B4401, 0xD53B4421, 0xD53B4201, 0xD53B0021, 0xD53B00E1, 0xD53BE001, 0xD53BE021,
        0xD53BE041, 0xD51BD041, 0xD51B4401, 0xD51B4421, 0xD51B4201, 0xD50B7434, 0xD50B7A34, 0xD50B7B34, 0xD50B7E34,
        0xD50B7534};
    const size_t nsystem = sizeof system / sizeof *system;
    uint32_t* all;
    size_t n = lqd_row_words(6, rnd_for_rows, &all), i, j = 0, k;
    OpWord* list = (OpWord*)malloc((3 * (n + nsystem) + 1) * sizeof *list);
    for (i = 0; i < n + nsystem; ++i) {
        uint32_t w = i < n ? all[i] : system[i - n], v;
        const char* name = op_name(w);
        for (k = 0; name && ops && k < nops; ++k)
            if (!strcmp(name, ops[k])) break;
        if (!name || (ops && k == nops)) continue;
        /* CNTPCT's and CNTVCT's time is another in each run */
        if (variants && ((w & ~31u) == 0xD53BE020u || (w & ~31u) == 0xD53BE040u)) continue;
        list[j++] = (OpWord){name, w};
        /* a system word's fields are no registers: DC ZVA of another would write anywhere */
        if (!variants || (w & 0xFFC00000u) == 0xD5000000u) continue;
        v = (w & ~31u) | (w >> 5 & 31);
        if (op_name(v) == name) list[j++] = (OpWord){name, v};
        v = (w & ~31u) | (w >> 16 & 31);
        if (op_name(v) == name) list[j++] = (OpWord){name, v};
    }
    free(all);
    qsort(list, j, sizeof *list, by_op);
    for (i = k = 0; i < j; ++i)
        if (!k || list[k - 1].word != list[i].word || list[k - 1].op != list[i].op) list[k++] = list[i];
    *count = k;
    return list;
}

/* how many of the functions at hints (each a word and a RET) of the code at CODE the back end makes; -1 an error */
static long made_count(uint8_t* code, size_t size, uint64_t* hints, size_t count) {
    SnxCodegen cg;
    SnxCodeImage view = {0};
    SnxNativeOptions options = {0};
    unsigned char *made, *object = NULL;
    size_t object_size = 0, n;
    long total = 0;
    char error[256];
    view.image = code;
    view.image_size = view.text_size = size;
    view.base = CODE;
    if (snx_codegen_analyze(&cg, &view, 1, hints, count, 1, g_features)) return -1;
    made = (unsigned char*)calloc(cg.function_count + 1, 1);
#ifdef SNX_TEST_NATIVE
    if (NATIVE_OBJECT(&cg, &options, made, &object, &object_size, error) < 0) total = -1;
#else
    if (snx_x64_object(&cg, 0, cg.function_count, &options, made, &object, &object_size, error, sizeof error) < 0)
        total = -1;
#endif
    if (total < 0) fprintf(stderr, "FAIL: %s\n", error);
    for (n = 0; total >= 0 && n < cg.function_count; ++n) total += made[n];
    /* what the analysis gave no function is not refused */
    if (total >= 0) total += (long)count - (long)cg.function_count;
    free(made), free(object);
    snx_codegen_free(&cg);
    return total;
}
/* --refused: each op's words made by the back end alone, and how many it left to LLVM; with ops, those ops' */
static int list_refused(char** names, unsigned nnames) {
    size_t count, i, j, ops = 0, left = 0, left_words = 0;
    OpWord* list = op_words(names, nnames, 0, &count);
    uint8_t* code = (uint8_t*)calloc(1, count * 8 + 8);
    uint64_t* at = (uint64_t*)malloc((count + 1) * sizeof *at);
    int bad = 0;
    for (i = 0; i < count && !bad; i = j) {
        size_t n = 0;
        long made;
        for (j = i; j < count && !strcmp(list[j].op, list[i].op); ++j, ++n) {
            put32(code, n * 8, list[j].word);
            put32(code, n * 8 + 4, 0xD65F03C0u);
            at[n] = CODE + n * 8;
        }
        ++ops;
        made = made_count(code, n * 8, at, n);
        if (made < 0) {
            fprintf(stderr, "FAIL: %s: not recompiled\n", list[i].op);
            bad = 1;
        } else if ((size_t)made < n) {
            /* the first word of those left, alone */
            uint32_t first = list[i].word;
            size_t k;
            for (k = i; k < j; ++k) {
                put32(code, 0, list[k].word);
                put32(code, 4, 0xD65F03C0u);
                at[0] = CODE;
                if (made_count(code, 8, at, 1) == 0) {
                    first = list[k].word;
                    break;
                }
            }
            printf("refused: %s, %lu of %lu words, the first %08x\n", list[i].op, (unsigned long)(n - (size_t)made),
                   (unsigned long)n, first);
            ++left;
            left_words += n - (size_t)made;
        }
    }
    printf("recompiler backends: %lu of %lu ops have words the back end leaves to LLVM, %lu of %lu words\n",
           (unsigned long)left, (unsigned long)ops, (unsigned long)left_words, (unsigned long)count);
    free(list), free(code), free(at);
    return bad || left;
}

int main(int argc, char** argv) {
    unsigned functions = 600, states = 8, i, s, k, nwords = 0, nops = 0;
    int refused = 0, must_make = 0;
    char** ops = NULL;
    OpWord* opw = NULL;
    uint32_t words[SLOT];
    struct snx_recompiler *rx = NULL, *rl = NULL;
    struct snx_recompiled *mx = NULL, *ml = NULL;
    uint8_t *mem[2] = {0}, *data0 = NULL;
    uint64_t* hints = NULL;
    int result = 1, hook = 0; /* every exit past the allocations frees them, so the sanitizers' leak check reads the
                                 test */
    RtAPI rt;
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--functions") && i + 1 < (unsigned)argc) functions = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--states") && i + 1 < (unsigned)argc)
            states = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < (unsigned)argc)
            seed = strtoull(argv[++i], NULL, 0) | 1;
        else if (!strcmp(argv[i], "--without") && i + 1 < (unsigned)argc)
            snx_x64_without(g_without = (unsigned)strtoul(argv[++i], NULL, 0));
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else if (!strcmp(argv[i], "--hook"))
            hook = 1;
        else if (!strcmp(argv[i], "--refused"))
            refused = 1;
        else if (!strcmp(argv[i], "--made"))
            must_make = 1;
        else if (!strcmp(argv[i], "--target") && i + 1 < (unsigned)argc) {
            const char* name = argv[++i];
            if (strcmp(name, "switch") && strcmp(name, "android") && strcmp(name, "ounce") && strcmp(name, "max")) {
                fprintf(stderr, "--target %s: the targets are switch, android, ounce and max\n", name);
                return 2;
            }
            g_features = !strcmp(name, "max")       ? snx_a64_features(SNX_A64_TARGET_MAX)
                         : !strcmp(name, "android") ? SNX_A64_TARGET_ANDROID
                         : !strcmp(name, "ounce")   ? SNX_A64_TARGET_OUNCE
                                                    : SNX_A64_TARGET_SWITCH;
        } else if (!strcmp(argv[i], "--ops")) {
            ops = argv + i + 1;
            while (i + 1 < (unsigned)argc && argv[i + 1][0] != '-') ++i, ++nops;
        }
        else if (!strcmp(argv[i], "--words")) {
            /* --words W...: one function of these words (and a RET), run from every state */
            while (i + 1 < (unsigned)argc && argv[i + 1][0] != '-' && nwords < SLOT - 1)
                words[nwords++] = (uint32_t)strtoul(argv[++i], NULL, 16);
            functions = 1;
        } else {
            fprintf(stderr, "%s: no such option\n", argv[i]);
            return 2;
        }
    }
    (void)hook;
    rows_init();
    if (refused) {
#ifndef SNX_TEST_NATIVE
        if (!snx_x64_available()) {
            printf("recompiler backends: no x86-64 back end on this host: nothing to list\n");
            return 0;
        }
#endif
        return list_refused(ops, nops);
    }
    if (g_features == SNX_A64_TARGET_OUNCE) {
        fprintf(stderr, "--target ounce: snx_recompiler.h has no such target to compare with: only with --refused\n");
        return 2;
    }
#ifdef SNX_TEST_NATIVE
    HostUc* uc = hostuc_new(NATIVE_UC, (size_t)64 << 20);
    const char* native_name = hostuc_name(NATIVE_UC);
    CPU* ucpu = NULL;
    RtAPI* urt = NULL;
    uint64_t* udisp = NULL;
    size_t made_native = 0;
    if (!uc) {
        printf("recompiler native: no Unicorn of the machine: nothing to compare\n");
        return 0;
    }
#endif
    rx = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    rl = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    if (!rx || !rl) {
        fprintf(stderr, "FAIL: no recompiler\n");
        goto out;
    }
    snx_recompiler_set_backend(rx, 1);
    snx_recompiler_set_backend(rl, 0);
    if (g_features != SNX_A64_TARGET_SWITCH) {
        int target = g_features == SNX_A64_TARGET_ANDROID ? SNX_RECOMPILER_A64_ANDROID : SNX_RECOMPILER_A64_MAX;
        (void)snx_recompiler_set_a64_target(rx, target), (void)snx_recompiler_set_a64_target(rl, target);
    }
    if (nops) {
        size_t count, n;
        opw = op_words(ops, nops, 1, &count);
        functions = (unsigned)count;
        for (i = 0; i < nops; ++i) {
            for (n = 0; n < count && strcmp(opw[n].op, ops[i]); ++n) {}
            if (n < count) continue;
            fprintf(stderr, "FAIL: --ops: the target has no word of %s\n", ops[i]);
            goto out;
        }
    }
    mem[0] = (uint8_t*)calloc(1, MEMORY);
#ifdef SNX_TEST_NATIVE
    mem[1] = (uint8_t*)hostuc_alloc(uc, MEMORY);
#else
    mem[1] = (uint8_t*)calloc(1, MEMORY);
#endif
    data0 = (uint8_t*)malloc(DATA_LEN);
    hints = (uint64_t*)malloc(functions * sizeof *hints);
    if (CODE + (uint64_t)functions * SLOT * 4 > DATA) {
        fprintf(stderr, "FAIL: %u functions do not fit in the code's %u bytes\n", functions, DATA - CODE);
        goto out;
    }
    /* the program, in both memories: each function its words then RET */
    for (i = 0; i < functions; ++i) {
        uint64_t at = CODE + (uint64_t)i * SLOT * 4;
        unsigned n = opw ? 1 : nwords ? nwords : 1 + pick(SLOT - 2);
        hints[i] = at;
        /* a quarter of them a loop: MOVZ x16, #1-3, the words, SUBS x16, x16, #1, B.NE back to the first word (x16
           no word writes); a branch forward in the words may leave it early */
        int loop = !nwords && !opw && n >= 4 && !pick(4);
        if (loop) {
            put32(mem[0], at, 0xD2800000u | (1 + pick(3)) << 5 | 16u);
            put32(mem[0], at + 4 * (n - 2), 0xF1000610u); /* SUBS x16, x16, #1 */
            put32(mem[0], at + 4 * (n - 1), 0x54000001u | ((0x80000u - (n - 2)) & 0x7FFFF) << 5); /* B.NE word 1 */
        }
        for (k = loop ? 1 : 0; k < (loop ? n - 2 : n); ++k) {
            uint32_t w = opw ? opw[i].word : nwords ? words[k] : word();
            if (!nwords && k + 1 < n && !pick(6)) {
                /* a branch forward over 1 to 3 words (to the RET at the most, to a loop's SUBS: past it the loop
                   wouldn't end): labels, and what is dead at them */
                unsigned end = loop ? n - 2 : n, skip = 1 + pick(end - k < 4 ? end - k : 3), r = lo();
                switch (pick(3)) {
                case 0: w = 0x54000000u | skip << 5 | pick(14); break;                                 /* B.cond */
                case 1: w = pick(2) << 31 | 0x34000000u | pick(2) << 24 | skip << 5 | r; break;        /* CBZ, CBNZ */
                default: w = 0x36000000u | pick(2) << 24 | pick(32) << 19 | skip << 5 | r; break;     /* TBZ, TBNZ */
                }
            }
            put32(mem[0], at + 4 * k, w);
        }
        put32(mem[0], at + 4 * n, 0xD65F03C0u);
        for (k = n; k + 1 < SLOT; ++k) put32(mem[0], at + 4 * (k + 1), 0xD65F03C0u);
    }
    memcpy(mem[1], mem[0], MEMORY);
    {
        struct snx_code_range range = {CODE, mem[0] + CODE, (size_t)functions * SLOT * 4};
#ifndef SNX_TEST_NATIVE
        mx = snx_recompile_ranges(rx, &range, 1, hints, functions);
#endif
        range.code = mem[1] + CODE;
        ml = snx_recompile_ranges(rl, &range, 1, hints, functions);
    }
#ifdef SNX_TEST_NATIVE
    {
        /* the program's functions as the native back end's object, loaded into Unicorn, the code range's table
           (CPU.disp) of their addresses there */
        SnxCodegen cg;
        SnxCodeImage view;
        SnxNativeOptions options;
        unsigned char *made = NULL, *object = NULL;
        size_t object_size = 0, n;
        char error[256];
        memset(&view, 0, sizeof view);
        memset(&options, 0, sizeof options);
        view.image = mem[1] + CODE;
        view.image_size = (size_t)functions * SLOT * 4;
        view.base = CODE;
        view.text_off = 0;
        view.text_size = view.image_size;
        if (snx_codegen_analyze(&cg, &view, 1, hints, functions, 1, g_features)) {
            fprintf(stderr, "FAIL: no functions found\n");
            goto out;
        }
        made = (unsigned char*)calloc(cg.function_count + 1, 1);
        cg.hook_direct_calls = hook;
        if (NATIVE_OBJECT(&cg, &options, made, &object, &object_size, error) < 0) {
            fprintf(stderr, "FAIL: the %s object: %s\n", native_name, error);
            goto out;
        }
#define NATIVE(f) hostuc_native(uc, #f, (void*)f)
        NATIVE(snx_x64_fp_binary), NATIVE(snx_x64_fp_fma), NATIVE(snx_x64_fp_unary), NATIVE(snx_x64_fp_minmax);
        NATIVE(snx_x64_fp_compare), NATIVE(snx_x64_fp_convert), NATIVE(snx_x64_fp_round);
        NATIVE(snx_x64_fp_round_int), NATIVE(snx_x64_fp_to_int), NATIVE(snx_x64_fp_from_int), NATIVE(snx_x64_fp_jcvt);
        NATIVE(snx_x64_fp_bfcvt), NATIVE(snx_mrs), NATIVE(snx_msr), NATIVE(snx_ic_ivau);
        NATIVE(snx_x64_crc32), NATIVE(snx_x64_stxp);
        hostuc_native_wide(uc, "snx_x64_casp", (void*)snx_x64_casp);
#undef NATIVE
        if (object && hostuc_load(uc, object, object_size, error, sizeof error)) {
            fprintf(stderr, "FAIL: loading the %s object: %s\n", native_name, error);
            goto out;
        }
        for (n = 0; n < cg.function_count; ++n) made_native += made[n];
        printf("recompiler %s: %zu functions, the %s back end made %zu (%zu bytes)\n", native_name,
               cg.function_count, native_name, made_native, object_size);
        ucpu = (CPU*)hostuc_alloc(uc, sizeof *ucpu);
        urt = (RtAPI*)hostuc_alloc(uc, sizeof *urt);
        udisp = (uint64_t*)hostuc_alloc(uc, (size_t)functions * SLOT * 8 + 8);
        g_swap.cpu = ucpu, g_swap.rt = urt, g_swap.disp = udisp, g_swap.ndisp = (size_t)functions * SLOT + 1;
        hostuc_swapper(uc, swap_shared, NULL);
        for (n = 0; n < functions; ++n) {
            char name[40];
            snprintf(name, sizeof name, "F_%llX", (unsigned long long)hints[n]);
            udisp[(hints[n] - CODE) / 4] = hostuc_symbol(uc, name);
        }
        free(made), free(object);
        snx_codegen_free(&cg);
    }
    if (!ml) {
#else
    if (!mx || !ml) {
#endif
        fprintf(stderr, "FAIL: recompiling: %s / %s\n", mx ? "" : snx_recompiler_error(rx),
                ml ? "" : snx_recompiler_error(rl));
        goto out;
    }
    printf("recompiler backends: %lu words of the decoder's AdvSIMD rows to draw from\n", (unsigned long)g_nrows);
#ifdef SNX_TEST_NATIVE
    if (opw && made_native < functions) {
        ++failures;
        fprintf(stderr, "FAIL: the %s back end made %zu of the ops' %u words\n", native_name, made_native, functions);
        (void)list_refused(ops, nops);
    } else if (!opw && made_native * 100 < (size_t)functions * (must_make ? 100 : NATIVE_MIN_MADE)) {
        fprintf(stderr, "FAIL: the %s back end made %zu of the %u functions\n", native_name, made_native, functions);
        ++failures;
    }
    if (0) {
#else
    printf("recompiler backends: %lu functions, the x86-64 back end made %lu\n",
           (unsigned long)snx_recompiled_functions(mx), (unsigned long)snx_recompiled_x64_functions(mx));
    /* a word the back end doesn't make is LLVM's on both sides: with --ops, and the random programs, it fails below */
    if (!snx_x64_available() || (nwords && !must_make && !snx_recompiled_x64_functions(mx))) {
#endif
        printf("recompiler backends: %s: nothing to compare\n",
               nwords ? "the back end leaves the function to LLVM" : "no x86-64 back end on this host");
        result = 0;
        goto out;
    }
#ifndef SNX_TEST_NATIVE
    if (opw && snx_recompiled_x64_functions(mx) < snx_recompiled_functions(mx)) {
        ++failures;
        fprintf(stderr, "FAIL: the x86-64 back end made %lu of the ops' %lu words\n",
                (unsigned long)snx_recompiled_x64_functions(mx), (unsigned long)snx_recompiled_functions(mx));
        (void)list_refused(ops, nops);
    } else if (snx_recompiled_x64_functions(mx) * 10 < snx_recompiled_functions(mx) * 9) {
        fprintf(stderr, "FAIL: the x86-64 back end made under 90%% of the functions\n");
        ++failures;
    }
#endif
    g_mem0 = mem[0];
#ifdef SIGBUS
    signal(SIGBUS, on_fault);
#endif
    signal(SIGSEGV, on_fault);
    signal(SIGILL, on_fault);
    snx_runtime_defaults(&rt);
    rt.undef = undef;
    rt.svc = on_svc;
    rt.brk = on_brk;
#ifdef SNX_TEST_NATIVE
    *urt = (RtAPI){
        .undef = (void (*)(CPU*, u64, u32))(uintptr_t)hostuc_native(uc, "undef", (void*)undef),
        .svc = (void (*)(CPU*, u32))(uintptr_t)hostuc_native(uc, "svc", (void*)on_svc),
        .brk = (void (*)(CPU*, u64, u32))(uintptr_t)hostuc_native(uc, "brk", (void*)on_brk),
        .miss = (GuestFn)(uintptr_t)hostuc_native(uc, "miss", (void*)on_miss),
        .poll = (void (*)(CPU*))(uintptr_t)hostuc_native(uc, "poll", (void*)on_poll),
        .cas128 = (uint32_t (*)(uint64_t*, uint64_t, uint64_t, uint64_t, uint64_t))(uintptr_t)hostuc_native(
            uc, "cas128", (void*)snx_runtime_cas128),
    };
#endif
    for (k = 0; k < DATA_LEN; ++k) data0[k] = (uint8_t)rnd();
    for (i = 0; i < functions; ++i)
        for (s = 0; s < states; ++s) {
            CPU c[2], start;
            memset(&start, 0, sizeof start);
            if (opw) ops_state(&start, s);
            else
                start_state(&start, s);
#ifdef SNX_TEST_NATIVE
            {
                /* the native side: F_<entry> under Unicorn, then the function of each pc a call left (a miss) */
                uint64_t fn;
                unsigned hops = 0;
                char name[40];
                snprintf(name, sizeof name, "F_%llX", (unsigned long long)hints[i]);
                fn = hostuc_symbol(uc, name);
                if (!fn) break; /* not made: another back end's */
                memcpy(mem[1] + DATA, data0, DATA_LEN);
                *ucpu = start;
                ucpu->mem = mem[1];
                ucpu->rt = urt;
                ucpu->x[30] = STOP;
                ucpu->disp = (SNX_ATOMIC(GuestFn)*)(void*)udisp;
                ucpu->code_lo = CODE;
                ucpu->code_hi = CODE + (uint64_t)functions * SLOT * 4;
                ucpu->pc = hints[i];
                g_running = hints[i];
                for (;;) {
                    uint64_t entry = ucpu->pc;
                    if (hostuc_call(uc, fn, (uint64_t)(uintptr_t)ucpu, entry, NULL)) {
                        ++failures;
                        fprintf(stderr, "FAIL: the function at %llx, state %u: the %s code stopped\n",
                                (unsigned long long)hints[i], s, native_name);
                        break;
                    }
                    if (ucpu->pc == STOP || ++hops > 64 || ucpu->pc < CODE || ucpu->pc >= ucpu->code_hi) break;
                    fn = udisp[(ucpu->pc - CODE) / 4];
                    if (!fn) break;
                }
                c[1] = *ucpu;
            }
            for (k = 0; k < 1; ++k) {
                struct snx_recompiled* m = ml;
#else
            for (k = 0; k < 2; ++k) {
                struct snx_recompiled* m = k ? mx : ml;
#endif
                memcpy(mem[k] + DATA, data0, DATA_LEN);
                c[k] = start;
                c[k].mem = mem[k];
                c[k].rt = &rt;
                c[k].x[30] = STOP;
                if (snx_recompiled_bind(m, &c[k])) {
                    fprintf(stderr, "FAIL: bind\n");
                    goto out;
                }
                g_running = hints[i];
                snx_recompiled_run(m, &c[k], hints[i], STOP);
            }
            c[0].mem = c[1].mem = NULL;
            c[0].disp = c[1].disp = NULL;
            if (differs(&c[0], &c[1]) || memcmp(mem[0] + DATA, mem[1] + DATA, DATA_LEN)) {
                unsigned n;
                ++failures;
                fprintf(stderr, "FAIL: the function at %llx, state %u:", (unsigned long long)hints[i], s);
                for (n = 0; n + 1 < SLOT; ++n) {
                    uint64_t a = hints[i] + 4 * n;
                    uint32_t w = (uint32_t)mem[0][a] | (uint32_t)mem[0][a + 1] << 8 | (uint32_t)mem[0][a + 2] << 16 |
                                 (uint32_t)mem[0][a + 3] << 24;
                    fprintf(stderr, " %08x", w);
                    if (w == 0xD65F03C0u) break;
                }
                fprintf(stderr, "%s\n", memcmp(mem[0] + DATA, mem[1] + DATA, DATA_LEN) ? " (memory differs)" : "");
                if (verbose) report(&c[0], &c[1]);
                if (failures > 40) goto done;
            }
        }
done:
    printf("recompiler backends: %u functions, %u states each: %d failures\n", functions, states, failures);
    result = failures != 0;
out:
    snx_recompiled_free(mx);
    snx_recompiled_free(ml);
#ifdef SNX_TEST_NATIVE
    hostuc_free(uc);
    mem[1] = NULL; /* Unicorn's mapping */
#endif
    snx_recompiler_free(rx);
    snx_recompiler_free(rl);
    free(mem[0]), free(mem[1]), free(data0), free(hints), free(opw);
    return result;
}
