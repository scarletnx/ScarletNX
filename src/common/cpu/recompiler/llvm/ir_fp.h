/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What ir_fpu.c (scalar) and ir_vec_fp.c (AdvSIMD) share of the floating-point emitters: FPCR and FPSR in the CPU
   structure, i1 logic, the host's FP features the fast paths need, and a few IR pieces that work on scalars and
   vectors alike. Internal to those two files. */
#ifndef SNX_RECOMPILER_LLVM_IR_FP_H
#define SNX_RECOMPILER_LLVM_IR_FP_H

#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

#define FPCR_RMODE (3u << 22)
#define FPCR_FZ    (1u << 24)
#define FPSR_IXC   0x10u

static inline unsigned esz_of(unsigned width) { return width == 16 ? 1 : width == 32 ? 2 : 3; }

static inline LLVMValueRef fpcr(SnxIr* ir) { return ir_cpu_load(ir, ir->t_i32, offsetof(CPU, fpcr)); }
static inline LLVMValueRef fpsr(SnxIr* ir) { return ir_cpu_load(ir, ir->t_i32, offsetof(CPU, fpsr)); }
/* FPCR.RMode as the helpers' rounding mode (0-3) */
static inline LLVMValueRef fpcr_rmode(SnxIr* ir) {
    return LLVMBuildAnd(ir->b, LLVMBuildLShr(ir->b, fpcr(ir), ir_i32(ir, 22), ""), ir_i32(ir, 3), "");
}
/* i1: none of the FPCR bits of mask is set */
static inline LLVMValueRef fpcr_clear(SnxIr* ir, SnxU32 mask) {
    return LLVMBuildICmp(ir->b, LLVMIntEQ, LLVMBuildAnd(ir->b, fpcr(ir), ir_i32(ir, mask), ""), ir_i32(ir, 0), "");
}
static inline LLVMValueRef ixc_set(SnxIr* ir) {
    return LLVMBuildICmp(ir->b, LLVMIntNE, LLVMBuildAnd(ir->b, fpsr(ir), ir_i32(ir, FPSR_IXC), ""), ir_i32(ir, 0),
                         "");
}
static inline void ir_fp_raise_ixc(SnxIr* ir, LLVMValueRef inexact) {
    LLVMValueRef bit = LLVMBuildShl(ir->b, LLVMBuildZExt(ir->b, inexact, ir->t_i32, ""), ir_i32(ir, 4), "");
    ir_cpu_store(ir, LLVMBuildOr(ir->b, fpsr(ir), bit, ""), offsetof(CPU, fpsr));
}
static inline LLVMValueRef both(SnxIr* ir, LLVMValueRef a, LLVMValueRef b) { return LLVMBuildAnd(ir->b, a, b, ""); }
static inline LLVMValueRef either(SnxIr* ir, LLVMValueRef a, LLVMValueRef b) { return LLVMBuildOr(ir->b, a, b, ""); }

static inline int host_native(SnxIr* ir) { return ir->features->x86 || ir->features->aarch64; }
static inline int host_fma(SnxIr* ir) { return (ir->features->x86 && ir->features->fma) || ir->features->aarch64; }
/* roundss/roundsd (SSE4.1) or FRINT*: the rounding intrinsics never become libm calls */
static inline int host_round(SnxIr* ir) {
    return (ir->features->x86 && ir->features->sse42) || ir->features->aarch64;
}

/* an encoding (or a vector of them) as the helpers' u64 operand */
static inline LLVMValueRef u64_of(SnxIr* ir, LLVMValueRef bits) { return ir_cast(ir, bits, ir->t_i64, 0); }
static inline SnxU64 sign_mask(unsigned width) { return (SnxU64)1 << (width - 1); }
static inline LLVMValueRef fma3(SnxIr* ir, LLVMValueRef a, LLVMValueRef b, LLVMValueRef c) {
    LLVMValueRef args[3] = {a, b, c};
    return ir_intrinsic(ir, "llvm.fma", LLVMTypeOf(a), args, 3);
}
static inline LLVMValueRef float_ne(SnxIr* ir, LLVMValueRef a, LLVMValueRef b) {
    return LLVMBuildFCmp(ir->b, LLVMRealUNE, a, b, "");
}

/* Fixed Arm rounding modes, or 5 for FPCR.RMode. A dynamic mode executes only its selected intrinsic. */
static inline LLVMValueRef ir_fp_round_value(SnxIr* ir, LLVMValueRef value, unsigned mode) {
    static const char* const names[] = {"llvm.roundeven", "llvm.ceil", "llvm.floor", "llvm.trunc", "llvm.round"};
    if (mode < 5) return ir_intrinsic(ir, names[mode], LLVMTypeOf(value), &value, 1);
    LLVMBasicBlockRef blocks[4], join = ir_block(ir, "fp_round_join");
    for (unsigned i = 0; i < 4; ++i) blocks[i] = ir_block(ir, "fp_round_mode");
    LLVMValueRef select = LLVMBuildSwitch(ir->b, fpcr_rmode(ir), blocks[3], 3), results[4];
    for (unsigned i = 0; i < 3; ++i) LLVMAddCase(select, ir_i32(ir, i), blocks[i]);
    for (unsigned i = 0; i < 4; ++i) {
        LLVMPositionBuilderAtEnd(ir->b, blocks[i]);
        results[i] = ir_intrinsic(ir, names[i], LLVMTypeOf(value), &value, 1);
        LLVMBuildBr(ir->b, join);
    }
    LLVMPositionBuilderAtEnd(ir->b, join);
    LLVMValueRef result = LLVMBuildPhi(ir->b, LLVMTypeOf(value), "");
    LLVMAddIncoming(result, results, blocks, 4);
    return result;
}

