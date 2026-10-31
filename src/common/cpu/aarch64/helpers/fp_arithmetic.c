/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (fpu/softfloat.c, fpu/softfloat-parts.c.inc, fpu/softfloat-parts-addsub.c.inc: see fp.c for
   their notices; target/arm/tcg/vfp_helper.c, helper-a64.c), LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The arithmetic and conversions of the FP ABI, on fp.c's unpacked values (included by fp.c). Each result is the
   exact value rounded once: sums and fused products are exact in 128 bits, quotients and roots carry a sticky bit
   from their remainder. The estimates are the Arm ARM's RecipEstimate and RecipSqrtEstimate tables, not a host's. */

/* ---- 128-bit significands: value = X * 2^(e - 125), the top bit of a normalized X at 125 ---- */
typedef struct FpWide {
    u64 hi, lo;
} FpWide;

static FpWide fp_wide(u64 hi, u64 lo) {
    FpWide w;
    w.hi = hi;
    w.lo = lo;
    return w;
}
static int fp_wide_zero(FpWide a) { return !a.hi && !a.lo; }
static int fp_wide_less(FpWide a, FpWide b) { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }
static FpWide fp_wide_add(FpWide a, FpWide b) {
    FpWide r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    return r;
}
static FpWide fp_wide_sub(FpWide a, FpWide b) {
    FpWide r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}
static FpWide fp_wide_shl(FpWide a, int n) {
    if (n <= 0) return a;
    if (n >= 64) return fp_wide(a.lo << (n - 64), 0);
    return fp_wide((a.hi << n) | (a.lo >> (64 - n)), a.lo << n);
}
/* a shift right that ORs anything shifted out into the lowest bit */
static FpWide fp_wide_shr_jam(FpWide a, int n) {
    u64 lost;
    if (n <= 0) return a;
    if (n >= 128) return fp_wide(0, !fp_wide_zero(a));
    if (n >= 64) {
        lost = a.lo | (n > 64 ? a.hi << (128 - n) : 0);
        return fp_wide(0, (n == 64 ? a.hi : a.hi >> (n - 64)) | (lost != 0));
    }
    lost = a.lo << (64 - n);
    return fp_wide(a.hi >> n, ((a.lo >> n) | (a.hi << (64 - n))) | (lost != 0));
}
static int fp_wide_top(FpWide a) { /* the index of the highest set bit, a nonzero */
    return a.hi ? 127 - (int)clz64(a.hi) : 63 - (int)clz64(a.lo);
}
/* the full 128-bit product of two 64-bit values */
static FpWide fp_mul64(u64 a, u64 b) {
    u64 al = (u32)a, ah = a >> 32, bl = (u32)b, bh = b >> 32;
    u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    u64 mid = (ll >> 32) + (u32)lh + (u32)hl;
    return fp_wide(hh + (lh >> 32) + (hl >> 32) + (mid >> 32), (mid << 32) | (u32)ll);
}

/* sign * X * 2^(e - 125), X normalized and compressed to 64 bits with a sticky bit, rounded (times 2^scale) */
static u64 fp_pack_wide(CPU* c, int sign, int e, FpWide x, int scale, int width, int rm) {
    int top = fp_wide_top(x);
    if (top > 125) {
        x = fp_wide_shr_jam(x, top - 125);
        e += top - 125;
    } else {
        x = fp_wide_shl(x, 125 - top);
        e -= 125 - top;
    }
    u64 frac = (x.hi << 1) | (x.lo >> 63) | ((x.lo << 1) != 0);
    return fp_round_pack(c, sign, e + scale, frac, width, rm);
}

/* the exact sum of two signed wide values (either may be zero), rounded once. Both are normalized first and the
   smaller one shifted with a sticky bit: their lowest bits are zero (at most 106 significant bits of 126), so the
   sticky bit can't disturb a cancellation. An exact zero is +0, or -0 when rounding toward -inf; the callers
   handle zeros of the same sign before. */
