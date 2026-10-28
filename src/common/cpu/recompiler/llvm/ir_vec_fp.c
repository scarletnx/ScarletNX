/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/translate-a64.c: the trans_* functions of these instructions, do_fp3_vector,
   do_fp3_scalar, do_fmla_vector_idx, do_fmlal, do_fp_reduction, do_fcvt_f, do_cvtf_f, do_2misc_narrow_vector;
   vec_helper.c's gvec_fcadd*, gvec_fcmla*, do_fmlal, do_bfmlal and float16_to_float32_by_bits), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de>
   Copyright (c) 2018 Linaro */
/* The floating-point AdvSIMD instructions as LLVM IR, keyed on the generated decoder's ops and arguments: three
   same, by element, pairwise and across lanes, the two-register forms (compares with zero, estimates, rounding,
   conversions to and from integers and fixed point, between precisions), FEAT_FHM's widening products, FEAT_FCMA's
   complex additions and products, FEAT_FRINTTS, and FEAT_BF16's BFDOT, BFMMLA, BFMLAL and BFCVTN, on
   halves (FEAT_FP16) as on singles and doubles. Every lane is the architecture's: FPCR (FZ, FZ16, DN, AHP, RMode),
   NaN propagation and FPSR's cumulative flags are the helpers' (helpers/fp.c, through helpers/fp_host.c's host
   arithmetic where it is exact), one call per lane. The common single and double operations first try the host's
   IEEE arithmetic on whole vectors, behind the guards ir_fpu.c's scalar fast paths use, checked in every lane: round
   to nearest, no denormal operand under FZ, a finite result above the smallest normal or an exact zero, and IXC
   computed exactly or already set. */
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include "common/cpu/recompiler/llvm/ir_fp.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/aarch64/helpers/fp_rsqrt_table.h"

typedef LLVMValueRef V;
#define B ir->b

#define FPCR_FZ16  (1u << 19)
#define RM_FPCR    5 /* a rounding mode argument: FPCR's */

/* ---- types and state ---- */

static LLVMTypeRef float_type(SnxIr* ir, unsigned width) {
    return width == 32 ? LLVMFloatTypeInContext(ir->ctx) : LLVMDoubleTypeInContext(ir->ctx);
}
static LLVMTypeRef fvec(SnxIr* ir, unsigned width, unsigned lanes) {
    return LLVMVectorType(float_type(ir, width), lanes);
}
static V KF(SnxIr* ir, unsigned width, unsigned lanes, double v) {
    V one = LLVMConstReal(float_type(ir, width), v), all[16];
    for (unsigned i = 0; i < lanes; ++i) all[i] = one;
    return LLVMConstVector(all, lanes);
}
static V reg(SnxIr* ir, unsigned n, unsigned width, unsigned lanes) {
    return ir_vec_read(ir, n, esz_of(width), 0, lanes);
}

static V not1(SnxIr* ir, V a) { return LLVMBuildNot(B, a, ""); }


/* ---- one lane through the helpers (cpu.h), declared as ir_fpu.c declares them ---- */


enum { H_BINARY, H_FMA, H_UNARY, H_MINMAX, H_COMPARE, H_CONVERT, H_ROUND, H_ROUND_INT, H_TO_INT, H_FROM_INT, H_BFCVT };
typedef struct {
    unsigned helper, width;
    unsigned op;          /* H_BINARY, H_UNARY: the operation; H_MINMAX: minimum; H_COMPARE: signal */
    unsigned p1, p2, p3;  /* H_MINMAX: numeric; H_CONVERT: to; H_ROUND: exact; H_ROUND_INT: intsize;
                             H_TO_INT, H_FROM_INT: integer, uns, fbits */
    V rm;                 /* the i32 rounding mode of H_CONVERT, H_ROUND, H_ROUND_INT, H_TO_INT */
} LaneOp;

/* name(c, operands...): the helpers read c->fpcr and change c->fpsr, and no register (ir_noregs, as ir_fpu.c's) */
static V fp_lane_call(SnxIr* ir, const char* name, LLVMTypeRef ret, const V* operands, unsigned count) {
    V r = ir_vec_call(ir, name, ret, operands, count);
    ir_noregs(ir, LLVMGetNamedFunction(ir->mod, name));
    return r;
}

/* the i64 result (i32 NZCV for H_COMPARE) of one lane's operands (iN) */
static V lane(SnxIr* ir, const LaneOp* o, V x, V y, V z) {
    V ops[6];
    unsigned n = 0;
    const char* name;
    LLVMTypeRef ret = ir->t_i64;
    switch (o->helper) {
        case H_BINARY:
            name = "snx_fp_host_binary";
            ops[n++] = u64_of(ir, x);
            ops[n++] = u64_of(ir, y);
            ops[n++] = ir_i32(ir, o->width);
            ops[n++] = ir_i32(ir, o->op);
            break;
        case H_FMA: /* x + y * z */
            name = "snx_fp_host_fma";
            ops[n++] = u64_of(ir, x);
            ops[n++] = u64_of(ir, y);
            ops[n++] = u64_of(ir, z);
            ops[n++] = ir_i32(ir, o->width);
            break;
        case H_UNARY:
            name = "snx_fp_host_unary";
            ops[n++] = u64_of(ir, x);
            ops[n++] = ir_i32(ir, o->width);
            ops[n++] = ir_i32(ir, o->op);
            break;
        case H_MINMAX:
            name = "snx_fp_host_minmax";
            ops[n++] = u64_of(ir, x);
            ops[n++] = u64_of(ir, y);
            ops[n++] = ir_i32(ir, o->width);
            ops[n++] = ir_i32(ir, o->op);
            ops[n++] = ir_i32(ir, o->p1);
            break;
        case H_COMPARE:
            name = "snx_fp_host_compare";
            ret = ir->t_i32;
            ops[n++] = u64_of(ir, x);
            ops[n++] = u64_of(ir, y);
            ops[n++] = ir_i32(ir, o->width);
            ops[n++] = ir_i32(ir, o->op);
            break;
        case H_CONVERT:
            name = "snx_fp_convert";
            ops[n++] = u64_of(ir, x);
            ops[n++] = ir_i32(ir, o->width);
            ops[n++] = ir_i32(ir, o->p1);
            ops[n++] = o->rm;
            break;
        case H_ROUND:
            name = "snx_fp_round";
            ops[n++] = u64_of(ir, x);
            ops[n++] = ir_i32(ir, o->width);
            ops[n++] = o->rm;
            ops[n++] = ir_i32(ir, o->p1);
            break;
        case H_ROUND_INT:
            name = "snx_fp_round_int";
            ops[n++] = u64_of(ir, x);
            ops[n++] = ir_i32(ir, o->width);
            ops[n++] = o->rm;
            ops[n++] = ir_i32(ir, o->p1);
            break;
        case H_TO_INT:
            name = "snx_fp_to_int";
            ops[n++] = u64_of(ir, x);
            ops[n++] = ir_i32(ir, o->width);
            ops[n++] = ir_i32(ir, o->p1);
            ops[n++] = ir_i32(ir, o->p2);
            ops[n++] = ir_i32(ir, o->p3);
            ops[n++] = o->rm;
            break;
        case H_FROM_INT:
            name = "snx_fp_from_int";
            ops[n++] = u64_of(ir, x);
            ops[n++] = ir_i32(ir, o->p1);
            ops[n++] = ir_i32(ir, o->width);
            ops[n++] = ir_i32(ir, o->p2);
            ops[n++] = ir_i32(ir, o->p3);
            break;
        default:
            name = "snx_fp_bfcvt";
            ops[n++] = u64_of(ir, x);
            break;
    }
    return fp_lane_call(ir, name, ret, ops, n);
}

/* v in a local of its type, for a helper to read */
static V spill(SnxIr* ir, V v) {
    V local = ir_local(ir, LLVMTypeOf(v));
    LLVMSetAlignment(LLVMBuildStore(B, v, local), 8);
    return local;
}

/* the lanes of X (Y, Z: the other operands, or NULL) through the helper, as <lanes x i out_bits>. The arithmetic,
   the estimates and FMAX and FMIN take a vector in one call (snx_fp_host_lanes), the rest one call a lane */
static V map(SnxIr* ir, const LaneOp* o, V X, V Y, V Z, unsigned out_bits) {
    unsigned lanes = ir_vec_count(X);
    LLVMTypeRef type = LLVMVectorType(LLVMIntTypeInContext(ir->ctx, out_bits), lanes);
    V r = LLVMGetUndef(type);
    if (lanes > 1 && o->helper <= H_MINMAX) {
        V out = ir_local(ir, type), x = spill(ir, X), ops[5];
        ops[0] = out;
        ops[1] = x;
        ops[2] = Y ? spill(ir, Y) : x;
        ops[3] = Z ? spill(ir, Z) : x;
        ops[4] = ir_i32(ir, o->helper | (o->width / 8) << 4 | lanes << 8 | o->op << 12 | o->p1 << 16);
        fp_lane_call(ir, "snx_fp_host_lanes", ir->t_void, ops, 5);
        V load = LLVMBuildLoad2(B, type, out, "");
        LLVMSetAlignment(load, 8);
        return load;
    }
    for (unsigned i = 0; i < lanes; ++i) {
        V at = ir_i32(ir, i), x = LLVMBuildExtractElement(B, X, at, ""), y = NULL, z = NULL;
        if (Y) y = LLVMBuildExtractElement(B, Y, at, "");
        if (Z) z = LLVMBuildExtractElement(B, Z, at, "");
        V v = lane(ir, o, x, y, z);
        r = LLVMBuildInsertElement(B, r, ir_cast(ir, v, LLVMIntTypeInContext(ir->ctx, out_bits), 0), at, "");
    }
    return r;
}

static LaneOp op_binary(unsigned width, unsigned op) {
    LaneOp o = {H_BINARY, 0, 0, 0, 0, 0, NULL};
    o.width = width;
    o.op = op;
    return o;
}
static LaneOp op_simple(unsigned helper, unsigned width) {
    LaneOp o = {0, 0, 0, 0, 0, 0, NULL};
    o.helper = helper;
    o.width = width;
    return o;
}

