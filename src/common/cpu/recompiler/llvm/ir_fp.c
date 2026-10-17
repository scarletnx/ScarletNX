/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate-a64.c, helper-a64.c, vec_helper.c, vfp_helper.c,
   crypto_helper.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de>
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org>
   Copyright (c) 2003 Fabrice Bellard */
/* Scalar floating point: the FP patterns of a64_templates.inc. Their C calls the integer-only FP of cpu_helpers.c
   (snx_fp_*), in the runtime library, and writes the result with SETSB/SETDB/SETH. */
#include "common/cpu/recompiler/llvm/ir.h"
#include <string.h>

typedef LLVMValueRef V;
#define B        ir->b
#define F(lo, n) ((w >> (lo)) & ((1UL << (n)) - 1))

static V i32c(SnxIr* ir, SnxU32 v) { return ir_i32(ir, v); }

/* ---- fast paths ---- */

typedef struct {
    LLVMTypeRef it, ft; /* lane types: iN and its float */
    unsigned lanes, mantissa, emax, precision;
} Shape;

static void shape(SnxIr* ir, unsigned width, unsigned lanes, Shape* s) {
    LLVMTypeRef i = width == 64 ? ir->t_i64 : ir->t_i32;
    LLVMTypeRef f = width == 64 ? LLVMDoubleTypeInContext(ir->ctx) : LLVMFloatTypeInContext(ir->ctx);
    s->lanes = lanes;
    s->it = lanes > 1 ? LLVMVectorType(i, lanes) : i;
    s->ft = lanes > 1 ? LLVMVectorType(f, lanes) : f;
    s->mantissa = width == 64 ? 52 : 23;
    s->emax = width == 64 ? 0x7FF : 0xFF;
    s->precision = s->mantissa + 1;
}
static V splat(const Shape* s, V scalar) {
    V values[4];
    unsigned i;
    if (s->lanes == 1) return scalar;
    for (i = 0; i < s->lanes; ++i) values[i] = scalar;
    return LLVMConstVector(values, s->lanes);
}
static V kint(SnxIr* ir, const Shape* s, SnxU64 v) {
    (void)ir;
    return splat(s, LLVMConstInt(s->lanes > 1 ? LLVMGetElementType(s->it) : s->it, v, 0));
}
static V kfloat(SnxIr* ir, const Shape* s, double v) {
    (void)ir;
    return splat(s, LLVMConstReal(s->lanes > 1 ? LLVMGetElementType(s->ft) : s->ft, v));
}
/* every lane / some lane of an i1 (vector) */
static V all(SnxIr* ir, const Shape* s, V c) {
    return s->lanes > 1 ? ir_intrinsic(ir, "llvm.vector.reduce.and", LLVMTypeOf(c), &c, 1) : c;
}
static V any(SnxIr* ir, const Shape* s, V c) {
    return s->lanes > 1 ? ir_intrinsic(ir, "llvm.vector.reduce.or", LLVMTypeOf(c), &c, 1) : c;
}
static V exponent(SnxIr* ir, const Shape* s, V bits) {
    return LLVMBuildAnd(B, LLVMBuildLShr(B, bits, kint(ir, s, s->mantissa), ""), kint(ir, s, s->emax), "");
}
/* lo <= exponent <= hi in each lane (i1, or a vector of them) */
static V lane_exponent_in(SnxIr* ir, const Shape* s, V bits, unsigned lo, unsigned hi) {
    return LLVMBuildICmp(B, LLVMIntULE, LLVMBuildSub(B, exponent(ir, s, bits), kint(ir, s, lo), ""),
                         kint(ir, s, hi - lo), "");
}
/* every lane */
static V exponent_in(SnxIr* ir, const Shape* s, V bits, unsigned lo, unsigned hi) {
    return all(ir, s, lane_exponent_in(ir, s, bits, lo, hi));
}
static V rounds_to_nearest(SnxIr* ir) {
    V fpcr = ir_cpu_load(ir, ir->t_i32, ir_off_fpcr());
    return LLVMBuildICmp(B, LLVMIntEQ, LLVMBuildAnd(B, fpcr, i32c(ir, 3u << 22), ""), i32c(ir, 0), "");
}
static V ixc_set(SnxIr* ir) {
    V fpsr = ir_cpu_load(ir, ir->t_i32, ir_off_fpcr() + 4);
    return LLVMBuildICmp(B, LLVMIntNE, LLVMBuildAnd(B, fpsr, i32c(ir, 1u << 4), ""), i32c(ir, 0), "");
}
/* the host fuses multiply-add in hardware (x86 FMA3 from CPUID; AArch64 always): llvm.fma is then one instruction,
   never a call into a C library the module doesn't link */
static int host_fma(SnxIr* ir) {
    if (ir->features->x86) return ir->features->fma;
    return !strncmp(LLVMGetTarget(ir->mod), "aarch64", 7);
}
static V fp_intrinsic(SnxIr* ir, const char* name, const Shape* s, V* args, unsigned count) {
    return ir_intrinsic(ir, name, s->ft, args, count);
}
static V nonzero(SnxIr* ir, const Shape* s, V f) {
    return any(ir, s, LLVMBuildFCmp(B, LLVMRealUNE, f, kfloat(ir, s, 0.0), ""));
}

/* +0 or -0 in each lane */
static V lane_zero(SnxIr* ir, const Shape* s, V bits) {
    SnxU64 magnitude = ((SnxU64)s->emax << s->mantissa) | (((SnxU64)1 << s->mantissa) - 1);
    return LLVMBuildICmp(B, LLVMIntEQ, LLVMBuildAnd(B, bits, kint(ir, s, magnitude), ""), kint(ir, s, 0), "");
}
static V lane_or(SnxIr* ir, V x, V y) { return LLVMBuildOr(B, x, y, ""); }
static V lane_and(SnxIr* ir, V x, V y) { return LLVMBuildAnd(B, x, y, ""); }