static u64 fp_sum(CPU* c, int sa, int ea, FpWide a, int sb, int eb, FpWide b, int scale, int width, int rm) {
    FpWide r;
    int sign;
    if (fp_wide_zero(a) && fp_wide_zero(b)) return fp_zero(rm == 2, width);
    if (fp_wide_zero(a)) return fp_pack_wide(c, sb, eb, b, scale, width, rm);
    if (fp_wide_zero(b)) return fp_pack_wide(c, sa, ea, a, scale, width, rm);
    ea -= 125 - fp_wide_top(a);
    a = fp_wide_shl(a, 125 - fp_wide_top(a));
    eb -= 125 - fp_wide_top(b);
    b = fp_wide_shl(b, 125 - fp_wide_top(b));
    if (ea < eb) {
        FpWide t = a;
        int s = sa, e = ea;
        a = b;
        sa = sb;
        ea = eb;
        b = t;
        sb = s;
        eb = e;
    }
    b = fp_wide_shr_jam(b, ea - eb);
    if (sa == sb) {
        r = fp_wide_add(a, b);
        sign = sa;
    } else if (fp_wide_less(a, b)) {
        r = fp_wide_sub(b, a);
        sign = sb;
    } else {
        r = fp_wide_sub(a, b);
        sign = sa;
    }
    if (fp_wide_zero(r)) return fp_zero(rm == 2, width);
    return fp_pack_wide(c, sign, ea, r, scale, width, rm);
}

static FpWide fp_wide_of(FpValue v) { /* a finite or zero operand as X (frac << 63) */
    return v.kind == FPV_ZERO ? fp_wide(0, 0) : fp_wide(v.frac >> 1, v.frac << 63);
}

/* FPMulAdd: add + a * b with one rounding, times 2^scale (scale -1 halves for the RSQRTS steps, still with one
   rounding). NaNs: signalling ones first, in the order addend, a, b, then quiet ones in that order; a quiet NaN
   addend with an infinity times zero is invalid too. */
static u64 fp_fused(CPU* c, u64 add, u64 a, u64 b, int width, int scale) {
    FpValue z = fp_unpack(c, add, width), x = fp_unpack(c, a, width), y = fp_unpack(c, b, width);
    int rm = fp_rmode(c), sp = x.sign ^ y.sign, product_inf = x.kind == FPV_INF || y.kind == FPV_INF;
    int product_zero = x.kind == FPV_ZERO || y.kind == FPV_ZERO;
    int inf_zero = (x.kind == FPV_INF && y.kind == FPV_ZERO) || (x.kind == FPV_ZERO && y.kind == FPV_INF);
    if (z.kind >= FPV_QNAN || x.kind >= FPV_QNAN || y.kind >= FPV_QNAN) {
        if (z.kind == FPV_QNAN && inf_zero) return fp_invalid(c, width);
        return fp_nan(c, fp_nan3(z, x, y), width);
    }
    if (inf_zero || (z.kind == FPV_INF && product_inf && z.sign != sp)) return fp_invalid(c, width);
    if (z.kind == FPV_INF) return fp_inf(z.sign, width);
    if (product_inf) return fp_inf(sp, width);
    if (z.kind == FPV_ZERO && product_zero && z.sign == sp) return fp_zero(sp, width);
    FpWide p = product_zero ? fp_wide(0, 0) : fp_mul64(x.frac, y.frac); /* (frac * frac) * 2^(ex + ey + 1 - 125) */
    return fp_sum(c, z.sign, z.exp, fp_wide_of(z), sp, x.exp + y.exp + 1, p, scale, width, rm);
}

u64 snx_fp_fma(CPU* c, u64 add, u64 a, u64 b, int width) { return fp_fused(c, add, a, b, width, 0); }

/* the quotient of two finite nonzero values, a sticky bit from the remainder */
static u64 fp_divide(CPU* c, FpValue x, FpValue y, int width, int rm) {
    u64 rem = x.frac, q = 0;
    int e = x.exp - y.exp;
    if (rem < y.frac) {
        rem <<= 1;
        --e;
    }
    for (int i = 62; i >= 0; --i) {
        if (rem >= y.frac) {
            rem -= y.frac;
            q |= (u64)1 << i;
        }
        rem <<= 1;
    }
    return fp_round_pack(c, x.sign ^ y.sign, e, q | (rem != 0), width, rm);
}