/* ---- lane predicates on <lanes x iN> encodings ---- */

static SnxU64 inf_bits(unsigned width) {
    return width == 16 ? 0x7C00u : width == 32 ? 0x7F800000u : 0x7FF0000000000000ull;
}
static SnxU64 min_normal(unsigned width) {
    return width == 16 ? 0x0400u : width == 32 ? 0x00800000u : 0x0010000000000000ull;
}
#define RESIDUAL_FLOOR 0x03F0000000000000ull /* ir_fpu.c's: doubles whose FMA residuals stay representable */

static V magnitude(SnxIr* ir, unsigned width, V bits) {
    return LLVMBuildAnd(B, bits, ir_vec_like(ir, bits, sign_mask(width) - 1), "");
}
static V is_zero(SnxIr* ir, unsigned width, V bits) {
    return LLVMBuildICmp(B, LLVMIntEQ, magnitude(ir, width, bits), ir_vec_like(ir, bits, 0), "");
}
static V is_nan(SnxIr* ir, unsigned width, V bits) {
    return LLVMBuildICmp(B, LLVMIntUGT, magnitude(ir, width, bits), ir_vec_like(ir, bits, inf_bits(width)), "");
}
static V magnitude_in(SnxIr* ir, unsigned width, V bits, SnxU64 lo, SnxU64 hi) {
    V offset = LLVMBuildSub(B, magnitude(ir, width, bits), ir_vec_like(ir, bits, lo), "");
    return LLVMBuildICmp(B, LLVMIntULT, offset, ir_vec_like(ir, bits, hi - lo), "");
}
static V is_denormal(SnxIr* ir, unsigned width, V bits) { return magnitude_in(ir, width, bits, 1, min_normal(width)); }
static V roomy(SnxIr* ir, unsigned width, V bits) {
    return magnitude_in(ir, width, bits, min_normal(width) + 1, inf_bits(width));
}
/* i1: no lane is a denormal FPCR.FZ flushes */
static V unflushed(SnxIr* ir, unsigned width, V bits) {
    return either(ir, fpcr_clear(ir, FPCR_FZ), ir_vec_all(ir, not1(ir, is_denormal(ir, width, bits))));
}
static V no_nan(SnxIr* ir, unsigned width, V bits) { return ir_vec_all(ir, not1(ir, is_nan(ir, width, bits))); }

static V as_float(SnxIr* ir, V bits, unsigned width) {
    return LLVMBuildBitCast(B, bits, fvec(ir, width, ir_vec_count(bits)), "");
}
static V as_bits(SnxIr* ir, V f, unsigned width) {
    return LLVMBuildBitCast(B, f, ir_vec_type(ir, esz_of(width), ir_vec_count(f)), "");
}
static V widen_f(SnxIr* ir, V f) { return LLVMBuildFPExt(B, f, fvec(ir, 64, ir_vec_count(f)), ""); }
static V nonzero(SnxIr* ir, V f, unsigned width) {
    return float_ne(ir, f, KF(ir, width, ir_vec_count(f), 0.0));
}

/* ---- the host's value where ok holds, the helpers' otherwise ---- */

typedef struct {
    LLVMBasicBlockRef fast_end, join;
    V fast;
} Split;
/* quick (NULL: none) implies ok and is cheaper: where it holds, ok's own test is skipped, which sink then takes out
   of the fast path */
