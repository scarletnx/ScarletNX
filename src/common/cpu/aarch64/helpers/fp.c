/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (fpu/softfloat.c, fpu/softfloat-parts.c.inc, fpu/softfloat-parts-addsub.c.inc,
   fpu/softfloat-specialize.c.inc), whose portions are under the SoftFloat-2a license, the BSD license and
   GPL-2.0-or-later:
   Copyright (c) 2006, Fabrice Bellard (BSD portions)
   This file is a derivative work of the SoftFloat IEC/IEEE Floating-point Arithmetic Package, Release 2a,
   written by John R. Hauser. That work was made possible in part by the International Computer Science Institute,
   located at Suite 600, 1947 Center Street, Berkeley, California 94704. Funding was partially provided by the
   National Science Foundation under grant MIP-9311980. The original version of that code was written as part of a
   project to build a fixed-point vector processor in collaboration with the University of California at Berkeley,
   overseen by Profs. Nelson Morgan and John Wawrzynek.
   THE SOFTFLOAT PACKAGE IS DISTRIBUTED AS IS, FOR FREE. Although reasonable effort has been made to avoid it, IT MAY
   CONTAIN FAULTS THAT WILL AT TIMES RESULT IN INCORRECT BEHAVIOR. USE OF IT IS RESTRICTED TO PERSONS AND
   ORGANIZATIONS WHO CAN AND WILL TAKE FULL RESPONSIBILITY FOR ANY AND ALL LOSSES, COSTS, OR OTHER PROBLEMS ARISING
   FROM ITS USE. Derivative works are acceptable, even for commercial purposes, so long as (1) they include prominent
   notice that the work is derivative, and (2) they include prominent notice akin to these four paragraphs for those
   parts of the code that are retained.
   The ARM-specific rules (FPUnpack, FPRound, FPProcessNaNs, FPMax/FPMin[Num], FPCompare, FPRoundInt) follow the Arm
   Architecture Reference Manual's shared pseudocode for ARMv8.0, with FEAT_FP16's FPCR.FZ16 (no FPCR.AH). */
/* The FP of the generated-code ABI: IEEE encodings in and out, integer arithmetic only, so a result is the same on
   any host and under any host rounding mode, and exceptions accumulate in the guest's FPSR, never in the host's.
   This file has the representation, rounding and NaN rules, and the compare, min/max and round-to-integral
   operations; fp_arithmetic.c, included at its end, the arithmetic and the conversions. The A32 backend compiles
   both again under other names (arm/fp_helpers.c), and uses fp_unpack, fp_nan, fp_nan2 and fp_fused itself. */

#define FPCR_AHP (1u << 26)
#define FPCR_DN  (1u << 25)
#define FPCR_FZ  (1u << 24)
#define FPCR_FZ16 (1u << 19)
#define FPSR_IOC 0x01u
#define FPSR_DZC 0x02u
#define FPSR_OFC 0x04u
#define FPSR_UFC 0x08u
#define FPSR_IXC 0x10u
#define FPSR_IDC 0x80u

/* an unpacked operand (QEMU's FloatParts). A finite nonzero value is frac * 2^(exp - 62) with frac's top bit at 62,
   whatever the format, so every format shares the arithmetic; a NaN keeps its fraction field as it was */
typedef struct FpValue {
    int kind;
    int sign;
    int exp;
    u64 frac;
} FpValue;
enum { FPV_ZERO, FPV_FINITE, FPV_INF, FPV_QNAN, FPV_SNAN }; /* NaNs last: kind >= FPV_QNAN is any NaN */

static int fp_fbits(int width) { return width == 16 ? 10 : width == 32 ? 23 : 52; }
static int fp_ebits(int width) { return width == 16 ? 5 : width == 32 ? 8 : 11; }
static int fp_bias(int width) { return (1 << (fp_ebits(width) - 1)) - 1; }
static u64 fp_signbit(int sign, int width) { return (u64)(sign & 1) << (width - 1); }
static u64 fp_inf(int sign, int width) {
    return fp_signbit(sign, width) | ((u64)((1 << fp_ebits(width)) - 1) << fp_fbits(width));
}
static u64 fp_zero(int sign, int width) { return fp_signbit(sign, width); }
static u64 fp_default_nan(int width) { return fp_inf(0, width) | ((u64)1 << (fp_fbits(width) - 1)); }
static u64 fp_max_normal(int sign, int width) { return fp_inf(sign, width) - 1; }
static int fp_rmode(const SnxFpEnv* c) { return (int)((c->fpcr >> 22) & 3); }

