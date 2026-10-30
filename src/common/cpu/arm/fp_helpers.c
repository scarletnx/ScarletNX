/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate.c, vec_helper.c, vfp_helper.c, crypto_helper.c),
   LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd.
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org>
   The included aarch64/helpers/fp.c and fp_arithmetic.c carry their own notices (QEMU's softfloat and
   target/arm, LGPL-2.1-or-later and the SoftFloat-2a, BSD and GPL-2.0-or-later portions of softfloat). */
#include "common/cpu/arm/simd.h"
#include <stdbool.h>
#include <string.h>
/* Share the integer implementation of ARM's FP pseudocode, not the A64 register ABI. Namespace this copy so both
   runtime libraries can coexist. */
#define snx_fp_binary   snx_arm_soft_binary
#define snx_fp_fma      snx_arm_soft_fma
#define snx_fp_unary    snx_arm_soft_unary
#define snx_fp_convert  snx_arm_soft_convert
#define snx_fp_to_int   snx_arm_soft_to_int
#define snx_fp_from_int snx_arm_soft_from_int
#define snx_fp_compare  snx_arm_soft_compare
#define snx_fp_minmax   snx_arm_soft_minmax
#define snx_fp_round    snx_arm_soft_round
#define snx_fp_round_int snx_arm_soft_round_int
#define snx_fp_jcvt     snx_arm_soft_jcvt
#define snx_fp_bfcvt    snx_arm_soft_bfcvt
#define urecpe32        snx_arm_soft_urecpe32
#define ursqrte32       snx_arm_soft_ursqrte32
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/aarch64/helpers/fp.c"

/* AArch32 FPRecipStep/FPRSqrtStep round the product BEFORE subtraction.
   A64's fused step helpers are not interchangeable with these operations. */
static u64 step(CPU* c, u64 x, u64 y, int half) {
    FpValue a = fp_unpack(c, x, 32), b = fp_unpack(c, y, 32);
    if (a.kind >= FPV_QNAN || b.kind >= FPV_QNAN) return fp_nan(c, fp_nan2(a, b), 32);
    int special = (a.kind == FPV_INF && b.kind == FPV_ZERO) || (b.kind == FPV_INF && a.kind == FPV_ZERO);
    u64 product = special ? 0 : snx_fp_binary(c, x, y, 32, SNX_FP_MUL);
    if (!half) return snx_fp_binary(c, 0x40000000, product, 32, SNX_FP_SUB);
    /* FPHalvedSub(3, product): a single rounding of the exact difference / 2. */
    return fp_fused(c, 0x40400000, product ^ 0x80000000u, 0x3f800000, 32, -1);
}
/* ---- the host's floating point where it gives ARM's bits ----
   IEEE 754's basic operations round the same on every IEEE host (square roots and fused products aren't taken:
   the game modules link no maths library, and x86-64's baseline has no fused multiply): with round to nearest, operands and results that
   are finite and normal (or zero), no trap enabled, and the sticky inexact flag already set (the only flag such an
   operation could raise), the host's result is ARM's and leaves FPSCR as ARM would. Flush to zero and default NaN
   don't apply then: no denormal or NaN is involved. Anything else takes the exact software path below. Each
   operation is a statement of its own and contraction is off: a product rounds before it is added, as VMLA's */
#pragma STDC FP_CONTRACT OFF
static float f32_of(uint64_t v) {
    uint32_t b = (uint32_t)v;
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}
static uint64_t bits_f32(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}
static double f64_of(uint64_t v) {
    double d;
    memcpy(&d, &v, sizeof d);
    return d;
}
static uint64_t bits_f64(double d) {
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return b;
}
/* a finite value that is normal or a zero (zero only when allowed: a product's or quotient's zero may have
   underflowed) */