static void split_begin(SnxIr* ir, V quick, V ok, V fast, V inexact, Split* s) {
    LLVMBasicBlockRef on = ir_block(ir, "vfp_fast"), off = ir_block(ir, "vfp_slow");
    s->join = ir_block(ir, "vfp_join");
    s->fast = fast;
    if (quick) {
        LLVMBasicBlockRef check = ir_block(ir, "vfp_check");
        ir_likely(ir, LLVMBuildCondBr(B, quick, on, check));
        LLVMPositionBuilderAtEnd(B, check);
    }
    ir_likely(ir, LLVMBuildCondBr(B, ok, on, off));
    LLVMPositionBuilderAtEnd(B, on);
    /* only while IXC is clear: it is sticky, and LLVM sinks the inexactness test into that block */
    if (inexact) {
        LLVMBasicBlockRef clear = ir_block(ir, "vfp_ixc"), done = ir_block(ir, "vfp_ixc_done");
        ir_unlikely(ir, LLVMBuildCondBr(B, LLVMBuildNot(B, ixc_set(ir), ""), clear, done));
        LLVMPositionBuilderAtEnd(B, clear);
        V bit = LLVMBuildShl(B, LLVMBuildZExt(B, inexact, ir->t_i32, ""), ir_i32(ir, 4), "");
        ir_cpu_store(ir, LLVMBuildOr(B, fpsr(ir), bit, ""), offsetof(CPU, fpsr));
        LLVMBuildBr(B, done);
        LLVMPositionBuilderAtEnd(B, done);
    }
    s->fast_end = LLVMGetInsertBlock(B);
    LLVMBuildBr(B, s->join);
    LLVMPositionBuilderAtEnd(B, off);
}
static V split_end(SnxIr* ir, Split* s, V slow) {
    LLVMBasicBlockRef slow_end = LLVMGetInsertBlock(B), blocks[2];
    V values[2];
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

/* ---- the vector fast paths (ir_fpu.c's, lane by lane) ---- */

typedef struct {
    V value, ok, inexact; /* ok, inexact: i1 over all lanes; inexact NULL when IXC must already be set */
    V quick;              /* NULL, or an i1 that implies ok and costs less: every lane above the smallest normal */
} Fast;

/* a op b, op SNX_FP_ADD, SUB, MUL (MULX: the same without infinities), DIV */
static void fast_binary(SnxIr* ir, unsigned op, unsigned width, V a, V b, Fast* f) {
    V fa = as_float(ir, a, width), fb = as_float(ir, b, width), r, rb, exact_zero, cut = NULL, inexact = NULL;
    switch (op) {
        case SNX_FP_ADD:
        case SNX_FP_SUB: {
            if (op == SNX_FP_SUB) fb = LLVMBuildFNeg(B, fb, "");
            r = LLVMBuildFAdd(B, fa, fb, "");
            V t = LLVMBuildFSub(B, r, fa, "");
            V va = LLVMBuildFSub(B, fa, LLVMBuildFSub(B, r, t, ""), "");
            V vb = LLVMBuildFSub(B, fb, t, "");
            inexact = nonzero(ir, LLVMBuildFAdd(B, va, vb, ""), width);
            rb = as_bits(ir, r, width);
            exact_zero = is_zero(ir, width, rb);
            break;
        }
        case SNX_FP_MUL:
        case SNX_FP_MULX:
            r = LLVMBuildFMul(B, fa, fb, "");
            rb = as_bits(ir, r, width);
            exact_zero = both(ir, is_zero(ir, width, rb), either(ir, is_zero(ir, width, a), is_zero(ir, width, b)));
            if (width == 32)
                inexact = float_ne(ir, LLVMBuildFMul(B, widen_f(ir, fa), widen_f(ir, fb), ""), widen_f(ir, r));
            else if (host_fma(ir)) {
                inexact = nonzero(ir, fma3(ir, fa, fb, LLVMBuildFNeg(B, r, "")), 64);
                cut = either(ir, exact_zero, magnitude_in(ir, 64, rb, RESIDUAL_FLOOR, inf_bits(64)));
            }
            break;
        default: /* SNX_FP_DIV */
            r = LLVMBuildFDiv(B, fa, fb, "");
            rb = as_bits(ir, r, width);
            exact_zero = both(ir, is_zero(ir, width, rb), is_zero(ir, width, a));
            if (width == 32) {
                V product = LLVMBuildFMul(B, widen_f(ir, r), widen_f(ir, fb), "");
                inexact = nonzero(ir, LLVMBuildFSub(B, widen_f(ir, fa), product, ""), 64);
            } else if (host_fma(ir)) {
                inexact = nonzero(ir, fma3(ir, LLVMBuildFNeg(B, r, ""), fb, fa), 64);
                cut = either(ir, is_zero(ir, 64, a), magnitude_in(ir, 64, a, RESIDUAL_FLOOR, inf_bits(64)));
            }
            /* a zero divided by an infinity is an exact zero, though its residual (0 * inf) is a NaN */
            if (inexact) inexact = both(ir, inexact, not1(ir, exact_zero));
            break;
    }
    V ok = either(ir, roomy(ir, width, rb), exact_zero);
    if (cut) ok = both(ir, ok, cut);
    f->value = rb;
    f->ok = both(ir, both(ir, fpcr_clear(ir, FPCR_RMODE), both(ir, unflushed(ir, width, a), unflushed(ir, width, b))),
                 ir_vec_all(ir, ok));
    f->inexact = inexact ? ir_vec_any(ir, inexact) : NULL;
    if (!inexact) f->ok = both(ir, f->ok, ixc_set(ir));
    f->quick = NULL;
    if (!cut) {
        f->quick = both(ir, fpcr_clear(ir, FPCR_RMODE | FPCR_FZ), ir_vec_all(ir, roomy(ir, width, rb)));
        if (!inexact) f->quick = both(ir, f->quick, ixc_set(ir));
    }
}

/* add + n * m rounded once, on hosts with FMA, once IXC is set */
static void fast_fma(SnxIr* ir, unsigned width, V add, V n, V m, Fast* f) {
    V r = as_bits(ir, fma3(ir, as_float(ir, n, width), as_float(ir, m, width), as_float(ir, add, width)), width);
    V zero_product = either(ir, is_zero(ir, width, n), is_zero(ir, width, m));
    V result = either(ir, roomy(ir, width, r), both(ir, is_zero(ir, width, r), zero_product));
    V operands = both(ir, unflushed(ir, width, add), both(ir, unflushed(ir, width, n), unflushed(ir, width, m)));
    f->value = r;
    f->ok = both(ir, both(ir, fpcr_clear(ir, FPCR_RMODE), ixc_set(ir)), both(ir, operands, ir_vec_all(ir, result)));
    f->quick = both(ir, both(ir, fpcr_clear(ir, FPCR_RMODE | FPCR_FZ), ixc_set(ir)), ir_vec_all(ir, roomy(ir, width, r)));
    f->inexact = NULL;
}

/* FRECPS (2 - a * b) and FRSQRTS ((3 - a * b) / 2), fused, on hosts with FMA, once IXC is set: halving a result
   above twice the smallest normal is exact */
static void fast_step(SnxIr* ir, unsigned width, unsigned rsqrt, V a, V b, Fast* f) {
    unsigned lanes = ir_vec_count(a);
    V fa = LLVMBuildFNeg(B, as_float(ir, a, width), "");
    V r = fma3(ir, fa, as_float(ir, b, width), KF(ir, width, lanes, rsqrt ? 3.0 : 2.0));
    if (rsqrt) r = LLVMBuildFMul(B, r, KF(ir, width, lanes, 0.5), "");
    f->value = as_bits(ir, r, width);
    f->ok = both(ir, both(ir, fpcr_clear(ir, FPCR_RMODE), ixc_set(ir)),
                 both(ir, both(ir, unflushed(ir, width, a), unflushed(ir, width, b)),
                      ir_vec_all(ir, roomy(ir, width, f->value))));
    f->inexact = NULL;
    f->quick = NULL;
}

static void fast_sqrt(SnxIr* ir, unsigned width, V a, Fast* f) {
    V fa = as_float(ir, a, width), r = ir_intrinsic(ir, "llvm.sqrt", LLVMTypeOf(fa), &fa, 1);
    V rb = as_bits(ir, r, width), zero = is_zero(ir, width, a), inexact = NULL;
    V ok = either(ir, roomy(ir, width, rb), zero);
    if (width == 32)
        inexact = float_ne(ir, widen_f(ir, fa), LLVMBuildFMul(B, widen_f(ir, r), widen_f(ir, r), ""));
    else if (host_fma(ir)) {
        inexact = nonzero(ir, fma3(ir, LLVMBuildFNeg(B, r, ""), r, fa), 64);
        ok = both(ir, ok, either(ir, zero, magnitude_in(ir, 64, a, RESIDUAL_FLOOR, inf_bits(64))));
    }
    f->value = rb;
    f->ok = both(ir, both(ir, fpcr_clear(ir, FPCR_RMODE), unflushed(ir, width, a)), ir_vec_all(ir, ok));
    f->inexact = inexact ? ir_vec_any(ir, inexact) : NULL;
    if (!inexact) f->ok = both(ir, f->ok, ixc_set(ir));
    f->quick = NULL;
}

/* FMAX, FMIN and the NM forms without a NaN or a flushed denormal: the larger or smaller operand, of equal ones
   their AND (max) or OR (min) */
static void fast_minmax(SnxIr* ir, unsigned width, unsigned minimum, V n, V m, Fast* f) {
    V fn = as_float(ir, n, width), fm = as_float(ir, m, width);
    LLVMRealPredicate wins = minimum ? LLVMRealOLT : LLVMRealOGT;
    V r = minimum ? LLVMBuildOr(B, n, m, "") : LLVMBuildAnd(B, n, m, "");
    r = LLVMBuildSelect(B, LLVMBuildFCmp(B, wins, fm, fn, ""), m, r, "");
    r = LLVMBuildSelect(B, LLVMBuildFCmp(B, wins, fn, fm, ""), n, r, "");
    f->value = r;
    f->ok = both(ir, both(ir, no_nan(ir, width, n), no_nan(ir, width, m)),
                 both(ir, unflushed(ir, width, n), unflushed(ir, width, m)));
    f->inexact = NULL;
    f->quick = NULL;
}

/* without FPSR's flags (snx_ir_fpsr_flags): the host's lanes wherever none is a NaN, without FZ, and where the
   exact guard holds (ir_fpu.c's relax) */
static void relax(SnxIr* ir, unsigned width, Fast* f) {
    if (snx_ir_fpsr_exact()) return;
    f->ok = either(ir, f->ok, both(ir, fpcr_clear(ir, FPCR_RMODE | FPCR_FZ), no_nan(ir, width, f->value)));
    f->inexact = NULL;
}

/* ---- the operations, fast where they can be ---- */

/* SNX_FP_* of a and b (lanes of width) */
static V binary(SnxIr* ir, unsigned op, unsigned width, V a, V b) {
    LaneOp o = op_binary(width, op);
    if (width != 16 && op <= SNX_FP_MULX && op != SNX_FP_ABD) {
        Fast f;
        Split s;
        fast_binary(ir, op, width, a, b, &f);
        relax(ir, width, &f);
        split_begin(ir, f.quick, f.ok, f.value, f.inexact, &s);
        return split_end(ir, &s, map(ir, &o, a, b, NULL, width));
    }
    if (width != 16 && host_fma(ir) && (op == SNX_FP_RECPS || op == SNX_FP_RSQRTS)) {
        Fast f;
        Split s;
        fast_step(ir, width, op == SNX_FP_RSQRTS, a, b, &f);
        relax(ir, width, &f);
        split_begin(ir, f.quick, f.ok, f.value, NULL, &s);
        return split_end(ir, &s, map(ir, &o, a, b, NULL, width));
    }
    return map(ir, &o, a, b, NULL, width);
}

static V fma_lanes(SnxIr* ir, unsigned width, V add, V n, V m) {
    LaneOp o = op_simple(H_FMA, width);
    if (width != 16 && host_fma(ir)) {
        Fast f;
        Split s;
        fast_fma(ir, width, add, n, m, &f);
        relax(ir, width, &f);
        split_begin(ir, f.quick, f.ok, f.value, NULL, &s);
        return split_end(ir, &s, map(ir, &o, add, n, m, width));
    }
    return map(ir, &o, add, n, m, width);
}

static V minmax(SnxIr* ir, unsigned width, unsigned minimum, unsigned numeric, V n, V m) {
    LaneOp o = op_simple(H_MINMAX, width);
    o.op = minimum;
    o.p1 = numeric;
    if (width != 16) {
        Fast f;
        Split s;
        fast_minmax(ir, width, minimum, n, m, &f);
        split_begin(ir, f.quick, f.ok, f.value, NULL, &s);
        return split_end(ir, &s, map(ir, &o, n, m, NULL, width));
    }
    return map(ir, &o, n, m, NULL, width);
}

/* The normal path supplies a bounded index for each lane. AVX2 has a native 32-bit gather.
   Other hosts retain the exact helper path until their lowering is validated. */
static V rsqrt_table(SnxIr* ir, V index) {
    LLVMTypeRef table_type = LLVMArrayType(ir->t_i32, 384);
    V table = LLVMGetNamedGlobal(ir->mod, "snx.frsqrte.table");
    if (!table) {
        V entries[384];
        for (unsigned i = 0; i < 384; i++) entries[i] = ir_i32(ir, fp_rsqrt_estimates[i] + 256u);
        table = LLVMAddGlobal(ir->mod, table_type, "snx.frsqrte.table");
        LLVMSetInitializer(table, LLVMConstArray(ir->t_i32, entries, 384));
        LLVMSetGlobalConstant(table, 1);
        LLVMSetLinkage(table, LLVMPrivateLinkage);
        LLVMSetUnnamedAddress(table, LLVMGlobalUnnamedAddr);
        LLVMSetAlignment(table, 4);
    }
    V offsets[] = {ir_i32(ir, 0), index};
    V pointers = LLVMBuildInBoundsGEP2(B, table_type, table, offsets, 2, "");
    unsigned lanes = ir_vec_count(index);
    LLVMTypeRef type = LLVMTypeOf(index), mask_type = LLVMVectorType(LLVMInt1TypeInContext(ir->ctx), lanes);
    LLVMTypeRef overloads[] = {type, LLVMTypeOf(pointers)};
    unsigned id = LLVMLookupIntrinsicID("llvm.masked.gather", sizeof "llvm.masked.gather" - 1);
    V function = LLVMGetIntrinsicDeclaration(ir->mod, id, overloads, 2);
    LLVMTypeRef signature = LLVMGlobalGetValueType(function);
    V args[4] = {pointers};
    unsigned n = 1;
    /* LLVM 22 removed the alignment operand. Ask the installed LLVM for its signature. */
    if (LLVMCountParamTypes(signature) == 4) args[n++] = ir_i32(ir, 4);
    args[n++] = ir_vec_k(ir, mask_type, 1);
    args[n++] = LLVMGetUndef(type);
    return LLVMBuildCall2(B, signature, function, args, n, "");
}

/* Positive normal f32 estimates use only their raw exponent and the top fraction bits. No rounding or exception
   occurs, so FPCR does not affect this path and sticky FPSR is unchanged. */
static V rsqrt_normal(SnxIr* ir, V a) {
    V exponent = LLVMBuildAnd(B, LLVMBuildLShr(B, a, ir_vec_like(ir, a, 23), ""), ir_vec_like(ir, a, 255), "");
    V fraction = LLVMBuildAnd(B, a, ir_vec_like(ir, a, 0x7fffff), "");
    V odd = LLVMBuildICmp(B, LLVMIntNE, LLVMBuildAnd(B, exponent, ir_vec_like(ir, a, 1), ""),
                        ir_vec_like(ir, a, 0), "");
    V low = LLVMBuildOr(B, LLVMBuildLShr(B, fraction, ir_vec_like(ir, a, 16), ""), ir_vec_like(ir, a, 128), "");
    V high = LLVMBuildOr(B, LLVMBuildLShr(B, fraction, ir_vec_like(ir, a, 15), ""), ir_vec_like(ir, a, 256), "");
    V index = LLVMBuildSub(B, LLVMBuildSelect(B, odd, low, high, ""), ir_vec_like(ir, a, 128), "");
    V estimate = LLVMBuildAnd(B, rsqrt_table(ir, index), ir_vec_like(ir, a, 255), "");
    V result_exp = LLVMBuildLShr(B, LLVMBuildSub(B, ir_vec_like(ir, a, 380), exponent, ""),
                                ir_vec_like(ir, a, 1), "");
    return LLVMBuildOr(B, LLVMBuildShl(B, result_exp, ir_vec_like(ir, a, 23), ""),
                       LLVMBuildShl(B, estimate, ir_vec_like(ir, a, 15), ""), "");
}

static V unary(SnxIr* ir, unsigned op, unsigned width, V a) {
    LaneOp o = op_simple(H_UNARY, width);
    o.op = op;
    if (op == SNX_FP_RSQRTE && width == 32 && ir_vec_count(a) > 1 && ir->features->x86 && ir->features->avx2) {
        V offset = LLVMBuildSub(B, a, ir_vec_like(ir, a, 0x00800000), "");
        V ok = ir_vec_all(ir, LLVMBuildICmp(B, LLVMIntULT, offset, ir_vec_like(ir, a, 0x7f000000), ""));
        LLVMBasicBlockRef on = ir_block(ir, "vfp_fast"), off = ir_block(ir, "vfp_slow");
        Split s;
        s.join = ir_block(ir, "vfp_join");
        ir_likely(ir, LLVMBuildCondBr(B, ok, on, off));
        LLVMPositionBuilderAtEnd(B, on);
        s.fast = rsqrt_normal(ir, a);
        s.fast_end = LLVMGetInsertBlock(B);
        LLVMBuildBr(B, s.join);
        LLVMPositionBuilderAtEnd(B, off);
        return split_end(ir, &s, map(ir, &o, a, NULL, NULL, width));
    }
    if (op == SNX_FP_SQRT && width != 16 && host_native(ir)) {
        Fast f;
        Split s;
        fast_sqrt(ir, width, a, &f);
        relax(ir, width, &f);
        split_begin(ir, f.quick, f.ok, f.value, f.inexact, &s);
        return split_end(ir, &s, map(ir, &o, a, NULL, NULL, width));
    }
    return map(ir, &o, a, NULL, NULL, width);
}

/* the compares' lane masks: a == b, a >= b, a > b (abs: of the magnitudes, FACGE and FACGT). Without a NaN
   nothing is raised and the host orders as FPCompare does */
enum { CMP_EQ, CMP_GE, CMP_GT };
/* the lanes' masks from snx_fp_compare's NZCV: equal 0110, greater 0010 (GE: either, NZCV & 1011 == 0010) */
static V compare_slow(SnxIr* ir, const LaneOp* o, unsigned cmp, V a, V b) {
    V nzcv = map(ir, o, a, b, NULL, 32);
    LLVMTypeRef type = LLVMTypeOf(nzcv);
    if (cmp == CMP_GE) nzcv = LLVMBuildAnd(B, nzcv, ir_vec_k(ir, type, 0xB), "");
    V r = LLVMBuildICmp(B, LLVMIntEQ, nzcv, ir_vec_k(ir, type, cmp == CMP_EQ ? 6 : 2), "");
    return LLVMBuildSExt(B, r, LLVMTypeOf(a), "");
}
static V compare(SnxIr* ir, unsigned cmp, unsigned width, V a, V b, int abs) {
    LaneOp o = op_simple(H_COMPARE, width);
    if (abs) {
        a = magnitude(ir, width, a);
        b = magnitude(ir, width, b);
    }
    o.op = cmp != CMP_EQ;
    if (width != 16) {
        static const LLVMRealPredicate predicates[] = {LLVMRealOEQ, LLVMRealOGE, LLVMRealOGT};
        V fa = as_float(ir, a, width), fb = as_float(ir, b, width);
        Split s;
        V ok = both(ir, ir_vec_all(ir, LLVMBuildFCmp(B, LLVMRealORD, fa, fb, "")),
                    both(ir, unflushed(ir, width, a), unflushed(ir, width, b)));
        V fast = LLVMBuildSExt(B, LLVMBuildFCmp(B, predicates[cmp], fa, fb, ""), LLVMTypeOf(a), "");
        split_begin(ir, NULL, ok, fast, NULL, &s);
        return split_end(ir, &s, compare_slow(ir, &o, cmp, a, b));
    }
    return compare_slow(ir, &o, cmp, a, b);
}

/* FRINT[NPMZA] (rm 0-4), FRINTX and FRINTI (rm RM_FPCR; exact: FRINTX's IXC). The host's rounding instructions
   give the encoding's modes exactly for every lane but a NaN or a flushed denormal, and raise nothing (ties away
   from zero only on AArch64) */
static V round_lanes(SnxIr* ir, unsigned width, unsigned rm, unsigned exact, V a) {
    LaneOp o = op_simple(H_ROUND, width);
    o.p1 = exact;
    o.rm = rm == RM_FPCR ? fpcr_rmode(ir) : ir_i32(ir, rm);
    if (width != 16 && rm < RM_FPCR && host_round(ir) && (rm != 4 || ir->features->aarch64)) {
        static const char* const names[] = {"llvm.roundeven", "llvm.ceil", "llvm.floor", "llvm.trunc", "llvm.round"};
        V fa = as_float(ir, a, width), r = ir_intrinsic(ir, names[rm], LLVMTypeOf(fa), &fa, 1);
        Split s;
        split_begin(ir, NULL, both(ir, no_nan(ir, width, a), unflushed(ir, width, a)), as_bits(ir, r, width), NULL, &s);
        return split_end(ir, &s, map(ir, &o, a, NULL, NULL, width));
    }
    return map(ir, &o, a, NULL, NULL, width);
}

/* SCVTF, UCVTF of integer lanes as wide as the floats (halves: 16-bit integers), scaled by 2^-fbits: the host's
   when every magnitude fits the significand (exact in any mode), or in round to nearest once IXC is set */
static V from_int(SnxIr* ir, unsigned width, unsigned uns, unsigned fbits, V v) {
    LaneOp o = op_simple(H_FROM_INT, width);
    o.p1 = width;
    o.p2 = uns;
    o.p3 = fbits;
    if (width != 16) {
        unsigned precision = width == 32 ? 24 : 53, lanes = ir_vec_count(v);
        LLVMTypeRef type = fvec(ir, width, lanes);
        V f, fits;
        Split s;
        if (uns)
            fits = LLVMBuildICmp(B, LLVMIntULT, v, ir_vec_like(ir, v, (SnxU64)1 << precision), "");
        else
            fits = LLVMBuildICmp(B, LLVMIntULT, LLVMBuildAdd(B, v, ir_vec_like(ir, v, (SnxU64)1 << precision), ""),
                                 ir_vec_like(ir, v, (SnxU64)1 << (precision + 1)), "");
        /* an unsigned lane that fits converts as a signed one: x86 before AVX-512 has no unsigned lane conversion,
           and LLVM's sum of two exact parts there is -0 for 0 when the host rounds toward minus infinity */
        if (uns)
            f = LLVMBuildSelect(B, fits, LLVMBuildSIToFP(B, v, type, ""), LLVMBuildUIToFP(B, v, type, ""), "");
        else
            f = LLVMBuildSIToFP(B, v, type, "");
        if (fbits) f = LLVMBuildFMul(B, f, KF(ir, width, lanes, 1.0 / (double)((SnxU64)1 << (fbits - 1)) / 2.0), "");
        V ok = either(ir, ir_vec_all(ir, fits), both(ir, fpcr_clear(ir, FPCR_RMODE), ixc_set(ir)));
        split_begin(ir, NULL, ok, as_bits(ir, f, width), NULL, &s);
        return split_end(ir, &s, map(ir, &o, v, NULL, NULL, width));
    }
    return map(ir, &o, v, NULL, NULL, width);
}

/* FCVT[NPMZA][SU] (rm 0-4) to integer lanes of integer bits, scaled by 2^fbits: the host's where the rounded value
   is in range in every lane and nothing is flushed, IXC where rounding changed one */
static V to_int(SnxIr* ir, unsigned width, unsigned integer, unsigned uns, unsigned fbits, unsigned rm, V a) {
    LaneOp o = op_simple(H_TO_INT, width);
    o.p1 = integer;
    o.p2 = uns;
    o.p3 = fbits;
    o.rm = ir_i32(ir, rm);
    if (width != 16 && integer == width && (rm == 3 || (host_round(ir) && (rm != 4 || ir->features->aarch64)))) {
        static const char* const names[] = {"llvm.roundeven", "llvm.ceil", "llvm.floor", NULL, "llvm.round"};
        unsigned precision = width == 32 ? 24 : 53, lanes = ir_vec_count(a);
        LLVMTypeRef type = ir_vec_type(ir, esz_of(integer), lanes);
        V f = as_float(ir, a, width), low;
        double top = (double)((SnxU64)1 << (integer - 1)) * (uns ? 2.0 : 1.0);
        Split s;
        if (fbits) f = LLVMBuildFMul(B, f, KF(ir, width, lanes, (double)((SnxU64)1 << (fbits - 1)) * 2.0), "");
        V rounded = rm == 3 ? f : ir_intrinsic(ir, names[rm], LLVMTypeOf(f), &f, 1);
        if (uns)
            low = LLVMBuildFCmp(B, LLVMRealOGT, rounded, KF(ir, width, lanes, -1.0), "");
        else if (integer < precision)
            low = LLVMBuildFCmp(B, LLVMRealOGT, rounded, KF(ir, width, lanes, -top - 1.0), "");
        else
            low = LLVMBuildFCmp(B, LLVMRealOGE, rounded, KF(ir, width, lanes, -top), "");
        V high = LLVMBuildFCmp(B, LLVMRealOLT, rounded, KF(ir, width, lanes, top), "");
        V ok = both(ir, ir_vec_all(ir, both(ir, low, high)), unflushed(ir, width, a));
        V fast = uns ? LLVMBuildFPToUI(B, rounded, type, "") : LLVMBuildFPToSI(B, rounded, type, "");
        if (rm == 3)
            rounded = uns ? LLVMBuildUIToFP(B, fast, LLVMTypeOf(f), "") : LLVMBuildSIToFP(B, fast, LLVMTypeOf(f), "");
        split_begin(ir, NULL, ok, fast, ir_vec_any(ir, float_ne(ir, rounded, f)), &s);
        return split_end(ir, &s, map(ir, &o, a, NULL, NULL, integer));
    }
    return map(ir, &o, a, NULL, NULL, integer);
}

/* FCVTL and FCVTN between precisions (odd: FCVTXN's rounding to odd). Singles widen exactly unless a NaN or a
   flushed denormal; doubles narrow in round to nearest as the host does when no lane is a NaN and every single is
   finite and above the smallest normal, or the double a zero */
static V convert(SnxIr* ir, unsigned from, unsigned to, int odd, V a) {
    LaneOp o = op_simple(H_CONVERT, from);
    unsigned lanes = ir_vec_count(a);
    o.p1 = to;
    o.rm = odd ? ir_i32(ir, 5) : fpcr_rmode(ir);
    if (from == 32 && to == 64) {
        Split s;
        V wide = as_bits(ir, widen_f(ir, as_float(ir, a, 32)), 64);
        split_begin(ir, NULL, both(ir, no_nan(ir, 32, a), unflushed(ir, 32, a)), wide, NULL, &s);
        return split_end(ir, &s, map(ir, &o, a, NULL, NULL, to));
    }
    if (from == 64 && to == 32 && !odd) {
        V fa = as_float(ir, a, 64), narrowed = LLVMBuildFPTrunc(B, fa, fvec(ir, 32, lanes), "");
        Split s;
        V bits = as_bits(ir, narrowed, 32);
        V ok = either(ir, roomy(ir, 32, bits), is_zero(ir, 64, a));
        ok = both(ir, fpcr_clear(ir, FPCR_RMODE), ir_vec_all(ir, both(ir, not1(ir, is_nan(ir, 64, a)), ok)));
        split_begin(ir, NULL, ok, bits, ir_vec_any(ir, float_ne(ir, widen_f(ir, narrowed), fa)), &s);
        return split_end(ir, &s, map(ir, &o, a, NULL, NULL, to));
    }
    return map(ir, &o, a, NULL, NULL, to);
}

/* ---- the instruction forms ---- */

static V sign_flip(SnxIr* ir, unsigned width, V v) {
    return LLVMBuildXor(B, v, ir_vec_like(ir, v, sign_mask(width)), "");
}

/* the three-same arithmetic: SNX_FP_* (or one of the forms below) on n and m, d the accumulator */
enum { F_MAX = 16, F_MIN, F_MAXNM, F_MINNM, F_MLA, F_MLS, F_CMEQ, F_CMGE, F_CMGT, F_ACGE, F_ACGT };

static V arith(SnxIr* ir, unsigned op, unsigned width, V n, V m, V d) {
    switch (op) {
        case F_MAX: return minmax(ir, width, 0, 0, n, m);
        case F_MIN: return minmax(ir, width, 1, 0, n, m);
        case F_MAXNM: return minmax(ir, width, 0, 1, n, m);
        case F_MINNM: return minmax(ir, width, 1, 1, n, m);
        case F_MLA: return fma_lanes(ir, width, d, n, m);
        case F_MLS: return fma_lanes(ir, width, d, sign_flip(ir, width, n), m);
        case F_CMEQ: return compare(ir, CMP_EQ, width, n, m, 0);
        case F_CMGE: return compare(ir, CMP_GE, width, n, m, 0);
        case F_CMGT: return compare(ir, CMP_GT, width, n, m, 0);
        case F_ACGE: return compare(ir, CMP_GE, width, n, m, 1);
        case F_ACGT: return compare(ir, CMP_GT, width, n, m, 1);
        default: return binary(ir, op, width, n, m);
    }
}

/* the vector (lanes) or scalar (lanes 1) three-same form, or by element (m_lane >= 0) */
static void three(SnxIr* ir, const SnxA64Args* a, unsigned op, unsigned lanes, int m_lane) {
    unsigned width = 8u << a->esz;
    V n = reg(ir, (unsigned)a->rn, width, lanes), m, d = NULL;
    if (m_lane >= 0) {
        V e = LLVMBuildExtractElement(B, ir_vec_read(ir, (unsigned)a->rm, (unsigned)a->esz, (unsigned)m_lane, 1),
                                      ir_i32(ir, 0), "");
        m = ir_vec_splat(ir, e, lanes);
    } else
        m = reg(ir, (unsigned)a->rm, width, lanes);
    if (op == F_MLA || op == F_MLS) d = reg(ir, (unsigned)a->rd, width, lanes);
    ir_vec_write(ir, (unsigned)a->rd, arith(ir, op, width, n, m, d));
}

/* FADDP, FMAXP and the rest: the operation on the even and odd elements of Vn:Vm (vector), or on Vn's two (scalar) */
static void pairwise(SnxIr* ir, const SnxA64Args* a, unsigned op, int scalar) {
    unsigned width = 8u << a->esz, lanes = scalar ? 2 : ir_vec_lanes(a->q, (unsigned)a->esz), mask[2][16];
    V n = reg(ir, (unsigned)a->rn, width, lanes), m = scalar ? n : reg(ir, (unsigned)a->rm, width, lanes);
    unsigned out = scalar ? 1 : lanes;
    for (unsigned i = 0; i < out; ++i) {
        mask[0][i] = 2 * i;
        mask[1][i] = 2 * i + 1;
    }
    V even = ir_vec_shuffle(ir, n, m, mask[0], out);
    V odd = ir_vec_shuffle(ir, n, m, mask[1], out);
    ir_vec_write(ir, (unsigned)a->rd, arith(ir, op, width, even, odd, NULL));
}

/* FMAXV, FMINV, FMAXNMV, FMINNMV: the Arm ARM's Reduce, halves combined recursively */
static void reduce(SnxIr* ir, const SnxA64Args* a, unsigned minimum, unsigned numeric) {
    unsigned width = 8u << a->esz, lanes = ir_vec_lanes(a->q, (unsigned)a->esz), mask[2][8];
    V v = reg(ir, (unsigned)a->rn, width, lanes);
    while (lanes > 1) {
        lanes /= 2;
        for (unsigned i = 0; i < lanes; ++i) {
            mask[0][i] = 2 * i;
            mask[1][i] = 2 * i + 1;
        }
        /* pairs of neighbours at each level are the halves' recursion, level by level */
        v = minmax(ir, width, minimum, numeric, ir_vec_shuffle(ir, v, v, mask[0], lanes),
                   ir_vec_shuffle(ir, v, v, mask[1], lanes));
    }
    ir_vec_write(ir, (unsigned)a->rd, v);
}

/* FCMxx (zero): n against +0, LE and LT as 0 >= n and 0 > n */
static void compare_zero(SnxIr* ir, const SnxA64Args* a, unsigned cmp, int swap, unsigned lanes) {
    unsigned width = 8u << a->esz;
    V n = reg(ir, (unsigned)a->rn, width, lanes), zero = ir_vec_like(ir, n, 0);
    ir_vec_write(ir, (unsigned)a->rd, swap ? compare(ir, cmp, width, zero, n, 0) : compare(ir, cmp, width, n, zero, 0));
}

static void unary_form(SnxIr* ir, const SnxA64Args* a, unsigned op, unsigned lanes) {
    unsigned width = 8u << a->esz;
    ir_vec_write(ir, (unsigned)a->rd, unary(ir, op, width, reg(ir, (unsigned)a->rn, width, lanes)));
}

/* FABS, FNEG: bit operations */
static void abs_neg(SnxIr* ir, const SnxA64Args* a, int neg) {
    unsigned width = 8u << a->esz;
    V n = reg(ir, (unsigned)a->rn, width, ir_vec_lanes(a->q, (unsigned)a->esz));
    ir_vec_write(ir, (unsigned)a->rd, neg ? sign_flip(ir, width, n) : magnitude(ir, width, n));
}

static void round_form(SnxIr* ir, const SnxA64Args* a, unsigned rm, unsigned exact) {
    unsigned width = 8u << a->esz;
    V n = reg(ir, (unsigned)a->rn, width, ir_vec_lanes(a->q, (unsigned)a->esz));
    ir_vec_write(ir, (unsigned)a->rd, round_lanes(ir, width, rm, exact, n));
}

/* FRINT32Z, FRINT32X, FRINT64Z, FRINT64X */
static void round_int_form(SnxIr* ir, const SnxA64Args* a, unsigned rm, unsigned intsize) {
    unsigned width = 8u << a->esz;
    LaneOp o = op_simple(H_ROUND_INT, width);
    o.p1 = intsize;
    o.rm = rm == RM_FPCR ? fpcr_rmode(ir) : ir_i32(ir, rm);
    ir_vec_write(ir, (unsigned)a->rd, map(ir, &o, reg(ir, (unsigned)a->rn, width, ir_vec_lanes(a->q, (unsigned)a->esz)),
                                          NULL, NULL, width));
}

/* [SU]CVTF of Vn's lanes (vector), element 0 (scalar) or, FEAT_FPRCVT's, element 0 of int_esz */
static void cvtf(SnxIr* ir, const SnxA64Args* a, unsigned uns, unsigned lanes, unsigned fbits) {
    unsigned width = 8u << a->esz;
    ir_vec_write(ir, (unsigned)a->rd, from_int(ir, width, uns, fbits, reg(ir, (unsigned)a->rn, width, lanes)));
}
static void fcvt_int(SnxIr* ir, const SnxA64Args* a, unsigned rm, unsigned uns, unsigned lanes, unsigned fbits) {
    unsigned width = 8u << a->esz;
    ir_vec_write(ir, (unsigned)a->rd, to_int(ir, width, width, uns, fbits, rm, reg(ir, (unsigned)a->rn, width, lanes)));
}
/* FCVTL: the q half of Vn widened (esz the destination's) */
static void fcvtl(SnxIr* ir, const SnxA64Args* a) {
    unsigned to = 8u << a->esz, from = to / 2, lanes = 64 / from;
    V n = ir_vec_read(ir, (unsigned)a->rn, esz_of(from), a->q ? lanes : 0, lanes);
    ir_vec_write(ir, (unsigned)a->rd, convert(ir, from, to, 0, n));
}
/* FCVTN, FCVTXN, BFCVTN: all of Vn narrowed into the low or (q) high half of Vd */
static void fcvtn(SnxIr* ir, const SnxA64Args* a, int odd, int bf16) {
    unsigned to = 8u << a->esz, from = bf16 ? 32 : to * 2, lanes = 64 / to;
    V n = reg(ir, (unsigned)a->rn, from, lanes), r;
    if (bf16) {
        LaneOp o = op_simple(H_BFCVT, 32);
        r = map(ir, &o, n, NULL, NULL, 16);
    } else
        r = convert(ir, from, to, odd, n);
    if (a->q)
        ir_vec_write_high(ir, (unsigned)a->rd, r);
    else
        ir_vec_write(ir, (unsigned)a->rd, r);
}

/* URECPE, URSQRTE: cpu.h's estimates of 32-bit fixed-point lanes */
static void unsigned_estimate(SnxIr* ir, const SnxA64Args* a, int rsqrt) {
    unsigned lanes = ir_vec_lanes(a->q, 2);
    LLVMTypeRef param = ir->t_i32;
    V fn = ir_extern(ir, rsqrt ? "ursqrte32" : "urecpe32", ir->t_i32, &param, 1);
    V n = reg(ir, (unsigned)a->rn, 32, lanes);
    V r = LLVMGetUndef(LLVMTypeOf(n));
    ir_pure(ir, fn);
    for (unsigned i = 0; i < lanes; ++i) {
        V x = LLVMBuildExtractElement(B, n, ir_i32(ir, i), "");
        x = LLVMBuildCall2(B, LLVMFunctionType(ir->t_i32, &param, 1, 0), fn, &x, 1, "");
        r = LLVMBuildInsertElement(B, r, x, ir_i32(ir, i), "");
    }
    ir_vec_write(ir, (unsigned)a->rd, r);
}

/* FMLAL, FMLSL (2: the upper source halves) and the by-element forms: halves widened exactly by their bits
   (float16_to_float32_by_bits: FPCR.FZ16 flushes their denormals, NaNs keep their payload), FMLSL's first
   operand negated, then a fused single multiply-add */
static V half_to_single(SnxIr* ir, V h) {
    V x = LLVMBuildZExt(B, h, ir_vec_type(ir, 2, ir_vec_count(h)), "");
    V fz16 = LLVMBuildNot(B, fpcr_clear(ir, FPCR_FZ16), ""), args[2];
    V sign = LLVMBuildShl(B, LLVMBuildLShr(B, x, ir_vec_like(ir, x, 15), ""), ir_vec_like(ir, x, 31), "");
    V exp = LLVMBuildAnd(B, LLVMBuildLShr(B, x, ir_vec_like(ir, x, 10), ""), ir_vec_like(ir, x, 0x1F), "");
    V frac = LLVMBuildAnd(B, x, ir_vec_like(ir, x, 0x3FF), "");
    /* normal: rebias; infinity and NaN: all ones */
    V r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, exp, ir_vec_like(ir, x, 0x1F), ""), ir_vec_like(ir, x, 0xFF),
                          LLVMBuildAdd(B, exp, ir_vec_like(ir, x, 127 - 15), ""), "");
    r = LLVMBuildOr(B, LLVMBuildShl(B, r, ir_vec_like(ir, x, 23), ""),
                    LLVMBuildShl(B, frac, ir_vec_like(ir, x, 13), ""), "");
    /* a denormal: normalized, its msb moved to bit 10 and dropped (exponent 127 - 15 - shift + 1) */
    args[0] = frac;
    args[1] = LLVMConstInt(ir->t_i1, 0, 0);
    V shift = LLVMBuildSub(B, ir_intrinsic(ir, "llvm.ctlz", LLVMTypeOf(frac), args, 2), ir_vec_like(ir, x, 21), "");
    V normalized = LLVMBuildAnd(B, LLVMBuildShl(B, frac, shift, ""), ir_vec_like(ir, x, 0x3FF), "");
    normalized = LLVMBuildOr(B,
                             LLVMBuildShl(B, LLVMBuildSub(B, ir_vec_like(ir, x, 127 - 15 + 1), shift, ""),
                                          ir_vec_like(ir, x, 23), ""),
                             LLVMBuildShl(B, normalized, ir_vec_like(ir, x, 13), ""), "");
    V denormal = LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntEQ, exp, ir_vec_like(ir, x, 0), ""),
                              LLVMBuildICmp(B, LLVMIntNE, frac, ir_vec_like(ir, x, 0), ""), "");
    normalized = LLVMBuildSelect(B, fz16, ir_vec_like(ir, x, 0), normalized, "");
    r = LLVMBuildSelect(B, denormal, normalized, r, "");
    r = LLVMBuildSelect(B, LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntEQ, exp, ir_vec_like(ir, x, 0), ""),
                                        LLVMBuildICmp(B, LLVMIntEQ, frac, ir_vec_like(ir, x, 0), ""), ""),
                        ir_vec_like(ir, x, 0), r, "");
    return LLVMBuildOr(B, sign, r, "");
}
static void fmlal(SnxIr* ir, const SnxA64Args* a, int sub, int upper, int m_lane) {
    unsigned lanes = a->q ? 4 : 2;
    V n = ir_vec_read(ir, (unsigned)a->rn, 1, upper ? lanes : 0, lanes), m, d = reg(ir, (unsigned)a->rd, 32, lanes);
    if (m_lane >= 0)
        m = ir_vec_splat(ir, LLVMBuildExtractElement(B, ir_vec_read(ir, (unsigned)a->rm, 1, (unsigned)m_lane, 1),
                                                     ir_i32(ir, 0), ""),
                         lanes);
    else
        m = ir_vec_read(ir, (unsigned)a->rm, 1, upper ? lanes : 0, lanes);
    if (sub) n = sign_flip(ir, 16, n);
    ir_vec_write(ir, (unsigned)a->rd, fma_lanes(ir, 32, d, half_to_single(ir, n), half_to_single(ir, m)));
}