/* FPAdd, FPSub, FPMul, FPDiv, FPMulX, FABD (the difference, then the sign cleared, NaNs included), and the fused
   steps FPRecipStepFused (2 - a * b) and FPRSqrtStepFused ((3 - a * b) / 2), which negate a before anything else,
   so a NaN a comes out with its sign flipped */
static u64 fp_binary_rm(CPU* c, u64 a, u64 b, int width, int op, int rm) {
    u64 r;
    if (op == SNX_FP_RECPS || op == SNX_FP_RSQRTS) a ^= fp_signbit(1, width);
    FpValue x = fp_unpack(c, a, width);
    FpValue y = fp_unpack(c, b, width);
    if (x.kind >= FPV_QNAN || y.kind >= FPV_QNAN) {
        r = fp_nan(c, fp_nan2(x, y), width);
        return op == SNX_FP_ABD ? r & ~fp_signbit(1, width) : r;
    }
    int sign = x.sign ^ y.sign;
    switch (op) {
        case SNX_FP_ADD:
        case SNX_FP_SUB:
        case SNX_FP_ABD:
            if (op != SNX_FP_ADD) y.sign ^= 1;
            if (x.kind == FPV_INF && y.kind == FPV_INF && x.sign != y.sign)
                r = fp_invalid(c, width);
            else if (x.kind == FPV_INF)
                r = fp_inf(x.sign, width);
            else if (y.kind == FPV_INF)
                r = fp_inf(y.sign, width);
            else if (x.kind == FPV_ZERO && y.kind == FPV_ZERO && x.sign == y.sign)
                r = fp_zero(x.sign, width);
            else
                r = fp_sum(c, x.sign, x.exp, fp_wide_of(x), y.sign, y.exp, fp_wide_of(y), 0, width, rm);
            return op == SNX_FP_ABD ? r & ~fp_signbit(1, width) : r;
        case SNX_FP_MUL:
        case SNX_FP_MULX:
            if ((x.kind == FPV_INF && y.kind == FPV_ZERO) || (x.kind == FPV_ZERO && y.kind == FPV_INF))
                return op == SNX_FP_MULX ? fp_round_pack(c, sign, 1, (u64)1 << 62, width, 3) /* 2.0 */
                                         : fp_invalid(c, width);
            if (x.kind == FPV_INF || y.kind == FPV_INF) return fp_inf(sign, width);
            if (x.kind == FPV_ZERO || y.kind == FPV_ZERO) return fp_zero(sign, width);
            return fp_pack_wide(c, sign, x.exp + y.exp + 1, fp_mul64(x.frac, y.frac), 0, width, rm);
        case SNX_FP_DIV:
            if ((x.kind == FPV_INF && y.kind == FPV_INF) || (x.kind == FPV_ZERO && y.kind == FPV_ZERO))
                return fp_invalid(c, width);
            if (x.kind == FPV_INF || y.kind == FPV_ZERO) {
                if (x.kind != FPV_INF) c->fpsr |= FPSR_DZC;
                return fp_inf(sign, width);
            }
            if (x.kind == FPV_ZERO || y.kind == FPV_INF) return fp_zero(sign, width);
            return fp_divide(c, x, y, width, rm);
        case SNX_FP_RECPS:
        case SNX_FP_RSQRTS: {
            /* the addend 2.0 or 3.0, and 1.5: frac * 2^(exp - 62) */
            u64 constant = fp_round_pack(c, 0, 1, (op == SNX_FP_RECPS ? (u64)2 : (u64)3) << 61, width, 3);
            if ((x.kind == FPV_INF && y.kind == FPV_ZERO) || (x.kind == FPV_ZERO && y.kind == FPV_INF))
                return op == SNX_FP_RECPS ? constant : fp_round_pack(c, 0, 0, (u64)3 << 61, width, 3);
            if (x.kind == FPV_INF || y.kind == FPV_INF) return fp_inf(sign, width);
            return fp_fused(c, constant, a, b, width, op == SNX_FP_RSQRTS ? -1 : 0);
        }
        default: return 0;
    }
}
u64 snx_fp_binary(CPU* c, u64 a, u64 b, int width, int op) { return fp_binary_rm(c, a, b, width, op, fp_rmode(c)); }

