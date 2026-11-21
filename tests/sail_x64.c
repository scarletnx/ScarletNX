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
     snx-test-sail-x64 [--states N] [--samples N] [--sequences N] [--verbose] [--dump WORD...]
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

static struct {
    u64 seed;
    unsigned undef;
    int verbose;
    sigjmp_buf jump;
    int jumping;
} g_test;

static u64 rnd(void) {
    g_test.seed ^= g_test.seed << 13;
    g_test.seed ^= g_test.seed >> 7;
    g_test.seed ^= g_test.seed << 17;
    return g_test.seed;
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
#endif
    return f;
}

/* the back end's sg_fail aborts: a word it can't make is named, and the test goes on */
static void on_abort(int signal_number) {
    (void)signal_number;
    if (g_test.jumping) siglongjmp(g_test.jump, 1);
}

static void on_undef(CPU* c, u64 pc, u32 word) {
    (void)c;
    (void)pc;
    (void)word;
    ++g_test.undef;
}

typedef u64 (*Step)(CPU* c);

int main(int argc, char** argv) {
    unsigned states = 64, samples = 3, sequences = 4000, i, k;
    unsigned hosts[2];
    u32 *rows = NULL, *words;
    size_t count, n = 0, failures = 0, unmade = 0, compared = 0, bytes = 0, made = 0, plain_unmade = 0;
    unsigned char* code;
    size_t code_at = 0, code_cap = (size_t)1 << 26;
    size_t (*offset)[2];
    LlvmSteps set;
    char error[1024];
    RtAPI rt = {.undef = on_undef};
    struct sigaction sa;
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--states") && i + 1 < (unsigned)argc) states = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--samples") && i + 1 < (unsigned)argc)
            samples = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sequences") && i + 1 < (unsigned)argc)
            sequences = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose"))
            g_test.verbose = 1;
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
    hosts[1] = 0;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_abort;
    sigaction(SIGABRT, &sa, NULL);
    code = (unsigned char*)mmap(NULL, code_cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code == MAP_FAILED) {
        fprintf(stderr, "FAIL: no memory for the code\n");
        return 1;
    }
    /* the words: each row's samples, and each with Rd, Rn and Rm 31 */
    count = lqd_row_words(samples, rnd, &rows);
    words = (u32*)malloc(sizeof(u32) * (count * 8 + 1));
    offset = malloc(sizeof *offset * (count * 8 + 1));
    if (!words || !offset) return 1;
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
    free(rows);
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
                end[0] = start;
                end[1] = start;
                g_test.undef = 0;
                next[0] = set.steps[i](&end[0]);
                undef[0] = g_test.undef;
                g_test.undef = 0;
                next[1] = x64(&end[1]);
                undef[1] = g_test.undef;
                ++compared;
                if (next[0] != next[1] || undef[0] != undef[1] || memcmp(end[0].x, end[1].x, sizeof end[0].x) ||
                    end[0].n != end[1].n || end[0].z != end[1].z || end[0].cf != end[1].cf || end[0].vf != end[1].vf) {
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
        if (mprotect(code, code_cap, PROT_READ | PROT_WRITE)) return 1;
        for (q = 0; q < sequences; ++q) {
            Seq* sq = &seq[nseq];
            size_t len = 2 + rnd() % 3, got, j;
            for (j = 0; j < len; ++j) sq->w[j] = words[rnd() % n];
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
                g_test.undef = 0;
                for (j = 0; j < seq[q].count; ++j) next[0] = set.steps[seq[q].first + j](&end[0]);
                undef[0] = g_test.undef;
                g_test.undef = 0;
                next[1] = x64(&end[1]);
                undef[1] = g_test.undef;
                ++compared;
                if (next[0] != next[1] || undef[0] != undef[1] || memcmp(end[0].x, end[1].x, sizeof end[0].x) ||
                    end[0].n != end[1].n || end[0].z != end[1].z || end[0].cf != end[1].cf || end[0].vf != end[1].vf) {
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
                        fprintf(stderr, "  nzcv: llvm %u%u%u%u x64 %u%u%u%u; next: llvm %llx x64 %llx\n", end[0].n,
                                end[0].z, end[0].cf, end[0].vf, end[1].n, end[1].z, end[1].cf, end[1].vf,
                                (unsigned long long)next[0], (unsigned long long)next[1]);
                    }
                    break;
                }
            }
        }
        llvm_steps_end(&set);
        printf("sail_x64: %zu blocks of 2 to 4 words\n", nseq);
        free(seq);
    }
    printf("sail_x64: %zu words made (%zu bytes of x86-64 on this host's features, %.1f a word; %zu of them not without "
           "BMI1, BMI2 and LZCNT), %zu runs, %zu failures (%zu words not made)\n",
           made, bytes, made ? (double)bytes / (double)made : 0.0, plain_unmade, compared, failures, unmade);
    return failures ? 1 : 0;
}
