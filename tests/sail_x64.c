/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's x86-64 back end (tools/sailgen/x64.h, its instruction selection derived from the Intel SDM) against QEMU,
   word by word: every word of the decoder's rows (llvm_qemu_words.c, decoded for the ounce target, each again with Rd,
   Rn and Rm 31) that the x86-64 back end's roots (a64_x64.roots) take becomes x86-64 code for u64 step(CPU*), once with
   the host's BMI1, BMI2 and LZCNT and once with none (the candidates without them); both run from the states of the
   word's own seed (sail_qemu.h) beside qemu-aarch64 -cpu max (tests/qemu_ref), and every register, the flags, FPCR,
   FPSR, the vector registers, the memory it changed and how it ended (the next pc, UNDEF) must be QEMU's. A host where
   QEMU can't run beside the test (Windows) holds them to a reference of QEMU's ends that the qemu-reference job records
   (--record): the hash of each word's every state. The words QEMU can't stand for, and a store-exclusive whose monitor
   is armed where the memory changed, are held to what LLVM's steps made of them, frozen (--frozen FILE, sail_qemu.h).
   Then sequences of two to four of those words (--sequences N) become
   one block each, one SSA (a register read once, a compare's flags read by the next word's condition, the stores a
   later one overwrites dropped), run beside the code of each word alone at its pc, one after another.
     snx-test-sail-x64 (--qemu QEMU RUNNER DIR [--record FILE] | --reference[-if-there] FILE | --skip REASON)
                       [--frozen FILE] [--states N] [--samples N] [--sequences N] [--pool N] [--xmm N] [--verbose]
                       [--dump WORD...]
   --pool N gives the back end N host registers of its nine, so it spills (ctest runs it with 3 too), --xmm N of its
   sixteen vector ones (ctest: 6 with 3). --record also writes QEMU's ends of every word a host of any feature makes;
   --reference-if-there skips (77) where the file is not there
   A word the x86-64 back end takes but can't make (its sg_fail) is a failure too, named with why */
#include "sail_x64_emit.h"
#include "sail_qemu.h"
#include "llvm_qemu_words.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__)
#include <cpuid.h>
#endif
#include "support/exec_pages.h"
#if defined(_WIN32)
/* no signal mask to keep */
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(buf, mask)   setjmp(buf)
#define siglongjmp(buf, value) longjmp(buf, value)

/* pools values spill in: x64.h's first registers on Windows are a pair's and a shift count's, and it has six XMM ones */
#define FEW_REGISTERS 4
#define FEW_XMM       4
#else
#define FEW_REGISTERS 3
#define FEW_XMM       6
#endif

#define TARGET SNX_A64_TARGET_OUNCE
#define CODE_MAX 8192
/* guest memory for the loads and stores, as llvm_sail_ir.c's: each side's own, guest address 0 MEMORY_ZERO bytes in,
   the same random bytes, every access below MEMORY_TOUCHED (the address registers point at 0x1000 to 0x1ff0) */
#define MEMORY_SIZE    (4u << 20)
#define MEMORY_ZERO    (2u << 20)
#define MEMORY_TOUCHED 0x14000u

static struct {
    u64 seed;
    unsigned undef;
    int verbose;
    sigjmp_buf jump;
    int jumping;
    unsigned char* memory[3]; /* the LLVM side's, the x86-64 side's, the bytes both start from */
    unsigned sev, wfe;
    /* what run allocates, which main frees after every one of its ways out */
    u32* rows;
    void* words; /* run's Words */
    void* sequences;
} g_test;

static u64 rnd(void) {
    g_test.seed ^= g_test.seed << 13;
    g_test.seed ^= g_test.seed >> 7;
    g_test.seed ^= g_test.seed << 17;
    return g_test.seed;
}

/* the SIMD&FP state a start has: each doubleword random, or two singles or a double of the values floating point
   treats apart (zeros, infinities, quiet and signalling NaNs, denormals, the largest, ordinary numbers); FPCR's
   rounding mode, FZ, DN, AHP and FZ16 random in odd states (0 in the rest), FPSR's flags random */
static u64 fp_value(int dbl) {
    static const u64 d[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xc008000000000000ull,
                            0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0x7ff4000000000000ull,
                            0x000fffffffffffffull, 0x8000000000000001ull, 0x7fefffffffffffffull, 0x0010000000000000ull,
                            0x41dfffffffc00000ull, 0xc3e0000000000000ull, 0x3fe0000000000000ull, 0x4059000000000000ull};
    static const u32 f[] = {0, 0x80000000u, 0x3f800000u, 0xc0400000u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
                            0x7fa00000u, 0x007fffffu, 0x80000001u, 0x7f7fffffu, 0x00800000u, 0x4effffffu, 0xdf000000u,
                            0x3f000000u, 0x42c80000u};
    u64 r = rnd();
    if (r & 0x100) return r >> 9; /* any bits */
    if (dbl) return d[r & 15];
    return (u64)f[r & 15] | (u64)f[(r >> 4) & 15] << 32;
}
static void fp_state(CPU* c, unsigned s) {
    unsigned r;
    for (r = 0; r < 32; ++r) {
        c->v[r].d[0] = fp_value(rnd() & 1);
        c->v[r].d[1] = fp_value(rnd() & 1);
    }
    c->fpcr = s & 1 ? (u32)rnd() & (3u << 22 | 1u << 24 | 1u << 25 | 1u << 26 | 1u << 19) : 0;
    c->fpsr = (u32)rnd() & 0x9f;
}
static int fp_differs(const CPU* a, const CPU* b) {
    return memcmp(a->v, b->v, sizeof a->v) || a->fpsr != b->fpsr || a->fpcr != b->fpcr;
}


static const char* op_name(u32 word) {
    SnxA64Insn insn;
    return snx_a64_decode_word(word, TARGET, &insn) != SNX_A64_OP_UNDEF ? snx_a64_op_text(insn.op) : NULL;
}

static unsigned host_features(void) {
    unsigned f = SNX_X64_ABI;
#if defined(__x86_64__)
    unsigned a, b, c, d;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) {
        if (b & (1u << 3)) f |= SNX_X64_BMI1;
        if (b & (1u << 8)) f |= SNX_X64_BMI2;
    }
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d) && (c & (1u << 5))) f |= SNX_X64_LZCNT;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        if (c & 1u) f |= SNX_X64_SSE3;
        if (c & (1u << 1)) f |= SNX_X64_PCLMUL;
        if (c & (1u << 9)) f |= SNX_X64_SSSE3;
        if (c & (1u << 19)) f |= SNX_X64_SSE41;
        if (c & (1u << 20)) f |= SNX_X64_SSE42;
        if (c & (1u << 23)) f |= SNX_X64_POPCNT;
        if ((c & (1u << 27)) && (c & (1u << 28))) {
            unsigned lo, hi;
            __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
            if ((lo & 6) == 6) {
                f |= SNX_X64_AVX;
                if (c & (1u << 12)) f |= SNX_X64_FMA;
                if (c & (1u << 29)) f |= SNX_X64_F16C;
            }
        }
    }
#endif
    return f;
}

/* the back end's sg_fail aborts: a word it can't make is named, and the test goes on */
static void on_abort(int signal_number) {
    (void)signal_number;
#if defined(_WIN32)
    signal(SIGABRT, on_abort); /* the handler is taken once there */
#endif
    if (g_test.jumping) siglongjmp(g_test.jump, 1);
}

static void on_sev(void) { ++g_test.sev; }
static void on_wfe(CPU* c) {
    (void)c;
    ++g_test.wfe;
}
static void on_undef(CPU* c, u64 pc, u32 word) {
    (void)c;
    (void)pc;
    (void)word;
    ++g_test.undef;
}

typedef u64 (*Step)(CPU* c);
/* A step is generated code, with no type signature before its entry, which the function sanitizer's check of an
   indirect call reads: the 8 bytes before the callee, below the mapping for the first one. So the call is unchecked */
[[clang::no_sanitize("function")]] static u64 run_step(Step step, CPU* c) { return step(c); }

/* FPCR.FZ set, as many games run: operands and results the host's arithmetic gives exactly, an exact zero among them,
   take the inline code with no helper call (snx_x64_fp_calls); a denormal operand, which FZ flushes, takes the helper
   and its IDC. FPSR's IXC is set first, as the inline code needs where an operation may be inexact */
