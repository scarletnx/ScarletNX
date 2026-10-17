/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate-a64.c, helper-a64.c, vec_helper.c, vfp_helper.c,
   crypto_helper.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de>
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org>
   Copyright (c) 2003 Fabrice Bellard */
/* Scalar floating point: the FP patterns of a64_templates.inc, as ir_fp.c builds them for LLVM. Their C calls the
   integer-only FP of cpu_helpers.c (snx_fp_*), in the runtime library; the fast paths use the host's IEEE arithmetic
   where it gives the helpers' exact result and flags (ir.h explains when). */
#include "common/cpu/recompiler/gccjit/jit.h"
#include "common/cpu/aarch64/cpu.h"
#include <string.h>

typedef gcc_jit_rvalue* V;
typedef gcc_jit_type* T;
#define F(lo, n) ((w >> (lo)) & ((1UL << (n)) - 1))

#define ADD(a, b) jit_op(j, GCC_JIT_BINARY_OP_PLUS, a, b)
#define SUB(a, b) jit_op(j, GCC_JIT_BINARY_OP_MINUS, a, b)
#define MUL(a, b) jit_op(j, GCC_JIT_BINARY_OP_MULT, a, b)
#define DIV(a, b) jit_op(j, GCC_JIT_BINARY_OP_DIVIDE, a, b)
#define AND(a, b) jit_op(j, GCC_JIT_BINARY_OP_BITWISE_AND, a, b)
#define OR(a, b)  jit_op(j, GCC_JIT_BINARY_OP_BITWISE_OR, a, b)
#define XOR(a, b) jit_op(j, GCC_JIT_BINARY_OP_BITWISE_XOR, a, b)
#define SHL(a, b) jit_op(j, GCC_JIT_BINARY_OP_LSHIFT, a, b)
#define SHR(a, b) jit_op(j, GCC_JIT_BINARY_OP_RSHIFT, a, b)
#define NEG(a)    jit_unary(j, GCC_JIT_UNARY_OP_MINUS, a)
#define EQ(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_EQ, a, b)
#define NE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_NE, a, b)
#define LT(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_LT, a, b)
#define LE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_LE, a, b)
#define GE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_GE, a, b)
#define U32(v)    jit_cast(j, v, j->t_u32)
#define U64(v)    jit_cast(j, v, j->t_u64)
#define K32(v)    jit_u32(j, v)
#define K64(v)    jit_u64(j, v)

/* ---- fast paths ---- */

typedef struct {
    T it, ft; /* the lane's uN and its float */
    unsigned width, mantissa, emax, precision;
} Shape;

static void shape(SnxJit* j, unsigned width, Shape* s) {
    s->width = width;
    s->it = width == 64 ? j->t_u64 : j->t_u32;
    s->ft = width == 64 ? j->t_f64 : j->t_f32;
    s->mantissa = width == 64 ? 52 : 23;
    s->emax = width == 64 ? 0x7FF : 0xFF;
    s->precision = s->mantissa + 1;
}
static V kint(SnxJit* j, const Shape* s, SnxU64 v) { return jit_k(j, s->it, v); }
static V kfloat(SnxJit* j, const Shape* s, double v) {
    return gcc_jit_context_new_rvalue_from_double(j->ctx, s->ft, v);
}
static V to_float(SnxJit* j, const Shape* s, V bits) { return jit_bitcast(j, jit_cast(j, bits, s->it), s->ft); }
static V to_bits(SnxJit* j, const Shape* s, V f) { return jit_bitcast(j, f, s->it); }
static V exponent(SnxJit* j, const Shape* s, V bits) {
    return AND(SHR(jit_cast(j, bits, s->it), kint(j, s, s->mantissa)), kint(j, s, s->emax));
}
/* lo <= exponent <= hi */
static V exponent_in(SnxJit* j, const Shape* s, V bits, unsigned lo, unsigned hi) {
    return LE(SUB(exponent(j, s, bits), kint(j, s, lo)), kint(j, s, hi - lo));
}
static V rounds_to_nearest(SnxJit* j) {
    return EQ(AND(jit_ld(j, offsetof(CPU, fpcr), j->t_u32), K32(3u << 22)), K32(0));
}
static V ixc_set(SnxJit* j) { return NE(AND(jit_ld(j, offsetof(CPU, fpsr), j->t_u32), K32(1u << 4)), K32(0)); }
static V flush_to_zero(SnxJit* j) { return NE(AND(jit_ld(j, offsetof(CPU, fpcr), j->t_u32), K32(1u << 24)), K32(0)); }
/* a math builtin of the lane's float type: name for double, namef for float */
static V math(SnxJit* j, const Shape* s, const char* name, int count, V* args) {
    char full[40];
    sprintf(full, "%s%s", name, s->width == 64 ? "" : "f");
    return jit_call(j, jit_builtin(j, full), count, args);
}
static V fma3f(SnxJit* j, const Shape* s, V a, V b, V c) {
    V args[3];
    args[0] = a, args[1] = b, args[2] = c;
    return math(j, s, "__builtin_fma", 3, args);
}
/* f != 0, true for NaN as LLVM's une */
static V nonzero(SnxJit* j, const Shape* s, V f) { return NE(f, kfloat(j, s, 0.0)); }

/* +0 or -0 */
static V is_zero(SnxJit* j, const Shape* s, V bits) {
    SnxU64 magnitude = ((SnxU64)s->emax << s->mantissa) | (((SnxU64)1 << s->mantissa) - 1);
    return EQ(AND(jit_cast(j, bits, s->it), kint(j, s, magnitude)), kint(j, s, 0));
}

