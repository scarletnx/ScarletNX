/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vector arithmetic's residuals preserve results and sticky FPSR, at O0 and O2, through both loaders. */
#include "llvm_steps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { u32 word; unsigned op, width, lanes; } Insn;
static const Insn instructions[] = {
    {0x4E22D420u, SNX_FP_ADD, 32, 4}, {0x4EA2D420u, SNX_FP_SUB, 32, 4},
    {0x6E22DC20u, SNX_FP_MUL, 32, 4}, {0x6E22FC20u, SNX_FP_DIV, 32, 4},
    {0x0E22D420u, SNX_FP_ADD, 32, 2}, {0x0EA2D420u, SNX_FP_SUB, 32, 2},
    {0x2E22DC20u, SNX_FP_MUL, 32, 2}, {0x2E22FC20u, SNX_FP_DIV, 32, 2},
    {0x4E62D420u, SNX_FP_ADD, 64, 2}, {0x4EE2D420u, SNX_FP_SUB, 64, 2},
    {0x6E62DC20u, SNX_FP_MUL, 64, 2}, {0x6E62FC20u, SNX_FP_DIV, 64, 2},
    {0x4E22DC20u, SNX_FP_MULX, 32, 4}, {0x0E22DC20u, SNX_FP_MULX, 32, 2},
    {0x4E62DC20u, SNX_FP_MULX, 64, 2},
    {0x6EA2D420u, SNX_FP_ABD, 32, 4}, {0x2EA2D420u, SNX_FP_ABD, 32, 2},
    {0x6EE2D420u, SNX_FP_ABD, 64, 2}, {0x7EA2D420u, SNX_FP_ABD, 32, 1},
    {0x7EE2D420u, SNX_FP_ABD, 64, 1},
};
static const u64 edges[] = {
    0, 0x8000000000000000ull, 1, 0x8000000000000001ull,
    0x000fffffffffffffull, 0x0010000000000000ull, 0x3ff0000000000000ull,
    0x3fb999999999999aull, 0xbfb999999999999aull, 0x7fefffffffffffffull,
    0xffefffffffffffffull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
    0x7ff8123456789abcull, 0xfff8123456789abcull, 0x7ff0123456789abcull,
};
static const u32 edges32[] = {
    0, 0x80000000u, 1, 0x80000001u, 0x007fffffu, 0x00800000u,
    0x3f800000u, 0x3dcccccdu, 0xbdcccccdu, 0x7f7fffffu, 0xff7fffffu,
    0x7f800000u, 0xff800000u, 0x7fc12345u, 0xffc12345u, 0x7f812345u,
};

