/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/translate-a64.c: the trans_* functions of these instructions, do_fp3_scalar,
   do_fp1_scalar, do_fp1_scalar_int, do_fmadd, handle_fp_compare, do_cvtf_scalar and do_fcvt_scalar),
   LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The scalar floating-point instructions as LLVM IR, keyed on the generated decoder's ops and arguments: the data
   processing with one, two and three sources, the compares and conditional compares, FCSEL, the FMOVs (immediate,
   register, and to and from the general registers), the conversions between precisions and to and from integers
   and fixed point, FJCVTZS, BFCVT and the FRINTs, FEAT_FRINTTS's included; halves for FEAT_FP16. The architecture's
   rules (FPCR.FZ, FZ16, DN, AHP and RMode, NaN propagation, the default NaN, input flushing, FPSR's cumulative
   flags) are cpu.h's soft-float helpers' (helpers/fp.c, the Arm ARM's FPUnpack, FPRound and FPProcessNaNs), called
   by name in the runtime library. The common single and double operations first try the host's IEEE arithmetic
   behind a guard proving it gives the helper's result and flags (the fast paths below). */
#include "common/cpu/recompiler/llvm/ir_fpu.h"
#include "common/cpu/recompiler/llvm/ir_fp.h"
#include "common/cpu/aarch64/cpu.h"

typedef LLVMValueRef V;
#define B ir->b


/* ---- types, registers and FP state ---- */

static LLVMTypeRef float_type(SnxIr* ir, unsigned bits) {
    return bits == 32 ? LLVMFloatTypeInContext(ir->ctx) : LLVMDoubleTypeInContext(ir->ctx);
}
static V kint(SnxIr* ir, unsigned bits, SnxU64 v) { return LLVMConstInt(ir_int_type(ir, bits), v, 0); }
static V kfloat(SnxIr* ir, unsigned bits, double v) { return LLVMConstReal(float_type(ir, bits), v); }
static unsigned width_of(int esz) { return 8u << esz; }

/* the low width bits of Vn (read_fp_hreg, read_fp_sreg, read_fp_dreg) */
static V fp_reg(SnxIr* ir, unsigned n, unsigned width) { return ir_ve(ir, n, esz_of(width), 0); }
/* Vd = value zero-extended to the whole register (write_fp_dreg): a scalar result clears the rest */
static void set_fp_reg(SnxIr* ir, unsigned d, V value) {
    ir_set_vd(ir, d, 0, ir_cast(ir, value, ir->t_i64, 0));
    ir_set_vd(ir, d, 1, ir_i64(ir, 0));
}
/* Xn or Wn (n = 31 the zero register), and Rd = value zero-extended (a write to the zero register dropped) */
static V gpr(SnxIr* ir, unsigned n, int sf) {
    V x = ir_rx(ir, n);
    return sf ? x : ir_u32(ir, x);
}
static void set_gpr(SnxIr* ir, unsigned d, V value) { ir_setx(ir, d, ir_cast(ir, value, ir->t_i64, 0)); }


/* ---- the helpers (cpu.h): integer arithmetic on IEEE encodings, reading c->fpcr and raising c->fpsr's flags ---- */

/* name(c, operands...), declared from the operands' types (u64 for an encoding or integer, int for the rest) */
static V fp_call(SnxIr* ir, const char* name, LLVMTypeRef ret, const V* operands, unsigned count) {
    LLVMTypeRef params[8];
    V args[8];
    params[0] = ir->t_ptr;
    args[0] = ir->c;
    for (unsigned i = 0; i < count; ++i) {
        args[i + 1] = operands[i];
        params[i + 1] = LLVMTypeOf(operands[i]);
    }
    V function = ir_extern(ir, name, ret, params, count + 1);
    ir_argmem(ir, function); /* only c->fpcr and c->fpsr */
    ir_noregs(ir, function);
    ir_state_invalidate(ir);
    return LLVMBuildCall2(B, LLVMFunctionType(ret, params, count + 1, 0), function, args, count + 1, "");
}
static V to_width(SnxIr* ir, V bits, unsigned width) { return ir_cast(ir, bits, ir_int_type(ir, width), 0); }

static V call_binary(SnxIr* ir, V a, V b, unsigned width, unsigned op) {
    V operands[4];
    operands[0] = u64_of(ir, a);
    operands[1] = u64_of(ir, b);
    operands[2] = ir_i32(ir, width);
    operands[3] = ir_i32(ir, op);
    return to_width(ir, fp_call(ir, "snx_fp_host_binary", ir->t_i64, operands, 4), width);
}
static V call_fma(SnxIr* ir, V add, V a, V b, unsigned width) {
    V operands[4];
    operands[0] = u64_of(ir, add);
    operands[1] = u64_of(ir, a);
    operands[2] = u64_of(ir, b);
    operands[3] = ir_i32(ir, width);
    return to_width(ir, fp_call(ir, "snx_fp_host_fma", ir->t_i64, operands, 4), width);
}
static V call_unary(SnxIr* ir, V a, unsigned width, unsigned op) {
    V operands[3];
    operands[0] = u64_of(ir, a);
    operands[1] = ir_i32(ir, width);
    operands[2] = ir_i32(ir, op);
    return to_width(ir, fp_call(ir, "snx_fp_host_unary", ir->t_i64, operands, 3), width);
}
static V call_minmax(SnxIr* ir, V a, V b, unsigned width, unsigned minimum, unsigned numeric) {
    V operands[5];
    operands[0] = u64_of(ir, a);
    operands[1] = u64_of(ir, b);
    operands[2] = ir_i32(ir, width);
    operands[3] = ir_i32(ir, minimum);
    operands[4] = ir_i32(ir, numeric);
    return to_width(ir, fp_call(ir, "snx_fp_host_minmax", ir->t_i64, operands, 5), width);
}
/* the NZCV nibble (i32) */
static V call_compare(SnxIr* ir, V a, V b, unsigned width, unsigned signal) {
    V operands[4];
    operands[0] = u64_of(ir, a);
    operands[1] = u64_of(ir, b);
    operands[2] = ir_i32(ir, width);
    operands[3] = ir_i32(ir, signal);
    return fp_call(ir, "snx_fp_host_compare", ir->t_i32, operands, 4);
}
/* rm: an i32, FPCR's mode or the encoding's */
static V call_convert(SnxIr* ir, V a, unsigned from, unsigned to, V rm) {
    V operands[4];
    operands[0] = u64_of(ir, a);
    operands[1] = ir_i32(ir, from);
    operands[2] = ir_i32(ir, to);
    operands[3] = rm;
    return to_width(ir, fp_call(ir, "snx_fp_convert", ir->t_i64, operands, 4), to);
}
static V call_round(SnxIr* ir, V a, unsigned width, V rm, unsigned exact) {
    V operands[4];
    operands[0] = u64_of(ir, a);
    operands[1] = ir_i32(ir, width);
    operands[2] = rm;
    operands[3] = ir_i32(ir, exact);
    return to_width(ir, fp_call(ir, "snx_fp_round", ir->t_i64, operands, 4), width);
}
static V call_round_int(SnxIr* ir, V a, unsigned width, V rm, unsigned intsize) {
    V operands[4];
    operands[0] = u64_of(ir, a);
    operands[1] = ir_i32(ir, width);
    operands[2] = rm;
    operands[3] = ir_i32(ir, intsize);
    return to_width(ir, fp_call(ir, "snx_fp_round_int", ir->t_i64, operands, 4), width);
}
/* the integer (i64, masked to its width) */
static V call_to_int(SnxIr* ir, V a, unsigned width, unsigned integer, unsigned uns, unsigned fbits, unsigned rm) {
    V operands[6];
    operands[0] = u64_of(ir, a);
    operands[1] = ir_i32(ir, width);
    operands[2] = ir_i32(ir, integer);
    operands[3] = ir_i32(ir, uns);
    operands[4] = ir_i32(ir, fbits);
    operands[5] = ir_i32(ir, rm);
    return fp_call(ir, "snx_fp_to_int", ir->t_i64, operands, 6);
}
static V call_from_int(SnxIr* ir, V v, unsigned integer, unsigned width, unsigned uns, unsigned fbits) {
    V operands[5];
    operands[0] = u64_of(ir, v);
    operands[1] = ir_i32(ir, integer);
    operands[2] = ir_i32(ir, width);
    operands[3] = ir_i32(ir, uns);
    operands[4] = ir_i32(ir, fbits);
    return to_width(ir, fp_call(ir, "snx_fp_from_int", ir->t_i64, operands, 5), width);
}

/* ---- the fast paths ----
   The host's IEEE operations in round to nearest give the helpers' exact result when the result is finite and above
   the smallest normal, or an exact zero, and under FZ no operand is a denormal (unflushed): then no NaN was processed
   (a NaN operand gives a NaN), no infinity was an operand (it gives an infinity or a NaN), a denormal operand is the
   IEEE value it is without FZ and raises nothing, nothing is flushed, tininess (before rounding, as the Arm ARM
   detects it) can't have occurred, and IXC is the only flag. Many games run with FZ set: a guard of FZ clear
   would send all of their arithmetic to the helpers. A
   result above the smallest normal rounds a value above it; a zero is exact for any sum (sums only cancel exactly),
   and for a product or quotient only with a zero operand. IXC is computed
   exactly where that is cheap (TwoSum for sums; for singles the double product, quotient remainder or square, all
   exact; for doubles an FMA residual where the host has FMA, kept to operands whose residual can't underflow), and
   otherwise the path is taken only once IXC is set: it is sticky, so an inexact result changes nothing then.
   Compares, FMAX and FMIN, widening and narrowing, and the integer conversions have guards of their own. Nothing
   reads the host's FP state, so this assumes only that the host thread never flushes denormals (x86's MXCSR.FTZ
   and DAZ, FPCR.FZ on an AArch64 host), which the runtime never sets. Halves always take the helpers. The slow
   paths call the helpers' snx_fp_host_* forms, which try the host's arithmetic on what these guards refuse (NaNs,
   infinities, underflows) before the soft-float. */


static SnxU64 inf_bits(unsigned width) { return width == 32 ? 0x7F800000u : 0x7FF0000000000000ull; }
static SnxU64 min_normal(unsigned width) { return width == 32 ? 0x00800000u : 0x0010000000000000ull; }
/* doubles whose FMA residuals (products of two ulps at least 2^-1074 apart from the operand's) stay representable */
#define RESIDUAL_FLOOR 0x03F0000000000000ull /* 2^-960 */

static V magnitude(SnxIr* ir, unsigned width, V bits) {
    return LLVMBuildAnd(B, bits, kint(ir, width, ~sign_mask(width) & (width == 32 ? 0xFFFFFFFFu : ~0ull)), "");
}
static V is_zero(SnxIr* ir, unsigned width, V bits) {
    return LLVMBuildICmp(B, LLVMIntEQ, magnitude(ir, width, bits), kint(ir, width, 0), "");
}
static V is_nan(SnxIr* ir, unsigned width, V bits) {
    return LLVMBuildICmp(B, LLVMIntUGT, magnitude(ir, width, bits), kint(ir, width, inf_bits(width)), "");
}
static V is_denormal(SnxIr* ir, unsigned width, V bits) {
    V m = magnitude(ir, width, bits);
    return both(ir, LLVMBuildICmp(B, LLVMIntNE, m, kint(ir, width, 0), ""),
                LLVMBuildICmp(B, LLVMIntULT, m, kint(ir, width, min_normal(width)), ""));
}
/* lo <= |bits| < hi */
static V magnitude_in(SnxIr* ir, unsigned width, V bits, SnxU64 lo, SnxU64 hi) {
    V offset = LLVMBuildSub(B, magnitude(ir, width, bits), kint(ir, width, lo), "");
    return LLVMBuildICmp(B, LLVMIntULT, offset, kint(ir, width, hi - lo), "");
}
/* finite and above the smallest normal */
static V roomy(SnxIr* ir, unsigned width, V bits) {
    return magnitude_in(ir, width, bits, min_normal(width) + 1, inf_bits(width));
}
/* a denormal operand is fine unless FPCR.FZ flushes it (which raises IDC) */
static V unflushed(SnxIr* ir, unsigned width, V bits) {
    return either(ir, fpcr_clear(ir, FPCR_FZ), LLVMBuildNot(B, is_denormal(ir, width, bits), ""));
}

static V as_float(SnxIr* ir, V bits, unsigned width) { return LLVMBuildBitCast(B, bits, float_type(ir, width), ""); }
static V as_bits(SnxIr* ir, V f, unsigned width) { return LLVMBuildBitCast(B, f, ir_int_type(ir, width), ""); }
static V widen(SnxIr* ir, V f) { return LLVMBuildFPExt(B, f, LLVMDoubleTypeInContext(ir->ctx), ""); }

/* A value from the host where ok holds, from the helpers otherwise: begin branches (positioning at the slow block
   for the caller's helper call), end joins them. The fast block sets IXC when inexact (NULL: never) */
typedef struct {
    LLVMBasicBlockRef fast_end, join;
    V fast;
} Split;
static void split_begin(SnxIr* ir, V ok, V fast, V inexact, Split* s) {
    LLVMBasicBlockRef on = ir_block(ir, "fp_fast"), off = ir_block(ir, "fp_slow");
    s->join = ir_block(ir, "fp_join");
    s->fast = fast;
    ir_likely(ir, LLVMBuildCondBr(B, ok, on, off));
    LLVMPositionBuilderAtEnd(B, on);
    /* only while IXC is clear: it is sticky, and LLVM sinks the inexactness test into that block */
    if (inexact) {
        LLVMBasicBlockRef clear = ir_block(ir, "fp_ixc"), done = ir_block(ir, "fp_ixc_done");
        ir_unlikely(ir, LLVMBuildCondBr(B, LLVMBuildNot(B, ixc_set(ir), ""), clear, done));
        LLVMPositionBuilderAtEnd(B, clear);
        V flags = fpsr(ir), bit = LLVMBuildShl(B, LLVMBuildZExt(B, inexact, ir->t_i32, ""), ir_i32(ir, 4), "");
        ir_cpu_store(ir, LLVMBuildOr(B, flags, bit, ""), offsetof(CPU, fpsr));
        LLVMBuildBr(B, done);
        LLVMPositionBuilderAtEnd(B, done);
    }
    s->fast_end = LLVMGetInsertBlock(B);
    LLVMBuildBr(B, s->join);
    LLVMPositionBuilderAtEnd(B, off);
}
static V split_end(SnxIr* ir, Split* s, V slow) {
    LLVMBasicBlockRef slow_end = LLVMGetInsertBlock(B);
    V values[2];
    LLVMBasicBlockRef blocks[2];
    LLVMBuildBr(B, s->join);
    LLVMPositionBuilderAtEnd(B, s->join);
    V phi = LLVMBuildPhi(B, LLVMTypeOf(slow), "");
    values[0] = s->fast;
    values[1] = slow;
    blocks[0] = s->fast_end;
    blocks[1] = slow_end;
    LLVMAddIncoming(phi, values, blocks, 2);
    return phi;
}

/* a op b, op SNX_FP_ADD, SUB, MUL or DIV, for singles and doubles: the host's result, with *ok and *inexact */
static V fast_binary(SnxIr* ir, unsigned op, unsigned width, V a, V b, V* ok, V* inexact) {
    V fa = as_float(ir, a, width), fb = as_float(ir, b, width), r, rb, exact_zero, cut = NULL;
    *inexact = NULL;
    switch (op) {
        case SNX_FP_ADD:
        case SNX_FP_SUB: { /* TwoSum: the rounding error of fa + fb, exactly */
            if (op == SNX_FP_SUB) fb = LLVMBuildFNeg(B, fb, "");
            r = LLVMBuildFAdd(B, fa, fb, "");
            V t = LLVMBuildFSub(B, r, fa, "");
            V va = LLVMBuildFSub(B, fa, LLVMBuildFSub(B, r, t, ""), "");
            V vb = LLVMBuildFSub(B, fb, t, "");
            *inexact = float_ne(ir, LLVMBuildFAdd(B, va, vb, ""), kfloat(ir, width, 0.0));
            rb = as_bits(ir, r, width);
            exact_zero = is_zero(ir, width, rb);
            break;
        }
        case SNX_FP_MUL:
            r = LLVMBuildFMul(B, fa, fb, "");
            rb = as_bits(ir, r, width);
            exact_zero = both(ir, is_zero(ir, width, rb), either(ir, is_zero(ir, width, a), is_zero(ir, width, b)));
            if (width == 32)
                *inexact = float_ne(ir, LLVMBuildFMul(B, widen(ir, fa), widen(ir, fb), ""), widen(ir, r));
            else if (host_fma(ir)) {
                *inexact = float_ne(ir, fma3(ir, fa, fb, LLVMBuildFNeg(B, r, "")), kfloat(ir, 64, 0.0));
                cut = either(ir, exact_zero, magnitude_in(ir, 64, rb, RESIDUAL_FLOOR, inf_bits(64)));
            }
            break;
        default: /* SNX_FP_DIV */
            r = LLVMBuildFDiv(B, fa, fb, "");
            rb = as_bits(ir, r, width);
            exact_zero = both(ir, is_zero(ir, width, rb), is_zero(ir, width, a));
            if (width == 32) {
                V product = LLVMBuildFMul(B, widen(ir, r), widen(ir, fb), "");
                *inexact = float_ne(ir, LLVMBuildFSub(B, widen(ir, fa), product, ""), kfloat(ir, 64, 0.0));
            } else if (host_fma(ir)) {
                *inexact = float_ne(ir, fma3(ir, LLVMBuildFNeg(B, r, ""), fb, fa), kfloat(ir, 64, 0.0));
                cut = either(ir, is_zero(ir, 64, a), magnitude_in(ir, 64, a, RESIDUAL_FLOOR, inf_bits(64)));
            }
            /* a zero divided by an infinity is an exact zero, though its residual (0 * inf) is a NaN */
            if (*inexact) *inexact = both(ir, *inexact, LLVMBuildNot(B, exact_zero, ""));
            break;
    }
    *ok = both(ir, both(ir, fpcr_clear(ir, FPCR_RMODE), both(ir, unflushed(ir, width, a), unflushed(ir, width, b))),
               either(ir, roomy(ir, width, rb), exact_zero));
    if (cut) *ok = both(ir, *ok, cut);
    if (!*inexact) *ok = both(ir, *ok, ixc_set(ir));
    return rb;
}

/* the square root, for singles and doubles on hosts with the instruction */
static V fast_sqrt(SnxIr* ir, unsigned width, V a, V* ok, V* inexact) {
    V fa = as_float(ir, a, width), r = ir_intrinsic(ir, "llvm.sqrt", LLVMTypeOf(fa), &fa, 1);
    V rb = as_bits(ir, r, width), zero = is_zero(ir, width, a);
    *inexact = NULL;
    *ok = both(ir, both(ir, fpcr_clear(ir, FPCR_RMODE), unflushed(ir, width, a)), either(ir, roomy(ir, width, rb), zero));
    if (width == 32)
        *inexact = float_ne(ir, widen(ir, fa), LLVMBuildFMul(B, widen(ir, r), widen(ir, r), ""));
    else if (host_fma(ir)) {
        *inexact = float_ne(ir, fma3(ir, LLVMBuildFNeg(B, r, ""), r, fa), kfloat(ir, 64, 0.0));
        *ok = both(ir, *ok, either(ir, zero, magnitude_in(ir, 64, a, RESIDUAL_FLOOR, inf_bits(64))));
    } else
        *ok = both(ir, *ok, ixc_set(ir));
    return rb;
}

/* add + n * m rounded once, on hosts with FMA, once IXC is set (no cheap exact residual) */
static V fast_fma(SnxIr* ir, unsigned width, V add, V n, V m, V* ok) {
    V r = as_bits(ir, fma3(ir, as_float(ir, n, width), as_float(ir, m, width), as_float(ir, add, width)), width);
    V zero_product = either(ir, is_zero(ir, width, n), is_zero(ir, width, m));
    V result = either(ir, roomy(ir, width, r), both(ir, is_zero(ir, width, r), zero_product));
    V operands = both(ir, unflushed(ir, width, add), both(ir, unflushed(ir, width, n), unflushed(ir, width, m)));
    *ok = both(ir, both(ir, fpcr_clear(ir, FPCR_RMODE), ixc_set(ir)), both(ir, operands, result));
    return r;
}

/* without FPSR's flags (snx_ir_fpsr_flags): the host's result wherever it is not a NaN, in round to nearest without
   FZ (infinities, denormals and zeros are the IEEE values the console gives there too), and where the exact guard
   holds, as under FZ */
static void relax(SnxIr* ir, unsigned width, V value, V* ok, V* inexact) {
    if (snx_ir_fpsr_exact()) return;
    *ok = either(ir, *ok,
                 both(ir, fpcr_clear(ir, FPCR_RMODE | FPCR_FZ), LLVMBuildNot(B, is_nan(ir, width, value), "")));
    if (inexact) *inexact = NULL;
}

/* ---- data processing ---- */

/* FADD, FSUB, FMUL, FDIV and FNMUL (negate: the product's sign flipped afterwards, a NaN's too) */
static void fp_binary(SnxIr* ir, const SnxA64Args* a, unsigned op, int negate) {
    unsigned width = width_of(a->esz);
    V n = fp_reg(ir, (unsigned)a->rn, width), m = fp_reg(ir, (unsigned)a->rm, width), r;
    if (width != 16) {
        V ok, inexact, fast = fast_binary(ir, op, width, n, m, &ok, &inexact);
        relax(ir, width, fast, &ok, &inexact);
        Split s;
        split_begin(ir, ok, fast, inexact, &s);
        r = split_end(ir, &s, call_binary(ir, n, m, width, op));
    } else
        r = call_binary(ir, n, m, width, op);
    if (negate) r = LLVMBuildXor(B, r, kint(ir, width, sign_mask(width)), "");
    set_fp_reg(ir, (unsigned)a->rd, r);
}

/* FMAX, FMIN and the NM forms. Without a NaN, and without a denormal FPCR.FZ flushes, FPMax and FPMin give the
   larger or smaller operand unchanged, and of equal ones their AND (max: +0 over -0) or OR (min), raising nothing */
static void fp_minmax(SnxIr* ir, const SnxA64Args* a, unsigned minimum, unsigned numeric) {
    unsigned width = width_of(a->esz);
    V n = fp_reg(ir, (unsigned)a->rn, width), m = fp_reg(ir, (unsigned)a->rm, width), r;
    if (width != 16) {
        V fn = as_float(ir, n, width), fm = as_float(ir, m, width);
        LLVMRealPredicate wins = minimum ? LLVMRealOLT : LLVMRealOGT;
        Split s;
        V ok = both(ir, LLVMBuildNot(B, either(ir, is_nan(ir, width, n), is_nan(ir, width, m)), ""),
                    both(ir, unflushed(ir, width, n), unflushed(ir, width, m)));
        V fast = minimum ? LLVMBuildOr(B, n, m, "") : LLVMBuildAnd(B, n, m, "");
        fast = LLVMBuildSelect(B, LLVMBuildFCmp(B, wins, fm, fn, ""), m, fast, "");
        fast = LLVMBuildSelect(B, LLVMBuildFCmp(B, wins, fn, fm, ""), n, fast, "");
        split_begin(ir, ok, fast, NULL, &s);
        r = split_end(ir, &s, call_minmax(ir, n, m, width, minimum, numeric));
    } else
        r = call_minmax(ir, n, m, width, minimum, numeric);
    set_fp_reg(ir, (unsigned)a->rd, r);
}

/* FMADD, FMSUB, FNMADD, FNMSUB: ra + rn * rm fused, with the addend and the first factor negated first as the
   instruction says, so a NaN among them comes out with its sign flipped (do_fmadd) */
static void fp_fma(SnxIr* ir, const SnxA64Args* a, int negate_add, int negate_n) {
    unsigned width = width_of(a->esz);
    V n = fp_reg(ir, (unsigned)a->rn, width), m = fp_reg(ir, (unsigned)a->rm, width), r;
    V add = fp_reg(ir, (unsigned)a->ra, width);
    if (negate_add) add = LLVMBuildXor(B, add, kint(ir, width, sign_mask(width)), "");
    if (negate_n) n = LLVMBuildXor(B, n, kint(ir, width, sign_mask(width)), "");
    if (width != 16 && host_fma(ir)) {
        V ok, fast = fast_fma(ir, width, add, n, m, &ok);
        relax(ir, width, fast, &ok, NULL);
        Split s;
        split_begin(ir, ok, fast, NULL, &s);
        r = split_end(ir, &s, call_fma(ir, add, n, m, width));
    } else
        r = call_fma(ir, add, n, m, width);
    set_fp_reg(ir, (unsigned)a->rd, r);
}

/* FMOV (register), FABS, FNEG: bit operations that raise nothing and process no NaN */
static void fp_move(SnxIr* ir, const SnxA64Args* a, int op) {
    unsigned width = width_of(a->esz);
    V v = fp_reg(ir, (unsigned)a->rn, width);
    if (op == 1) v = LLVMBuildAnd(B, v, kint(ir, width, sign_mask(width) - 1), "");
    if (op == 2) v = LLVMBuildXor(B, v, kint(ir, width, sign_mask(width)), "");
    set_fp_reg(ir, (unsigned)a->rd, v);
}

static void fp_sqrt(SnxIr* ir, const SnxA64Args* a) {
    unsigned width = width_of(a->esz);
    V n = fp_reg(ir, (unsigned)a->rn, width), r;
    if (width != 16 && host_native(ir)) {
        V ok, inexact, fast = fast_sqrt(ir, width, n, &ok, &inexact);
        relax(ir, width, fast, &ok, &inexact);
        Split s;
        split_begin(ir, ok, fast, inexact, &s);
        r = split_end(ir, &s, call_unary(ir, n, width, SNX_FP_SQRT));
    } else
        r = call_unary(ir, n, width, SNX_FP_SQRT);
    set_fp_reg(ir, (unsigned)a->rd, r);
}

/* FRINTN, P, M, Z, A (rm the encoding's), X and I (rm < 0: FPCR's); exact: FRINTX's IXC */
static void fp_round(SnxIr* ir, const SnxA64Args* a, int rm, unsigned exact) {
    unsigned width = width_of(a->esz);
    V mode = rm < 0 ? fpcr_rmode(ir) : ir_i32(ir, (SnxU32)rm);
    set_fp_reg(ir, (unsigned)a->rd, call_round(ir, fp_reg(ir, (unsigned)a->rn, width), width, mode, exact));
}

/* FRINT32Z, FRINT32X, FRINT64Z, FRINT64X (rm < 0: FPCR's) */
static void fp_round_int(SnxIr* ir, const SnxA64Args* a, int rm, unsigned intsize) {
    unsigned width = width_of(a->esz);
    V mode = rm < 0 ? fpcr_rmode(ir) : ir_i32(ir, (SnxU32)rm);
    set_fp_reg(ir, (unsigned)a->rd, call_round_int(ir, fp_reg(ir, (unsigned)a->rn, width), width, mode, intsize));
}

/* FCVT between precisions, rounded per FPCR. A single widened to a double is exact unless it is a NaN or FPCR.FZ
   flushes it; a double narrowed in round to nearest is the host's when it isn't a NaN and the single is finite and
   above the smallest normal, or the double was a zero */
static void fp_convert(SnxIr* ir, const SnxA64Args* a, unsigned from, unsigned to) {
    V n = fp_reg(ir, (unsigned)a->rn, from), r;
    if (from == 32 && to == 64) {
        V ok = both(ir, LLVMBuildNot(B, is_nan(ir, 32, n), ""), unflushed(ir, 32, n)), fast;
        Split s;
        fast = as_bits(ir, widen(ir, as_float(ir, n, 32)), 64);
        split_begin(ir, ok, fast, NULL, &s);
        r = split_end(ir, &s, call_convert(ir, n, from, to, fpcr_rmode(ir)));
    } else if (from == 64 && to == 32) {
        V fn = as_float(ir, n, 64), narrow = LLVMBuildFPTrunc(B, fn, float_type(ir, 32), ""), ok, fast;
        Split s;
        fast = as_bits(ir, narrow, 32);
        ok = both(ir, both(ir, fpcr_clear(ir, FPCR_RMODE), LLVMBuildNot(B, is_nan(ir, 64, n), "")),
                  either(ir, roomy(ir, 32, fast), is_zero(ir, 64, n)));
        split_begin(ir, ok, fast, float_ne(ir, widen(ir, narrow), fn), &s);
        r = split_end(ir, &s, call_convert(ir, n, from, to, fpcr_rmode(ir)));
    } else
        r = call_convert(ir, n, from, to, fpcr_rmode(ir));
    set_fp_reg(ir, (unsigned)a->rd, r);
}

/* VFPExpandImm: imm8 as sign, NOT(b6) followed by b6 repeated and imm8<5:4> as the exponent, imm8<3:0> as the
   fraction's top */
static SnxU64 expand_imm(unsigned imm8, unsigned width) {
    unsigned e = width == 16 ? 5 : width == 32 ? 8 : 11, f = width - e - 1;
    SnxU64 b6 = (imm8 >> 6) & 1;
    SnxU64 exponent = (b6 ^ 1) << (e - 1) | (b6 ? ((SnxU64)1 << (e - 3)) - 1 : 0) << 2 | ((imm8 >> 4) & 3);
    return (SnxU64)((imm8 >> 7) & 1) << (width - 1) | exponent << f | (SnxU64)(imm8 & 15) << (f - 4);
}

/* ---- compares and selects ---- */

/* the NZCV nibble of FCMP or FCMPE (signal) of a and b. Without a NaN, and without a denormal FPCR.FZ flushes,
   FPCompare raises nothing and orders as the host does: N for less, ZC for equal, C for greater */
static V fp_compare(SnxIr* ir, V a, V b, unsigned width, unsigned signal) {
    Split s;
    if (width == 16) return call_compare(ir, a, b, width, signal);
    V fa = as_float(ir, a, width);
    V fb = as_float(ir, b, width);
    V ok = both(ir, unflushed(ir, width, a), unflushed(ir, width, b));
    ok = both(ir, LLVMBuildFCmp(B, LLVMRealORD, fa, fb, ""), ok);
    V fast = LLVMBuildSelect(B, LLVMBuildFCmp(B, LLVMRealOEQ, fa, fb, ""), ir_i32(ir, 6), ir_i32(ir, 2), "");
    fast = LLVMBuildSelect(B, LLVMBuildFCmp(B, LLVMRealOLT, fa, fb, ""), ir_i32(ir, 8), fast, "");
    split_begin(ir, ok, fast, NULL, &s);
    return split_end(ir, &s, call_compare(ir, a, b, width, signal));
}

/* FCMP, FCMPE: with z, against +0 (handle_fp_compare) */
static void fp_cmp(SnxIr* ir, const SnxA64Args* a) {
    unsigned width = width_of(a->esz);
    V n = fp_reg(ir, (unsigned)a->rn, width), m = a->z ? kint(ir, width, 0) : fp_reg(ir, (unsigned)a->rm, width);
    ir_flags_set_nzcv(ir, fp_compare(ir, n, m, width, (unsigned)a->e));
}

/* FCCMP, FCCMPE: the compare when cond holds (only then can it raise anything), else nzcv */
static void fp_ccmp(SnxIr* ir, const SnxA64Args* a) {
    unsigned width = width_of(a->esz);
    if (a->cond >= 14) {
        fp_cmp(ir, a);
        return;
    }
    LLVMBasicBlockRef match = ir_block(ir, "fccmp");
    LLVMBasicBlockRef other = ir_block(ir, "fccmp_else");
    LLVMBasicBlockRef done = ir_block(ir, "fccmp_done");
    V holds = ir_flags_cond(ir, (unsigned)a->cond);
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntNE, holds, LLVMConstNull(LLVMTypeOf(holds)), ""), match, other);
    LLVMPositionBuilderAtEnd(B, other);
    ir_flags_set_nzcv(ir, ir_i32(ir, (SnxU32)a->nzcv));
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, match);
    ir_flags_set_nzcv(ir, fp_compare(ir, fp_reg(ir, (unsigned)a->rn, width), fp_reg(ir, (unsigned)a->rm, width),
                                     width, (unsigned)a->e));
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, done);
    ir_flags_invalidate(ir);
}

