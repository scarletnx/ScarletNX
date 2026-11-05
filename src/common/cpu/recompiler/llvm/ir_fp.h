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

/* x86-64 Windows, s390x and big-endian PowerPC64's ELFv1 return a 16-byte structure through memory, the caller
   passing where as a hidden first argument (sret), and Clang compiles the helpers so there: an LLVM structure would
   come back in two registers. The code runs where it is built, so the host's architecture is the target's */
static inline int ir_fp_reply_in_memory(SnxIr* ir) {
#if defined(__s390x__)
    (void)ir;
    return 1;
#else
#if defined(__x86_64__)
    if (ir->windows) return 1;
#endif
    return ir->ppc && !ir->elfv2;
#endif
}

/* One call of an FP helper (cpu.h), shared by ir_fpu.c's scalar path and ir_vec_fp.c's lanes so both declare it
   the same way: the operands, then the guest's FPCR by value. The reply is {value, flags}: the value is the result
   (an encoding, an integer, or a compare's NZCV nibble), zero-extended to i64, and "flags" the FPSR bits the
   operation raised, which are ORed into FPSR here. The
   helper reads and writes no memory, so it is no site for ir_promote_registers (nothing of the CPU structure is
   unknown behind it) and LLVM may share the FPCR load and the calls themselves. Where the reply comes back
   through memory it lands in a local, which keeps the call out of ir_promote_registers too. */
static inline LLVMValueRef ir_fp_reply_call(SnxIr* ir, const char* name, LLVMValueRef* operands, unsigned count,
                                            int with_fpsr) {
    /* in registers, a big-endian host's ABI (PowerPC64's ELFv2, MIPS64's n64) loads the structure's second
       doubleword whole, the flags in its high half */
    const int in_memory = ir_fp_reply_in_memory(ir), high = !in_memory && ir->big_endian;
    LLVMTypeRef params[9], fields[2] = {ir->t_i64, high ? ir->t_i64 : ir->t_i32};
    LLVMValueRef args[9];
    unsigned n = count;
    for (unsigned i = 0; i < count; ++i) {
        args[i] = operands[i];
        params[i] = LLVMTypeOf(operands[i]);
    }
    args[n] = fpcr(ir);
    params[n++] = ir->t_i32;
    if (with_fpsr) {
        args[n] = fpsr(ir);
        params[n++] = ir->t_i32;
    }
    LLVMTypeRef reply = LLVMStructTypeInContext(ir->ctx, fields, 2, 0);
    LLVMValueRef call;
    if (in_memory) {
        LLVMTypeRef with[10] = {ir->t_ptr};
        LLVMValueRef pass[10] = {ir_local(ir, reply)};
        for (unsigned i = 0; i < n; ++i) {
            with[i + 1] = params[i];
            pass[i + 1] = args[i];
        }
        LLVMValueRef function = ir_extern(ir, name, ir->t_void, with, n + 1);
        LLVMAttributeRef sret = LLVMCreateTypeAttribute(ir->ctx, LLVMGetEnumAttributeKindForName("sret", 4), reply);
        LLVMAddAttributeAtIndex(function, 1, sret);
        ir_argmem(ir, function);
        LLVMValueRef made =
            LLVMBuildCall2(ir->b, LLVMFunctionType(ir->t_void, with, n + 1, 0), function, pass, n + 1, "");
        LLVMAddCallSiteAttribute(made, 1, sret);
        call = LLVMBuildLoad2(ir->b, reply, pass[0], "");
    } else {
        LLVMValueRef function = ir_extern(ir, name, reply, params, n);
        ir_pure(ir, function);
        call = LLVMBuildCall2(ir->b, LLVMFunctionType(reply, params, n, 0), function, args, n, "");
    }
    LLVMValueRef flags = LLVMBuildExtractValue(ir->b, call, 1, "");
    if (high) flags = LLVMBuildTrunc(ir->b, LLVMBuildLShr(ir->b, flags, ir_i64(ir, 32), ""), ir->t_i32, "");
    ir_cpu_store(ir, LLVMBuildOr(ir->b, fpsr(ir), flags, ""), offsetof(CPU, fpsr));
    return LLVMBuildExtractValue(ir->b, call, 0, "");
}
static inline LLVMValueRef ir_fp_call(SnxIr* ir, const char* name, LLVMValueRef* operands, unsigned count) {
    return ir_fp_reply_call(ir, name, operands, count, 0);
}
/* snx_fp_host_binary, which also takes FPSR (cpu.h) and replies with FPSR after it */
static inline LLVMValueRef ir_fp_call_fpsr(SnxIr* ir, const char* name, LLVMValueRef* operands, unsigned count) {
    return ir_fp_reply_call(ir, name, operands, count, 1);
}
static inline LLVMValueRef both(SnxIr* ir, LLVMValueRef a, LLVMValueRef b) { return LLVMBuildAnd(ir->b, a, b, ""); }
static inline LLVMValueRef either(SnxIr* ir, LLVMValueRef a, LLVMValueRef b) { return LLVMBuildOr(ir->b, a, b, ""); }