/* BFMLALB, BFMLALT (q: the odd bfloat16 elements) and by element: bfloat16s as singles, a single fused
   multiply-add under FPCR */
static void bfmlal(SnxIr* ir, const SnxA64Args* a, int m_lane) {
    unsigned mask[4];
    V n = reg(ir, (unsigned)a->rn, 16, 8), m = reg(ir, (unsigned)a->rm, 16, 8), d = reg(ir, (unsigned)a->rd, 32, 4);
    for (unsigned i = 0; i < 4; ++i) mask[i] = 2 * i + (unsigned)a->q;
    n = ir_vec_shuffle(ir, n, n, mask, 4);
    if (m_lane >= 0)
        for (unsigned i = 0; i < 4; ++i) mask[i] = (unsigned)m_lane;
    m = ir_vec_shuffle(ir, m, m, mask, 4);
    n = LLVMBuildShl(B, LLVMBuildZExt(B, n, LLVMTypeOf(d), ""), ir_vec_like(ir, d, 16), "");
    m = LLVMBuildShl(B, LLVMBuildZExt(B, m, LLVMTypeOf(d), ""), ir_vec_like(ir, d, 16), "");
    ir_vec_write(ir, (unsigned)a->rd, fma_lanes(ir, 32, d, n, m));
}