/* FCSEL: no FP arithmetic, so nothing is raised */
static void fp_csel(SnxIr* ir, const SnxA64Args* a) {
    unsigned width = width_of(a->esz);
    V holds = ir_flags_cond(ir, (unsigned)a->cond);
    V n = fp_reg(ir, (unsigned)a->rn, width), m = fp_reg(ir, (unsigned)a->rm, width);
    holds = LLVMBuildICmp(B, LLVMIntNE, holds, LLVMConstNull(LLVMTypeOf(holds)), "");
    set_fp_reg(ir, (unsigned)a->rd, LLVMBuildSelect(B, holds, n, m, ""));
}

/* ---- conversions to and from integers ---- */

/* SCVTF, UCVTF from Wn or Xn, fixed point with shift fraction bits (do_cvtf_g). Exact on the host when the
   integer's magnitude fits the significand, whatever the mode (the scaling by a power of two is exact: the result is
   at least 2^-64); otherwise once IXC is set, in round to nearest */
static void fp_from_int(SnxIr* ir, const SnxA64Args* a, unsigned uns) {
    unsigned width = width_of(a->esz), integer = a->sf ? 64 : 32, fbits = (unsigned)a->shift;
    unsigned precision = width == 32 ? 24 : 53;
    V v = gpr(ir, (unsigned)a->rn, a->sf), r;
    if (width != 16) {
        LLVMTypeRef type = float_type(ir, width);
        V f, ok;
        Split s;
        if (integer <= precision)
            ok = LLVMConstInt(ir->t_i1, 1, 0);
        else if (uns)
            ok = LLVMBuildICmp(B, LLVMIntULT, v, kint(ir, integer, (SnxU64)1 << precision), "");
        else
            ok = LLVMBuildICmp(B, LLVMIntULT, LLVMBuildAdd(B, v, kint(ir, integer, (SnxU64)1 << precision), ""),
                               kint(ir, integer, (SnxU64)1 << (precision + 1)), "");
        /* an unsigned value that fits converts as a signed one: x86 before AVX-512 has no unsigned conversion from
           64 bits, and LLVM's sum of two exact halves there is -0 for 0 when the host rounds toward minus infinity */
        if (uns && integer > precision)
            f = LLVMBuildSelect(B, ok, LLVMBuildSIToFP(B, v, type, ""), LLVMBuildUIToFP(B, v, type, ""), "");
        else
            f = uns ? LLVMBuildUIToFP(B, v, type, "") : LLVMBuildSIToFP(B, v, type, "");
        if (fbits) f = LLVMBuildFMul(B, f, kfloat(ir, width, 1.0 / (double)((SnxU64)1 << (fbits - 1)) / 2.0), "");
        V fast = as_bits(ir, f, width);
        ok = either(ir, ok, both(ir, fpcr_clear(ir, FPCR_RMODE), ixc_set(ir)));
        split_begin(ir, ok, fast, NULL, &s);
        r = split_end(ir, &s, call_from_int(ir, v, integer, width, uns, fbits));
    } else
        r = call_from_int(ir, v, integer, width, uns, fbits);
    set_fp_reg(ir, (unsigned)a->rd, r);
}