void ir_fp_fast(SnxIr* ir, unsigned op, unsigned width, unsigned lanes, V a, V b, V add, SnxFpFast* out) {
    Shape s;
    V fa, fb, r, lane_in, zero_ok, exact_ok, args[3], ixc, za, zb = NULL, result;
    unsigned p, top;
    shape(ir, width, lanes, &s);
    p = s.precision;
    top = s.emax - 1;
    fa = LLVMBuildBitCast(B, a, s.ft, "");
    fb = b ? LLVMBuildBitCast(B, b, s.ft, "") : NULL;
    /* operands normal or zero: zeros follow IEEE's rules on the host, which are the architecture's (FPAdd, FPMul,
       FPMulAdd, FPDiv and FPSqrt give the same zero and sign in round to nearest, and raise nothing for them) */
    za = lane_zero(ir, &s, a);
    lane_in = lane_or(ir, lane_exponent_in(ir, &s, a, 1, top), za);
    if (b) {
        zb = lane_zero(ir, &s, b);
        lane_in = lane_and(ir, lane_in, lane_or(ir, lane_exponent_in(ir, &s, b, 1, top), zb));
    }
    /* a zero result is exact when it comes from a zero operand. Nonzero operands whose result rounds to zero
       underflowed (UFC, IXC), except for a sum: two floats that cancel do so exactly */
    zero_ok = splat(&s, LLVMConstInt(ir->t_i1, 1, 0));
    ixc = ixc_set(ir);
    exact_ok = ixc;
    out->inexact = LLVMConstInt(ir->t_i1, 0, 0);
    switch (op) {
        case SNX_IR_FP_ADD:
        case SNX_IR_FP_SUB: {
            /* TwoSum: the rounding error of a sum is a float, and exactly da+db */
            V y = op == SNX_IR_FP_SUB ? LLVMBuildFNeg(B, fb, "") : fb, bp, ap;
            r = LLVMBuildFAdd(B, fa, y, "");
            bp = LLVMBuildFSub(B, r, fa, "");
            ap = LLVMBuildFSub(B, r, bp, "");
            out->inexact =
                nonzero(ir, &s, LLVMBuildFAdd(B, LLVMBuildFSub(B, fa, ap, ""), LLVMBuildFSub(B, y, bp, ""), ""));
            exact_ok = LLVMConstInt(ir->t_i1, 1, 0);
            break;
        }
        case SNX_IR_FP_MUL:
            zero_ok = lane_or(ir, za, zb);
            r = LLVMBuildFMul(B, fa, fb, "");
            if (host_fma(ir)) { /* a*b - r, exact while the result is at least 2^(emin+p) or a zero operand's */
                args[0] = fa, args[1] = fb, args[2] = LLVMBuildFNeg(B, r, "");
                out->inexact = nonzero(ir, &s, fp_intrinsic(ir, "llvm.fma", &s, args, 3));
                exact_ok = LLVMBuildOr(
                    B, ixc,
                    all(ir, &s,
                        lane_or(ir, lane_exponent_in(ir, &s, LLVMBuildBitCast(B, r, s.it, ""), p + 2, top - 1),
                                zero_ok)),
                    "");
            }
            break;
        case SNX_IR_FP_DIV:
            lane_in = lane_and(ir, lane_in, LLVMBuildNot(B, zb, "")); /* by zero: DZC, or IOC for 0/0 */
            zero_ok = za;
            r = LLVMBuildFDiv(B, fa, fb, "");
            if (host_fma(ir)) { /* a - r*b, exact under the same bounds on a and r, or for a zero dividend */
                args[0] = LLVMBuildFNeg(B, r, ""), args[1] = fb, args[2] = fa;
                out->inexact = nonzero(ir, &s, fp_intrinsic(ir, "llvm.fma", &s, args, 3));
                exact_ok = LLVMBuildOr(
                    B, ixc,
                    all(ir, &s,
                        lane_or(ir,
                                lane_and(ir, lane_exponent_in(ir, &s, LLVMBuildBitCast(B, r, s.it, ""), p + 2, top - 1),
                                         lane_exponent_in(ir, &s, a, p + 2, top - 1)),
                                za)),
                    "");
            }
            break;
        case SNX_IR_FP_SQRT: {
            /* sqrt(-0) is -0 without a flag; other negative operands are invalid */
            lane_in = lane_and(ir, lane_in, lane_or(ir, LLVMBuildICmp(B, LLVMIntSGE, a, kint(ir, &s, 0), ""), za));
            zero_ok = za;
            r = fp_intrinsic(ir, "llvm.sqrt", &s, &fa, 1);
            if (host_fma(ir)) { /* a - r*r */
                args[0] = LLVMBuildFNeg(B, r, ""), args[1] = r, args[2] = fa;
                out->inexact = nonzero(ir, &s, fp_intrinsic(ir, "llvm.fma", &s, args, 3));
                exact_ok =
                    LLVMBuildOr(B, ixc, all(ir, &s, lane_or(ir, lane_exponent_in(ir, &s, a, p + 2, top - 1), za)), "");
            }
            break;
        }
        default: { /* FMA: add + a*b, fused; whether it is exact isn't cheap to know, so only once IXC is set, or when
                      every product is a zero operand's (the result is then add, or a zero by IEEE's sign rules) */
            V zp = lane_or(ir, za, zb);
            args[0] = fa, args[1] = fb, args[2] = LLVMBuildBitCast(B, add, s.ft, "");
            lane_in = lane_and(ir, lane_in, lane_or(ir, lane_exponent_in(ir, &s, add, 1, top), lane_zero(ir, &s, add)));
            zero_ok = zp;
            r = host_fma(ir) ? fp_intrinsic(ir, "llvm.fma", &s, args, 3) : NULL;
            if (r)
                exact_ok = LLVMBuildOr(B, ixc, all(ir, &s, zp), "");
            else { /* no hardware FMA: never fast */
                r = LLVMBuildFAdd(B, args[2], LLVMBuildFMul(B, fa, fb, ""), "");
                exact_ok = LLVMConstInt(ir->t_i1, 0, 0);
            }
            break;
        }
    }
    out->value = LLVMBuildBitCast(B, r, s.it, "");
    /* no denormal or smallest-binade result (tininess is detected before rounding), no overflow, and a zero only
       where zero_ok says it is exact */
    result =
        lane_or(ir, lane_exponent_in(ir, &s, out->value, 2, top), lane_and(ir, lane_zero(ir, &s, out->value), zero_ok));
    out->ok = LLVMBuildAnd(B, LLVMBuildAnd(B, rounds_to_nearest(ir), all(ir, &s, lane_and(ir, lane_in, result)), ""),
                           exact_ok, "");
}

LLVMBasicBlockRef ir_fp_branch(SnxIr* ir, V ok, LLVMBasicBlockRef* slow) {
    LLVMBasicBlockRef fast = ir_block(ir, "fp_fast"), join = ir_block(ir, "fp_join");
    LLVMMetadataRef md[3];
    V br;
    *slow = ir_block(ir, "fp_slow");
    br = LLVMBuildCondBr(B, ok, fast, *slow);
    md[0] = LLVMMDStringInContext2(ir->ctx, "branch_weights", 14);
    md[1] = LLVMValueAsMetadata(ir_i32(ir, 2000));
    md[2] = LLVMValueAsMetadata(ir_i32(ir, 1));
    LLVMSetMetadata(br, LLVMGetMDKindIDInContext(ir->ctx, "prof", 4),
                    LLVMMetadataAsValue(ir->ctx, LLVMMDNodeInContext2(ir->ctx, md, 3)));
    LLVMPositionBuilderAtEnd(B, fast);
    return join;
}
void ir_fp_else(SnxIr* ir, LLVMBasicBlockRef join, LLVMBasicBlockRef slow, V inexact) {
    if (!LLVMIsConstant(inexact) || LLVMConstIntGetZExtValue(inexact)) {
        size_t off = ir_off_fpcr() + 4; /* fpsr |= IXC */
        V fpsr = ir_cpu_load(ir, ir->t_i32, off);
        ir_cpu_store(ir, LLVMBuildOr(B, fpsr, LLVMBuildShl(B, ir_bool32(ir, inexact), i32c(ir, 4), ""), ""), off);
    }
    LLVMBuildBr(B, join);
    LLVMPositionBuilderAtEnd(B, slow);
}
void ir_fp_join(SnxIr* ir, LLVMBasicBlockRef join) {
    LLVMBuildBr(B, join);
    LLVMPositionBuilderAtEnd(B, join);
}