/* BFDOT, BFMMLA: helpers/vector.c's step, one lane (or matrix element) after another */
static V bfdot_step(SnxIr* ir, V sum, V e1, V e2) {
    V ops[3];
    ops[0] = sum;
    ops[1] = e1;
    ops[2] = e2;
    return ir_vec_call(ir, "snx_vec_bfdot", ir->t_i32, ops, 3);
}
/* BFDOT (vector, or by element: the pair m_lane of each 128-bit segment of Vm) */
static void bfdot(SnxIr* ir, const SnxA64Args* a, int m_lane) {
    unsigned lanes = ir_vec_lanes(a->q, 2);
    V n = reg(ir, (unsigned)a->rn, 32, lanes), m = reg(ir, (unsigned)a->rm, 32, 4);
    V d = reg(ir, (unsigned)a->rd, 32, lanes);
    V r = LLVMGetUndef(LLVMTypeOf(d));
    for (unsigned i = 0; i < lanes; ++i) {
        V at = ir_i32(ir, i), pick = ir_i32(ir, m_lane >= 0 ? (unsigned)m_lane : i);
        V x = bfdot_step(ir, LLVMBuildExtractElement(B, d, at, ""), LLVMBuildExtractElement(B, n, at, ""),
                         LLVMBuildExtractElement(B, m, pick, ""));
        r = LLVMBuildInsertElement(B, r, x, at, "");
    }
    ir_vec_write(ir, (unsigned)a->rd, r);
}
/* BFMMLA: Vd's 2x2 singles += Vn's 2x4 bfloat16s times the transpose of Vm's, two steps of pairs per element */
static void bfmmla(SnxIr* ir, const SnxA64Args* a) {
    V n = reg(ir, (unsigned)a->rn, 32, 4), m = reg(ir, (unsigned)a->rm, 32, 4), d = reg(ir, (unsigned)a->rd, 32, 4);
    V r = LLVMGetUndef(LLVMTypeOf(d));
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            V sum = LLVMBuildExtractElement(B, d, ir_i32(ir, 2 * i + j), "");
            for (unsigned k = 0; k < 2; ++k)
                sum = bfdot_step(ir, sum, LLVMBuildExtractElement(B, n, ir_i32(ir, 2 * i + k), ""),
                                 LLVMBuildExtractElement(B, m, ir_i32(ir, 2 * j + k), ""));
            r = LLVMBuildInsertElement(B, r, sum, ir_i32(ir, 2 * i + j), "");
        }
    ir_vec_write(ir, (unsigned)a->rd, r);
}