/* Knuth TwoSum's error term, with the rounded sum already available. No reassociation or contraction is allowed. */
static inline LLVMValueRef ir_fp_sum_error(SnxIr* ir, LLVMValueRef a, LLVMValueRef b, LLVMValueRef sum) {
    LLVMValueRef z = LLVMBuildFSub(ir->b, sum, a, "");
    LLVMValueRef da = LLVMBuildFSub(ir->b, a, LLVMBuildFSub(ir->b, sum, z, ""), "");
    return LLVMBuildFAdd(ir->b, da, LLVMBuildFSub(ir->b, b, z, ""), "");
}

/* Conservatively keep intermediate doubles away from underflow and infinity. Zero is exact here. */
static inline LLVMValueRef ir_fp_residual_safe(SnxIr* ir, LLVMValueRef value) {
    LLVMTypeRef bits = LLVMGetTypeKind(LLVMTypeOf(value)) == LLVMVectorTypeKind ?
        LLVMVectorType(ir->t_i64, LLVMGetVectorSize(LLVMTypeOf(value))) : ir->t_i64;
    LLVMValueRef raw = LLVMBuildBitCast(ir->b, value, bits, "");
    LLVMValueRef magnitude = LLVMBuildAnd(ir->b, raw, ir_vec_k(ir, bits, 0x7fffffffffffffffull), "");
    LLVMValueRef normal = both(ir,
        LLVMBuildICmp(ir->b, LLVMIntUGE, magnitude, ir_vec_k(ir, bits, 0x03f0000000000000ull), ""),
        LLVMBuildICmp(ir->b, LLVMIntULT, magnitude, ir_vec_k(ir, bits, 0x7ff0000000000000ull), ""));
    return either(ir, normal, LLVMBuildICmp(ir->b, LLVMIntEQ, magnitude, LLVMConstNull(bits), ""));
}

/* Exact FMA inexactness, evaluated only with IXC clear and after the result/operand guards.
   f32 products fit exactly in f64. For f64 use the ErrFma expansion of Boldo/Muller, with tighter range guards:
   https://perso.ens-lyon.fr/jean-michel.muller/correct_fma_err.pdf
   Rejected intermediates go to the existing helper; no approximation is used for FPSR. */
static inline LLVMValueRef ir_fp_fma_error(SnxIr* ir, LLVMValueRef a, LLVMValueRef b, LLVMValueRef add,
                                          LLVMValueRef result, unsigned width, LLVMValueRef* valid) {
    LLVMTypeRef type = LLVMGetTypeKind(LLVMTypeOf(a)) == LLVMVectorTypeKind ?
        LLVMVectorType(LLVMDoubleTypeInContext(ir->ctx), LLVMGetVectorSize(LLVMTypeOf(a))) :
        LLVMDoubleTypeInContext(ir->ctx);
    if (width == 32) {
        a = LLVMBuildFPExt(ir->b, a, type, "");
        b = LLVMBuildFPExt(ir->b, b, type, "");
        add = LLVMBuildFPExt(ir->b, add, type, "");
        result = LLVMBuildFPExt(ir->b, result, type, "");
    }
    LLVMValueRef zero = LLVMConstNull(type), product = LLVMBuildFMul(ir->b, a, b, "");
    if (width == 32) {
        LLVMValueRef sum = LLVMBuildFAdd(ir->b, product, add, "");
        LLVMValueRef error = ir_fp_sum_error(ir, product, add, sum);
        LLVMValueRef inexact = either(ir, float_ne(ir, sum, result), float_ne(ir, error, zero));
        *valid = LLVMConstAllOnes(LLVMTypeOf(inexact));
        return inexact;
    }
    LLVMValueRef product_error = fma3(ir, a, b, LLVMBuildFNeg(ir->b, product, ""));
    LLVMValueRef alpha = LLVMBuildFAdd(ir->b, add, product_error, "");
    LLVMValueRef alpha_error = ir_fp_sum_error(ir, add, product_error, alpha);
    LLVMValueRef beta = LLVMBuildFAdd(ir->b, product, alpha, "");
    LLVMValueRef beta_error = ir_fp_sum_error(ir, product, alpha, beta);
    LLVMValueRef gamma = LLVMBuildFAdd(ir->b, LLVMBuildFSub(ir->b, beta, result, ""), beta_error, "");
    LLVMValueRef hi = LLVMBuildFAdd(ir->b, gamma, alpha_error, "");
    LLVMValueRef lo = ir_fp_sum_error(ir, gamma, alpha_error, hi);
    LLVMValueRef zero_product = either(ir, LLVMBuildFCmp(ir->b, LLVMRealOEQ, a, zero, ""),
                                       LLVMBuildFCmp(ir->b, LLVMRealOEQ, b, zero, ""));
    LLVMValueRef product_nonzero = LLVMBuildFCmp(ir->b, LLVMRealONE, product, zero, "");
    *valid = both(ir, either(ir, zero_product, both(ir, product_nonzero, ir_fp_residual_safe(ir, product))),
                  both(ir, ir_fp_residual_safe(ir, alpha), ir_fp_residual_safe(ir, beta)));
    return either(ir, float_ne(ir, hi, zero), float_ne(ir, lo, zero));
}

#endif