static bool plain(uint64_t v, uint32_t width, bool zero) {
    uint64_t e = width == 32 ? (v >> 23) & 0xff : (v >> 52) & 0x7ff, top = width == 32 ? 0xff : 0x7ff;
    uint64_t m = width == 32 ? v & 0x7fffff : v & 0xfffffffffffffull;
    return e != top && (e != 0 || (m == 0 && zero));
}
static bool host_add(uint64_t x, uint64_t y, uint32_t width, int sub, uint64_t* out) {
    if (width == 32) {
        float a = f32_of(x), b = f32_of(y);
        float r = sub ? a - b : a + b;
        *out = bits_f32(r);
    } else {
        double a = f64_of(x), b = f64_of(y);
        double r = sub ? a - b : a + b;
        *out = bits_f64(r);
    }
    return plain(*out, width, true); /* a sum of normals is zero only exactly */
}
static bool host_mul_div(uint64_t x, uint64_t y, uint32_t width, int div, uint64_t* out) {
    if (width == 32) {
        float a = f32_of(x), b = f32_of(y);
        if (div && b == 0.0f) return false;
        float r = div ? a / b : a * b;
        *out = bits_f32(r);
    } else {
        double a = f64_of(x), b = f64_of(y);
        if (div && b == 0.0) return false;
        double r = div ? a / b : a * b;
        *out = bits_f64(r);
    }
    /* zero only from a zero operand (else it underflowed) */
    return plain(*out, width, (x & ~(1ull << (width - 1))) == 0 || (!div && (y & ~(1ull << (width - 1))) == 0));
}
static bool host_fp(const CPU* c, uint32_t ixc, uint64_t x, uint64_t y, uint64_t add, uint32_t op, uint32_t width,
                    uint32_t param, uint64_t* out) {
    uint64_t sign = 1ull << (width - 1), product;
    if ((width != 32 && width != 64) || (c->fpcr & 0x00c09f00u)) return false; /* rounding mode, trap enables */
    switch (op) {
        case AV_FCMP:
        case AV_FCMPE: /* no flag, ordered */
            if (!plain(x, width, true) || !plain(y, width, true)) return false;
            if (width == 32) {
                float a = f32_of(x), b = f32_of(y);
                *out = a < b ? 8 : a == b ? 6 : 2;
            } else {
                double a = f64_of(x), b = f64_of(y);
                *out = a < b ? 8 : a == b ? 6 : 2;
            }
            return true;
        default: break;
    }
    if (!ixc) return false;
    switch (op) {
        case AV_FADD:
        case AV_FSUB:
            return plain(x, width, true) && plain(y, width, true) && host_add(x, y, width, op == AV_FSUB, out);
        case AV_FMUL:
        case AV_FDIV:
            return plain(x, width, true) && plain(y, width, true) && host_mul_div(x, y, width, op == AV_FDIV, out);
        case AV_FMLA:
        case AV_FMLS:
        case AV_FNMLA:
        case AV_FNMLS:
        case AV_FNMUL:
            if (!plain(x, width, true) || !plain(y, width, true) || !host_mul_div(x, y, width, 0, &product))
                return false;
            if (op == AV_FMLS || op == AV_FNMLA || op == AV_FNMUL) product ^= sign;
            if (op == AV_FNMUL) {
                *out = product;
                return true;
            }
            if (op == AV_FNMLA || op == AV_FNMLS) add ^= sign;
            return plain(add, width, true) && host_add(add, product, width, 0, out);
        case AV_ITOF: { /* a 32-bit integer, no fraction bits */
            unsigned iw = (param >> 8) & 255, fb = (param >> 16) & 255;
            if (iw != 32 || fb) return false;
            uint32_t v = (uint32_t)x;
            bool uns = param & (1u << 28);
            if (width == 32) *out = bits_f32(uns ? (float)v : (float)(int32_t)v);
            else *out = bits_f64(uns ? (double)v : (double)(int32_t)v);
            return true;
        }
        case AV_FTOI: { /* toward zero (VCVT's), in range, to a 32-bit integer */
            unsigned rm = (param >> 24) & 15, iw = (param >> 8) & 255, fb = (param >> 16) & 255;
            if (rm == 8) rm = (c->fpcr >> 22) & 3;
            if (rm != 3 || iw != 32 || fb || !plain(x, width, true)) return false;
            double d = width == 32 ? (double)f32_of(x) : f64_of(x);
            bool uns = param & (1u << 28);
            if (uns ? !(d > -1.0 && d < 4294967296.0) : !(d > -2147483649.0 && d < 2147483648.0)) return false;
            *out = uns ? (uint32_t)d : (uint32_t)(int32_t)d;
            return true;
        }
        case AV_FCVT: { /* between single and double, to nearest */
            unsigned to = param & 255, rm = (param >> 24) & 15;
            if (rm == 8) rm = (c->fpcr >> 22) & 3;
            if (rm || !plain(x, width, true) || to == width || (to != 32 && to != 64)) return false;
            if (to == 64) {
                *out = bits_f64((double)f32_of(x));
                return true;
            }
            *out = bits_f32((float)f64_of(x));
            return plain(*out, 32, (x & ~sign) == 0);
        }
        default: return false;
    }
}
uint64_t snx_arm_fp(SnxArmCPU* cpu, uint64_t x, uint64_t y, uint64_t add, uint32_t op, uint32_t width, uint32_t param) {
    CPU c; /* only fpcr/fpsr are consumed by the raw-bit primitives */
    c.fpcr = param & (1u << 30) ? (cpu->fpscr & (1u << 26)) | 0x03000000 : cpu->fpscr;
    c.fpsr = 0;
    uint64_t fast;
    if (host_fp(&c, cpu->fpscr & 0x10u, x, y, add, op, width, param, &fast)) return fast;
    unsigned rm = (param >> 24) & 15, iw = (param >> 8) & 255, fb = (param >> 16) & 255;
    int uns = !!(param & (1u << 28));
    uint64_t sign = 1ull << (width - 1), result = 0, product;
    if (rm == 8) rm = (c.fpcr >> 22) & 3;
    switch (op) {
        case AV_FADD: result = snx_fp_binary(&c, x, y, width, SNX_FP_ADD); break;
        case AV_FSUB: result = snx_fp_binary(&c, x, y, width, SNX_FP_SUB); break;
        case AV_FMUL: result = snx_fp_binary(&c, x, y, width, SNX_FP_MUL); break;
        case AV_FDIV: result = snx_fp_binary(&c, x, y, width, SNX_FP_DIV); break;
        case AV_FABD: result = snx_fp_binary(&c, x, y, width, SNX_FP_ABD); break;
        case AV_FMLA:
        case AV_FMLS:
        case AV_FNMLA:
        case AV_FNMLS:
        case AV_FNMUL:
            product = snx_fp_binary(&c, x, y, width, SNX_FP_MUL);
            if (op == AV_FMLS || op == AV_FNMLA || op == AV_FNMUL) product ^= sign;
            if (op == AV_FNMLA || op == AV_FNMLS) add ^= sign;
            result = op == AV_FNMUL ? product : snx_fp_binary(&c, add, product, width, SNX_FP_ADD);
            break;
        case AV_FMA:
        case AV_FMS:
        case AV_FNMA:
        case AV_FNMS:
            if (op == AV_FMS || op == AV_FNMA) x ^= sign;
            if (op == AV_FNMA || op == AV_FNMS) add ^= sign;
            result = snx_fp_fma(&c, add, x, y, width);
            break;
        case AV_FSQRT: result = snx_fp_unary(&c, x, width, SNX_FP_SQRT); break;
        case AV_MOV: result = x; break;
        case AV_FABS: result = x & (sign - 1); break;
        case AV_FNEG: result = x ^ sign; break;
        case AV_FRECPE: result = snx_fp_unary(&c, x, width, SNX_FP_RECPE); break;
        case AV_FRSQRTE: result = snx_fp_unary(&c, x, width, SNX_FP_RSQRTE); break;
        case AV_URECPE: result = urecpe32((u32)x); break;
        case AV_URSQRTE: result = ursqrte32((u32)x); break;
        case AV_FRECPS:
        case AV_FRSQRTS: result = step(&c, x, y, op == AV_FRSQRTS); break;
        case AV_FCMP:
        case AV_FCMPE: result = snx_fp_compare(&c, x, y, width, op == AV_FCMPE); break;
        case AV_FCEQ:
        case AV_FCGE:
        case AV_FCGT:
        case AV_FACGE:
        case AV_FACGT: {
            if (op == AV_FACGE || op == AV_FACGT) {
                x &= sign - 1;
                y &= sign - 1;
            }
            unsigned f = snx_fp_compare(&c, x, y, width, op != AV_FCEQ);
            int ok = op == AV_FCEQ ? f == 6 : (op == AV_FCGE || op == AV_FACGE) ? f == 6 || f == 2 : f == 2;
            result = ok ? UINT32_MAX : 0;
            break;
        }
        case AV_FCVT: result = snx_fp_convert(&c, x, width, param & 255, rm); break;
        case AV_ITOF: result = snx_fp_from_int(&c, x, iw, width, uns, fb); break;
        case AV_FTOI: result = snx_fp_to_int(&c, x, width, iw, uns, fb, rm); break;
        case AV_FROUND: result = snx_fp_round(&c, x, width, rm, param & 1); break;
        case AV_FMIN:
        case AV_FMAX:
        case AV_FMINNM:
        case AV_FMAXNM:
            result =
                snx_fp_minmax(&c, x, y, width, op == AV_FMIN || op == AV_FMINNM, op == AV_FMINNM || op == AV_FMAXNM);
            break;
        default: break; /* emitter only calls this API for the above operations */
    }
    cpu->fpscr |= c.fpsr & 0x9f;
    return result;
}