/* FCVT[NPMZA][SU] to Wd or Xd, fixed point with shift fraction bits, rounding mode rm (do_fcvt_g). On the host when
   the scaled value (exact: a product by a power of two that overflows only out of range) rounds to an integer in
   range and no denormal is flushed; IXC when rounding changed it. Toward zero is the conversion's own rounding, the
   other modes need the host's rounding instructions (ties away from zero only AArch64's FRINTA) */
static void fp_to_int(SnxIr* ir, const SnxA64Args* a, unsigned rm, unsigned uns) {
    unsigned width = width_of(a->esz), integer = a->sf ? 64 : 32, fbits = (unsigned)a->shift;
    unsigned precision = width == 32 ? 24 : 53;
    V n = fp_reg(ir, (unsigned)a->rn, width), r;
    int fast_path = width != 16 && (rm == 3 || (host_round(ir) && (rm != 4 || ir->features->aarch64)));
    if (fast_path) {
        static const char* const rounding[] = {"llvm.roundeven", "llvm.ceil", "llvm.floor", NULL, "llvm.round"};
        LLVMTypeRef type = ir_int_type(ir, integer);
        V f = as_float(ir, n, width), low;
        Split s;
        double top = (double)((SnxU64)1 << (integer - 1)) * (uns ? 2.0 : 1.0);
        if (fbits) f = LLVMBuildFMul(B, f, kfloat(ir, width, (double)((SnxU64)1 << (fbits - 1)) * 2.0), "");
        V rounded = rm == 3 ? f : ir_intrinsic(ir, rounding[rm], LLVMTypeOf(f), &f, 1);
        /* the lowest integer in range less one, where the format holds it (trunc of anything above it is in
           range), else the lowest integer itself */
        if (uns)
            low = LLVMBuildFCmp(B, LLVMRealOGT, rounded, kfloat(ir, width, -1.0), "");
        else if (integer < precision)
            low = LLVMBuildFCmp(B, LLVMRealOGT, rounded, kfloat(ir, width, -top - 1.0), "");
        else
            low = LLVMBuildFCmp(B, LLVMRealOGE, rounded, kfloat(ir, width, -top), "");
        V high = LLVMBuildFCmp(B, LLVMRealOLT, rounded, kfloat(ir, width, top), "");
        V ok = both(ir, both(ir, low, high), unflushed(ir, width, n));
        V fast = uns ? LLVMBuildFPToUI(B, rounded, type, "") : LLVMBuildFPToSI(B, rounded, type, "");
        if (rm == 3)
            rounded = uns ? LLVMBuildUIToFP(B, fast, LLVMTypeOf(f), "") : LLVMBuildSIToFP(B, fast, LLVMTypeOf(f), "");
        V inexact = float_ne(ir, rounded, f);
        split_begin(ir, ok, ir_cast(ir, fast, ir->t_i64, 0), inexact, &s);
        r = split_end(ir, &s, call_to_int(ir, n, width, integer, uns, fbits, rm));
    } else
        r = call_to_int(ir, n, width, integer, uns, fbits, rm);
    set_gpr(ir, (unsigned)a->rd, a->sf ? r : ir_u32(ir, r));
}

