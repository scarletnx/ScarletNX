/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A local-only performance check for a device build, chiefly iOS and Android. It uses only standard C timing and the
   recompiler library, so it can be built for a device without the host driver, a game, a window or Vulkan. It
   compiles a synthetic module of 64 functions and runs a small A64 loop in-process, printing the compile time and
   the recompiled/native ratio. There is no baseline, no ceiling and no CI test: the numbers are for a developer
   comparing two builds on the same device. Linux remains the performance gate (docs/PERF.md). */
#include "common/cpu/recompiler/snx_recompiler.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CODE          0x1000u
#define FUNCTIONS_AT  0x2000u
#define FUNCTION_SIZE 0x200u
#define FUNCTIONS     64
#define BODY_REPEAT   6
#define STOP          0u
#define MEMORY        (0x12000u + FUNCTIONS * FUNCTION_SIZE)

static const uint32_t body[] = {
    0x8b010000, 0xca000c21, 0x92089c02, 0xb2401843, 0xab010064, 0x9a804085,
    0xd34554a6, 0x9b0100c0, 0x8b411c27, 0x12001ce8, 0xca080021, 0xd1000c21,
};
static const uint32_t loop_words[] = {
    0xd2800000, 0xd2800021, 0x8b010000, 0xca000c21, 0x92089c02, 0xb2401843, 0xab010064, 0x9a804085, 0xd34554a6,
    0x9b0100c0, 0x8b411c27, 0x12001ce8, 0xca080021, 0x36280041, 0xd1000c21, 0xf1000673, 0x54fffe41, 0xd65f03c0,
};

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

static void put32(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t branch(uint32_t pc, uint32_t target) { return 0x94000000u | (((target - pc) >> 2) & 0x03ffffffu); }

static void place(uint8_t* memory, uint32_t at, const uint32_t* words, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) put32(memory + at + i * 4, words[i]);
}

static double elapsed_ms(clock_t start) { return 1000.0 * (double)(clock() - start) / (double)CLOCKS_PER_SEC; }

/* The same loop as perf_cpu.c, operation for operation */
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

static int compile_benchmark(struct snx_recompiler* recompiler, uint8_t* memory) {
    struct snx_recompiled* module;
    clock_t start;
    double ms;
    unsigned i;
    uint32_t at;
    for (i = 0; i < FUNCTIONS; ++i)
        put32(memory + CODE + i * 4, branch(CODE + i * 4, FUNCTIONS_AT + i * FUNCTION_SIZE));
    put32(memory + CODE + FUNCTIONS * 4, 0xd65f03c0);
    for (i = 0; i < FUNCTIONS; ++i) {
        at = FUNCTIONS_AT + i * FUNCTION_SIZE;
        for (unsigned r = 0; r < BODY_REPEAT; ++r) {
            place(memory, at, body, sizeof body / sizeof body[0]);
            at += (uint32_t)sizeof body;
        }
        put32(memory + at, 0xd65f03c0);
    }
    start = clock();
    module = snx_recompile(recompiler, CODE, memory + CODE, MEMORY - CODE);
    ms = elapsed_ms(start);
    if (!module) {
        fprintf(stderr, "perf_local: compile failed: %s\n", snx_recompiler_error(recompiler));
        return -1;
    }
    if (snx_recompiled_fallbacks(module)) {
        fprintf(stderr, "perf_local: the compiled module has instructions without an emitter\n");
        snx_recompiled_free(module);
        return -1;
    }
    printf("perf_local: compiled %u functions in %.1f ms\n", FUNCTIONS, ms);
    snx_recompiled_free(module);
    return 0;
}

static int run_benchmark(struct snx_recompiler* recompiler, uint8_t* memory) {
    struct snx_recompiled* module;
    RtAPI rt;
    CPU base, cpu;
    uint64_t iterations = 2000000, guest0 = 0, guest1 = 0, native0 = 0, native1 = 0;
    clock_t start;
    double guest, native;
    place(memory, CODE, loop_words, sizeof loop_words / sizeof loop_words[0]);
    module = snx_recompile(recompiler, CODE, memory + CODE, sizeof loop_words);
    if (!module) {
        fprintf(stderr, "perf_local: loop compile failed: %s\n", snx_recompiler_error(recompiler));
        return -1;
    }
    snx_runtime_defaults(&rt);
    rt.svc = fault_svc;
    rt.undef = fault_undef;
    rt.brk = fault_brk;
    memset(&base, 0, sizeof base);
    base.mem = memory;
    base.rt = &rt;
    if (snx_recompiled_bind(module, &base)) {
        fprintf(stderr, "perf_local: the CPU was refused the loop module\n");
        snx_recompiled_free(module);
        return -1;
    }
    cpu = base;
    cpu.pc = 0;
    cpu.poll = 0;
    memset(cpu.x, 0, sizeof cpu.x);
    cpu.x[19] = iterations;
    cpu.x[30] = STOP;
    start = clock();
    snx_recompiled_run(module, &cpu, CODE, STOP);
    guest = elapsed_ms(start);
    guest0 = cpu.x[0];
    guest1 = cpu.x[1];
    start = clock();
    native_kernel(iterations, &native0, &native1);
    native = elapsed_ms(start);
    snx_recompiled_free(module);
    if (faults || guest0 != native0 || guest1 != native1) {
        fprintf(stderr, "perf_local: the recompiled loop and the C loop disagree\n");
        return -1;
    }
    printf("perf_local: loop %llu iterations, recompiled %.1f ms, native %.1f ms, ratio %.2f\n",
           (unsigned long long)iterations, guest, native, guest / native);
    return 0;
}

int main(void) {
    struct snx_recompiler* recompiler;
    uint8_t* memory;
    int result = 0;
    recompiler = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    if (!recompiler) {
        fprintf(stderr, "perf_local: the LLVM backend is not available on this host\n");
        return 1;
    }
    snx_recompiler_set_jobs(recompiler, 1);
    memory = (uint8_t*)calloc(1, MEMORY);
    if (!memory) {
        fprintf(stderr, "perf_local: out of memory\n");
        snx_recompiler_free(recompiler);
        return 1;
    }
    if (compile_benchmark(recompiler, memory) || run_benchmark(recompiler, memory)) result = 1;
    free(memory);
    snx_recompiler_free(recompiler);
    return result;
}