/* the helpers' prototypes in cpu.h: CPU* first, then u64 operands and int parameters */
static V fp_call(SnxIr* ir, const char* name, LLVMTypeRef ret, unsigned wide, V* operands, unsigned count) {
    LLVMTypeRef p[8];
    V args[8];
    unsigned i;
    p[0] = ir->t_ptr;
    args[0] = ir->c;
    for (i = 0; i < count; ++i) {
        p[i + 1] = i < wide ? ir->t_i64 : ir->t_i32;
        args[i + 1] = i < wide ? ir_u64(ir, operands[i]) : operands[i];
    }
    return ir_call(ir, ir_extern(ir, name, ret, p, count + 1), args, count + 1);
}
V ir_fp_binary(SnxIr* ir, V a, V b, unsigned width, unsigned op) {
    V x[4];
    x[0] = a, x[1] = b, x[2] = i32c(ir, width), x[3] = i32c(ir, op);
    return fp_call(ir, "snx_fp_binary", ir->t_i64, 2, x, 4);
}
V ir_fp_minmax(SnxIr* ir, V a, V b, unsigned width, unsigned minimum, unsigned numeric) {
    V x[5];
    x[0] = a, x[1] = b, x[2] = i32c(ir, width), x[3] = i32c(ir, minimum), x[4] = i32c(ir, numeric);
    return fp_call(ir, "snx_fp_minmax", ir->t_i64, 2, x, 5);
}
static V fma3(SnxIr* ir, V add, V a, V b, unsigned width) {
    V x[4];
    x[0] = add, x[1] = a, x[2] = b, x[3] = i32c(ir, width);
    return fp_call(ir, "snx_fp_fma", ir->t_i64, 3, x, 4);
}
V ir_fp_unary(SnxIr* ir, V bits, unsigned width, unsigned op) {
    V x[3];
    x[0] = bits, x[1] = i32c(ir, width), x[2] = i32c(ir, op);
    return fp_call(ir, "snx_fp_unary", ir->t_i64, 1, x, 3);
}
static V convert(SnxIr* ir, V bits, unsigned from, unsigned to, V rm) {
    V x[4];
    x[0] = bits, x[1] = i32c(ir, from), x[2] = i32c(ir, to), x[3] = rm;
    return fp_call(ir, "snx_fp_convert", ir->t_i64, 1, x, 4);
}
static V round_to(SnxIr* ir, V bits, unsigned width, V rm, unsigned exact) {
    V x[4];
    x[0] = bits, x[1] = i32c(ir, width), x[2] = rm, x[3] = i32c(ir, exact);
    return fp_call(ir, "snx_fp_round", ir->t_i64, 1, x, 4);
}
static V compare(SnxIr* ir, V a, V b, unsigned width, V signal) {
    V x[4];
    x[0] = a, x[1] = b, x[2] = i32c(ir, width), x[3] = signal;
    return fp_call(ir, "snx_fp_compare", ir->t_i32, 2, x, 4);
}
V ir_fp_to_int(SnxIr* ir, V bits, unsigned fp, unsigned integer, unsigned uns, unsigned fbits, unsigned rm) {
    V x[6];
    x[0] = bits, x[1] = i32c(ir, fp), x[2] = i32c(ir, integer), x[3] = i32c(ir, uns), x[4] = i32c(ir, fbits),
    x[5] = i32c(ir, rm);
    return fp_call(ir, "snx_fp_to_int", ir->t_i64, 1, x, 6);
}
V ir_fp_from_int(SnxIr* ir, V bits, unsigned integer, unsigned fp, unsigned uns, unsigned fbits) {
    V x[5];
    x[0] = bits, x[1] = i32c(ir, integer), x[2] = i32c(ir, fp), x[3] = i32c(ir, uns), x[4] = i32c(ir, fbits);
    return fp_call(ir, "snx_fp_from_int", ir->t_i64, 1, x, 5);
}
/* FPRM: (int)((c->fpcr >> 22) & 3) */
static V fprm(SnxIr* ir) {
    V fpcr = ir_cpu_load(ir, ir->t_i32, ir_off_fpcr());
    return LLVMBuildAnd(B, LLVMBuildLShr(B, fpcr, i32c(ir, 22), ""), i32c(ir, 3), "");
}
/* simd_2misc's FCVTN (FPRM) and FCVTXN (mode 5, round to odd) lane */
V ir_fp_narrow(SnxIr* ir, V bits, unsigned from, unsigned odd) {
    return convert(ir, bits, from, from / 2, odd ? i32c(ir, 5) : fprm(ir));
}

/* SETSB/SETDB/SETH(d,e): t=(uN)(e); c->v[d].d[1]=0; c->v[d].d[0]=t; */
static void set_bits(SnxIr* ir, unsigned d, unsigned bits, V value) {
    V t = ir_cast(ir, value, LLVMIntTypeInContext(ir->ctx, bits), 0);
    ir_set_vd(ir, d, 1, ir_i64(ir, 0));
    ir_set_vd(ir, d, 0, ir_u64(ir, t));
}

/* c->v[n].s[0], .d[0] or .h[0] for ftype 0, 1, 3 */
static V operand(SnxIr* ir, unsigned n, unsigned width) {
    return ir_ve(ir, n, width == 64 ? 3 : width == 32 ? 2 : 1, 0);
}
static V sign(SnxIr* ir, V x, unsigned width) {
    return LLVMBuildXor(B, x, width == 64 ? ir_i64(ir, (SnxU64)1 << 63) : i32c(ir, 0x80000000UL), "");
}
static V truth(SnxIr* ir, V v) { return LLVMBuildICmp(B, LLVMIntNE, v, i32c(ir, 0), ""); }
static V cond_ok(SnxIr* ir, unsigned cond) {
    V args[2];
    args[0] = ir->c;
    args[1] = i32c(ir, cond);
    return ir_call_helper(ir, SNX_IR_COND_OK, args, 2);
}
static void set_nzcv(SnxIr* ir, V flags) {
    V args[2];
    args[0] = ir->c;
    args[1] = flags;
    ir_call_helper(ir, SNX_IR_SET_NZCV, args, 2);
}

