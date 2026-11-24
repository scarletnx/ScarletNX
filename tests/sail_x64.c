/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's x86-64 back end (tools/sailgen/x64.h, its instruction selection derived from the Intel SDM) against its
   LLVM back end, word by word: every word of the decoder's rows (llvm_qemu_words.c, decoded for the ounce target, each
   again with Rd, Rn and Rm 31) that the x86-64 back end's roots (a64_x64.roots) take becomes x86-64 code for u64
   step(CPU*), once with the host's BMI1, BMI2 and LZCNT and once with none (the candidates without them), and an LLVM
   step (llvm_steps.c, the generated LLVM emitters); both run from the same random states, and every register, the
   flags, the next pc and the runtime's UNDEF must match. Then sequences of two to four of those words (--sequences N)
   become one block each, one SSA (a register read once, a compare's flags read by the next word's condition, the stores
   a later one overwrites dropped), run beside the LLVM steps of the words one after another, each at its pc.
     snx-test-sail-x64 [--states N] [--samples N] [--sequences N] [--pool N] [--verbose] [--dump WORD...]
   --pool N gives the back end N host registers of its nine, so it spills (ctest runs it with 3 too)
   A word the x86-64 back end takes but can't make (its sg_fail) is a failure too, named with why */
#include "sail_x64_emit.h"
#include "llvm_steps.h"
#include "llvm_qemu_words.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if defined(__x86_64__)
#include <cpuid.h>
#endif

#define TARGET SNX_A64_TARGET_OUNCE
#define CODE_MAX 4096
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
    size_t (*offset)[2]; /* and the words after them */
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
/* --verbose: the SIMD&FP registers, FPSR and FPCR where the ends differ */
static void fp_report(const CPU* a, const CPU* b, const CPU* start) {
    unsigned r;
    for (r = 0; r < 32; ++r)
        if (memcmp(&a->v[r], &b->v[r], sizeof a->v[r]))
            fprintf(stderr, "  v%u: llvm %016llx:%016llx x64 %016llx:%016llx (was %016llx:%016llx)\n", r,
                    (unsigned long long)a->v[r].d[1], (unsigned long long)a->v[r].d[0],
                    (unsigned long long)b->v[r].d[1], (unsigned long long)b->v[r].d[0],
                    (unsigned long long)start->v[r].d[1], (unsigned long long)start->v[r].d[0]);
    if (a->fpsr != b->fpsr || a->fpcr != b->fpcr)
        fprintf(stderr, "  fpsr: llvm %08x x64 %08x (was %08x); fpcr: llvm %08x x64 %08x\n", a->fpsr, b->fpsr,
                start->fpsr, a->fpcr, b->fpcr);
}
/* the SIMD&FP state two ends differ in */
static int fp_differs(const CPU* a, const CPU* b) {
    return memcmp(a->v, b->v, sizeof a->v) || a->fpsr != b->fpsr || a->fpcr != b->fpcr;
}


static const char* op_name(u32 word) {
    SnxA64Insn insn;
    return snx_a64_decode_word(word, TARGET, &insn) != SNX_A64_OP_UNDEF ? snx_a64_op_text(insn.op) : NULL;
}

