/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Every scalar/vector FRINT mode, signed zero, NaN payload, input flush and sticky FPSR through both loaders. */
#include "llvm_steps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { u32 word; unsigned width, lanes, mode, exact; } Insn;
static const Insn instructions[] = {
    {0x1E244020u, 32, 1, 0, 0},
    {0x1E24C020u, 32, 1, 1, 0},
    {0x1E254020u, 32, 1, 2, 0},
    {0x1E25C020u, 32, 1, 3, 0},
    {0x1E264020u, 32, 1, 4, 0},
    {0x1E274020u, 32, 1, 5, 1},
    {0x1E27C020u, 32, 1, 5, 0},
    {0x1E644020u, 64, 1, 0, 0},
    {0x1E64C020u, 64, 1, 1, 0},
    {0x1E654020u, 64, 1, 2, 0},
    {0x1E65C020u, 64, 1, 3, 0},
    {0x1E664020u, 64, 1, 4, 0},
    {0x1E674020u, 64, 1, 5, 1},
    {0x1E67C020u, 64, 1, 5, 0},
    {0x0E218820u, 32, 2, 0, 0},
    {0x0EA18820u, 32, 2, 1, 0},
    {0x0E219820u, 32, 2, 2, 0},
    {0x0EA19820u, 32, 2, 3, 0},
    {0x2E218820u, 32, 2, 4, 0},
    {0x2E219820u, 32, 2, 5, 1},
    {0x2EA19820u, 32, 2, 5, 0},
    {0x4E218820u, 32, 4, 0, 0},
    {0x4EA18820u, 32, 4, 1, 0},
    {0x4E219820u, 32, 4, 2, 0},
    {0x4EA19820u, 32, 4, 3, 0},
    {0x6E218820u, 32, 4, 4, 0},
    {0x6E219820u, 32, 4, 5, 1},
    {0x6EA19820u, 32, 4, 5, 0},
    {0x4E618820u, 64, 2, 0, 0},
    {0x4EE18820u, 64, 2, 1, 0},
    {0x4E619820u, 64, 2, 2, 0},
    {0x4EE19820u, 64, 2, 3, 0},
    {0x6E618820u, 64, 2, 4, 0},
    {0x6E619820u, 64, 2, 5, 1},
    {0x6EE19820u, 64, 2, 5, 0},
};
static u64 random_next(u64* state) { *state ^= *state << 13; *state ^= *state >> 7; *state ^= *state << 17; return *state; }
static const u64 edge64[] = {0, 0x8000000000000000ull, 1, 0x8000000000000001ull, 0x000fffffffffffffull,
    0x0010000000000000ull, 0x3fd0000000000000ull, 0xbfd0000000000000ull, 0x3fe0000000000000ull,
    0xbfe0000000000000ull, 0x3ff8000000000000ull, 0xbff8000000000000ull, 0x4004000000000000ull,
    0x4330000000000000ull, 0x7fefffffffffffffull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
    0x7ff8123456789abcull, 0xfff0123456789abcull};
static const u32 edge32[] = {0, 0x80000000u, 1, 0x80000001u, 0x007fffffu, 0x00800000u,
    0x3e800000u, 0xbe800000u, 0x3f000000u, 0xbf000000u, 0x3fc00000u, 0xbfc00000u,
    0x40200000u, 0x4b000000u, 0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc12345u, 0xff812345u};