/* FPUnpack (cv 0) and FPUnpackCV (cv 1, the conversions between FP formats): single and double denormals become
   zeros of their sign under FPCR.FZ, with IDC, before anything looks at them. Halves are flushed under FPCR.FZ16
   without IDC, and never by a conversion (FZ16 is RES0 on the A57); a conversion's halves take FPCR.AHP, where the
   top exponent is a number */
static FpValue fp_unpack_as(SnxFpEnv* c, u64 bits, int width, int cv) {
    int fb = fp_fbits(width), emax = (1 << fp_ebits(width)) - 1, bias = fp_bias(width);
    int e = (int)((bits >> fb) & (u64)emax);
    u64 m = bits & (((u64)1 << fb) - 1);
    FpValue v;
    v.sign = (int)((bits >> (width - 1)) & 1);
    v.exp = 0;
    v.frac = m;
    if (e == emax && !(width == 16 && cv && (c->fpcr & FPCR_AHP))) {
        v.kind = !m ? FPV_INF : (m >> (fb - 1)) & 1 ? FPV_QNAN : FPV_SNAN;
    } else if (e == 0) {
        if (!m) {
            v.kind = FPV_ZERO;
        } else if (width != 16 && (c->fpcr & FPCR_FZ)) {
            c->fpsr |= FPSR_IDC;
            v.kind = FPV_ZERO;
            v.frac = 0;
        } else if (width == 16 && !cv && (c->fpcr & FPCR_FZ16)) {
            v.kind = FPV_ZERO;
            v.frac = 0;
        } else { /* denormal: m * 2^(1 - bias - fb), normalized */
            int shift = (int)clz64(m) - 1;
            v.kind = FPV_FINITE;
            v.frac = m << shift;
            v.exp = 63 - bias - fb - shift;
        }
    } else {
        v.kind = FPV_FINITE;
        v.frac = (m | ((u64)1 << fb)) << (62 - fb);
        v.exp = e - bias;
    }
    return v;
}
static FpValue fp_unpack(SnxFpEnv* c, u64 bits, int width) { return fp_unpack_as(c, bits, width, 0); }

/* FPProcessNaN: a signalling NaN raises IOC; the result is the default NaN under FPCR.DN, else the operand quieted
   with its sign and payload */
static u64 fp_nan(SnxFpEnv* c, FpValue v, int width) {
    int fb = fp_fbits(width);
    if (v.kind == FPV_SNAN) c->fpsr |= FPSR_IOC;
    if (c->fpcr & FPCR_DN) return fp_default_nan(width);
    return fp_inf(v.sign, width) | v.frac | ((u64)1 << (fb - 1));
}
/* FPProcessNaNs, FPProcessNaNs3: the first signalling NaN in operand order, else the first quiet one */
static FpValue fp_nan2(FpValue a, FpValue b) {
    if (a.kind == FPV_SNAN) return a;
    if (b.kind == FPV_SNAN) return b;
    return a.kind == FPV_QNAN ? a : b;
}
static FpValue fp_nan3(FpValue a, FpValue b, FpValue d) {
    if (a.kind == FPV_SNAN) return a;
    if (b.kind == FPV_SNAN) return b;
    if (d.kind == FPV_SNAN) return d;
    if (a.kind == FPV_QNAN) return a;
    return b.kind == FPV_QNAN ? b : d;
}
static u64 fp_invalid(SnxFpEnv* c, int width) {
    c->fpsr |= FPSR_IOC;
    return fp_default_nan(width);
}

/* FPRound (FPRoundBase; cv 1: FPRoundCV, for the conversions between FP formats): sign * frac * 2^(exp - 62) (frac
   nonzero, its lowest bit a sticky bit of anything below) rounded once in mode rm (0-6) to the format.
   - FPCR.FZ: a single or double result whose unrounded value is below the smallest normal is a zero of its sign,
     with UFC and without IXC, inexact or not. FPCR.FZ16 does the same to halves, except in a conversion.
   - Tininess is detected before rounding: a tiny inexact result raises UFC (and IXC).
   - Overflow raises OFC and IXC and gives infinity, or the largest finite value where the mode rounds toward zero.
   - A conversion's half with FPCR.AHP has no infinities: past its largest exponent it raises IOC alone and gives
     0x7FFF. */