static unsigned host_features(void) {
    unsigned f = 0;
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

/* a load's or a store's: the address registers into the memory, and for a store-exclusive an armed monitor half the
   time (as llvm_sail_ir.c's ldst_start) */
static int is_ldst(u32 word) {
    const char* n = op_name(word);
    return n && (!strncmp(n, "LD", 2) || !strncmp(n, "ST", 2) || !strncmp(n, "CAS", 3) || !strncmp(n, "SWP", 3) ||
                 !strncmp(n, "PRF", 3));
}
static void ldst_start(CPU* c, u32 word, unsigned s) {
    SnxA64Insn insn;
    u64 address;
    snx_a64_decode_word(word, TARGET, &insn);
    c->x[31] = 0x1000 + (rnd() & 0xff0);
    c->x[insn.args.rn] = 0x1000 + (rnd() & 0xff0);
    c->x[insn.args.rm] = 0x1000 + (rnd() & 0xff0);
    if (insn.op == SNX_A64_OP_LDAPR_i || insn.op == SNX_A64_OP_STLR_i)
        c->x[insn.args.rn] -= (c->x[insn.args.rn] + (u64)(int64_t)insn.args.imm) & ((1u << insn.args.sz) - 1);
    address = c->x[insn.args.rn];
    c->excl_on = 0;
    if (!(s & 1)) return;
    if (insn.op == SNX_A64_OP_STXR || (insn.op == SNX_A64_OP_STXP && insn.args.sz == 3)) {
        c->excl_on = 1;
        c->excl_addr = address;
        memcpy(&c->excl_val, g_test.memory[2] + MEMORY_ZERO + address, 8);
        memcpy(&c->excl_val2, g_test.memory[2] + MEMORY_ZERO + address + 8, 8);
        if (s & 2) c->excl_val = ~c->excl_val;
        c->excl_size = insn.op == SNX_A64_OP_STXP ? 0x13 : (u32)insn.args.sz;
    }
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
    unsigned char* code = (unsigned char*)mmap(NULL, CODE_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    RtAPI rt = {.undef = on_undef, .sev = on_sev, .wfe = on_wfe, .cas128 = snx_runtime_cas128};
    size_t failures = 0;
    int pool = snx_x64_pool();
    if (code == MAP_FAILED) {
        fprintf(stderr, "FAIL: CASP: no memory for the code\n");
        return 1;
    }
    for (int variant = 0; variant < 4; ++variant) {
        unsigned features = variant & 1 ? SNX_X64_BASELINE : host_features();
        snx_x64_set_pool(variant & 2 ? 3 : pool);
        if (mprotect(code, CODE_MAX, PROT_READ | PROT_WRITE) ||
            !snx_x64_step(0x48207C82u, 0x1000, TARGET, features, code, CODE_MAX) ||
            mprotect(code, CODE_MAX, PROT_READ | PROT_EXEC)) {
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
    munmap(code, CODE_MAX);
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
    unsigned char* code = (unsigned char*)mmap(NULL, CODE_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    size_t failures = 0, k;
    if (code == MAP_FAILED) {
        fprintf(stderr, "FAIL: FZ's hot paths: no memory for the code\n");
        return 1;
    }
    for (k = 0; k < sizeof cases / sizeof *cases; ++k) {
        CPU c;
        unsigned long calls;
        u64 got;
        int cold, want_cold;
        if (mprotect(code, CODE_MAX, PROT_READ | PROT_WRITE) ||
            !snx_x64_step(cases[k].word, 0x1000, TARGET, host_features(), code, CODE_MAX) ||
            mprotect(code, CODE_MAX, PROT_READ | PROT_EXEC)) {
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
    munmap(code, CODE_MAX);
    printf("sail_x64: FZ's hot paths, %zu cases\n", sizeof cases / sizeof *cases);
    return failures;
}

static int run(int argc, char** argv) {
    unsigned states = 64, samples = 3, sequences = 4000, i, k;
    unsigned hosts[2];
    u32 *rows, *words;
    size_t count, n = 0, failures = 0, unmade = 0, compared = 0, bytes = 0, made = 0, plain_unmade = 0;
    unsigned char* code;
    size_t code_at = 0, code_cap = (size_t)1 << 26;
    size_t (*offset)[2];
    LlvmSteps set;
    char error[1024];
    RtAPI rt = {.undef = on_undef, .sev = on_sev, .wfe = on_wfe, .cas128 = snx_runtime_cas128};
    struct sigaction sa;
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--states") && i + 1 < (unsigned)argc) states = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--samples") && i + 1 < (unsigned)argc)
            samples = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pool") && i + 1 < (unsigned)argc)
            snx_x64_set_pool(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--sequences") && i + 1 < (unsigned)argc)
            sequences = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose"))
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
        }
        else {
            fprintf(stderr, "usage: snx-test-sail-x64 [--states N] [--samples N] [--verbose]\n");
            return 2;
        }
    }
#if !defined(__x86_64__)
    printf("not an x86-64 host: skipped\n");
    return 0;
#endif
    g_test.seed = 0x9e3779b97f4a7c15ull;
    hosts[0] = host_features();
    hosts[1] = SNX_X64_BASELINE; /* no BMI1, BMI2, LZCNT, AVX, FMA or F16C */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_abort;
    sigaction(SIGABRT, &sa, NULL);
    for (k = 0; k < 3; ++k) {
        g_test.memory[k] = (unsigned char*)mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (g_test.memory[k] == MAP_FAILED) return 1;
    }
    failures += check_fz_hot();
    failures += check_casp();
    for (i = 0; i < MEMORY_SIZE; ++i) g_test.memory[2][i] = (unsigned char)rnd();
    memcpy(g_test.memory[0], g_test.memory[2], MEMORY_SIZE);
    memcpy(g_test.memory[1], g_test.memory[2], MEMORY_SIZE);
    code = (unsigned char*)mmap(NULL, code_cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code == MAP_FAILED) {
        fprintf(stderr, "FAIL: no memory for the code\n");
        return 1;
    }
    /* the words: each row's samples, and each with Rd, Rn and Rm 31 */
    count = lqd_row_words(samples, rnd, &g_test.rows);
    rows = g_test.rows;
    /* the offsets of the words' code, then the words: one block */
    offset = g_test.offset = malloc((sizeof *offset + sizeof *words) * (count * 8 + 1));
    if (!offset) return 1;
    words = (u32*)(offset + count * 8 + 1);
    for (i = 0; i < count; ++i) {
        static const u32 fields[3] = {0x1fu, 0x1fu << 5, 0x1fu << 16};
        const char* name = op_name(rows[i]);
        unsigned f;
        if (!name) continue;
        for (f = 0; f < 8; ++f) {
            u32 w = rows[i];
            for (k = 0; k < 3; ++k)
                if (f & (1u << k)) w |= fields[k];
            if (f && (w == rows[i] || !op_name(w) || strcmp(op_name(w), name))) continue;
            /* x86-64 code for both hosts; a word neither makes is not the x86-64 back end's */
            {
                size_t got[2] = {0, 0};
                int failed = 0;
                for (k = 0; k < 2; ++k) {
                    g_test.jumping = 1;
                    if (sigsetjmp(g_test.jump, 1)) {
                        failed = 1;
                        got[k] = 0;
                    } else
                        got[k] = snx_x64_step(w, 0x1000, TARGET, hosts[k], code + code_at, code_cap - code_at);
                    g_test.jumping = 0;
                    if (failed) break;
                    offset[n][k] = code_at;
                    code_at += (got[k] + 15) & ~(size_t)15;
                }
                if (failed) {
                    ++unmade;
                    ++failures;
                    fprintf(stderr, "FAIL: %s %08x: the x86-64 back end takes it but can't make it\n", name, (unsigned)w);
                    continue;
                }
                if (!got[0]) continue;
                if (!got[1]) {
                    /* a primitive no form without BMI1, BMI2 and LZCNT makes (CLZ without LZCNT): this host's alone */
                    ++plain_unmade;
                    offset[n][1] = (size_t)-1;
                }
                bytes += got[0];
                ++made;
                words[n++] = w;
            }
        }
    }
    if (mprotect(code, code_cap, PROT_READ | PROT_EXEC)) {
        fprintf(stderr, "FAIL: the code can't be made executable\n");
        return 1;
    }
    if (llvm_steps_begin(&set, 0, LLVM_STEPS_HOST, error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    for (i = 0; i < n; ++i) llvm_steps_add(&set, words[i], 0x1000, TARGET);
    if (llvm_steps_load(&set, error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    for (i = 0; i < n; ++i)
        for (k = 0; k < 2; ++k) {
            Step x64;
            unsigned s;
            if (offset[i][k] == (size_t)-1) continue;
            x64 = (Step)(void*)(code + offset[i][k]);
            for (s = 0; s < states; ++s) {
                CPU start, end[2];
                u64 next[2];
                unsigned undef[2], r;
                memset(&start, 0, sizeof start);
                for (r = 0; r < 32; ++r) start.x[r] = rnd();
                fp_state(&start, s);
                if (s & 1) start.x[rnd() & 31] = 0;
                if (s & 2) start.x[rnd() & 31] = 0x8000000000000000ull;
                if (s & 4) start.x[rnd() & 31] = ~0ull;
                if (s & 8) start.x[rnd() & 31] = 0xffffffffull;
                start.n = rnd() & 1;
                start.z = rnd() & 1;
                start.cf = rnd() & 1;
                start.vf = rnd() & 1;
                start.rt = &rt;
                start.pc = 0x1000;
                if (is_ldst(words[i])) ldst_start(&start, words[i], s);
                end[0] = start;
                end[1] = start;
                end[0].mem = g_test.memory[0] + MEMORY_ZERO;
                end[1].mem = g_test.memory[1] + MEMORY_ZERO;
                g_test.undef = g_test.sev = g_test.wfe = 0;
                if (g_test.verbose == 2) fprintf(stderr, "run %s %08x (%s host) state %u\n", op_name(words[i]),
                                                 (unsigned)words[i], k ? "a plain" : "this", s);
                next[0] = set.steps[i](&end[0]);
                undef[0] = g_test.undef + 16 * g_test.sev + 256 * g_test.wfe;
                g_test.undef = g_test.sev = g_test.wfe = 0;
                next[1] = run_step(x64, &end[1]);
                undef[1] = g_test.undef + 16 * g_test.sev + 256 * g_test.wfe;
                ++compared;
                {
                    int memory = memcmp(g_test.memory[0] + MEMORY_ZERO, g_test.memory[1] + MEMORY_ZERO, MEMORY_TOUCHED);
                    end[0].mem = end[1].mem = NULL;
                    if (memory || end[0].excl_on != end[1].excl_on) next[1] = ~next[0]; /* a difference */
                    memcpy(g_test.memory[0] + MEMORY_ZERO, g_test.memory[2] + MEMORY_ZERO, MEMORY_TOUCHED);
                    memcpy(g_test.memory[1] + MEMORY_ZERO, g_test.memory[2] + MEMORY_ZERO, MEMORY_TOUCHED);
                }
                if (next[0] != next[1] || undef[0] != undef[1] || memcmp(end[0].x, end[1].x, sizeof end[0].x) ||
                    end[0].n != end[1].n || end[0].z != end[1].z || end[0].cf != end[1].cf || end[0].vf != end[1].vf ||
                    fp_differs(&end[0], &end[1])) {
                    ++failures;
                    fprintf(stderr, "FAIL: %s %08x (%s host) state %u: the x86-64 code's result differs\n",
                            op_name(words[i]), (unsigned)words[i], k ? "a plain" : "this", s);
                    if (g_test.verbose) {
                        for (r = 0; r < 32; ++r)
                            if (end[0].x[r] != end[1].x[r])
                                fprintf(stderr, "  x%u: llvm %016llx x64 %016llx (was %016llx)\n", r,
                                        (unsigned long long)end[0].x[r], (unsigned long long)end[1].x[r],
                                        (unsigned long long)start.x[r]);
                        fprintf(stderr, "  nzcv: llvm %u%u%u%u x64 %u%u%u%u; next: llvm %llx x64 %llx; undef %u %u\n",
                                end[0].n, end[0].z, end[0].cf, end[0].vf, end[1].n, end[1].z, end[1].cf, end[1].vf,
                                (unsigned long long)next[0], (unsigned long long)next[1], undef[0], undef[1]);
                        fp_report(&end[0], &end[1], &start);
                    }
                    break;
                }
            }
        }
    llvm_steps_end(&set);
    /* sequences: words in a row as one block, against their LLVM steps one after another */
    if (n && sequences) {
        typedef struct {
            u32 w[4];
            size_t count, code, first;
        } Seq;
        Seq* seq = (Seq*)malloc(sizeof(Seq) * sequences);
        size_t q, nseq = 0, steps = 0;
        if (!seq) return 1;
        g_test.sequences = seq;
        if (mprotect(code, code_cap, PROT_READ | PROT_WRITE)) return 1;
        for (q = 0; q < sequences; ++q) {
            Seq* sq = &seq[nseq];
            size_t len = 2 + rnd() % 3, got, j;
            for (j = 0; j < len; ++j)
                do
                    sq->w[j] = words[rnd() % n];
                while (is_ldst(sq->w[j])); /* their registers would point anywhere after another word */
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
            got = snx_x64_block(sq->w, &sq->count, 0x1000, TARGET, hosts[0], code + code_at, code_cap - code_at);
            g_test.jumping = 0;
            if (!got) continue;
            sq->code = code_at;
            code_at += (got + 15) & ~(size_t)15;
            ++nseq;
        }
        if (mprotect(code, code_cap, PROT_READ | PROT_EXEC)) return 1;
        if (llvm_steps_begin(&set, 0, LLVM_STEPS_HOST, error, sizeof error)) {
            fprintf(stderr, "FAIL: %s\n", error);
            return 1;
        }
        for (q = 0; q < nseq; ++q) {
            size_t j;
            seq[q].first = steps;
            for (j = 0; j < seq[q].count; ++j, ++steps) llvm_steps_add(&set, seq[q].w[j], 0x1000 + 4 * j, TARGET);
        }
        if (llvm_steps_load(&set, error, sizeof error)) {
            fprintf(stderr, "FAIL: %s\n", error);
            return 1;
        }
        for (q = 0; q < nseq; ++q) {
            Step x64 = (Step)(void*)(code + seq[q].code);
            unsigned st;
            for (st = 0; st < states; ++st) {
                CPU start, end[2];
                u64 next[2] = {0, 0};
                unsigned undef[2], r;
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
                start.pc = 0x1000;
                end[0] = start;
                end[1] = start;
                g_test.undef = g_test.sev = g_test.wfe = 0;
                for (j = 0; j < seq[q].count; ++j) next[0] = set.steps[seq[q].first + j](&end[0]);
                undef[0] = g_test.undef + 16 * g_test.sev + 256 * g_test.wfe;
                g_test.undef = g_test.sev = g_test.wfe = 0;
                next[1] = run_step(x64, &end[1]);
                undef[1] = g_test.undef + 16 * g_test.sev + 256 * g_test.wfe;
                ++compared;
                if (next[0] != next[1] || undef[0] != undef[1] || memcmp(end[0].x, end[1].x, sizeof end[0].x) ||
                    end[0].n != end[1].n || end[0].z != end[1].z || end[0].cf != end[1].cf || end[0].vf != end[1].vf ||
                    fp_differs(&end[0], &end[1])) {
                    ++failures;
                    fprintf(stderr, "FAIL: the block of");
                    for (j = 0; j < seq[q].count; ++j)
                        fprintf(stderr, " %s %08x", op_name(seq[q].w[j]), (unsigned)seq[q].w[j]);
                    fprintf(stderr, " state %u: the x86-64 code's result differs\n", st);
                    if (g_test.verbose) {
                        for (r = 0; r < 32; ++r)
                            if (end[0].x[r] != end[1].x[r])
                                fprintf(stderr, "  x%u: llvm %016llx x64 %016llx\n", r,
                                        (unsigned long long)end[0].x[r], (unsigned long long)end[1].x[r]);
                        fprintf(stderr, "  nzcv: llvm %u%u%u%u x64 %u%u%u%u; next: llvm %llx x64 %llx; undef %u %u\n",
                                end[0].n, end[0].z, end[0].cf, end[0].vf, end[1].n, end[1].z, end[1].cf, end[1].vf,
                                (unsigned long long)next[0], (unsigned long long)next[1], undef[0], undef[1]);
                    }
                    break;
                }
            }
        }
        llvm_steps_end(&set);
        printf("sail_x64: %zu blocks of 2 to 4 words\n", nseq);
    }
    printf("sail_x64: %zu words made (%zu bytes of x86-64 on this host's features, %.1f a word; %zu of them not without "
           "BMI1, BMI2 and LZCNT), %zu runs, %zu failures (%zu words not made)\n",
           made, bytes, made ? (double)bytes / (double)made : 0.0, plain_unmade, compared, failures, unmade);
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    int rc = run(argc, argv);
    free(g_test.rows);
    free(g_test.offset);
    free(g_test.sequences);
    return rc;
}
