/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Every scalar/vector FRINT mode, signed zero, NaN payload, input flush and sticky FPSR through both loaders. */
#include "llvm_steps.h"
#include "common/cpu/aarch64/a64_decoder.h"
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
    {0x4E798820u, 16, 8, 0, 0},
    {0x4EF98820u, 16, 8, 1, 0},
    {0x4E799820u, 16, 8, 2, 0},
    {0x4EF99820u, 16, 8, 3, 0},
    {0x6E798820u, 16, 8, 4, 0},
    {0x6E799820u, 16, 8, 5, 1},
    {0x6EF99820u, 16, 8, 5, 0},
    {0x0E798820u, 16, 4, 0, 0},
    {0x0EF98820u, 16, 4, 1, 0},
    {0x0E799820u, 16, 4, 2, 0},
    {0x0EF99820u, 16, 4, 3, 0},
    {0x2E798820u, 16, 4, 4, 0},
    {0x2E799820u, 16, 4, 5, 1},
    {0x2EF99820u, 16, 4, 5, 0},
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
static const u16 edge16[] = {0,0x8000,1,0x8001,0x03ff,0x0400,0x3400,0xb400,0x3800,0xb800,0x3e00,0xbe00,
    0x4100,0x6400,0x7bff,0x7c00,0xfc00,0x7e7b,0xfc7b};