void jit_fp_fast(SnxJit* j, unsigned op, unsigned width, unsigned lanes, V const* a, V const* b, V const* add,
                 SnxJitFpFast* out) {
    Shape s;
    V ok, exact_ok, all_exact, ixc, inexact = NULL;
    unsigned p, top, i;
    shape(j, width, &s);
    p = s.precision;
    top = s.emax - 1;
    ixc = jit_val(j, ixc_set(j));
    ok = rounds_to_nearest(j);
    all_exact = jit_true(j); /* every lane's IXC is known exactly: ir_fp_fast's all() of its lane conditions */
    for (i = 0; i < lanes; ++i) {
        V fa = to_float(j, &s, a[i]), fb = b ? to_float(j, &s, b[i]) : NULL, r = NULL, lane_inexact = NULL, lane_ok;
        V za = jit_val(j, is_zero(j, &s, a[i])), zb = b ? jit_val(j, is_zero(j, &s, b[i])) : NULL;
        V zero_ok = jit_true(j), lane_exact = jit_true(j);
        /* operands normal or zero, a zero result only where it is exact (ir_fp_fast says why) */
        ok = jit_land(j, ok, jit_lor(j, exponent_in(j, &s, a[i], 1, top), za));
        if (b) ok = jit_land(j, ok, jit_lor(j, exponent_in(j, &s, b[i], 1, top), zb));
        switch (op) {
            case SNX_JIT_FP_ADD:
            case SNX_JIT_FP_SUB: { /* TwoSum: the rounding error of a sum is a float, and exactly da+db */
                V y = jit_val(j, op == SNX_JIT_FP_SUB ? NEG(fb) : fb), bp, ap;
                fa = jit_val(j, fa);
                r = jit_val(j, ADD(fa, y));
                bp = jit_val(j, SUB(r, fa));
                ap = jit_val(j, SUB(r, bp));
                lane_inexact = nonzero(j, &s, ADD(SUB(fa, ap), SUB(y, bp)));
                break;
            }
            case SNX_JIT_FP_MUL:
                zero_ok = jit_lor(j, za, zb);
                r = jit_val(j, MUL(fa, fb));
                if (j->fma) { /* a*b - r, exact while the result is at least 2^(emin+p) or a zero operand's */
                    lane_inexact = nonzero(j, &s, fma3f(j, &s, fa, fb, NEG(r)));
                    lane_exact = jit_lor(j, exponent_in(j, &s, to_bits(j, &s, r), p + 2, top - 1), zero_ok);
                }
                break;
            case SNX_JIT_FP_DIV:
                ok = jit_land(j, ok, jit_lnot(j, zb)); /* by zero: DZC, or IOC for 0/0 */
                zero_ok = za;
                r = jit_val(j, DIV(fa, fb));
                if (j->fma) { /* a - r*b, exact under the same bounds on a and r, or for a zero dividend */
                    lane_inexact = nonzero(j, &s, fma3f(j, &s, NEG(r), fb, fa));
                    lane_exact = jit_lor(j,
                                         jit_land(j, exponent_in(j, &s, to_bits(j, &s, r), p + 2, top - 1),
                                                  exponent_in(j, &s, a[i], p + 2, top - 1)),
                                         za);
                }
                break;
            case SNX_JIT_FP_SQRT: /* sqrt(-0) is -0 without a flag; other negative operands are invalid */
                ok = jit_land(
                    j, ok, jit_lor(j, GE(jit_cast(j, a[i], jit_sint(j, width)), jit_k(j, jit_sint(j, width), 0)), za));
                zero_ok = za;
                r = math(j, &s, "__builtin_sqrt", 1, &fa);
                if (j->fma) { /* a - r*r */
                    lane_inexact = nonzero(j, &s, fma3f(j, &s, NEG(r), r, fa));
                    lane_exact = jit_lor(j, exponent_in(j, &s, a[i], p + 2, top - 1), za);
                }
                break;
            default: { /* FMA: add + a*b, fused; exact when the product is a zero operand's, otherwise only fast once
                          IXC is set */
                V fadd = to_float(j, &s, add[i]);
                ok = jit_land(j, ok, jit_lor(j, exponent_in(j, &s, add[i], 1, top), is_zero(j, &s, add[i])));
                zero_ok = jit_lor(j, za, zb);
                lane_exact = zero_ok;
                if (j->fma)
                    r = fma3f(j, &s, fa, fb, fadd);
                else /* no hardware FMA: never fast */
                    r = jit_val(j, ADD(fadd, MUL(fa, fb)));
                break;
            }
        }
        all_exact = jit_land(j, all_exact, lane_exact);
        out->value[i] = jit_val(j, to_bits(j, &s, r));
        /* no denormal or smallest-binade result (tininess is detected before rounding), no overflow, and a zero only
           where zero_ok says it is exact */
        lane_ok =
            jit_lor(j, exponent_in(j, &s, out->value[i], 2, top), jit_land(j, is_zero(j, &s, out->value[i]), zero_ok));
        ok = jit_land(j, ok, lane_ok);
        if (lane_inexact) inexact = inexact ? jit_lor(j, inexact, lane_inexact) : lane_inexact;
    }
    if (op == SNX_JIT_FP_ADD || op == SNX_JIT_FP_SUB)
        exact_ok = jit_true(j);
    else if (op == SNX_JIT_FP_FMA && !j->fma)
        exact_ok = jit_false(j);
    else
        exact_ok = j->fma ? jit_lor(j, ixc, all_exact) : ixc;
    out->ok = jit_val(j, jit_land(j, ok, exact_ok));
    out->inexact = inexact ? jit_val(j, inexact) : NULL;
}

gcc_jit_block* jit_fp_branch(SnxJit* j, V ok, gcc_jit_block** slow) {
    gcc_jit_block *fast = jit_block(j), *join = jit_block(j);
    *slow = jit_block(j);
    jit_branch(j, jit_expect(j, ok, 1), fast, *slow);
    jit_enter(j, fast);
    return join;
}
void jit_fp_else(SnxJit* j, gcc_jit_block* join, gcc_jit_block* slow, V inexact) {
    if (inexact) { /* fpsr |= IXC */
        V fpsr = jit_ld(j, offsetof(CPU, fpsr), j->t_u32);
        jit_st(j, offsetof(CPU, fpsr), j->t_u32, OR(fpsr, SHL(U32(inexact), K32(4))));
    }
    jit_goto(j, join);
    jit_enter(j, slow);
}
void jit_fp_join(SnxJit* j, gcc_jit_block* join) {
    jit_goto(j, join);
    jit_enter(j, join);
}

