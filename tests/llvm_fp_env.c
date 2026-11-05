/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A generated entry must normalize host control bits even after the bitcode helper is inlined. */
#include "llvm_steps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
static u64 host_control(void) { return _mm_getcsr(); }
static void host_set(u64 value) { _mm_setcsr((unsigned)value); }
static u64 default_control(void) { return 0x1f80; }
static u64 control_mask(void) { return 0xffc0; }
static u64 altered(unsigned n) { return 0x1f80u | (n & 3) << 13 | ((n >> 2) & 1) << 15 | ((n >> 3) & 1) << 6 | (n & 16 ? 0x3f : 0); }
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
static u64 host_control(void) { u64 value; __asm__ volatile("mrs %0, fpcr" : "=r"(value)); return value; }
static void host_set(u64 value) { __asm__ volatile("msr fpcr, %0" : : "r"(value)); }
static u64 default_control(void) { return 0; }
static u64 control_mask(void) { return 0x07c8ff00ull; }
static u64 altered(unsigned n) { return (u64)(n & 3) << 22 | (u64)((n >> 2) & 1) << 24 | (u64)((n >> 3) & 1) << 19; }
#else
static u64 host_control(void) { return 0; }
static void host_set(u64 value) { (void)value; }
static u64 default_control(void) { return 0; }
static u64 control_mask(void) { return 0; }
static u64 altered(unsigned n) { (void)n; return 0; }
#endif

static int run(int jit, char opt, int benchmark) {
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps, jit, 0, opt, error, sizeof error)) return snx_error("%s", error);
    llvm_steps_add(&steps, 0x4e22d420u, 0x1000, 0); /* FADD V0.4S,V1.4S,V2.4S requires the host environment. */
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    CPU cpu = {0};
    cpu.pc = 0x1000;
    cpu.fpsr = 16;
    for (unsigned lane = 0; lane < 4; lane++) {
        cpu.v[1].s[lane] = 0x3f800000;
        cpu.v[2].s[lane] = 0x33800000; /* halfway: round-to-nearest must produce 1, not the next float */
    }
    u64 saved = host_control();
    int failed = 0;
    for (unsigned n = 0; n < 32; n++) {
        u64 input = altered(n);
        host_set(input);
        u64 next = steps.steps[0](&cpu), control = host_control();
        int values = 1;
        for (unsigned lane = 0; lane < 4; lane++) values &= cpu.v[0].s[lane] == 0x3f800000;
        host_set(saved);
        if (next != 0x1004 || (control & control_mask()) != default_control() || !values) {
            fprintf(stderr, "entry control O%c jit%d mode%u: %llx\n", opt, jit, n, (unsigned long long)control);
            failed = 1;
            break;
        }
    }
    if (benchmark && !failed) {
        host_set(default_control());
        u64 checksum = 0;
        double start = snx_seconds();
        for (unsigned n = 0; n < 10000000; n++) checksum += steps.steps[0](&cpu);
        double elapsed = snx_seconds() - start;
        host_set(saved);
        printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n", elapsed * 1e9, (unsigned long long)checksum);
    }
    llvm_steps_end(&steps);
    return failed;
}
int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--bench")) return run(1, argc > 2 ? argv[2][0] : '2', 1);
    for (int jit = 0; jit < 2; jit++)
        for (unsigned level = 0; level < 3; level++)
            if (run(jit, (char)('0' + level), 0)) return 1;
    return 0;
}