static int fp2(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned op = index & 15, width = index >> 4 ? 64 : 32, rd = F(0, 5);
    V a = operand(ir, F(5, 5), width), b = operand(ir, F(16, 5), width), r;
    static const unsigned binary_ops[4] = {2, 3, 0, 1}; /* MUL, DIV, ADD, SUB */
    if (op < 4 || op == 8) {                            /* MUL, DIV, ADD, SUB, FNMUL: inline when exact */
        static const unsigned fast_ops[4] = {SNX_IR_FP_MUL, SNX_IR_FP_DIV, SNX_IR_FP_ADD, SNX_IR_FP_SUB};
        LLVMBasicBlockRef slow, join;
        SnxFpFast f;
        ir_fp_fast(ir, op == 8 ? SNX_IR_FP_MUL : fast_ops[op], width, 1, a, b, NULL, &f);
        join = ir_fp_branch(ir, f.ok, &slow);
        set_bits(ir, rd, width, op == 8 ? sign(ir, f.value, width) : f.value);
        ir_fp_else(ir, join, slow, f.inexact);
        if (op == 8)
            r = sign(ir, ir_cast(ir, ir_fp_binary(ir, a, b, width, 2), width == 64 ? ir->t_i64 : ir->t_i32, 0), width);
        else
            r = ir_fp_binary(ir, a, b, width, binary_ops[op]);
        set_bits(ir, rd, width, r);
        ir_fp_join(ir, join);
        return 1;
    }
    if (op < 8) { /* FMAX, FMIN, FMAXNM, FMINNM: inline without a NaN (or a denormal under FZ) */
        LLVMBasicBlockRef slow, join;
        SnxFpFast f;
        ir_fp_minmax_fast(ir, width, 1, a, b, op & 1, &f);
        join = ir_fp_branch(ir, f.ok, &slow);
        set_bits(ir, rd, width, f.value);
        ir_fp_else(ir, join, slow, f.inexact);
        set_bits(ir, rd, width, ir_fp_minmax(ir, a, b, width, op & 1, (op >> 1) & 1));
        ir_fp_join(ir, join);
        return 1;
    }
    return 0;
}

static int fp1(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned op = index & 63, type = (unsigned)(index >> 6), rd = F(0, 5), rn = F(5, 5);
    unsigned width = type == 0 ? 32 : type == 1 ? 64 : 16;
    V a = operand(ir, rn, width), r;
    if (type == 3) { /* FCVT from half */
        if (op != 4 && op != 5) return 0;
        set_bits(ir, rd, op == 4 ? 32 : 64, convert(ir, a, 16, op == 4 ? 32 : 64, fprm(ir)));
        return 1;
    }
    if (type > 1) return 0;
    switch (op) {
        case 0: r = a; break;
        case 1:
            r = LLVMBuildAnd(B, a, width == 64 ? ir_i64(ir, ~((SnxU64)1 << 63)) : i32c(ir, 0x7FFFFFFFUL), "");
            break;
        case 2: r = sign(ir, a, width); break;
        case 3: { /* SNX_FP_SQRT, inline when exact */
            LLVMBasicBlockRef slow, join;
            SnxFpFast f;
            ir_fp_fast(ir, SNX_IR_FP_SQRT, width, 1, a, NULL, NULL, &f);
            join = ir_fp_branch(ir, f.ok, &slow);
            set_bits(ir, rd, width, f.value);
            ir_fp_else(ir, join, slow, f.inexact);
            set_bits(ir, rd, width, ir_fp_unary(ir, a, width, 0));
            ir_fp_join(ir, join);
            return 1;
        }
        case 4:
        case 5:
        case 7: {
            unsigned to = op == 7 ? 16 : op == 4 ? 32 : 64;
            if (to == width) return 0;
            if (to != 16) {
                /* single <-> double: widening is exact for normal values and zeros; narrowing takes the usual
                   bounds, inexact when widening back differs */
                Shape from, into;
                V f, r, ok, inexact;
                LLVMBasicBlockRef slow, join;
                shape(ir, width, 1, &from);
                shape(ir, to, 1, &into);
                f = LLVMBuildBitCast(B, a, from.ft, "");
                if (to == 64) {
                    r = LLVMBuildFPExt(B, f, into.ft, "");
                    ok = LLVMBuildOr(
                        B, exponent_in(ir, &from, a, 1, from.emax - 1),
                        LLVMBuildICmp(B, LLVMIntEQ, LLVMBuildShl(B, a, kint(ir, &from, 1), ""), kint(ir, &from, 0), ""),
                        "");
                    inexact = LLVMConstInt(ir->t_i1, 0, 0);
                } else {
                    r = LLVMBuildFPTrunc(B, f, into.ft, "");
                    ok = LLVMBuildAnd(
                        B, LLVMBuildAnd(B, rounds_to_nearest(ir), exponent_in(ir, &from, a, 1, from.emax - 1), ""),
                        exponent_in(ir, &into, LLVMBuildBitCast(B, r, into.it, ""), 2, into.emax - 1), "");
                    inexact = LLVMBuildFCmp(B, LLVMRealUNE, LLVMBuildFPExt(B, r, from.ft, ""), f, "");
                }
                join = ir_fp_branch(ir, ok, &slow);
                set_bits(ir, rd, to, LLVMBuildBitCast(B, r, into.it, ""));
                ir_fp_else(ir, join, slow, inexact);
                set_bits(ir, rd, to, convert(ir, a, width, to, fprm(ir)));
                ir_fp_join(ir, join);
                return 1;
            }
            set_bits(ir, rd, to, convert(ir, a, width, to, fprm(ir)));
            return 1;
        }
        case 8:
        case 9:
        case 10:
        case 11:
        case 12: r = round_to(ir, a, width, i32c(ir, op - 8), 0); break;
        case 14: r = round_to(ir, a, width, fprm(ir), 1); break;
        case 15: r = round_to(ir, a, width, fprm(ir), 0); break;
        default: return 0;
    }
    set_bits(ir, rd, width, r);
    return 1;
}

/* a denormal: exponent 0 and a nonzero fraction (zero has exponent 0 too, and flushing it changes nothing) */
static V lane_denormal(SnxIr* ir, const Shape* s, V bits) {
    return LLVMBuildAnd(B, LLVMBuildNot(B, lane_exponent_in(ir, s, bits, 1, s->emax), ""),
                        LLVMBuildNot(B, lane_zero(ir, s, bits), ""), "");
}

/* neither operand a NaN in any lane, and FZ clear or no operand a denormal in any lane: fp_input then changes
   nothing and sets no IDC, and only a NaN can raise IOC */
