/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The FP instructions whose fast paths run on the host's arithmetic (ir_vec_fp.c for the vectors, ir_fpu.c for the
   scalar FADD, FSUB, FMUL, FDIV and FRINTs) through the LLVM backend's steps (llvm_steps.c), against the soft-float helpers
   lane by lane: every lane's bits and FPSR after the whole instruction, and a scalar's cleared upper lanes. The
   operands mix lanes the host can take (normal values) with lanes it can't (zeros, denormals, infinities, NaNs, the
   normal range's ends) in one vector, which the fast paths must leave to the helpers or get exactly right; FPCR and
   FPSR are drawn too (round to nearest or not, FZ, DN, IXC set or clear). With the host's extensions and in the
   portable form, which has no FMA on x86. The instructions whose fast paths don't take the host's rounding (the
   FRINTs and FCVTs to integers, the estimates) run in each of the host's rounding modes too */
#include "llvm_steps.h"
#include <fenv.h>
#include <stdio.h>
#include <string.h>

static int failures;
static u64 next(u64* s) {
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

/* Vd = Vn op Vm (FMLA: Vd += Vn * Vm) with d 0, n 1, m 2, single (4S) or double (2D) */
enum { MINMAX = 100, UNARY = 200 }; /* MINMAX + minimum + 2 * numeric; UNARY + SNX_FP_RECPE or RSQRTE, of Vn */
typedef struct {
    const char* name;
    u32 word;   /* the 4S form (S for a scalar); bit 22 makes it 2D (D) */
    int op;     /* SNX_FP_*, -1 for FMLA, or MINMAX, UNARY's */
    int scalar; /* one lane, the rest of Vd cleared (ir_fpu.c's fast paths) */
} Insn;
static const Insn g_insns[] = {
    {"FADD", 0x4E22D420u, SNX_FP_ADD, 0},
    {"FSUB", 0x4EA2D420u, SNX_FP_SUB, 0},
    {"FMUL", 0x6E22DC20u, SNX_FP_MUL, 0},
    {"FDIV", 0x6E22FC20u, SNX_FP_DIV, 0},
    {"FMULX", 0x4E22DC20u, SNX_FP_MULX, 0},
    {"FMLA", 0x4E22CC20u, -1, 0},
    {"FRECPS", 0x4E22FC20u, SNX_FP_RECPS, 0},
    {"FRSQRTS", 0x4EA2FC20u, SNX_FP_RSQRTS, 0},
    {"FADD", 0x1E222820u, SNX_FP_ADD, 1},
    {"FSUB", 0x1E223820u, SNX_FP_SUB, 1},
    {"FMUL", 0x1E220820u, SNX_FP_MUL, 1},
    {"FDIV", 0x1E221820u, SNX_FP_DIV, 1},
    {"FABD", 0x6EA2D420u, SNX_FP_ABD, 0},
    {"FABD", 0x7EA2D420u, SNX_FP_ABD, 1},
    {"FMAX", 0x4E22F420u, MINMAX + 0, 0},
    {"FMIN", 0x4EA2F420u, MINMAX + 1, 0},
    {"FMAXNM", 0x4E22C420u, MINMAX + 2, 0},
    {"FMINNM", 0x4EA2C420u, MINMAX + 3, 0},
    {"FRECPE", 0x4EA1D820u, UNARY + SNX_FP_RECPE, 0},
    {"FRSQRTE", 0x6EA1D820u, UNARY + SNX_FP_RSQRTE, 0},
    {"FRECPE", 0x5EA1D820u, UNARY + SNX_FP_RECPE, 1},
    {"FRSQRTE", 0x7EA1D820u, UNARY + SNX_FP_RSQRTE, 1},
};
#define NINSN (sizeof g_insns / sizeof *g_insns)

/* Vd = FRINT*(Vn), d 0, n 1: the 4S form (S for a scalar), bit 22 the 2D (D) one */
typedef struct {
    const char* name;
    u32 word;
    int rm;    /* the encoding's rounding mode, -1 for FPCR's */
    int exact; /* FRINTX's IXC */
    int scalar;
} Round;
static const Round g_rounds[] = {
    {"FRINTN", 0x4E218820u, 0, 0, 0},  {"FRINTP", 0x4EA18820u, 1, 0, 0},  {"FRINTM", 0x4E219820u, 2, 0, 0},
    {"FRINTZ", 0x4EA19820u, 3, 0, 0},  {"FRINTA", 0x6E218820u, 4, 0, 0},  {"FRINTX", 0x6E219820u, -1, 1, 0},
    {"FRINTI", 0x6EA19820u, -1, 0, 0}, {"FRINTN", 0x1E244020u, 0, 0, 1},  {"FRINTP", 0x1E24C020u, 1, 0, 1},
    {"FRINTM", 0x1E254020u, 2, 0, 1},  {"FRINTZ", 0x1E25C020u, 3, 0, 1},  {"FRINTA", 0x1E264020u, 4, 0, 1},
    {"FRINTX", 0x1E274020u, -1, 1, 1}, {"FRINTI", 0x1E27C020u, -1, 0, 1},
};
#define NROUND (sizeof g_rounds / sizeof *g_rounds)

/* Xd or Vd = FCVT*(Vn) to integers of the float's width, d 0, n 1: the 4S form (W from S for a scalar), bit 22 the
   2D (X from D: bit 31 too) one */
typedef struct {
    const char* name;
    u32 word;
    int rm, uns, scalar;
} Convert;
static const Convert g_converts[] = {
    {"FCVTAS", 0x4E21C820u, 4, 0, 0}, {"FCVTAU", 0x6E21C820u, 4, 1, 0}, {"FCVTNS", 0x4E21A820u, 0, 0, 0},
    {"FCVTAS", 0x1E240020u, 4, 0, 1}, {"FCVTAU", 0x1E250020u, 4, 1, 1}, {"FCVTMS", 0x1E300020u, 2, 0, 1},
};
#define NCONVERT (sizeof g_converts / sizeof *g_converts)
static const int g_host_modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};