/* the helpers' prototypes in cpu.h: CPU* first, then u64 operands and int parameters */
static V fp_call(SnxJit* j, const char* name, T ret, unsigned wide, V* operands, unsigned count) {
    gcc_jit_type* p[8];
    V args[8];
    unsigned i;
    p[0] = j->t_ptr;
    args[0] = j->c;
    for (i = 0; i < count; ++i) {
        p[i + 1] = i < wide ? j->t_u64 : j->t_int;
        args[i + 1] = operands[i];
    }
    return jit_call(j, jit_extern(j, name, ret, (int)count + 1, p), (int)count + 1, args);
}
V jit_fp_binary(SnxJit* j, V a, V b, unsigned width, unsigned op) {
    V x[4];
    x[0] = a, x[1] = b, x[2] = jit_int(j, (int)width), x[3] = jit_int(j, (int)op);
    return fp_call(j, "snx_fp_binary", j->t_u64, 2, x, 4);
}
V jit_fp_fma(SnxJit* j, V add, V a, V b, unsigned width) {
    V x[4];
    x[0] = add, x[1] = a, x[2] = b, x[3] = jit_int(j, (int)width);
    return fp_call(j, "snx_fp_fma", j->t_u64, 3, x, 4);
}
V jit_fp_minmax(SnxJit* j, V a, V b, unsigned width, unsigned minimum, unsigned numeric) {
    V x[5];
    x[0] = a, x[1] = b, x[2] = jit_int(j, (int)width), x[3] = jit_int(j, (int)minimum), x[4] = jit_int(j, (int)numeric);
    return fp_call(j, "snx_fp_minmax", j->t_u64, 2, x, 5);
}
V jit_fp_unary(SnxJit* j, V bits, unsigned width, unsigned op) {
    V x[3];
    x[0] = bits, x[1] = jit_int(j, (int)width), x[2] = jit_int(j, (int)op);
    return fp_call(j, "snx_fp_unary", j->t_u64, 1, x, 3);
}
static V convert(SnxJit* j, V bits, unsigned from, unsigned to, V rm) {
    V x[4];
    x[0] = bits, x[1] = jit_int(j, (int)from), x[2] = jit_int(j, (int)to), x[3] = rm;
    return fp_call(j, "snx_fp_convert", j->t_u64, 1, x, 4);
}
static V round_to(SnxJit* j, V bits, unsigned width, V rm, unsigned exact) {
    V x[4];
    x[0] = bits, x[1] = jit_int(j, (int)width), x[2] = rm, x[3] = jit_int(j, (int)exact);
    return fp_call(j, "snx_fp_round", j->t_u64, 1, x, 4);
}
static V compare(SnxJit* j, V a, V b, unsigned width, V signal) {
    V x[4];
    x[0] = a, x[1] = b, x[2] = jit_int(j, (int)width), x[3] = signal;
    return fp_call(j, "snx_fp_compare", j->t_u32, 2, x, 4);
}
V jit_fp_to_int(SnxJit* j, V bits, unsigned fp, unsigned integer, unsigned uns, unsigned fbits, unsigned rm) {
    V x[6];
    x[0] = bits, x[1] = jit_int(j, (int)fp), x[2] = jit_int(j, (int)integer), x[3] = jit_int(j, (int)uns),
    x[4] = jit_int(j, (int)fbits), x[5] = jit_int(j, (int)rm);
    return fp_call(j, "snx_fp_to_int", j->t_u64, 1, x, 6);
}
V jit_fp_from_int(SnxJit* j, V bits, unsigned integer, unsigned fp, unsigned uns, unsigned fbits) {
    V x[5];
    x[0] = bits, x[1] = jit_int(j, (int)integer), x[2] = jit_int(j, (int)fp), x[3] = jit_int(j, (int)uns),
    x[4] = jit_int(j, (int)fbits);
    return fp_call(j, "snx_fp_from_int", j->t_u64, 1, x, 5);
}
/* FPRM: (int)((c->fpcr >> 22) & 3) */
static V fprm(SnxJit* j) {
    return jit_cast(j, AND(SHR(jit_ld(j, offsetof(CPU, fpcr), j->t_u32), K32(22)), K32(3)), j->t_int);
}
/* simd_2misc's FCVTN (FPRM) and FCVTXN (mode 5, round to odd) lane */
V jit_fp_narrow(SnxJit* j, V bits, unsigned from, unsigned odd) {
    return convert(j, bits, from, from / 2, odd ? jit_int(j, 5) : fprm(j));
}

/* SETSB/SETDB/SETH(d,e): t=(uN)(e); c->v[d].d[1]=0; c->v[d].d[0]=t; */
static void set_bits(SnxJit* j, unsigned d, unsigned bits, V value) {
    V t = jit_val(j, jit_cast(j, value, jit_uint(j, bits)));
    jit_set_vd(j, d, 1, K64(0));
    jit_set_vd(j, d, 0, U64(t));
}
/* c->v[n].s[0], .d[0] or .h[0] */
static V operand(SnxJit* j, unsigned n, unsigned width) {
    return jit_ve(j, n, width == 64 ? 3 : width == 32 ? 2 : 1, 0);
}
static V sign(SnxJit* j, V x, unsigned width) {
    T t = jit_uint(j, width);
    return XOR(jit_cast(j, x, t), jit_k(j, t, (SnxU64)1 << (width - 1)));
}
/* cond_ok(c,cond) for a constant condition (jit_a64.c has the same) */
static V condition(SnxJit* j, unsigned cond) {
    V zero = K32(0), r, n, z, cf, vf;
    unsigned k = cond >> 1;
    if (k == 7) return jit_true(j);
    n = jit_ld(j, offsetof(CPU, n), j->t_u32);
    z = jit_ld(j, offsetof(CPU, z), j->t_u32);
    cf = jit_ld(j, offsetof(CPU, cf), j->t_u32);
    vf = jit_ld(j, offsetof(CPU, vf), j->t_u32);
    switch (k) {
        case 0: r = NE(z, zero); break;
        case 1: r = NE(cf, zero); break;
        case 2: r = NE(n, zero); break;
        case 3: r = NE(vf, zero); break;
        case 4: r = jit_land(j, NE(cf, zero), EQ(z, zero)); break;
        case 5: r = EQ(n, vf); break;
        default: r = jit_land(j, EQ(z, zero), EQ(n, vf)); break;
    }
    return jit_val(j, cond & 1 ? jit_lnot(j, r) : r);
}
/* set_nzcv(c,f) */
static void nzcv_from(SnxJit* j, V f) {
    V one = K32(1);
    f = jit_val(j, U32(f));
    jit_st(j, offsetof(CPU, n), j->t_u32, AND(SHR(f, K32(3)), one));
    jit_st(j, offsetof(CPU, z), j->t_u32, AND(SHR(f, K32(2)), one));
    jit_st(j, offsetof(CPU, cf), j->t_u32, AND(SHR(f, one), one));
    jit_st(j, offsetof(CPU, vf), j->t_u32, AND(f, one));
}