/* FCADD: n plus m with each pair's elements swapped and one negated (rot 0: the real part's, for 90 degrees; 1: the
   imaginary part's, for 270) */
static void fcadd(SnxIr* ir, const SnxA64Args* a, int rot) {
    unsigned width = 8u << a->esz, lanes = ir_vec_lanes(a->q, (unsigned)a->esz), mask[16];
    V n = reg(ir, (unsigned)a->rn, width, lanes), m = reg(ir, (unsigned)a->rm, width, lanes);
    V signs[16];
    for (unsigned i = 0; i < lanes; ++i) {
        mask[i] = i ^ 1;
        SnxU64 sign = ((i & 1) == (unsigned)rot) ? sign_mask(width) : 0;
        signs[i] = LLVMConstInt(ir_vec_lane(ir, (unsigned)a->esz), sign, 0);
    }
    V negate = LLVMConstVector(signs, lanes);
    m = LLVMBuildXor(B, ir_vec_shuffle(ir, m, m, mask, lanes), negate, "");
    ir_vec_write(ir, (unsigned)a->rd, binary(ir, SNX_FP_ADD, width, n, m));
}

/* FCMLA (gvec_fcmla*): d[2k] += n[2k + flip] * (m[2k + flip] negated per rot), d[2k+1] += n[2k + flip] * m[2k + 1 -
   flip] (negated per rot); by element, the pair m_pair of each 128-bit segment of Vm */
