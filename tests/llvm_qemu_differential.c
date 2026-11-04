/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LLVM backend against QEMU, instruction by instruction: each word runs as the backend's step function
   (snx_ir_static_step, compiled and linked by the backend) and under qemu-aarch64 -cpu max (tests/qemu_ref), from the
   same state and the same seeded window of guest memory, and the registers, flags, FPCR, FPSR, vector registers,
   the memory it changed and how it ended (the next pc, UNDEF, BRK or SVC) must match. A store-exclusive also runs
   after a load-exclusive of its address, so that it can succeed.
     snx-test-llvm-qemu-differential QEMU RUNNER DIR EXPECTED [--sweep] [--states N] [--seed N] [--quiet] [--update]
                                     [--jit] [--portable | --generic]
     snx-test-llvm-qemu-differential --skip REASON
   The words: samples of every row of the generated decoder's table with every value of each field of 3 bits or
   fewer, directed() words, and the words the other instruction tests hand-check (kept[]). The default run takes 2
   samples of each row, 20 directed rounds and 3 states of each word; --sweep takes 4 samples, 300 rounds and 6
   states. The steps are linked into a module for dlopen, as the AOT path loads a game's; --jit loads them with the
   JIT's loader (code_loader.c) instead, and --portable builds them without the host's extensions (x86's PSHUFB,
   PCLMULQDQ, AES-NI, SSE4.2's CRC32C, AArch64's CRC32 and crypto...), so the emitters' portable forms are compared.
   --generic builds them without the host's architecture too: the path of the hosts SnxIrFeatures has no bit for
   (PowerPC64, RISC-V, LoongArch, MIPS64), a byte loop for TBL and helpers for FMA, square roots and rounding.
   A word the backend leaves UNDEF (the generated decoder for the Switch target), or decodes without an emitter,
   while QEMU runs it, has no emitter yet: it is listed in DIR/llvm_qemu_coverage.txt, not failed. So is a word that
   differs where the Switch target decodes it as another name (a hint's NOP) or, as the backend does, as UNDEF: what
   max makes of it is beyond the Cortex-A57. The mismatches are
   grouped by the max target's name for the word and their cause, with examples in DIR/llvm_qemu_mismatches.txt.
   EXPECTED lists the groups known to differ until the emitters are rewritten, with their counts in the default run
   and the sweep: a group missing from it fails the test, and so does a group it lists for this run that no longer
   differs. --update rewrites this run's counts in EXPECTED instead. */
#include "common/utilities/common.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/ir_ldst.h"
#include "common/cpu/recompiler/llvm/ir_fpu.h"
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include "llvm_qemu_words.h"
#include "llvm_steps.h"
#include "qemu_ref/qemu_ref.h"
#include <stdlib.h>
#include <string.h>

typedef u64 (*StepFn)(CPU* c);

#define WINDOW      0x0ull /* the window starts at guest address 0 on both sides */
#define WINDOW_SIZE SNX_QREF_WINDOW_SIZE
#define PC          (WINDOW + 0x100000ull)
/* a pair's code, apart from the window: the runner makes a full diff's window writable, code pages in it too */
#define PAIR_PC     (WINDOW + WINDOW_SIZE)
#define CODE_SIZE   (2 * SNX_QREF_PAGE)
#define SEED        0x5EEDull
#define DIRECTED    51u  /* words directed() makes per round */
#define BATCH       8192 /* QEMU cases per run of the runner */
#define EXAMPLES    3    /* kept per group */
#define SIGBUS_A64  7u   /* QEMU's alignment fault: SIGBUS, as Linux on AArch64 numbers it */

/* the runtime's callbacks' record of a step (llvm_qemu_differential's outcome) */
typedef struct {
    int undef, brk, svc;
    u64 imm;
} Events;

static struct {
    u64 rng;
    Events* events; /* the step running */
} g_test = {0x9E3779B97F4A7C15ull, NULL};

static u64 next_random(void) {
    g_test.rng ^= g_test.rng << 13;
    g_test.rng ^= g_test.rng >> 7;
    g_test.rng ^= g_test.rng << 17;
    return g_test.rng;
}

/* ---- the states: random_state(), its FP edges, and the directed() and kept[] words ---- */
static u64 special64(void) {
    static const u64 values[] = {0,
                                 1,
                                 ~0ull,
                                 0x7FFFFFFFFFFFFFFFull,
                                 0x8000000000000000ull,
                                 0xFFFFFFFFull,
                                 0x80000000ull,
                                 0x7FFFFFFFull,
                                 0x3FF0000000000000ull, /* 1.0 */
                                 0x7FF0000000000000ull, /* inf */
                                 0x7FF8000000000001ull, /* qNaN */
                                 0x7FF0000000000001ull, /* sNaN */
                                 0x000FFFFFFFFFFFFFull, /* denormal */
                                 0x3F8000003F800000ull, /* 1.0f, 1.0f */
                                 0x7F8000007FC00001ull, /* inf, qNaN */
                                 0x007FFFFF80000000ull, /* denormal, -0 */
                                 0x7F8000017F800000ull};
    u64 r = next_random();
    if (r % 3 == 0) return values[(r >> 8) % (sizeof(values) / sizeof(values[0]))];
    if (r % 3 == 1) return next_random() >> (r >> 58);
    return next_random();
}

static u32 edge32(void) {
    static const u32 exponents[] = {0, 1, 2, 3, 24, 25, 26, 27, 100, 127, 128, 150, 200, 252, 253, 254, 255};
    u64 r = next_random();
    u32 e = exponents[r % (sizeof(exponents) / sizeof(exponents[0]))];
    u32 m = (r >> 8) & 7 ? (u32)(r >> 20) & 0x7FFFFF : (u32)((r >> 20) & 7) << 20;
    if (r % 6 == 0) return (u32)((r >> 40) & 1) << 31;
    return (u32)((r >> 40) & 1) << 31 | e << 23 | m;
}

static u64 edge64(void) {
    static const u64 exponents[] = {0, 1, 2, 3, 53, 54, 55, 56, 500, 1023, 1024, 1075, 1500, 2044, 2045, 2046, 2047};
    u64 r = next_random(), r2 = next_random();
    u64 e = exponents[r % (sizeof(exponents) / sizeof(exponents[0]))];
    u64 m = (r >> 8) & 7 ? r2 & 0xFFFFFFFFFFFFFull : (r2 & 7) << 49;
    if (r2 % 6 == 0) return ((r >> 40) & 1) << 63;
    return ((r >> 40) & 1) << 63 | e << 52 | m;
}

static u64 fp_value(void) {
    u64 r = next_random();
    if (r % 4 == 0) return (u64)edge32() << 32 | edge32();
    if (r % 4 == 1) return edge64();
    return special64();
}

/* integers near float and double precision limits, floats near the integer ranges' ends and halfway values */
static u64 conversion_edge(void) {
    static const u32 floats[] = {0x4B800000u, 0x4F000000u, 0x4F800000u, 0x5F000000u,
                                 0x5F800000u, 0x3F000000u, 0x3FC00000u, 0x40200000u};
    static const u64 doubles[] = {0x4340000000000000ull, 0x41E0000000000000ull, 0x41F0000000000000ull,
                                  0x43E0000000000000ull, 0x43F0000000000000ull, 0x3FE0000000000000ull,
                                  0x3FF8000000000000ull, 0x4004000000000000ull};
    u64 r = next_random(), k = next_random(), lane[2];
    unsigned i;
    if (r % 4 == 1) {
        u64 v = (r >> 8) & 1 ? ((u64)1 << 53) + (k % 5) - 2 : k % 4096;
        return (r >> 9) & 1 ? 0 - v : v;
    }
    if (r % 4 == 3) return ((r >> 9) & 1) << 63 | (doubles[(r >> 10) % 8] + (k % 5) - 2);
    for (i = 0; i < 2; ++i, k >>= 16) {
        u32 v;
        if (r % 4 == 0) {
            v = (k >> 3) & 1 ? (1u << 24) + (u32)(k % 5) - 2 : (u32)(k >> 4) % 4096;
            if ((k >> 15) & 1) v = 0 - v;
        } else {
            v = (u32)((k >> 15) & 1) << 31 | (floats[(k >> 4) % 8] + (u32)(k % 5) - 2);
        }
        lane[i] = v;
    }
    return lane[1] << 32 | lane[0];
}

/* a half (FEAT_FP16): zeros, denormals, the normal range's ends, infinities, quiet and signalling NaNs, or any */
static u64 half_value(void) {
    u64 k = next_random(), sign = (k & 1) << 15, bits = k >> 8;
    switch ((k >> 1) % 8) {
        case 0: return sign;
        case 1: return sign | (1 + bits % 0x3FF);
        case 2: return sign | 0x7C00;
        case 3: return sign | 0x7E00 | (bits & 0x1FF);
        case 4: return sign | 0x7C01 | (bits & 0x1FE);
        case 5: return sign | 0x0400 | (bits & 0x3FF);
        case 6: return sign | 0x7800 | (bits & 0x3FF);
        default: return bits & 0xFFFF;
    }
}
/* a scalar FP instruction (data processing, compares, selects, conversions) on halves: ftype 3 */
static int half_word(u32 word) {
    return ((word & 0x5F000000u) == 0x1E000000u || (word & 0x5F000000u) == 0x1F000000u) && ((word >> 22) & 3) == 3;
}

/* FCVTN/FCVTXN inputs at the narrower format's normal range ends, dropped bits near half an ulp */
static u64 narrow_edge(unsigned doubles) {
    static const u32 exponents64[] = {897, 898, 1149, 1150, 897, 898, 1150, 1150, 896, 1151},
                     exponents32[] = {113, 114, 141, 142, 113, 114, 142, 142, 112, 143};
    u64 k = next_random(), lane[2];
    unsigned i, n = doubles ? 1 : 2, shift = doubles ? 29 : 13, mantissa = doubles ? 52 : 23;
    for (i = 0; i < n; ++i, k >>= 16) {
        u64 low = (u64)1 << (shift - 1), fraction;
        switch (k % 6) {
            case 0: fraction = 0; break;
            case 1: fraction = low; break;
            case 2: fraction = low + 1; break;
            case 3: fraction = low - 1; break;
            case 4: fraction = ((u64)1 << mantissa) - 1; break;
            default: fraction = next_random() & (((u64)1 << mantissa) - 1); break;
        }
        if ((k >> 3) % 2 && k % 6 != 4) fraction ^= (next_random() << shift) & (((u64)1 << mantissa) - 1);
        lane[i] = ((k >> 4) & 1) << (doubles ? 63 : 31) |
                  (u64)(doubles ? exponents64[(k >> 5) % 10] : exponents32[(k >> 5) % 10]) << mantissa | fraction;
    }
    return doubles ? lane[0] : lane[1] << 32 | lane[0];
}

/* a starting state: with addresses, every X register and SP points into the window, 16-byte aligned, so that any
   base, index and offset the word combines stays in it */
static void random_state(CPU* c, u32 word, int addresses) {
    static const u32 fpcr[] = {0, 1u << 24, 1u << 25, 3u << 22, 1u << 22, 2u << 22, 1u << 26, 0x07C00000u};
    static const u64 negations[] = {0, 0x8000000080000000ull, 0x8000000000000000ull};
    unsigned i, rn = (word >> 5) & 31, rm = (word >> 16) & 31, ra = (word >> 10) & 31;
    u64 r, negate;
    memset(c, 0, sizeof(*c));
    for (i = 0; i < 32; ++i) {
        c->x[i] = addresses ? WINDOW + 0x1000 + (next_random() % 0x7000 & ~(u64)15) : special64();
        c->v[i].d[0] = fp_value();
        c->v[i].d[1] = fp_value();
    }
    /* the word's own FP operands now and then: copies and negations for exact cancellation, zeros of random signs,
       lanes needing different checks, conversion and narrowing edges */
    r = next_random();
    negate = negations[(r >> 8) % 3];
    if (r % 4 == 0) {
        c->v[rm].d[0] = c->v[rn].d[0] ^ negate;
        c->v[rm].d[1] = c->v[rn].d[1] ^ negate;
    }
    if ((r >> 16) % 8 == 0) {
        c->v[ra].d[0] = c->v[rn].d[0] ^ negate;
        c->v[ra].d[1] = c->v[rn].d[1] ^ negate;
    }
    if ((r >> 24) % 12 == 0) {
        V128* zero = &c->v[(r >> 32) % 2 ? rm : rn];
        zero->d[0] = next_random() & 0x8000000080000000ull;
        zero->d[1] = next_random() & 0x8000000080000000ull;
        if ((r >> 34) % 2) {
            zero = &c->v[(r >> 32) % 2 ? rn : rm];
            zero->d[0] = next_random() & 0x8000000080000000ull;
            zero->d[1] = next_random() & 0x8000000080000000ull;
        }
    }
    if ((r >> 40) % 24 == 0) {
        c->v[rn].d[0] = c->v[rn].d[1] = 0x3F8000013F800000ull;
        c->v[rm].d[0] = c->v[rm].d[1] = 0x018000013F800000ull;
    }
    if ((r >> 48) % 4 == 0) {
        c->v[rn].d[0] = conversion_edge();
        c->v[rn].d[1] = conversion_edge();
    }
    if ((word & 0x8F3FFC00u) == 0x0E216800u && (r >> 52) % 4) {
        c->v[rn].d[0] = narrow_edge((word >> 22) & 1);
        c->v[rn].d[1] = narrow_edge((word >> 22) & 1);
    }
    c->n = next_random() & 1;
    c->z = next_random() & 1;
    c->cf = next_random() & 1;
    c->vf = next_random() & 1;
    c->fpcr = fpcr[next_random() % 8] | (next_random() % 2 ? (u32)(next_random() & 0x07C00000u) : 0);
    if ((word & 0x8F3FFC00u) == 0x0E216800u && next_random() % 2) c->fpcr &= ~(3u << 22);
    if (half_word(word)) { /* the operands as halves, and FPCR.FZ16 */
        unsigned operands[3], i;
        operands[0] = rn;
        operands[1] = rm;
        operands[2] = ra;
        for (i = 0; i < 3; ++i)
            if (next_random() % 4) c->v[operands[i]].d[0] = (c->v[operands[i]].d[0] & ~0xFFFFull) | half_value();
        if (next_random() % 2) c->fpcr |= 1u << 19;
    }
    /* FPSR's cumulative flags and QC: its AArch32 NZCV bits are not A64 state */
    c->fpsr = next_random() % 4 ? (next_random() % 2 ? 0x10 : 0) : (u32)(next_random() & 0x0800009Fu);
}

/* directed() words, the encodings random fields rarely produce, unfiltered */
static size_t directed(size_t rounds, u32* out) {
    static const u32 same_ops[] = {3, 0x10, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F},
                     element_ops[] = {1, 5, 9}, copy_ops[] = {0, 1, 3, 7}, fp_ops[] = {0x18, 0x1A, 0x1C, 0x1D, 0x1E};
    static const u32 misc_fp[] = {0x1A, 0x1B, 0x1C, 0x1D, 0x3A, 0x3B, 0x3D, 0x5A,
                                  0x5B, 0x5C, 0x5D, 0x7A, 0x7B, 0x7D, 0x7F},
                     steps[] = {0x7A, 0x1B, 0x1F, 0x3F};
    static const unsigned char misc[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x12, 0x13, 0x14},
                               across[] = {0x03, 0x0A, 0x1A, 0x1B, 0x0C, 0x0F}, pairwise[] = {0x1B, 0x0D, 0x0C, 0x0F},
                               element[] = {0, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13},
                               shifts[] = {0x04, 0x06, 0x0C, 0x0E, 0x10, 0x11, 0x12, 0x13},
                               scalar_shifts[] = {0x00, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x11, 0x12, 0x13},
                               scalar_misc[] = {0x03, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x12, 0x14},
                               same[] = {0x04, 0x08, 0x09, 0x0A, 0x0B, 0x16},
                               scalar_same[] = {0x01, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x10, 0x11, 0x16};
    const size_t nsame = sizeof(same_ops) / sizeof(same_ops[0]);
    size_t got = 0, i;
    for (i = 0; i < rounds; ++i) {
        u32 r = (u32)next_random(), n = (r >> 5) & 31, *w = out + got;
        u32 m = misc_fp[next_random() % 15], t = steps[next_random() % 4], f = next_random() % 2 ? 0x1C : 0x1F;
        u32 immh = 1u << (19 + (u32)(next_random() % 4));
        w[0] = 0x2F00E400u | (r & 31) | ((r >> 9) & 1) << 30;
        w[1] = 0x0EA01C00u | ((r >> 10) & 1) << 30 | n << 16 | n << 5 | (r & 31);
        w[2] = ((r & ~0x9F20FC00u) | 0x0E200400u) | same_ops[next_random() % nsame] << 11;
        w[3] = ((r & ~0x9F00F400u) | (next_random() % 2 ? 0x5F000000u : 0x0F000000u) | 0x00800000u) |
               element_ops[next_random() % 3] << 12;
        w[4] = ((r & ~0x9FE0FC00u) | 0x0E000400u) | copy_ops[next_random() % 4] << 11;
        w[5] = ((r & ~0x9F20FC00u) | 0x0E200400u) | same_ops[next_random() % nsame] << 11 | 0x40000000u;
        w[6] = (r & 0x40C0001Fu) | 0x0C000000u | (u32)(next_random() % 2) << 23 |
               (u32)(next_random() % 2 ? 31 : (r >> 16) & 31) << 16 | (u32) "\x07\x0A\x06\x02"[next_random() % 4] << 12 |
               ((r >> 10) & 3) << 10 | ((r >> 5) & 31) << 5;
        w[7] = (r & 0x40E0FFFFu & ~0x00202000u) | 0x0D000000u;
        w[8] = (r & 0x60C003FFu) | 0x0E205800u;
        w[9] = (r & 0x401F03FFu) | 0x0EE0E000u;
        w[10] = (r & 0x6007FBFFu) | 0x0F000400u;
        w[11] = (r & 0x40DF73FFu) | 0x0E000800u;
        w[12] = (r & 0x401F7BFFu) | 0x2E000000u;
        w[13] = (r & 0x000033FFu) | 0x4E284800u;
        w[14] = (r & 0x60DFFBFFu) | 0x0E200400u;
        w[15] = (r & 0x607FFBFFu & ~0x0000F800u) | 0x0F000400u | (u32) "\x00\x02\x08\x0A\x14\x10"[next_random() % 6] << 11 |
                (1u << (19 + (r >> 30) % 4));
        w[16] = (r & 0x60C003FFu) | 0x0EA0F800u;
        w[17] = (r & 0x40DF07FFu) | 0x0E200400u | (u32)(next_random() % 6) << 11 | (u32)(next_random() % 2) << 29;
        w[18] = (r & 0x40DF03FFu) | 0x2E201C00u | (1u + (u32)(next_random() % 3)) << 22;
        w[19] = (r & 0x40DF03FFu) | 0x2E605800u;
        w[20] = (r & 0x60DF03FFu) | 0x0E200400u | fp_ops[next_random() % 5] << 11;
        w[21] = (r & 0x604003FFu) | 0x0EA00800u | (0x0Cu + (u32)(next_random() % 3)) << 12;
        m = (m >> 6) << 29 | ((m >> 5) & 1) << 23 | (m & 31) << 12;
        t = (t >> 6) << 29 | ((t >> 5) & 1) << 23 | (t & 31) << 11;
        w[22] = (r & 0x404003FFu) | 0x0E200800u | m;
        w[23] = (r & 0x004003FFu) | 0x5E200800u | m;
        w[24] = (r & 0x405F03FFu) | 0x0E200400u | t;
        w[25] = (r & 0x005F03FFu) | 0x5E200400u | t;
        w[26] = (r & 0x607F03FFu) | 0x0F200400u | f << 11;
        w[27] = (r & 0x207F03FFu) | 0x5F200400u | f << 11;
        w[28] = (r & 0x001F03FFu) | 0x5E000400u;
        w[29] = (r & 0x401F03FFu) | 0x0E002C00u;
        w[30] = (r & 0x401F73FFu) | 0x0E000000u;
        w[31] = (r & 0x40C0001Fu) | 0x0C000000u | (u32)(next_random() % 2) << 23 |
                (u32)(next_random() % 2 ? 31 : (r >> 16) & 31) << 16 | (u32) "\x00\x04\x08"[next_random() % 3] << 12 |
                ((r >> 10) & 3) << 10 | ((r >> 5) & 31) << 5;
        w[32] = (r & 0x40FFFFFFu & ~0x001F0000u) | 0x0D000000u | (u32)(next_random() % 2 ? 31 : (r >> 16) & 31) << 16;
        w[33] = (r & 0x60DFF3FFu) | 0x0E200000u;
        w[34] = (r & 0x60C003FFu) | 0x0E200800u | (u32)misc[next_random() % sizeof(misc)] << 12;
        w[35] = (r & 0x60C003FFu) | 0x0E300800u | (u32)across[next_random() % sizeof(across)] << 12;
        w[36] = (r & 0x20C003FFu) | 0x5E300800u | (u32)pairwise[next_random() % sizeof(pairwise)] << 12;
        w[37] = (r & 0x60FF0BFFu) | 0x0F000000u | (u32)element[next_random() % sizeof(element)] << 12 |
                (next_random() % 2 ? 0x50000000u : 0);
        w[38] = (r & 0x6007FBFFu & ~0x0000F800u) | 0x0F000400u | (u32)shifts[next_random() % sizeof(shifts)] << 11 | immh;
        w[39] = (r & 0x2007FBFFu & ~0x0000F800u) | 0x5F000400u |
                (u32)scalar_shifts[next_random() % sizeof(scalar_shifts)] << 11 | (next_random() % 2 ? 1u << 22 : immh);
        w[40] = (r & 0x60DF07FFu) | 0x0E200400u | (u32)same[next_random() % sizeof(same)] << 11;
        w[41] = (r & 0x20DF07FFu) | 0x5E200400u | (u32)scalar_same[next_random() % sizeof(scalar_same)] << 11;
        w[42] = (r & 0x20C003FFu) | 0x5E200800u | (u32)scalar_misc[next_random() % sizeof(scalar_misc)] << 12;
        w[43] = (r & 0x00DF03FFu) | 0x5E200000u | (u32) "\x09\x0B\x0D"[next_random() % 3] << 12;
        w[44] = (r & 0x401F03FFu) | (next_random() % 2 ? 0x2E209C00u : 0x0E20E000u);
        w[45] = (r & 0x20DF03FFu) | 0x5E200400u | (0x1Cu + (u32)(next_random() % 2)) << 11;
        w[46] = (r & 0x204003FFu) | 0x5EA00800u | (0x0Cu + (u32)(next_random() % 3)) << 12;
        w[47] = (r & 0x604003FFu) | 0x0E216800u;
        w[48] = (r & 0x204003FFu) | 0x5E216800u | (next_random() % 4 ? 0x20400000u : 0);
        w[49] = (r & 0x001F03FFu) | 0x5E000000u | (u32)(next_random() % 8) << 12;
        w[50] = (r & 0x000003FFu) | 0x5E280800u | (u32)(next_random() % 4) << 12;
        got += DIRECTED;
    }
    return got;
}

/* the words the other instruction tests hand-check against expected values (llvm_module's program, native_codegen's
   semantic cases and the FP witnesses' vector and reserved encodings), so that QEMU checks them from random states
   too */
static const u32 kept[] = {
    /* llvm_module.c: its program[] and tail[] */
    0xD2800000, 0xD2800181, 0x1E6F1001, 0x6F00E402, 0x94000016, 0x9E620003, 0x1E632800, 0x1E610800, 0x4EA28442,
    0x4E040C24, 0x4EA48442, 0xF1000421, 0x54FFFF01, 0x10000062, 0xD61F0040, 0xD4200020, 0xF81F0FE0, 0xA8C113E3,
    0xB4000043, 0x910FA000, 0x885FFFE7, 0x8808FFE0, 0x37180040, 0xD2401C00, 0xAA1F03FE, 0x14000047, 0x8B010000,
    0xD37FF804, 0xCA440C00, 0x9B011005, 0x9AC108A6, 0x9A868000, 0xD65F03C0, 0xAA0003E6, 0xD40000E1, 0x9E780009,
    /* codegen_test.c's semantic cases */
    0x31000420, 0xBA1F03E0, 0xDA1F03E0, 0xD3485C20, 0x93401C20, 0x93C12020, 0x9E220020, 0x9E660020, 0x1E620C20,
    0x4EA28420, 0x4E22CC20, 0x4E040C20, 0x4E183C20, 0x5FA21820, 0xF94007E0, 0xF900043F, 0x17FFFFFF, 0x1200FC00,
    0x1E221820, 0x1F020C20, 0x1E22C020, 0x1E200020, 0x4E22D420, 0x1E63C020, 0x9E580020, 0x1E61C020, 0x1E228820,
    0x1E23C020, 0x0E216820, 0x1F420C20, 0x1F620C20, 0x4E62CC20, 0x7EE1D820, 0x4EA1D820, 0x4EE1D820, 0x5EE1D820,
    0x9E630020, 0x6E61D820, 0x4E218800, 0x6E20D400, 0x6E21D820,
    /* the FP witnesses' reserved encodings */
    0x5E218820, 0x0E618820, 0x5E217820, 0x5EA1C820, 0x7EA1C820, 0x7EA1F820};
#define KEPT (sizeof(kept) / sizeof(kept[0]))

/* ---- the cases ---- */

/* a word, or a store-exclusive after the load-exclusive that arms its monitor */
typedef struct {
    u32 words[2], nwords;
    u32 step;  /* its steps' first index in the LLVM table */
    int skip;  /* no emitter yet: the Switch target leaves a word UNDEF or the backend has no emitter for it */
    char op[40], switch_op[40]; /* the max and Switch targets' names for the (last) word, "" for UNDEF */
} Item;

/* a store-exclusive (STXR, STLXR, STXP, STLXP): size 001000 o2=0 L=0 o1 Rs o0 Rt2 Rn Rt */
static int store_exclusive(u32 w) { return (w & 0x3FC00000u) == 0x08000000u; }

/* the load-exclusive of the same size and address into registers the store doesn't name */
static u32 load_exclusive(u32 st) {
    const u32 rt = st & 31, rn = (st >> 5) & 31, rt2 = (st >> 10) & 31, rs = (st >> 16) & 31, pair = (st >> 21) & 1;
    u32 regs[2], k = 0, r;
    for (r = 0; r < 31 && k < 2; ++r)
        if (r != rt && r != rn && r != rt2 && r != rs) regs[k++] = r;
    return (st & 0xC0200000u) | 0x08400000u | 31u << 16 | (pair ? regs[1] : 31u) << 10 | rn << 5 | regs[0];
}

static u64 item_pc(const Item* it) { return it->nwords > 1 ? PAIR_PC : PC; }

/* a word whose registers should point into the window: a load or store, a system instruction (DC ZVA), or one the
   Switch leaves UNDEF, which QEMU may still run */
static int memory_pattern(u32 word) {
    SnxA64Insn insn;
    const SnxA64Op op = snx_a64_decode_word(word, 0, &insn);
    return op == SNX_A64_OP_UNDEF || op == SNX_A64_OP_SYS || snx_ir_ldst_handles(op);
}

/* ---- the runtime's callbacks, recorded for the outcome ---- */
static void cb_svc(CPU* c, u32 imm) {
    (void)c;
    g_test.events->svc = 1;
    g_test.events->imm = imm;
}
static void cb_undef(CPU* c, u64 pc, u32 w) {
    (void)c, (void)pc, (void)w;
    g_test.events->undef = 1;
}
static void cb_note(u64 pc) { (void)pc; }
static u64 cb_ticks(void) { return 123456789; }
static void cb_brk(CPU* c, u64 pc, u32 imm) {
    (void)c, (void)pc;
    g_test.events->brk = 1;
    g_test.events->imm = imm;
}
static void cb_fpcr(CPU* c) { (void)c; }
static void cb_sev(void) {}
static void cb_wfe(CPU* c) { (void)c; }

/* ---- the comparison ---- */
typedef struct {
    char op[40], cause[48];
    unsigned long count, expected;
    int listed;
    char examples[EXAMPLES][1536];
    unsigned nexamples;
} Group;

typedef struct {
    Group* groups;
    size_t ngroups, cap;
} Groups;

static Group* group(Groups* g, const char* op, const char* cause) {
    size_t i;
    for (i = 0; i < g->ngroups; ++i)
        if (!strcmp(g->groups[i].op, op) && !strcmp(g->groups[i].cause, cause)) return &g->groups[i];
    if (g->ngroups == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 64;
        g->groups = (Group*)snx_realloc(g->groups, g->cap * sizeof(*g->groups));
    }
    memset(&g->groups[g->ngroups], 0, sizeof(Group));
    snprintf(g->groups[g->ngroups].op, sizeof(g->groups[0].op), "%s", op);
    snprintf(g->groups[g->ngroups].cause, sizeof(g->groups[0].cause), "%s", cause);
    return &g->groups[g->ngroups++];
}

static void state_of(const CPU* c, SnxQrefState* s) {
    unsigned i;
    memset(s, 0, sizeof(*s));
    for (i = 0; i < 31; ++i) s->x[i] = c->x[i];
    s->sp = c->x[31];
    s->nzcv = c->n << 31 | c->z << 30 | c->cf << 29 | c->vf << 28;
    s->fpcr = c->fpcr;
    s->fpsr = c->fpsr;
    for (i = 0; i < 32; ++i) {
        s->v[i][0] = c->v[i].d[0];
        s->v[i][1] = c->v[i].d[1];
    }
}

/* how a case ended, both sides alike: NEXT and BRANCH are one, by the pc */
static const char* outcome_kind(u32 outcome) {
    switch (outcome) {
        case SNX_QREF_NEXT:
        case SNX_QREF_BRANCH: return "runs";
        default: return snx_qref_outcome_name(outcome);
    }
}

typedef struct {
    u32 outcome;
    u64 pc, imm;
    SnxQrefState state;
} End;

/* the register an MRS of a system register QEMU gives otherwise reads into, which the comparison leaves out (-1 for
   none), and the value the Switch gives for it when constant */
static int environment_register(u32 word, u64* value, int* fixed) {
    const char* name;
    int kind = snx_qref_switch_sysreg(word, value, &name);
    *fixed = kind == SNX_QREF_SYSREG_FIXED;
    if (kind != SNX_QREF_SYSREG_NONE) return (int)(word & 31);
    if ((word & ~0x1Fu) == 0xD53BD040u) return (int)(word & 31); /* MRS TPIDR_EL0: other cases' MSR leave it set */
    return -1;
}

/* the parts of the end states that differ, as the cause ("" when none), and the example's text */
static void compare(const Item* it, const SnxQrefState* start, const End* l, const End* q, const u8* initial,
                    const u8* llvm, const SnxQrefBatch* b, const SnxQrefResult* r, char* cause, size_t cause_size,
                    char* text, size_t text_size) {
    size_t used = 0, cu = 0;
    unsigned i;
    int x = 0, v = 0;
    u64 switch_value = 0;
    int fixed, env = environment_register(it->words[it->nwords - 1], &switch_value, &fixed);
    u32 rn = (it->words[it->nwords - 1] >> 5) & 31, rm = (it->words[it->nwords - 1] >> 16) & 31,
        ra = (it->words[it->nwords - 1] >> 10) & 31;
#define PART(name) cu += (size_t)snprintf(cause + cu, cu < cause_size ? cause_size - cu : 0, "%s%s", cu ? "+" : "", name)
#define TEXT(...)                                                                                                     \
    do {                                                                                                              \
        if (used < text_size) used += (size_t)snprintf(text + used, text_size - used, __VA_ARGS__);                   \
    } while (0)
    cause[0] = 0;
    text[0] = 0;
    TEXT("  word %08x", it->words[it->nwords - 1]);
    if (it->nwords > 1) TEXT(" after %08x", it->words[0]);
    TEXT(" (the Switch target decodes it as %s)\n", it->switch_op[0] ? it->switch_op : "UNDEF");
    TEXT("  start x%u=%016llx x%u=%016llx x%u=%016llx sp=%016llx nzcv=%x fpcr=%08x fpsr=%08x\n", rn,
         (unsigned long long)(rn < 31 ? start->x[rn] : start->sp), rm,
         (unsigned long long)(rm < 31 ? start->x[rm] : start->sp), ra,
         (unsigned long long)(ra < 31 ? start->x[ra] : start->sp), (unsigned long long)start->sp, start->nzcv >> 28,
         start->fpcr, start->fpsr);
    TEXT("  start v%u=%016llx:%016llx v%u=%016llx:%016llx v%u=%016llx:%016llx\n", rn,
         (unsigned long long)start->v[rn][1], (unsigned long long)start->v[rn][0], rm,
         (unsigned long long)start->v[rm][1], (unsigned long long)start->v[rm][0], ra,
         (unsigned long long)start->v[ra][1], (unsigned long long)start->v[ra][0]);
    if (strcmp(outcome_kind(l->outcome), outcome_kind(q->outcome))) {
        snprintf(cause, cause_size, "llvm-%s/qemu-%s", outcome_kind(l->outcome), outcome_kind(q->outcome));
        TEXT("  llvm %s pc %llx, qemu %s pc %llx (signal %u, fault %llx)\n", snx_qref_outcome_name(l->outcome),
             (unsigned long long)l->pc, snx_qref_outcome_name(q->outcome), (unsigned long long)q->pc, r->signo,
             (unsigned long long)r->fault);
        return;
    }
    /* the pc after an UNDEF is the runtime's to choose. Linux sets TCR_EL1.TBI0, so QEMU's user mode drops the top
       byte of a branch's target: the kernel's setting, not the instruction's */
    if (l->outcome != SNX_QREF_UNDEF && l->pc != q->pc && (l->pc & 0x00FFFFFFFFFFFFFFull) != q->pc) {
        PART("pc");
        TEXT("  pc llvm %llx (%s) qemu %llx (%s, signal %u)\n", (unsigned long long)l->pc,
             snx_qref_outcome_name(l->outcome), (unsigned long long)q->pc, snx_qref_outcome_name(q->outcome), r->signo);
    }
    if ((l->outcome == SNX_QREF_BRK || l->outcome == SNX_QREF_SVC) && l->imm != q->imm) {
        PART("imm");
        TEXT("  imm llvm %llx qemu %llx\n", (unsigned long long)l->imm, (unsigned long long)q->imm);
    }
    for (i = 0; i < 31; ++i) {
        u64 want = q->state.x[i];
        if ((int)i == env) {
            if (!fixed) continue;
            want = switch_value;
        }
        if (l->state.x[i] != want) {
            if (!x++) PART("x");
            TEXT("  x%u: start %016llx llvm %016llx qemu %016llx\n", i, (unsigned long long)start->x[i],
                 (unsigned long long)l->state.x[i], (unsigned long long)want);
        }
    }
    if (l->state.sp != q->state.sp) {
        PART("sp");
        TEXT("  sp: start %016llx llvm %016llx qemu %016llx\n", (unsigned long long)start->sp,
             (unsigned long long)l->state.sp, (unsigned long long)q->state.sp);
    }
    if ((l->state.nzcv ^ q->state.nzcv) & 0xF0000000u) {
        PART("nzcv");
        TEXT("  nzcv: start %x llvm %x qemu %x\n", start->nzcv >> 28, l->state.nzcv >> 28, q->state.nzcv >> 28);
    }
    for (i = 0; i < 32; ++i)
        if (l->state.v[i][0] != q->state.v[i][0] || l->state.v[i][1] != q->state.v[i][1]) {
            if (!v++) PART("v");
            TEXT("  v%u: start %016llx:%016llx llvm %016llx:%016llx qemu %016llx:%016llx\n", i,
                 (unsigned long long)start->v[i][1], (unsigned long long)start->v[i][0],
                 (unsigned long long)l->state.v[i][1], (unsigned long long)l->state.v[i][0],
                 (unsigned long long)q->state.v[i][1], (unsigned long long)q->state.v[i][0]);
        }
    if (l->state.fpsr != q->state.fpsr) {
        PART("fpsr");
        TEXT("  fpsr: start %08x llvm %08x qemu %08x\n", start->fpsr, l->state.fpsr, q->state.fpsr);
    }
    if (l->state.fpcr != q->state.fpcr) {
        PART("fpcr");
        TEXT("  fpcr: start %08x llvm %08x qemu %08x\n", start->fpcr, l->state.fpcr, q->state.fpcr);
    }
    /* memory: every byte QEMU reports must be the LLVM side's, and every byte the LLVM side changed must be in a run
       QEMU reports */
    {
        u64 first = ~0ull;
        unsigned c;
        int differs = r->truncated;
        for (c = 0; c < r->changes && !differs; ++c) {
            const SnxQrefChange* ch = &b->changes[r->first_change + c];
            u32 k;
            if (ch->address < WINDOW || ch->address + ch->size > WINDOW + WINDOW_SIZE) {
                differs = 1;
                first = ch->address;
                break;
            }
            for (k = 0; k < ch->size; ++k)
                if (llvm[ch->address - WINDOW + k] != b->bytes[ch->offset + k]) {
                    differs = 1;
                    first = ch->address + k;
                    break;
                }
        }
        for (i = 0; i < WINDOW_SIZE && !differs; i += SNX_QREF_PAGE) {
            u32 k;
            if (!memcmp(llvm + i, initial + i, SNX_QREF_PAGE)) continue;
            for (k = 0; k < SNX_QREF_PAGE && !differs; ++k) {
                u64 a = WINDOW + i + k;
                if (llvm[i + k] == initial[i + k]) continue;
                for (c = 0; c < r->changes; ++c) {
                    const SnxQrefChange* ch = &b->changes[r->first_change + c];
                    if (a >= ch->address && a < ch->address + ch->size) break;
                }
                if (c == r->changes) {
                    differs = 1;
                    first = a;
                }
            }
        }
        if (differs) {
            PART("mem");
            if (first == ~0ull)
                TEXT("  memory: QEMU's changes truncated\n");
            else if (first >= WINDOW && first < WINDOW + WINDOW_SIZE)
                TEXT("  memory: first difference at %llx, start %02x llvm %02x (%u QEMU runs)\n",
                     (unsigned long long)first, initial[first - WINDOW], llvm[first - WINDOW], r->changes);
            else
                TEXT("  memory: QEMU changed %llx, outside the window\n", (unsigned long long)first);
        }
    }
#undef PART
#undef TEXT
}

/* the QEMU case of an item from a state: a word at PC in the seeded window, one step, or a pair at PAIR_PC */
static int add_case(SnxQrefBatch* b, const Item* it, const CPU* start) {
    SnxQrefCase qc;
    snx_qref_case_init(&qc);
    qc.window = WINDOW;
    qc.window_size = WINDOW_SIZE;
    qc.seed_kind = SNX_QREF_SEED_RANDOM;
    qc.seed = SEED;
    qc.code = item_pc(it);
    qc.code_size = CODE_SIZE;
    qc.pc = item_pc(it);
    /* a pair runs free (each step enters through a signal, whose return clears the monitor) with the window
       writable, as a fault between the two would clear it too */
    qc.steps = it->nwords > 1 ? 0 : 1;
    qc.flags = it->nwords > 1 ? SNX_QREF_FULL_DIFF : 0;
    state_of(start, &qc.state);
    return snx_qref_add(b, &qc, it->words, it->nwords, NULL, 0);
}

/* ---- the expected-failure list ---- */
typedef struct {
    char op[40], cause[48];
    unsigned long counts[2]; /* the default run's, the sweep's */
    unsigned long seen;
} Expected;

static size_t read_expected(const char* path, Expected** out) {
    size_t n = 0, cap = 0, size = 0;
    char* text = (char*)snx_read_file(path, &size);
    char* line;
    *out = NULL;
    if (!text) return 0;
    text = (char*)snx_realloc(text, size + 1);
    text[size] = 0;
    for (line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        Expected e = {{0}, {0}, {0}, 0};
        if (line[0] == '#' || sscanf(line, "%39s %47s %lu %lu", e.op, e.cause, &e.counts[0], &e.counts[1]) != 4)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            *out = (Expected*)snx_realloc(*out, cap * sizeof(**out));
        }
        (*out)[n++] = e;
    }
    free(text);
    return n;
}

