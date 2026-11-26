/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ARM_SIMD_H
#define SNX_ARM_SIMD_H
#include "common/cpu/arm/arm.h"

/* Decoded operations, not guest encodings. Register operands are byte offsets
   into the shared D bank: S0..S31 alias D0..D15, Q0..Q15 alias D0..D31. */
typedef enum {
    AV_NONE,
    AV_MOV,
    AV_IMM,
    AV_CORE,
    AV_CORE_PAIR,
    AV_STATUS,
    AV_MEM,
    AV_STRUCT,
    AV_FADD,
    AV_FSUB,
    AV_FMUL,
    AV_FDIV,
    AV_FMLA,
    AV_FMLS,
    AV_FNMLA,
    AV_FNMLS,
    AV_FNMUL,
    AV_FMA,
    AV_FMS,
    AV_FNMA,
    AV_FNMS,
    AV_FABS,
    AV_FNEG,
    AV_FSQRT,
    AV_FCMP,
    AV_FCMPE,
    AV_FCVT,
    AV_FTOI,
    AV_ITOF,
    AV_FROUND,
    AV_FMIN,
    AV_FMAX,
    AV_FMINNM,
    AV_FMAXNM,
    AV_FABD,
    AV_FCEQ,
    AV_FCGE,
    AV_FCGT,
    AV_FACGE,
    AV_FACGT,
    AV_FRECPE,
    AV_FRSQRTE,
    AV_FRECPS,
    AV_FRSQRTS,
    AV_FSELECT,
    AV_ADD,
    AV_SUB,
    AV_HADD,
    AV_HSUB,
    AV_RHADD,
    AV_QADD,
    AV_QSUB,
    AV_AND,
    AV_BIC,
    AV_ORR,
    AV_ORN,
    AV_EOR,
    AV_BSL,
    AV_BIT,
    AV_BIF,
    AV_CGT,
    AV_CGE,
    AV_CEQ,
    AV_TST,
    AV_MIN,
    AV_MAX,
    AV_ABD,
    AV_ABA,
    AV_MUL,
    AV_MLA,
    AV_MLS,
    AV_PMUL,
    AV_PADD,
    AV_PMIN,
    AV_PMAX,
    AV_SHL,
    AV_RSHL,
    AV_QSHL,
    AV_QRSHL,
    AV_SHR,
    AV_RSRA,
    AV_SRA,
    AV_RSHR,
    AV_SLI,
    AV_SRI,
    AV_QSHLU,
    AV_ABS,
    AV_NEG,
    AV_QABS,
    AV_QNEG,
    AV_CLZ,
    AV_CLS,
    AV_CNT,
    AV_NOT,
    AV_REV,
    AV_EXT,
    AV_DUP,
    AV_TBL,
    AV_TBX,
    AV_TRN,
    AV_ZIP,
    AV_UZP,
    AV_SWP,
    AV_ADDL,
    AV_ADDW,
    AV_SUBL,
    AV_SUBW,
    AV_ABDL,
    AV_ABAL,
    AV_MULL,
    AV_MLAL,
    AV_MLSL,
    AV_PMULL,
    AV_QDMULL,
    AV_QDMLAL,
    AV_QDMLSL,
    AV_QDMULH,
    AV_QRDMULH,
    AV_ADDHN,
    AV_SUBHN,
    AV_PADDL,
    AV_PADAL,
    AV_MOVN,
    AV_QMOVN,
    AV_QMOVUN,
    AV_SHRN,
    AV_QSHRN,
    AV_QSHRUN,
    AV_SHLL,
    AV_URECPE,
    AV_URSQRTE,
    AV_CRYPTO
} SnxArmVectorOp;

enum {
    AV_SIMD = 1,
    AV_UNSIGNED = 2,
    AV_ROUND = 4,
    AV_ACCUM = 8,
    AV_LOAD = 16,
    AV_WB = 32,
    AV_UP = 64,
    AV_PRE = 128,
    AV_SCALAR = 256,
    AV_PAIR = 512,
    AV_ZERO = 1024,
    AV_REVERSE = 2048,
    AV_REPLICATE = 4096,
    AV_LANE = 8192
};
typedef struct {
    SnxArmVectorOp op;
    unsigned d, n, m, width, bytes, flags, aux;
    uint64_t imm;
    unsigned count, groups, stride, lane, alignment; /* structure transfer geometry */
} SnxArmVector;

/* Strictly decode Cortex-A57 A32 VFP/Advanced SIMD, excluding later FP16
   arithmetic, dot-product and RDM extensions. Does not depend on LLVM. */
int snx_arm_vector_decode(uint32_t word, SnxArmVector* out);

/* Exact raw-bit FP primitive used by generated LLVM. param: destination width
   [7:0], integer width [15:8], fractional bits [23:16], rounding [27:24]
   (8=current FPSCR), unsigned [28], StandardFPSCR [30]. No host FP or fenv. */
uint64_t snx_arm_fp(SnxArmCPU* cpu, uint64_t x, uint64_t y, uint64_t add, uint32_t op, uint32_t width, uint32_t param);
/* The same, op in desc's bits 7:0, width 15:8 and param 63:32: the x86-64 back end's call (its cold calls take four
   arguments after the state) */
uint64_t snx_arm_fp_x64(SnxArmCPU* cpu, uint64_t x, uint64_t y, uint64_t add, uint64_t desc);
/* the host FP environment the blocks' VFP fast paths assume, put back where it drifted: each LLVM block calls it on
   entry, as an A64 function does snx_fp_env (aarch64/helpers/fp_env.h) */
void snx_arm_fp_env(void);
/* Portable ARM AES/SHA primitives. Operands are byte offsets into D[]. */
void snx_arm_crypto(SnxArmCPU* cpu, uint32_t d, uint32_t n, uint32_t m, uint32_t op);
#endif