/* ---- the reciprocal and reciprocal square root estimates (vfp_helper.c) ---- */

/* RecipEstimate: a in 256..511 is 0.5 <= x < 1 in steps of 1/512; the result 256..511, 1 <= r < 2 */
static u32 fp_recip_estimate(u32 a) {
    u32 b = (1u << 19) / (a * 2 + 1);
    return (b + 1) >> 1;
}
/* RecipSqrtEstimate: a in 128..511 (0.25 <= x < 1, in steps of 1/512 below 256 and of 1/256 above) gives (b + 1) / 2
   for the largest b in 512..1023 with a' * b^2 below 2^28, where a' is a * 2 + 1 below 256 and (a & ~1) * 2 + 2 from
   it. The pseudocode searches for b one step at a time, up to 500 steps an estimate, and that search was a twelfth of
   one game's main thread: the results of the search for every a are this table, which tests/a64_fp_arithmetic_test.c
   checks against the search itself */
#include "common/cpu/aarch64/helpers/fp_rsqrt_table.h"
static u32 fp_rsqrt_estimate(u32 a) { return fp_rsqrt_estimates[a - 128] + 256u; }

u32 urecpe32(u32 x) {
    if (!(x >> 31)) return 0xFFFFFFFFu;
    return fp_recip_estimate(x >> 23) << 23;
}
u32 ursqrte32(u32 x) {
    if (!(x >> 30)) return 0xFFFFFFFFu;
    return fp_rsqrt_estimate(x >> 23) << 23;
}

/* FPRecipEstimate on a finite nonzero operand's raw fields (after FZ, which made denormals zeros): the scaled
   fraction 1:frac<51:44> of the double-aligned fraction, the result exponent 2*bias-1 - e, denormal results at
   exponents 0 and -1 */
static u64 fp_recpe(CPU* c, u64 bits, int width) {
    int fb = fp_fbits(width), bias = fp_bias(width), sign = (int)((bits >> (width - 1)) & 1);
    int e = (int)((bits >> fb) & (u64)((1 << fp_ebits(width)) - 1));
    u64 frac = (bits & (((u64)1 << fb) - 1)) << (52 - fb);
    if ((bits & ~fp_signbit(1, width)) < ((u64)1 << (fb - 2))) { /* the reciprocal overflows */
        int rm = fp_rmode(c), to_inf = rm == 0 || (rm == 1 && !sign) || (rm == 2 && sign);
        c->fpsr |= FPSR_OFC | FPSR_IXC;
        return to_inf ? fp_inf(sign, width) : fp_max_normal(sign, width);
    }
    if ((c->fpcr & FPCR_FZ) && width != 16 && e >= 2 * bias - 1) { /* the reciprocal underflows */
        c->fpsr |= FPSR_UFC;
        return fp_zero(sign, width);
    }
    if (e == 0) { /* denormal: normalize the top fraction bit into the implicit one */
        if (!((frac >> 51) & 1)) {
            e = -1;
            frac <<= 2;
        } else {
            frac <<= 1;
        }
    }
    u64 result_frac = (u64)fp_recip_estimate(0x100 | (u32)((frac >> 44) & 0xFF)) << 44 & (((u64)1 << 52) - 1);
    int result_exp = 2 * bias - 1 - e;
    if (result_exp == 0) {
        result_frac = (result_frac >> 1) | ((u64)1 << 51);
    } else if (result_exp == -1) {
        result_frac = (result_frac >> 2) | ((u64)1 << 50);
        result_exp = 0;
    }
    return fp_signbit(sign, width) | (u64)result_exp << fb | result_frac >> (52 - fb);
}

/* FPRSqrtEstimate on a positive finite nonzero operand: denormals normalized, the scaled fraction keeps the
   exponent's parity, the result exponent (3*bias-1 - e) / 2 */