static u64 fp_round_pack_as(SnxFpEnv* c, int sign, int exp, u64 frac, int width, int rm, int cv) {
    int fb = fp_fbits(width), bias = fp_bias(width), emax = (1 << fp_ebits(width)) - 1, up, to_inf;
    int inexact, half_cmp, ahp = width == 16 && cv && (c->fpcr & FPCR_AHP);
    int flush = width == 16 ? !cv && (c->fpcr & FPCR_FZ16) : (c->fpcr & FPCR_FZ) != 0;
    u64 mant;
    if (frac >> 63) {
        frac = (frac >> 1) | (frac & 1);
        ++exp;
    } else {
        int lead = (int)clz64(frac) - 1;
        frac <<= lead;
        exp -= lead;
    }
    if (flush && exp < 1 - bias) {
        c->fpsr |= FPSR_UFC;
        return fp_zero(sign, width);
    }
    int biased = exp + bias;
    int shift = 62 - fb;
    if (biased < 1) {
        shift += 1 - biased;
        biased = 0;
    }
    if (shift > 63) { /* below half the smallest denormal */
        mant = 0;
        inexact = 1;
        half_cmp = -1;
    } else {
        u64 rest = frac & (((u64)1 << shift) - 1), half = (u64)1 << (shift - 1);
        mant = frac >> shift;
        inexact = rest != 0;
        half_cmp = rest > half ? 1 : rest == half ? 0 : -1;
    }
    if (biased == 0 && inexact) c->fpsr |= FPSR_UFC;
    switch (rm) {
        case 0:
            up = half_cmp > 0 || (half_cmp == 0 && (mant & 1));
            to_inf = 1;
            break;
        case 1:
            up = inexact && !sign;
            to_inf = !sign;
            break;
        case 2:
            up = inexact && sign;
            to_inf = sign;
            break;
        case 4:
            up = half_cmp >= 0;
            to_inf = 1;
            break;
        case 5: /* to odd: a dropped nonzero part sets the lowest bit */
        case 6: /* the same, overflowing to infinity (BFDOT's and BFMMLA's products and sums) */
            up = 0;
            if (inexact) mant |= 1;
            to_inf = rm == 6;
            break;
        default:
            up = 0;
            to_inf = 0;
            break;
    }
    mant += (u64)up;
    /* a normal mant holds the implicit bit, so a carry out of the fraction lands in the exponent, and a denormal
       that rounds up to 1 << fb becomes the smallest normal */
    u64 encoded = biased ? ((u64)(biased - 1) << fb) + mant : mant;
    if (ahp) {
        if ((encoded >> fb) > (u64)emax) {
            c->fpsr |= FPSR_IOC;
            return fp_signbit(sign, width) | 0x7FFF;
        }
    } else if ((encoded >> fb) >= (u64)emax) {
        c->fpsr |= FPSR_OFC | FPSR_IXC;
        return to_inf ? fp_inf(sign, width) : fp_max_normal(sign, width);
    }
    if (inexact) c->fpsr |= FPSR_IXC;
    return fp_signbit(sign, width) | encoded;
}
static u64 fp_round_pack(SnxFpEnv* c, int sign, int exp, u64 frac, int width, int rm) {
    return fp_round_pack_as(c, sign, exp, frac, width, rm, 0);
}
/* an unpacked zero, infinity or finite value encoded again (exact, so no flags) */
static u64 fp_repack(SnxFpEnv* c, FpValue v, int width) {
    if (v.kind == FPV_ZERO) return fp_zero(v.sign, width);
    if (v.kind == FPV_INF) return fp_inf(v.sign, width);
    return fp_round_pack(c, v.sign, v.exp, v.frac, width, 3);
}

/* the order of two numbers (no NaNs): -1, 0 or 1; zeros of either sign are equal */
static int fp_order(FpValue a, FpValue b) {
    int sa = a.kind == FPV_ZERO ? 0 : a.sign ? -1 : 1, sb = b.kind == FPV_ZERO ? 0 : b.sign ? -1 : 1, magnitude;
    if (sa != sb) return sa < sb ? -1 : 1;
    if (sa == 0) return 0;
    if (a.kind != b.kind)
        magnitude = a.kind == FPV_INF ? 1 : -1;
    else if (a.kind == FPV_INF || (a.exp == b.exp && a.frac == b.frac))
        magnitude = 0;
    else if (a.exp != b.exp)
        magnitude = a.exp > b.exp ? 1 : -1;
    else
        magnitude = a.frac > b.frac ? 1 : -1;
    return sa > 0 ? magnitude : -magnitude;
}

/* FPCompare: NZCV 8 less, 6 equal, 2 greater, 3 unordered. IOC for a signalling NaN, and for any NaN when signal
   (FCMPE, and the ordered vector compares) */
SnxFpReply snx_fp_compare(u64 a, u64 b, int width, int signal, u32 fpcr) {
    SnxFpEnv c = {fpcr, 0};
    FpValue x = fp_unpack(&c, a, width), y = fp_unpack(&c, b, width);
    if (x.kind >= FPV_QNAN || y.kind >= FPV_QNAN) {
        if (signal || x.kind == FPV_SNAN || y.kind == FPV_SNAN) c.fpsr |= FPSR_IOC;
        return (SnxFpReply){3, c.fpsr};
    }
    int order = fp_order(x, y);
    return (SnxFpReply){(u64)(order < 0 ? 8 : order == 0 ? 6 : 2), c.fpsr};
}