static int check(LlvmStep step, const Insn* insn, const CPU* input) {
    CPU actual = *input, expected = *input;
    expected.v[0] = (V128){0};
    for (unsigned lane = 0; lane < insn->lanes; ++lane) {
        u64 bits = insn->width == 32 ? input->v[1].s[lane] : input->v[1].d[lane];
        int mode = insn->mode == 5 ? (int)((input->fpcr >> 22) & 3) : (int)insn->mode;
        u64 result = snx_fp_round(&expected, bits, (int)insn->width, mode, (int)insn->exact);
        if (insn->width == 32) expected.v[0].s[lane] = (u32)result;
        else expected.v[0].d[lane] = result;
    }
    if (step(&actual) == 0x1004 && !memcmp(&actual, &expected, sizeof actual)) return 0;
    fprintf(stderr, "FRINT %08x FPCR %x input %llx result %llx/%llx flags %x/%x\n", insn->word, input->fpcr,
            (unsigned long long)input->v[1].d[0], (unsigned long long)actual.v[0].d[0],
            (unsigned long long)expected.v[0].d[0], actual.fpsr, expected.fpsr);
    return 1;
}
static int run(int jit, int portable, char opt, unsigned long long* checks) {
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps,jit,portable,opt,error,sizeof error)) return snx_error("%s",error);
    for (unsigned i = 0; i < sizeof instructions / sizeof instructions[0]; ++i)
        llvm_steps_add(&steps,instructions[i].word,0x1000,0);
    if (llvm_steps_load(&steps,error,sizeof error)) return snx_error("%s",error);
    u64 rng = 0x49b2c075136a8fedull;
    int failed = 0;
    for (unsigned i = 0; i < sizeof instructions / sizeof instructions[0] && !failed; ++i) {
        const Insn* insn = &instructions[i];
        for (unsigned mode = 0; mode < 32 && !failed; ++mode)
            for (unsigned edge = 0; edge < sizeof edge32 / sizeof edge32[0] && !failed; ++edge)
                for (unsigned lane = 0; lane < insn->lanes && !failed; ++lane) {
                    CPU c = {0}; c.pc = 0x1000;
                    c.fpcr = (mode & 3) << 22 | ((mode >> 2) & 1) << 24 | ((mode >> 3) & 1) << 25;
                    c.fpsr = mode & 16 ? 0x0800009fu : 0;
                    for (unsigned reg = 0; reg < 32; ++reg)
                        for (unsigned h = 0; h < 2; ++h) c.v[reg].d[h] = random_next(&rng);
                    if (insn->width == 32) c.v[1].s[lane] = edge32[edge];
                    else c.v[1].d[lane] = edge64[edge];
                    failed = check(steps.steps[i],insn,&c); ++*checks;
                }
        for (unsigned sample = 0; sample < 4096 && !failed; ++sample) {
            CPU c = {0}; c.pc = 0x1000;
            c.fpcr = (sample & 3) << 22 | ((sample >> 2) & 1) << 24 | ((sample >> 3) & 1) << 25;
            c.fpsr = sample & 16 ? 0x0800009fu : 0;
            for (unsigned reg = 0; reg < 32; ++reg)
                for (unsigned h = 0; h < 2; ++h) c.v[reg].d[h] = random_next(&rng);
            failed = check(steps.steps[i],insn,&c); ++*checks;
        }
    }
    llvm_steps_end(&steps); return failed;
}
static int bench(unsigned index,char opt,int reference, const char* dump) {
    if (index >= sizeof instructions / sizeof instructions[0]) return 2;
    LlvmSteps steps; char error[1024];
    if (llvm_steps_begin_opt(&steps,1,reference,opt,error,sizeof error)) return snx_error("%s",error);
    llvm_steps_add(&steps,instructions[index].word,0x1000,0);
    if (dump) {
        int failed=llvm_steps_dump(&steps,dump,error,sizeof error);
        if(failed)fprintf(stderr,"%s\n",error);
        llvm_steps_end(&steps);return failed!=0;
    }
    if (llvm_steps_load(&steps,error,sizeof error)) return snx_error("%s",error);
    CPU c = {0}; c.pc = 0x1000; c.fpcr = 1u << 24;
    if (instructions[index].width == 32) c.v[1] = (V128){.s = {0x3fc00000u,0xc0200000u,0x3eaaaaabu,0xbf000000u}};
    else c.v[1] = (V128){.d = {0x3ff8000000000000ull,0xc004000000000000ull}};
    u64 checksum = 0; double begin = snx_seconds();
    for (unsigned i=0;i<1000000;++i) { steps.steps[0](&c); checksum += c.v[0].d[0] + (c.v[0].d[1] ^ c.fpsr); }
    double elapsed = snx_seconds()-begin;
    printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n",elapsed*1e9,(unsigned long long)checksum);
    llvm_steps_end(&steps); return 0;
}
int main(int argc,char**argv) {
    if (argc >= 3 && !strcmp(argv[1],"--bench"))
        return bench((unsigned)strtoul(argv[2],NULL,10),argc>3?argv[3][0]:'0',argc>4&&!strcmp(argv[4],"--reference"),argc>5?argv[5]:NULL);
    unsigned long long checks = 0;
    for(int jit=0;jit<2;++jit) for(int portable=0;portable<2;++portable) for(unsigned opt=0;opt<2;++opt)
        if(run(jit,portable,opt?'2':'0',&checks)) return 1;
    printf("FRINT: %llu exact cases\n",checks);return 0;
}