static u64 random_next(u64* state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}
static int check(LlvmStep step, const Insn* insn, const CPU* input) {
    CPU expected = *input, actual = *input;
    expected.v[0] = (V128){0};
    for (unsigned lane = 0; lane < insn->lanes; ++lane) {
        u64 a = insn->width == 32 ? input->v[1].s[lane] : input->v[1].d[lane];
        u64 b = insn->width == 32 ? input->v[2].s[lane] : input->v[2].d[lane];
        u64 result = snx_fp_binary(&expected, a, b, (int)insn->width, (int)insn->op);
        if (insn->width == 32) expected.v[0].s[lane] = (u32)result;
        else expected.v[0].d[lane] = result;
    }
    u64 next = step(&actual);
    if (next == 0x1004 && !memcmp(&actual, &expected, sizeof actual)) return 0;
    fprintf(stderr, "word %08x FPCR %08x FPSR %08x: got %016llx/%016llx flags %08x, "
                    "expected %016llx/%016llx flags %08x\n", insn->word, input->fpcr, input->fpsr,
            (unsigned long long)actual.v[0].d[0], (unsigned long long)actual.v[0].d[1], actual.fpsr,
            (unsigned long long)expected.v[0].d[0], (unsigned long long)expected.v[0].d[1], expected.fpsr);
    return 1;
}
static int run(int jit, int portable, char opt, unsigned long long* checked) {
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps, jit, portable, opt, error, sizeof error)) return snx_error("%s", error);
    for (unsigned i = 0; i < sizeof instructions / sizeof instructions[0]; ++i) {
        llvm_steps_add(&steps, instructions[i].word, 0x1000, 0);
        if (instructions[i].width != 32 || instructions[i].op == SNX_FP_ADD ||
            instructions[i].op == SNX_FP_SUB) continue;
        for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(steps.fns[i]); block;
             block = LLVMGetNextBasicBlock(block))
            for (LLVMValueRef inst = LLVMGetFirstInstruction(block); inst; inst = LLVMGetNextInstruction(inst))
                if (LLVMGetInstructionOpcode(inst) == LLVMFPExt && strcmp(LLVMGetBasicBlockName(block), "vfp_ixc"))
                    return snx_error("a residual's widening escaped the IXC-clear block");
    }
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    u64 rng = 0x9e3779b97f4a7c15ull;
    static const u32 sticky[] = {0, 0x10, 0x80, 0x0800009f};
    int failed = 0;
    for (unsigned i = 0; i < sizeof instructions / sizeof instructions[0] && !failed; ++i) {
        const Insn* insn = &instructions[i];
        for (unsigned mode = 0; mode < 32 && !failed; ++mode)
            for (unsigned flags = 0; flags < sizeof sticky / sizeof sticky[0] && !failed; ++flags)
                for (unsigned edge = 0; edge < 16 && !failed; ++edge)
                    for (unsigned lane = 0; lane < insn->lanes && !failed; ++lane) {
                        CPU cpu = {0};
                        cpu.pc = 0x1000;
                        cpu.fpcr = (mode & 3) << 22 | ((mode >> 2) & 1) << 24 |
                                   ((mode >> 3) & 1) << 25 | ((mode >> 4) & 1) << 19;
                        cpu.fpsr = sticky[flags];
                        for (unsigned v = 0; v < 32; ++v)
                            for (unsigned half = 0; half < 2; ++half) cpu.v[v].d[half] = random_next(&rng);
                        for (unsigned l = 0; l < insn->lanes; ++l) {
                            if (insn->width == 32) {
                                cpu.v[1].s[l] = 0x3fc00000u;
                                cpu.v[2].s[l] = 0x3dcccccdu;
                            } else {
                                cpu.v[1].d[l] = 0x3ff8000000000000ull;
                                cpu.v[2].d[l] = 0x3fb999999999999aull;
                            }
                        }
                        if (insn->width == 32) cpu.v[1].s[lane] = edges32[edge];
                        else cpu.v[1].d[lane] = edges[edge];
                        failed = check(steps.steps[i], insn, &cpu);
                        ++*checked;
                        if (insn->width == 32) cpu.v[2].s[(lane + 1) % insn->lanes] = edges32[(edge + 5) % 16];
                        else cpu.v[2].d[(lane + 1) % insn->lanes] = edges[(edge + 5) % 16];
                        if (!failed) { failed = check(steps.steps[i], insn, &cpu); ++*checked; }
                    }
        for (unsigned n = 0; n < 4096 && !failed; ++n) {
            CPU cpu = {0};
            cpu.pc = 0x1000;
            cpu.fpcr = (n & 3) << 22 | ((n >> 2) & 1) << 24;
            cpu.fpsr = sticky[(n >> 3) & 3];
            for (unsigned v = 0; v < 32; ++v)
                for (unsigned half = 0; half < 2; ++half) cpu.v[v].d[half] = random_next(&rng);
            failed = check(steps.steps[i], insn, &cpu);
            ++*checked;
        }
    }
    llvm_steps_end(&steps);
    return failed;
}

static int bench(const char* workload, char opt, const char* dump) {
    unsigned index = !strncmp(workload, "abd", 3) ? 15 : !strncmp(workload, "add", 3) ? 0 : !strncmp(workload, "sub", 3) ? 1 :
                     !strncmp(workload, "div", 3) ? 3 : 2;
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps, 1, 0, opt, error, sizeof error)) return snx_error("%s", error);
    llvm_steps_add(&steps, instructions[index].word, 0x1000, 0);
    if (dump) {
        int failed = llvm_steps_dump(&steps, dump, error, sizeof error);
        if (failed) fprintf(stderr, "%s\n", error);
        llvm_steps_end(&steps);
        return failed != 0;
    }
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    CPU cpu = {0};
    cpu.pc = 0x1000;
    cpu.fpcr = 1u << 24;
    cpu.fpsr = 0x10;
    for (unsigned lane = 0; lane < 4; ++lane) {
        cpu.v[1].s[lane] = 0x3fc00000u;
        cpu.v[2].s[lane] = 0x3dcccccdu;
    }
    if (strstr(workload, "mixed")) cpu.v[1].s[2] = 0x7f812345u;
    if (strstr(workload, "fallback")) cpu.fpcr |= 1u << 22;
    int clear = strstr(workload, "clear") != NULL;
    u64 checksum = 0;
    double begin = snx_seconds();
    for (unsigned n = 0; n < 2000000; ++n) {
        if (clear) cpu.fpsr = 0;
        steps.steps[0](&cpu);
        checksum += cpu.v[0].d[0] ^ cpu.v[0].d[1] ^ cpu.fpsr;
    }
    double elapsed = snx_seconds() - begin;
    printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n", elapsed * 1e9, (unsigned long long)checksum);
    llvm_steps_end(&steps);
    return 0;
}
int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--bench"))
        return bench(argv[2], argc > 3 ? argv[3][0] : '0', argc > 4 ? argv[4] : NULL);
    unsigned long long checked = 0;
    for (int jit = 0; jit < 2; ++jit)
        for (int portable = 0; portable < 2; ++portable)
            for (unsigned level = 0; level < 2; ++level)
                if (run(jit, portable, level ? '2' : '0', &checked)) return 1;
    printf("Vector FP residuals: %llu exact cases\n", checked);
    return 0;
}