static int fp2(SnxJit* j, SnxU32 w, size_t index) {
    unsigned op = index & 15, width = index >> 4 ? 64 : 32, rd = F(0, 5);
    V a = operand(j, F(5, 5), width), b = operand(j, F(16, 5), width), r;
    static const unsigned binary_ops[4] = {2, 3, 0, 1}; /* MUL, DIV, ADD, SUB */
    if (op < 4 || op == 8) {                            /* MUL, DIV, ADD, SUB, FNMUL: inline when exact */
        static const unsigned fast_ops[4] = {SNX_JIT_FP_MUL, SNX_JIT_FP_DIV, SNX_JIT_FP_ADD, SNX_JIT_FP_SUB};
        gcc_jit_block *slow, *join;
        SnxJitFpFast f;
        jit_fp_fast(j, op == 8 ? SNX_JIT_FP_MUL : fast_ops[op], width, 1, &a, &b, NULL, &f);
        join = jit_fp_branch(j, f.ok, &slow);
        set_bits(j, rd, width, op == 8 ? sign(j, f.value[0], width) : f.value[0]);
        jit_fp_else(j, join, slow, f.inexact);
        if (op == 8)
            r = sign(j, jit_fp_binary(j, a, b, width, 2), width);
        else
            r = jit_fp_binary(j, a, b, width, binary_ops[op]);
        set_bits(j, rd, width, r);
        jit_fp_join(j, join);
        return 1;
    }
    if (op < 8) { /* FMAX, FMIN, FMAXNM, FMINNM: inline without a NaN (or a denormal under FZ) */
        gcc_jit_block *slow, *join;
        SnxJitFpFast f;
        jit_fp_minmax_fast(j, width, 1, &a, &b, op & 1, &f);
        join = jit_fp_branch(j, f.ok, &slow);
        set_bits(j, rd, width, f.value[0]);
        jit_fp_else(j, join, slow, f.inexact);
        set_bits(j, rd, width, jit_fp_minmax(j, a, b, width, op & 1, (op >> 1) & 1));
        jit_fp_join(j, join);
        return 1;
    }
    return 0;
}

static int fp1(SnxJit* j, SnxU32 w, size_t index) {
    unsigned op = index & 63, type = (unsigned)(index >> 6), rd = F(0, 5), rn = F(5, 5);
    unsigned width = type == 0 ? 32 : type == 1 ? 64 : 16;
    V a = operand(j, rn, width), r;
    if (type == 3) { /* FCVT from half */
        if (op != 4 && op != 5) return 0;
        set_bits(j, rd, op == 4 ? 32 : 64, convert(j, a, 16, op == 4 ? 32 : 64, fprm(j)));
        return 1;
    }
    if (type > 1) return 0;
    switch (op) {
        case 0: r = a; break;
        case 1: r = AND(a, jit_k(j, jit_uint(j, width), ~((SnxU64)1 << (width - 1)))); break;
        case 2: r = sign(j, a, width); break;
        case 3: { /* SNX_FP_SQRT, inline when exact */
            gcc_jit_block *slow, *join;
            SnxJitFpFast f;
            jit_fp_fast(j, SNX_JIT_FP_SQRT, width, 1, &a, NULL, NULL, &f);
            join = jit_fp_branch(j, f.ok, &slow);
            set_bits(j, rd, width, f.value[0]);
            jit_fp_else(j, join, slow, f.inexact);
            set_bits(j, rd, width, jit_fp_unary(j, a, width, 0));
            jit_fp_join(j, join);
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
                V f, converted, ok, inexact;
                gcc_jit_block *slow, *join;
                shape(j, width, &from);
                shape(j, to, &into);
                f = jit_val(j, to_float(j, &from, a));
                converted = jit_val(j, jit_cast(j, f, into.ft));
                if (to == 64) {
                    ok = jit_lor(j, exponent_in(j, &from, a, 1, from.emax - 1),
                                 EQ(SHL(jit_cast(j, a, from.it), kint(j, &from, 1)), kint(j, &from, 0)));
                    inexact = NULL;
                } else {
                    ok = jit_land(j, jit_land(j, rounds_to_nearest(j), exponent_in(j, &from, a, 1, from.emax - 1)),
                                  exponent_in(j, &into, to_bits(j, &into, converted), 2, into.emax - 1));
                    inexact = NE(jit_cast(j, converted, from.ft), f);
                }
                join = jit_fp_branch(j, jit_val(j, ok), &slow);
                set_bits(j, rd, to, to_bits(j, &into, converted));
                jit_fp_else(j, join, slow, inexact);
                set_bits(j, rd, to, convert(j, a, width, to, fprm(j)));
                jit_fp_join(j, join);
                return 1;
            }
            set_bits(j, rd, to, convert(j, a, width, to, fprm(j)));
            return 1;
        }
        case 8:
        case 9:
        case 10:
        case 11:
        case 12: r = round_to(j, a, width, jit_int(j, (int)op - 8), 0); break;
        case 14: r = round_to(j, a, width, fprm(j), 1); break;
        case 15: r = round_to(j, a, width, fprm(j), 0); break;
        default: return 0;
    }
    set_bits(j, rd, width, r);
    return 1;
}

/* FCMP without a NaN, and without a denormal when FZ is set (flushing it sets IDC): the host comparison gives the
   flags, and no FPSR bit changes */
/* a denormal: exponent 0 and a nonzero fraction (zero has exponent 0 too, and flushing it changes nothing) */
static V denormal(SnxJit* j, const Shape* s, V bits) {
    return jit_land(j, jit_lnot(j, exponent_in(j, s, bits, 1, s->emax)), jit_lnot(j, is_zero(j, s, bits)));
}
/* in every lane neither operand a NaN, and FZ clear or no operand a denormal: fp_input then changes nothing and sets
   no IDC, and only a NaN can raise IOC (ir_fp.c's no_nan_no_flush) */