static V no_nan_no_flush(SnxIr* ir, const Shape* s, V a, V b, V fa, V fb) {
    V fz =
        LLVMBuildICmp(B, LLVMIntNE, LLVMBuildAnd(B, ir_cpu_load(ir, ir->t_i32, ir_off_fpcr()), i32c(ir, 1u << 24), ""),
                      i32c(ir, 0), "");
    V flush = LLVMBuildAnd(B, fz, any(ir, s, lane_or(ir, lane_denormal(ir, s, a), lane_denormal(ir, s, b))), "");
    return LLVMBuildNot(B, LLVMBuildOr(B, any(ir, s, LLVMBuildFCmp(B, LLVMRealUNO, fa, fb, "")), flush, ""), "");
}

void ir_fp_minmax_fast(SnxIr* ir, unsigned width, unsigned lanes, V a, V b, unsigned minimum, SnxFpFast* out) {
    Shape s;
    V fa, fb, gt, lt, tie;
    shape(ir, width, lanes, &s);
    fa = LLVMBuildBitCast(B, a, s.ft, "");
    fb = LLVMBuildBitCast(B, b, s.ft, "");
    gt = LLVMBuildFCmp(B, LLVMRealOGT, fa, fb, "");
    lt = LLVMBuildFCmp(B, LLVMRealOLT, fa, fb, "");
    /* neither larger: equal, so the same bits unless they are zeros of different signs */
    tie = minimum ? LLVMBuildOr(B, a, b, "") : LLVMBuildAnd(B, a, b, "");
    out->value = minimum ? LLVMBuildSelect(B, lt, a, LLVMBuildSelect(B, gt, b, tie, ""), "")
                         : LLVMBuildSelect(B, gt, a, LLVMBuildSelect(B, lt, b, tie, ""), "");
    out->ok = no_nan_no_flush(ir, &s, a, b, fa, fb);
    out->inexact = LLVMConstInt(ir->t_i1, 0, 0);
}

void ir_fp_compare_fast(SnxIr* ir, unsigned cmp, unsigned width, unsigned lanes, V a, V b, SnxFpFast* out) {
    static const LLVMRealPredicate predicates[5] = {LLVMRealOEQ, LLVMRealOGE, LLVMRealOGT, LLVMRealOLE, LLVMRealOLT};
    Shape s;
    V fa, fb;
    shape(ir, width, lanes, &s);
    fa = LLVMBuildBitCast(B, a, s.ft, "");
    fb = LLVMBuildBitCast(B, b, s.ft, "");
    out->value = LLVMBuildSExt(B, LLVMBuildFCmp(B, predicates[cmp], fa, fb, ""), s.it, "");
    out->ok = no_nan_no_flush(ir, &s, a, b, fa, fb);
    out->inexact = LLVMConstInt(ir->t_i1, 0, 0);
}

/* fcmpmask: snx_fp_compare's NZCV (8 less, 6 equal, 2 greater, 3 unordered) tested for the predicate */
V ir_fp_compare_mask(SnxIr* ir, unsigned cmp, V a, V b, unsigned width) {
    V flags = compare(ir, a, b, width, i32c(ir, cmp != SNX_IR_CMP_EQ)), hit;
    V less = LLVMBuildICmp(B, LLVMIntEQ, flags, i32c(ir, 8), "");
    V equal = LLVMBuildICmp(B, LLVMIntEQ, flags, i32c(ir, 6), "");
    V greater = LLVMBuildICmp(B, LLVMIntEQ, flags, i32c(ir, 2), "");
    switch (cmp) {
        case SNX_IR_CMP_EQ: hit = equal; break;
        case SNX_IR_CMP_GE: hit = LLVMBuildOr(B, equal, greater, ""); break;
        case SNX_IR_CMP_GT: hit = greater; break;
        case SNX_IR_CMP_LE: hit = LLVMBuildOr(B, less, equal, ""); break;
        default: hit = less; break;
    }
    return LLVMBuildSExt(B, hit, width == 64 ? ir->t_i64 : ir->t_i32, "");
}

/* FCMP without a NaN, and without a denormal when FZ is set (flushing it sets IDC): the host comparison gives the
   flags, and no FPSR bit changes. Zeros compare as IEEE says, equal whatever their signs, with FZ or without */
static V compare_fast(SnxIr* ir, V a, V b, unsigned width, V* ok) {
    Shape s;
    V fa, fb, nzcv;
    shape(ir, width, 1, &s);
    a = ir_cast(ir, a, s.it, 0);
    b = ir_cast(ir, b, s.it, 0);
    fa = LLVMBuildBitCast(B, a, s.ft, "");
    fb = LLVMBuildBitCast(B, b, s.ft, "");
    *ok = no_nan_no_flush(ir, &s, a, b, fa, fb);
    nzcv = LLVMBuildSelect(B, LLVMBuildFCmp(B, LLVMRealOLT, fa, fb, ""), i32c(ir, 8), i32c(ir, 2), "");
    return LLVMBuildSelect(B, LLVMBuildFCmp(B, LLVMRealOEQ, fa, fb, ""), i32c(ir, 6), nzcv, "");
}

static void fcmp(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned width = index >> 2 ? 64 : 32;
    V b = index & 1 ? ir_i64(ir, 0) : operand(ir, F(16, 5), width), a = operand(ir, F(5, 5), width), ok, fast;
    LLVMBasicBlockRef slow, join;
    fast = compare_fast(ir, a, b, width, &ok);
    join = ir_fp_branch(ir, ok, &slow);
    set_nzcv(ir, fast);
    ir_fp_else(ir, join, slow, LLVMConstInt(ir->t_i1, 0, 0));
    set_nzcv(ir, compare(ir, a, b, width, i32c(ir, (index >> 1) & 1)));
    ir_fp_join(ir, join);
}

static void fccmp(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned width = index ? 64 : 32;
    LLVMBasicBlockRef taken = ir_block(ir, "fccmp"), flags = ir_block(ir, "nzcv"), done = ir_block(ir, "done");
    LLVMBuildCondBr(B, truth(ir, cond_ok(ir, F(12, 4))), taken, flags);
    LLVMPositionBuilderAtEnd(B, taken);
    {
        V a = operand(ir, F(5, 5), width), b = operand(ir, F(16, 5), width), ok, fast;
        LLVMBasicBlockRef slow, join;
        fast = compare_fast(ir, a, b, width, &ok);
        join = ir_fp_branch(ir, ok, &slow);
        set_nzcv(ir, fast);
        ir_fp_else(ir, join, slow, LLVMConstInt(ir->t_i1, 0, 0));
        set_nzcv(ir, compare(ir, a, b, width, i32c(ir, F(4, 1))));
        ir_fp_join(ir, join);
    }
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, flags);
    set_nzcv(ir, i32c(ir, F(0, 4)));
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, done);
}

static void fcsel(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned width = index ? 64 : 32;
    V ok = truth(ir, cond_ok(ir, F(12, 4)));
    set_bits(ir, F(0, 5), width, LLVMBuildSelect(B, ok, operand(ir, F(5, 5), width), operand(ir, F(16, 5), width), ""));
}