/* FPMax, FPMin, and FPMaxNum, FPMinNum (numeric): the numeric forms replace a lone quiet NaN by the infinity that
   loses, so the number wins; any other NaN goes through NaN processing. Zeros of both signs: max is +0, min -0 */
SnxFpReply snx_fp_minmax(u64 a, u64 b, int width, int minimum, int numeric, u32 fpcr) {
    SnxFpEnv c = {fpcr, 0};
    FpValue x = fp_unpack(&c, a, width), y = fp_unpack(&c, b, width);
    u64 r;
    if (numeric) {
        if (x.kind == FPV_QNAN && y.kind != FPV_QNAN) {
            x.kind = FPV_INF;
            x.sign = !minimum;
        } else if (y.kind == FPV_QNAN && x.kind != FPV_QNAN) {
            y.kind = FPV_INF;
            y.sign = !minimum;
        }
    }
    if (x.kind >= FPV_QNAN || y.kind >= FPV_QNAN) {
        r = fp_nan(&c, fp_nan2(x, y), width);
    } else if (x.kind == FPV_ZERO && y.kind == FPV_ZERO) {
        r = fp_zero(minimum ? x.sign | y.sign : x.sign & y.sign, width);
    } else {
        int order = fp_order(x, y);
        r = fp_repack(&c, (minimum ? order < 0 : order > 0) ? x : y, width);
    }
    return (SnxFpReply){r, c.fpsr};
}

/* the magnitude of v * 2^scale (v finite) rounded to an integer in mode rm (0-4): *inexact if something was
   dropped; *huge, and 0, when the magnitude is 2^64 or more */
static u64 fp_round_magnitude(FpValue v, int scale, int rm, int* inexact, int* huge) {
    int e = v.exp + scale, half_cmp, up;
    u64 mag;
    *inexact = 0;
    *huge = 0;
    if (v.kind == FPV_ZERO) return 0;
    if (e > 63) {
        *huge = 1;
        return 0;
    }
    if (e >= 62) return v.frac << (e - 62);
    if (e < -1) {
        mag = 0;
        half_cmp = -1;
        *inexact = 1;
    } else {
        int shift = 62 - e;
        u64 rest = v.frac & (((u64)1 << shift) - 1), half = (u64)1 << (shift - 1);
        mag = v.frac >> shift;
        *inexact = rest != 0;
        half_cmp = rest > half ? 1 : rest == half ? 0 : -1;
    }
    switch (rm) {
        case 0: up = half_cmp > 0 || (half_cmp == 0 && (mag & 1)); break;
        case 1: up = *inexact && !v.sign; break;
        case 2: up = *inexact && v.sign; break;
        case 4: up = half_cmp >= 0; break;
        default: up = 0; break;
    }
    return mag + (u64)up;
}
/* a nonzero integer magnitude as an unpacked finite value */
static FpValue fp_from_magnitude(int sign, u64 mag, int scale) {
    FpValue v;
    v.kind = FPV_FINITE;
    v.sign = sign;
    if (mag >> 63) {
        v.frac = (mag >> 1) | (mag & 1);
        v.exp = 63 + scale;
    } else {
        int lead = (int)clz64(mag) - 1;
        v.frac = mag << lead;
        v.exp = 62 - lead + scale;
    }
    return v;
}

/* FPRoundInt (FRINTN/P/M/Z/A/X/I) in the source format: NaN processing, infinities and zeros kept, numbers rounded
   to an integral value that keeps the sign (a negative one that rounds to zero is -0). Only FRINTX (exact) raises
   IXC; nothing else but IOC for a signalling NaN and IDC for a flushed input. */
SnxFpReply snx_fp_round(u64 bits, int width, int rm, int exact, u32 fpcr) {
    SnxFpEnv c = {fpcr, 0};
    FpValue v = fp_unpack(&c, bits, width);
    int inexact, huge;
    u64 r;
    if (v.kind >= FPV_QNAN) {
        r = fp_nan(&c, v, width);
    } else if (v.kind != FPV_FINITE) {
        r = fp_repack(&c, v, width);
    } else if (v.exp >= fp_fbits(width)) {
        r = bits; /* integral already */
    } else {
        u64 mag = fp_round_magnitude(v, 0, rm, &inexact, &huge);
        if (exact && inexact) c.fpsr |= FPSR_IXC;
        r = mag ? fp_repack(&c, fp_from_magnitude(v.sign, mag, 0), width) : fp_zero(v.sign, width);
    }
    return (SnxFpReply){r, c.fpsr};
}

#include "fp_arithmetic.c"