/* FJCVTZS: Wd, and NZCV 0Z00 */
static void fp_jcvt(SnxIr* ir, const SnxA64Args* a) {
    V operand = fp_reg(ir, (unsigned)a->rn, 64), r = fp_call(ir, "snx_fp_jcvt", ir->t_i64, &operand, 1);
    V z = ir_u32(ir, LLVMBuildLShr(B, r, ir_i64(ir, 32), ""));
    set_gpr(ir, (unsigned)a->rd, ir_u32(ir, r));
    ir_flags_set_nzcv(ir, LLVMBuildShl(B, z, ir_i32(ir, 2), ""));
}

/* BFCVT: a single to a bfloat16 (FEAT_BF16) */
static void fp_bfcvt(SnxIr* ir, const SnxA64Args* a) {
    V operand = fp_reg(ir, (unsigned)a->rn, 32);
    set_fp_reg(ir, (unsigned)a->rd, to_width(ir, fp_call(ir, "snx_fp_bfcvt", ir->t_i64, &operand, 1), 16));
}

/* ---- the entry points ---- */

int snx_ir_fpu_handles(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_FADD_s:
        case SNX_A64_OP_FSUB_s:
        case SNX_A64_OP_FDIV_s:
        case SNX_A64_OP_FMUL_s:
        case SNX_A64_OP_FNMUL_s:
        case SNX_A64_OP_FMAX_s:
        case SNX_A64_OP_FMIN_s:
        case SNX_A64_OP_FMAXNM_s:
        case SNX_A64_OP_FMINNM_s:
        case SNX_A64_OP_FMADD:
        case SNX_A64_OP_FMSUB:
        case SNX_A64_OP_FNMADD:
        case SNX_A64_OP_FNMSUB:
        case SNX_A64_OP_FMOV_s:
        case SNX_A64_OP_FABS_s:
        case SNX_A64_OP_FNEG_s:
        case SNX_A64_OP_FSQRT_s:
        case SNX_A64_OP_FRINTN_s:
        case SNX_A64_OP_FRINTP_s:
        case SNX_A64_OP_FRINTM_s:
        case SNX_A64_OP_FRINTZ_s:
        case SNX_A64_OP_FRINTA_s:
        case SNX_A64_OP_FRINTX_s:
        case SNX_A64_OP_FRINTI_s:
        case SNX_A64_OP_BFCVT_s:
        case SNX_A64_OP_FRINT32Z_s:
        case SNX_A64_OP_FRINT32X_s:
        case SNX_A64_OP_FRINT64Z_s:
        case SNX_A64_OP_FRINT64X_s:
        case SNX_A64_OP_FCVT_s_ds:
        case SNX_A64_OP_FCVT_s_hs:
        case SNX_A64_OP_FCVT_s_sd:
        case SNX_A64_OP_FCVT_s_hd:
        case SNX_A64_OP_FCVT_s_sh:
        case SNX_A64_OP_FCVT_s_dh:
        case SNX_A64_OP_FMOVI_s:
        case SNX_A64_OP_FCMP:
        case SNX_A64_OP_FCCMP:
        case SNX_A64_OP_FCSEL:
        case SNX_A64_OP_SCVTF_g:
        case SNX_A64_OP_UCVTF_g:
        case SNX_A64_OP_FCVTNS_g:
        case SNX_A64_OP_FCVTNU_g:
        case SNX_A64_OP_FCVTPS_g:
        case SNX_A64_OP_FCVTPU_g:
        case SNX_A64_OP_FCVTMS_g:
        case SNX_A64_OP_FCVTMU_g:
        case SNX_A64_OP_FCVTZS_g:
        case SNX_A64_OP_FCVTZU_g:
        case SNX_A64_OP_FCVTAS_g:
        case SNX_A64_OP_FCVTAU_g:
        case SNX_A64_OP_FJCVTZS:
        case SNX_A64_OP_FMOV_ws:
        case SNX_A64_OP_FMOV_sw:
        case SNX_A64_OP_FMOV_xd:
        case SNX_A64_OP_FMOV_dx:
        case SNX_A64_OP_FMOV_xu:
        case SNX_A64_OP_FMOV_ux:
        case SNX_A64_OP_FMOV_xh:
        case SNX_A64_OP_FMOV_hx: return 1;
        default: return 0;
    }
}

