/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The helpers' host fast paths (helpers/fp_host.c) against the soft-float they front (helpers/fp.c): every result's
   bits and FPSR, for operands drawn to reach each case they take on (NaNs signalling and quiet, infinities, zeros,
   denormals, the normal range's ends, products and quotients around the smallest normal, sums that cancel, fused
   sums whose double rounding would land on a single's tie) and the ones they leave to the soft-float, under FPCR's
   FZ, DN and rounding modes, with FPSR's flags set and clear; in the host's default FP environment, and in its other
   rounding modes and flushes, which the fast paths must leave to the soft-float */
#include "common/cpu/aarch64/cpu.h"
#include <fenv.h>
#include <stdio.h>

static struct {
    unsigned failures, checks;
    u64 seed;
} g_test = {0, 0, 0x9E3779B97F4A7C15ull};
static u64 next(void) {
    g_test.seed ^= g_test.seed << 13;
    g_test.seed ^= g_test.seed >> 7;
    g_test.seed ^= g_test.seed << 17;
    return g_test.seed;
}

static int fbits(int width) { return width == 32 ? 23 : 52; }
static u64 make(int width, u64 sign, u64 exponent, u64 fraction) {
    return sign << (width - 1) | exponent << fbits(width) | (fraction & (((u64)1 << fbits(width)) - 1));
}
/* an operand: one of the classes the paths tell apart, or an exponent near e (the products' and quotients'
   neighbourhoods of the smallest normal) */
static u64 operand(int width, int near) {
    u64 r = next(), sign = r >> 63, fraction = next(), emax = width == 32 ? 255 : 2047, bias = emax >> 1;
    switch (r % 24) {
        case 0: return make(width, sign, 0, 0);
        case 1: return make(width, sign, 0, fraction | 1); /* a denormal */
        case 2: return make(width, sign, emax, 0);
        case 3: return make(width, sign, emax, fraction | (u64)1 << (fbits(width) - 1));               /* quiet NaN */
        case 4: return make(width, sign, emax, (fraction & (((u64)1 << (fbits(width) - 1)) - 1)) | 1); /* signalling */
        case 5: return make(width, sign, 1, fraction);
        case 6: return make(width, sign, emax - 1, fraction);
        case 7: return make(width, sign, 1, 0);                      /* the smallest normal */
        case 8: return make(width, sign, bias, fraction & 0xF);      /* few bits: exact products and sums */
        case 9: return next() & (width == 32 ? 0xFFFFFFFFu : ~0ull); /* anything */
        default: {
            /* around 2^near: products and quotients of two of them land near the smallest normal, or overflow */
            long e = (long)bias + near + (long)(next() % 21) - 10;
            if (e < 1) e = 1;
            if (e > (long)emax - 1) e = (long)emax - 1;
            return make(width, sign, (u64)e, fraction);
        }
    }
}
static u32 control(void) {
    u64 r = next();
    u32 fpcr = 0;
    if (r & 1) fpcr |= 1u << 24;                     /* FZ */
    if (r & 2) fpcr |= 1u << 25;                     /* DN */
    if (r % 7 == 0) fpcr |= (u32)(next() % 4) << 22; /* RMode */
    return fpcr;
}
static u32 status(void) { return (u32)(next() & 0x9Fu) | 0x08000000u; } /* the cumulative flags, and QC kept */

static void report(const char* what, int width, int op, u64 a, u64 b, u64 d, u32 fpcr, u32 fpsr, u64 got, u64 want,
                   u32 got_fpsr, u32 want_fpsr) {
    if (g_test.failures++ >= 20) return;
    fprintf(stderr,
            "%s/%d op %d: %016llx %016llx %016llx FPCR %08x FPSR %08x: %016llx FPSR %08x, soft-float %016llx FPSR "
            "%08x\n",
            what, width, op, (unsigned long long)a, (unsigned long long)b, (unsigned long long)d, fpcr, fpsr,
            (unsigned long long)got, got_fpsr, (unsigned long long)want, want_fpsr);
}
static CPU state(u32 fpcr, u32 fpsr) {
    CPU c = {0};
    c.fpcr = fpcr;
    c.fpsr = fpsr;
    return c;
}

static void binary(int width, int op, u64 a, u64 b, u32 fpcr, u32 fpsr) {
    CPU host = state(fpcr, fpsr), soft = host;
    u64 got = snx_fp_host_binary(&host, a, b, width, op), want = snx_fp_binary(&soft, a, b, width, op);
    g_test.checks++;
    if (got != want || host.fpsr != soft.fpsr)
        report("binary", width, op, a, b, 0, fpcr, fpsr, got, want, host.fpsr, soft.fpsr);
}
static void fma(int width, u64 add, u64 a, u64 b, u32 fpcr, u32 fpsr) {
    CPU host = state(fpcr, fpsr), soft = host;
    u64 got = snx_fp_host_fma(&host, add, a, b, width), want = snx_fp_fma(&soft, add, a, b, width);
    g_test.checks++;
    if (got != want || host.fpsr != soft.fpsr)
        report("fma", width, 0, add, a, b, fpcr, fpsr, got, want, host.fpsr, soft.fpsr);
}
static void others(int width, u64 a, u64 b, u32 fpcr, u32 fpsr) {
    int k;
    for (k = 0; k < 4; ++k) {
        CPU host = state(fpcr, fpsr), soft = host;
        u64 got = snx_fp_host_minmax(&host, a, b, width, k & 1, k >> 1),
            want = snx_fp_minmax(&soft, a, b, width, k & 1, k >> 1);
        g_test.checks++;
        if (got != want || host.fpsr != soft.fpsr)
            report("minmax", width, k, a, b, 0, fpcr, fpsr, got, want, host.fpsr, soft.fpsr);
    }
    for (k = 0; k < 2; ++k) {
        CPU host = state(fpcr, fpsr), soft = host;
        u32 got = snx_fp_host_compare(&host, a, b, width, k), want = snx_fp_compare(&soft, a, b, width, k);
        g_test.checks++;
        if (got != want || host.fpsr != soft.fpsr)
            report("compare", width, k, a, b, 0, fpcr, fpsr, got, want, host.fpsr, soft.fpsr);
    }
    for (k = SNX_FP_SQRT; k <= SNX_FP_RECPX; ++k) {
        CPU host = state(fpcr, fpsr), soft = host;
        u64 got = snx_fp_host_unary(&host, a, width, k), want = snx_fp_unary(&soft, a, width, k);
        g_test.checks++;
        if (got != want || host.fpsr != soft.fpsr)
            report("unary", width, k, a, 0, 0, fpcr, fpsr, got, want, host.fpsr, soft.fpsr);
    }
}

/* fused sums whose double rounding a single would see: a * b = 1 + 2^-11 + 2^-24, a single's tie, and an addend far
   below it that decides the tie, of either sign; and products that cancel the addend to a tiny or zero result */
static void fused_edges(void) {
    static const u32 addends[] = {0x00000000u, 0x80000000u, 0x16800000u, 0x96800000u,
                                  0x0D800000u, 0x8D800000u, 0x00000001u, 0x80000001u};
    u32 one = 0x3F800800u; /* 1 + 2^-12 */
    unsigned i;
    for (i = 0; i < sizeof addends / sizeof *addends; ++i) {
        u32 s;
        for (s = 0; s < 4; ++s) {
            fma(32, addends[i], one ^ (s & 1 ? 0x80000000u : 0), one ^ (s & 2 ? 0x80000000u : 0), 0, 0);
            fma(32, addends[i], one, one, 1u << 24, 0);
        }
    }
    for (i = 0; i < 200000; ++i) {
        u32 fpcr = control(), fpsr = status();
        u64 a = operand(32, -63), b = operand(32, -63), add;
        CPU c = state(0, 0);
        u64 p = snx_fp_binary(&c, a, b, 32, SNX_FP_MUL);
        add = ((p ^ 0x80000000u) + (next() % 5) - 2) & 0xFFFFFFFFu; /* near minus the rounded product */
        fma(32, add, a, b, fpcr, fpsr);
        fma(32, operand(32, -126), a, b, fpcr, fpsr);
    }
}

/* results at the smallest normal exactly, which is not tiny, and just below it, which is, for each width */
static void normal_edges(void) {
    static const struct {
        int width, op;
        u64 a, b;
    } t[] = {
        {32, SNX_FP_MUL, 0x20000000u, 0x20000000u}, /* 2^-63 * 2^-63 */
        {32, SNX_FP_MUL, 0x20000000u, 0x1FFFFFFFu}, /* just below */
        {32, SNX_FP_MUL, 0x1FFFFFFFu, 0x20000001u}, /* below, rounding to it */
        {32, SNX_FP_DIV, 0x01000000u, 0x40000000u}, /* 2^-125 / 2 */
        {32, SNX_FP_DIV, 0x01000000u, 0x40000001u},
        {32, SNX_FP_DIV, 0x01400000u, 0x40400000u}, /* 3 * 2^-126 / 3 */
        {32, SNX_FP_DIV, 0x01400001u, 0x40400000u},
        {32, SNX_FP_DIV, 0x013FFFFFu, 0x40400000u},
        {32, SNX_FP_SUB, 0x01000000u, 0x00800000u}, /* 2^-125 - 2^-126 */
        {32, SNX_FP_SUB, 0x01000000u, 0x00800001u},
        {64, SNX_FP_MUL, 0x2000000000000000ull, 0x2000000000000000ull},
        {64, SNX_FP_DIV, 0x0020000000000000ull, 0x4000000000000000ull},
        {64, SNX_FP_SUB, 0x0020000000000000ull, 0x0010000000000000ull},
        {64, SNX_FP_SUB, 0x0020000000000000ull, 0x0010000000000001ull},
    };
    unsigned i, k;
    for (i = 0; i < sizeof t / sizeof *t; ++i)
        for (k = 0; k < 8; ++k) {
            u64 sign = (u64)(k & 1) << (t[i].width - 1);
            u32 fpcr = k & 2 ? 1u << 24 : 0, fpsr = k & 4 ? 0x10u : 0;
            binary(t[i].width, t[i].op, t[i].a ^ sign, t[i].b, fpcr, fpsr);
            binary(t[i].width, t[i].op == SNX_FP_SUB ? SNX_FP_ABD : SNX_FP_MULX, t[i].a ^ sign, t[i].b, fpcr, fpsr);
        }
    /* fused sums landing on the smallest normal: 2^-63 * 2^-63 + 0, and a product cancelled to it */
    fma(32, 0, 0x20000000u, 0x20000000u, 1u << 24, 0);
    fma(32, 0x80000000u, 0x20000000u, 0x1FFFFFFFu, 1u << 24, 0);
    fma(32, 0x01000000u, 0x80800000u, 0x3F800000u, 1u << 24, 0); /* 2^-125 - 2^-126 */
    fma(32, 0x01000000u, 0x80800001u, 0x3F800000u, 1u << 24, 0);
}

static void draws(unsigned count) {
    static const int nears[] = {0, -63, 63, -126, -20};
    int width, op, n;
    unsigned i;
    for (width = 32; width <= 64; width += 32)
        for (n = 0; n < (int)(sizeof nears / sizeof *nears); ++n) {
            int near = width == 32 ? nears[n] : nears[n] * 8;
            for (i = 0; i < count; ++i) {
                u32 fpcr = control(), fpsr = status();
                u64 a = operand(width, near), b = operand(width, near), d = operand(width, 2 * near);
                for (op = SNX_FP_ADD; op <= SNX_FP_RSQRTS; ++op) binary(width, op, a, b, fpcr, fpsr);
                /* sums that cancel: b near -a */
                binary(width, SNX_FP_ADD, a,
                       ((a ^ (u64)1 << (width - 1)) + (next() % 3) - 1) & (width == 32 ? 0xFFFFFFFFu : ~0ull), fpcr,
                       fpsr);
                /* quotients near the smallest normal */
                binary(width, SNX_FP_DIV, operand(width, near - (width == 32 ? 126 : 1022)), operand(width, near), fpcr,
                       fpsr);
                fma(width, d, a, b, fpcr, fpsr);
                others(width, a, b, fpcr, fpsr);
            }
        }
}

/* the cases, in the host's default FP environment and then in the others, where the host paths must notice and
   leave everything to the soft-float */
int main(void) {
    static const int modes[] = {FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    const int original = fegetround();
    unsigned m;
    fused_edges();
    normal_edges();
    draws(100000);
    for (m = 0; m < sizeof modes / sizeof *modes; ++m) {
        if (fesetround(modes[m])) return 2;
        fused_edges();
        normal_edges();
        draws(5000);
    }
    fesetround(original);
#if defined(__x86_64__)
    {
        unsigned csr = __builtin_ia32_stmxcsr();
        __builtin_ia32_ldmxcsr(csr | 0x8040u); /* FTZ and DAZ */
        normal_edges();
        draws(5000);
        __builtin_ia32_ldmxcsr(csr);
    }
#endif
    printf("Host FP fast paths against the soft-float: %u checks, %u failed\n", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
