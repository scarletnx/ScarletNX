/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Fixed witnesses from ARM DDI0487A.k shared FP pseudocode and A57 DDI0488H.
   No host floating-point literals or software-emulator dependency. */
#include "common/cpu/aarch64/cpu.h"
#include <fenv.h>
#include <stdio.h>

static int failures;
static void check(u64 got, u64 want, const char* name) {
    if (got == want) return;
    fprintf(stderr, "%s: %016llx != %016llx\n", name, (unsigned long long)got, (unsigned long long)want);
    failures++;
}
static CPU state(u32 control) {
    CPU c = {0};
    c.fpcr = control;
    c.fpsr = 0xA8000000u; /* NZCV and QC in FPSR must survive arithmetic. */
    c.n = c.cf = 1;
    return c;
}
static void result(CPU* c, u64 got, u64 want, u32 flags, u32 control, const char* name) {
    check(got, want, name);
    check(c->fpsr, 0xA8000000u | flags, "cumulative FPSR");
    check(c->fpcr, control, "unchanged FPCR");
    check(get_nzcv(c), 0xA0000000u, "unchanged PSTATE.NZCV");
}
static void binary(void) {
    static const struct {
        int w, op;
        u32 control;
        u64 a, b, out;
        u32 flags;
    } t[] = {
        {32, SNX_FP_DIV, 0, 0, 0x80000000, 0x7FC00000, 1},
        {64, SNX_FP_DIV, 0, 0, 0, 0x7FF8000000000000ull, 1},
        {32, SNX_FP_DIV, 0, 0xBF800000, 0, 0xFF800000, 2},
        {32, SNX_FP_DIV, 0, 0x7F800000, 0, 0x7F800000, 0},
        {32, SNX_FP_DIV, 0, 0x3F800000, 0x7F800000, 0, 0},
        {32, SNX_FP_ADD, 0, 0x7FC00123, 0xFF800456, 0xFFC00456, 1},
        {32, SNX_FP_ADD, 1u << 25, 0x7FC00123, 0xFF800456, 0x7FC00000, 1},
        {32, SNX_FP_ADD, 0, 0xFFC00123, 0x7FC00456, 0xFFC00123, 0},
        {32, SNX_FP_SUB, 0, 0x3F800000, 0xFFC00123, 0xFFC00123, 0},
        {32, SNX_FP_ABD, 0, 0x3F800000, 0xFFC00123, 0x7FC00123, 0},
        {32, SNX_FP_MUL, 0, 0x7F800000, 0, 0x7FC00000, 1},
        {32, SNX_FP_MULX, 0, 0xFF800000, 0, 0xC0000000, 0},
        {32, SNX_FP_ADD, 0, 0x3F800000, 0xBF800000, 0, 0},
        {32, SNX_FP_ADD, 2u << 22, 0x3F800000, 0xBF800000, 0x80000000, 0},
        {32, SNX_FP_ADD, 0, 0x80000000, 0x80000000, 0x80000000, 0},
        {32, SNX_FP_MUL, 0, 0x00800000, 0x3F000000, 0x00400000, 0},
        {32, SNX_FP_MUL, 1u << 24, 0x00800000, 0x3F000000, 0, 8},
        {32, SNX_FP_MUL, 0, 1, 0x3F000000, 0, 0x18},
        {32, SNX_FP_MUL, 1u << 24, 1, 0x3F000000, 0, 0x80},
        {32, SNX_FP_MUL, 0, 0x7F7FFFFF, 0x40000000, 0x7F800000, 0x14},
        {32, SNX_FP_MUL, 3u << 22, 0x7F7FFFFF, 0x40000000, 0x7F7FFFFF, 0x14},
        {32, SNX_FP_MUL, 1u << 22, 0xFF7FFFFF, 0x40000000, 0xFF7FFFFF, 0x14},
        {32, SNX_FP_MUL, 2u << 22, 0xFF7FFFFF, 0x40000000, 0xFF800000, 0x14},
        {32, SNX_FP_RECPS, 0, 0x7F800000, 0, 0x40000000, 0},
        {32, SNX_FP_RSQRTS, 0, 0x7F800000, 0, 0x3FC00000, 0},
        {32, SNX_FP_RECPS, 0, 0xFFC00123, 0, 0x7FC00123, 0},
        {32, SNX_FP_RSQRTS, 0, 0xFF7FFFFF, 0x40000000, 0x7F7FFFFF, 0x10},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        CPU c = state(t[i].control);
        u64 got = snx_fp_binary(&c, t[i].a, t[i].b, t[i].w, t[i].op);
        result(&c, got, t[i].out, t[i].flags, t[i].control, "binary");
    }
}
static void fused(void) {
    static const struct {
        int w;
        u32 control;
        u64 d, a, b, out;
        u32 flags;
    } t[] = {
        {32, 0, 0x7FC00123, 0xFFC00456, 0x7FC00789, 0x7FC00123, 0},
        {32, 0, 0x7FC00123, 0xFF800456, 0x7FC00789, 0xFFC00456, 1},
        {32, 0, 0x7FC00123, 0x7F800000, 0, 0x7FC00000, 1},
        {32, 0, 0xFF800123, 0x7F800000, 0, 0xFFC00123, 1},
        {32, 0, 0xFF7FFFFF, 0x7F7FFFFF, 0x40000000, 0x7F7FFFFF, 0},
        {32, (1u << 22) | (1u << 24), 0x3F800000, 0x00800000, 0x00800000, 0x3F800001, 0x10},
        {64, 0, 0xFFEFFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull, 0x4000000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        CPU c = state(t[i].control);
        u64 got = snx_fp_fma(&c, t[i].d, t[i].a, t[i].b, t[i].w);
        result(&c, got, t[i].out, t[i].flags, t[i].control, "fused");
    }
}
static void unary(void) {
    static const struct {
        int w, op;
        u32 control;
        u64 a, out;
        u32 flags;
    } t[] = {
        {32, SNX_FP_SQRT, 0, 0x40000000, 0x3FB504F3, 0x10},
        {32, SNX_FP_SQRT, 1u << 22, 0x40000000, 0x3FB504F4, 0x10},
        {64, SNX_FP_SQRT, 0, 0x4000000000000000ull, 0x3FF6A09E667F3BCDull, 0x10},
        {64, SNX_FP_SQRT, 3u << 22, 0x4000000000000000ull, 0x3FF6A09E667F3BCCull, 0x10},
        {32, SNX_FP_SQRT, 0, 0x80000000, 0x80000000, 0},
        {32, SNX_FP_SQRT, 0, 0xFF800000, 0x7FC00000, 1},
        {32, SNX_FP_SQRT, 0, 0xFFC00123, 0xFFC00123, 0},
        {32, SNX_FP_RECPE, 0, 0x3F800000, 0x3F7F8000, 0},
        {32, SNX_FP_RSQRTE, 0, 0x3F800000, 0x3F7F8000, 0},
        {32, SNX_FP_RECPE, 0, 0x80000000, 0xFF800000, 2},
        {32, SNX_FP_RSQRTE, 0, 0xBF800000, 0x7FC00000, 1},
        {32, SNX_FP_RECPE, 0, 1, 0x7F800000, 0x14},
        {32, SNX_FP_RECPE, 3u << 22, 1, 0x7F7FFFFF, 0x14},
        {32, SNX_FP_RECPE, 1u << 24, 1, 0x7F800000, 0x82},
        {32, SNX_FP_RECPE, 1u << 24, 0x7E800000, 0, 8},
        {32, SNX_FP_RECPX, 0, 0x3F800000, 0x40000000, 0},
        {32, SNX_FP_RECPX, 0, 0x80000000, 0xFF000000, 0},
        {32, SNX_FP_RECPX, 1u << 24, 1, 0x7F000000, 0x80},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        CPU c = state(t[i].control);
        u64 got = snx_fp_unary(&c, t[i].a, t[i].w, t[i].op);
        result(&c, got, t[i].out, t[i].flags, t[i].control, "unary");
    }
}
static void formats(void) {
    static const struct {
        int src, dst, rm;
        u32 control;
        u64 a, out;
        u32 flags;
    } t[] = {
        {32, 16, 0, 0, 0x3F801000, 0x3C00, 0x10},
        {32, 16, 1, 0, 0x3F801000, 0x3C01, 0x10},
        {32, 16, 0, 1u << 26, 0x7F800000, 0x7FFF, 1},
        {32, 16, 0, 1u << 26, 0xFFC00123, 0x8000, 1},
        {32, 16, 0, 1u << 26, 0x48000000, 0x7FFF, 1},
        {16, 32, 0, 1u << 26, 0x7FFF, 0x47FFE000, 0},
        {16, 32, 0, 1u << 24, 1, 0x33800000, 0}, /* FZ does not flush half inputs. */
        {16, 32, 0, 0, 0x7D01, 0x7FE02000, 1},
        {64, 16, 0, 0, 0x3FF0020000000001ull, 0x3C01, 0x10}, /* Do not double-round through single. */
        {64, 16, 0, 0, 0xBFF0020000000001ull, 0xBC01, 0x10},
        {64, 32, 0, 0, 0x380FFFFFFFFFFFFFull, 0x00800000, 0x18}, /* Tininess before rounding. */
        {64, 32, 0, 1u << 24, 0x380FFFFFFFFFFFFFull, 0, 8},
        {64, 32, 5, 0, 0x3FF0000010000000ull, 0x3F800001, 0x10},
        {64, 32, 0, 0, 0x3690000000000000ull, 0, 0x18},
        {64, 32, 1, 0, 0x3690000000000000ull, 1, 0x18},
        {64, 32, 5, 0, 0x0010000000000000ull, 1, 0x18},
        {64, 32, 5, 1u << 24, 0x0010000000000000ull, 0, 8},
        {64, 32, 0, 1u << 25, 0xFFF8000000000123ull, 0x7FC00000, 0},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        CPU c = state(t[i].control);
        u64 got = snx_fp_convert(&c, t[i].a, t[i].src, t[i].dst, t[i].rm);
        result(&c, got, t[i].out, t[i].flags, t[i].control, "format conversion");
    }
}
static void integers(void) {
    static const struct {
        int from, fw, iw, uns, fb, rm;
        u32 control;
        u64 a, out;
        u32 flags;
    } t[] = {
        {0, 32, 32, 0, 0, 0, 0, 0xFFC00123, 0, 1},
        {0, 32, 32, 1, 0, 3, 0, 0xBE800000, 0, 0x10},
        {0, 32, 32, 1, 0, 2, 0, 0xBE800000, 0, 1},
        {0, 32, 32, 0, 0, 4, 0, 0xBF000000, 0xFFFFFFFF, 0x10},
        {0, 32, 32, 0, 0, 0, 0, 0x3F000000, 0, 0x10},
        {0, 32, 32, 0, 1, 3, 0, 0x3FC00000, 3, 0},
        {0, 64, 64, 1, 0, 3, 0, 0x43F0000000000000ull, ~0ull, 1},
        {0, 64, 64, 0, 0, 3, 0, 0xC3E0000000000000ull, 0x8000000000000000ull, 0},
        {0, 64, 64, 0, 0, 3, 0, 0xC3E0000000000001ull, 0x8000000000000000ull, 1},
        {1, 32, 32, 0, 0, 0, 0, 0x7FFFFFFF, 0x4F000000, 0x10},
        {1, 32, 64, 1, 0, 0, 0, ~0ull, 0x5F800000, 0x10},
        {1, 32, 64, 1, 0, 0, 3u << 22, ~0ull, 0x5F7FFFFF, 0x10},
        {1, 32, 32, 0, 1, 0, 0, 3, 0x3FC00000, 0},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        CPU c = state(t[i].control);
        u64 got = t[i].from ? snx_fp_from_int(&c, t[i].a, t[i].iw, t[i].fw, t[i].uns, t[i].fb)
                            : snx_fp_to_int(&c, t[i].a, t[i].fw, t[i].iw, t[i].uns, t[i].fb, t[i].rm);
        result(&c, got, t[i].out, t[i].flags, t[i].control, "integer conversion");
    }
}
/* RecipSqrtEstimate as the Arm ARM's pseudocode computes it, one step of b at a time: the oracle for the table
   fp_arithmetic.c reads */
static u32 rsqrt_estimate_search(u32 a) {
    u32 b = 512;
    if (a < 256)
        a = a * 2 + 1;
    else
        a = ((a >> 1) << 1) * 2 + 2;
    while ((u64)a * (b + 1) * (b + 1) < ((u64)1 << 28)) ++b;
    return (b + 1) / 2;
}
static void vector_and_state(void) {
    /* every input of the estimate, through URSQRTE (its 9 top bits, 128..511) */
    for (u32 a = 128; a < 512; a++) check(ursqrte32(a << 23), (u64)rsqrt_estimate_search(a) << 23, "URSQRTE estimate");
    check(urecpe32(0x7FFFFFFFu), 0xFFFFFFFFu, "URECPE saturation");
    check(urecpe32(0x80000000u), 0xFF800000u, "URECPE first estimate bin");
    check(urecpe32(0xFFFFFFFFu), 0x80000000u, "URECPE last estimate bin");
    check(ursqrte32(0x3FFFFFFFu), 0xFFFFFFFFu, "URSQRTE saturation");
    check(ursqrte32(0x40000000u), 0xFF800000u, "URSQRTE first estimate bin");
    check(ursqrte32(0xFFFFFFFFu), 0x80000000u, "URSQRTE strict boundary");
    CPU c = state(0);
    check(snx_simd(&c, NULL, 0x5E217820), 0, "reserved scalar FCVTL");
    check(snx_simd(&c, NULL, 0x5EA1C820), 0, "reserved scalar URECPE");
    check(snx_simd(&c, NULL, 0x7EA1C820), 0, "reserved scalar URSQRTE");
    check(snx_simd(&c, NULL, 0x7EA1F820), 0, "reserved AdvSIMD scalar FSQRT");
    c.v[0].s[0] = 0x7FC00123;
    c.v[1].s[0] = 0x7F800000;
    c.v[2].s[0] = 0;
    c.v[0].s[1] = 0xFF7FFFFF;
    c.v[1].s[1] = 0x7F7FFFFF;
    c.v[2].s[1] = 0x40000000;
    check(snx_simd(&c, NULL, 0x4E22CC20), 1, "FMLA V0.4S,V1.4S,V2.4S");
    check(c.v[0].s[0], 0x7FC00000, "vector QNaN addend with inf*zero");
    check(c.v[0].s[1], 0x7F7FFFFF, "vector fused overflow cancellation");
    check(c.fpsr, 0xA8000001u, "vector cumulative flags");
    c = state(0);
    c.v[0].s[0] = 0x3F800000;
    c.v[0].s[1] = 0x33800000;
    c.v[0].s[2] = 0x7FC00123;
    c.v[0].s[3] = 0x7F800456;
    check(snx_simd(&c, NULL, 0x6E20D400), 1, "FADDP V0.4S,V0.4S,V0.4S");
    check(c.v[0].s[0], 0x3F800000, "pairwise tie to even");
    check(c.v[0].s[1], 0x7FC00456, "pairwise SNaN priority");
    check(c.v[0].s[2], c.v[0].s[0], "pairwise aliases");
    check(c.fpsr, 0xA8000011u, "pairwise cumulative IOC/IXC");
    RtAPI rt = {0};
    c.rt = &rt;
    snx_msr(&c, 0x5A20, ~0ull);
    check(snx_mrs(&c, 0x5A20), 0x07C00000, "A57 FPCR RES0");
    snx_msr(&c, 0x5A21, ~0ull);
    check(snx_mrs(&c, 0x5A21), 0xF800009F, "A57 FPSR RES0");
    check(snx_fp_binary(&c, 0x3F800000, 0x3F800000, 32, SNX_FP_ADD), 0x40000000, "exact sum");
    check(c.fpsr, 0xF800009F, "exact operation preserves every existing exception");
}
int main(void) {
    const int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    fenv_t saved;
    if (fegetenv(&saved)) return 2;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (fesetround(modes[i]) || feclearexcept(FE_ALL_EXCEPT)) return 2;
        binary();
        fused();
        unary();
        formats();
        integers();
        vector_and_state();
        check(fegetround(), modes[i], "host rounding unchanged");
        check(fetestexcept(FE_ALL_EXCEPT), 0, "host exceptions unchanged");
    }
    if (fesetenv(&saved)) return 2;
    if (!failures) puts("ARMv8 FP arithmetic witnesses passed independently of host rounding");
    return failures != 0;
}