static void fcmla(SnxIr* ir, const SnxA64Args* a, int m_pair) {
    unsigned width = 8u << a->esz, lanes = ir_vec_lanes(a->q, (unsigned)a->esz), rot = (unsigned)a->rot;
    unsigned flip = rot & 1, neg_imag = (rot >> 1) & 1, neg_real = flip ^ neg_imag, nmask[16], mmask[16];
    unsigned segment = lanes < 128 / width ? lanes : 128 / width;
    V n = reg(ir, (unsigned)a->rn, width, lanes), d = reg(ir, (unsigned)a->rd, width, lanes), signs[16];
    V m = reg(ir, (unsigned)a->rm, width, m_pair >= 0 ? 128 / width : lanes);
    for (unsigned i = 0; i < lanes; ++i) {
        unsigned pair = i & ~1u, base = m_pair >= 0 ? (i / segment) * segment + 2 * (unsigned)m_pair : pair;
        nmask[i] = pair + flip;
        mmask[i] = (i & 1) ? base + 1 - flip : base + flip;
        SnxU64 sign = ((i & 1) ? neg_imag : neg_real) ? sign_mask(width) : 0;
        signs[i] = LLVMConstInt(ir_vec_lane(ir, (unsigned)a->esz), sign, 0);
    }
    n = ir_vec_shuffle(ir, n, n, nmask, lanes);
    m = LLVMBuildXor(B, ir_vec_shuffle(ir, m, m, mmask, lanes), LLVMConstVector(signs, lanes), "");
    ir_vec_write(ir, (unsigned)a->rd, fma_lanes(ir, width, d, n, m));
}

/* ---- the entry points ---- */

/* three same (vector, scalar, by element): the arithmetic's op, or -1; *form 0 vector, 1 scalar, 2 vector by element,
   3 scalar by element, 4 vector pairwise, 5 scalar pairwise */
static int three_of(SnxA64Op op, int* form) {
    *form = 0;
    switch (op) {
        case SNX_A64_OP_FADD_v: return SNX_FP_ADD;
        case SNX_A64_OP_FSUB_v: return SNX_FP_SUB;
        case SNX_A64_OP_FDIV_v: return SNX_FP_DIV;
        case SNX_A64_OP_FMUL_v: return SNX_FP_MUL;
        case SNX_A64_OP_FMULX_v: return SNX_FP_MULX;
        case SNX_A64_OP_FABD_v: return SNX_FP_ABD;
        case SNX_A64_OP_FRECPS_v: return SNX_FP_RECPS;
        case SNX_A64_OP_FRSQRTS_v: return SNX_FP_RSQRTS;
        case SNX_A64_OP_FMAX_v: return F_MAX;
        case SNX_A64_OP_FMIN_v: return F_MIN;
        case SNX_A64_OP_FMAXNM_v: return F_MAXNM;
        case SNX_A64_OP_FMINNM_v: return F_MINNM;
        case SNX_A64_OP_FMLA_v: return F_MLA;
        case SNX_A64_OP_FMLS_v: return F_MLS;
        case SNX_A64_OP_FCMEQ_v: return F_CMEQ;
        case SNX_A64_OP_FCMGE_v: return F_CMGE;
        case SNX_A64_OP_FCMGT_v: return F_CMGT;
        case SNX_A64_OP_FACGE_v: return F_ACGE;
        case SNX_A64_OP_FACGT_v: return F_ACGT;
        default: break;
    }
    *form = 1;
    switch (op) {
        case SNX_A64_OP_FMULX_s: return SNX_FP_MULX;
        case SNX_A64_OP_FABD_s: return SNX_FP_ABD;
        case SNX_A64_OP_FRECPS_s: return SNX_FP_RECPS;
        case SNX_A64_OP_FRSQRTS_s: return SNX_FP_RSQRTS;
        case SNX_A64_OP_FCMEQ_s: return F_CMEQ;
        case SNX_A64_OP_FCMGE_s: return F_CMGE;
        case SNX_A64_OP_FCMGT_s: return F_CMGT;
        case SNX_A64_OP_FACGE_s: return F_ACGE;
        case SNX_A64_OP_FACGT_s: return F_ACGT;
        default: break;
    }
    *form = 2;
    switch (op) {
        case SNX_A64_OP_FMUL_vi: return SNX_FP_MUL;
        case SNX_A64_OP_FMULX_vi: return SNX_FP_MULX;
        case SNX_A64_OP_FMLA_vi: return F_MLA;
        case SNX_A64_OP_FMLS_vi: return F_MLS;
        default: break;
    }
    *form = 3;
    switch (op) {
        case SNX_A64_OP_FMUL_si: return SNX_FP_MUL;
        case SNX_A64_OP_FMULX_si: return SNX_FP_MULX;
        case SNX_A64_OP_FMLA_si: return F_MLA;
        case SNX_A64_OP_FMLS_si: return F_MLS;
        default: break;
    }
    *form = 4;
    switch (op) {
        case SNX_A64_OP_FADDP_v: return SNX_FP_ADD;
        case SNX_A64_OP_FMAXP_v: return F_MAX;
        case SNX_A64_OP_FMINP_v: return F_MIN;
        case SNX_A64_OP_FMAXNMP_v: return F_MAXNM;
        case SNX_A64_OP_FMINNMP_v: return F_MINNM;
        default: break;
    }
    *form = 5;
    switch (op) {
        case SNX_A64_OP_FADDP_s: return SNX_FP_ADD;
        case SNX_A64_OP_FMAXP_s: return F_MAX;
        case SNX_A64_OP_FMINP_s: return F_MIN;
        case SNX_A64_OP_FMAXNMP_s: return F_MAXNM;
        case SNX_A64_OP_FMINNMP_s: return F_MINNM;
        default: return -1;
    }
}

/* the rounding modes of the FCVT[NPMZA][SU] names: N 0, P 1, M 2, Z 3, A 4, with uns; -1 for another op. form: 0
   vector, 1 scalar (and the fixed-point forms) */
static int fcvt_of(SnxA64Op op, unsigned* uns, int* form) {
    static const struct {
        SnxA64Op op;
        unsigned char rm, uns, form;
    } table[] = {
        {SNX_A64_OP_FCVTNS_vi, 0, 0, 0},     {SNX_A64_OP_FCVTNU_vi, 0, 1, 0},     {SNX_A64_OP_FCVTPS_vi, 1, 0, 0},
        {SNX_A64_OP_FCVTPU_vi, 1, 1, 0},     {SNX_A64_OP_FCVTMS_vi, 2, 0, 0},     {SNX_A64_OP_FCVTMU_vi, 2, 1, 0},
        {SNX_A64_OP_FCVTZS_vi, 3, 0, 0},     {SNX_A64_OP_FCVTZU_vi, 3, 1, 0},     {SNX_A64_OP_FCVTAS_vi, 4, 0, 0},
        {SNX_A64_OP_FCVTAU_vi, 4, 1, 0},     {SNX_A64_OP_FCVTZS_vf, 3, 0, 0},     {SNX_A64_OP_FCVTZU_vf, 3, 1, 0},
        {SNX_A64_OP_FCVTNS_f, 0, 0, 1},      {SNX_A64_OP_FCVTNU_f, 0, 1, 1},      {SNX_A64_OP_FCVTPS_f, 1, 0, 1},
        {SNX_A64_OP_FCVTPU_f, 1, 1, 1},      {SNX_A64_OP_FCVTMS_f, 2, 0, 1},      {SNX_A64_OP_FCVTMU_f, 2, 1, 1},
        {SNX_A64_OP_FCVTZS_f, 3, 0, 1},      {SNX_A64_OP_FCVTZU_f, 3, 1, 1},      {SNX_A64_OP_FCVTAS_f, 4, 0, 1},
        {SNX_A64_OP_FCVTAU_f, 4, 1, 1},
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
        if (table[i].op == op) {
            *uns = table[i].uns;
            *form = table[i].form;
            return table[i].rm;
        }
    return -1;
}

/* FRINT* (vector): the mode (RM_FPCR for X and I) and exact, or -1 */
static int frint_of(SnxA64Op op, unsigned* exact) {
    *exact = 0;
    switch (op) {
        case SNX_A64_OP_FRINTN_v: return 0;
        case SNX_A64_OP_FRINTP_v: return 1;
        case SNX_A64_OP_FRINTM_v: return 2;
        case SNX_A64_OP_FRINTZ_v: return 3;
        case SNX_A64_OP_FRINTA_v: return 4;
        case SNX_A64_OP_FRINTX_v: *exact = 1; return RM_FPCR;
        case SNX_A64_OP_FRINTI_v: return RM_FPCR;
        default: return -1;
    }
}

static int other(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_FMAXNMV_h:
        case SNX_A64_OP_FMAXNMV_s:
        case SNX_A64_OP_FMINNMV_h:
        case SNX_A64_OP_FMINNMV_s:
        case SNX_A64_OP_FMAXV_h:
        case SNX_A64_OP_FMAXV_s:
        case SNX_A64_OP_FMINV_h:
        case SNX_A64_OP_FMINV_s:
        case SNX_A64_OP_FCMGT0_s:
        case SNX_A64_OP_FCMGE0_s:
        case SNX_A64_OP_FCMEQ0_s:
        case SNX_A64_OP_FCMLE0_s:
        case SNX_A64_OP_FCMLT0_s:
        case SNX_A64_OP_FCMGT0_v:
        case SNX_A64_OP_FCMGE0_v:
        case SNX_A64_OP_FCMEQ0_v:
        case SNX_A64_OP_FCMLE0_v:
        case SNX_A64_OP_FCMLT0_v:
        case SNX_A64_OP_FRECPE_s:
        case SNX_A64_OP_FRECPX_s:
        case SNX_A64_OP_FRSQRTE_s:
        case SNX_A64_OP_FRECPE_v:
        case SNX_A64_OP_FRSQRTE_v:
        case SNX_A64_OP_FSQRT_v:
        case SNX_A64_OP_FABS_v:
        case SNX_A64_OP_FNEG_v:
        case SNX_A64_OP_FRINT32Z_v:
        case SNX_A64_OP_FRINT32X_v:
        case SNX_A64_OP_FRINT64Z_v:
        case SNX_A64_OP_FRINT64X_v:
        case SNX_A64_OP_SCVTF_vi:
        case SNX_A64_OP_UCVTF_vi:
        case SNX_A64_OP_SCVTF_vf:
        case SNX_A64_OP_UCVTF_vf:
        case SNX_A64_OP_SCVTF_f:
        case SNX_A64_OP_UCVTF_f:
        case SNX_A64_OP_URECPE_v:
        case SNX_A64_OP_URSQRTE_v:
        case SNX_A64_OP_FCVTL_v:
        case SNX_A64_OP_FCVTN_v:
        case SNX_A64_OP_FCVTXN_v:
        case SNX_A64_OP_FCVTXN_s:
        case SNX_A64_OP_BFCVTN_v:
        case SNX_A64_OP_FMLAL_v:
        case SNX_A64_OP_FMLSL_v:
        case SNX_A64_OP_FMLAL2_v:
        case SNX_A64_OP_FMLSL2_v:
        case SNX_A64_OP_FMLAL_vi:
        case SNX_A64_OP_FMLSL_vi:
        case SNX_A64_OP_FMLAL2_vi:
        case SNX_A64_OP_FMLSL2_vi:
        case SNX_A64_OP_BFMLAL_v:
        case SNX_A64_OP_BFMLAL_vi:
        case SNX_A64_OP_BFDOT_v:
        case SNX_A64_OP_BFDOT_vi:
        case SNX_A64_OP_BFMMLA:
        case SNX_A64_OP_FCADD_90:
        case SNX_A64_OP_FCADD_270:
        case SNX_A64_OP_FCMLA_v:
        case SNX_A64_OP_FCMLA_vi: return 1;
        default: return 0;
    }
}