static V no_nan_no_flush(SnxJit* j, const Shape* s, unsigned lanes, V const* a, V const* b, V const* fa, V const* fb) {
    V nan = jit_false(j), denormals = jit_false(j);
    unsigned i;
    for (i = 0; i < lanes; ++i) {
        nan = jit_lor(j, nan, jit_lor(j, NE(fa[i], fa[i]), NE(fb[i], fb[i])));
        denormals = jit_lor(j, denormals, jit_lor(j, denormal(j, s, a[i]), denormal(j, s, b[i])));
    }
    return jit_val(j, jit_lnot(j, jit_lor(j, nan, jit_land(j, flush_to_zero(j), denormals))));
}
static V compare_fast(SnxJit* j, V a, V b, unsigned width, V* ok) {
    Shape s;
    V fa, fb, nzcv;
    shape(j, width, &s);
    fa = jit_val(j, to_float(j, &s, a));
    fb = jit_val(j, to_float(j, &s, b));
    *ok = no_nan_no_flush(j, &s, 1, &a, &b, &fa, &fb);
    nzcv = jit_sel(j, LT(fa, fb), K32(8), K32(2));
    return jit_sel(j, EQ(fa, fb), K32(6), nzcv);
}

void jit_fp_minmax_fast(SnxJit* j, unsigned width, unsigned lanes, V const* a, V const* b, unsigned minimum,
                        SnxJitFpFast* out) {
    Shape s;
    V fa[4], fb[4];
    unsigned i;
    shape(j, width, &s);
    for (i = 0; i < lanes; ++i) {
        V x = jit_cast(j, a[i], s.it), y = jit_cast(j, b[i], s.it), gt, lt, tie;
        fa[i] = jit_val(j, to_float(j, &s, x));
        fb[i] = jit_val(j, to_float(j, &s, y));
        gt = LT(fb[i], fa[i]);
        lt = LT(fa[i], fb[i]);
        /* neither larger: equal, so the same bits unless they are zeros of different signs */
        tie = minimum ? OR(x, y) : AND(x, y);
        out->value[i] =
            jit_val(j, minimum ? jit_sel(j, lt, x, jit_sel(j, gt, y, tie)) : jit_sel(j, gt, x, jit_sel(j, lt, y, tie)));
    }
    out->ok = no_nan_no_flush(j, &s, lanes, a, b, fa, fb);
    out->inexact = NULL;
}

void jit_fp_compare_fast(SnxJit* j, unsigned cmp, unsigned width, unsigned lanes, V const* a, V const* b,
                         SnxJitFpFast* out) {
    Shape s;
    V fa[4], fb[4];
    unsigned i;
    shape(j, width, &s);
    for (i = 0; i < lanes; ++i) {
        V holds;
        fa[i] = jit_val(j, to_float(j, &s, a[i]));
        fb[i] = jit_val(j, to_float(j, &s, b[i]));
        switch (cmp) {
            case SNX_JIT_CMP_EQ: holds = EQ(fa[i], fb[i]); break;
            case SNX_JIT_CMP_GE: holds = GE(fa[i], fb[i]); break;
            case SNX_JIT_CMP_GT: holds = LT(fb[i], fa[i]); break;
            case SNX_JIT_CMP_LE: holds = LE(fa[i], fb[i]); break;
            default: holds = LT(fa[i], fb[i]); break;
        }
        out->value[i] = jit_val(j, jit_sel(j, holds, kint(j, &s, ~(SnxU64)0), kint(j, &s, 0)));
    }
    out->ok = no_nan_no_flush(j, &s, lanes, a, b, fa, fb);
    out->inexact = NULL;
}

/* fcmpmask: snx_fp_compare's NZCV (8 less, 6 equal, 2 greater, 3 unordered) tested for the predicate */
V jit_fp_compare_mask(SnxJit* j, unsigned cmp, V a, V b, unsigned width) {
    Shape s;
    V flags = compare(j, a, b, width, jit_int(j, cmp != SNX_JIT_CMP_EQ)), holds;
    shape(j, width, &s);
    switch (cmp) {
        case SNX_JIT_CMP_EQ: holds = EQ(flags, K32(6)); break;
        case SNX_JIT_CMP_GE: holds = jit_lor(j, EQ(flags, K32(6)), EQ(flags, K32(2))); break;
        case SNX_JIT_CMP_GT: holds = EQ(flags, K32(2)); break;
        case SNX_JIT_CMP_LE: holds = jit_lor(j, EQ(flags, K32(8)), EQ(flags, K32(6))); break;
        default: holds = EQ(flags, K32(8)); break;
    }
    return jit_val(j, jit_sel(j, holds, kint(j, &s, ~(SnxU64)0), kint(j, &s, 0)));
}

static void fcmp_flags(SnxJit* j, V a, V b, unsigned width, V signal) {
    V ok, fast = compare_fast(j, a, b, width, &ok);
    gcc_jit_block *slow, *join = jit_fp_branch(j, ok, &slow);
    nzcv_from(j, fast);
    jit_fp_else(j, join, slow, NULL);
    nzcv_from(j, compare(j, a, b, width, signal));
    jit_fp_join(j, join);
}

static void fcmp(SnxJit* j, SnxU32 w, size_t index) {
    unsigned width = index >> 2 ? 64 : 32;
    V b = index & 1 ? jit_k(j, jit_uint(j, width), 0) : operand(j, F(16, 5), width), a = operand(j, F(5, 5), width);
    fcmp_flags(j, a, b, width, jit_int(j, (int)((index >> 1) & 1)));
}

static void fccmp(SnxJit* j, SnxU32 w, size_t index) {
    unsigned width = index ? 64 : 32;
    gcc_jit_block *taken = jit_block(j), *flags = jit_block(j), *done = jit_block(j);
    jit_branch(j, condition(j, F(12, 4)), taken, flags);
    jit_enter(j, taken);
    fcmp_flags(j, operand(j, F(5, 5), width), operand(j, F(16, 5), width), width, jit_int(j, (int)F(4, 1)));
    jit_goto(j, done);
    jit_enter(j, flags);
    nzcv_from(j, K32(F(0, 4)));
    jit_goto(j, done);
    jit_enter(j, done);
}