static int order_expected(const void* a, const void* b) {
    const Expected *x = a, *y = b;
    int c = strcmp(x->op, y->op);
    return c ? c : strcmp(x->cause, y->cause);
}

static int write_expected(const char* path, Expected* e, size_t n) {
    FILE* f = fopen(path, "w");
    size_t i;
    if (!f) return -1;
    qsort(e, n, sizeof(*e), order_expected);
    fputs("# llvm_qemu_differential's expected mismatches until the emitters are rewritten from QEMU's translate-a64.c:\n"
          "# the max target's name for the word, the cause (the state parts that differ, or llvm-X/qemu-Y for\n"
          "# different outcomes, or qemu-alignment-fault for an alignment fault the LLVM side doesn't run), and the\n"
          "# words differing in the default run and in --sweep. A group missing here fails the test, and so does a\n"
          "# listed one that no longer differs in a run whose count isn't 0.\n"
          "# --update rewrites the counts of the run it is given with. qemu-diff-triage.md has the judgement on each.\n",
          f);
    for (i = 0; i < n; ++i)
        if (e[i].counts[0] || e[i].counts[1])
            fprintf(f, "%-20s %-28s %6lu %6lu\n", e[i].op, e[i].cause, e[i].counts[0], e[i].counts[1]);
    return fclose(f);
}