/* a lane: a normal value three times in four, else one of the values the fast paths leave to the helpers */
static u64 lane_value(u64* s, int width) {
    u64 r = next(s), sign = (r >> 63) << (width - 1);
    int ebits = width == 32 ? 8 : 11, fbits = width - 1 - ebits;
    u64 emax = ((u64)1 << ebits) - 1, frac = next(s) & (((u64)1 << fbits) - 1), bias = emax >> 1, e;
    switch (r % 16) {
        case 0: return sign; /* a zero */
        case 1: return sign | (frac ? frac : 1); /* a denormal */
        case 2: return sign | emax << fbits; /* an infinity */
        case 3: return sign | emax << fbits | ((u64)1 << (fbits - 1)) | (frac >> 1); /* a quiet NaN */
        case 4: return sign | (u64)1 << fbits | frac; /* the smallest normals */
        case 5: return sign | (emax - 1) << fbits | frac; /* the largest */
        case 6: return sign | emax << fbits | (frac & (((u64)1 << (fbits - 1)) - 1)) | 1; /* a signalling NaN */
        case 7: /* square roots of the smallest normal: products land around it */
            e = bias / 2 + (next(s) % 17) - 8;
            return sign | e << fbits | frac;
        default: /* exponents near 1, so sums cancel and products and quotients stay in range */
            e = bias - 8 + (next(s) % 17);
            return sign | e << fbits | frac;
    }
}

/* a value to round: one with a fraction, a tie (n + 1/2) or a neighbour of one, of either sign, else lane_value's */
static u64 round_value(u64* s, int width) {
    u64 r = next(s), sign = (r >> 63) << (width - 1), v;
    switch (r % 4) {
        case 0:
            return sign | (width == 64 ? 0x4030000000000000ull : 0x41800000u) |
                   (next(s) & (width == 64 ? 0xFFFFFFFFFFFFull : 0x7FFFFu));
        case 1: { /* n + 1/2 for n below 2^22 (2^51), one encoding up or down, or exact */
            unsigned shift = (unsigned)(next(s) % (width == 64 ? 52 : 23));
            u64 n = next(s) & (((u64)1 << shift) - 1);
            if (width == 64) {
                double d = (double)n + 0.5;
                memcpy(&v, &d, 8);
            } else {
                float f = (float)n + 0.5f;
                u32 u;
                memcpy(&u, &f, 4);
                v = u;
            }
            return (sign | v) + (next(s) % 3) - 1;
        }
        default: return lane_value(s, width);
    }
}