static void fcsel(SnxJit* j, SnxU32 w, size_t index) {
    unsigned width = index ? 64 : 32;
    V ok = condition(j, F(12, 4));
    set_bits(j, F(0, 5), width, jit_sel(j, ok, operand(j, F(5, 5), width), operand(j, F(16, 5), width)));
}

/* vfp_imm32/vfp_imm64(imm8), evaluated here */
static SnxU64 vfp_imm(unsigned i, int wide) {
    SnxU64 b6 = (i >> 6) & 1, e, sign = i >> 7;
    e = ((b6 ^ 1) << (wide ? 10 : 7)) | ((b6 ? (wide ? 0xFF : 0x1F) : 0) << 2) | ((i >> 4) & 3);
    return (sign << (wide ? 63 : 31)) | (e << (wide ? 52 : 23)) | ((SnxU64)(i & 15) << (wide ? 48 : 19));
}

static void fmov_imm(SnxJit* j, SnxU32 w, size_t index) {
    set_bits(j, F(0, 5), index ? 64 : 32, K64(vfp_imm(F(13, 8), index != 0)));
}

static void fp3(SnxJit* j, SnxU32 w, size_t index) {
    unsigned o0 = index & 1, o1 = (index >> 1) & 1, width = index >> 2 ? 64 : 32;
    V a = operand(j, F(10, 5), width), n = operand(j, F(5, 5), width), m = operand(j, F(16, 5), width);
    /* FMADD, FMSUB (-n), FNMADD (-a, -n), FNMSUB (-a) */
    gcc_jit_block *slow, *join;
    SnxJitFpFast f;
    if (o1) a = jit_val(j, sign(j, a, width));
    if (o0 != o1) n = jit_val(j, sign(j, n, width));
    jit_fp_fast(j, SNX_JIT_FP_FMA, width, 1, &n, &m, &a, &f);
    join = jit_fp_branch(j, f.ok, &slow);
    set_bits(j, F(0, 5), width, f.value[0]);
    jit_fp_else(j, join, slow, f.inexact);
    set_bits(j, F(0, 5), width, jit_fp_fma(j, a, n, m, width));
    jit_fp_join(j, join);
}

/* 2^e, exactly (|e| <= 64 here) */
static double pow2(int e) {
    double r = 1.0;
    for (; e > 0; --e) r *= 2.0;
    for (; e < 0; ++e) r *= 0.5;
    return r;
}

/* ir_fp_to_int_fast: round with the instruction's mode, convert, inexact when rounding changed the value. Fast for
   finite values in range (no IOC), denormals only without FZ (flushing sets IDC) */
int jit_fp_to_int_fast(SnxJit* j, unsigned width, unsigned lanes, V const* bits, unsigned integer, unsigned uns,
                       unsigned rm, unsigned fbits, SnxJitFpFast* out) {
    static const char* const rounding[5] = {"__builtin_roundeven", "__builtin_ceil", "__builtin_floor", NULL,
                                            "__builtin_round"};
    Shape s;
    V scaled[4], rounded[4], limit, low, ok, normal, inexact;
    gcc_jit_lvalue* r[4];
    gcc_jit_block *convert, *done;
    T it = uns ? jit_uint(j, integer) : jit_sint(j, integer);
    double top = integer == 64 ? 9223372036854775808.0 : 2147483648.0;
    unsigned i;
    if (rm != 3 && !j->round) return 0;
    shape(j, width, &s);
    if (uns) {
        limit = kfloat(j, &s, top * 2);
        low = kfloat(j, &s, 0.0); /* -0 compares equal: FCVTPU(-0.3) is 0, inexact */
    } else {
        limit = kfloat(j, &s, top);
        low = kfloat(j, &s, -top);
    }
    ok = jit_true(j);
    normal = jit_true(j);
    for (i = 0; i < lanes; ++i) {
        V e = exponent(j, &s, bits[i]),
          mantissa = AND(jit_cast(j, bits[i], s.it), kint(j, &s, ((SnxU64)1 << s.mantissa) - 1));
        scaled[i] = jit_val(j, to_float(j, &s, bits[i]));
        /* scaling by 2^fbits is exact, or overflows to an infinity, which is out of range */
        if (fbits) scaled[i] = jit_val(j, MUL(scaled[i], kfloat(j, &s, pow2((int)fbits))));
        rounded[i] = rm == 3 ? scaled[i] : math(j, &s, rounding[rm], 1, &scaled[i]); /* the conversion truncates */
        ok = jit_land(j, ok,
                      jit_land(j, NE(e, kint(j, &s, s.emax)), jit_land(j, GE(rounded[i], low), LT(rounded[i], limit))));
        normal = jit_land(j, normal, jit_lor(j, NE(e, kint(j, &s, 0)), EQ(mantissa, kint(j, &s, 0))));
    }
    ok = jit_val(j, jit_land(j, ok, jit_lor(j, jit_lnot(j, flush_to_zero(j)), normal)));
    /* converted only where every lane is in range: out of range the C conversion is undefined, and GCC may assume it
       isn't */
    convert = jit_block(j);
    done = jit_block(j);
    for (i = 0; i < lanes; ++i) {
        r[i] = jit_local(j, it);
        jit_assign(j, r[i], jit_k(j, it, 0));
    }
    jit_branch(j, ok, convert, done);
    jit_enter(j, convert);
    for (i = 0; i < lanes; ++i) jit_assign(j, r[i], jit_cast(j, rounded[i], it));
    jit_goto(j, done);
    jit_enter(j, done);
    inexact = jit_false(j);
    for (i = 0; i < lanes; ++i) {
        V result = jit_val(j, gcc_jit_lvalue_as_rvalue(r[i]));
        /* the conversion rounded: the integer, converted back (exact while it is in range) */
        V back = rm == 3 ? jit_cast(j, result, s.ft) : rounded[i];
        inexact = jit_lor(j, inexact, NE(back, scaled[i]));
        out->value[i] = jit_val(j, jit_cast(j, result, jit_uint(j, integer)));
    }
    out->ok = ok;
    out->inexact = jit_val(j, inexact);
    return 1;
}