void snx_ir_fpu(SnxIr* ir, const SnxA64Insn* insn, SnxU32 word, SnxU64 pc) {
    const SnxA64Args* a = &insn->args;
    unsigned rd = (unsigned)a->rd, rn = (unsigned)a->rn;
    (void)word;
    (void)pc;
    switch (insn->op) {
        case SNX_A64_OP_FADD_s: fp_binary(ir, a, SNX_FP_ADD, 0); break;
        case SNX_A64_OP_FSUB_s: fp_binary(ir, a, SNX_FP_SUB, 0); break;
        case SNX_A64_OP_FDIV_s: fp_binary(ir, a, SNX_FP_DIV, 0); break;
        case SNX_A64_OP_FMUL_s: fp_binary(ir, a, SNX_FP_MUL, 0); break;
        case SNX_A64_OP_FNMUL_s: fp_binary(ir, a, SNX_FP_MUL, 1); break;
        case SNX_A64_OP_FMAX_s: fp_minmax(ir, a, 0, 0); break;
        case SNX_A64_OP_FMIN_s: fp_minmax(ir, a, 1, 0); break;
        case SNX_A64_OP_FMAXNM_s: fp_minmax(ir, a, 0, 1); break;
        case SNX_A64_OP_FMINNM_s: fp_minmax(ir, a, 1, 1); break;
        case SNX_A64_OP_FMADD: fp_fma(ir, a, 0, 0); break;
        case SNX_A64_OP_FMSUB: fp_fma(ir, a, 0, 1); break;
        case SNX_A64_OP_FNMADD: fp_fma(ir, a, 1, 1); break;
        case SNX_A64_OP_FNMSUB: fp_fma(ir, a, 1, 0); break;
        case SNX_A64_OP_FMOV_s: fp_move(ir, a, 0); break;
        case SNX_A64_OP_FABS_s: fp_move(ir, a, 1); break;
        case SNX_A64_OP_FNEG_s: fp_move(ir, a, 2); break;
        case SNX_A64_OP_FSQRT_s: fp_sqrt(ir, a); break;
        case SNX_A64_OP_FRINTN_s: fp_round(ir, a, 0, 0); break;
        case SNX_A64_OP_FRINTP_s: fp_round(ir, a, 1, 0); break;
        case SNX_A64_OP_FRINTM_s: fp_round(ir, a, 2, 0); break;
        case SNX_A64_OP_FRINTZ_s: fp_round(ir, a, 3, 0); break;
        case SNX_A64_OP_FRINTA_s: fp_round(ir, a, 4, 0); break;
        case SNX_A64_OP_FRINTX_s: fp_round(ir, a, -1, 1); break;
        case SNX_A64_OP_FRINTI_s: fp_round(ir, a, -1, 0); break;
        case SNX_A64_OP_BFCVT_s: fp_bfcvt(ir, a); break;
        case SNX_A64_OP_FRINT32Z_s: fp_round_int(ir, a, 3, 32); break;
        case SNX_A64_OP_FRINT32X_s: fp_round_int(ir, a, -1, 32); break;
        case SNX_A64_OP_FRINT64Z_s: fp_round_int(ir, a, 3, 64); break;
        case SNX_A64_OP_FRINT64X_s: fp_round_int(ir, a, -1, 64); break;
        case SNX_A64_OP_FCVT_s_ds: fp_convert(ir, a, 32, 64); break;
        case SNX_A64_OP_FCVT_s_hs: fp_convert(ir, a, 32, 16); break;
        case SNX_A64_OP_FCVT_s_sd: fp_convert(ir, a, 64, 32); break;
        case SNX_A64_OP_FCVT_s_hd: fp_convert(ir, a, 64, 16); break;
        case SNX_A64_OP_FCVT_s_sh: fp_convert(ir, a, 16, 32); break;
        case SNX_A64_OP_FCVT_s_dh: fp_convert(ir, a, 16, 64); break;
        case SNX_A64_OP_FMOVI_s:
            set_fp_reg(ir, rd, ir_i64(ir, expand_imm((unsigned)a->imm, width_of(a->esz))));
            break;
        case SNX_A64_OP_FCMP: fp_cmp(ir, a); break;
        case SNX_A64_OP_FCCMP: fp_ccmp(ir, a); break;
        case SNX_A64_OP_FCSEL: fp_csel(ir, a); break;
        case SNX_A64_OP_SCVTF_g: fp_from_int(ir, a, 0); break;
        case SNX_A64_OP_UCVTF_g: fp_from_int(ir, a, 1); break;
        case SNX_A64_OP_FCVTNS_g: fp_to_int(ir, a, 0, 0); break;
        case SNX_A64_OP_FCVTNU_g: fp_to_int(ir, a, 0, 1); break;
        case SNX_A64_OP_FCVTPS_g: fp_to_int(ir, a, 1, 0); break;
        case SNX_A64_OP_FCVTPU_g: fp_to_int(ir, a, 1, 1); break;
        case SNX_A64_OP_FCVTMS_g: fp_to_int(ir, a, 2, 0); break;
        case SNX_A64_OP_FCVTMU_g: fp_to_int(ir, a, 2, 1); break;
        case SNX_A64_OP_FCVTZS_g: fp_to_int(ir, a, 3, 0); break;
        case SNX_A64_OP_FCVTZU_g: fp_to_int(ir, a, 3, 1); break;
        case SNX_A64_OP_FCVTAS_g: fp_to_int(ir, a, 4, 0); break;
        case SNX_A64_OP_FCVTAU_g: fp_to_int(ir, a, 4, 1); break;
        case SNX_A64_OP_FJCVTZS: fp_jcvt(ir, a); break;
        case SNX_A64_OP_FMOV_ws: set_gpr(ir, rd, fp_reg(ir, rn, 32)); break;
        case SNX_A64_OP_FMOV_sw: set_fp_reg(ir, rd, ir_u32(ir, ir_rx(ir, rn))); break;
        case SNX_A64_OP_FMOV_xd: set_gpr(ir, rd, fp_reg(ir, rn, 64)); break;
        case SNX_A64_OP_FMOV_dx: set_fp_reg(ir, rd, ir_rx(ir, rn)); break;
        case SNX_A64_OP_FMOV_xu: set_gpr(ir, rd, ir_vd(ir, rn, 1)); break; /* Xd = Vn.D[1] */
        case SNX_A64_OP_FMOV_ux: ir_set_vd(ir, rd, 1, ir_rx(ir, rn)); break; /* Vd.D[1] = Xn, D[0] kept */
        case SNX_A64_OP_FMOV_xh: set_gpr(ir, rd, fp_reg(ir, rn, 16)); break;
        case SNX_A64_OP_FMOV_hx: set_fp_reg(ir, rd, ir_cast(ir, ir_rx(ir, rn), ir->t_i16, 0)); break;
        default: break;
    }
}