static void set_lane(V128* v, int width, int i, u64 x) {
    if (width == 32)
        v->s[i] = (u32)x;
    else
        v->d[i] = x;
}
static u64 get_lane(const V128* v, int width, int i) { return width == 32 ? v->s[i] : v->d[i]; }

/* FRECPE and FRSQRTE of every operand class their fast path takes apart: each top fraction byte with an odd and an
   even exponent (all the integers the estimates are a function of), at the ends of the exponents they take and
   just past them, of either sign, in a lane with three other values, in each host rounding mode */
static unsigned estimates(int portable) {
    static const u32 words[] = {0x4EA1D820u, 0x6EA1D820u, 0x5EA1D820u, 0x7EA1D820u}; /* RECPE, RSQRTE: 4S, S */
    LlvmSteps steps;
    char error[1024];
    unsigned checks = 0, k, dbl, top, e, m;
    u64 seed = 0x2545F4914F6CDD1Dull;
    if (llvm_steps_begin(&steps, 1, portable, error, sizeof(error))) return snx_error("%s", error), 0;
    for (k = 0; k < 4; k++)
        for (dbl = 0; dbl < 2; dbl++) llvm_steps_add(&steps, words[k] | dbl << 22, 0x1000, 0);
    if (llvm_steps_load(&steps, error, sizeof(error))) return snx_error("%s", error), 0;
    for (k = 0; k < 4; k++)
        for (dbl = 0; dbl < 2; dbl++) {
            int width = dbl ? 64 : 32, fbits = dbl ? 52 : 23, lanes = k >= 2 ? 1 : 128 / width;
            u64 emax = dbl ? 2047 : 255, bias = emax >> 1;
            u64 exps[] = {1, 2, bias - 1, bias, 2 * bias - 3, 2 * bias - 2, 2 * bias - 1, 2 * bias, emax - 1};
            LlvmStep step = steps.steps[k * 2 + dbl];
            for (top = 0; top < 256; top++)
                for (e = 0; e < sizeof exps / sizeof *exps; e++)
                    for (m = 0; m < 4; m++) {
                        CPU c, ref;
                        RtAPI rt;
                        u64 want[4];
                        int lane;
                        memset(&c, 0, sizeof c);
                        memset(&rt, 0, sizeof rt);
                        c.rt = &rt;
                        c.fpcr = m & 1 ? 1u << 24 : 0;
                        for (lane = 0; lane < lanes; lane++) {
                            u64 v = lane_value(&seed, width);
                            if (lane == 0 || next(&seed) & 1)
                                v = (u64)(m >> 1) << (width - 1) | exps[e] << fbits | (u64)top << (fbits - 8) |
                                    (next(&seed) & (((u64)1 << (fbits - 8)) - 1));
                            set_lane(&c.v[1], width, lane, v);
                        }
                        ref = c;
                        for (lane = 0; lane < lanes; lane++)
                            want[lane] = snx_fp_unary(&ref, get_lane(&c.v[1], width, lane), width,
                                                      k & 1 ? SNX_FP_RSQRTE : SNX_FP_RECPE);
                        fesetround(g_host_modes[(top + e + m) % 4]);
                        step(&c);
                        fesetround(FE_TONEAREST);
                        checks++;
                        for (lane = 0; lane < lanes; lane++)
                            if (get_lane(&c.v[0], width, lane) != (width == 32 ? (u32)want[lane] : want[lane])) break;
                        if ((lane < lanes || c.fpsr != ref.fpsr) && failures++ < 8)
                            fprintf(stderr, "%s %s %s: %llx gives %llx, expected %llx, FPSR %08x, expected %08x\n",
                                    portable ? "the portable" : "the host's", k & 1 ? "FRSQRTE" : "FRECPE",
                                    k >= 2 ? (dbl ? "D" : "S")
                                    : dbl  ? "2D"
                                           : "4S",
                                    (unsigned long long)get_lane(&ref.v[1], width, lane < lanes ? lane : 0),
                                    (unsigned long long)get_lane(&c.v[0], width, lane < lanes ? lane : 0),
                                    (unsigned long long)want[lane < lanes ? lane : 0], c.fpsr, ref.fpsr);
                    }
        }
    llvm_steps_end(&steps);
    return checks;
}