/* CASP X0, X1, X2, X3, [X4] of doublewords (LOCK CMPXCHG16B, the new pair from R8 and R9): where memory holds X0:X1
   it takes X2:X3, X3 the machine instruction's sixth input; else it keeps its pair; X0:X1 the old pair either way.
   Made on this host and the plain one, with every register and with three (spills) */
static size_t check_casp(void) {
    static const u64 old[2] = {0x1111111111111111ull, 0x2222222222222222ull};
    unsigned char* code = snx_test_pages(CODE_MAX);
    RtAPI rt = {.undef = on_undef, .sev = on_sev, .wfe = on_wfe, .cas128 = snx_runtime_cas128};
    size_t failures = 0;
    int pool = snx_x64_pool();
    if (code == NULL) {
        fprintf(stderr, "FAIL: CASP: no memory for the code\n");
        return 1;
    }
    for (int variant = 0; variant < 4; ++variant) {
        unsigned features = variant & 1 ? SNX_X64_BASELINE : host_features();
        snx_x64_set_pool(variant & 2 ? FEW_REGISTERS : pool);
        if (snx_test_pages_exec(code, CODE_MAX, 0) ||
            !snx_x64_step(0x48207C82u, 0x1000, TARGET, features, code, CODE_MAX) ||
            snx_test_pages_exec(code, CODE_MAX, 1)) {
            fprintf(stderr, "FAIL: CASP X0, X1, X2, X3, [X4]: not made (variant %d)\n", variant);
            ++failures;
            continue;
        }
        for (int match = 0; match < 2; ++match) {
            CPU c;
            u64 now[2];
            unsigned char* at = g_test.memory[0] + MEMORY_ZERO + 0x1000;
            memcpy(at, old, sizeof old);
            memset(&c, 0, sizeof c);
            c.x[0] = match ? old[0] : 0x9999999999999999ull;
            c.x[1] = old[1];
            c.x[2] = 0x3333333333333333ull;
            c.x[3] = 0x4444444444444444ull;
            c.x[4] = 0x1000;
            c.mem = g_test.memory[0] + MEMORY_ZERO;
            c.rt = &rt;
            c.pc = 0x1000;
            run_step((Step)(void*)code, &c);
            memcpy(now, at, sizeof now);
            u64 want0 = match ? c.x[2] : old[0], want1 = match ? 0x4444444444444444ull : old[1];
            if (now[0] != want0 || now[1] != want1 || c.x[0] != old[0] || c.x[1] != old[1]) {
                fprintf(stderr,
                        "FAIL: CASP %s (variant %d): memory %016llx %016llx (want %016llx %016llx), X0 %016llx X1 "
                        "%016llx (want the old pair)\n",
                        match ? "where memory matches" : "where it doesn't", variant, (unsigned long long)now[0],
                        (unsigned long long)now[1], (unsigned long long)want0, (unsigned long long)want1,
                        (unsigned long long)c.x[0], (unsigned long long)c.x[1]);
                ++failures;
            }
        }
    }
    snx_x64_set_pool(pool);
    snx_test_pages_free(code, CODE_MAX);
    printf("CASP of doublewords, 4 ways made, matching and not: %zu failures\n", failures);
    return failures;
}
static size_t check_fz_hot(void) {
    static const struct {
        u32 word;
        const char* text;
        u64 v1, v2, v3;
        int cold;
        u64 want; /* V0's low doubleword, or the NZCV nibble for FCMP */
        u32 fpsr;
    } cases[] = {
        {0x1E222820u, "FADD s0, s1, s2", 0x3FC00000u, 0x40100000u, 0, 0, 0x40700000u, 0x10},
        {0x1E222820u, "FADD s0, s1, s2 of a denormal", 0x3FC00000u, 0x00000001u, 0, 1, 0x3FC00000u, 0x90},
        {0x1E220820u, "FMUL s0, s1, s2 of a zero", 0x3FC00000u, 0, 0, 0, 0, 0x10},
        {0x1E223820u, "FSUB s0, s1, s2 to a zero", 0x3FC00000u, 0x3FC00000u, 0, 0, 0, 0x10},
        {0x1E220820u, "FMUL s0, s1, s2 of a denormal and a zero", 0x00000001u, 0, 0, 1, 0, 0x90},
        {0x1E620820u, "FMUL d0, d1, d2", 0x3FF8000000000000ull, 0x4002000000000000ull, 0, 0, 0x400B000000000000ull,
         0x10},
        {0x1F020C20u, "FMADD s0, s1, s2, s3", 0x3FC00000u, 0x40100000u, 0x3F800000u, 0, 0x408C0000u, 0x10},
        {0x1E61C020u, "FSQRT d0, d1", 0x4010000000000000ull, 0, 0, 0, 0x4000000000000000ull, 0x10},
        {0x1E222020u, "FCMP s1, s2", 0x3FC00000u, 0x40100000u, 0, 0, 0x8, 0x10},
        {0x1E222020u, "FCMP s1, s2 of a denormal", 0x00000001u, 0x40100000u, 0, 1, 0x8, 0x90},
        {0x1E22C020u, "FCVT d0, s1", 0x3FC00000u, 0, 0, 0, 0x3FF8000000000000ull, 0x10},
    };
    unsigned char* code = snx_test_pages(CODE_MAX);
    size_t failures = 0, k;
    if (code == NULL) {
        fprintf(stderr, "FAIL: FZ's hot paths: no memory for the code\n");
        return 1;
    }
    for (k = 0; k < sizeof cases / sizeof *cases; ++k) {
        CPU c;
        unsigned long calls;
        u64 got;
        int cold, want_cold;
        if (snx_test_pages_exec(code, CODE_MAX, 0) ||
            !snx_x64_step(cases[k].word, 0x1000, TARGET, host_features(), code, CODE_MAX) ||
            snx_test_pages_exec(code, CODE_MAX, 1)) {
            fprintf(stderr, "FAIL: %s %08x: not made\n", cases[k].text, (unsigned)cases[k].word);
            ++failures;
            continue;
        }
        memset(&c, 0, sizeof c);
        c.v[1].d[0] = cases[k].v1;
        c.v[2].d[0] = cases[k].v2;
        c.v[3].d[0] = cases[k].v3;
        c.fpcr = 1u << 24; /* FZ, round to nearest */
        c.fpsr = 0x10;     /* IXC */
        c.pc = 0x1000;
        calls = snx_x64_fp_calls();
        run_step((Step)(void*)code, &c);
        cold = snx_x64_fp_calls() != calls;
        got = cases[k].word == 0x1E222020u ? (u64)(c.n << 3 | c.z << 2 | c.cf << 1 | c.vf) : c.v[0].d[0];
        /* FMADD is the helper's on a host without FMA */
        want_cold = cases[k].cold || (cases[k].word == 0x1F020C20u && !(host_features() & SNX_X64_FMA));
        if (got != cases[k].want || c.fpsr != cases[k].fpsr || cold != want_cold) {
            fprintf(stderr,
                    "FAIL: %s under FZ: %016llx (want %016llx), FPSR %08x (want %08x), %s the helper (want %s)\n",
                    cases[k].text, (unsigned long long)got, (unsigned long long)cases[k].want, c.fpsr, cases[k].fpsr,
                    cold ? "through" : "without", want_cold ? "through" : "without");
            ++failures;
        }
    }
    snx_test_pages_free(code, CODE_MAX);
    printf("sail_x64: FZ's hot paths, %zu cases\n", sizeof cases / sizeof *cases);
    return failures;
}

/* FCMP and FCMPE's flags inline (x64_a64.h's sg_FPCompare): an ordered compare, and FCMP's of a quiet NaN, take no
   helper call; a signalling NaN (FCMP), any NaN (FCMPE) or a denormal FZ flushes take the helper, which raises IOC or
   IDC, and its flags come back by the cold path's POPFQ. NZCV and FPSR as the console gives them, from FPSR clear */