static void fmov_imm(SnxIr* ir, SnxU32 w, size_t index) {
    V imm = i32c(ir, F(13, 8));
    set_bits(ir, F(0, 5), index ? 64 : 32, ir_call_helper(ir, index ? SNX_IR_VFP_IMM64 : SNX_IR_VFP_IMM32, &imm, 1));
}

static void fp3(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned o0 = index & 1, o1 = (index >> 1) & 1, width = index >> 2 ? 64 : 32;
    V a = operand(ir, F(10, 5), width), n = operand(ir, F(5, 5), width), m = operand(ir, F(16, 5), width);
    /* FMADD, FMSUB (-n), FNMADD (-a, -n), FNMSUB (-a) */
    LLVMBasicBlockRef slow, join;
    SnxFpFast f;
    if (o1) a = sign(ir, a, width);
    if (o0 != o1) n = sign(ir, n, width);
    ir_fp_fast(ir, SNX_IR_FP_FMA, width, 1, n, m, a, &f);
    join = ir_fp_branch(ir, f.ok, &slow);
    set_bits(ir, F(0, 5), width, f.value);
    ir_fp_else(ir, join, slow, f.inexact);
    set_bits(ir, F(0, 5), width, fma3(ir, a, n, m, width));
    ir_fp_join(ir, join);
}

/* the host rounds to an integral value in one instruction (x86 SSE4.1 ROUNDSD, part of the CPUID sse4.2 gate;
   AArch64 FRINT*): llvm.floor and friends never become C library calls */
static int host_round(SnxIr* ir) {
    if (ir->features->x86) return ir->features->sse42;
    return !strncmp(LLVMGetTarget(ir->mod), "aarch64", 7);
}

/* 2^e, exactly (|e| <= 64 here) */
static double pow2(int e) {
    double r = 1.0;
    for (; e > 0; --e) r *= 2.0;
    for (; e < 0; ++e) r *= 0.5;
    return r;
}

int ir_fp_to_int_fast(SnxIr* ir, unsigned width, unsigned lanes, V bits, unsigned integer, unsigned uns, unsigned rm,
                      unsigned fbits, SnxFpFast* out) {
    static const char* const rounding[5] = {"llvm.roundeven", "llvm.ceil", "llvm.floor", "llvm.trunc", "llvm.round"};
    Shape s;
    V f, scaled, rounded, fz, limit, low, result, e, mantissa, ok;
    LLVMTypeRef it = LLVMIntTypeInContext(ir->ctx, integer);
    double top = integer == 64 ? 9223372036854775808.0 : 2147483648.0;
    if (rm != 3 && !host_round(ir)) return 0;
    shape(ir, width, lanes, &s);
    if (lanes > 1) it = LLVMVectorType(it, lanes);
    f = LLVMBuildBitCast(B, bits, s.ft, "");
    /* scaling by 2^fbits is exact, or overflows to an infinity, which is out of range */
    scaled = fbits ? LLVMBuildFMul(B, f, kfloat(ir, &s, pow2((int)fbits)), "") : f;
    rounded = rm == 3 ? scaled : fp_intrinsic(ir, rounding[rm], &s, &scaled, 1); /* FPToSI truncates by itself */
    e = exponent(ir, &s, bits);
    mantissa = LLVMBuildAnd(B, bits, kint(ir, &s, (((SnxU64)1 << s.mantissa) - 1)), "");
    fz = LLVMBuildICmp(B, LLVMIntNE, LLVMBuildAnd(B, ir_cpu_load(ir, ir->t_i32, ir_off_fpcr()), i32c(ir, 1u << 24), ""),
                       i32c(ir, 0), "");
    /* finite, and no denormal to flush */
    ok = LLVMBuildAnd(B, all(ir, &s, LLVMBuildICmp(B, LLVMIntNE, e, kint(ir, &s, s.emax), "")),
                      LLVMBuildOr(B, LLVMBuildNot(B, fz, ""),
                                  all(ir, &s,
                                      LLVMBuildOr(B, LLVMBuildICmp(B, LLVMIntNE, e, kint(ir, &s, 0), ""),
                                                  LLVMBuildICmp(B, LLVMIntEQ, mantissa, kint(ir, &s, 0), ""), "")),
                                  ""),
                      "");
    if (uns) {
        limit = kfloat(ir, &s, top * 2);
        low = kfloat(ir, &s, 0.0); /* -0 compares equal: FCVTPU(-0.3) is 0, inexact */
        result = LLVMBuildFPToUI(B, rounded, it, "");
    } else {
        limit = kfloat(ir, &s, top);
        low = kfloat(ir, &s, -top);
        result = LLVMBuildFPToSI(B, rounded, it, "");
    }
    /* out of range the conversion is poison, used only on the slow path's side of the branch, which ignores it */
    out->ok = LLVMBuildAnd(B, ok,
                           all(ir, &s,
                               LLVMBuildAnd(B, LLVMBuildFCmp(B, LLVMRealOGE, rounded, low, ""),
                                            LLVMBuildFCmp(B, LLVMRealOLT, rounded, limit, ""), "")),
                           "");
    if (rm == 3) /* FPToSI rounded: the integer, converted back (exact while it is in range) */
        rounded = uns ? LLVMBuildUIToFP(B, result, s.ft, "") : LLVMBuildSIToFP(B, result, s.ft, "");
    out->inexact = any(ir, &s, LLVMBuildFCmp(B, LLVMRealUNE, rounded, scaled, ""));
    out->value = result;
    return 1;
}

void ir_fp_from_int_fast(SnxIr* ir, unsigned width, unsigned lanes, V v, unsigned uns, unsigned fbits, SnxFpFast* out) {
    Shape s;
    V f, x, small, p, p2;
    SnxU64 limit;
    shape(ir, width, lanes, &s);
    limit = (SnxU64)1 << s.precision;
    f = uns ? LLVMBuildUIToFP(B, v, s.ft, "") : LLVMBuildSIToFP(B, v, s.ft, "");
    /* scaling by 2^-fbits is exact: the smallest nonzero result, 2^-64, is a normal number */
    if (fbits) f = LLVMBuildFMul(B, f, kfloat(ir, &s, pow2(-(int)fbits)), "");
    /* exact for magnitudes up to 2^precision: one lane widened to 64 bits, lanes in their own width (as wide as the
       float, so 2^(precision+1) fits them) */
    if (lanes == 1) {
        x = uns ? ir_u64(ir, v) : ir_s64(ir, v);
        p = ir_i64(ir, limit);
        p2 = ir_i64(ir, 2 * limit);
    } else {
        x = v;
        p = kint(ir, &s, limit);
        p2 = kint(ir, &s, 2 * limit);
    }
    if (uns)
        small = LLVMBuildICmp(B, LLVMIntULE, x, p, "");
    else
        small = LLVMBuildICmp(B, LLVMIntULE, LLVMBuildAdd(B, x, p, ""), p2, "");
    out->ok = LLVMBuildAnd(B, rounds_to_nearest(ir), LLVMBuildOr(B, all(ir, &s, small), ixc_set(ir), ""), "");
    out->value = LLVMBuildBitCast(B, f, s.it, "");
    out->inexact = LLVMConstInt(ir->t_i1, 0, 0);
}