/* The host's own scalar FP arithmetic: LLVM gives MIPS64's FP operations to libm, so every gate here leaves that one
   on the helpers */
static inline int host_fp(SnxIr* ir) {
    return ir->features->x86 || ir->features->aarch64 || ir->features->ppc || ir->features->riscv ||
           ir->features->loongarch;
}
/* The same over 128-bit vectors: PowerPC64 needs VSX (AltiVec alone has no vector double arithmetic and flushes
   single-precision denormals), RISC-V the V extension, LoongArch LSX. MIPS64's MSA keeps the helpers too */
static inline int host_fp_vec(SnxIr* ir) {
    return ir->features->x86 || ir->features->aarch64 || (ir->features->ppc && ir->features->ppc_vsx) ||
           (ir->features->riscv && ir->features->riscv_v) || (ir->features->loongarch && ir->features->la_lsx);
}
static inline int host_fma_scalar(SnxIr* ir) {
    return (ir->features->x86 && ir->features->fma) || ir->features->aarch64 || ir->features->ppc ||
           ir->features->riscv || ir->features->loongarch;
}
static inline int host_fma_vec(SnxIr* ir) {
    return (ir->features->x86 && ir->features->fma) || ir->features->aarch64 ||
           (ir->features->ppc && ir->features->ppc_vsx) || (ir->features->riscv && ir->features->riscv_v) ||
           (ir->features->loongarch && ir->features->la_lsx);
}
/* The host's rounding instructions for mode rm (FPRounding: 0 TIEEVEN, 1 POSINF, 2 NEGINF, 3 ZERO, 4 TIEAWAY), never
   a libm call: x86's need SSE4.2, PowerPC64's the FPU instructions of POWER5 and later (a 970's are libm), where
   roundeven stays a call too, and LoongArch's round as well (host_rint does that one in arithmetic). A mode this
   refuses uses the helper */
static inline int host_round_mode(SnxIr* ir, unsigned rm) {
    if (ir->features->x86) return ir->features->sse42;
    if (ir->features->aarch64) return 1;
    if (ir->features->ppc) return ir->features->ppc_vsx && rm != 0;
    if (ir->features->riscv) return 1;
    if (ir->features->loongarch) return 1;
    return 0;
}
static inline int host_round_vec_mode(SnxIr* ir, unsigned rm) {
    return host_fp_vec(ir) && host_round_mode(ir, rm);
}
/* FPCR's mode, which picks one of 0-3 at run time: every one of them must be an instruction */
static inline int host_round_dynamic(SnxIr* ir) {
    return host_round_mode(ir, 0) && host_round_mode(ir, 1) && host_round_mode(ir, 2) && host_round_mode(ir, 3);
}
static inline int host_round_vec_dynamic(SnxIr* ir) { return host_fp_vec(ir) && host_round_dynamic(ir); }

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
   POSINF, NEGINF, ZERO, TIEAWAY) by the host_round_mode instructions. x86 and PowerPC64 have none for ties away from
   zero (and LLVM 23 makes a libm call of LoongArch's): the truncation, one more away from zero where the fraction it
   dropped is a half or more. Exact in every host rounding mode: the fraction f - trunc(f) is f's own low bits, and
   trunc(f) + 1 or - 1 is taken only where a fraction was dropped, so below 2^24 (2^53) */
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