static size_t check_fcmp_paths(void) {
    static const struct {
        u32 word;
        const char* text;
        u64 v1, v2;
        u32 fpcr;
        u32 nzcv, fpsr;
        int cold;
    } cases[] = {
        {0x1E222020u, "FCMP s1, s2 of 1.5 and 2.25", 0x3FC00000u, 0x40100000u, 0, 0x8, 0, 0},
        {0x1E222020u, "FCMP s1, s2 of equals", 0x40100000u, 0x40100000u, 0, 0x6, 0, 0},
        {0x1E222020u, "FCMP s1, s2 of -0 and 0", 0x80000000u, 0, 0, 0x6, 0, 0},
        {0x1E222020u, "FCMP s1, s2 of 2.25 and 1.5", 0x40100000u, 0x3FC00000u, 0, 0x2, 0, 0},
        {0x1E222020u, "FCMP s1, s2 of -inf and inf", 0xFF800000u, 0x7F800000u, 0, 0x8, 0, 0},
        {0x1E222020u, "FCMP s1, s2 of a quiet NaN", 0x7FC00000u, 0x3F800000u, 0, 0x3, 0, 0},
        {0x1E222020u, "FCMP s1, s2 of 1 and a quiet NaN", 0x3F800000u, 0xFFC00001u, 0, 0x3, 0, 0},
        {0x1E222020u, "FCMP s1, s2 of a signalling NaN", 0x7FA00000u, 0x3F800000u, 0, 0x3, 0x1, 1},
        {0x1E222020u, "FCMP s1, s2 of 1 and a signalling NaN", 0x3F800000u, 0xFF800001u, 0, 0x3, 0x1, 1},
        {0x1E222030u, "FCMPE s1, s2 of a quiet NaN", 0x7FC00000u, 0x3F800000u, 0, 0x3, 0x1, 1},
        {0x1E222030u, "FCMPE s1, s2 of 1.5 and 2.25", 0x3FC00000u, 0x40100000u, 0, 0x8, 0, 0},
        {0x1E222020u, "FCMP s1, s2 of a denormal and 0", 0x00000001u, 0, 0, 0x2, 0, 0},
        {0x1E222020u, "FCMP s1, s2 of a denormal and 0 under FZ", 0x00000001u, 0, 1u << 24, 0x6, 0x80, 1},
        {0x1E622020u, "FCMP d1, d2 of 1.5 and 2.25", 0x3FF8000000000000ull, 0x4002000000000000ull, 0, 0x8, 0, 0},
        {0x1E622020u, "FCMP d1, d2 of a quiet NaN", 0x3FF8000000000000ull, 0x7FF8000000000000ull, 0, 0x3, 0, 0},
        {0x1E622020u, "FCMP d1, d2 of a signalling NaN", 0x7FF4000000000000ull, 0x3FF8000000000000ull, 0, 0x3, 0x1, 1},
        {0x1E622030u, "FCMPE d1, d2 of a quiet NaN", 0xFFF8000000000001ull, 0x3FF8000000000000ull, 0, 0x3, 0x1, 1},
        {0x1E622020u, "FCMP d1, d2 of denormals under FZ", 0x8000000000000001ull, 1, 1u << 24, 0x6, 0x80, 1},
        {0x1E622020u, "FCMP d1, d2 of 2.25 and 1.5 under FZ", 0x4002000000000000ull, 0x3FF8000000000000ull, 1u << 24,
         0x2, 0, 0},
    };
    unsigned char* code = snx_test_pages(CODE_MAX);
    size_t failures = 0, k;
    if (code == NULL) {
        fprintf(stderr, "FAIL: FCMP's paths: no memory for the code\n");
        return 1;
    }
    for (k = 0; k < sizeof cases / sizeof *cases; ++k) {
        CPU c;
        unsigned long calls;
        u32 nzcv;
        int cold;
        if (snx_test_pages_exec(code, CODE_MAX, 0) ||
            !snx_x64_step(cases[k].word, 0x1000, TARGET, host_features(), code, CODE_MAX) ||
            snx_test_pages_exec(code, CODE_MAX, 1)) {
            fprintf(stderr, "FAIL: %s %08x: not made\n", cases[k].text, (unsigned)cases[k].word);
            ++failures;
            continue;
        }
        memset(&c, 0, sizeof c);
        c.v[1].d[0] = cases[k].v1;
        c.v[2].d[0] = cases[k].v2;
        c.fpcr = cases[k].fpcr;
        c.n = c.z = c.cf = c.vf = 1; /* each written */
        c.pc = 0x1000;
        calls = snx_x64_fp_calls();
        run_step((Step)(void*)code, &c);
        cold = snx_x64_fp_calls() != calls;
        nzcv = (u32)(c.n << 3 | c.z << 2 | c.cf << 1 | c.vf);
        if (nzcv != cases[k].nzcv || c.fpsr != cases[k].fpsr || cold != cases[k].cold) {
            fprintf(stderr, "FAIL: %s: NZCV %x (want %x), FPSR %08x (want %08x), %s the helper (want %s)\n",
                    cases[k].text, nzcv, cases[k].nzcv, c.fpsr, cases[k].fpsr, cold ? "through" : "without",
                    cases[k].cold ? "through" : "without");
            ++failures;
        }
    }
    snx_test_pages_free(code, CODE_MAX);
    printf("sail_x64: FCMP's paths, %zu cases\n", sizeof cases / sizeof *cases);
    return failures;
}

/* the compare helper's flags for the native cold path (desc bit 9): the RFLAGS UCOMISx b, a leaves, of the operands
   as FPCR.FZ flushes them (bits 1 and 9 set), for every pair of these floats, FCMP's and FCMPE's, FZ clear and set */
u64 snx_x64_fp_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
static size_t check_fcmp_image(void) {
    size_t failures = 0;
#if defined(__x86_64__)
    static const u64 k[2][10] = {
        {0, 0x80000000u, 0x3F800000u, 0xBFC00000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7FA00000u, 0x00000001u,
         0x807FFFFFu},
        {0, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF8000000000000ull, 0x7FF0000000000000ull,
         0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x7FF4000000000000ull, 1, 0x800FFFFFFFFFFFFFull},
    };
    for (int dbl = 0; dbl < 2; ++dbl)
        for (int fz = 0; fz < 2; ++fz)
            for (int signal = 0; signal < 2; ++signal)
                for (int i = 0; i < 10; ++i)
                    for (int j = 0; j < 10; ++j) {
                        u64 a = k[dbl][i], b = k[dbl][j], fa = a, fb = b, sign = dbl ? 1ull << 63 : 1ull << 31;
                        u64 exp = dbl ? 0x7FF0000000000000ull : 0x7F800000u;
                        unsigned char cf, zf, pf;
                        CPU c;
                        if (fz && !(a & exp)) fa = a & sign; /* a denormal (or zero) as FZ flushes it */
                        if (fz && !(b & exp)) fb = b & sign;
                        if (dbl) {
                            double x, y;
                            memcpy(&x, &fa, 8);
                            memcpy(&y, &fb, 8);
                            __asm__("ucomisd %3, %4\n\tsetc %0\n\tsetz %1\n\tsetp %2"
                                    : "=q"(cf), "=q"(zf), "=q"(pf)
                                    : "x"(x), "x"(y)
                                    : "cc");
                        } else {
                            u32 ua = (u32)fa, ub = (u32)fb;
                            float x, y;
                            memcpy(&x, &ua, 4);
                            memcpy(&y, &ub, 4);
                            __asm__("ucomiss %3, %4\n\tsetc %0\n\tsetz %1\n\tsetp %2"
                                    : "=q"(cf), "=q"(zf), "=q"(pf)
                                    : "x"(x), "x"(y)
                                    : "cc");
                        }
                        memset(&c, 0, sizeof c);
                        u64 want = 0x202u | cf | (u64)pf << 2 | (u64)zf << 6;
                        u64 got = snx_x64_fp_compare(&c, a, b, fz ? 1u << 24 : 0,
                                                     (dbl ? 64u : 32u) | (u32)signal << 8 | 1u << 9);
                        if (got != want) {
                            fprintf(stderr,
                                    "FAIL: the compare's flags of %016llx and %016llx (%s, FZ %d, %s): %03llx, "
                                    "UCOMIS's %03llx\n",
                                    (unsigned long long)a, (unsigned long long)b, dbl ? "doubles" : "singles", fz,
                                    signal ? "FCMPE" : "FCMP", (unsigned long long)got, (unsigned long long)want);
                            ++failures;
                        }
                    }
    printf("sail_x64: the compare's flags image, 800 pairs\n");
#endif
    return failures;
}

