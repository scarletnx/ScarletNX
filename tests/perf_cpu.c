/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The CPU half of the performance regression test: a small A64 loop recompiled by the LLVM backend in this process,
   against the same loop written in C. Both run the same 15 operations an iteration, interleaved, and only this
   thread's CPU time is counted: another process running beside the test does not add to it, and a processor that
   changes its clock slows both sides of a round alike. The ratio of the two is what the ceiling in
   tests/perf_baseline.txt limits, so a slower code emitter, a worse instruction sequence or a slower helper shows up
   where the host's own speed and its load cancel out.

   A recompiled loop against native C is not a lower bound on what a recompiler can do (the guest's flags, the
   dispatch and the ABI each cost something), so the ceiling is generous. It is a regression signal, not a score.

   It times only this loop's integer, flag, select, shift, multiply and branch instructions, and the generated-code
   ABI they go through. SIMD, floating point, crypto, loads and stores, atomics, the JIT's compile time, services,
   threads and real game code are outside it; one of those needs its own test or a game run.

   The loop (llvm-mc -triple=aarch64, at 0x1000; x19 counts down from the caller):
       mov x0,#0; mov x1,#1
   loop:
       add x0,x0,x1
       eor x1,x1,x0,lsl #3
       and x2,x0,#0xff00ff00ff00ff00
       orr x3,x2,#0x7f
       adds x4,x3,x1
       csel x5,x4,x0,mi
       ubfx x6,x5,#5,#17
       madd x0,x6,x1,x0
       add x7,x1,x1,lsr #7
       and w8,w7,#0xff
       eor x1,x1,x8
       tbz x1,#5,1f
       sub x1,x1,#3
   1:  subs x19,x19,#1
       b.ne loop
       ret */
#include "support/perf.h"
#include "common/cpu/recompiler/snx_recompiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE          0x1000u
#define STOP          0u
#define MEMORY        0x10000u
#define ROUNDS        401 /* about a second of pairs: a narrower interval than a runner's run-to-run spread */
#define CANARY_ROUNDS 3   /* a few pairs first: only a massive regression stops before the full block */

static const uint32_t kernel_words[] = {
    0xd2800000, /* mov x0, #0 */
    0xd2800021, /* mov x1, #1 */
    0x8b010000, /* add x0, x0, x1 */
    0xca000c21, /* eor x1, x1, x0, lsl #3 */
    0x92089c02, /* and x2, x0, #0xff00ff00ff00ff00 */
    0xb2401843, /* orr x3, x2, #0x7f */
    0xab010064, /* adds x4, x3, x1 */
    0x9a804085, /* csel x5, x4, x0, mi */
    0xd34554a6, /* ubfx x6, x5, #5, #17 */
    0x9b0100c0, /* madd x0, x6, x1, x0 */
    0x8b411c27, /* add x7, x1, x1, lsr #7 */
    0x12001ce8, /* and w8, w7, #0xff */
    0xca080021, /* eor x1, x1, x8 */
    0x36280041, /* tbz x1, #5, 1f */
    0xd1000c21, /* sub x1, x1, #3 */
    0xf1000673, /* subs x19, x19, #1 */
    0x54fffe41, /* b.ne loop */
    0xd65f03c0, /* ret */
};

/* A Mac on Apple silicon runs the same guest loop against the same C mirror with a different ratio: its performance
   and efficiency cores make the C loop's speed move more than the generated code's. It has a key of its own so a
   Linux CI runner's reference is not applied to it */
#if defined(__APPLE__) && (defined(__aarch64__) || defined(_M_ARM64))
#define PERF_CPU_KEY "cpu_aarch64_darwin"
#elif defined(__x86_64__) || defined(_M_X64)
#define PERF_CPU_KEY "cpu_x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define PERF_CPU_KEY "cpu_aarch64"
#else
#define PERF_CPU_KEY "cpu_other"
#endif

static unsigned faults;

static void fault_svc(CPU* cpu, uint32_t imm) {
    (void)imm;
    ++faults;
    cpu->pc = STOP;
}

static void fault_undef(CPU* cpu, uint64_t pc, uint32_t word) {
    (void)pc;
    (void)word;
    ++faults;
    cpu->pc = STOP;
}

static void fault_brk(CPU* cpu, uint64_t pc, uint32_t imm) {
    (void)pc;
    (void)imm;
    ++faults;
    cpu->pc = STOP;
}

