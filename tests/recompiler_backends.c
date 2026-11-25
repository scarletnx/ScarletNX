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
         [--hook] [--jit] [--words HEX...] [--made] [--ops OP...] [--refused] [--coverage]
         [--qemu QEMU RUNNER DIR] [--programs] [--long N BYTES]
   --without MASK: the x86-64 back end goes without these features of the host (sailgen's SGX_F_*: FMA 0x800, AVX 0x400,
   F16C 0x100, BMI1, BMI2 and LZCNT 7, AES-NI 0x1000, SHA-NI 0x2000), as on a host that lacks them; arm64's without its
   (SNX_ARM64_F_*)
   --long N BYTES: --words' words N times over in one function, entered past them by a CBZ, gone over twice by a B.NE
   back, and left by a B back over them: more than BYTES of a native back end's code, which is what its short jumps reach
   --target NAME: the guest's features, switch (the default), android, max or, with --refused, ounce
   --made: the back end must make --words' function (else, where LLVM makes it, there is nothing to compare)
   --ops OP...: every word of these ops of the decoder (llvm_qemu_words.c's samples, and each value of its small fields)
   is a function of its own with a RET, and the back end must make them all; they run from as many states as the rest,
   half of them with lanes of the values where an integer's or a float's arithmetic turns
   --refused: each of the target's ops that has words the back end leaves to LLVM, how many of its words, and stops
   there, failing where there is one
   --qemu: with --ops, their words' results held to qemu-aarch64 -cpu max's (tests/qemu_ref: its path, the runner's, a
   folder for its files) rather than to LLVM's, which lacks what no guest's CPU has; not loads nor stores
   --programs: what LLVM's back end and QEMU's one word don't show, as C says it: FEAT_CMPBR's compare and branch,
   FEAT_MOPS's SET and CPY, FEAT_LSE128's atomics
   --coverage: a line of each of the target's ops, how many of its words the back end makes with its own instructions
   alone, with an exact helper's call, and leaves, and the first of those, and stops there
   On a host without the back end (not x86-64, or built without SNX_SAIL_X64) it passes, saying so.

   Built with SNX_TEST_ARM64 or SNX_TEST_RV64 (snx-test-recompiler-arm64, -rv64), the same programs hold that native
   back end (src/common/cpu/recompiler/arm64, rv64) to LLVM on any host: its object, made for the program's
   functions, runs under Unicorn (support/host_unicorn.c), the native side's memory and state mapped into it, the
   helpers and the runtime's functions it calls trapped to this process's. --hook makes it with every call through
   CPU.disp (hook_direct_calls, as NVN's handlers want), which holds each function; --jit makes it a JIT unit, its
   functions its table's. With SNX_TEST_WIN64 (snx-test-recompiler-win64) it is the x86-64 back end's code for Windows
   that runs, on an x86-64 host itself, entered and called out of as that convention wants, its frames unwound by its
   function table where it calls out, or with --step after every instruction (support/host_win64.c) */
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "llvm_qemu_words.h"
#include "qemu_ref/qemu_ref.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(SNX_TEST_ARM64) || defined(SNX_TEST_RV64) || defined(SNX_TEST_PPC64) || defined(SNX_TEST_MIPS64) ||        \
    defined(SNX_TEST_LA64) || defined(SNX_TEST_PPC64BE) || defined(SNX_TEST_MIPS64BE) || defined(SNX_TEST_S390X) ||    \
    defined(SNX_TEST_WIN64)
#define SNX_TEST_NATIVE 1
#include "common/cpu/recompiler/llvm/elf_object.h"
#include "support/host_unicorn.h"
#define SNX_TEST_SWAP_CPU 1
#include "support/swap_state.h"
#if defined(SNX_TEST_ARM64)
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#define NATIVE_UC HOSTUC_ARM64
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_arm64_object(cg, 0, (cg)->function_count, options, SNX_ARM64_F_ALL & ~g_without, made, object, size, error,    \
                     sizeof error)
#elif defined(SNX_TEST_WIN64)
#define NATIVE_UC       HOSTUC_WIN64
#define NATIVE_MIN_MADE 90
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                          \
    snx_x64_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
void snx_x64_fp_vec(CPU* c, void* out, const void* x, const void* y, const void* z, u32 desc);
void snx_x64_crypto(CPU* c, void* out, const void* x, const void* y, const void* z, u32 op);
extern const u32 snx_x64_crc32_table[256];
#elif defined(SNX_TEST_LA64)
#include "common/cpu/recompiler/la64/la64_codegen.h"
#define NATIVE_UC HOSTUC_LA64
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_la64_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#elif defined(SNX_TEST_PPC64BE)
#include "common/cpu/recompiler/ppc64/ppc64be_codegen.h"
#define NATIVE_UC HOSTUC_PPC64
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_ppc64be_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#elif defined(SNX_TEST_S390X)
#include "common/cpu/recompiler/s390x/s390x_codegen.h"
#define NATIVE_UC HOSTUC_S390X
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_s390x_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#elif defined(SNX_TEST_MIPS64BE)
#include "common/cpu/recompiler/mips64/mips64be_codegen.h"
#define NATIVE_UC HOSTUC_MIPS64
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_mips64be_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#elif defined(SNX_TEST_MIPS64)
#include "common/cpu/recompiler/mips64/mips64_codegen.h"
#define NATIVE_UC HOSTUC_MIPS64EL
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_mips64_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#elif defined(SNX_TEST_PPC64)
#include "common/cpu/recompiler/ppc64/ppc64_codegen.h"
#define NATIVE_UC HOSTUC_PPC64LE
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_ppc64_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#else
#include "common/cpu/recompiler/rv64/rv64_codegen.h"
#define NATIVE_UC HOSTUC_RV64
#define NATIVE_OBJECT(cg, options, made, object, size, error)                                                         \
    snx_rv64_object(cg, 0, (cg)->function_count, options, made, object, size, error, sizeof error)
#endif
u64 snx_x64_crc32(u64 acc, u64 value, u32 desc);
u64 snx_x64_aes(u64 x, u32 op);
u64 snx_x64_estimate(u64 a, u32 sqrt);
u64 snx_x64_half(u64 a, u64 fpcr);
u64 snx_x64_bfdot(u64 sum, u64 a, u64 b);
u64 snx_x64_sm4_box(u64 a);
u64 snx_x64_fp_scale(CPU* c, u64 a, u64 by, u64 fpcr, u32 desc);
u64 snx_x64_fp8_widen(CPU* c, u64 a, u32 bfloat);
u64 snx_x64_fp8_narrow(CPU* c, u64 a, u32 width);
u64 snx_x64_fp8_dot(u64 add, u64 a, u64 b, u32 desc);
void snx_x64_set(CPU* c, u64 a, u64 size, u64 value);
void snx_x64_copy(CPU* c, u64 to, u64 from, u64 size);
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
static int g_qemu; /* --qemu: the end compared is QEMU's, of the word alone */
static void undef(CPU* c, uint64_t pc, uint32_t word) {
    /* what it was passed, in the state the sides are compared by; not QEMU's, which has no RET of a word's X30 */
    if (!g_qemu) c->x[17] ^= pc ^ (uint64_t)word << 32;
    c->pc = STOP;
}
static void on_svc(CPU* c, uint32_t imm) { (void)c, (void)imm; }
static void on_poll(CPU* c) { atomic_store_explicit(&c->poll, 0, memory_order_relaxed); }
#ifdef SNX_TEST_NATIVE
/* the native side's: a call no function takes (CALLDYN's miss) goes on at its target, which the run's loop finds */
static void on_miss(CPU* c, uint64_t entry) { c->pc = entry; }
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
    size_t n = variants ? lqd_row_words(6, rnd_for_rows, &all) : lqd_all_row_words(6, rnd_for_rows, &all), i, j = 0, k;
    OpWord* list = (OpWord*)malloc((3 * (n + nsystem) + 1) * sizeof *list);
    for (i = 0; i < n + nsystem; ++i) {
        uint32_t w = i < n ? all[i] : system[i - n], v;
        const char* name = op_name(w);
        for (k = 0; name && ops && k < nops; ++k)
            if (!strcmp(name, ops[k])) break;
        if (!name || (ops && k == nops)) continue;
        /* CNTPCT's and CNTVCT's time is another in each run */
        if (variants && ((w & ~31u) == 0xD53BE020u || (w & ~31u) == 0xD53BE040u)) continue;
        /* a load's or store's base is an address, which the link register, RET's, is not */
        if (variants && (w >> 5 & 31) == 30 && (!strncmp(name, "LD", 2) || !strncmp(name, "ST", 2))) continue;
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

/* how many of the functions at hints (each a word and a RET) of the code at CODE the back end makes; -1 an error.
   *helped (where not NULL): the object calls one of the exact helpers (x64_helpers.c) */
static long made_count(uint8_t* code, size_t size, uint64_t* hints, size_t count, int* helped) {
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
    for (n = 0; helped && n + 8 <= object_size; ++n)
        if (!memcmp(object + n, "snx_x64_", 8)) {
            *helped = 1;
            break;
        }
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
        made = made_count(code, n * 8, at, n, NULL);
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
                if (made_count(code, 8, at, 1, NULL) == 0) {
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

/* --coverage: each of the target's ops, how many of its words the back end makes with no helper, with one, and
   leaves */
static int list_coverage(void) {
    size_t count, i, j;
    OpWord* list = op_words(NULL, 0, 0, &count);
    uint8_t code[8];
    uint64_t at = CODE;
    for (i = 0; i < count; i = j) {
        unsigned n[3] = {0, 0, 0};
        uint32_t first = 0;
        for (j = i; j < count && !strcmp(list[j].op, list[i].op); ++j) {
            int helped = 0;
            long made;
            put32(code, 0, list[j].word);
            put32(code, 4, 0xD65F03C0u);
            made = made_count(code, sizeof code, &at, 1, &helped);
            if (made < 0) return 1;
            if (!made && !n[2]) first = list[j].word;
            ++n[made ? helped : 2];
        }
        printf("%s\t%u\t%u\t%u\t%08x\n", list[i].op, n[0], n[1], n[2], first);
    }
    free(list);
    return 0;
}

/* --programs: CB<cc>, CBB<cc> and CBH<cc> of x1 and x2 and of x1 and an integer, over an ADD; SET's and CPY's three, of
   x9, x10 and x11; LDCLRP, LDSETP and SWPP of x1 and x2 at x20. The words after each one's first in more, 0 for none */
static OpWord* programs(size_t* count, uint32_t (**more)[2]) {
    static const unsigned ccs[] = {0, 1, 2, 3, 6, 7}, integers[] = {0, 1, 31, 32, 63};
    static const struct {
        const char* op;
        uint32_t w, step;
    } moves[] = {{"SETP", 0x19C00400u, 0x4000u}, {"SETGP", 0x1DC00400u, 0x4000u}, {"CPYFP", 0x19000400u, 0x400000u},
                 {"CPYP", 0x1D000400u, 0x400000u}};
    static const struct {
        const char* op;
        uint32_t w;
    } pairs[] = {{"LDCLRP", 0x19201000u}, {"LDSETP", 0x19203000u}, {"SWPP", 0x19208000u}};
    OpWord* list = (OpWord*)malloc(sizeof *list * 96);
    uint32_t (*rest)[2] = calloc(96, sizeof *rest);
    unsigned c, k;
    size_t n = 0;
    for (c = 0; c < 6; ++c) {
        uint32_t w = 0x74000000u | ccs[c] << 21 | 2u << 16 | 2u << 5 | 1u;
        list[n++] = (OpWord){"CB_cond", w};
        list[n++] = (OpWord){"CB_cond", w | 1u << 31};
        list[n++] = (OpWord){"CB_cond", w | 2u << 14};
        list[n++] = (OpWord){"CB_cond", w | 3u << 14};
        for (k = 0; k < 5; ++k)
            list[n++] = (OpWord){"CB_cond_imm",
                                 0x75000000u | (k & 1) << 31 | ccs[c] << 21 | integers[k] << 15 | 2u << 5 | 1u};
    }
    for (k = 0; k < n; ++k) rest[k][0] = 0x91000400u; /* ADD x0, x0, #1 */
    for (k = 0; k < 4; ++k) {
        uint32_t w = moves[k].w | 10u << 16 | 11u << 5 | 9u;
        rest[n][0] = w + moves[k].step, rest[n][1] = w + 2 * moves[k].step;
        list[n++] = (OpWord){moves[k].op, w};
    }
    for (k = 0; k < 8; ++k) { /* the second and the third alone, where the first did not do it all */
        static const uint32_t alone[8] = {0x19C04400u, 0x19C08400u, 0x1DC04400u, 0x1DC08400u,
                                          0x19400400u, 0x19800400u, 0x1D400400u, 0x1D800400u};
        list[n++] = (OpWord){k < 4 ? "SETM" : "CPYM", alone[k] | 10u << 16 | 11u << 5 | 9u};
    }
    for (k = 0; k < 12; ++k) { /* with and without acquire (bit 23) and release (22), an ADD before it */
        rest[n][0] = pairs[k / 4].w | (k & 3) << 22 | 2u << 16 | 20u << 5 | 1u;
        list[n++] = (OpWord){pairs[k / 4].op, 0x91000400u};
    }
    *count = n;
    *more = rest;
    return list;
}
/* does a CB branch, of these x1 and x2 */
static int branch_taken(uint32_t w, uint64_t a, uint64_t b) {
    unsigned cc = w >> 21 & 7, bits;
    int64_t sa, sb;
    if (w >> 24 & 1) bits = w >> 31 ? 64 : 32, b = w >> 15 & 63;
    else
        bits = w >> 31 ? 64 : (w >> 14 & 3) == 2 ? 8 : (w >> 14 & 3) == 3 ? 16 : 32;
    if (bits < 64) a &= (1ull << bits) - 1, b &= (1ull << bits) - 1;
    sa = (int64_t)(a << (64 - bits)), sb = (int64_t)(b << (64 - bits)); /* their signs at the top: the same order */
    switch (cc) {
    case 0: return sa > sb;
    case 1: return w >> 24 & 1 ? sa < sb : sa >= sb;
    case 2: return a > b;
    case 3: return w >> 24 & 1 ? a < b : a >= b;
    case 6: return a == b;
    default: return a != b;
    }
}

/* --programs: the writes of another thread to come between a pair's read and its compare and swap */
static unsigned g_between;
#if defined(SNX_TEST_NATIVE) && !defined(SNX_TEST_WIN64)
static typeof(snx_x64_casp(NULL, 0, 0, 0, 0, 0)) casp_between(CPU* c, u64 a, u64 e0, u64 e1, u64 d0, u64 d1) {
    if (g_between) --g_between, c->mem[a] ^= 0x10;
    return snx_x64_casp(c, a, e0, e1, d0, d1);
}
#endif
/* what a program makes of the state and of the memory, written by another thread first where between */
static void program_end(const OpWord* p, CPU* c, uint8_t* mem, int between) {
    uint64_t d = c->x[9], s = c->x[10], n = c->x[11], lo, hi;
    if (between) mem[c->x[20]] ^= 0x10;
    if (!strncmp(p->op, "CB", 2)) c->x[0] += !branch_taken(p->word, c->x[1], c->x[2]);
    else if (!strcmp(p->op, "SETM") || !strcmp(p->op, "CPYM")) {
        /* the second one alone: Xn below 0 so many bytes still to do before Xd and Xs, above 0 a backward copy's */
        uint64_t left = (int64_t)n < 0 ? 0 - n : n, before = (int64_t)n < 0 ? left : 0;
        if (p->op[0] == 'S') memset(mem + d - before, (int)(s & 255), left);
        else
            memmove(mem + d - before, mem + s - before, left);
        c->x[11] = 0;
    } else if (!strncmp(p->op, "SET", 3)) {
        memset(mem + d, (int)(s & 255), n);
        c->x[9] = d + n, c->x[11] = 0;
        c->n = c->z = c->cf = c->vf = 0;
    } else if (!strncmp(p->op, "CPY", 3)) {
        int back = p->op[3] == 'P' && d > s && d < s + n;
        memmove(mem + d, mem + s, n);
        c->x[9] = back ? d : d + n, c->x[10] = back ? s : s + n, c->x[11] = 0;
        c->n = c->z = c->cf = c->vf = 0;
    } else {
        memcpy(&lo, mem + c->x[20], 8), memcpy(&hi, mem + c->x[20] + 8, 8);
        uint64_t a = c->x[1], b = c->x[2];
        c->x[0] += 1, c->x[1] = lo, c->x[2] = hi;
        if (p->op[2] == 'C') lo &= ~a, hi &= ~b;
        else if (p->op[0] == 'L')
            lo |= a, hi |= b;
        else
            lo = a, hi = b;
        memcpy(mem + c->x[20], &lo, 8), memcpy(mem + c->x[20] + 8, &hi, 8);
    }
}

/* the register that stands for Vn in QEMU's FCVTN2 over its own Vn, which QEMU (11.1.2) reads after writing it; 32 for
   any other word */
static unsigned own_vn(const OpWord* p) {
    unsigned rd = p->word & 31, rm = p->word >> 16 & 31;
    if (strcmp(p->op, "FCVTN_bs") || !(p->word >> 30 & 1) || rd != (p->word >> 5 & 31)) return 32;
    return (rd ^ 1) == rm ? rd ^ 2 : rd ^ 1;
}

/* each word's end states under QEMU (how: its path, the runner's, a folder) from the starts, states of each; 0 or -1 */
static int qemu_ends(const OpWord* words, size_t count, unsigned states, const CPU* starts, CPU* ends,
                     char* const* how) {
    enum { BATCH = 2048 };
    SnxQrefBatch batch = {0};
    size_t total = count * states, done, k;
    int result = -1;
    char error[512];
    for (done = 0; done < total; done += BATCH) {
        size_t n = total - done < BATCH ? total - done : BATCH;
        snx_qref_clear(&batch);
        for (k = 0; k < n; ++k) {
            const CPU* s = &starts[done + k];
            SnxQrefCase c;
            uint32_t word = words[(done + k) / states].word;
            unsigned r, spare;
            snx_qref_case_init(&c);
            for (r = 0; r < 31; ++r) c.state.x[r] = s->x[r];
            c.state.sp = s->x[31];
            c.state.nzcv = (uint32_t)s->n << 31 | (uint32_t)s->z << 30 | (uint32_t)s->cf << 29 | (uint32_t)s->vf << 28;
            c.state.fpcr = s->fpcr;
            c.state.fpsr = s->fpsr;
            for (r = 0; r < 32; ++r) c.state.v[r][0] = s->v[r].d[0], c.state.v[r][1] = s->v[r].d[1];
            spare = own_vn(&words[(done + k) / states]);
            if (spare < 32) {
                c.state.v[spare][0] = s->v[word & 31].d[0], c.state.v[spare][1] = s->v[word & 31].d[1];
                word = (word & ~(31u << 5)) | spare << 5;
            }
            if (snx_qref_add(&batch, &c, &word, 1, NULL, 0) < 0) goto out;
        }
        if (snx_qref_run(&batch, how[0], how[1], "max", how[2], error, sizeof error)) {
            fprintf(stderr, "FAIL: QEMU: %s\n", error);
            goto out;
        }
        for (k = 0; k < n; ++k) {
            const SnxQrefResult* q = &batch.results[k];
            CPU* e = &ends[done + k];
            unsigned r;
            *e = starts[done + k];
            if (q->outcome == SNX_QREF_UNDEF) continue; /* undef's: the state as it was */
            if (q->outcome != SNX_QREF_NEXT) {
                fprintf(stderr, "FAIL: QEMU: %s %08x ends another way (%u)\n", words[(done + k) / states].op,
                        words[(done + k) / states].word, (unsigned)q->outcome);
                goto out;
            }
            for (r = 0; r < 31; ++r) e->x[r] = q->state.x[r];
            e->x[31] = q->state.sp;
            e->n = q->state.nzcv >> 31, e->z = q->state.nzcv >> 30 & 1;
            e->cf = q->state.nzcv >> 29 & 1, e->vf = q->state.nzcv >> 28 & 1;
            e->fpcr = q->state.fpcr, e->fpsr = q->state.fpsr;
            for (r = 0; r < 32; ++r)
                if (r != own_vn(&words[(done + k) / states]))
                    e->v[r].d[0] = q->state.v[r][0], e->v[r].d[1] = q->state.v[r][1];
        }
    }
    result = 0;
out:
    snx_qref_free(&batch);
    return result;
}

#ifdef SNX_TEST_NATIVE
/* the arm64 code of each function under QEMU too, whose CPU has I8MM and BF16 (Unicorn's hasn't) and raises FCMLA by
   element's flags for its own lanes alone: ran[k] where it got to its return, which code that calls a helper's doesn't
   there. 0 or -1 */
static int qemu_native(HostUc* uc, const uint64_t* hints, unsigned functions, unsigned states, const CPU* starts,
                       CPU* ends, unsigned char* ran, char* const* how) {
    /* the host's own word is a short function; what is longer is an emulation, which Unicorn runs as well */
    enum { BATCH = 512, MOST = 0x400, CODE_SIZE = 2 * SNX_QREF_PAGE };
    /* the state at the window's start, the stack below its end */
    const uint64_t cpu_at = SNX_QREF_WINDOW, size = 8 * SNX_QREF_PAGE;
    const uint64_t code = SNX_QREF_WINDOW + SNX_QREF_MAX_WINDOW; /* apart from the window, which is compared whole */
    uint64_t* fns = (uint64_t*)calloc(functions, sizeof *fns);
    uint32_t* words = (uint32_t*)malloc(MOST);
    SnxQrefBatch batch = {0};
    size_t total = (size_t)functions * states, done, k;
    unsigned i, j;
    int result = -1;
    char name[40], error[512];
    for (i = 0; i < functions; ++i) {
        snprintf(name, sizeof name, "F_%llX", (unsigned long long)hints[i]);
        fns[i] = hostuc_symbol(uc, name);
    }
    for (done = 0; done < total; done += BATCH) {
        size_t n = total - done < BATCH ? total - done : BATCH;
        snx_qref_clear(&batch);
        for (k = 0; k < n; ++k) {
            SnxQrefCase c;
            CPU cpu = starts[done + k];
            SnxQrefPatch patch = {cpu_at, &cpu, sizeof cpu};
            uint64_t fn = fns[i = (unsigned)((done + k) / states)], end = 0;
            for (j = 0; j < functions; ++j)
                if (fns[j] > fn && (!end || fns[j] < end)) end = fns[j];
            if (!end) end = fn + MOST; /* the last one's, and what is after it */
            cpu.mem = NULL, cpu.rt = NULL, cpu.disp = NULL;
            cpu.code_lo = CODE, cpu.code_hi = CODE + (uint64_t)functions * SLOT * 4, cpu.pc = hints[i];
            snx_qref_case_init(&c);
            c.window_size = size;
            c.flags = SNX_QREF_FULL_DIFF;
            c.seed_kind = SNX_QREF_SEED_ZERO;
            c.code = c.pc = code;
            c.code_size = CODE_SIZE;
            c.steps = 0;
            c.state.x[0] = cpu_at, c.state.x[1] = hints[i];
            c.state.x[30] = code + CODE_SIZE - 4, c.state.sp = SNX_QREF_WINDOW + size;
            if (end - fn > MOST) words[0] = SNX_QREF_PAD, end = fn + 4;
            else
                memcpy(words, (const void*)(uintptr_t)fn, (size_t)(end - fn));
            if (snx_qref_add(&batch, &c, words, (uint32_t)(end - fn) / 4, &patch, 1) < 0) goto out;
        }
        if (snx_qref_run(&batch, how[0], how[1], "max", how[2], error, sizeof error)) {
            fprintf(stderr, "FAIL: QEMU: %s\n", error);
            goto out;
        }
        for (k = 0; k < n; ++k) {
            const SnxQrefResult* q = &batch.results[k];
            uint8_t bytes[sizeof(CPU)];
            CPU start = starts[done + k];
            if (q->outcome != SNX_QREF_BRANCH || q->pc != code + CODE_SIZE - 4) continue;
            start.mem = NULL, start.rt = NULL, start.disp = NULL;
            start.code_lo = CODE, start.code_hi = CODE + (uint64_t)functions * SLOT * 4;
            start.pc = hints[(done + k) / states];
            memcpy(bytes, &start, sizeof bytes);
            for (j = 0; j < q->changes; ++j) {
                const SnxQrefChange* d = &batch.changes[q->first_change + j];
                uint64_t at;
                for (at = d->address; at < d->address + d->size; ++at)
                    if (at >= cpu_at && at < cpu_at + sizeof bytes)
                        bytes[at - cpu_at] = batch.bytes[d->offset + (at - d->address)];
            }
            memcpy(&ends[done + k], bytes, sizeof bytes);
            ran[done + k] = 1;
        }
    }
    result = 0;
out:
    snx_qref_free(&batch);
    free(words), free(fns);
    return result;
}
#endif

int main(int argc, char** argv) {
    char** qemu = NULL;
    CPU *starts = NULL, *ends = NULL; /* --qemu's */
#ifdef SNX_TEST_NATIVE
    CPU* own = NULL; /* --qemu's on arm64: its code's ends there, and which ran to one */
    unsigned char* ran = NULL;
#endif
    unsigned functions = 600, states = 8, i, s, k, nwords = 0, nops = 0, slot = SLOT, times = 0;
    int refused = 0, must_make = 0, branches = 0;
    uint32_t (*more)[2] = NULL; /* --programs' words after the first */
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
#ifdef SNX_TEST_NATIVE
    size_t longer = 0;
    int jit = 0;
#endif
#ifdef SNX_TEST_WIN64
    int step = 0;
#endif
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
#ifdef SNX_TEST_NATIVE
        else if (!strcmp(argv[i], "--jit"))
            jit = 1;
#endif
#ifdef SNX_TEST_WIN64
        else if (!strcmp(argv[i], "--step"))
            step = 1;
#endif
        else if (!strcmp(argv[i], "--refused"))
            refused = 1;
        else if (!strcmp(argv[i], "--made"))
            must_make = 1;
        else if (!strcmp(argv[i], "--coverage"))
            refused = 2;
        else if (!strcmp(argv[i], "--programs"))
            branches = 1;
        else if (!strcmp(argv[i], "--long") && i + 2 < (unsigned)argc) {
            times = (unsigned)atoi(argv[++i]);
#ifdef SNX_TEST_NATIVE
            longer = (size_t)strtoull(argv[i + 1], NULL, 0);
#endif
            must_make = 1, ++i;
        } else if (!strcmp(argv[i], "--qemu") && i + 3 < (unsigned)argc) {
            qemu = argv + i + 1;
            g_qemu = 1;
            i += 3;
        } else if (!strcmp(argv[i], "--target") && i + 1 < (unsigned)argc) {
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
#ifdef SNX_TEST_WIN64
    snx_x64_set_win64(1);
#endif
    rows_init();
    if (refused) {
#ifndef SNX_TEST_NATIVE
        if (!snx_x64_available()) {
            printf("recompiler backends: no x86-64 back end on this host: nothing to list\n");
            return 0;
        }
#endif
        return refused == 2 ? list_coverage() : list_refused(ops, nops);
    }
    if (times) {
        if (!nwords || qemu || nops) {
            fprintf(stderr, "--long %u: only with --words\n", times);
            return 2;
        }
        slot = (nwords * times + 8 + SLOT - 1) / SLOT * SLOT;
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
        printf("SKIP: no Unicorn of %s: nothing to compare\n", native_name);
        return 77;
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
    if (branches) {
        size_t count;
        opw = programs(&count, &more);
        functions = (unsigned)count;
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
    if (CODE + (uint64_t)functions * slot * 4 > DATA) {
        fprintf(stderr, "FAIL: %u functions of %u words do not fit in the code's %u bytes\n", functions, slot, DATA - CODE);
        goto out;
    }
    /* the program, in both memories: each function its words then RET */
    for (i = 0; i < functions; ++i) {
        uint64_t at = CODE + (uint64_t)i * slot * 4;
        unsigned n = branches ? 1 + (more[i][0] != 0) + (more[i][1] != 0)
                     : opw    ? 1
                     : nwords ? nwords
                              : 1 + pick(SLOT - 2);
        hints[i] = at;
        if (times) {
            unsigned all = nwords * times;
            put32(mem[0], at, 0xB4000011u | (all + 6) << 5);                     /* CBZ x17, the ADD past them */
            put32(mem[0], at + 4, 0xD2800050u);                                  /* MOVZ x16, #2 */
            put32(mem[0], at + 8, 0x14000002u);                                  /* B, over the RET */
            put32(mem[0], at + 12, 0xD65F03C0u);                                 /* RET */
            for (k = 0; k < all; ++k) put32(mem[0], at + 16 + 4 * k, words[k % nwords]);
            put32(mem[0], at + 16 + 4 * all, 0xF1000610u);                       /* SUBS x16, x16, #1 */
            put32(mem[0], at + 20 + 4 * all, 0x54000001u | ((0x80000u - (all + 1)) & 0x7FFFF) << 5); /* B.NE, the first */
            put32(mem[0], at + 24 + 4 * all, 0x91000400u);                       /* ADD x0, x0, #1 */
            put32(mem[0], at + 28 + 4 * all, 0x14000000u | ((0x4000000u - (all + 4)) & 0x3FFFFFF)); /* B, the RET */
            for (k = all + 8; k < slot; ++k) put32(mem[0], at + 4 * k, 0xD65F03C0u);
            continue;
        }
        /* a quarter of them a loop: MOVZ x16, #1-3, the words, SUBS x16, x16, #1, B.NE back to the first word (x16
           no word writes); a branch forward in the words may leave it early */
        int loop = !nwords && !opw && n >= 4 && !pick(4);
        if (loop) {
            put32(mem[0], at, 0xD2800000u | (1 + pick(3)) << 5 | 16u);
            put32(mem[0], at + 4 * (n - 2), 0xF1000610u); /* SUBS x16, x16, #1 */
            put32(mem[0], at + 4 * (n - 1), 0x54000001u | ((0x80000u - (n - 2)) & 0x7FFFF) << 5); /* B.NE word 1 */
        }
        for (k = loop ? 1 : 0; k < (loop ? n - 2 : n); ++k) {
            uint32_t w = branches && k ? more[i][k - 1] : opw ? opw[i].word : nwords ? words[k] : word();
            if (!nwords && !opw && k + 1 < n && !pick(6)) {
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
        struct snx_code_range range = {CODE, mem[0] + CODE, (size_t)functions * slot * 4};
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
        view.image_size = (size_t)functions * slot * 4;
        view.base = CODE;
        view.text_off = 0;
        view.text_size = view.image_size;
        if (snx_codegen_analyze(&cg, &view, 1, hints, functions, 1, g_features)) {
            fprintf(stderr, "FAIL: no functions found\n");
            goto out;
        }
        made = (unsigned char*)calloc(cg.function_count + 1, 1);
        cg.hook_direct_calls = hook;
        options.jit = jit;
        options.table_name = "UNIT_TABLE";
        if (NATIVE_OBJECT(&cg, &options, made, &object, &object_size, error) < 0) {
            fprintf(stderr, "FAIL: the %s object: %s\n", native_name, error);
            goto out;
        }
#define NATIVE(f) hostuc_native(uc, #f, (void*)f)
        NATIVE(snx_x64_fp_binary), NATIVE(snx_x64_fp_fma), NATIVE(snx_x64_fp_unary), NATIVE(snx_x64_fp_minmax);
        NATIVE(snx_x64_fp_compare), NATIVE(snx_x64_fp_convert), NATIVE(snx_x64_fp_round);
        NATIVE(snx_x64_fp_round_int), NATIVE(snx_x64_fp_to_int), NATIVE(snx_x64_fp_from_int), NATIVE(snx_x64_fp_jcvt);
        NATIVE(snx_x64_fp_bfcvt), NATIVE(snx_mrs), NATIVE(snx_msr), NATIVE(snx_ic_ivau);
        NATIVE(snx_x64_crc32), NATIVE(snx_x64_stxp), NATIVE(snx_x64_aes), NATIVE(snx_x64_estimate);
        NATIVE(snx_x64_half), NATIVE(snx_x64_bfdot), NATIVE(snx_x64_sm4_box);
        NATIVE(snx_x64_set), NATIVE(snx_x64_copy);
        NATIVE(snx_x64_fp_scale), NATIVE(snx_x64_fp8_widen), NATIVE(snx_x64_fp8_narrow), NATIVE(snx_x64_fp8_dot);
#ifdef SNX_TEST_WIN64
        hostuc_step(uc, step);
        NATIVE(snx_x64_fp_vec), NATIVE(snx_x64_crypto);
        void* crc32_table = hostuc_alloc(uc, sizeof snx_x64_crc32_table);
        if (crc32_table) memcpy(crc32_table, snx_x64_crc32_table, sizeof snx_x64_crc32_table);
        hostuc_define(uc, "snx_x64_crc32_table", (uint64_t)(uintptr_t)crc32_table);
#else
        hostuc_native_wide(uc, "snx_x64_casp", (void*)casp_between);
#endif
#undef NATIVE
        if (object && hostuc_load(uc, object, object_size, error, sizeof error)) {
            fprintf(stderr, "FAIL: loading the %s object: %s\n", native_name, error);
            goto out;
        }
        for (n = 0; n < cg.function_count; ++n) made_native += made[n];
        printf("recompiler %s: %zu functions, the %s back end made %zu (%zu bytes)\n", native_name,
               cg.function_count, native_name, made_native, object_size);
        if (made_native && object_size <= longer) {
            fprintf(stderr, "FAIL: the %s object has %zu bytes, not more than the %zu a short jump reaches\n", native_name,
                    object_size, longer);
            ++failures;
        }
        ucpu = (CPU*)hostuc_alloc(uc, sizeof *ucpu);
        urt = (RtAPI*)hostuc_alloc(uc, sizeof *urt);
        udisp = (uint64_t*)hostuc_alloc(uc, (size_t)functions * slot * 8 + 8);
        g_swap.cpu = ucpu, g_swap.rt = urt, g_swap.disp = udisp, g_swap.ndisp = (size_t)functions * slot + 1;
        hostuc_swapper(uc, swap_shared, NULL);
        for (n = 0; n < functions; ++n) {
            char name[40];
            snprintf(name, sizeof name, "F_%llX", (unsigned long long)hints[n]);
            udisp[(hints[n] - CODE) / 4] = hostuc_symbol(uc, name);
        }
        /* and the functions the analysis found besides (a BL's target in --words) */
        for (n = 0; n < cg.function_count; ++n) {
            char name[40];
            snprintf(name, sizeof name, "F_%llX", (unsigned long long)cg.functions[n].start);
            udisp[(cg.functions[n].start - CODE) / 4] = hostuc_symbol(uc, name);
        }
        /* a JIT unit's functions are its table's, in the codegen's order */
        if (jit) {
            const uint64_t* table = (const uint64_t*)(uintptr_t)hostuc_symbol(uc, "UNIT_TABLE");
            for (n = 0; table && n < cg.function_count; ++n)
                udisp[(cg.functions[n].start - CODE) / 4] = hostuc_big(NATIVE_UC) ? snx_bswap64(table[n]) : table[n];
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
    } else if ((!nwords || must_make) && made_native < functions) {
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
    } else if (!nwords && snx_recompiled_x64_functions(mx) < snx_recompiled_functions(mx)) {
        fprintf(stderr, "FAIL: the x86-64 back end made %lu of the %lu functions\n",
                (unsigned long)snx_recompiled_x64_functions(mx), (unsigned long)snx_recompiled_functions(mx));
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
    rt.poll = on_poll;
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
    if (qemu) {
        if (!opw) {
            fprintf(stderr, "--qemu: only with --ops\n");
            goto out;
        }
        starts = (CPU*)calloc((size_t)functions * states, sizeof *starts);
        ends = (CPU*)calloc((size_t)functions * states, sizeof *ends);
        for (i = 0; i < functions; ++i)
            for (s = 0; s < states; ++s) {
                ops_state(&starts[(size_t)i * states + s], s);
                starts[(size_t)i * states + s].x[30] = STOP;
            }
        if (qemu_ends(opw, functions, states, starts, ends, qemu)) goto out;
#ifdef SNX_TEST_NATIVE
        if (NATIVE_UC == HOSTUC_ARM64) {
            own = (CPU*)calloc((size_t)functions * states, sizeof *own);
            ran = (unsigned char*)calloc((size_t)functions * states, 1);
            if (qemu_native(uc, hints, functions, states, starts, own, ran, qemu)) goto out;
            for (i = k = 0; k < functions * states; ++k) i += ran[k];
            printf("recompiler arm64: %u of %u runs of its code under QEMU, the others under Unicorn\n", i,
                   functions * states);
        }
#endif
    }
    for (i = 0; i < functions; ++i)
        for (s = 0; s < states; ++s) {
            CPU c[2], start;
            unsigned between = branches && (opw[i].op[0] == 'L' || !strcmp(opw[i].op, "SWPP"));
            g_between = between;
            memset(&start, 0, sizeof start);
            if (qemu) start = starts[(size_t)i * states + s];
            else if (opw) {
                ops_state(&start, s);
                if (!branches && (!strncmp(opw[i].op, "LD", 2) || !strncmp(opw[i].op, "ST", 2))) /* any register's */
                    for (k = 0; k < 32; ++k) start.x[k] = DATA + 0x6000u + (rnd() % 0x4000u & ~15ull);
                if (!strcmp(opw[i].op, "LDG") && (opw[i].word & 31) != (opw[i].word >> 5 & 31))
                    start.x[opw[i].word & 31] |= rnd() << 56; /* a tag to take away */
                if (branches && s % 4 == 1) start.x[2] = start.x[1]; /* equal */
                if (branches && s % 4 == 2) /* near the integers */
                    start.x[1] = rnd() % 66 - 1, start.x[2] = rnd() % 66 - 1;
                if (branches) {
                    start.x[9] = DATA + 0x1000 + rnd() % 0x200, start.x[10] = DATA + 0x1000 + rnd() % 0x200;
                    start.x[11] = s % 5 ? rnd() % 0x180 : 0;
                    if (opw[i].op[3] == 'M' && (s & 1 || opw[i].op[0] == 'S')) start.x[11] = 0 - start.x[11];
                    start.x[20] = DATA + 0x2000 + (rnd() % 0x400 & ~15ull);
                }
                if (branches && s % 4 == 3) start.x[1] &= 0xFF, start.x[2] = start.x[1] ^ 0x80; /* a byte's sign */
            } else
                start_state(&start, s);
            if (times) { /* past the words or not, and a poll at the branch back, which resumes in the function's midst */
                start.x[17] = s & 1 ? 0 : 1 + s;
                atomic_store_explicit(&start.poll, s >> 1 & 1, memory_order_relaxed);
            }
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
                ucpu->code_hi = CODE + (uint64_t)functions * slot * 4;
                ucpu->pc = hints[i];
                g_running = hints[i];
                for (; !ran || !ran[(size_t)i * states + s];) {
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
                if (ran && ran[(size_t)i * states + s]) c[1] = own[(size_t)i * states + s], c[1].rt = urt;
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
            if (branches) {
                c[0] = start;
                c[0].rt = c[1].rt, c[0].pc = c[1].pc, c[0].x[30] = c[1].x[30];
                memcpy(mem[0] + DATA, data0, DATA_LEN);
                program_end(&opw[i], &c[0], mem[0], between && !g_between);
            }
            if (qemu) {
                c[0] = ends[(size_t)i * states + s];
                c[0].pc = c[1].pc; /* RET's, which a word that writes x30 moves */
                memcpy(mem[0] + DATA, data0, DATA_LEN);
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
                if (verbose && opw) { /* what its Rn and Rm held */
                    unsigned rn = opw[i].word >> 5 & 31, rm = opw[i].word >> 16 & 31;
                    fprintf(stderr, "  from v%u %016llx:%016llx v%u %016llx:%016llx\n", rn,
                            (unsigned long long)start.v[rn].d[1], (unsigned long long)start.v[rn].d[0], rm,
                            (unsigned long long)start.v[rm].d[1], (unsigned long long)start.v[rm].d[0]);
                }
                if (verbose) report(&c[0], &c[1]);
                if (failures > 40) goto done;
            }
        }
done:
    printf("recompiler backends: %u functions, %u states each: %d failures\n", functions, states, failures);
#ifdef SNX_TEST_WIN64
    {
        size_t bad, unwound = hostuc_unwound(uc, &bad);
        printf("recompiler backends: %zu states of the code unwound by its function table, %zu wrongly\n", unwound,
               bad);
        if (step && !unwound) ++failures, fprintf(stderr, "FAIL: --step unwound no state of the code\n");
    }
#endif
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
    free(mem[0]), free(mem[1]), free(data0), free(hints), free(opw), free(starts), free(ends);
#ifdef SNX_TEST_NATIVE
    free(own), free(ran);
#endif
    free(more);
    return result;
}
