/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Whole-vector comparisons against independent soft-float unpack/order, with exact sticky FPSR. */
#include "llvm_steps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { u32 word; unsigned width, lanes, cmp, absolute, zero, swap; } Insn;
static const Insn instructions[] = {
    {0x4e422420u, 16, 8, 0, 0, 0, 0}, /* fcmeq v0.8h,v1.8h,v2.8h */
    {0x6e422420u, 16, 8, 1, 0, 0, 0}, /* fcmge v0.8h,v1.8h,v2.8h */
    {0x6ec22420u, 16, 8, 2, 0, 0, 0}, /* fcmgt v0.8h,v1.8h,v2.8h */
    {0x6e422c20u, 16, 8, 1, 1, 0, 0}, /* facge v0.8h,v1.8h,v2.8h */
    {0x6ec22c20u, 16, 8, 2, 1, 0, 0}, /* facgt v0.8h,v1.8h,v2.8h */
    {0x4ef8d820u, 16, 8, 0, 0, 1, 0}, /* fcmeq v0.8h,v1.8h,#0.0 */
    {0x6ef8c820u, 16, 8, 1, 0, 1, 0}, /* fcmge v0.8h,v1.8h,#0.0 */
    {0x4ef8c820u, 16, 8, 2, 0, 1, 0}, /* fcmgt v0.8h,v1.8h,#0.0 */
    {0x6ef8d820u, 16, 8, 1, 0, 1, 1}, /* fcmle v0.8h,v1.8h,#0.0 */
    {0x4ef8e820u, 16, 8, 2, 0, 1, 1}, /* fcmlt v0.8h,v1.8h,#0.0 */
    {0x0e422420u, 16, 4, 0, 0, 0, 0}, /* fcmeq v0.4h,v1.4h,v2.4h */
    {0x2e422420u, 16, 4, 1, 0, 0, 0}, /* fcmge v0.4h,v1.4h,v2.4h */
    {0x2ec22420u, 16, 4, 2, 0, 0, 0}, /* fcmgt v0.4h,v1.4h,v2.4h */
    {0x2e422c20u, 16, 4, 1, 1, 0, 0}, /* facge v0.4h,v1.4h,v2.4h */
    {0x2ec22c20u, 16, 4, 2, 1, 0, 0}, /* facgt v0.4h,v1.4h,v2.4h */
    {0x0ef8d820u, 16, 4, 0, 0, 1, 0}, /* fcmeq v0.4h,v1.4h,#0.0 */
    {0x2ef8c820u, 16, 4, 1, 0, 1, 0}, /* fcmge v0.4h,v1.4h,#0.0 */
    {0x0ef8c820u, 16, 4, 2, 0, 1, 0}, /* fcmgt v0.4h,v1.4h,#0.0 */
    {0x2ef8d820u, 16, 4, 1, 0, 1, 1}, /* fcmle v0.4h,v1.4h,#0.0 */
    {0x0ef8e820u, 16, 4, 2, 0, 1, 1}, /* fcmlt v0.4h,v1.4h,#0.0 */
    {0x5e422420u, 16, 1, 0, 0, 0, 0}, /* fcmeq h0,h1,h2 */
    {0x7e422420u, 16, 1, 1, 0, 0, 0}, /* fcmge h0,h1,h2 */
    {0x7ec22420u, 16, 1, 2, 0, 0, 0}, /* fcmgt h0,h1,h2 */
    {0x7e422c20u, 16, 1, 1, 1, 0, 0}, /* facge h0,h1,h2 */
    {0x7ec22c20u, 16, 1, 2, 1, 0, 0}, /* facgt h0,h1,h2 */
    {0x5ef8d820u, 16, 1, 0, 0, 1, 0}, /* fcmeq h0,h1,#0.0 */
    {0x7ef8c820u, 16, 1, 1, 0, 1, 0}, /* fcmge h0,h1,#0.0 */
    {0x5ef8c820u, 16, 1, 2, 0, 1, 0}, /* fcmgt h0,h1,#0.0 */
    {0x7ef8d820u, 16, 1, 1, 0, 1, 1}, /* fcmle h0,h1,#0.0 */
    {0x5ef8e820u, 16, 1, 2, 0, 1, 1}, /* fcmlt h0,h1,#0.0 */
    {0x4e22e420u, 32, 4, 0, 0, 0, 0}, /* fcmeq v0.4s,v1.4s,v2.4s */
    {0x6e22e420u, 32, 4, 1, 0, 0, 0}, /* fcmge v0.4s,v1.4s,v2.4s */
    {0x6ea2e420u, 32, 4, 2, 0, 0, 0}, /* fcmgt v0.4s,v1.4s,v2.4s */
    {0x6e22ec20u, 32, 4, 1, 1, 0, 0}, /* facge v0.4s,v1.4s,v2.4s */
    {0x6ea2ec20u, 32, 4, 2, 1, 0, 0}, /* facgt v0.4s,v1.4s,v2.4s */
    {0x4ea0d820u, 32, 4, 0, 0, 1, 0}, /* fcmeq v0.4s,v1.4s,#0.0 */
    {0x6ea0c820u, 32, 4, 1, 0, 1, 0}, /* fcmge v0.4s,v1.4s,#0.0 */
    {0x4ea0c820u, 32, 4, 2, 0, 1, 0}, /* fcmgt v0.4s,v1.4s,#0.0 */
    {0x6ea0d820u, 32, 4, 1, 0, 1, 1}, /* fcmle v0.4s,v1.4s,#0.0 */
    {0x4ea0e820u, 32, 4, 2, 0, 1, 1}, /* fcmlt v0.4s,v1.4s,#0.0 */
    {0x0e22e420u, 32, 2, 0, 0, 0, 0}, /* fcmeq v0.2s,v1.2s,v2.2s */
    {0x2e22e420u, 32, 2, 1, 0, 0, 0}, /* fcmge v0.2s,v1.2s,v2.2s */
    {0x2ea2e420u, 32, 2, 2, 0, 0, 0}, /* fcmgt v0.2s,v1.2s,v2.2s */
    {0x2e22ec20u, 32, 2, 1, 1, 0, 0}, /* facge v0.2s,v1.2s,v2.2s */
    {0x2ea2ec20u, 32, 2, 2, 1, 0, 0}, /* facgt v0.2s,v1.2s,v2.2s */
    {0x0ea0d820u, 32, 2, 0, 0, 1, 0}, /* fcmeq v0.2s,v1.2s,#0.0 */
    {0x2ea0c820u, 32, 2, 1, 0, 1, 0}, /* fcmge v0.2s,v1.2s,#0.0 */
    {0x0ea0c820u, 32, 2, 2, 0, 1, 0}, /* fcmgt v0.2s,v1.2s,#0.0 */
    {0x2ea0d820u, 32, 2, 1, 0, 1, 1}, /* fcmle v0.2s,v1.2s,#0.0 */
    {0x0ea0e820u, 32, 2, 2, 0, 1, 1}, /* fcmlt v0.2s,v1.2s,#0.0 */
    {0x5e22e420u, 32, 1, 0, 0, 0, 0}, /* fcmeq s0,s1,s2 */
    {0x7e22e420u, 32, 1, 1, 0, 0, 0}, /* fcmge s0,s1,s2 */
    {0x7ea2e420u, 32, 1, 2, 0, 0, 0}, /* fcmgt s0,s1,s2 */
    {0x7e22ec20u, 32, 1, 1, 1, 0, 0}, /* facge s0,s1,s2 */
    {0x7ea2ec20u, 32, 1, 2, 1, 0, 0}, /* facgt s0,s1,s2 */
    {0x5ea0d820u, 32, 1, 0, 0, 1, 0}, /* fcmeq s0,s1,#0.0 */
    {0x7ea0c820u, 32, 1, 1, 0, 1, 0}, /* fcmge s0,s1,#0.0 */
    {0x5ea0c820u, 32, 1, 2, 0, 1, 0}, /* fcmgt s0,s1,#0.0 */
    {0x7ea0d820u, 32, 1, 1, 0, 1, 1}, /* fcmle s0,s1,#0.0 */
    {0x5ea0e820u, 32, 1, 2, 0, 1, 1}, /* fcmlt s0,s1,#0.0 */
    {0x4e62e420u, 64, 2, 0, 0, 0, 0}, /* fcmeq v0.2d,v1.2d,v2.2d */
    {0x6e62e420u, 64, 2, 1, 0, 0, 0}, /* fcmge v0.2d,v1.2d,v2.2d */
    {0x6ee2e420u, 64, 2, 2, 0, 0, 0}, /* fcmgt v0.2d,v1.2d,v2.2d */
    {0x6e62ec20u, 64, 2, 1, 1, 0, 0}, /* facge v0.2d,v1.2d,v2.2d */
    {0x6ee2ec20u, 64, 2, 2, 1, 0, 0}, /* facgt v0.2d,v1.2d,v2.2d */
    {0x4ee0d820u, 64, 2, 0, 0, 1, 0}, /* fcmeq v0.2d,v1.2d,#0.0 */
    {0x6ee0c820u, 64, 2, 1, 0, 1, 0}, /* fcmge v0.2d,v1.2d,#0.0 */
    {0x4ee0c820u, 64, 2, 2, 0, 1, 0}, /* fcmgt v0.2d,v1.2d,#0.0 */
    {0x6ee0d820u, 64, 2, 1, 0, 1, 1}, /* fcmle v0.2d,v1.2d,#0.0 */
    {0x4ee0e820u, 64, 2, 2, 0, 1, 1}, /* fcmlt v0.2d,v1.2d,#0.0 */
    {0x5e62e420u, 64, 1, 0, 0, 0, 0}, /* fcmeq d0,d1,d2 */
    {0x7e62e420u, 64, 1, 1, 0, 0, 0}, /* fcmge d0,d1,d2 */
    {0x7ee2e420u, 64, 1, 2, 0, 0, 0}, /* fcmgt d0,d1,d2 */
    {0x7e62ec20u, 64, 1, 1, 1, 0, 0}, /* facge d0,d1,d2 */
    {0x7ee2ec20u, 64, 1, 2, 1, 0, 0}, /* facgt d0,d1,d2 */
    {0x5ee0d820u, 64, 1, 0, 0, 1, 0}, /* fcmeq d0,d1,#0.0 */
    {0x7ee0c820u, 64, 1, 1, 0, 1, 0}, /* fcmge d0,d1,#0.0 */
    {0x5ee0c820u, 64, 1, 2, 0, 1, 0}, /* fcmgt d0,d1,#0.0 */
    {0x7ee0d820u, 64, 1, 1, 0, 1, 1}, /* fcmle d0,d1,#0.0 */
    {0x5ee0e820u, 64, 1, 2, 0, 1, 1}, /* fcmlt d0,d1,#0.0 */
};
#define COUNT (sizeof instructions / sizeof *instructions)
static u64 random_next(u64* state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}
static u64 get(const V128* v, unsigned lane, unsigned width) {
    return width == 16 ? SNX_VH(*v, lane) : width == 32 ? SNX_VS(*v, lane) : SNX_VD(*v, lane);
}
static void put(V128* v, unsigned lane, unsigned width, u64 value) {
    if (width == 16) SNX_VSET_H(*v, lane, value);
    else if (width == 32) SNX_VSET_S(*v, lane, value);
    else SNX_VSET_D(*v, lane, value);
}
/* Includes each NaN class/sign, zeros, denormals, normal boundaries and infinities. */
static u64 edge(unsigned index, unsigned width) {
    unsigned fraction = width == 16 ? 10 : width == 32 ? 23 : 52;
    u64 sign = (u64)1 << (width - 1), normal = (u64)1 << fraction;
    u64 infinity = width == 16 ? 0x7c00 : width == 32 ? 0x7f800000 : 0x7ff0000000000000ull;
    u64 values[] = {0, sign, 1, sign | 1, normal - 1, normal, infinity - 1, infinity,
                    sign | infinity, infinity | 1, infinity | (normal >> 1), infinity | normal >> 1 | 1,
                    sign | infinity | 1, sign | infinity | (normal >> 1), sign | normal, normal + 1};
    return values[index & 15];
}
/* Execute the retained endian path on this host with reversed lane storage. This checks its masks and flags,
   not another host's ABI or instruction set. Comparison masks themselves are invariant under byte reversal. */