/* ---- the floating-point guards: which path an operation takes, for every pair (or triple) of the operands that tell
   them apart, FPCR's rounding and FZ, and FPSR's IXC ---- */

u64 snx_x64_fp_binary(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_fma(CPU* c, u64 add, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_unary(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_minmax(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_convert(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_round(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_to_int(CPU* c, u64 a, u64 fpcr, u32 desc);

enum { G_ADD, G_SUB, G_MUL, G_DIV, G_SQRT, G_FMA, G_MAX, G_MIN, G_ROUND, G_CONVERT, G_COMPARE, G_TO_INT };
typedef struct {
    const char* text;
    u32 word;
    int op;
    unsigned w, nops;
} GuardCase;

/* a float of w bits (32 or 64) as the host's double, exactly */
static double guard_value(u64 bits, unsigned w) {
    double d;
    float f;
    if (w == 64) {
        memcpy(&d, &bits, sizeof d);
        return d;
    }
    u32 low = (u32)bits;
    memcpy(&f, &low, sizeof f);
    return f;
}
static u64 guard_bits(double v, unsigned w) {
    u64 d;
    u32 f;
    float s = (float)v;
    memcpy(&d, &v, sizeof d);
    memcpy(&f, &s, sizeof f);
    return w == 64 ? d : f;
}
static int guard_nan(u64 v, unsigned w) {
    return w == 64 ? v << 1 > 0xFFE0000000000000ull : (u32)(v << 1) > 0xFF000000u;
}
/* a NaN whose quiet bit is clear */
static int guard_snan(u64 v, unsigned w) { return guard_nan(v, w) && !(v >> (w == 64 ? 51 : 22) & 1); }
static int guard_zero(u64 v, unsigned w) { return !(w == 64 ? v << 1 : (u32)(v << 1)); }
static int guard_denormal(u64 v, unsigned w) {
    u64 m = w == 64 ? v & 0x7FFFFFFFFFFFFFFFull : v & 0x7FFFFFFFu;
    return m && m < (w == 64 ? 0x0010000000000000ull : 0x00800000ull);
}
/* a result the host's arithmetic gives as Arm's without a flag but IXC: normal, above the smallest */
static int guard_roomy(u64 v, unsigned w) {
    u64 m = w == 64 ? v & 0x7FFFFFFFFFFFFFFFull : v & 0x7FFFFFFFu;
    return w == 64 ? m > 0x0010000000000000ull && m < 0x7FF0000000000000ull : m > 0x00800000u && m < 0x7F800000u;
}
/* the host's result, in the operation's own precision */
static u64 guard_host(const GuardCase* g, const u64* v) {
    if (g->w == 64) {
        double a = guard_value(v[0], 64), b = guard_value(v[1], 64), c = guard_value(v[2], 64);
        return guard_bits(g->op == G_ADD    ? a + b
                          : g->op == G_SUB  ? a - b
                          : g->op == G_MUL  ? a * b
                          : g->op == G_DIV  ? a / b
                          : g->op == G_SQRT ? sqrt(a)
                                            : fma(a, b, c),
                          64);
    }
    float a = (float)guard_value(v[0], 32), b = (float)guard_value(v[1], 32), c = (float)guard_value(v[2], 32);
    return guard_bits(g->op == G_ADD    ? a + b
                      : g->op == G_SUB  ? a - b
                      : g->op == G_MUL  ? a * b
                      : g->op == G_DIV  ? a / b
                      : g->op == G_SQRT ? sqrtf(a)
                                        : fmaf(a, b, c),
                      32);
}
/* whether the operation may take the inline code alone (v in the operands' order: FMADD's n, m, a) */
static int guard_hot(const GuardCase* g, const u64* v, u32 fpcr, u32 fpsr) {
    unsigned k, w = g->w;
    int nan = 0, snan = 0, flushed = 0, rounds = (fpcr >> 22 & 3) == 0, ixc = fpsr >> 4 & 1;
    for (k = 0; k < g->nops; ++k) {
        nan |= guard_nan(v[k], w);
        snan |= guard_snan(v[k], w);
        flushed |= (fpcr >> 24 & 1) && guard_denormal(v[k], w);
    }
    if (flushed) return 0;
    switch (g->op) {
        case G_MAX:
        case G_MIN:
        case G_ROUND:
        case G_CONVERT: return !nan;
        case G_COMPARE: return !snan; /* FCMP's: a quiet NaN's flags (unordered) are the inline code's too */
        case G_TO_INT: {
            double d = guard_value(v[0], w);
            return ixc && !nan && d > -2147483649.0 && d < 2147483648.0;
        }
        default: break;
    }
    if (g->op == G_FMA && !(host_features() & SNX_X64_FMA)) return 0;
    u64 r = guard_host(g, v);
    if (!rounds) return 0;
    if (guard_zero(r, w))
        return g->op == G_MUL || g->op == G_FMA    ? guard_zero(v[0], w) || guard_zero(v[1], w)
               : g->op == G_DIV || g->op == G_SQRT ? guard_zero(v[0], w)
                                                   : 1;
    return ixc && guard_roomy(r, w);
}
/* cpu.h's helper's result, which both paths' is */
static u64 guard_exact(const GuardCase* g, CPU* c, const u64* v, u32 fpcr) {
    u32 w = g->w;
    switch (g->op) {
        case G_ADD:
        case G_SUB:
        case G_MUL:
        case G_DIV:
            return snx_x64_fp_binary(c, v[0], v[1], fpcr,
                                     w | (u32)(g->op == G_ADD   ? SNX_FP_ADD
                                               : g->op == G_SUB ? SNX_FP_SUB
                                               : g->op == G_MUL ? SNX_FP_MUL
                                                                : SNX_FP_DIV)
                                             << 8);
        case G_SQRT: return snx_x64_fp_unary(c, v[0], fpcr, w | (u32)SNX_FP_SQRT << 8);
        case G_FMA: return snx_x64_fp_fma(c, v[2], v[0], v[1], fpcr, w);
        case G_MAX:
        case G_MIN: return snx_x64_fp_minmax(c, v[0], v[1], fpcr, w | (u32)(g->op == G_MIN) << 8);
        case G_ROUND: return snx_x64_fp_round(c, v[0], fpcr, w | 3u << 8); /* FRINTM: 2, NEGINF, and 1 */
        case G_CONVERT: return snx_x64_fp_convert(c, v[0], fpcr, w | 64u << 8);
        case G_COMPARE: return snx_x64_fp_compare(c, v[0], v[1], fpcr, w);
        default: return (u32)snx_x64_fp_to_int(c, v[0], fpcr, w | 32u << 8 | 4u << 24); /* FCVTZS: 3, ZERO, and 1 */
    }
}
static size_t check_fp_guards(void) {
    static const GuardCase cases[] = {
        {"FADD s0, s1, s2", 0x1E222820u, G_ADD, 32, 2},      {"FADD d0, d1, d2", 0x1E622820u, G_ADD, 64, 2},
        {"FSUB s0, s1, s2", 0x1E223820u, G_SUB, 32, 2},      {"FSUB d0, d1, d2", 0x1E623820u, G_SUB, 64, 2},
        {"FMUL s0, s1, s2", 0x1E220820u, G_MUL, 32, 2},      {"FMUL d0, d1, d2", 0x1E620820u, G_MUL, 64, 2},
        {"FMUL s0, s1, s1", 0x1E210820u, G_MUL, 32, 1},      {"FDIV s0, s1, s2", 0x1E221820u, G_DIV, 32, 2},
        {"FDIV d0, d1, d2", 0x1E621820u, G_DIV, 64, 2},      {"FSQRT s0, s1", 0x1E21C020u, G_SQRT, 32, 1},
        {"FSQRT d0, d1", 0x1E61C020u, G_SQRT, 64, 1},        {"FMADD s0, s1, s2, s3", 0x1F020C20u, G_FMA, 32, 3},
        {"FMADD d0, d1, d2, d3", 0x1F420C20u, G_FMA, 64, 3}, {"FMAX s0, s1, s2", 0x1E224820u, G_MAX, 32, 2},
        {"FMIN d0, d1, d2", 0x1E625820u, G_MIN, 64, 2},      {"FRINTM s0, s1", 0x1E254020u, G_ROUND, 32, 1},
        {"FRINTM d0, d1", 0x1E654020u, G_ROUND, 64, 1},      {"FCVT d0, s1", 0x1E22C020u, G_CONVERT, 32, 1},
        {"FCMP s1, s2", 0x1E222020u, G_COMPARE, 32, 2},      {"FCMP d1, d2", 0x1E622020u, G_COMPARE, 64, 2},
        {"FCVTZS w0, s1", 0x1E380020u, G_TO_INT, 32, 1},     {"FCVTZS w0, d1", 0x1E780020u, G_TO_INT, 64, 1},
    };
    /* each side of what the guards compare: zeros, the least and largest denormal, the smallest normal and the next,
       numbers whose product is near it, one exact and one not, 2^31 and what is under it, the largest, infinity, NaNs */
    static const u32 singles[] = {0,           0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00800000u,
                                  0x80800001u, 0x00800002u, 0x3F800000u, 0xBFC00000u, 0x40490FDBu,
                                  0x5F000000u, 0x1F800001u, 0xCF000000u, 0x4F000000u, 0x7F7FFFFFu,
                                  0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u};
    static const u64 doubles[] = {0,
                                  0x8000000000000000ull,
                                  0x0000000000000001ull,
                                  0x800FFFFFFFFFFFFFull,
                                  0x0010000000000000ull,
                                  0x8010000000000001ull,
                                  0x0010000000000002ull,
                                  0x3FF0000000000000ull,
                                  0xBFF8000000000000ull,
                                  0x400921FB54442D18ull,
                                  0x5FE0000000000000ull,
                                  0x1FF0000000000001ull,
                                  0xC1E0000000000000ull,
                                  0x41E0000000000000ull,
                                  0x7FEFFFFFFFFFFFFFull,
                                  0x7FF0000000000000ull,
                                  0xFFF0000000000000ull,
                                  0x7FF8000000000000ull,
                                  0x7FF0000000000001ull};
    enum { VALUES = sizeof singles / sizeof *singles };
    static const u32 fpcrs[] = {0, 1u << 24, 1u << 22, 3u << 22 | 1u << 24};
    unsigned char* code = snx_test_pages(CODE_MAX);
    size_t failures = 0, runs = 0, hot = 0;
    if (code == NULL) {
        fprintf(stderr, "FAIL: the floating-point guards: no memory for the code\n");
        return 1;
    }
    for (size_t k = 0; k < sizeof cases / sizeof *cases; ++k) {
        const GuardCase* g = &cases[k];
        size_t total = g->nops == 3 ? VALUES * VALUES * VALUES : g->nops == 2 ? VALUES * VALUES : VALUES;
        if (snx_test_pages_exec(code, CODE_MAX, 0) ||
            !snx_x64_step(g->word, 0x1000, TARGET, host_features(), code, CODE_MAX) ||
            snx_test_pages_exec(code, CODE_MAX, 1)) {
            fprintf(stderr, "FAIL: %s %08x: not made\n", g->text, (unsigned)g->word);
            ++failures;
            continue;
        }
        for (size_t at = 0; at < total * sizeof fpcrs / sizeof *fpcrs * 2; ++at) {
            u32 fpcr = fpcrs[at / total % (sizeof fpcrs / sizeof *fpcrs)];
            u32 fpsr = at / total / (sizeof fpcrs / sizeof *fpcrs) ? 0x10 : 0;
            size_t pick = at % total;
            CPU c = {0}, exact;
            u64 v[3] = {0}, got, want;
            unsigned long calls;
            for (unsigned o = 0; o < g->nops; ++o, pick /= VALUES)
                v[o] = g->w == 64 ? doubles[pick % VALUES] : singles[pick % VALUES];
            if (g->word == 0x1E210820u) v[1] = v[0];
            for (unsigned o = 0; o < 3; ++o) c.v[1 + o].d[0] = v[o], c.v[1 + o].d[1] = ~v[o];
            if (g->w == 32)
                for (unsigned o = 0; o < 3; ++o)
                    c.v[1 + o].d[0] |= 0xA5A5A5A500000000ull; /* S's upper bits are not read */
            c.fpcr = fpcr;
            c.fpsr = fpsr;
            c.pc = 0x1000;
            exact = c;
            want = guard_exact(g, &exact, v, fpcr);
            calls = snx_x64_fp_calls();
            run_step((Step)(void*)code, &c);
            calls = snx_x64_fp_calls() - calls;
            got = g->op == G_COMPARE  ? (u64)(c.n << 3 | c.z << 2 | c.cf << 1 | c.vf)
                  : g->op == G_TO_INT ? c.x[0]
                                      : c.v[0].d[0];
            ++runs;
            hot += !calls;
            int want_hot = guard_hot(g, v, fpcr, fpsr);
            if (got != want || c.fpsr != exact.fpsr || c.v[0].d[1] || calls != (unsigned long)!want_hot) {
                if (++failures <= 12)
                    fprintf(
                        stderr,
                        "FAIL: %s of %llx %llx %llx, FPCR %08x FPSR %02x: %016llx:%016llx (want %016llx), FPSR %08x "
                        "(want %08x), %lu calls of the helper (want %d)\n",
                        g->text, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2], fpcr,
                        fpsr, (unsigned long long)c.v[0].d[1], (unsigned long long)got, (unsigned long long)want,
                        c.fpsr, exact.fpsr, calls, !want_hot);
            }
        }
    }
    /* each condition is a jump of its own: four operations in a row make none of them 0 or 1 (SETcc, 0F 90 to 0F 9F) */
    static const struct {
        const char* text;
        u32 words[4];
    } rows[] = {{"FMUL s", {0x1E220820u, 0x1E250883u, 0x1E2808E6u, 0x1E2B0949u}},
                {"FADD d", {0x1E622820u, 0x1E652883u, 0x1E6828E6u, 0x1E6B2949u}},
                {"FMADD s", {0x1F020C20u, 0x1F061CA4u, 0x1F0A2D28u, 0x1F0E3DACu}},
                {"FSQRT d", {0x1E61C020u, 0x1E61C062u, 0x1E61C0A4u, 0x1E61C0E6u}},
                {"FMAX s", {0x1E224820u, 0x1E254883u, 0x1E2848E6u, 0x1E2B4949u}},
                {"FRINTM d", {0x1E654020u, 0x1E654062u, 0x1E6540A4u, 0x1E6540E6u}}};
    for (size_t k = 0; k < sizeof rows / sizeof *rows; ++k) {
        size_t count = 4, size = 0, set = 0;
        if (!snx_test_pages_exec(code, CODE_MAX, 0))
            size = snx_x64_block(rows[k].words, &count, 0x1000, TARGET, host_features(), code, CODE_MAX);
        for (size_t at = 0; at + 1 < size; ++at) set += code[at] == 0x0F && code[at + 1] >> 4 == 9;
        if (!size || count != 4 || set) {
            fprintf(stderr, "FAIL: four %s: %zu bytes of %zu words (want 4), %zu SETcc (want none)\n", rows[k].text,
                    size, count, set);
            ++failures;
        }
    }
    snx_test_pages_free(code, CODE_MAX);
    printf("sail_x64: the floating-point guards, %zu runs of %zu words, %zu on the inline code alone: %zu failures\n",
           runs, sizeof cases / sizeof *cases, hot, failures);
    return failures;
}

/* guarded operations in a row, where one's limits are another's, the registers they need are all taken, an atomic
   between them ends what lived in registers and a guest's flags live on the host's: the block's state after it is its
   words' one by one. With every register, with three, with six vector ones (a guard's value in a spill slot) and
   with three and six (a general-purpose value has no vector register to spill to) */
static size_t check_fp_rows(void) {
    enum { WORDS = 16, STATES = 400 };
    static const struct {
        const char* text;
        u32 words[WORDS];
    } rows[] = {
        {"four limits alive",
         {0x1E220820u, 0x1E650883u, 0x1F021C26u, 0x1F452488u, 0x1E2028CAu, 0x1E63390Bu, 0x1E202140u, 0x1E780161u}},
        {"an atomic between",
         {0x1E220820u, 0xB82600A7u, 0x1F021C26u, 0xB82600A7u, 0x1E61C062u, 0x1E221820u, 0x1E202008u}},
        {"the guest's flags across",
         {0x1E220820u, 0xEB090107u, 0x1E250883u, 0x9A8CB16Au, 0x1E6F29CDu, 0x9A8C816Du, 0x1E2E2000u, 0x9A8CA16Eu}},
        {"guest values taking every register",
         {0x8B030041u, 0x8B060024u, 0x8B010087u, 0x1E27C020u, 0x8B0400E8u, 0x8B010109u, 0x1E67C043u, 0x8B07012Au,
          0x1E38000Bu, 0x8B08014Cu, 0x1E274085u}},
        {"more vector values than a small pool",
         {0x1E220820u, 0x1E650883u, 0x1E6808E7u, 0x1E2B090Au, 0x1F021C26u, 0x1E223843u, 0x1E6A48C9u, 0x1E224841u,
          0x1E6828C5u, 0x1E202928u, 0x1E28082Bu, 0x1E2A6821u}},
        {"a double's low half rounded where it is spilled",
         {0x1E610810u, 0x1E630851u, 0x1E250892u, 0x1E2708D3u, 0x1E290914u, 0x1E2B0955u, 0x1E2D0996u, 0x1E254218u,
          0x1E274239u, 0x1E332A5Au, 0x1E352A9Bu, 0x1E302ADCu, 0x1E322A3Du, 0x1E25427Eu}},
        {"each kind", {0x1E222020u, 0x1E224823u, 0x1E380020u, 0x1E6540A4u, 0x1E22C066u, 0x1E21C067u, 0x1E665888u}},
    };
    static const u64 values[] = {0,
                                 0x8000000080000000ull,
                                 0x0000000000000001ull,
                                 0x807FFFFF00000001ull,
                                 0x0080000000800000ull,
                                 0x3F8000003FC00000ull,
                                 0x404900003F800000ull,
                                 0x3FF8000000000000ull,
                                 0xC00921FB54442D18ull,
                                 0x7F80000000000000ull,
                                 0x7FC000007FC00001ull,
                                 0x7F7FFFFF7F7FFFFFull,
                                 0x001FFFFF00000000ull,
                                 0x000000000080000Aull};
    static const u32 fpcrs[] = {0, 1u << 24, 1u << 22};
    unsigned char* code = snx_test_pages((WORDS + 1) * (size_t)CODE_MAX);
    RtAPI rt = {.undef = on_undef, .sev = on_sev, .wfe = on_wfe, .cas128 = snx_runtime_cas128};
    unsigned char* at = g_test.memory[0] + MEMORY_ZERO + 0x1000;
    u64 seed = 0x9E3779B97F4A7C15ull;
    size_t failures = 0, runs = 0;
    int pool = snx_x64_pool(), xpool = snx_x64_xpool();
    if (code == NULL) {
        fprintf(stderr, "FAIL: guarded operations in a row: no memory for the code\n");
        return 1;
    }
    for (size_t k = 0; k < 4 * (sizeof rows / sizeof *rows); ++k) {
        size_t n = 0, count, made;
        const char* text = rows[k / 4].text;
        snx_x64_set_pool(k & 1 ? FEW_REGISTERS : pool);
        snx_x64_set_xpool(k & 2 ? FEW_XMM : xpool);
        while (n < WORDS && rows[k / 4].words[n]) ++n;
        count = n;
        if (snx_test_pages_exec(code, (WORDS + 1) * (size_t)CODE_MAX, 0)) return 1;
        made = snx_x64_block(rows[k / 4].words, &count, 0x1000, TARGET, host_features(), code, CODE_MAX) != 0;
        for (size_t w = 0; w < n; ++w)
            made += snx_x64_step(rows[k / 4].words[w], 0x1000 + 4 * w, TARGET, host_features(),
                                 code + (w + 1) * CODE_MAX, CODE_MAX) != 0;
        if (made != n + 1 || count != n || snx_test_pages_exec(code, (WORDS + 1) * (size_t)CODE_MAX, 1)) {
            fprintf(stderr, "FAIL: %s: %zu of %zu made, the block of %zu words\n", text, made, n + 1, count);
            ++failures;
            continue;
        }
        for (size_t state = 0; state < STATES; ++state) {
            CPU c[2] = {{0}, {0}};
            u64 memory[2];
            for (unsigned r = 0; r < 16; ++r)
                for (unsigned h = 0; h < 2; ++h) {
                    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                    c[0].v[r].d[h] = values[(seed >> 33) % (sizeof values / sizeof *values)];
                }
            for (unsigned r = 0; r < 31; ++r) {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                c[0].x[r] = seed >> 7;
            }
            c[0].x[5] = 0x1000;
            c[0].n = seed >> 40 & 1, c[0].z = seed >> 41 & 1, c[0].cf = seed >> 42 & 1, c[0].vf = seed >> 43 & 1;
            c[0].fpcr = fpcrs[(seed >> 44) % 3];
            c[0].fpsr = seed >> 47 & 1 ? 0x10 : 0;
            c[0].mem = g_test.memory[0] + MEMORY_ZERO;
            c[0].rt = &rt;
            c[0].pc = 0x1000;
            c[1] = c[0];
            memset(at, 0x5A, 8);
            run_step((Step)(void*)code, &c[0]);
            memcpy(&memory[0], at, 8);
            memset(at, 0x5A, 8);
            for (size_t w = 0; w < n; ++w) run_step((Step)(void*)(code + (w + 1) * CODE_MAX), &c[1]);
            memcpy(&memory[1], at, 8);
            ++runs;
            if (memcmp(c[0].x, c[1].x, sizeof c[0].x) || memcmp(c[0].v, c[1].v, sizeof c[0].v) || c[0].n != c[1].n ||
                c[0].z != c[1].z || c[0].cf != c[1].cf || c[0].vf != c[1].vf || c[0].fpsr != c[1].fpsr ||
                memory[0] != memory[1]) {
                if (++failures <= 8)
                    fprintf(stderr,
                            "FAIL: %s, state %zu: the block differs from its words (FPCR %08x FPSR %02x: %02x, %02x)\n",
                            text, state, c[0].fpcr, (unsigned)(seed >> 47 & 1 ? 0x10 : 0), c[0].fpsr, c[1].fpsr);
            }
        }
    }
    snx_x64_set_pool(pool);
    snx_x64_set_xpool(xpool);
    snx_test_pages_free(code, (WORDS + 1) * (size_t)CODE_MAX);
    printf("sail_x64: guarded operations in a row, %zu runs: %zu failures\n", runs, failures);
    return failures;
}

#define USAGE                                                                                                           \
    "usage: snx-test-sail-x64 (--qemu QEMU RUNNER DIR [--record FILE] | --reference[-if-there] FILE | --skip REASON)\n" \
    "                         [--frozen FILE] [--states N] [--samples N] [--sequences N] [--pool N] [--xmm N]\n"        \
    "                         [--verbose]\n"
/* whether path is there to read */
static int there(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}
/* every feature the back end has a form for: a reference's words are those made with them too, beside this host's and
   the plain one's, so that a host with features this one lacks finds its words in it */
#define SNX_X64_EVERY                                                                                                  \
    (SNX_X64_BMI1 | SNX_X64_BMI2 | SNX_X64_LZCNT | SNX_X64_SSE3 | SNX_X64_SSSE3 | SNX_X64_SSE41 | SNX_X64_SSE42 |      \
     SNX_X64_PCLMUL | SNX_X64_F16C | SNX_X64_POPCNT | SNX_X64_AVX | SNX_X64_FMA | SNX_X64_ABI)
#define NONE ((size_t)-1)

/* a word: its code on this host and the plain one, at SAIL_PC, and a store-exclusive's after its load-exclusive */
typedef struct {
    u32 word;
    size_t code[2];    /* NONE where not made */
    size_t pair[2][2]; /* the load-exclusive at SAIL_PAIR_PC, the word at SAIL_PAIR_PC + 4 */
} Word;

/* what run_native reaches: the words, their code */
typedef struct {
    const Word* words;
    const unsigned char* code;
} Made;
/* word's code at pc for host on code's next bytes: its offset, NONE where the back end makes none; *failed where it
   takes the word but can't make it */
static size_t make(u32 word, u64 pc, unsigned host, unsigned char* code, size_t* at, size_t cap, int* failed) {
    size_t got = 0, where = *at;
    g_test.jumping = 1;
    if (sigsetjmp(g_test.jump, 1))
        *failed = 1;
    else
        got = snx_x64_step(word, pc, TARGET, host, code + where, cap - where);
    g_test.jumping = 0;
    if (*failed || !got) return NONE;
    *at += (got + 15) & ~(size_t)15;
    return where;
}

static int has_code(void* user, size_t i, unsigned form) { return ((const Made*)user)->words[i].code[form] != NONE; }
/* the native end of word i's code of form in state s from start, its memory in work */
static void run_native(void* user, size_t i, unsigned form, unsigned s, const CPU* start, u8* work, const u8* initial,
                       SailEnd* e) {
    const Word* w = &((const Made*)user)->words[i];
    const unsigned char* code = ((const Made*)user)->code;
    CPU c = *start;
    u64 next, pc = SAIL_PC;
    const int pair = start->pc == SAIL_PAIR_PC;
    (void)s;
    sail_window_start(work, initial, w->word, pair);
    c.mem = work - SAIL_WINDOW;
    memset(&g_sail_calls, 0, sizeof g_sail_calls);
    if (pair) {
        (void)run_step((Step)(void*)(code + w->pair[form][0]), &c);
        pc = SAIL_PAIR_PC + 4;
        next = g_sail_calls.undef ? pc : run_step((Step)(void*)(code + w->pair[form][1]), &c);
    } else
        next = run_step((Step)(void*)(code + w->code[form]), &c);
    sail_native_end(e, &c, next, pc, work, initial);
}

static int run(int argc, char** argv) {
    unsigned states = SAIL_STATES_MAX, samples = 3, sequences = 4000, i, k;
    int states_given = 0;
    unsigned hosts[2];
    u32* rows;
    Word* words;
    size_t count, n = 0, failures = 0, unmade = 0, compared = 0, made = 0, plain_unmade = 0;
    unsigned char* code;
    size_t code_at = 0, code_cap = (size_t)1 << 27;
    static const char* const forms[2] = {"this", "a plain"};
    SailHold hold = {0};
    SailRef ref = {0};
    int reference_kind = 0; /* 1: --reference, 2: --reference-if-there */
    char error[1024];
    const char *reference = NULL, *frozen = NULL;
    u32* list;
    Made made_code;
    RtAPI rt = {.cas128 = snx_runtime_cas128};
#if !defined(_WIN32)
    struct sigaction sa;
#endif
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--states") && i + 1 < (unsigned)argc)
            states = (unsigned)atoi(argv[++i]), states_given = 1;
        else if (!strcmp(argv[i], "--samples") && i + 1 < (unsigned)argc)
            samples = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pool") && i + 1 < (unsigned)argc)
            snx_x64_set_pool(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--xmm") && i + 1 < (unsigned)argc)
            snx_x64_set_xpool(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--sequences") && i + 1 < (unsigned)argc)
            sequences = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--qemu") && i + 3 < (unsigned)argc) {
            hold.qemu = argv + i + 1;
            i += 3;
        } else if ((!strcmp(argv[i], "--reference") || !strcmp(argv[i], "--reference-if-there")) &&
                   i + 1 < (unsigned)argc) {
            reference_kind = argv[i][11] ? 2 : 1;
            reference = argv[++i];
        } else if (!strcmp(argv[i], "--record") && i + 1 < (unsigned)argc)
            hold.record = argv[++i];
        else if (!strcmp(argv[i], "--frozen") && i + 1 < (unsigned)argc)
            frozen = argv[++i];
        else if (!strcmp(argv[i], "--skip") && i + 1 < (unsigned)argc) {
            printf("sail_x64 skipped: %s\n", argv[i + 1]);
            return 77;
        } else if (!strcmp(argv[i], "--verbose"))
            g_test.verbose = 1;
        else if (!strcmp(argv[i], "--trace"))
            g_test.verbose = 2; /* each word named before it runs: a crash's */
        else if (!strcmp(argv[i], "--dump") && i + 1 < (unsigned)argc) {
            /* the words' x86-64 code as one block on this host's features, as hex bytes (for a disassembler) */
            unsigned char buf[CODE_MAX];
            u32 ws[16];
            size_t nw = 0, got, b;
            while (i + 1 < (unsigned)argc && nw < 16) ws[nw++] = (u32)strtoul(argv[++i], NULL, 16);
            got = snx_x64_block(ws, &nw, 0x1000, TARGET, host_features(), buf, sizeof buf);
            for (b = 0; b < got; ++b) printf("0x%02x%c", buf[b], b + 1 < got ? ' ' : '\n');
            if (!got) printf("(not made)\n");
            return 0;
        } else {
            fprintf(stderr, "snx-test-sail-x64: '%s' is not an option\n" USAGE, argv[i]);
            return 2;
        }
    }
    if (!hold.qemu == !reference_kind) {
        fprintf(stderr,
                "snx-test-sail-x64: the words are held to QEMU (--qemu) or to a reference of it (--reference): "
                "%s\n" USAGE,
                hold.qemu ? "both given" : "neither given");
        return 2;
    }
    if (states > SAIL_STATES_MAX || (hold.record && states_given && states != SAIL_STATES_MAX)) {
        fprintf(stderr, "snx-test-sail-x64: --states %u: a word has %u at most%s\n", states, SAIL_STATES_MAX,
                hold.record ? ", and --record takes every one" : "");
        return 2;
    }
    if (hold.record && !hold.qemu) {
        fprintf(stderr, "snx-test-sail-x64: --record %s needs --qemu: it writes QEMU's ends\n", hold.record);
        return 2;
    }
#if !defined(__x86_64__)
    printf("not an x86-64 host: skipped\n");
    return 0;
#endif
    if (reference_kind && sail_ref_read(reference, &ref, error, sizeof error)) {
        if (reference_kind == 2 && !there(reference)) {
            printf("sail_x64 skipped: %s\n", error);
            return 77;
        }
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    sail_set_target(TARGET);
    g_test.seed = 0x9e3779b97f4a7c15ull;
    hosts[0] = host_features();
    hosts[1] = SNX_X64_BASELINE; /* no BMI1, BMI2, LZCNT, AVX, FMA or F16C */
#if defined(_WIN32)
    signal(SIGABRT, on_abort);
#else
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_abort;
    sigaction(SIGABRT, &sa, NULL);
#endif
    for (k = 0; k < 3; ++k) {
        g_test.memory[k] = snx_test_pages(MEMORY_SIZE);
        if (g_test.memory[k] == NULL) return 1;
    }
    failures += check_fz_hot();
    failures += check_fcmp_paths();
    failures += check_fcmp_image();
    failures += check_fp_guards();
    failures += check_fp_rows();
    failures += check_casp();
    for (i = 0; i < MEMORY_SIZE; ++i) g_test.memory[2][i] = (unsigned char)rnd();
    memcpy(g_test.memory[0], g_test.memory[2], MEMORY_SIZE);
    memcpy(g_test.memory[1], g_test.memory[2], MEMORY_SIZE);
    code = snx_test_pages(code_cap);
    if (code == NULL) {
        fprintf(stderr, "FAIL: no memory for the code\n");
        return 1;
    }
    /* the words: each row's samples, and each with Rd, Rn and Rm 31 */
    count = sail_candidates(samples, &g_test.rows);
    rows = g_test.rows;
    words = g_test.words = (Word*)malloc(sizeof *words * (count + 1));
    if (!words) return 1;
    for (i = 0; i < count; ++i) {
        {
            const u32 w = rows[i];
            const char* name = op_name(w);
            Word* x = &words[n];
            int failed = 0, form;
            size_t every = NONE;
            /* x86-64 code for both hosts; a word neither makes is not the x86-64 back end's */
            x->word = w;
            for (form = 0; form < 2 && !failed; ++form) {
                x->code[form] = make(w, SAIL_PC, hosts[form], code, &code_at, code_cap, &failed);
                x->pair[form][0] = x->pair[form][1] = NONE;
                if (x->code[form] != NONE && sail_pair(w, 1)) {
                    x->pair[form][0] =
                        make(sail_pair(w, 1), SAIL_PAIR_PC, hosts[form], code, &code_at, code_cap, &failed);
                    x->pair[form][1] = make(w, SAIL_PAIR_PC + 4, hosts[form], code, &code_at, code_cap, &failed);
                    if (x->pair[form][0] == NONE || x->pair[form][1] == NONE) x->code[form] = NONE;
                }
            }
            if (!failed && hold.record && x->code[0] == NONE && x->code[1] == NONE) {
                size_t at = code_at;
                every = make(w, SAIL_PC, SNX_X64_EVERY, code, &code_at, code_cap, &failed);
                code_at = at; /* only whether a host of every feature makes it */
            }
            if (failed) {
                ++unmade;
                ++failures;
                fprintf(stderr, "FAIL: %s %08x: the x86-64 back end takes it but can't make it\n", name, (unsigned)w);
                continue;
            }
            if (x->code[0] == NONE && x->code[1] == NONE && every == NONE) continue;
            if (x->code[0] != NONE) {
                ++made;
                if (x->code[1] == NONE) ++plain_unmade; /* a primitive no form without BMI1, BMI2 and LZCNT makes */
            }
            ++n;
        }
    }
    if (snx_test_pages_exec(code, code_cap, 1)) {
        fprintf(stderr, "FAIL: the code can't be made executable\n");
        return 1;
    }
    list = (u32*)malloc(sizeof *list * (n ? n : 1));
    hold.work = (u8*)malloc(SAIL_WINDOW_SIZE);
    if (!list || !hold.work) {
        free(list), free(hold.work);
        return 1;
    }
    for (i = 0; i < n; ++i) list[i] = words[i].word;
    made_code.words = words;
    made_code.code = code;
    hold.ref = reference_kind ? &ref : NULL;
    hold.name = "sail_x64";
    hold.forms = forms;
    hold.nforms = 2;
    hold.verbose = g_test.verbose;
    hold.user = &made_code;
    hold.has = has_code;
    hold.run = run_native;
    failures += sail_hold(&hold, list, n, states, &compared);
    if (frozen) failures += sail_frozen(&hold, frozen, list, n, &compared);
    sail_ref_free(&ref);
    free(list), free(hold.work);
    /* sequences: words in a row as one block, against the code of each word alone at its pc, one after another */
    sail_rt(&rt);
    if (n && sequences) {
        typedef struct {
            u32 w[4];
            size_t count, code, alone[4];
        } Seq;
        Seq* seq = (Seq*)malloc(sizeof(Seq) * sequences);
        size_t q, nseq = 0;
        if (!seq) return 1;
        g_test.sequences = seq;
        if (snx_test_pages_exec(code, code_cap, 0)) return 1;
        for (q = 0; q < sequences; ++q) {
            Seq* sq = &seq[nseq];
            size_t len = 2 + rnd() % 3, got, j;
            int failed = 0;
            for (j = 0; j < len; ++j) {
                const Word* w;
                do w = &words[rnd() % n];
                while (w->code[0] == NONE || sail_is_ldst(w->word)); /* their registers would point anywhere after
                                                                        another word */
                sq->w[j] = w->word;
            }
            sq->count = len;
            g_test.jumping = 1;
            if (sigsetjmp(g_test.jump, 1)) {
                g_test.jumping = 0;
                ++failures;
                fprintf(stderr, "FAIL: the block of");
                for (j = 0; j < len; ++j) fprintf(stderr, " %08x", (unsigned)sq->w[j]);
                fprintf(stderr, " can't be made\n");
                continue;
            }
            got = snx_x64_block(sq->w, &sq->count, SAIL_PC, TARGET, hosts[0], code + code_at, code_cap - code_at);
            g_test.jumping = 0;
            if (!got) continue;
            sq->code = code_at;
            code_at += (got + 15) & ~(size_t)15;
            for (j = 0; j < sq->count && !failed; ++j)
                sq->alone[j] = make(sq->w[j], SAIL_PC + 4 * j, hosts[0], code, &code_at, code_cap, &failed);
            for (j = 0; j < sq->count; ++j) failed |= sq->alone[j] == NONE;
            if (failed) {
                ++failures;
                fprintf(stderr, "FAIL: a word of the block of %08x... can't be made alone at its pc\n",
                        (unsigned)sq->w[0]);
                continue;
            }
            ++nseq;
        }
        if (snx_test_pages_exec(code, code_cap, 1)) return 1;
        for (q = 0; q < nseq; ++q) {
            Step block = (Step)(void*)(code + seq[q].code);
            unsigned st;
            for (st = 0; st < states; ++st) {
                CPU start, end[2];
                u64 next[2] = {0, 0};
                unsigned calls[2], r;
                size_t j;
                memset(&start, 0, sizeof start);
                for (r = 0; r < 32; ++r) start.x[r] = rnd();
                fp_state(&start, st);
                if (st & 1) start.x[rnd() & 31] = 0;
                if (st & 2) start.x[rnd() & 31] = ~0ull;
                start.n = rnd() & 1;
                start.z = rnd() & 1;
                start.cf = rnd() & 1;
                start.vf = rnd() & 1;
                start.rt = &rt;
                start.pc = SAIL_PC;
                start.mem = g_test.memory[0] + MEMORY_ZERO;
                end[0] = start;
                end[1] = start;
                memset(&g_sail_calls, 0, sizeof g_sail_calls);
                for (j = 0; j < seq[q].count && !g_sail_calls.undef; ++j)
                    next[0] = run_step((Step)(void*)(code + seq[q].alone[j]), &end[0]);
                calls[0] = g_sail_calls.undef + 16 * g_sail_calls.sev + 256 * g_sail_calls.wfe;
                memset(&g_sail_calls, 0, sizeof g_sail_calls);
                next[1] = run_step(block, &end[1]);
                calls[1] = g_sail_calls.undef + 16 * g_sail_calls.sev + 256 * g_sail_calls.wfe;
                ++compared;
                if ((!calls[0] && next[0] != next[1]) || calls[0] != calls[1] ||
                    memcmp(end[0].x, end[1].x, sizeof end[0].x) || end[0].n != end[1].n || end[0].z != end[1].z ||
                    end[0].cf != end[1].cf || end[0].vf != end[1].vf || fp_differs(&end[0], &end[1])) {
                    ++failures;
                    fprintf(stderr, "FAIL: the block of");
                    for (j = 0; j < seq[q].count; ++j)
                        fprintf(stderr, " %s %08x", op_name(seq[q].w[j]), (unsigned)seq[q].w[j]);
                    fprintf(stderr, " state %u: its end is not the words' one after another\n", st);
                    if (g_test.verbose) {
                        for (r = 0; r < 32; ++r)
                            if (end[0].x[r] != end[1].x[r])
                                fprintf(stderr, "  x%u: words %016llx block %016llx\n", r,
                                        (unsigned long long)end[0].x[r], (unsigned long long)end[1].x[r]);
                        fprintf(stderr,
                                "  nzcv: words %u%u%u%u block %u%u%u%u; next: words %llx block %llx; calls "
                                "%u %u\n",
                                end[0].n, end[0].z, end[0].cf, end[0].vf, end[1].n, end[1].z, end[1].cf, end[1].vf,
                                (unsigned long long)next[0], (unsigned long long)next[1], calls[0], calls[1]);
                    }
                    break;
                }
            }
        }
        printf("sail_x64: %zu blocks of 2 to 4 words\n", nseq);
    }
    printf("sail_x64: %zu words made (%zu of them not without BMI1, BMI2 and LZCNT), %zu runs, %zu failures (%zu words "
           "not made)\n",
           made, plain_unmade, compared, failures, unmade);
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    const char* cold = snx_x64_cold_pool();
    if (cold) {
        fprintf(stderr, "FAIL: the cold code's pool operand: %s\n", cold);
        return 1;
    }
    int rc = run(argc, argv);
    free(g_test.rows);
    free(g_test.words);
    free(g_test.sequences);
    return rc;
}
