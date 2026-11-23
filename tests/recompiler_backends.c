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
     snx-test-recompiler-backends [--functions N] [--states N] [--seed N] [--verbose] [--words HEX...]
   On a host without the back end (not x86-64, or Windows, or built without SNX_SAIL_X64) it passes, saying so */
#include "common/cpu/recompiler/snx_recompiler.h"
#include "llvm_qemu_words.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CODE     0x10000u
#define SLOT     16u          /* words a function has room for, its RET included */
#define DATA     0x200000u    /* the loads' and stores' memory */
#define DATA_LEN 0x10000u
#define MEMORY   (DATA + DATA_LEN + 0x1000u)
#define STOP     0u

static int failures, verbose;
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
    "SQRDMULH_vi", "SQSHL_v", "ADDP_s", NULL};
static uint32_t* g_rows;
static size_t g_nrows;
static uint64_t rnd_for_rows(void) { return rnd(); }
static void rows_init(void) {
    uint32_t* all;
    size_t n = lqd_row_words(6, rnd_for_rows, &all), i, k;
    g_rows = (uint32_t*)malloc((n + 1) * sizeof *g_rows);
    for (i = 0; i < n; ++i) {
        const char* name = lqd_switch_op_name(all[i]);
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
static void on_brk(CPU* c, uint64_t pc, uint32_t imm) {
    (void)imm, (void)pc;
    c->pc = STOP;
}

int main(int argc, char** argv) {
    unsigned functions = 600, states = 8, i, s, k, nwords = 0;
    uint32_t words[SLOT];
    struct snx_recompiler *rx = NULL, *rl = NULL;
    struct snx_recompiled *mx = NULL, *ml = NULL;
    uint8_t *mem[2] = {0}, *data0 = NULL;
    uint64_t* hints = NULL;
    int result = 1; /* every exit past the allocations frees them, so the sanitizers' leak check reads the test */
    RtAPI rt;
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--functions") && i + 1 < (unsigned)argc) functions = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--states") && i + 1 < (unsigned)argc)
            states = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < (unsigned)argc)
            seed = strtoull(argv[++i], NULL, 0) | 1;
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else if (!strcmp(argv[i], "--words")) {
            /* --words W...: one function of these words (and a RET), run from every state */
            while (i + 1 < (unsigned)argc && argv[i + 1][0] != '-' && nwords < SLOT - 1)
                words[nwords++] = (uint32_t)strtoul(argv[++i], NULL, 16);
            functions = 1;
        }
    }
    rows_init();
    rx = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    rl = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    if (!rx || !rl) {
        fprintf(stderr, "FAIL: no recompiler\n");
        goto out;
    }
    snx_recompiler_set_backend(rx, 1);
    snx_recompiler_set_backend(rl, 0);
    mem[0] = (uint8_t*)calloc(1, MEMORY);
    mem[1] = (uint8_t*)calloc(1, MEMORY);
    data0 = (uint8_t*)malloc(DATA_LEN);
    hints = (uint64_t*)malloc(functions * sizeof *hints);
    /* the program, in both memories: each function its words then RET */
    for (i = 0; i < functions; ++i) {
        uint64_t at = CODE + (uint64_t)i * SLOT * 4;
        unsigned n = nwords ? nwords : 1 + pick(SLOT - 2);
        hints[i] = at;
        /* a quarter of them a loop: MOVZ x16, #1-3, the words, SUBS x16, x16, #1, B.NE back to the first word (x16
           no word writes); a branch forward in the words may leave it early */
        int loop = !nwords && n >= 4 && !pick(4);
        if (loop) {
            put32(mem[0], at, 0xD2800000u | (1 + pick(3)) << 5 | 16u);
            put32(mem[0], at + 4 * (n - 2), 0xF1000610u); /* SUBS x16, x16, #1 */
            put32(mem[0], at + 4 * (n - 1), 0x54000001u | ((0x80000u - (n - 2)) & 0x7FFFF) << 5); /* B.NE word 1 */
        }
        for (k = loop ? 1 : 0; k < (loop ? n - 2 : n); ++k) {
            uint32_t w = nwords ? words[k] : word();
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
        mx = snx_recompile_ranges(rx, &range, 1, hints, functions);
        range.code = mem[1] + CODE;
        ml = snx_recompile_ranges(rl, &range, 1, hints, functions);
    }
    if (!mx || !ml) {
        fprintf(stderr, "FAIL: recompiling: %s / %s\n", mx ? "" : snx_recompiler_error(rx),
                ml ? "" : snx_recompiler_error(rl));
        goto out;
    }
    printf("recompiler backends: %lu words of the decoder's AdvSIMD rows to draw from\n", (unsigned long)g_nrows);
    printf("recompiler backends: %lu functions, the x86-64 back end made %lu\n",
           (unsigned long)snx_recompiled_functions(mx), (unsigned long)snx_recompiled_x64_functions(mx));
    if (!snx_recompiled_x64_functions(mx)) {
        printf("recompiler backends: no x86-64 back end on this host: nothing to compare\n");
        result = 0;
        goto out;
    }
    if (snx_recompiled_x64_functions(mx) * 10 < snx_recompiled_functions(mx) * 9) {
        fprintf(stderr, "FAIL: the x86-64 back end made under 90%% of the functions\n");
        ++failures;
    }
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
    for (k = 0; k < DATA_LEN; ++k) data0[k] = (uint8_t)rnd();
    for (i = 0; i < functions; ++i)
        for (s = 0; s < states; ++s) {
            CPU c[2], start;
            memset(&start, 0, sizeof start);
            start_state(&start, s);
            for (k = 0; k < 2; ++k) {
                struct snx_recompiled* m = k ? mx : ml;
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
    snx_recompiler_free(rx);
    snx_recompiler_free(rl);
    free(mem[0]), free(mem[1]), free(data0), free(hints);
    return result;
}