/* ir_fp_from_int_fast: exact for magnitudes up to 2^precision; with IXC set the host's rounding suffices. Scaling by
   2^-fbits is exact: the smallest nonzero result, 2^-64, is a normal number */
void jit_fp_from_int_fast(SnxJit* j, unsigned width, unsigned lanes, V const* v, unsigned integer, unsigned uns,
                          unsigned fbits, SnxJitFpFast* out) {
    Shape s;
    SnxU64 p;
    V small = jit_true(j);
    unsigned i;
    shape(j, width, &s);
    p = (SnxU64)1 << s.precision;
    for (i = 0; i < lanes; ++i) {
        V x, f;
        if (uns) {
            x = jit_val(j, jit_cast(j, v[i], jit_uint(j, integer)));
            small = jit_land(j, small, LE(U64(x), K64(p)));
        } else {
            x = jit_val(j, jit_cast(j, v[i], jit_sint(j, integer)));
            small = jit_land(j, small, LE(ADD(U64(jit_cast(j, x, j->t_s64)), K64(p)), K64(2 * p)));
        }
        f = jit_cast(j, x, s.ft);
        if (fbits) f = MUL(f, kfloat(j, &s, pow2(-(int)fbits)));
        out->value[i] = jit_val(j, to_bits(j, &s, f));
    }
    out->ok = jit_val(j, jit_land(j, rounds_to_nearest(j), jit_lor(j, small, ixc_set(j))));
    out->inexact = NULL;
}

/* ir_fp_estimate_fast, a lane at a time (ir_fp.c says why the float division gives recip_est's quotient exactly) */
void jit_fp_estimate_fast(SnxJit* j, unsigned rsqrt, unsigned width, unsigned lanes, V const* bits, SnxJitFpFast* out) {
    Shape s;
    V ok = jit_true(j);
    unsigned i, k, f, bias;
    shape(j, width, &s);
    f = s.mantissa;
    bias = s.emax >> 1;
    for (i = 0; i < lanes; ++i) {
        V x = jit_val(j, jit_cast(j, bits[i], s.it)), e = jit_val(j, exponent(j, &s, x)), est, power;
        if (!rsqrt) { /* (2^19 / (513 + 2t) + 1) >> 1 */
            V t = jit_cast(j, AND(SHR(x, kint(j, &s, f - 8)), kint(j, &s, 255)), j->t_u32);
            V d = jit_cast(j, ADD(SHL(t, K32(1)), K32(513)), j->t_f32);
            est = jit_cast(j, DIV(gcc_jit_context_new_rvalue_from_double(j->ctx, j->t_f32, 524288.0), d), j->t_u32);
            est = SHR(ADD(est, K32(1)), K32(1));
            ok = jit_land(j, ok, exponent_in(j, &s, x, 1, s.emax - 3));
            power =
                OR(AND(x, kint(j, &s, (SnxU64)1 << (width - 1))), SHL(SUB(kint(j, &s, s.emax - 2), e), kint(j, &s, f)));
        } else { /* rsqrt_est of 257 + 2t (odd exponent) or 514 + 4t (even), t the fraction's top 7 bits */
            V t = jit_val(j, jit_cast(j, AND(SHR(x, kint(j, &s, f - 7)), kint(j, &s, 127)), j->t_u32));
            V odd = NE(AND(e, kint(j, &s, 1)), kint(j, &s, 0));
            V a = jit_val(j, jit_sel(j, odd, ADD(SHL(t, K32(1)), K32(257)), ADD(SHL(t, K32(2)), K32(514))));
            V lo = K32(512), hi = K32(1024);
            for (k = 0; k < 9; ++k) {
                V mid = jit_val(j, SHR(ADD(lo, hi), K32(1)));
                V below = jit_val(j, LT(MUL(MUL(a, mid), mid), K32(1u << 28)));
                lo = jit_val(j, jit_sel(j, below, mid, lo));
                hi = jit_val(j, jit_sel(j, below, hi, mid));
            }
            est = SHR(ADD(lo, K32(1)), K32(1));
            ok = jit_land(
                j, ok,
                jit_land(j, exponent_in(j, &s, x, 1, s.emax - 1), EQ(SHR(x, kint(j, &s, width - 1)), kint(j, &s, 0))));
            power = SHL(SHR(SUB(kint(j, &s, 3 * bias - 1), e), kint(j, &s, 1)), kint(j, &s, f));
        }
        out->value[i] = jit_val(j, OR(power, SHL(jit_cast(j, AND(est, K32(255)), s.it), kint(j, &s, f - 8))));
    }
    out->ok = jit_val(j, ok);
    out->inexact = NULL;
}

/* ir_fp_step_fast: 2 + (-a)*b or (3 + (-a)*b) / 2 under jit_fp_fast's FMA guard, the half kept out of the smallest
   binade */
void jit_fp_step_fast(SnxJit* j, unsigned rsqrt, unsigned width, unsigned lanes, V const* a, V const* b,
                      SnxJitFpFast* out) {
    Shape s;
    V negated[4], add[4];
    SnxU64 two;
    unsigned i;
    shape(j, width, &s);
    two = (SnxU64)((s.emax >> 1) + 1) << s.mantissa;
    for (i = 0; i < lanes; ++i) {
        negated[i] = jit_val(j, XOR(jit_cast(j, a[i], s.it), kint(j, &s, (SnxU64)1 << (width - 1))));
        add[i] = kint(j, &s, two | (rsqrt ? (SnxU64)1 << (s.mantissa - 1) : 0));
    }
    jit_fp_fast(j, SNX_JIT_FP_FMA, width, lanes, negated, b, add, out);
    if (rsqrt) {
        V ok = out->ok;
        for (i = 0; i < lanes; ++i) {
            V half = jit_val(j, to_bits(j, &s, MUL(to_float(j, &s, out->value[i]), kfloat(j, &s, 0.5))));
            ok = jit_land(j, ok, exponent_in(j, &s, half, 2, s.emax - 1));
            out->value[i] = half;
        }
        out->ok = jit_val(j, ok);
    }
}

