/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* FRECPX is an integer exponent operation with exact NaN and input-denormal exceptions. */
#include "llvm_steps.h"
#include <stdio.h>
#include <string.h>

static int check(LlvmStep step, unsigned width, u64 input, u32 fpcr, u32 fpsr) {
    CPU expected = {0};
    for (unsigned reg = 0; reg < 32; ++reg)
        expected.v[reg] = (V128){.d = {0x123456789abcdef0ull + reg, 0xfedcba9876543210ull - reg}};
    expected.v[1].d[0] = input;
    expected.fpcr = fpcr;
    expected.fpsr = fpsr;
    expected.pc = 0x1000;
    CPU actual = expected;
    u64 result = snx_fp_unary(&expected, input, (int)width, SNX_FP_RECPX);
    expected.v[0] = (V128){.d = {result, 0}};
    if (step(&actual) == 0x1004 && !memcmp(&actual, &expected, sizeof actual)) return 0;
    fprintf(stderr, "FRECPX width %u input %llx FPCR %x: result %llx/%llx FPSR %x/%x\n", width,
            (unsigned long long)input, fpcr, (unsigned long long)actual.v[0].d[0],
            (unsigned long long)expected.v[0].d[0], actual.fpsr, expected.fpsr);
    return 1;
}

static int run(int jit, char opt, unsigned long long* count) {
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps, jit, 0, opt, error, sizeof error)) return snx_error("%s", error);
    llvm_steps_add(&steps, 0x5EA1F820u, 0x1000, 0);
    llvm_steps_add(&steps, 0x5EE1F820u, 0x1000, 0);
    if (LLVMGetNamedFunction(steps.ir->mod, "snx_fp_unary")) {
        llvm_steps_end(&steps);
        return snx_error("FRECPX still contains a lane helper");
    }
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    int failed = 0;
    for (unsigned form = 0; form < 2 && !failed; ++form) {
        unsigned width = form ? 64 : 32, fraction_bits = form ? 52 : 23, emax = form ? 2047 : 255;
        u64 quiet = 1ull << (fraction_bits - 1), fraction = (1ull << fraction_bits) - 1;
        const u64 fractions[] = {0, 1, quiet - 1, quiet, quiet + 1, fraction};
        for (unsigned exponent = 0; exponent <= emax && !failed; ++exponent)
            for (unsigned f = 0; f < sizeof fractions / sizeof fractions[0] && !failed; ++f)
                for (unsigned mode = 0; mode < 32 && !failed; ++mode) {
                    u64 bits = (u64)(mode & 1) << (width - 1) | (u64)exponent << fraction_bits | fractions[f];
                    u32 fpcr = ((mode >> 1) & 3) << 22 | ((mode >> 3) & 1) << 24 | ((mode >> 4) & 1) << 25;
                    failed = check(steps.steps[form], width, bits, fpcr, (exponent & 1) ? 0x0800009fu : 0);
                    ++*count;
                }
    }
    llvm_steps_end(&steps);
    return failed;
}

int main(void) {
    unsigned long long count = 0;
    for (int jit = 0; jit < 2; ++jit)
        for (unsigned level = 0; level < 2; ++level)
            if (run(jit, level ? '2' : '0', &count)) return 1;
    printf("FRECPX: %llu exact cases\n", count);
    return 0;
}