int main(void) {
    const char* forms[2] = {"the host's", "the portable"};
    unsigned checks = 0;
    int portable;
    for (portable = 0; portable < 2; portable++) {
        LlvmSteps steps;
        char error[1024];
        u64 seed = (u64)0x9E3779B9 << 32 | 0x7F4A7C15;
        size_t k;
        int dbl, i, lane;
        if (llvm_steps_begin(&steps, 1, portable, error, sizeof(error))) return snx_error("%s", error);
        for (k = 0; k < NINSN; k++)
            for (dbl = 0; dbl < 2; dbl++) llvm_steps_add(&steps, g_insns[k].word | (u32)dbl << 22, 0x1000, 0);
        if (llvm_steps_load(&steps, error, sizeof(error))) return snx_error("%s", error);
        for (k = 0; k < NINSN; k++)
            for (dbl = 0; dbl < 2; dbl++) {
                int width = dbl ? 64 : 32, lanes = g_insns[k].scalar ? 1 : 128 / width;
                LlvmStep step = steps.steps[k * 2 + (size_t)dbl];
                for (i = 0; i < 20000; i++) {
                    CPU c, ref;
                    RtAPI rt;
                    u64 r = next(&seed), want[4];
                    u32 fpcr = 0, fpsr = r & 1 ? 0x10u : 0;
                    if (r % 3 == 0) fpsr |= (u32)next(&seed) & 0x9Fu; /* the other cumulative flags */
                    memset(&c, 0, sizeof c);
                    memset(&rt, 0, sizeof rt);
                    c.rt = &rt;
                    if (r % 10 == 1) fpcr = (u32)(next(&seed) % 4) << 22; /* a rounding mode */
                    if (r % 10 == 2) fpcr = 1u << 24;                     /* FZ */
                    if (r % 10 == 3) fpcr = 1u << 25;                     /* DN */
                    if (r % 10 == 4) fpcr = 3u << 24;                     /* FZ and DN */
                    for (lane = 0; lane < lanes; lane++) {
                        set_lane(&c.v[0], width, lane, lane_value(&seed, width));
                        set_lane(&c.v[1], width, lane, lane_value(&seed, width));
                        set_lane(&c.v[2], width, lane, lane_value(&seed, width));
                    }
                    c.fpcr = fpcr;
                    c.fpsr = fpsr;
                    ref = c;
                    for (lane = 0; lane < lanes; lane++) {
                        u64 d = get_lane(&c.v[0], width, lane), n = get_lane(&c.v[1], width, lane);
                        u64 m = get_lane(&c.v[2], width, lane);
                        int op = g_insns[k].op;
                        want[lane] = op < 0          ? snx_fp_fma(&ref, d, n, m, width)
                                     : op >= UNARY  ? snx_fp_unary(&ref, n, width, op - UNARY)
                                     : op >= MINMAX ? snx_fp_minmax(&ref, n, m, width, (op - MINMAX) & 1, (op - MINMAX) >> 1)
                                                    : snx_fp_binary(&ref, n, m, width, op);
                    }
                    if (g_insns[k].op >= UNARY) fesetround(g_host_modes[i % 4]); /* the estimates' own rounding */
                    step(&c);
                    fesetround(FE_TONEAREST);
                    checks++;
                    for (lane = 0; lane < lanes; lane++)
                        if (get_lane(&c.v[0], width, lane) != (width == 32 ? (u32)want[lane] : want[lane])) break;
                    if (g_insns[k].scalar && lane == lanes) /* a scalar clears the rest of the register */
                        for (int w = 1; w < 128 / width; w++)
                            if (get_lane(&c.v[0], width, w)) lane = 0;
                    if ((lane < lanes || c.fpsr != ref.fpsr) && failures++ < 8) {
                        fprintf(stderr, "%s %s.%s, FPCR %08x, FPSR %08x:", forms[portable], g_insns[k].name,
                                g_insns[k].scalar ? (dbl ? "D" : "S") : dbl ? "2D" : "4S", fpcr, fpsr);
                        for (lane = 0; lane < lanes; lane++)
                            fprintf(stderr, " [%llx %llx %llx: %llx, expected %llx]",
                                    (unsigned long long)get_lane(&ref.v[0], width, lane),
                                    (unsigned long long)get_lane(&ref.v[1], width, lane),
                                    (unsigned long long)get_lane(&ref.v[2], width, lane),
                                    (unsigned long long)get_lane(&c.v[0], width, lane),
                                    (unsigned long long)(width == 32 ? (u32)want[lane] : want[lane]));
                        fprintf(stderr, " FPSR %08x, expected %08x\n", c.fpsr, ref.fpsr);
                    }
                }
            }
        llvm_steps_end(&steps);
        if (llvm_steps_begin(&steps, 1, portable, error, sizeof(error))) return snx_error("%s", error);
        for (k = 0; k < NROUND; k++)
            for (dbl = 0; dbl < 2; dbl++) llvm_steps_add(&steps, g_rounds[k].word | (u32)dbl << 22, 0x1000, 0);
        if (llvm_steps_load(&steps, error, sizeof(error))) return snx_error("%s", error);
        for (k = 0; k < NROUND; k++)
            for (dbl = 0; dbl < 2; dbl++) {
                int width = dbl ? 64 : 32, lanes = g_rounds[k].scalar ? 1 : 128 / width;
                LlvmStep step = steps.steps[k * 2 + (size_t)dbl];
                for (i = 0; i < 20000; i++) {
                    CPU c, ref;
                    RtAPI rt;
                    u64 r = next(&seed), want[4];
                    u32 fpcr = (u32)(r % 4) << 22, fpsr = r & 4 ? 0x10u : 0;
                    memset(&c, 0, sizeof c);
                    memset(&rt, 0, sizeof rt);
                    c.rt = &rt;
                    if (r & 8) fpcr = 0;          /* mostly round to nearest */
                    if (r & 16) fpcr |= 1u << 24; /* FZ */
                    if (r & 32) fpcr |= 1u << 25; /* DN */
                    fesetround(g_host_modes[i % 4]); /* the generated code's rounding is the host's in none */
                    for (lane = 0; lane < lanes; lane++) set_lane(&c.v[1], width, lane, round_value(&seed, width));
                    c.fpcr = fpcr;
                    c.fpsr = fpsr;
                    ref = c;
                    for (lane = 0; lane < lanes; lane++)
                        want[lane] = snx_fp_round(&ref, get_lane(&c.v[1], width, lane), width,
                                                  g_rounds[k].rm < 0 ? (int)(fpcr >> 22 & 3) : g_rounds[k].rm,
                                                  g_rounds[k].exact);
                    step(&c);
                    fesetround(FE_TONEAREST);
                    checks++;
                    for (lane = 0; lane < lanes; lane++)
                        if (get_lane(&c.v[0], width, lane) != (width == 32 ? (u32)want[lane] : want[lane])) break;
                    if (g_rounds[k].scalar && lane == lanes)
                        for (int w = 1; w < 128 / width; w++)
                            if (get_lane(&c.v[0], width, w)) lane = 0;
                    if ((lane < lanes || c.fpsr != ref.fpsr) && failures++ < 8) {
                        fprintf(stderr, "%s %s.%s, FPCR %08x, FPSR %08x:", forms[portable], g_rounds[k].name,
                                g_rounds[k].scalar ? (dbl ? "D" : "S")
                                : dbl              ? "2D"
                                                   : "4S",
                                fpcr, fpsr);
                        for (lane = 0; lane < lanes; lane++)
                            fprintf(stderr, " [%llx: %llx, expected %llx]",
                                    (unsigned long long)get_lane(&ref.v[1], width, lane),
                                    (unsigned long long)get_lane(&c.v[0], width, lane),
                                    (unsigned long long)(width == 32 ? (u32)want[lane] : want[lane]));
                        fprintf(stderr, " FPSR %08x, expected %08x\n", c.fpsr, ref.fpsr);
                    }
                }
            }
        llvm_steps_end(&steps);
        if (llvm_steps_begin(&steps, 1, portable, error, sizeof(error))) return snx_error("%s", error);
        for (k = 0; k < NCONVERT; k++)
            for (dbl = 0; dbl < 2; dbl++)
                llvm_steps_add(&steps, g_converts[k].word | (u32)dbl << 22 | (u32)(dbl && g_converts[k].scalar) << 31,
                               0x1000, 0);
        if (llvm_steps_load(&steps, error, sizeof(error))) return snx_error("%s", error);
        for (k = 0; k < NCONVERT; k++)
            for (dbl = 0; dbl < 2; dbl++) {
                int width = dbl ? 64 : 32, lanes = g_converts[k].scalar ? 1 : 128 / width;
                LlvmStep step = steps.steps[k * 2 + (size_t)dbl];
                for (i = 0; i < 20000; i++) {
                    CPU c, ref;
                    RtAPI rt;
                    u64 r = next(&seed), want[4], got;
                    u32 fpcr = r & 16 ? 1u << 24 : 0, fpsr = r & 4 ? 0x10u : 0;
                    memset(&c, 0, sizeof c);
                    memset(&rt, 0, sizeof rt);
                    c.rt = &rt;
                    if (r % 7 == 0) fpcr |= (u32)(next(&seed) % 4) << 22; /* FPCR's mode, which these ignore */
                    for (lane = 0; lane < lanes; lane++) {
                        u64 v = round_value(&seed, width);
                        if (next(&seed) % 4 == 0) /* near the integers' range */
                            v = (v & ((u64)1 << (width - 1))) |
                                (u64)((width == 32 ? 127 : 1023) + width - 2 + next(&seed) % 3)
                                    << (width == 32 ? 23 : 52) |
                                (next(&seed) & 0xFF);
                        set_lane(&c.v[1], width, lane, v);
                    }
                    c.fpcr = fpcr;
                    c.fpsr = fpsr;
                    ref = c;
                    for (lane = 0; lane < lanes; lane++)
                        want[lane] = snx_fp_to_int(&ref, get_lane(&c.v[1], width, lane), width, width,
                                                   g_converts[k].uns, 0, g_converts[k].rm);
                    fesetround(g_host_modes[i % 4]);
                    step(&c);
                    fesetround(FE_TONEAREST);
                    checks++;
                    for (lane = 0; lane < lanes; lane++) {
                        got = g_converts[k].scalar ? c.x[0] : get_lane(&c.v[0], width, lane);
                        if (got != (width == 32 ? (u32)want[lane] : want[lane])) break;
                    }
                    if ((lane < lanes || c.fpsr != ref.fpsr) && failures++ < 8) {
                        fprintf(stderr, "%s %s %s, FPCR %08x, FPSR %08x:", forms[portable], g_converts[k].name,
                                g_converts[k].scalar ? (dbl ? "X, D" : "W, S")
                                : dbl                ? "2D"
                                                     : "4S",
                                fpcr, fpsr);
                        for (lane = 0; lane < lanes; lane++)
                            fprintf(
                                stderr, " [%llx: %llx, expected %llx]",
                                (unsigned long long)get_lane(&ref.v[1], width, lane),
                                (unsigned long long)(g_converts[k].scalar ? c.x[0] : get_lane(&c.v[0], width, lane)),
                                (unsigned long long)(width == 32 ? (u32)want[lane] : want[lane]));
                        fprintf(stderr, " FPSR %08x, expected %08x\n", c.fpsr, ref.fpsr);
                    }
                }
            }
        llvm_steps_end(&steps);
        checks += estimates(portable);
    }
    printf("Vector FP fast paths through the LLVM steps: %u checks, %d failed\n", checks, failures);
    return failures != 0;
}