static u64 fp_rsqrte(u64 bits, int width) {
    int fb = fp_fbits(width), bias = fp_bias(width);
    int e = (int)((bits >> fb) & (u64)((1 << fp_ebits(width)) - 1));
    u64 frac = (bits & (((u64)1 << fb) - 1)) << (52 - fb);
    if (e == 0) {
        while (!((frac >> 51) & 1)) {
            frac <<= 1;
            --e;
        }
        frac = (frac & (((u64)1 << 51) - 1)) << 1;
    }
    u32 scaled = (e & 1) ? 0x80 | (u32)((frac >> 45) & 0x7F) : 0x100 | (u32)((frac >> 44) & 0xFF);
    u32 estimate = fp_rsqrt_estimate(scaled) & 0xFF;
    return (u64)((3 * bias - 1 - e) / 2) << fb | ((u64)estimate << 44) >> (52 - fb);
}

/* the square root of a positive finite value, digit by digit: 63 result bits and a sticky bit */
static u64 fp_sqrt(CPU* c, FpValue v, int width, int rm) {
    int odd = v.exp & 1, half = (v.exp - odd) / 2;
    FpWide n = fp_wide_shl(fp_wide(0, v.frac), 62 + odd), rem = fp_wide(0, 0);
    u64 root = 0;
    for (int i = 62; i >= 0; --i) {
        rem = fp_wide_shl(rem, 2);
        rem.lo |= (i >= 32 ? n.hi >> (2 * i - 64) : n.lo >> (2 * i)) & 3;
        FpWide trial = fp_wide_shl(fp_wide(0, root), 2);
        trial.lo |= 1;
        root <<= 1;
        if (!fp_wide_less(rem, trial)) {
            rem = fp_wide_sub(rem, trial);
            root |= 1;
        }
    }
    return fp_round_pack(c, 0, half, root | !fp_wide_zero(rem), width, rm);
}

/* FPSqrt, FPRecipEstimate, FPRSqrtEstimate, FPRecpX */
u64 snx_fp_unary(CPU* c, u64 bits, int width, int op) {
    FpValue v;
    if (op == SNX_FP_RECPX) { /* NaNs before flushing; then the exponent inverted and the fraction cleared */
        int fb = fp_fbits(width), emax = (1 << fp_ebits(width)) - 1;
        v = fp_unpack(c, bits, width);
        if (v.kind >= FPV_QNAN) return fp_nan(c, v, width);
        int e = v.kind == FPV_ZERO ? 0 : (int)((bits >> fb) & (u64)emax);
        return fp_signbit(v.sign, width) | (u64)(e ? ~e & emax : emax - 1) << fb;
    }
    v = fp_unpack(c, bits, width);
    if (v.kind >= FPV_QNAN) return fp_nan(c, v, width);
    switch (op) {
        case SNX_FP_SQRT:
            if (v.kind == FPV_ZERO) return fp_zero(v.sign, width);
            if (v.sign) return fp_invalid(c, width);
            if (v.kind == FPV_INF) return fp_inf(0, width);
            return fp_sqrt(c, v, width, fp_rmode(c));
        case SNX_FP_RECPE:
            if (v.kind == FPV_INF) return fp_zero(v.sign, width);
            if (v.kind == FPV_ZERO) {
                c->fpsr |= FPSR_DZC;
                return fp_inf(v.sign, width);
            }
            return fp_recpe(c, bits, width);
        case SNX_FP_RSQRTE:
            if (v.kind == FPV_ZERO) {
                c->fpsr |= FPSR_DZC;
                return fp_inf(v.sign, width);
            }
            if (v.sign) return fp_invalid(c, width);
            if (v.kind == FPV_INF) return fp_zero(0, width);
            return fp_rsqrte(bits, width);
        default: return 0;
    }
}

/* FPConvert (FCVT, FCVTN/FCVTL, FCVTXN with rm 5): halves are never flushed on input, NaNs keep their sign and the
   top of their payload (default NaN under DN), and a half with FPCR.AHP takes no NaN or infinity: IOC, and a zero
   or the largest magnitude. Numbers are rounded once, from the source (f64 to f16 directly). */