void ir_fp_estimate_fast(SnxIr* ir, unsigned rsqrt, unsigned width, unsigned lanes, V bits, SnxFpFast* out) {
    Shape s, n;
    V e, est, power, t;
    unsigned f, bias, k;
    shape(ir, width, lanes, &s);
    shape(ir, 32, lanes, &n); /* the estimate's arithmetic, in 32-bit lanes */
    f = s.mantissa;
    bias = s.emax >> 1;
    e = exponent(ir, &s, bits);
    if (!rsqrt) {
        /* recip_est(256 + i) = (2^19 / (513 + 2i) + 1) >> 1, i the fraction's top 8 bits. The quotient's floor comes
           from a float division: the divisor is odd and below 2^10, so the quotient is never an integer and lies
           more than 2^-10 from one, far beyond the division's rounding error (2^-15 below 1024) */
        V d;
        t = LLVMBuildAnd(B, LLVMBuildLShr(B, bits, kint(ir, &s, f - 8), ""), kint(ir, &s, 255), "");
        if (width == 64) t = LLVMBuildTrunc(B, t, n.it, "");
        d = LLVMBuildAdd(B, LLVMBuildShl(B, t, kint(ir, &n, 1), ""), kint(ir, &n, 513), "");
        est = LLVMBuildFPToUI(B, LLVMBuildFDiv(B, kfloat(ir, &n, 524288.0), LLVMBuildUIToFP(B, d, n.ft, ""), ""), n.it,
                              "");
        est = LLVMBuildLShr(B, LLVMBuildAdd(B, est, kint(ir, &n, 1), ""), kint(ir, &n, 1), "");
        /* exponents 1..emax-3: the reciprocal's, emax-2-e, is normal */
        power = LLVMBuildSub(B, kint(ir, &s, s.emax - 2), e, "");
        out->ok = exponent_in(ir, &s, bits, 1, s.emax - 3);
        power = LLVMBuildOr(B, LLVMBuildAnd(B, bits, kint(ir, &s, (SnxU64)1 << (width - 1)), ""),
                            LLVMBuildShl(B, power, kint(ir, &s, f), ""), "");
    } else {
        /* rsqrt_est's operand: 2*sc+1 for an odd exponent, sc = 128 + t (t the fraction's top 7 bits), and
           2*((sc&~1)+1) for an even one, sc = 256 + the top 8 bits: 257 + 2t and 514 + 4t. Then its binary search,
           nine halvings of [512, 1024) */
        V odd, a, lo = kint(ir, &n, 512), hi = kint(ir, &n, 1024);
        t = LLVMBuildAnd(B, LLVMBuildLShr(B, bits, kint(ir, &s, f - 7), ""), kint(ir, &s, 127), "");
        odd = LLVMBuildICmp(B, LLVMIntNE, LLVMBuildAnd(B, e, kint(ir, &s, 1), ""), kint(ir, &s, 0), "");
        if (width == 64) t = LLVMBuildTrunc(B, t, n.it, "");
        a = LLVMBuildSelect(B, odd, LLVMBuildAdd(B, LLVMBuildShl(B, t, kint(ir, &n, 1), ""), kint(ir, &n, 257), ""),
                            LLVMBuildAdd(B, LLVMBuildShl(B, t, kint(ir, &n, 2), ""), kint(ir, &n, 514), ""), "");
        for (k = 0; k < 9; ++k) {
            V mid = LLVMBuildLShr(B, LLVMBuildAdd(B, lo, hi, ""), kint(ir, &n, 1), "");
            V below = LLVMBuildICmp(B, LLVMIntULT, LLVMBuildMul(B, LLVMBuildMul(B, a, mid, ""), mid, ""),
                                    kint(ir, &n, 1u << 28), "");
            lo = LLVMBuildSelect(B, below, mid, lo, "");
            hi = LLVMBuildSelect(B, below, hi, mid, "");
        }
        est = LLVMBuildLShr(B, LLVMBuildAdd(B, lo, kint(ir, &n, 1), ""), kint(ir, &n, 1), "");
        /* positive normal operands: the result exponent (3*bias - 1 - e) / 2 is normal */
        out->ok = LLVMBuildAnd(B, exponent_in(ir, &s, bits, 1, s.emax - 1),
                               all(ir, &s, LLVMBuildICmp(B, LLVMIntSGE, bits, kint(ir, &s, 0), "")), "");
        power =
            LLVMBuildShl(B, LLVMBuildLShr(B, LLVMBuildSub(B, kint(ir, &s, 3 * bias - 1), e, ""), kint(ir, &s, 1), ""),
                         kint(ir, &s, f), "");
    }
    est = LLVMBuildAnd(B, est, kint(ir, &n, 255), "");
    if (width == 64) est = LLVMBuildZExt(B, est, s.it, "");
    out->value = LLVMBuildOr(B, power, LLVMBuildShl(B, est, kint(ir, &s, f - 8), ""), "");
    out->inexact = LLVMConstInt(ir->t_i1, 0, 0);
}

void ir_fp_step_fast(SnxIr* ir, unsigned rsqrt, unsigned width, unsigned lanes, V a, V b, SnxFpFast* out) {
    Shape s;
    SnxU64 two;
    shape(ir, width, lanes, &s);
    two = (SnxU64)((s.emax >> 1) + 1) << s.mantissa;
    /* snx_fp_binary negates a and fuses: 2 + (-a)*b, or 3 + (-a)*b */
    ir_fp_fast(ir, SNX_IR_FP_FMA, width, lanes, LLVMBuildXor(B, a, kint(ir, &s, (SnxU64)1 << (width - 1)), ""), b,
               kint(ir, &s, two | (rsqrt ? (SnxU64)1 << (s.mantissa - 1) : 0)), out);
    if (rsqrt) {
        /* halving the rounded sum is the rounded half (FPMulAdd's scale) while the half stays out of the smallest
           binade, where it could be tiny before rounding */
        V half = LLVMBuildBitCast(
            B, LLVMBuildFMul(B, LLVMBuildBitCast(B, out->value, s.ft, ""), kfloat(ir, &s, 0.5), ""), s.it, "");
        out->ok = LLVMBuildAnd(B, out->ok, exponent_in(ir, &s, half, 2, s.emax - 1), "");
        out->value = half;
    }
}

/* snx_fp_convert's fp_pack for a normal operand whose exponent, rebiased, is a normal one of the narrow format: the
   magnitude shifted right by the difference of the fractions, less the difference of the biases in the exponent
   field, is the truncated result; the dropped bits round it (fp_quantize: to nearest even, or to odd), and a carry
   out of the fraction is the exponent's increment, as fp_pack's renormalization. Only round to nearest can carry
   into the infinity encoding, which takes the helper (OFC) */