static int check(LlvmStep step, const Insn* insn, const CPU* input, unsigned rd) {
    CPU actual = *input, expected = *input;
    expected.v[rd] = (V128){0};
    for (unsigned lane = 0; lane < insn->lanes; ++lane) {
        u64 bits = insn->width == 16 ? input->v[1].h[lane] : insn->width == 32 ? input->v[1].s[lane] : input->v[1].d[lane];
        int mode = insn->mode == 5 ? (int)((input->fpcr >> 22) & 3) : (int)insn->mode;
        u64 result = snx_fp_round_c(&expected, bits, (int)insn->width, mode, (int)insn->exact);
        if (insn->width == 16) expected.v[rd].h[lane] = (u16)result;
        else if (insn->width == 32) expected.v[rd].s[lane] = (u32)result;
        else expected.v[rd].d[lane] = result;
    }
    if (step(&actual) == 0x1004 && !memcmp(&actual, &expected, sizeof actual)) return 0;
    fprintf(stderr, "FRINT %08x rd%u FPCR %x input %llx result %llx/%llx flags %x/%x\n", insn->word, rd, input->fpcr,
            (unsigned long long)input->v[1].d[0], (unsigned long long)actual.v[rd].d[0],
            (unsigned long long)expected.v[rd].d[0], actual.fpsr, expected.fpsr);
    return 1;
}
static int run(int jit, int portable, char opt, unsigned long long* checks) {
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps,jit,portable,opt,error,sizeof error)) return snx_error("%s",error);
    for (unsigned i = 0; i < sizeof instructions / sizeof instructions[0]; ++i)
        for(unsigned rd=0;rd<2;rd++)llvm_steps_add(&steps,instructions[i].word|rd,0x1000,SNX_A64_TARGET_MAX);
    if (llvm_steps_load(&steps,error,sizeof error)) return snx_error("%s",error);
    u64 rng = 0x49b2c075136a8fedull;
    int failed = 0;
    for (unsigned i = 0; i < sizeof instructions / sizeof instructions[0] && !failed; ++i) {
        const Insn* insn = &instructions[i];
        for (unsigned mode = 0; mode < 64 && !failed; ++mode)
            for (unsigned edge = 0; edge < sizeof edge32 / sizeof edge32[0] && !failed; ++edge)
                for (unsigned lane = 0; lane < insn->lanes && !failed; ++lane) {
                    CPU c = {0}; c.pc = 0x1000;
                    c.fpcr = (mode & 3) << 22 | ((mode >> 2) & 1) << 24 | ((mode >> 3) & 1) << 25 | ((mode >> 4) & 1) << 19;
                    c.fpsr = mode & 32 ? 0x0800009fu : 0;
                    for (unsigned reg = 0; reg < 32; ++reg)
                        for (unsigned h = 0; h < 2; ++h) c.v[reg].d[h] = random_next(&rng);
                    if (insn->width == 16) c.v[1].h[lane] = edge16[edge];
                    else if (insn->width == 32) c.v[1].s[lane] = edge32[edge];
                    else c.v[1].d[lane] = edge64[edge];
                    for(unsigned rd=0;rd<2&&!failed;rd++){failed = check(steps.steps[2*i+rd],insn,&c,rd); ++*checks;}
                }
        for (unsigned sample = 0; sample < (insn->width == 16 ? 65536u : 4096u) && !failed; ++sample) {
            CPU c = {0}; c.pc = 0x1000;
            c.fpcr = (sample & 3) << 22 | ((sample >> 2) & 1) << 24 | ((sample >> 3) & 1) << 25 | ((sample >> 4) & 1) << 19;
            c.fpsr = sample & 32 ? 0x0800009fu : 0;
            for (unsigned reg = 0; reg < 32; ++reg)
                for (unsigned h = 0; h < 2; ++h) c.v[reg].d[h] = random_next(&rng);
            if (insn->width == 16) for (unsigned lane=0;lane<insn->lanes;lane++) c.v[1].h[lane]=(u16)(sample+lane);
            for(unsigned rd=0;rd<2&&!failed;rd++){failed = check(steps.steps[2*i+rd],insn,&c,rd); ++*checks;}
        }
    }
    llvm_steps_end(&steps); return failed;
}
static int bench(unsigned index,char opt,int reference, const char* dump, unsigned pattern) {
    if (index >= sizeof instructions / sizeof instructions[0] || pattern > 2) return 2;
    LlvmSteps steps; char error[1024];
    if (llvm_steps_begin_opt(&steps,1,reference,opt,error,sizeof error)) return snx_error("%s",error);
    llvm_steps_add(&steps,instructions[index].word,0x1000,SNX_A64_TARGET_MAX);
    if (dump && strcmp(dump,"-")) {
        int failed=llvm_steps_dump(&steps,dump,error,sizeof error);
        if(failed)fprintf(stderr,"%s\n",error);
        llvm_steps_end(&steps);return failed!=0;
    }
    if (llvm_steps_load(&steps,error,sizeof error)) return snx_error("%s",error);
    CPU c = {0}; c.pc = 0x1000; c.fpcr = 1u << 24;
    if (instructions[index].width == 16) c.v[1] = (V128){.h={0x3e00,0xc100,0x3555,0xb800,0x3e00,0xc100,0x3555,0xb800}};
    else if (instructions[index].width == 32) c.v[1] = (V128){.s = {0x3fc00000u,0xc0200000u,0x3eaaaaabu,0xbf000000u}};
    else c.v[1] = (V128){.d = {0x3ff8000000000000ull,0xc004000000000000ull}};
    if (pattern) {
        if (instructions[index].width == 16) c.v[1].h[0]=0x7c01;
        else if (instructions[index].width == 32) c.v[1].s[0]=0x7f800001u;
        else c.v[1].d[0]=0x7ff0000000000001ull;
    }
    if (pattern == 2) {
        if (instructions[index].width == 16) for(unsigned lane=0;lane<8;lane++)c.v[1].h[lane]=(u16)(lane+1);
        else if (instructions[index].width == 32) for(unsigned lane=0;lane<4;lane++)c.v[1].s[lane]=lane+1;
        else c.v[1]=(V128){.d={1,2}};
        c.fpcr |= 1u<<19;
    }
    u64 checksum = 0; double begin = snx_seconds();
    for (unsigned i=0;i<1000000;++i) { steps.steps[0](&c); checksum += c.v[0].d[0] + (c.v[0].d[1] ^ c.fpsr); }
    double elapsed = snx_seconds()-begin;
    printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n",elapsed*1e9,(unsigned long long)checksum);
    llvm_steps_end(&steps); return 0;
}
int main(int argc,char**argv) {
    if (argc >= 3 && !strcmp(argv[1],"--bench"))
        return bench((unsigned)strtoul(argv[2],NULL,10),argc>3?argv[3][0]:'0',argc>4&&!strcmp(argv[4],"--reference"),argc>5?argv[5]:NULL,argc>6?(unsigned)strtoul(argv[6],NULL,10):0);
    unsigned long long checks = 0;
    for(int jit=0;jit<2;++jit) for(int portable=0;portable<2;++portable) for(unsigned opt=0;opt<2;++opt)
        if(run(jit,portable,opt?'2':'0',&checks)) return 1;
    printf("FRINT: %llu exact cases\n",checks);return 0;
}