u64 snx_fp_convert(CPU* c, u64 bits, int src_width, int dst_width, int rm) {
    FpValue v = fp_unpack_as(c, bits, src_width, 1);
    int alt = dst_width == 16 && (c->fpcr & FPCR_AHP), sfb = fp_fbits(src_width), dfb = fp_fbits(dst_width);
    if (v.kind >= FPV_QNAN) {
        u64 payload = v.frac & (((u64)1 << (sfb - 1)) - 1);
        if (v.kind == FPV_SNAN || alt) c->fpsr |= FPSR_IOC;
        if (alt) return fp_zero(v.sign, dst_width);
        if (c->fpcr & FPCR_DN) return fp_default_nan(dst_width);
        payload = dfb >= sfb ? payload << (dfb - sfb) : payload >> (sfb - dfb);
        return fp_inf(v.sign, dst_width) | ((u64)1 << (dfb - 1)) | payload;
    }
    if (v.kind == FPV_INF) {
        if (!alt) return fp_inf(v.sign, dst_width);
        c->fpsr |= FPSR_IOC;
        return fp_signbit(v.sign, 16) | 0x7FFF;
    }
    if (v.kind == FPV_ZERO) return fp_zero(v.sign, dst_width);
    return fp_round_pack_as(c, v.sign, v.exp, v.frac, dst_width, rm, 1);
}

/* FixedToFP (SCVTF, UCVTF): the operand masked to its width (and two's complement for signed), divided by
   2^fbits exactly, rounded once in FPCR's mode */
u64 snx_fp_from_int(CPU* c, u64 bits, int int_width, int fp_width, int uns, int fbits) {
    u64 mag = int_width >= 64 ? bits : bits & (((u64)1 << int_width) - 1);
    int sign = 0;
    if (!uns && ((mag >> (int_width - 1)) & 1)) {
        sign = 1;
        mag = (0 - mag) & (int_width >= 64 ? ~(u64)0 : (((u64)1 << int_width) - 1));
    }
    if (!mag) return fp_zero(0, fp_width);
    FpValue v = fp_from_magnitude(sign, mag, -fbits);
    return fp_round_pack(c, sign, v.exp, v.frac, fp_width, fp_rmode(c));
}

/* FPToFixed (FCVT[NPMZA][SU], fixed point with fbits): the value times 2^fbits rounded in mode rm, then saturated:
   a NaN is 0, and out of range after rounding (infinities included, and any negative nonzero for unsigned)
   raises IOC without IXC and gives the nearest end of the range. The result is masked to the integer width. */
u64 snx_fp_to_int(CPU* c, u64 bits, int fp_width, int int_width, int uns, int fbits, int rm) {
    FpValue v = fp_unpack(c, bits, fp_width);
    u64 mask = int_width >= 64 ? ~(u64)0 : (((u64)1 << int_width) - 1), mag, top = (u64)1 << (int_width - 1);
    int inexact = 0, huge = 0, overflow;
    if (v.kind >= FPV_QNAN) {
        c->fpsr |= FPSR_IOC;
        return 0;
    }
    if (v.kind == FPV_INF)
        huge = 1, mag = 0;
    else
        mag = fp_round_magnitude(v, fbits, rm, &inexact, &huge);
    if (uns)
        overflow = huge || (v.sign && mag) || mag > mask;
    else
        overflow = huge || (v.sign ? mag > top : mag >= top);
    if (overflow) {
        c->fpsr |= FPSR_IOC;
        if (uns) return v.sign ? 0 : mask;
        return v.sign ? top : top - 1;
    }
    if (inexact) c->fpsr |= FPSR_IXC;
    return (v.sign ? 0 - mag : mag) & mask;
}

/* FPRoundIntN (FRINT32Z, FRINT32X, FRINT64Z, FRINT64X; vfp_helper.c's frint_s, frint_d): rounded to an integral value
   in mode rm as FPRoundInt does, IXC when that changed it, then checked against the intsize-bit integers. A NaN, an
   infinity or a value out of their range raises IOC alone and gives -2^(intsize-1). */