void ir_fp_narrow_fast(SnxIr* ir, unsigned from, unsigned lanes, V bits, unsigned odd, SnxFpFast* out) {
    Shape s;
    unsigned to = from / 2, narrow_mantissa = from == 64 ? 23 : 10, narrow_emax = from == 64 ? 0xFF : 0x1F;
    unsigned shift, rebias;
    V sign, magnitude, truncated, dropped, lost, r, ok;
    LLVMTypeRef t = LLVMIntTypeInContext(ir->ctx, to);
    shape(ir, from, lanes, &s);
    shift = s.mantissa - narrow_mantissa;
    rebias = (s.emax >> 1) - (narrow_emax >> 1); /* 1023 - 127, 127 - 15 */
    sign = LLVMBuildAnd(B, bits, kint(ir, &s, (SnxU64)1 << (from - 1)), "");
    magnitude = LLVMBuildXor(B, bits, sign, "");
    ok = lane_exponent_in(ir, &s, bits, rebias + 1, rebias + narrow_emax - 1);
    truncated = LLVMBuildSub(B, LLVMBuildLShr(B, magnitude, kint(ir, &s, shift), ""),
                             kint(ir, &s, (SnxU64)rebias << narrow_mantissa), "");
    dropped = LLVMBuildAnd(B, magnitude, kint(ir, &s, ((SnxU64)1 << shift) - 1), "");
    lost = LLVMBuildICmp(B, LLVMIntNE, dropped, kint(ir, &s, 0), "");
    if (odd)
        r = LLVMBuildOr(B, truncated, LLVMBuildZExt(B, lost, s.it, ""), "");
    else {
        V half = kint(ir, &s, (SnxU64)1 << (shift - 1));
        V up = LLVMBuildOr(
            B, LLVMBuildICmp(B, LLVMIntUGT, dropped, half, ""),
            LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntEQ, dropped, half, ""),
                         LLVMBuildTrunc(B, truncated, lanes > 1 ? LLVMVectorType(ir->t_i1, lanes) : ir->t_i1, ""), ""),
            "");
        r = LLVMBuildAdd(B, truncated, LLVMBuildZExt(B, up, s.it, ""), "");
        ok = LLVMBuildAnd(
            B, ok, LLVMBuildICmp(B, LLVMIntULT, r, kint(ir, &s, (SnxU64)narrow_emax << narrow_mantissa), ""), "");
    }
    r = LLVMBuildOr(B, r, LLVMBuildLShr(B, sign, kint(ir, &s, from - to), ""), "");
    out->value = LLVMBuildTrunc(B, r, lanes > 1 ? LLVMVectorType(t, lanes) : t, "");
    out->ok = all(ir, &s, ok);
    if (!odd) out->ok = LLVMBuildAnd(B, rounds_to_nearest(ir), out->ok, "");
    out->inexact = any(ir, &s, lost);
}

/* FCVT_INT and FCVT_FIX: index = opcode | rmode << 3 | ftype << 5 | sf << 7 */
static int fcvt(SnxIr* ir, SnxU32 w, size_t index, int fixed) {
    unsigned opcode = index & 7, rmode = (index >> 3) & 3, type = (index >> 5) & 3, sf = (unsigned)(index >> 7);
    unsigned rd = F(0, 5), rn = F(5, 5), integer = sf ? 64 : 32, width = type == 0 ? 32 : type == 1 ? 64 : 16;
    unsigned fbits = fixed ? 64 - F(10, 6) : 0;
    LLVMBasicBlockRef slow, join;
    SnxFpFast f;
    switch (opcode) {
        case 0:
        case 1:
        case 4:
        case 5:
            if (type > 1) return 0;
            {
                unsigned rm = opcode >= 4 ? 4 : rmode;
                V a = operand(ir, rn, width);
                if (!ir_fp_to_int_fast(ir, width, 1, a, integer, opcode & 1, rm, fbits, &f)) {
                    ir_setx(ir, rd, ir_fp_to_int(ir, a, width, integer, opcode & 1, fbits, rm));
                    return 1;
                }
                join = ir_fp_branch(ir, f.ok, &slow);
                ir_setx(ir, rd, ir_u64(ir, f.value)); /* the helper masks a 32-bit result to 32 bits */
                ir_fp_else(ir, join, slow, f.inexact);
                ir_setx(ir, rd, ir_fp_to_int(ir, a, width, integer, opcode & 1, fbits, rm));
                ir_fp_join(ir, join);
            }
            return 1;
        case 2:
        case 3:
            if (type > 1) return 0;
            {
                V x = ir_rx(ir, rn);
                ir_fp_from_int_fast(ir, width, 1, LLVMBuildTrunc(B, x, LLVMIntTypeInContext(ir->ctx, integer), ""),
                                    opcode & 1, fbits, &f);
                join = ir_fp_branch(ir, f.ok, &slow);
                set_bits(ir, rd, width, f.value);
                ir_fp_else(ir, join, slow, f.inexact);
                set_bits(ir, rd, width, ir_fp_from_int(ir, x, integer, width, opcode & 1, fbits));
                ir_fp_join(ir, join);
            }
            return 1;
        case 6: /* FMOV to a general register */
            if (type == 2)
                ir_setx(ir, rd, ir_vd(ir, rn, 1));
            else
                ir_setx(ir, rd, ir_u64(ir, operand(ir, rn, width)));
            return 1;
        case 7: /* FMOV from a general register */
            if (type == 2)
                ir_set_vd(ir, rd, 1, ir_rx(ir, rn));
            else
                set_bits(ir, rd, width, width == 64 ? ir_rx(ir, rn) : ir_u32(ir, ir_rx(ir, rn)));
            return 1;
    }
    return 0;
}

int snx_ir_fp_plain(SnxIr* ir, SnxU32 w, SnxU64 pc, const SnxA64Pattern* pattern, size_t index) {
    const char* name = pattern->name;
    (void)pc;
    if (!strcmp(name, "FP2")) return fp2(ir, w, index);
    if (!strcmp(name, "FP1")) return fp1(ir, w, index);
    if (!strcmp(name, "FCMP"))
        fcmp(ir, w, index);
    else if (!strcmp(name, "FCCMP"))
        fccmp(ir, w, index);
    else if (!strcmp(name, "FCSEL"))
        fcsel(ir, w, index);
    else if (!strcmp(name, "FMOV_IMM"))
        fmov_imm(ir, w, index);
    else if (!strcmp(name, "FP3"))
        fp3(ir, w, index);
    else if (!strcmp(name, "FCVT_INT"))
        return fcvt(ir, w, index, 0);
    else if (!strcmp(name, "FCVT_FIX"))
        return fcvt(ir, w, index, 1);
    else
        return 0;
    return 1;
}