/* ir_fp_narrow_fast's integer narrowing (ir_fp.c says why it is fp_pack's result), a lane at a time */
void jit_fp_narrow_fast(SnxJit* j, unsigned from, unsigned lanes, V const* bits, unsigned odd, SnxJitFpFast* out) {
    Shape s;
    unsigned to = from / 2, narrow_mantissa = from == 64 ? 23 : 10, narrow_emax = from == 64 ? 0xFF : 0x1F;
    unsigned shift, rebias, i;
    V ok = jit_true(j), lost = jit_false(j);
    shape(j, from, &s);
    shift = s.mantissa - narrow_mantissa;
    rebias = (s.emax >> 1) - (narrow_emax >> 1); /* 1023 - 127, 127 - 15 */
    for (i = 0; i < lanes; ++i) {
        V x = jit_val(j, jit_cast(j, bits[i], s.it)), sign, magnitude, truncated, dropped, r;
        sign = jit_val(j, AND(x, kint(j, &s, (SnxU64)1 << (from - 1))));
        magnitude = jit_val(j, XOR(x, sign));
        ok = jit_land(j, ok, exponent_in(j, &s, x, rebias + 1, rebias + narrow_emax - 1));
        truncated = jit_val(j, SUB(SHR(magnitude, kint(j, &s, shift)), kint(j, &s, (SnxU64)rebias << narrow_mantissa)));
        dropped = jit_val(j, AND(magnitude, kint(j, &s, ((SnxU64)1 << shift) - 1)));
        lost = jit_lor(j, lost, NE(dropped, kint(j, &s, 0)));
        if (odd)
            r = OR(truncated, jit_sel(j, NE(dropped, kint(j, &s, 0)), kint(j, &s, 1), kint(j, &s, 0)));
        else {
            V half = kint(j, &s, (SnxU64)1 << (shift - 1));
            V up = jit_lor(j, LT(half, dropped),
                           jit_land(j, EQ(dropped, half), NE(AND(truncated, kint(j, &s, 1)), kint(j, &s, 0))));
            r = jit_val(j, ADD(truncated, jit_sel(j, up, kint(j, &s, 1), kint(j, &s, 0))));
            ok = jit_land(j, ok, LT(r, kint(j, &s, (SnxU64)narrow_emax << narrow_mantissa)));
        }
        out->value[i] = jit_val(j, jit_cast(j, OR(r, SHR(sign, kint(j, &s, from - to))), jit_uint(j, to)));
    }
    out->ok = jit_val(j, odd ? ok : jit_land(j, rounds_to_nearest(j), ok));
    out->inexact = jit_val(j, lost);
}

/* FCVT_INT and FCVT_FIX: index = opcode | rmode << 3 | ftype << 5 | sf << 7 */
static int fcvt(SnxJit* j, SnxU32 w, size_t index, int fixed) {
    unsigned opcode = index & 7, rmode = (index >> 3) & 3, type = (index >> 5) & 3, sf = (unsigned)(index >> 7);
    unsigned rd = F(0, 5), rn = F(5, 5), integer = sf ? 64 : 32, width = type == 0 ? 32 : type == 1 ? 64 : 16;
    unsigned fbits = fixed ? 64 - F(10, 6) : 0;
    gcc_jit_block *slow, *join;
    SnxJitFpFast f;
    switch (opcode) {
        case 0:
        case 1:
        case 4:
        case 5:
            if (type > 1) return 0;
            {
                unsigned rm = opcode >= 4 ? 4 : rmode;
                V a = operand(j, rn, width);
                if (!jit_fp_to_int_fast(j, width, 1, &a, integer, opcode & 1, rm, fbits, &f)) {
                    jit_setx(j, rd, jit_fp_to_int(j, a, width, integer, opcode & 1, fbits, rm));
                    return 1;
                }
                join = jit_fp_branch(j, f.ok, &slow);
                jit_setx(j, rd, U64(f.value[0])); /* the helper masks a 32-bit result to 32 bits */
                jit_fp_else(j, join, slow, f.inexact);
                jit_setx(j, rd, jit_fp_to_int(j, a, width, integer, opcode & 1, fbits, rm));
                jit_fp_join(j, join);
            }
            return 1;
        case 2:
        case 3:
            if (type > 1) return 0;
            {
                V x = jit_rx(j, rn);
                jit_fp_from_int_fast(j, width, 1, &x, integer, opcode & 1, fbits, &f);
                join = jit_fp_branch(j, f.ok, &slow);
                set_bits(j, rd, width, f.value[0]);
                jit_fp_else(j, join, slow, f.inexact);
                set_bits(j, rd, width, jit_fp_from_int(j, x, integer, width, opcode & 1, fbits));
                jit_fp_join(j, join);
            }
            return 1;
        case 6: /* FMOV to a general register */
            if (type == 2)
                jit_setx(j, rd, jit_vd(j, rn, 1));
            else
                jit_setx(j, rd, U64(operand(j, rn, width)));
            return 1;
        case 7: /* FMOV from a general register */
            if (type == 2)
                jit_set_vd(j, rd, 1, jit_rx(j, rn));
            else
                set_bits(j, rd, width, width == 64 ? jit_rx(j, rn) : U32(jit_rx(j, rn)));
            return 1;
    }
    return 0;
}

int snx_jit_fp_plain(SnxJit* j, SnxU32 w, SnxU64 pc, const SnxA64Pattern* pattern, size_t index) {
    const char* name = pattern->name;
    (void)pc;
    if (!strcmp(name, "FP2")) return fp2(j, w, index);
    if (!strcmp(name, "FP1")) return fp1(j, w, index);
    if (!strcmp(name, "FCMP"))
        fcmp(j, w, index);
    else if (!strcmp(name, "FCCMP"))
        fccmp(j, w, index);
    else if (!strcmp(name, "FCSEL"))
        fcsel(j, w, index);
    else if (!strcmp(name, "FMOV_IMM"))
        fmov_imm(j, w, index);
    else if (!strcmp(name, "FP3"))
        fp3(j, w, index);
    else if (!strcmp(name, "FCVT_INT"))
        return fcvt(j, w, index, 0);
    else if (!strcmp(name, "FCVT_FIX"))
        return fcvt(j, w, index, 1);
    else
        return 0;
    return 1;
}
