/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What ir_fpu.c (scalar) and ir_vec_fp.c (AdvSIMD) share of the floating-point emitters: FPCR and FPSR in the CPU
   structure, i1 logic, the host's FP features the fast paths need, and a few IR pieces that work on scalars and
   vectors alike. Internal to those two files. */
#ifndef SNX_RECOMPILER_LLVM_IR_FP_H
#define SNX_RECOMPILER_LLVM_IR_FP_H

#include "common/cpu/recompiler/llvm/ir.h"
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
static inline LLVMValueRef both(SnxIr* ir, LLVMValueRef a, LLVMValueRef b) { return LLVMBuildAnd(ir->b, a, b, ""); }
static inline LLVMValueRef either(SnxIr* ir, LLVMValueRef a, LLVMValueRef b) { return LLVMBuildOr(ir->b, a, b, ""); }

static inline int host_native(SnxIr* ir) { return ir->features->x86 || ir->features->aarch64; }
static inline int host_fma(SnxIr* ir) { return (ir->features->x86 && ir->features->fma) || ir->features->aarch64; }
/* roundss/roundsd (SSE4.1) or FRINT*: the rounding intrinsics never become libm calls */
static inline int host_round(SnxIr* ir) {
    return (ir->features->x86 && ir->features->sse42) || ir->features->aarch64;
}

/* the constant v as a float like f (a scalar or a vector) */
static inline LLVMValueRef fp_like(LLVMValueRef f, double v) {
    LLVMTypeRef type = LLVMTypeOf(f);
    if (LLVMGetTypeKind(type) != LLVMVectorTypeKind) return LLVMConstReal(type, v);
    LLVMValueRef k = LLVMConstReal(LLVMGetElementType(type), v), all[16];
    unsigned lanes = LLVMGetVectorSize(type);
    for (unsigned i = 0; i < lanes; ++i) all[i] = k;
    return LLVMConstVector(all, lanes);
}
/* f (a float or a vector of them, neither a NaN) rounded to an integral value in rm (0-4: FPRounding's TIEEVEN,
   POSINF, NEGINF, ZERO, TIEAWAY) by the host_round rounding instructions. x86 has none for ties away from zero
   (LLVM's llvm.round there adds the half's predecessor, whose sum the host's rounding mode rounds): the truncation,
   one more away from zero where the fraction it dropped is a half or more. Exact in every host rounding mode: the
   fraction f - trunc(f) is f's own low bits, and trunc(f) + 1 or - 1 is taken only where a fraction was dropped,
   so below 2^24 (2^53) */
static inline LLVMValueRef host_rint(SnxIr* ir, LLVMValueRef f, unsigned rm) {
    static const char* const names[] = {"llvm.roundeven", "llvm.ceil", "llvm.floor", "llvm.trunc", "llvm.round"};
    LLVMTypeRef type = LLVMTypeOf(f);
    if (rm != 4 || ir->features->aarch64) return ir_intrinsic(ir, names[rm], type, &f, 1);
    LLVMValueRef t = ir_intrinsic(ir, "llvm.trunc", type, &f, 1), dropped = LLVMBuildFSub(ir->b, f, t, ""), step[2];
    dropped = ir_intrinsic(ir, "llvm.fabs", type, &dropped, 1);
    step[0] = fp_like(f, 1.0);
    step[1] = f;
    LLVMValueRef away = LLVMBuildFAdd(ir->b, t, ir_intrinsic(ir, "llvm.copysign", type, step, 2), "");
    return LLVMBuildSelect(ir->b, LLVMBuildFCmp(ir->b, LLVMRealOGE, dropped, fp_like(f, 0.5), ""), away, t, "");
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

#endif