/* ---- the run ---- */
typedef struct {
    const char *qemu, *runner, *dir;
    int quiet;
} Qemu;

int main(int argc, char** argv) {
    Qemu q;
    const char* expected_path;
    size_t states, samples, rounds, nwords, nitems = 0, i, k, nsteps = 0, compared = 0, mismatched = 0,
                                                    no_emitter = 0, nexpected;
    int sweep = 0, update = 0, jit = 0, portable = LLVM_STEPS_HOST, a, failed = 0;
    static const char* const forms[] = {"host", "portable", "generic"}; /* the emitters', by LLVM_STEPS_ value */
    u32* words = NULL;
    Item* items;
    Groups groups = {0}, missing = {0};
    Expected* expected;
    char error[1024], *path;
    LlvmSteps built;
    SnxIr* ir;
    StepFn* steps;
    RtAPI rt = {0};
    u8 *initial, *work;
    SnxQrefBatch batch = {0};
    double started = snx_seconds();
    FILE* f;
    if (argc == 3 && !strcmp(argv[1], "--skip")) {
        printf("llvm_qemu_differential skipped: %s\n", argv[2]);
        return 77;
    }
    if (argc < 5) {
        fputs("usage: snx-test-llvm-qemu-differential QEMU RUNNER DIR EXPECTED [--sweep] [--states N] [--seed N] "
              "[--quiet] [--update] [--jit] [--portable | --generic] | --skip REASON\n",
              stderr);
        return 2;
    }
    q.qemu = argv[1];
    q.runner = argv[2];
    q.dir = argv[3];
    expected_path = argv[4];
    q.quiet = 0;
    states = 0;
    for (a = 5; a < argc; ++a) {
        if (!strcmp(argv[a], "--sweep"))
            sweep = 1;
        else if (!strcmp(argv[a], "--quiet"))
            q.quiet = 1;
        else if (!strcmp(argv[a], "--update"))
            update = 1;
        else if (!strcmp(argv[a], "--jit"))
            jit = 1;
        else if (!strcmp(argv[a], "--portable") || !strcmp(argv[a], "--generic")) {
            int form = !strcmp(argv[a], "--portable") ? LLVM_STEPS_PORTABLE : LLVM_STEPS_GENERIC;
            if (portable && portable != form)
                return snx_error("%s after --%s: the emitters take one form a run", argv[a], forms[portable]);
            portable = form;
        } else if (a + 1 < argc && !strcmp(argv[a], "--states"))
            states = strtoul(argv[++a], NULL, 10);
        else if (a + 1 < argc && !strcmp(argv[a], "--seed"))
            g_test.rng = strtoull(argv[++a], NULL, 0) | 1;
        else
            return snx_error("unknown argument %s", argv[a]);
    }
    if (!states) states = sweep ? 6 : 3;
    samples = sweep ? 4 : 2;
    rounds = sweep ? 300 : 20;

    /* the words */
    nwords = lqd_row_words((unsigned)samples, next_random, &words);
    words = (u32*)snx_realloc(words, (nwords + DIRECTED * rounds + KEPT) * sizeof(*words));
    nwords += directed(rounds, words + nwords);
    memcpy(words + nwords, kept, sizeof(kept));
    nwords += KEPT;
    items = (Item*)snx_alloc((2 * nwords) * sizeof(*items));
    for (i = 0; i < nwords; ++i) {
        int unrunnable;
        const char *op = lqd_op_name(words[i], &unrunnable), *sw = lqd_switch_op_name(words[i]);
        unsigned pairs = store_exclusive(words[i]) ? 2 : 1, p;
        if (unrunnable) continue;
        for (p = 0; p < pairs; ++p) {
            Item* it = &items[nitems++];
            memset(it, 0, sizeof(*it));
            snprintf(it->op, sizeof(it->op), "%s", op);
            snprintf(it->switch_op, sizeof(it->switch_op), "%s", sw ? sw : "");
            it->nwords = p + 1;
            it->words[0] = p ? load_exclusive(words[i]) : words[i];
            it->words[p] = words[i];
        }
    }
    free(words);

    /* the LLVM steps, one function a word, at the word's pc; a word without an emitter is noted and left out */
    if (llvm_steps_begin(&built, jit, portable, error, sizeof(error))) return snx_error("%s", error);
    ir = built.ir;
    for (i = 0; i < nitems; ++i) {
        Item* it = &items[i];
        it->step = (u32)nsteps;
        for (k = 0; k < it->nwords; ++k) {
            unsigned long before = ir->fallbacks;
            SnxA64Insn max, sw;
            SnxU64 features = 0;
            /* the load/store emitters (ir_ldst.c) implement the LSE and RCpc atomics too, the scalar FP ones
               (ir_fpu.c) FEAT_FP16, FEAT_JSCVT, FEAT_FRINTTS and BFCVT, the AdvSIMD ones (ir_vec.c) the vector
               extensions: a word the Switch lacks that they emit runs with max's features, against QEMU's -cpu max */
            if (snx_a64_decode_word(it->words[k], SNX_A64_TARGET_MAX, &max) != SNX_A64_OP_UNDEF &&
                (snx_ir_ldst_handles(max.op) || snx_ir_fpu_handles(max.op) || snx_ir_vec_handles(max.op)) &&
                (max.feature & ~(SnxU64)SNX_A64_TARGET_SWITCH))
                features = SNX_A64_TARGET_MAX;
            else if (snx_a64_decode_word(it->words[k], 0, &sw) == SNX_A64_OP_UNDEF)
                it->skip = 1;
            llvm_steps_add(&built, it->words[k], item_pc(it) + 4 * k, features);
            ++nsteps;
            if (ir->fallbacks != before) {
                it->skip = 1;
                ir->fallbacks = before; /* noted here: the module is still wanted for the rest */
            }
        }
    }
    if (!q.quiet) printf("%lu cases of %lu words; compiling %lu LLVM steps\n", (unsigned long)nitems,
                         (unsigned long)nwords, (unsigned long)nsteps);
    if (llvm_steps_load(&built, error, sizeof(error))) return snx_error("%s", error);
    steps = built.steps;
    rt.svc = cb_svc;
    rt.undef = cb_undef;
    rt.note_hint = cb_note;
    rt.ticks = cb_ticks;
    rt.brk = cb_brk;
    rt.fpcr_changed = cb_fpcr;
    rt.sev = cb_sev;
    rt.wfe = cb_wfe;

    /* the window both sides start from: the seed, and pads in the code region (each case's words go over them) */
    initial = (u8*)snx_alloc(WINDOW_SIZE);
    work = (u8*)snx_alloc(WINDOW_SIZE);
    for (i = 0; i < WINDOW_SIZE; i += 8) {
        u64 v = snx_qref_seed64(SNX_QREF_SEED_RANDOM, SEED, WINDOW, i);
        for (k = 0; k < 8; ++k) initial[i + k] = (u8)(v >> (8 * k));
    }
    for (i = PC - WINDOW; i < PC - WINDOW + CODE_SIZE; i += 4) memcpy(initial + i, &(u32){SNX_QREF_PAD}, 4);
    memcpy(work, initial, WINDOW_SIZE);

    /* in batches: the states, QEMU's run of them, then the LLVM side's of each and the comparison */
    expected = NULL;
    nexpected = read_expected(expected_path, &expected);
    for (i = 0; i < nitems;) {
        size_t first = i, s;
        CPU* starts;
        int* index;
        u64 batch_seed = next_random();
        size_t end = first;
        while (end < nitems && (end - first) * states < BATCH) ++end;
        starts = (CPU*)snx_alloc((end - first) * states * sizeof(*starts));
        index = (int*)snx_alloc((end - first) * states * sizeof(*index));
        g_test.rng = batch_seed;
        for (k = first; k < end; ++k) {
            const Item* it = &items[k];
            int addresses = memory_pattern(it->words[it->nwords - 1]);
            for (s = 0; s < states; ++s) {
                CPU* c = &starts[(k - first) * states + s];
                random_state(c, it->words[it->nwords - 1], addresses);
                /* a word without an emitter runs its first state only, under QEMU, for the coverage report */
                index[(k - first) * states + s] = -1;
                if (it->skip && s) continue;
                index[(k - first) * states + s] = add_case(&batch, it, c);
                if (index[(k - first) * states + s] < 0) return snx_error("the runner refuses %08x", it->words[0]);
            }
        }
        if (snx_qref_run(&batch, q.qemu, q.runner, "max", q.dir, error, sizeof(error)))
            return snx_error("QEMU: %s (words %08x %08x)", error, items[first].words[0], items[first].words[1]);
        for (k = first; k < end; ++k) {
            const Item* it = &items[k];
            for (s = 0; s < states; ++s) {
                const CPU* start = &starts[(k - first) * states + s];
                const int at = index[(k - first) * states + s];
                const SnxQrefResult* r;
                CPU c;
                Events ev = {0};
                End l = {0}, qe;
                SnxQrefState st;
                char cause[48], text[1536];
                u32 w;
                if (at < 0) continue;
                r = &batch.results[at];
                if (r->outcome == SNX_QREF_ERROR) return snx_error("QEMU could not run %08x", it->words[0]);
                if (it->skip) {
                    if (r->outcome != SNX_QREF_UNDEF) {
                        ++group(&missing, it->op, it->switch_op[0] ? "Switch" : "")->count;
                        ++no_emitter;
                    }
                    break;
                }
                /* an alignment fault (an ordered access across 16 bytes, LDAPUR's or STLUR's): the backend
                   raises none, and on an AArch64 host the access faults the host and ends this test, so the LLVM
                   side doesn't run it. It differs alike on every host */
                if (r->outcome == SNX_QREF_FAULT && r->signo == SIGBUS_A64) {
                    Group* g = group(&groups, it->op, "qemu-alignment-fault");
                    const u32 word = it->words[it->nwords - 1], rn = (word >> 5) & 31;
                    if (g->nexamples < EXAMPLES)
                        snprintf(g->examples[g->nexamples++], sizeof(text),
                                 "  word %08x (the Switch target decodes it as %s)\n  start x%u=%016llx\n"
                                 "  qemu fault pc %llx (signal %u, fault %llx), not run on the LLVM side\n",
                                 word, it->switch_op[0] ? it->switch_op : "UNDEF", rn, (unsigned long long)start->x[rn],
                                 (unsigned long long)r->pc, r->signo, (unsigned long long)r->fault);
                    ++g->count;
                    ++mismatched;
                    break;
                }
                /* the LLVM side */
                c = *start;
                c.rt = &rt;
                c.mem = work - WINDOW;
                if (it->nwords == 1) memcpy(work + PC - WINDOW, &it->words[0], 4);
                g_test.events = &ev;
                for (w = 0; w < it->nwords; ++w) {
                    l.pc = steps[it->step + w](&c);
                    if (ev.undef || ev.brk || l.pc != item_pc(it) + 4 * (w + 1)) break;
                }
                l.outcome = ev.undef ? SNX_QREF_UNDEF
                            : ev.brk ? SNX_QREF_BRK
                            : ev.svc ? SNX_QREF_SVC
                                     : SNX_QREF_BRANCH;
                l.imm = ev.imm;
                state_of(&c, &l.state);
                qe.outcome = r->outcome;
                if (snx_qref_switch_sysreg(it->words[it->nwords - 1], NULL, NULL) == SNX_QREF_SYSREG_UNDEF &&
                    r->outcome == SNX_QREF_NEXT)
                    qe.outcome = SNX_QREF_UNDEF; /* QEMU's user mode reads the ID registers EL0 can't */
                qe.pc = r->pc;
                qe.imm = r->imm;
                qe.state = r->state;
                /* the window as the case found it: the words over the pads */
                if (it->nwords == 1) memcpy(initial + PC - WINDOW, &it->words[0], 4);
                state_of(start, &st);
                compare(it, &st, &l, &qe, initial, work, &batch, r, cause, sizeof(cause), text, sizeof(text));
                memcpy(initial + PC - WINDOW, &(u32){SNX_QREF_PAD}, 4);
                for (w = 0; w < WINDOW_SIZE; w += SNX_QREF_PAGE)
                    if (memcmp(work + w, initial + w, SNX_QREF_PAGE)) memcpy(work + w, initial + w, SNX_QREF_PAGE);
                ++compared;
                /* beyond the Switch: a word the Cortex-A57 decodes as another name (a hint's NOP), or leaves UNDEF as
                   the backend does, has no emitter for what max makes of it yet */
                if (cause[0] && ((it->switch_op[0] && strcmp(it->switch_op, it->op)) ||
                                 (!it->switch_op[0] && l.outcome == SNX_QREF_UNDEF))) {
                    ++group(&missing, it->op, "")->count;
                    ++no_emitter;
                    break;
                }
                if (cause[0]) {
                    Group* g = group(&groups, it->op, cause);
                    if (g->nexamples < EXAMPLES) snprintf(g->examples[g->nexamples++], sizeof(text), "%s", text);
                    ++g->count;
                    ++mismatched;
                    break; /* a word counts once, by its first differing state */
                }
            }
        }
        snx_qref_clear(&batch);
        free(starts);
        free(index);
        i = end;
    }
    snx_qref_free(&batch);

    /* the expected-failure list against what differed */
    for (i = 0; i < groups.ngroups; ++i) {
        Group* g = &groups.groups[i];
        for (k = 0; k < nexpected; ++k)
            if (!strcmp(expected[k].op, g->op) && !strcmp(expected[k].cause, g->cause)) break;
        if (k < nexpected) {
            g->listed = 1;
            g->expected = expected[k].counts[sweep];
            expected[k].seen = g->count;
        } else if (update) {
            Expected e = {{0}, {0}, {0}, 0};
            snprintf(e.op, sizeof(e.op), "%s", g->op);
            snprintf(e.cause, sizeof(e.cause), "%s", g->cause);
            e.seen = g->count;
            expected = (Expected*)snx_realloc(expected, (nexpected + 1) * sizeof(*expected));
            expected[nexpected++] = e;
            g->listed = 1;
        }
    }
    printf("LLVM against QEMU%s, %s emitters: %lu cases of %lu rows' and directed words, %lu runs compared, %lu "
           "without an emitter yet, %lu differing in %lu groups\n",
           sweep ? " (sweep)" : "", forms[portable], (unsigned long)nitems, (unsigned long)lqd_rows(),
           (unsigned long)compared, (unsigned long)no_emitter, (unsigned long)mismatched,
           (unsigned long)groups.ngroups);
    for (i = 0; i < groups.ngroups; ++i) {
        const Group* g = &groups.groups[i];
        if (!g->listed) failed = 1;
        if (!q.quiet || !g->listed)
            printf("  %-20s %-28s %5lu %s\n", g->op, g->cause, g->count,
                   g->listed ? (g->count == g->expected ? "expected" : "expected, count changed") : "NEW");
    }
    for (k = 0; k < nexpected; ++k)
        if (expected[k].counts[sweep] && !expected[k].seen) {
            printf("  %-20s %-28s no longer differs: remove it from %s\n", expected[k].op, expected[k].cause,
                   expected_path);
            if (!update) failed = 1;
        }
    if (update) {
        for (k = 0; k < nexpected; ++k) expected[k].counts[sweep] = expected[k].seen;
        if (write_expected(expected_path, expected, nexpected)) return snx_error("cannot write %s", expected_path);
        printf("updated %s\n", expected_path);
        failed = 0;
    }

    /* the reports */
    snx_mkdirs(q.dir);
    path = snx_path_join(q.dir, "llvm_qemu_coverage.txt");
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "# the names (max target) of words QEMU runs and the backend has no emitter for yet, with the words;\n"
                   "# Switch: the Switch target decodes the word, as the same name, and no emitter takes it\n");
        for (i = 0; i < missing.ngroups; ++i)
            fprintf(f, "%s\t%lu\t%s\n", missing.groups[i].op, missing.groups[i].count, missing.groups[i].cause);
        fclose(f);
    }
    free(path);
    path = snx_path_join(q.dir, "llvm_qemu_mismatches.txt");
    f = fopen(path, "w");
    if (f) {
        for (i = 0; i < groups.ngroups; ++i) {
            const Group* g = &groups.groups[i];
            unsigned e;
            fprintf(f, "%s %s: %lu words\n", g->op, g->cause, g->count);
            for (e = 0; e < g->nexamples; ++e) fputs(g->examples[e], f);
        }
        fclose(f);
    }
    printf("%lu names without an emitter in %s/llvm_qemu_coverage.txt, examples in llvm_qemu_mismatches.txt; "
           "%.1f s: %s\n",
           (unsigned long)missing.ngroups, q.dir, snx_seconds() - started,
           failed ? "FAILED" : "passed");
    free(path);
    llvm_steps_end(&built);
    free(initial);
    free(work);
    free(items);
    free(groups.groups);
    free(missing.groups);
    free(expected);
    return failed;
}