static void reverse_lanes(CPU* c, unsigned width) {
    unsigned bytes = width / 8;
    for (unsigned reg = 0; reg < 32; reg++)
        for (unsigned at = 0; at < 16; at += bytes)
            for (unsigned i = 0; i < bytes / 2; i++) {
                u8 value = c->v[reg].b[at + i];
                c->v[reg].b[at + i] = c->v[reg].b[at + bytes - i - 1];
                c->v[reg].b[at + bytes - i - 1] = value;
            }
}
static int check(LlvmStep step, const Insn* insn, unsigned rd, const CPU* input, int reversed) {
    CPU expected = *input, actual = *input;
    expected.v[rd] = (V128){0};
    u64 sign = (u64)1 << (insn->width - 1), all = sign | (sign - 1);
    for (unsigned lane = 0; lane < insn->lanes; ++lane) {
        u64 a = get(&input->v[1], lane, insn->width);
        u64 b = insn->zero ? 0 : get(&input->v[2], lane, insn->width);
        if (insn->absolute) { a &= sign - 1; b &= sign - 1; }
        SnxFpReply result = snx_fp_compare(insn->swap ? b : a, insn->swap ? a : b,
                                          (int)insn->width, insn->cmp != 0, input->fpcr);
        expected.fpsr |= result.flags;
        int match = insn->cmp == 0 ? result.value == 6 : insn->cmp == 1 ?
                    result.value == 6 || result.value == 2 : result.value == 2;
        put(&expected.v[rd], lane, insn->width, match ? all : 0);
    }
    if (reversed) reverse_lanes(&actual, insn->width);
    u64 pc = step(&actual);
    if (reversed) reverse_lanes(&actual, insn->width);
    if (pc == 0x1004 && !memcmp(&actual, &expected, sizeof actual)) return 0;
    fprintf(stderr, "comparison %08x rd %u FPCR %08x FPSR %08x: actual %016llx/%016llx %08x, "
                    "expected %016llx/%016llx %08x\n", insn->word, rd, input->fpcr, input->fpsr,
            (unsigned long long)SNX_VD(actual.v[rd], 0), (unsigned long long)SNX_VD(actual.v[rd], 1), actual.fpsr,
            (unsigned long long)SNX_VD(expected.v[rd], 0), (unsigned long long)SNX_VD(expected.v[rd], 1), expected.fpsr);
    return 1;
}
static int run(int jit, int portable, char opt, unsigned long long* checked, int reversed) {
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps, jit, portable, opt, error, sizeof error)) return snx_error("%s", error);
    if (reversed && steps.ir->big_endian) { llvm_steps_end(&steps); return 0; }
    if (reversed) steps.ir->big_endian = 1;
    for (unsigned i = 0; i < COUNT; ++i)
        for (unsigned rd = 0; rd < 3; ++rd)
            llvm_steps_add(&steps, instructions[i].word | rd, 0x1000, SNX_A64_TARGET_MAX);
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    static const u32 sticky[] = {0, 1, 0x80, 0x0800009f};
    u64 random = 0x9e3779b97f4a7c15ull;
    int failed = 0;
    for (unsigned i = 0; i < COUNT && !failed; ++i) {
        const Insn* insn = &instructions[i];
        for (unsigned mode = 0; mode < 64 && !failed; ++mode)
            for (unsigned e = 0; e < 32 && !failed; ++e) {
                CPU input = {0};
                input.pc = 0x1000;
                input.fpcr = (mode & 3) << 22 | ((mode >> 2) & 1) << 24 | ((mode >> 3) & 1) << 19 |
                             ((mode >> 4) & 1) << 25 | ((mode >> 5) & 1) << 26;
                input.fpsr = sticky[e & 3];
                for (unsigned r = 0; r < 32; ++r)
                    for (unsigned half = 0; half < 2; ++half) input.v[r].d[half] = random_next(&random);
                for (unsigned lane = 0; lane < insn->lanes; ++lane) {
                    /* Exceptional lanes move through every position, among ordinary lanes. */
                    put(&input.v[1], lane, insn->width, lane == e % insn->lanes ? edge(e, insn->width) : edge(5, insn->width));
                    put(&input.v[2], lane, insn->width, edge(e / 2 + lane, insn->width));
                }
                for (unsigned rd = 0; rd < 3 && !failed; ++rd) {
                    failed = check(steps.steps[3 * i + rd], insn, rd, &input, reversed);
                    ++*checked;
                }
            }
        for (unsigned n = 0; n < 1024 && !failed; ++n) {
            CPU input = {0};
            input.pc = 0x1000;
            input.fpcr = (n & 1) << 24 | ((n >> 1) & 1) << 19;
            input.fpsr = sticky[(n >> 2) & 3];
            for (unsigned lane = 0; lane < insn->lanes; ++lane) {
                put(&input.v[1], lane, insn->width, random_next(&random));
                put(&input.v[2], lane, insn->width, random_next(&random));
            }
            failed = check(steps.steps[3 * i], insn, 0, &input, reversed);
            ++*checked;
        }
    }
    /* Every half encoding, in both operand positions, including FZ16 and all mixed-vector positions. */
    for (unsigned i = 0; i < 5 && !failed; ++i)
        for (unsigned flush = 0; flush < 2 && !failed; ++flush)
            for (unsigned base = 0; base < 65536 && !failed; base += 8) {
                CPU input = {0};
                input.pc = 0x1000;
                input.fpcr = flush << 19;
                for (unsigned lane = 0; lane < 8; ++lane) {
                    put(&input.v[1], lane, 16, base + lane);
                    put(&input.v[2], lane, 16, edge((base / 8 + lane) & 15, 16));
                }
                failed = check(steps.steps[3 * i], &instructions[i], 0, &input, reversed);
                ++*checked;
                V128 swap = input.v[1]; input.v[1] = input.v[2]; input.v[2] = swap;
                if (!failed) failed = check(steps.steps[3 * i], &instructions[i], 0, &input, reversed);
                ++*checked;
            }
    llvm_steps_end(&steps);
    return failed;
}
static int bench(const char* workload, char opt, const char* dump, int portable) {
    /* FCMEQ and FCMGE: ordinary, a NaN lane, or flushed denormals. */
    unsigned index = strstr(workload, "16") ? 0 : strstr(workload, "64") ? 60 : 30;
    if (strncmp(workload, "eq", 2)) ++index;
    const Insn* insn = &instructions[index];
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps, 1, portable, opt, error, sizeof error)) return snx_error("%s", error);
    llvm_steps_add(&steps, insn->word, 0x1000, SNX_A64_TARGET_MAX);
    if (dump) {
        int result = llvm_steps_dump(&steps, dump, error, sizeof error);
        llvm_steps_end(&steps);
        return result != 0;
    }
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    CPU input = {0};
    input.pc = 0x1000; input.fpcr = (1u << 24) | (1u << 19); input.fpsr = 0x10;
    for (unsigned lane = 0; lane < insn->lanes; ++lane) {
        put(&input.v[1], lane, insn->width, edge(5, insn->width));
        put(&input.v[2], lane, insn->width, edge(6, insn->width));
    }
    if (strstr(workload, "mixed")) put(&input.v[1], insn->lanes / 2, insn->width, edge(9, insn->width));
    if (strstr(workload, "denormal")) put(&input.v[1], insn->lanes / 2, insn->width, 1);
    u64 checksum = 0;
    double start = snx_seconds();
    for (unsigned n = 0; n < 8000000; ++n) {
        steps.steps[0](&input);
        checksum += SNX_VD(input.v[0], 0) ^ SNX_VD(input.v[0], 1) ^ input.fpsr;
    }
    double elapsed = snx_seconds() - start;
    printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n", elapsed * 1e9, (unsigned long long)checksum);
    llvm_steps_end(&steps);
    return 0;
}
int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--bench"))
        return bench(argv[2], argc > 3 ? argv[3][0] : '2',
                     argc > 4 && strcmp(argv[4], "-") ? argv[4] : NULL, argc > 5 ? atoi(argv[5]) : 0);
    unsigned long long checked = 0;
    for (int jit = 0; jit < 2; ++jit)
        for (int portable = 0; portable < 2; ++portable)
            for (unsigned level = 0; level < 2; ++level)
                if (run(jit, portable, level ? '2' : '0', &checked, 0)) return 1;
    if (run(1, LLVM_STEPS_PORTABLE, '0', &checked, 1) ||
        run(1, LLVM_STEPS_PORTABLE, '2', &checked, 1)) return 1;
    printf("Vector comparisons: %llu exact cases\n", checked);
    return 0;
}
