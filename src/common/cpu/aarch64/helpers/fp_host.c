/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The helpers' own fast paths: the host's IEEE arithmetic where it gives the soft-float's exact bits and flags, for
   the operands the emitters' guards leave to the helpers that are still cheap to get right. A game's vectors often
   carry a lane the guards refuse (a NaN or an infinity in an unused lane, a product that underflows), and a vector
   goes to the helpers whole, so most calls are such lanes or their neighbours. Included by cpu_helpers.c after fp.c,
   whose operations stay the soft-float the tests hold these to (the A32 backend's copy of fp.c has none of this).
   The emitters' slow paths call the snx_fp_host_* entry points at the end; each fp_host_* path returns 1 with *out
   and FPSR's flags set, or 0 having changed nothing, for the soft-float to do it.
   What it takes on: round to nearest (FPCR.RMode 0), singles and doubles, and
   - NaN operands (FPProcessNaNs: the first signalling one, else the first quiet one, quieted, or the default NaN
     under FPCR.DN; IOC for a signalling one) and invalid operations (the default NaN and IOC);
   - infinities as operands and results: exact from an infinite operand, DZC from a division by zero, OFC and IXC
     from an overflow;
   - denormal operands, flushed to zeros of their sign with IDC under FPCR.FZ;
   - zero, tiny and normal results. A sum is exact whenever it is below the smallest normal, so only its rounding
     error (TwoSum) matters. A single's product or quotient is decided in double precision: the product exactly,
     the quotient up to its residual and its rounding to double, which can't cross the smallest normal.
     A tiny result is a zero of its sign with UFC under FZ (tininess is before rounding, the double value is
     compared), else the host's denormal with UFC and IXC when inexact. A double's tiny product or quotient is the
     soft-float's, and its other results need IXC set already (no cheap exact residual without FMA).
   - a single's fused multiply-add and the FRECPS and FRSQRTS steps: the product is exact in double precision and
     the sum is rounded to odd there (TwoSum's error says which way), which a single then rounds exactly once.
   Only in the host thread's default FP environment (round to nearest, no flush to zero), which each call checks:
   the soft-float takes the rest, so the results never depend on the host's state, though the host's own exception
   flags may be raised. Nothing here may be contracted into an FMA a target may not have: the C operations are the
   host's SSE2 or AArch64 FP. Other hosts take the soft-float. */
#if (defined(__x86_64__) || defined(__aarch64__) || defined(_M_X64) || defined(_M_ARM64))
#define FP_HOST_ON 1
#pragma clang fp contract(off)

static float fp_host_f(u64 bits) {
    u32 v = (u32)bits;
    float f;
    __builtin_memcpy(&f, &v, 4);
    return f;
}
static u64 fp_host_fbits(float f) {
    u32 v;
    __builtin_memcpy(&v, &f, 4);
    return v;
}
static double fp_host_d(u64 bits) {
    double d;
    __builtin_memcpy(&d, &bits, 8);
    return d;
}
static u64 fp_host_dbits(double d) {
    u64 v;
    __builtin_memcpy(&v, &d, 8);
    return v;
}

#define FP_HOST_TINY32 0x3810000000000000ull /* 2^-126 as a double */

/* the host thread's FP environment is IEEE's default: round to nearest, no flush to zero of inputs or results
   (x86's MXCSR.RC, FTZ and DAZ; AArch64's FPCR.RMode, FZ, AH and FIZ) */
static int fp_host_default(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return !(__builtin_ia32_stmxcsr() & 0xE040u);
#else
    return !(__builtin_arm_rsr64("fpcr") & 0x01C00003u);
#endif
}
/* the format's width and the host's environment allow the paths below (each assumes it: their callers check it,
   once a vector), and (rounded) FPCR rounds to nearest, which the arithmetic needs */
static int fp_host_ready(const CPU* c, int width, int rounded) {
    return width != 16 && (!rounded || !(c->fpcr & (3u << 22))) && fp_host_default();
}

typedef struct {
    u64 sign, magnitude, inf, quiet, normal; /* the format's masks and encodings */
} FpHostFormat;
SNX_INLINE FpHostFormat fp_host_format(int width) {
    FpHostFormat f = {fp_signbit(1, width), fp_signbit(1, width) - 1, fp_inf(0, width), (u64)1 << (fp_fbits(width) - 1),
                      (u64)1 << fp_fbits(width)};
    return f;
}
static int fp_host_nan(const FpHostFormat* f, u64 x) { return (x & f->magnitude) > f->inf; }
static int fp_host_snan(const FpHostFormat* f, u64 x) { return fp_host_nan(f, x) && !(x & f->quiet); }
static int fp_host_inf(const FpHostFormat* f, u64 x) { return (x & f->magnitude) == f->inf; }
static int fp_host_zero(const FpHostFormat* f, u64 x) { return !(x & f->magnitude); }
/* an operand's encoding (the bits above its width dropped, as FPUnpack reads it), flushed under FPCR.FZ: a denormal
   is a zero of its sign, and IDC */
static u64 fp_host_flush(const CPU* c, const FpHostFormat* f, u64 x, u32* flags) {
    u64 m = x & f->magnitude;
    x &= f->sign | f->magnitude;
    if (!(c->fpcr & FPCR_FZ) || !m || m >= f->normal) return x;
    *flags |= FPSR_IDC;
    return x & f->sign;
}
/* FPProcessNaN of x (a NaN): quieted, or the default NaN under FPCR.DN; IOC when it signals */
static u64 fp_host_process(const CPU* c, const FpHostFormat* f, int width, u64 x, u32* flags) {
    if (!(x & f->quiet)) *flags |= FPSR_IOC;
    return c->fpcr & FPCR_DN ? fp_default_nan(width) : x | f->quiet;
}
/* FPProcessNaNs: the first signalling NaN of a and b, else the first quiet one (one of them is a NaN) */
static u64 fp_host_nans(const CPU* c, const FpHostFormat* f, int width, u64 a, u64 b, u32* flags) {
    u64 pick = fp_host_snan(f, a) ? a : fp_host_snan(f, b) ? b : fp_host_nan(f, a) ? a : b;
    if (fp_host_snan(f, a) || fp_host_snan(f, b)) *flags |= FPSR_IOC;
    return fp_host_process(c, f, width, pick, flags);
}
static u64 fp_host_invalid(int width, u32* flags) {
    *flags |= FPSR_IOC;
    return fp_default_nan(width);
}
static int fp_host_done(CPU* c, u64 r, u32 flags, u64* out) {
    c->fpsr |= flags;
    *out = r;
    return 1;
}

/* the TwoSum error of x + y = s (finite), nonzero when the sum was rounded; not finite when an intermediate
   overflowed */
static float fp_host_err32(float x, float y, float s) {
    float t = s - x;
    return (x - (s - t)) + (y - t);
}
static double fp_host_err64(double x, double y, double s) {
    double t = s - x;
    return (x - (s - t)) + (y - t);
}

/* a single's result r (finite, from the host) whose exact value v is known by a double: its tininess (|v| below the
   smallest normal) and inexactness. Under FZ a tiny one is a zero of its sign with UFC, else the host's value with
   UFC and IXC when inexact */
static u64 fp_host_finish32(const CPU* c, u64 r, int tiny, int inexact, u32* flags) {
    if (tiny && (c->fpcr & FPCR_FZ)) {
        *flags |= FPSR_UFC;
        return r & 0x80000000u;
    }
    if (inexact) *flags |= tiny ? FPSR_UFC | FPSR_IXC : FPSR_IXC;
    return r;
}

/* FPAdd, FPSub, FPMul, FPDiv, FPMulX and FABD of singles and doubles */
SNX_INLINE int fp_host_binary(CPU* c, u64 a, u64 b, int width, int op, u64* out) {
    if (op > SNX_FP_ABD) return 0;
    int sum = op == SNX_FP_ADD || op == SNX_FP_SUB || op == SNX_FP_ABD, mul = op == SNX_FP_MUL || op == SNX_FP_MULX;
    FpHostFormat f = fp_host_format(width);
    u32 flags = 0;
    u64 r;
    a = fp_host_flush(c, &f, a, &flags);
    b = fp_host_flush(c, &f, b, &flags);
    if (fp_host_nan(&f, a) || fp_host_nan(&f, b)) {
        r = fp_host_nans(c, &f, width, a, b, &flags);
        return fp_host_done(c, op == SNX_FP_ABD ? r & ~f.sign : r, flags, out);
    }
    if (width == 32) {
        float x = fp_host_f(a), y = fp_host_f(b);
        /* a product, and a sum, exact as a double: one below the smallest normal under FZ is a zero of its sign
           without the host's denormal, whose result an x86 CPU makes in a microcode assist a hundred times slower */
        double p = mul ? (double)x * (double)y : sum ? (double)x + (op == SNX_FP_ADD ? (double)y : -(double)y) : 0.0;
        double size = p < 0 ? -p : p;
        if ((c->fpcr & FPCR_FZ) && size != 0.0 && size < fp_host_d(FP_HOST_TINY32)) {
            flags |= FPSR_UFC;
            r = (fp_host_dbits(p) >> 63) << 31;
            return fp_host_done(c, op == SNX_FP_ABD ? 0 : r, flags, out);
        }
        float s = op == SNX_FP_ADD ? x + y : sum ? x - y : mul ? (float)p : x / y;
        r = fp_host_fbits(s);
        if (fp_host_nan(&f, r)) { /* inf - inf, 0 * inf, 0 / 0, inf / inf */
            if (op == SNX_FP_MULX)
                r = (u64)((a ^ b) & f.sign) | 0x40000000u; /* FPMulX: 2.0 */
            else
                r = fp_host_invalid(32, &flags);
        } else if (fp_host_inf(&f, r)) {
            if (fp_host_inf(&f, a) || (op != SNX_FP_DIV && fp_host_inf(&f, b))) {
            } /* exact */
            else if (op == SNX_FP_DIV && fp_host_zero(&f, b))
                flags |= FPSR_DZC;
            else
                flags |= FPSR_OFC | FPSR_IXC;
        } else if (sum) { /* exact below the smallest normal */
            float e = fp_host_err32(x, op == SNX_FP_ADD ? y : -y, s);
            if (e - e != 0.0f) return 0;
            r = fp_host_finish32(c, r, (r & f.magnitude) < f.normal && !fp_host_zero(&f, r), e != 0.0f, &flags);
        } else if (fp_host_zero(&f, a) || (op == SNX_FP_DIV ? fp_host_inf(&f, b) : fp_host_zero(&f, b))) {
        } /* an exact zero, or an infinity times a number */
        else if (mul) {
            r = fp_host_finish32(c, r, size < fp_host_d(FP_HOST_TINY32), (double)s != p, &flags);
        } else {
            /* the double quotient is the smallest normal only when the single one is exactly (a single's inexact
               quotient is further from it than 2^-47 relative), else on the exact quotient's side of it */
            double q = (double)x / (double)y, m = q < 0 ? -q : q, tiny = fp_host_d(FP_HOST_TINY32);
            double rest = (double)x - (double)s * (double)y; /* exact: s is the rounded quotient */
            r = fp_host_finish32(c, r, m < tiny, rest != 0.0, &flags);
        }
    } else {
        double x = fp_host_d(a), y = fp_host_d(b);
        double s = op == SNX_FP_ADD ? x + y : sum ? x - y : mul ? x * y : x / y;
        r = fp_host_dbits(s);
        if (fp_host_nan(&f, r)) {
            if (op == SNX_FP_MULX)
                r = ((a ^ b) & f.sign) | 0x4000000000000000ull;
            else
                r = fp_host_invalid(64, &flags);
        } else if (fp_host_inf(&f, r)) {
            if (fp_host_inf(&f, a) || (op != SNX_FP_DIV && fp_host_inf(&f, b))) {
            } else if (op == SNX_FP_DIV && fp_host_zero(&f, b))
                flags |= FPSR_DZC;
            else
                flags |= FPSR_OFC | FPSR_IXC;
        } else if (sum) {
            double e = fp_host_err64(x, op == SNX_FP_ADD ? y : -y, s);
            int tiny = (r & f.magnitude) < f.normal && !fp_host_zero(&f, r);
            if (e - e != 0.0) return 0;
            if (tiny && (c->fpcr & FPCR_FZ)) {
                flags |= FPSR_UFC;
                r &= f.sign;
            } else if (e != 0.0)
                flags |= FPSR_IXC;
        } else if (fp_host_zero(&f, a) || (op == SNX_FP_DIV ? fp_host_inf(&f, b) : fp_host_zero(&f, b))) {
        } else if ((r & f.magnitude) <= f.normal || !(c->fpsr & FPSR_IXC))
            return 0; /* tiny, or IXC unknown */
    }
    return fp_host_done(c, op == SNX_FP_ABD ? r & ~f.sign : r, flags, out);
}

/* RN(v * 2^scale) as a single, v = p + z exactly (p, z doubles, s = p + z rounded, err its TwoSum error, both
   finite): s rounded to odd first, which a single rounds as it would v. Exact zeros take s's sign (IEEE's, the Arm
   ARM's in round to nearest) */
static u64 fp_host_fused32(const CPU* c, double s, double err, int scale, u32* flags) {
    u64 bits = fp_host_dbits(s);
    if (err != 0.0) { /* v lies beyond s, or short of it: truncated, the lowest bit set */
        if ((err < 0) != (s < 0)) bits -= 1;
        bits |= 1;
    }
    s = fp_host_d(bits);
    if (scale) s *= 0.5; /* exact in double precision */
    double m = s < 0 ? -s : s;
    if ((c->fpcr & FPCR_FZ) && m != 0.0 && m < fp_host_d(FP_HOST_TINY32)) { /* without the host's denormal */
        *flags |= FPSR_UFC;
        return (fp_host_dbits(s) >> 63) << 31;
    }
    float single = (float)s;
    u64 r = fp_host_fbits(single);
    if ((r & 0x7FFFFFFFu) == 0x7F800000u) {
        *flags |= FPSR_OFC | FPSR_IXC;
        return r;
    }
    if (s == 0.0) return r;
    return fp_host_finish32(c, r, m < fp_host_d(FP_HOST_TINY32), err != 0.0 || (double)single != s, flags);
}

/* FPMulAdd of singles: add + a * b, rounded once. NaNs: the signalling ones first, in the order add, a, b, then the
   quiet ones; a quiet NaN addend with an infinity times zero is invalid too */
SNX_INLINE int fp_host_fma(CPU* c, u64 add, u64 a, u64 b, int width, u64* out) {
    if (width != 32) return 0;
    FpHostFormat f = fp_host_format(32);
    u32 flags = 0;
    add = fp_host_flush(c, &f, add, &flags);
    a = fp_host_flush(c, &f, a, &flags);
    b = fp_host_flush(c, &f, b, &flags);
    int inf_zero = (fp_host_inf(&f, a) && fp_host_zero(&f, b)) || (fp_host_zero(&f, a) && fp_host_inf(&f, b));
    if (fp_host_nan(&f, add) || fp_host_nan(&f, a) || fp_host_nan(&f, b)) {
        if (fp_host_nan(&f, add) && !fp_host_snan(&f, add) && inf_zero)
            return fp_host_done(c, fp_host_invalid(32, &flags), flags, out);
        u64 pick = fp_host_snan(&f, add)  ? add
                   : fp_host_snan(&f, a)  ? a
                   : fp_host_snan(&f, b)  ? b
                   : fp_host_nan(&f, add) ? add
                   : fp_host_nan(&f, a)   ? a
                                          : b;
        return fp_host_done(c, fp_host_process(c, &f, 32, pick, &flags), flags, out);
    }
    int product_inf = fp_host_inf(&f, a) || fp_host_inf(&f, b);
    if (inf_zero || (fp_host_inf(&f, add) && product_inf && ((add ^ a ^ b) & f.sign)))
        return fp_host_done(c, fp_host_invalid(32, &flags), flags, out);
    if (fp_host_inf(&f, add)) return fp_host_done(c, add, flags, out);
    if (product_inf) return fp_host_done(c, f.inf | ((a ^ b) & f.sign), flags, out);
    double p = (double)fp_host_f(a) * (double)fp_host_f(b), z = (double)fp_host_f(add), s = p + z;
    return fp_host_done(c, fp_host_fused32(c, s, fp_host_err64(p, z, s), 0, &flags), flags, out);
}

/* FPRecipStepFused (2 - a * b) and FPRSqrtStepFused ((3 - a * b) / 2) of singles: a negated first */
static int fp_host_step(CPU* c, u64 a, u64 b, int width, int op, u64* out) {
    if (width != 32) return 0;
    FpHostFormat f = fp_host_format(32);
    u32 flags = 0;
    a = fp_host_flush(c, &f, a ^ f.sign, &flags);
    b = fp_host_flush(c, &f, b, &flags);
    if (fp_host_nan(&f, a) || fp_host_nan(&f, b))
        return fp_host_done(c, fp_host_nans(c, &f, 32, a, b, &flags), flags, out);
    if ((fp_host_inf(&f, a) && fp_host_zero(&f, b)) || (fp_host_zero(&f, a) && fp_host_inf(&f, b)))
        return fp_host_done(c, op == SNX_FP_RECPS ? 0x40000000u : 0x3FC00000u, flags, out); /* 2.0, 1.5 */
    if (fp_host_inf(&f, a) || fp_host_inf(&f, b)) return fp_host_done(c, f.inf | ((a ^ b) & f.sign), flags, out);
    double p = (double)fp_host_f(a) * (double)fp_host_f(b), z = op == SNX_FP_RECPS ? 2.0 : 3.0, s = p + z;
    return fp_host_done(c, fp_host_fused32(c, s, fp_host_err64(p, z, s), op == SNX_FP_RSQRTS, &flags), flags, out);
}

/* FPMax, FPMin, FPMaxNum and FPMinNum: integer tests and the host's ordering */
SNX_INLINE int fp_host_minmax(CPU* c, u64 a, u64 b, int width, int minimum, int numeric, u64* out) {
    FpHostFormat f = fp_host_format(width);
    u32 flags = 0;
    a = fp_host_flush(c, &f, a, &flags);
    b = fp_host_flush(c, &f, b, &flags);
    if (numeric) { /* a lone quiet NaN is the infinity that loses */
        u64 loses = f.inf | (minimum ? 0 : f.sign);
        if (fp_host_nan(&f, a) && !fp_host_snan(&f, a) && !fp_host_nan(&f, b))
            a = loses;
        else if (fp_host_nan(&f, b) && !fp_host_snan(&f, b) && !fp_host_nan(&f, a))
            b = loses;
    }
    if (fp_host_nan(&f, a) || fp_host_nan(&f, b))
        return fp_host_done(c, fp_host_nans(c, &f, width, a, b, &flags), flags, out);
    if (fp_host_zero(&f, a) && fp_host_zero(&f, b)) return fp_host_done(c, minimum ? a | b : a & b, flags, out);
    /* the winner, else b: equal numbers have one encoding */
    int wins = minimum ? (width == 32 ? fp_host_f(a) < fp_host_f(b) : fp_host_d(a) < fp_host_d(b))
                       : (width == 32 ? fp_host_f(a) > fp_host_f(b) : fp_host_d(a) > fp_host_d(b));
    return fp_host_done(c, wins ? a : b, flags, out);
}

/* FPCompare: NZCV 8 less, 6 equal, 2 greater, 3 unordered (IOC for a signalling NaN, any NaN when signal) */
static int fp_host_compare(CPU* c, u64 a, u64 b, int width, int signal, u32* out) {
    FpHostFormat f = fp_host_format(width);
    u32 flags = 0;
    a = fp_host_flush(c, &f, a, &flags);
    b = fp_host_flush(c, &f, b, &flags);
    if (fp_host_nan(&f, a) || fp_host_nan(&f, b)) {
        if (signal || fp_host_snan(&f, a) || fp_host_snan(&f, b)) flags |= FPSR_IOC;
        c->fpsr |= flags;
        *out = 3;
        return 1;
    }
    int less = width == 32 ? fp_host_f(a) < fp_host_f(b) : fp_host_d(a) < fp_host_d(b);
    int equal = width == 32 ? fp_host_f(a) == fp_host_f(b) : fp_host_d(a) == fp_host_d(b);
    c->fpsr |= flags;
    *out = less ? 8 : equal ? 6 : 2;
    return 1;
}

/* FPSqrt of everything but a positive number (the emitters' fast path, else the soft-float): NaNs, zeros of either
   sign, infinities and negative values */
static int fp_host_sqrt(CPU* c, u64 a, int width, u64* out) {
    FpHostFormat f = fp_host_format(width);
    u32 flags = 0;
    a = fp_host_flush(c, &f, a, &flags);
    if (fp_host_nan(&f, a)) return fp_host_done(c, fp_host_process(c, &f, width, a, &flags), flags, out);
    if (fp_host_zero(&f, a) || a == f.inf) return fp_host_done(c, a, flags, out);
    if (a & f.sign) return fp_host_done(c, fp_host_invalid(width, &flags), flags, out);
    return 0;
}

/* ---- four single lanes at a time: the paths above as the host's vector operations (SSE2, NEON) ----
   Each lane takes the same decisions as fp_host_binary, fp_host_fma, fp_host_step and fp_host_minmax, as masks: a
   lane's result is selected from the candidates of its case, and its flags are a vector the lanes in use are ORed
   from. A case the scalar paths leave to the soft-float (a sum's rounding error the host can't give) is a lane for
   fp_host_lanes to do alone. Under FPCR.FZ a tiny lane's operands are zeros before the host's operation, so it never
   makes a denormal */
typedef u32 FpHostU4 __attribute__((vector_size(16)));
typedef int32_t FpHostM4 __attribute__((vector_size(16))); /* a lane mask: all ones or zero */
typedef float FpHostF4 __attribute__((vector_size(16)));
typedef double FpHostD4 __attribute__((vector_size(32)));
typedef u64 FpHostQ4 __attribute__((vector_size(32)));
typedef int64_t FpHostL4 __attribute__((vector_size(32)));

#define FP_HOST_DNAN32 0x7FC00000u

static FpHostU4 fp_host_sel4(FpHostM4 m, FpHostU4 a, FpHostU4 b) { return (a & (FpHostU4)m) | (b & ~(FpHostU4)m); }
static FpHostD4 fp_host_wide4(FpHostF4 f) { return __builtin_convertvector(f, FpHostD4); }
static FpHostM4 fp_host_mask4(FpHostL4 m) { return __builtin_convertvector(m, FpHostM4); }
static FpHostU4 fp_host_flag4(FpHostM4 m, u32 flags) { return (FpHostU4)m & flags; }
/* the lanes of a double vector where m is set, the others zeros */
static FpHostD4 fp_host_keep4(FpHostM4 m, FpHostD4 v) {
    FpHostL4 keep = __builtin_convertvector(m, FpHostL4);
    return (FpHostD4)((FpHostQ4)v & (FpHostQ4)keep);
}
/* a sign bit for each lane of v that is negative */
static FpHostU4 fp_host_sign4(FpHostD4 v) { return (FpHostU4)fp_host_mask4(v < 0.0) & 0x80000000u; }
/* |v| < 2^-126, v nonzero (false for a NaN) */
static FpHostM4 fp_host_tiny4(FpHostD4 v) {
    FpHostD4 m = (FpHostD4)((FpHostQ4)v & 0x7FFFFFFFFFFFFFFFull);
    return fp_host_mask4((m < fp_host_d(FP_HOST_TINY32)) & (m != 0.0));
}

/* fp_host_flush of four lanes */
static FpHostU4 fp_host_flush4(const CPU* c, FpHostU4 x, FpHostU4* flags) {
    FpHostU4 m = x & 0x7FFFFFFFu;
    if (!(c->fpcr & FPCR_FZ)) return x;
    FpHostM4 denormal = (m != 0) & (m < 0x00800000u);
    *flags |= fp_host_flag4(denormal, FPSR_IDC);
    return fp_host_sel4(denormal, x & 0x80000000u, x);
}
static FpHostM4 fp_host_nan4(FpHostU4 x) { return (x & 0x7FFFFFFFu) > 0x7F800000u; }
static FpHostM4 fp_host_snan4(FpHostU4 x) { return fp_host_nan4(x) & ((x & 0x00400000u) == 0); }
/* fp_host_process of a NaN picked in each lane */
static FpHostU4 fp_host_process4(const CPU* c, FpHostU4 pick, FpHostU4* flags) {
    FpHostU4 dnan = {FP_HOST_DNAN32, FP_HOST_DNAN32, FP_HOST_DNAN32, FP_HOST_DNAN32};
    *flags |= fp_host_flag4(fp_host_snan4(pick), FPSR_IOC);
    return c->fpcr & FPCR_DN ? dnan : pick | 0x00400000u;
}
/* fp_host_nans of four lanes (the lanes without a NaN get b's operand, to be replaced) */
static FpHostU4 fp_host_nans4(const CPU* c, FpHostU4 a, FpHostU4 b, FpHostU4* flags) {
    FpHostU4 pick =
        fp_host_sel4(fp_host_snan4(a), a, fp_host_sel4(fp_host_snan4(b), b, fp_host_sel4(fp_host_nan4(a), a, b)));
    return fp_host_process4(c, pick, flags);
}

/* fp_host_binary of four single lanes (op ADD, SUB, MUL, DIV, MULX, ABD) */
static FpHostU4 fp_host_binary4(const CPU* c, FpHostU4 a, FpHostU4 b, int op, FpHostU4* flags, FpHostM4* slow) {
    int sum = op == SNX_FP_ADD || op == SNX_FP_SUB || op == SNX_FP_ABD, mul = op == SNX_FP_MUL || op == SNX_FP_MULX;
    FpHostU4 f = {0, 0, 0, 0};
    a = fp_host_flush4(c, a, &f);
    b = fp_host_flush4(c, b, &f);
    FpHostU4 ma = a & 0x7FFFFFFFu, mb = b & 0x7FFFFFFFu;
    FpHostM4 nan = fp_host_nan4(a) | fp_host_nan4(b);
    FpHostU4 nans = fp_host_nans4(c, a, b, &f);
    FpHostF4 x = (FpHostF4)a, y = op == SNX_FP_ADD || !sum ? (FpHostF4)b : -(FpHostF4)b;
    FpHostD4 wx = fp_host_wide4(x), wy = fp_host_wide4(y);
    /* a product, a sum (exact as a double where tiny) and a quotient: under FZ a tiny one is a zero of its sign */
    FpHostD4 p = mul ? wx * wy : sum ? wx + wy : wx / wy;
    FpHostM4 tiny = fp_host_tiny4(p), flushed = c->fpcr & FPCR_FZ ? tiny & ~nan : (FpHostM4){0, 0, 0, 0};
    FpHostF4 kx = (FpHostF4)((FpHostU4)x & ~(FpHostU4)flushed), ky = (FpHostF4)((FpHostU4)y & ~(FpHostU4)flushed);
    FpHostF4 s = sum ? kx + ky : mul ? __builtin_convertvector(fp_host_keep4(~flushed, p), FpHostF4) : kx / y;
    FpHostU4 r = (FpHostU4)s, mr = r & 0x7FFFFFFFu;
    FpHostM4 r_nan = (mr > 0x7F800000u) & ~nan, r_inf = (mr == 0x7F800000u) & ~nan;
    /* inf - inf, 0 * inf, 0 / 0, inf / inf: the default NaN and IOC, FPMulX's 2.0 */
    if (op == SNX_FP_MULX)
        r = fp_host_sel4(r_nan, ((a ^ b) & 0x80000000u) | 0x40000000u, r);
    else {
        f |= fp_host_flag4(r_nan, FPSR_IOC);
        r = fp_host_sel4(r_nan, (FpHostU4){FP_HOST_DNAN32, FP_HOST_DNAN32, FP_HOST_DNAN32, FP_HOST_DNAN32}, r);
    }
    /* infinities: exact from an infinite operand, DZC from a division by zero, else an overflow */
    FpHostM4 exact = (ma == 0x7F800000u) | (op != SNX_FP_DIV ? mb == 0x7F800000u : (FpHostM4){0, 0, 0, 0});
    FpHostM4 by_zero = op == SNX_FP_DIV ? mb == 0 : (FpHostM4){0, 0, 0, 0};
    f |= fp_host_flag4(r_inf & ~exact & by_zero, FPSR_DZC) |
         fp_host_flag4(r_inf & ~exact & ~by_zero, FPSR_OFC | FPSR_IXC);
    /* finite results: their inexactness, none for an exact zero (a zero operand, or an infinite divisor) */
    FpHostM4 finite = ~(nan | r_nan | r_inf), inexact;
    if (sum) {
        FpHostF4 t = s - kx, e = (kx - (s - t)) + (ky - t);
        inexact = e != 0.0f;
        *slow |= finite & ~flushed & ((e - e) != 0.0f); /* an intermediate overflowed */
    } else {
        FpHostM4 zero = (ma == 0) | (op == SNX_FP_DIV ? mb == 0x7F800000u : mb == 0);
        inexact = mul ? fp_host_mask4(fp_host_wide4(s) != p) : fp_host_mask4(wx - fp_host_wide4(s) * wy != 0.0);
        inexact &= ~zero;
    }
    inexact &= finite & ~flushed;
    f |= fp_host_flag4(finite & flushed, FPSR_UFC) | fp_host_flag4(inexact, FPSR_IXC) |
         fp_host_flag4(inexact & tiny, FPSR_UFC);
    r = fp_host_sel4(finite & flushed, fp_host_sign4(p), r);
    r = fp_host_sel4(nan, nans, r);
    *flags |= f;
    return op == SNX_FP_ABD ? r & 0x7FFFFFFFu : r;
}

/* fp_host_fused32 of four lanes: RN(v * 2^-scale) as a single, v = p + z exactly (p and z doubles, finite where
   fused is set), with its flags in the lanes of fused */
static FpHostU4 fp_host_fused4(const CPU* c, FpHostD4 p, FpHostD4 z, int scale, FpHostM4 fused, FpHostU4* flags) {
    FpHostD4 s = p + z, t = s - p, err = (p - (s - t)) + (z - t);
    FpHostL4 rounded = err != 0.0;
    FpHostQ4 bits = ((FpHostQ4)s + (FpHostQ4)(rounded & ((err < 0.0) ^ (s < 0.0)))) | ((FpHostQ4)rounded & 1);
    s = (FpHostD4)bits;
    if (scale) s *= 0.5; /* exact in double precision */
    FpHostM4 tiny = fp_host_tiny4(s) & fused, flushed = c->fpcr & FPCR_FZ ? tiny : (FpHostM4){0, 0, 0, 0};
    FpHostF4 single = __builtin_convertvector(fp_host_keep4(~flushed, s), FpHostF4);
    FpHostU4 r = (FpHostU4)single;
    FpHostM4 over = fused & ~flushed & ((r & 0x7FFFFFFFu) == 0x7F800000u);
    /* rounded to odd, s is a single only where it is exact */
    FpHostM4 inexact = fused & ~flushed & ~over & fp_host_mask4(fp_host_wide4(single) != s);
    *flags |= fp_host_flag4(flushed, FPSR_UFC) | fp_host_flag4(over, FPSR_OFC | FPSR_IXC) |
              fp_host_flag4(inexact, FPSR_IXC) | fp_host_flag4(inexact & tiny, FPSR_UFC);
    return fp_host_sel4(flushed, fp_host_sign4(s), r);
}

/* fp_host_fma of four single lanes: add + a * b */
static FpHostU4 fp_host_fma4(const CPU* c, FpHostU4 add, FpHostU4 a, FpHostU4 b, FpHostU4* flags) {
    FpHostU4 f = {0, 0, 0, 0}, sign = f + 0x80000000u,
             dnan = {FP_HOST_DNAN32, FP_HOST_DNAN32, FP_HOST_DNAN32, FP_HOST_DNAN32};
    add = fp_host_flush4(c, add, &f);
    a = fp_host_flush4(c, a, &f);
    b = fp_host_flush4(c, b, &f);
    FpHostU4 madd = add & 0x7FFFFFFFu, ma = a & 0x7FFFFFFFu, mb = b & 0x7FFFFFFFu;
    FpHostM4 inf_add = madd == 0x7F800000u, inf_a = ma == 0x7F800000u, inf_b = mb == 0x7F800000u;
    FpHostM4 inf_zero = (inf_a & (mb == 0)) | ((ma == 0) & inf_b);
    FpHostM4 nan_add = fp_host_nan4(add), nan = nan_add | fp_host_nan4(a) | fp_host_nan4(b);
    /* NaNs: the signalling ones in the order add, a, b, then the quiet ones; a quiet addend with inf * 0 invalid */
    FpHostU4 pick =
        fp_host_sel4(fp_host_snan4(add), add,
                     fp_host_sel4(fp_host_snan4(a), a,
                                  fp_host_sel4(fp_host_snan4(b), b,
                                               fp_host_sel4(nan_add, add, fp_host_sel4(fp_host_nan4(a), a, b)))));
    FpHostU4 nan_flags = {0, 0, 0, 0}, nans = fp_host_process4(c, pick, &nan_flags);
    FpHostM4 quiet_invalid = nan_add & ~fp_host_snan4(add) & inf_zero;
    f |= (nan_flags & (FpHostU4)~quiet_invalid) | fp_host_flag4(quiet_invalid, FPSR_IOC);
    nans = fp_host_sel4(quiet_invalid, dnan, nans);
    FpHostM4 product_inf = inf_a | inf_b;
    FpHostM4 invalid = ~nan & (inf_zero | (inf_add & product_inf & (((add ^ a ^ b) & sign) != 0)));
    FpHostM4 infinite = ~nan & ~invalid & (inf_add | product_inf), fused = ~(nan | invalid | infinite);
    f |= fp_host_flag4(invalid, FPSR_IOC);
    /* the product exact as a double */
    FpHostD4 p = fp_host_wide4((FpHostF4)a) * fp_host_wide4((FpHostF4)b), z = fp_host_wide4((FpHostF4)add);
    FpHostU4 r = fp_host_fused4(c, p, z, 0, fused, &f);
    r = fp_host_sel4(infinite, fp_host_sel4(inf_add, add, 0x7F800000u | ((a ^ b) & sign)), r);
    r = fp_host_sel4(invalid, dnan, r);
    r = fp_host_sel4(nan, nans, r);
    *flags |= f;
    return r;
}

/* fp_host_step of four single lanes: FPRecipStepFused (2 - a * b, op RECPS) or FPRSqrtStepFused ((3 - a * b) / 2) */
static FpHostU4 fp_host_step4(const CPU* c, FpHostU4 a, FpHostU4 b, int op, FpHostU4* flags) {
    FpHostU4 f = {0, 0, 0, 0};
    a = fp_host_flush4(c, a ^ 0x80000000u, &f);
    b = fp_host_flush4(c, b, &f);
    FpHostU4 ma = a & 0x7FFFFFFFu, mb = b & 0x7FFFFFFFu;
    FpHostM4 nan = fp_host_nan4(a) | fp_host_nan4(b);
    FpHostU4 nans = fp_host_nans4(c, a, b, &f);
    FpHostM4 inf_a = ma == 0x7F800000u, inf_b = mb == 0x7F800000u;
    FpHostM4 inf_zero = ~nan & ((inf_a & (mb == 0)) | ((ma == 0) & inf_b)); /* 2.0, 1.5 */
    FpHostM4 infinite = ~nan & ~inf_zero & (inf_a | inf_b), fused = ~(nan | inf_zero | infinite);
    double k = op == SNX_FP_RECPS ? 2.0 : 3.0;
    FpHostD4 p = fp_host_wide4((FpHostF4)a) * fp_host_wide4((FpHostF4)b), z = {k, k, k, k};
    FpHostU4 r = fp_host_fused4(c, p, z, op == SNX_FP_RSQRTS, fused, &f);
    r = fp_host_sel4(infinite, 0x7F800000u | ((a ^ b) & 0x80000000u), r);
    r = fp_host_sel4(inf_zero, (a & 0) | (op == SNX_FP_RECPS ? 0x40000000u : 0x3FC00000u), r);
    r = fp_host_sel4(nan, nans, r);
    *flags |= f;
    return r;
}

/* fp_host_minmax of four single lanes */
static FpHostU4 fp_host_minmax4(const CPU* c, FpHostU4 a, FpHostU4 b, int minimum, int numeric, FpHostU4* flags) {
    FpHostU4 f = {0, 0, 0, 0};
    a = fp_host_flush4(c, a, &f);
    b = fp_host_flush4(c, b, &f);
    if (numeric) { /* a lone quiet NaN is the infinity that loses */
        FpHostU4 loses = (a & 0) | 0x7F800000u | (minimum ? 0 : 0x80000000u);
        FpHostM4 quiet_a = fp_host_nan4(a) & ~fp_host_snan4(a), quiet_b = fp_host_nan4(b) & ~fp_host_snan4(b);
        FpHostM4 lone_a = quiet_a & ~fp_host_nan4(b), lone_b = quiet_b & ~fp_host_nan4(a);
        a = fp_host_sel4(lone_a, loses, a);
        b = fp_host_sel4(lone_b, loses, b);
    }
    FpHostM4 nan = fp_host_nan4(a) | fp_host_nan4(b);
    FpHostU4 nans = fp_host_nans4(c, a, b, &f);
    FpHostM4 zeros = ((a & 0x7FFFFFFFu) == 0) & ((b & 0x7FFFFFFFu) == 0);
    FpHostF4 fa = (FpHostF4)a, fb = (FpHostF4)b;
    FpHostM4 wins = minimum ? fa < fb : fa > fb;
    FpHostU4 r = fp_host_sel4(zeros, minimum ? a | b : a & b, fp_host_sel4(wins, a, b));
    *flags |= f;
    return fp_host_sel4(nan, nans, r);
}
#endif

/* ---- the entry points (cpu.h): the host's result where the above takes the operation, else the soft-float's ---- */

/* one lane of helper (0 binary, 1 FMA: a + b * d, 2 unary, 3 minmax) through the paths above where ready (the
   host's environment, and the width) and rounded (and FPCR's rounding) allow them, else the soft-float */
SNX_INLINE u64 fp_host_lane(CPU* c, unsigned helper, int width, int op, int numeric, int ready, int rounded, u64 a,
                            u64 b, u64 d) {
#ifdef FP_HOST_ON
    u64 r;
#else
    (void)ready;
    (void)rounded;
#endif
    switch (helper) {
        case 0:
#ifdef FP_HOST_ON
            if (rounded && (op == SNX_FP_RECPS || op == SNX_FP_RSQRTS ? fp_host_step(c, a, b, width, op, &r)
                                                                      : fp_host_binary(c, a, b, width, op, &r)))
                return r;
#endif
            return snx_fp_binary(c, a, b, width, op);
        case 1:
#ifdef FP_HOST_ON
            if (rounded && fp_host_fma(c, a, b, d, width, &r)) return r;
#endif
            return snx_fp_fma(c, a, b, d, width);
        case 2:
#ifdef FP_HOST_ON
            if (ready && op == SNX_FP_SQRT && fp_host_sqrt(c, a, width, &r)) return r;
#endif
            return snx_fp_unary(c, a, width, op);
        default:
#ifdef FP_HOST_ON
            if (ready && fp_host_minmax(c, a, b, width, op, numeric, &r)) return r;
#endif
            return snx_fp_minmax(c, a, b, width, op, numeric);
    }
}
#ifdef FP_HOST_ON
#define FP_HOST_READY(c, width, rounded) fp_host_ready(c, width, rounded)
#else
#define FP_HOST_READY(c, width, rounded) 0
#endif

u64 snx_fp_host_binary(CPU* c, u64 a, u64 b, int width, int op) {
    return fp_host_lane(c, 0, width, op, 0, 0, FP_HOST_READY(c, width, 1), a, b, 0);
}
u64 snx_fp_host_fma(CPU* c, u64 add, u64 a, u64 b, int width) {
    return fp_host_lane(c, 1, width, 0, 0, 0, FP_HOST_READY(c, width, 1), add, a, b);
}
u64 snx_fp_host_unary(CPU* c, u64 bits, int width, int op) {
    return fp_host_lane(c, 2, width, op, 0, op == SNX_FP_SQRT && FP_HOST_READY(c, width, 0), 0, bits, 0, 0);
}
u64 snx_fp_host_minmax(CPU* c, u64 a, u64 b, int width, int minimum, int numeric) {
    return fp_host_lane(c, 3, width, minimum, numeric, FP_HOST_READY(c, width, 0), 0, a, b, 0);
}
u32 snx_fp_host_compare(CPU* c, u64 a, u64 b, int width, int signal) {
#ifdef FP_HOST_ON
    u32 r;
    if (fp_host_ready(c, width, 0) && fp_host_compare(c, a, b, width, signal, &r)) return r;
#endif
    return snx_fp_compare(c, a, b, width, signal);
}

/* one lane of a vector through its entry point above */
static u64 fp_host_each(CPU* c, unsigned helper, int width, int op, int numeric, u64 a, u64 b, u64 d) {
    switch (helper) {
        case 0: return snx_fp_host_binary(c, a, b, width, op);
        case 1: return snx_fp_host_fma(c, a, b, d, width);
        case 2: return snx_fp_host_unary(c, a, width, op);
        default: return snx_fp_host_minmax(c, a, b, width, op, numeric);
    }
}
/* a lane of width bits at byte offset at, read and written as a number of its width (the same on a host of either
   byte order) */
static u64 fp_host_get(const char* v, unsigned at, int width) {
    if (width == 16) {
        u16 h;
        __builtin_memcpy(&h, v + at, 2);
        return h;
    }
    if (width == 32) {
        u32 s;
        __builtin_memcpy(&s, v + at, 4);
        return s;
    }
    u64 d;
    __builtin_memcpy(&d, v + at, 8);
    return d;
}
static void fp_host_put(char* v, unsigned at, int width, u64 x) {
    if (width == 16) {
        u16 h = (u16)x;
        __builtin_memcpy(v + at, &h, 2);
    } else if (width == 32) {
        u32 s = (u32)x;
        __builtin_memcpy(v + at, &s, 4);
    } else
        __builtin_memcpy(v + at, &x, 8);
}

#ifdef FP_HOST_ON
/* up to four single lanes through the four-lane paths, where the host's environment and FPCR's rounding allow them
   (else 0, nothing done); the lanes they leave go one at a time */
static int fp_host_vector4(CPU* c, char* out, const char* x, const char* y, const char* z, unsigned helper,
                           unsigned lanes, int op, int numeric) {
    if (helper == 2 || !fp_host_ready(c, 32, helper != 3)) return 0;
    FpHostU4 a = {0, 0, 0, 0}, b = a, d = a, flags = a, r;
    FpHostM4 slow = {0, 0, 0, 0}, used = (FpHostM4){0, 1, 2, 3} < (int32_t)lanes;
    if (lanes == 4) { /* copies of a constant size, which need no call */
        __builtin_memcpy(&a, x, 16);
        __builtin_memcpy(&b, y, 16);
        __builtin_memcpy(&d, z, 16);
    } else {
        __builtin_memcpy(&a, x, 8);
        __builtin_memcpy(&b, y, 8);
        __builtin_memcpy(&d, z, 8);
    }
    if (helper == 0 && (op == SNX_FP_RECPS || op == SNX_FP_RSQRTS))
        r = fp_host_step4(c, a, b, op, &flags);
    else if (helper == 0)
        r = fp_host_binary4(c, a, b, op, &flags, &slow);
    else if (helper == 1)
        r = fp_host_fma4(c, a, b, d, &flags);
    else
        r = fp_host_minmax4(c, a, b, op, numeric, &flags);
    slow &= used;
    flags &= (FpHostU4)(used & ~slow);
    c->fpsr |= flags[0] | flags[1] | flags[2] | flags[3];
    if (slow[0] | slow[1] | slow[2] | slow[3])
        for (unsigned i = 0; i < lanes; ++i)
            if (slow[i]) r[i] = (u32)fp_host_each(c, helper, 32, op, numeric, a[i], b[i], d[i]);
    if (lanes == 4)
        __builtin_memcpy(out, &r, 16);
    else
        __builtin_memcpy(out, &r, 8);
    return 1;
}
#endif

/* the lanes of a vector: desc's nibbles are the helper (0 binary, 1 FMA, x + y * z, 2 unary, 3 minmax), the width in
   bytes, the lane count, the operation (minmax's minimum) and minmax's numeric; x, y, z and out hold the lanes in
   memory, lane 0 first. Singles four lanes at a time where they can be, the rest one lane at a time */
void snx_fp_host_lanes(CPU* c, void* out, const void* x, const void* y, const void* z, u32 desc) {
    unsigned helper = desc & 15, bytes = desc >> 4 & 15, lanes = desc >> 8 & 15;
    int op = (int)(desc >> 12 & 15), numeric = (int)(desc >> 16 & 15), width = (int)bytes * 8;
    const char *a = (const char*)x, *b = (const char*)y, *d = (const char*)z;
#ifdef FP_HOST_ON
    if (bytes == 4 && (lanes == 2 || lanes == 4) && fp_host_vector4(c, (char*)out, a, b, d, helper, lanes, op, numeric))
        return;
#endif
    for (unsigned i = 0; i < lanes; ++i)
        fp_host_put((char*)out, i * bytes, width,
                    fp_host_each(c, helper, width, op, numeric, fp_host_get(a, i * bytes, width),
                                 fp_host_get(b, i * bytes, width), fp_host_get(d, i * bytes, width)));
}