int ir_vec_fp_handles(SnxA64Op op) {
    int form;
    unsigned u;
    return three_of(op, &form) >= 0 || fcvt_of(op, &u, &form) >= 0 || frint_of(op, &u) >= 0 || other(op);
}

void ir_vec_fp(SnxIr* ir, const SnxA64Insn* insn) {
    const SnxA64Args* a = &insn->args;
    SnxA64Op op = insn->op;
    unsigned esz = (unsigned)a->esz, lanes = ir_vec_lanes(a->q, esz), uns, exact;
    int t, form;
    if ((t = three_of(op, &form)) >= 0) {
        switch (form) {
            case 0: three(ir, a, (unsigned)t, lanes, -1); break;
            case 1: three(ir, a, (unsigned)t, 1, -1); break;
            case 2: three(ir, a, (unsigned)t, lanes, a->idx); break;
            case 3: three(ir, a, (unsigned)t, 1, a->idx); break;
            case 4: pairwise(ir, a, (unsigned)t, 0); break;
            default: pairwise(ir, a, (unsigned)t, 1); break;
        }
        return;
    }
    if ((t = fcvt_of(op, &uns, &form)) >= 0) {
        fcvt_int(ir, a, (unsigned)t, uns, form ? 1 : lanes, (unsigned)a->shift);
        return;
    }
    if ((t = frint_of(op, &exact)) >= 0) {
        round_form(ir, a, (unsigned)t, exact);
        return;
    }
    switch (op) {
        case SNX_A64_OP_FMAXNMV_h:
        case SNX_A64_OP_FMAXNMV_s: reduce(ir, a, 0, 1); break;
        case SNX_A64_OP_FMINNMV_h:
        case SNX_A64_OP_FMINNMV_s: reduce(ir, a, 1, 1); break;
        case SNX_A64_OP_FMAXV_h:
        case SNX_A64_OP_FMAXV_s: reduce(ir, a, 0, 0); break;
        case SNX_A64_OP_FMINV_h:
        case SNX_A64_OP_FMINV_s: reduce(ir, a, 1, 0); break;
        case SNX_A64_OP_FCMGT0_s: compare_zero(ir, a, CMP_GT, 0, 1); break;
        case SNX_A64_OP_FCMGE0_s: compare_zero(ir, a, CMP_GE, 0, 1); break;
        case SNX_A64_OP_FCMEQ0_s: compare_zero(ir, a, CMP_EQ, 0, 1); break;
        case SNX_A64_OP_FCMLE0_s: compare_zero(ir, a, CMP_GE, 1, 1); break;
        case SNX_A64_OP_FCMLT0_s: compare_zero(ir, a, CMP_GT, 1, 1); break;
        case SNX_A64_OP_FCMGT0_v: compare_zero(ir, a, CMP_GT, 0, lanes); break;
        case SNX_A64_OP_FCMGE0_v: compare_zero(ir, a, CMP_GE, 0, lanes); break;
        case SNX_A64_OP_FCMEQ0_v: compare_zero(ir, a, CMP_EQ, 0, lanes); break;
        case SNX_A64_OP_FCMLE0_v: compare_zero(ir, a, CMP_GE, 1, lanes); break;
        case SNX_A64_OP_FCMLT0_v: compare_zero(ir, a, CMP_GT, 1, lanes); break;
        case SNX_A64_OP_FRECPE_s: unary_form(ir, a, SNX_FP_RECPE, 1); break;
        case SNX_A64_OP_FRECPX_s: unary_form(ir, a, SNX_FP_RECPX, 1); break;
        case SNX_A64_OP_FRSQRTE_s: unary_form(ir, a, SNX_FP_RSQRTE, 1); break;
        case SNX_A64_OP_FRECPE_v: unary_form(ir, a, SNX_FP_RECPE, lanes); break;
        case SNX_A64_OP_FRSQRTE_v: unary_form(ir, a, SNX_FP_RSQRTE, lanes); break;
        case SNX_A64_OP_FSQRT_v: unary_form(ir, a, SNX_FP_SQRT, lanes); break;
        case SNX_A64_OP_FABS_v: abs_neg(ir, a, 0); break;
        case SNX_A64_OP_FNEG_v: abs_neg(ir, a, 1); break;
        case SNX_A64_OP_FRINT32Z_v: round_int_form(ir, a, 3, 32); break;
        case SNX_A64_OP_FRINT32X_v: round_int_form(ir, a, RM_FPCR, 32); break;
        case SNX_A64_OP_FRINT64Z_v: round_int_form(ir, a, 3, 64); break;
        case SNX_A64_OP_FRINT64X_v: round_int_form(ir, a, RM_FPCR, 64); break;
        case SNX_A64_OP_SCVTF_vi: cvtf(ir, a, 0, lanes, 0); break;
        case SNX_A64_OP_UCVTF_vi: cvtf(ir, a, 1, lanes, 0); break;
        case SNX_A64_OP_SCVTF_vf: cvtf(ir, a, 0, lanes, (unsigned)a->shift); break;
        case SNX_A64_OP_UCVTF_vf: cvtf(ir, a, 1, lanes, (unsigned)a->shift); break;
        case SNX_A64_OP_SCVTF_f: cvtf(ir, a, 0, 1, (unsigned)a->shift); break;
        case SNX_A64_OP_UCVTF_f: cvtf(ir, a, 1, 1, (unsigned)a->shift); break;
        case SNX_A64_OP_URECPE_v: unsigned_estimate(ir, a, 0); break;
        case SNX_A64_OP_URSQRTE_v: unsigned_estimate(ir, a, 1); break;
        case SNX_A64_OP_FCVTL_v: fcvtl(ir, a); break;
        case SNX_A64_OP_FCVTN_v: fcvtn(ir, a, 0, 0); break;
        case SNX_A64_OP_FCVTXN_v: fcvtn(ir, a, 1, 0); break;
        case SNX_A64_OP_BFCVTN_v: fcvtn(ir, a, 0, 1); break;
        case SNX_A64_OP_FCVTXN_s:
            ir_vec_write(ir, (unsigned)a->rd, convert(ir, 64, 32, 1, reg(ir, (unsigned)a->rn, 64, 1)));
            break;
        case SNX_A64_OP_FMLAL_v: fmlal(ir, a, 0, 0, -1); break;
        case SNX_A64_OP_FMLSL_v: fmlal(ir, a, 1, 0, -1); break;
        case SNX_A64_OP_FMLAL2_v: fmlal(ir, a, 0, 1, -1); break;
        case SNX_A64_OP_FMLSL2_v: fmlal(ir, a, 1, 1, -1); break;
        case SNX_A64_OP_FMLAL_vi: fmlal(ir, a, 0, 0, a->idx); break;
        case SNX_A64_OP_FMLSL_vi: fmlal(ir, a, 1, 0, a->idx); break;
        case SNX_A64_OP_FMLAL2_vi: fmlal(ir, a, 0, 1, a->idx); break;
        case SNX_A64_OP_FMLSL2_vi: fmlal(ir, a, 1, 1, a->idx); break;
        case SNX_A64_OP_BFMLAL_v: bfmlal(ir, a, -1); break;
        case SNX_A64_OP_BFMLAL_vi: bfmlal(ir, a, a->idx); break;
        case SNX_A64_OP_BFDOT_v: bfdot(ir, a, -1); break;
        case SNX_A64_OP_BFDOT_vi: bfdot(ir, a, a->idx); break;
        case SNX_A64_OP_BFMMLA: bfmmla(ir, a); break;
        case SNX_A64_OP_FCADD_90: fcadd(ir, a, 0); break;
        case SNX_A64_OP_FCADD_270: fcadd(ir, a, 1); break;
        case SNX_A64_OP_FCMLA_v: fcmla(ir, a, -1); break;
        case SNX_A64_OP_FCMLA_vi: fcmla(ir, a, a->idx); break;
        default: break;
    }
}