static void put32le(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* The loop above, operation for operation: the unsigned wrapping, the 32-bit and, the negative flag's selection and
   the skipped subtraction are all the guest's */
static void native_kernel(uint64_t iterations, uint64_t* out0, uint64_t* out1) {
    uint64_t x0 = 0, x1 = 1, i;
    for (i = 0; i < iterations; ++i) {
        uint64_t x2, x3, x4, x5, x6, x7, x8;
        x0 += x1;
        x1 ^= x0 << 3;
        x2 = x0 & UINT64_C(0xff00ff00ff00ff00);
        x3 = x2 | UINT64_C(0x7f);
        x4 = x3 + x1;
        x5 = (int64_t)x4 < 0 ? x4 : x0;
        x6 = (x5 >> 5) & UINT64_C(0x1ffff);
        x0 = x6 * x1 + x0;
        x7 = x1 + (x1 >> 7);
        x8 = x7 & UINT64_C(0xff);
        x1 ^= x8;
        if (x1 & UINT64_C(0x20)) x1 -= 3;
    }
    *out0 = x0;
    *out1 = x1;
}

static uint64_t measure_guest(struct snx_recompiled* module, const CPU* base, uint64_t iterations, uint64_t* out0,
                              uint64_t* out1) {
    CPU cpu = *base;
    uint64_t start, end;
    cpu.pc = 0;
    cpu.poll = 0;
    memset(cpu.x, 0, sizeof cpu.x);
    cpu.x[19] = iterations;
    cpu.x[30] = STOP;
    start = perf_thread_cpu_ns();
    snx_recompiled_run(module, &cpu, CODE, STOP);
    end = perf_thread_cpu_ns();
    *out0 = cpu.x[0];
    *out1 = cpu.x[1];
    return end - start;
}

static uint64_t measure_native(uint64_t iterations, uint64_t* out0, uint64_t* out1) {
    uint64_t start, end;
    start = perf_thread_cpu_ns();
    native_kernel(iterations, out0, out1);
    end = perf_thread_cpu_ns();
    return end - start;
}

/* ROUNDS of the ABBA pairs above. Two of each side per round, alternating which side starts, so a clock or a core
   that changes during the round moves both sides alike. 0 when the two loops disagree */
static int measure_cpu_block(struct snx_recompiled* module, const CPU* base, uint64_t iterations, size_t rounds,
                             double* ratios) {
    size_t i;
    for (i = 0; i < rounds; ++i) {
        uint64_t g1, g2, n1, n2, ga, gb, na, nb;
        double guest, native;
        if (i & 1) {
            na = measure_native(iterations, &n1, &n2);
            ga = measure_guest(module, base, iterations, &g1, &g2);
            gb = measure_guest(module, base, iterations, &g1, &g2);
            nb = measure_native(iterations, &n1, &n2);
        } else {
            ga = measure_guest(module, base, iterations, &g1, &g2);
            na = measure_native(iterations, &n1, &n2);
            nb = measure_native(iterations, &n1, &n2);
            gb = measure_guest(module, base, iterations, &g1, &g2);
        }
        if (g1 != n1 || g2 != n2) {
            fprintf(stderr, "perf_cpu: round %llu: the loops disagree\n", (unsigned long long)i);
            return 0;
        }
        guest = (double)(ga + gb) / 2.0;
        native = (double)(na + nb) / 2.0;
        ratios[i] = guest / native;
        if (getenv("SNX_PERF_VERBOSE"))
            printf("perf_cpu: round %llu: recompiled %.3f ms, native %.3f ms, ratio %.2f\n", (unsigned long long)i,
                   guest / 1e6, native / 1e6, ratios[i]);
    }
    return 1;
}

int main(int argc, char** argv) {
    PerfEnvironment env;
    PerfDecision decision;
    struct snx_recompiler* recompiler;
    struct snx_recompiled* module;
    RtAPI rt;
    CPU base;
    uint8_t* memory;
    uint64_t iterations = 200000;
    double ratios[ROUNDS], retest_ratios[ROUNDS];
    double reference, warn_percent, fail_percent, warn, fail, ceiling, massive;
    const char* baseline;
    PerfResult first, second;
    char note[256];
    size_t k;
    int failed = 0;
    int skipped = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s BASELINE\n", argv[0]);
        return 2;
    }
    env = perf_environment();
    printf("perf_cpu: %d processors, load %.2f, %llu MiB available, %s\n", env.processors, env.load1,
           (unsigned long long)env.memory_mb, env.strict ? "CI mode (a skip fails)" : "local mode (may skip)");
    decision = perf_decide(&env, "perf_cpu", perf_resource_refusal(&env));
    if (decision != PERF_RUN) return decision == PERF_SKIP ? 77 : 1;
    perf_prepare_thread();
    baseline = getenv("SNX_PERF_BASELINE");
    if (!baseline || !*baseline) baseline = argv[1];
    {
        int found = perf_baseline4(baseline, PERF_CPU_KEY, &reference, &warn_percent, &fail_percent, &ceiling);
        if (found != 1) {
            fprintf(stderr, "perf_cpu: %s: %s\n", baseline,
                    found < 0 ? "cannot be read or has a line without four numbers" : "has no " PERF_CPU_KEY " line");
            return 1;
        }
        warn = reference * (1.0 + warn_percent / 100.0);
        fail = reference * (1.0 + fail_percent / 100.0);
        if (!(reference > 0.0 && warn_percent >= 0.0 && warn_percent < fail_percent && warn < fail && fail < ceiling)) {
            fprintf(stderr, "perf_cpu: %s must have 0 < reference and warn%% < fail%% with warn < fail < ceiling\n",
                    baseline);
            return 1;
        }
    }

    memory = (uint8_t*)calloc(1, MEMORY);
    if (!memory) {
        fprintf(stderr, "perf_cpu: out of memory\n");
        return 1;
    }
    for (k = 0; k < sizeof kernel_words / sizeof kernel_words[0]; ++k) put32le(memory + CODE + k * 4, kernel_words[k]);

    recompiler = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    if (!recompiler) {
        fprintf(stderr, "perf_cpu: the LLVM backend is not available on this host\n");
        free(memory);
        return 1;
    }
    snx_recompiler_set_jobs(recompiler, 1); /* one core compiles and one core runs: the rest of the machine is free */
    module = snx_recompile(recompiler, CODE, memory + CODE, sizeof kernel_words);
    if (!module) {
        fprintf(stderr, "perf_cpu: the kernel did not compile: %s\n", snx_recompiler_error(recompiler));
        snx_recompiler_free(recompiler);
        free(memory);
        return 1;
    }
    if (snx_recompiled_fallbacks(module)) {
        fprintf(stderr, "perf_cpu: the kernel has instructions without an emitter\n");
        failed = 1;
        goto done;
    }
    snx_runtime_defaults(&rt);
    rt.svc = fault_svc;
    rt.undef = fault_undef;
    rt.brk = fault_brk;
    memset(&base, 0, sizeof base);
    base.mem = memory;
    base.rt = &rt;
    if (snx_recompiled_bind(module, &base)) {
        fprintf(stderr, "perf_cpu: the CPU was refused the module\n");
        failed = 1;
        goto done;
    }

    /* Both sides do the same work before a clock is read: the guest loop's first run also faults its code in */
    {
        uint64_t guest0, guest1, native0, native1;
        measure_guest(module, &base, 1000, &guest0, &guest1);
        measure_native(1000, &native0, &native1);
        if (guest0 != native0 || guest1 != native1 || faults) {
            fprintf(stderr,
                    "perf_cpu: the recompiled loop and the C loop disagree (%llu,%llu against %llu,%llu, %u "
                    "faults)\n",
                    (unsigned long long)guest0, (unsigned long long)guest1, (unsigned long long)native0,
                    (unsigned long long)native1, faults);
            failed = 1;
            goto done;
        }
    }
    /* A sample near half a millisecond of this thread's time. Two samples of each side are taken back to back in an
       ABBA pattern in every round: a processor that is warming up or cooling down, or that moves the thread between
       the two kinds of core a hybrid CPU has, moves both sides of the round alike, which one long sample of each
       would not cancel. The whole run stays around a tenth of a second. */
    {
        uint64_t probe0, probe1, probe = measure_guest(module, &base, iterations, &probe0, &probe1);
        if (probe > 0) {
            double scaled = (double)iterations * 500000.0 / (double)probe;
            if (scaled > 5000000.0) scaled = 5000000.0;
            if (scaled < 20000.0) scaled = 20000.0;
            iterations = (uint64_t)scaled;
        }
    }
    printf("perf_cpu: %llu iterations a sample, %d rounds a block\n", (unsigned long long)iterations, ROUNDS);
    if (!measure_cpu_block(module, &base, iterations, CANARY_ROUNDS, ratios) || faults) {
        fprintf(stderr, "perf_cpu: the canary faulted or the loops disagreed\n");
        failed = 1;
        goto done;
    }
    first = perf_result(ratios, CANARY_ROUNDS);
    massive = ceiling * 1.5;
    printf("perf_cpu: canary ratio %.2f over %d rounds (massive line %.2f)\n", first.ratio, CANARY_ROUNDS, massive);
    if (first.lower_strict > massive) {
        if (!measure_cpu_block(module, &base, iterations, CANARY_ROUNDS, retest_ratios) || faults) {
            fprintf(stderr, "perf_cpu: the canary confirmation faulted or the loops disagreed\n");
            failed = 1;
            goto done;
        }
        second = perf_result(retest_ratios, CANARY_ROUNDS);
        printf("perf_cpu: canary confirmation ratio %.2f (99.9%% lower %.2f)\n", second.ratio, second.lower_strict);
        if (second.lower_strict > massive) {
            snprintf(note, sizeof note,
                     "two canary blocks (%.2f and %.2f) are over the %.2f massive line at 99.9 percent confidence; "
                     "the full block was not run",
                     first.ratio, second.ratio, massive);
            perf_annotation(PERF_ERROR, "perf_cpu", note);
            failed = 1;
            goto done;
        }
        /* not confirmed: the full blocks below decide, as for any other reading */
    }
    if (!measure_cpu_block(module, &base, iterations, ROUNDS, ratios) || faults) {
        fprintf(stderr, "perf_cpu: the kernel faulted or the loops disagreed\n");
        failed = 1;
        goto done;
    }
    first = perf_result(ratios, ROUNDS);
    {
        const char* result = getenv("SNX_PERF_RESULT");
        if (result && *result) {
            FILE* file = fopen(result, "w");
            if (file) {
                fprintf(file, "%s %.6f %.6f %.6f %llu\n", PERF_CPU_KEY, first.ratio, first.lower, first.upper,
                        (unsigned long long)ROUNDS);
                fclose(file);
            }
        }
    }
    decision = perf_decide_noise(&env, "perf_cpu", first.spread);
    if (decision != PERF_RUN) {
        failed = decision == PERF_FAIL;
        skipped = decision == PERF_SKIP;
        goto done;
    }
    printf("perf_cpu: ratio %.2f (95%% lower %.2f, 99.9%% lower %.2f, reference %.2f, warn %.2f, fail %.2f, "
           "ceiling %.2f, spread %.0f%%)\n",
           first.ratio, first.lower, first.lower_strict, reference, warn, fail, ceiling, first.spread * 100);
    if (first.lower_strict > fail || first.ratio > ceiling) {
        if (!measure_cpu_block(module, &base, iterations, ROUNDS, retest_ratios) || faults) {
            fprintf(stderr, "perf_cpu: the confirmation faulted or the loops disagreed\n");
            failed = 1;
            goto done;
        }
        second = perf_result(retest_ratios, ROUNDS);
        printf("perf_cpu: confirmation ratio %.2f (99.9%% lower %.2f, spread %.0f%%)\n", second.ratio,
               second.lower_strict, second.spread * 100);
        if (second.lower_strict > fail) {
            snprintf(note, sizeof note,
                     "two independent blocks (%.2f and %.2f) are over the %.2f fail line at 99.9 percent confidence",
                     first.ratio, second.ratio, fail);
            perf_annotation(PERF_ERROR, "perf_cpu", note);
            failed = 1;
            goto done;
        }
        snprintf(note, sizeof note,
                 "first block %.2f crossed the %.2f fail line and the confirmation at %.2f did not; likely variance",
                 first.ratio, fail, second.ratio);
        perf_annotation(PERF_WARN, "perf_cpu", note);
        goto done;
    }
    if (first.lower > warn) {
        if (!measure_cpu_block(module, &base, iterations, ROUNDS, retest_ratios) || faults) {
            fprintf(stderr, "perf_cpu: the warning confirmation faulted or the loops disagreed\n");
            failed = 1;
            goto done;
        }
        second = perf_result(retest_ratios, ROUNDS);
        printf("perf_cpu: warning confirmation ratio %.2f (95%% lower %.2f)\n", second.ratio, second.lower);
        if (second.lower > warn)
            snprintf(note, sizeof note,
                     "ratio %.2f is over the %.2f warning line and a second block confirms it; this is advisory and "
                     "may still be variance",
                     first.ratio, warn);
        else
            snprintf(note, sizeof note,
                     "ratio %.2f is over the %.2f warning line but a second block does not confirm it; likely "
                     "variance",
                     first.ratio, warn);
        perf_annotation(PERF_WARN, "perf_cpu", note);
    }

done:
    snx_recompiled_free(module);
    snx_recompiler_free(recompiler);
    free(memory);
    return skipped ? 77 : failed;
}