u64 snx_fp_round_int(CPU* c, u64 bits, int width, int rm, int intsize) {
    FpValue v = fp_unpack(c, bits, width);
    u64 mag, top = (u64)1 << (intsize - 1);
    int inexact = 0, huge = 0;
    if (v.kind == FPV_ZERO) return fp_zero(v.sign, width);
    if (v.kind == FPV_FINITE) {
        mag = fp_round_magnitude(v, 0, rm, &inexact, &huge);
        if (!huge && (v.sign ? mag <= top : mag < top)) {
            if (inexact) c->fpsr |= FPSR_IXC;
            if (!mag) return fp_zero(v.sign, width);
            return fp_repack(c, fp_from_magnitude(v.sign, mag, 0), width);
        }
    }
    c->fpsr |= FPSR_IOC;
    return fp_signbit(1, width) | (u64)(fp_bias(width) + intsize - 1) << fp_fbits(width);
}

/* FPToFixedJS (FJCVTZS; vfp_helper.c's fjcvtzs, softfloat's float_to_sint_modulo): a double rounded toward zero, the
   low 32 bits of the integer whatever its size, and bit 32 the Z flag: set when the double was exactly that integer
   in the int32 range, not -0, and not a denormal flushed by FPCR.FZ. A NaN, an infinity or an integer outside the
   int32 range raises IOC (and gives 0 for the first two), else a dropped fraction raises IXC */
u64 snx_fp_jcvt(CPU* c, u64 bits) {
    FpValue v = fp_unpack(c, bits, 64);
    u64 mag;
    int inexact = 0, huge = 0, overflow;
    if (v.kind >= FPV_INF) {
        c->fpsr |= FPSR_IOC;
        return 0;
    }
    if (v.kind == FPV_ZERO) return (u64)(!v.sign && !(bits & (((u64)1 << 52) - 1))) << 32;
    if (v.exp > 62) { /* integral, at least 2^63: only its low bits survive */
        int shift = v.exp - 62;
        mag = shift < 64 ? v.frac << shift : 0;
        overflow = 1;
    } else {
        mag = fp_round_magnitude(v, 0, 3, &inexact, &huge);
        overflow = v.sign ? mag > ((u64)1 << 31) : mag >= ((u64)1 << 31);
    }
    mag = (u32)(v.sign ? 0 - mag : mag);
    if (overflow)
        c->fpsr |= FPSR_IOC;
    else if (inexact)
        c->fpsr |= FPSR_IXC;
    return mag | (u64)(!overflow && !inexact) << 32;
}

/* FPConvertBF (BFCVT; vfp_helper.c's bfcvt, softfloat's float32_to_bfloat16): a single to a bfloat16, its top half,
   rounded in FPCR's mode. The formats share the exponent range and their denormal grids, so a number is its
   encoding with the low 16 bits rounded off: a carry reaches the exponent, and past the largest number infinity
   (OFC). Tiny before rounding is a denormal, which raises UFC when inexact; FPCR.FZ flushed it already (IDC) */
u64 snx_fp_bfcvt(CPU* c, u64 bits) {
    FpValue v = fp_unpack(c, bits, 32);
    u64 magnitude = bits & 0x7FFFFFFFu, low = magnitude & 0xFFFF, mant = magnitude >> 16, up;
    if (v.kind >= FPV_QNAN) return fp_nan(c, v, 32) >> 16;
    if (v.kind == FPV_ZERO) return (u64)v.sign << 15;
    if (v.kind == FPV_INF) return fp_inf(v.sign, 32) >> 16;
    switch (fp_rmode(c)) {
        case 0: up = low > 0x8000 || (low == 0x8000 && (mant & 1)); break;
        case 1: up = low && !v.sign; break;
        case 2: up = low && v.sign; break;
        default: up = 0; break;
    }
    mant += up;
    if (low) {
        c->fpsr |= FPSR_IXC;
        if (!(magnitude >> 23)) c->fpsr |= FPSR_UFC;
        if (mant == 0x7F80) c->fpsr |= FPSR_OFC;
    }
    return (u64)v.sign << 15 | mant;
}
