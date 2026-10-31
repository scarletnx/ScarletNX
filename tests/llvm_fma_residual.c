/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Scalar and vector fused arithmetic preserves result bits and FPSR across exact/inexact residual cases. */
#include "llvm_steps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { u32 word; unsigned width, lanes, addreg, negate_add, negate_n; } Insn;
static const Insn instructions[] = {
    {0x1F020C20u,32,1,3,0,0},{0x1F028C20u,32,1,3,0,1},
    {0x1F220C20u,32,1,3,1,1},{0x1F228C20u,32,1,3,1,0},
    {0x1F420C20u,64,1,3,0,0},{0x1F428C20u,64,1,3,0,1},
    {0x1F620C20u,64,1,3,1,1},{0x1F628C20u,64,1,3,1,0},
    {0x4E22CC20u,32,4,0,0,0},{0x4EA2CC20u,32,4,0,0,1},
    {0x0E22CC20u,32,2,0,0,0},{0x0EA2CC20u,32,2,0,0,1},
    {0x4E62CC20u,64,2,0,0,0},{0x4EE2CC20u,64,2,0,0,1},
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
        u64 add = insn->width == 32 ? input->v[insn->addreg].s[lane] : input->v[insn->addreg].d[lane];
        if (insn->negate_add) add ^= 1ull << (insn->width - 1);
        if (insn->negate_n) a ^= 1ull << (insn->width - 1);
        u64 result = snx_fp_fma(&expected, add, a, b, (int)insn->width);
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
    }
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    u64 rng = 0x9e3779b97f4a7c15ull;
    static const u32 sticky[] = {0, 0x10, 0x80, 0x0800009f};
    int failed = 0;
    for (unsigned i = 0; i < sizeof instructions / sizeof instructions[0] && !failed; ++i) {
        const Insn* insn = &instructions[i];
        static const u64 triples64[][3] = {
            {0x3ff0000002000000ull,0x3feffffffc000000ull,0xbff0000000000000ull},
            {0x3ff0000000000001ull,0x3ff0000000000001ull,0xbff0000000000000ull},
            {0x7fefffffffffffffull,0x4000000000000000ull,0xffefffffffffffffull},
            {1,0x3fe0000000000000ull,0x3ff0000000000000ull},
        };
        static const u32 triples32[][3] = {
            {0x3f800800u,0x3f7ff000u,0xbf800000u},
            {0x3f800001u,0x3f800001u,0xbf800000u},
            {0x7f7fffffu,0x40000000u,0xff7fffffu},
            {1,0x3f000000u,0x3f800000u},
        };
        for(unsigned triple=0;triple<4&&!failed;++triple)
            for(unsigned mode=0;mode<32&&!failed;++mode) {
                CPU cpu={0};cpu.pc=0x1000;
                cpu.fpcr=(mode&3)<<22|((mode>>2)&1)<<24|((mode>>3)&1)<<25;
                cpu.fpsr=mode&16?0x0800009fu:0;
                for(unsigned lane=0;lane<insn->lanes;++lane) {
                    if(insn->width==32) {
                        cpu.v[1].s[lane]=triples32[triple][0];cpu.v[2].s[lane]=triples32[triple][1];
                        cpu.v[insn->addreg].s[lane]=triples32[triple][2];
                    } else {
                        cpu.v[1].d[lane]=triples64[triple][0];cpu.v[2].d[lane]=triples64[triple][1];
                        cpu.v[insn->addreg].d[lane]=triples64[triple][2];
                    }
                }
                failed=check(steps.steps[i],insn,&cpu);++*checked;
            }
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
                                cpu.v[insn->addreg].s[l] = 0x3eaaaaabu;
                            } else {
                                cpu.v[1].d[l] = 0x3ff8000000000000ull;
                                cpu.v[2].d[l] = 0x3fb999999999999aull;
                                cpu.v[insn->addreg].d[l] = 0x3fd5555555555555ull;
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
        for (unsigned n = 0; n < 32768 && !failed; ++n) {
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

static int bench(const char* workload,char opt,const char* dump) {
    unsigned index = strstr(workload,"vector") ? (strstr(workload,"64")?12:8) : (strstr(workload,"64")?4:0);
    const Insn* insn=&instructions[index];
    LlvmSteps steps;char error[1024];
    if(llvm_steps_begin_opt(&steps,1,0,opt,error,sizeof error))return snx_error("%s",error);
    llvm_steps_add(&steps,insn->word,0x1000,0);
    if(dump){int rc=llvm_steps_dump(&steps,dump,error,sizeof error);llvm_steps_end(&steps);return rc!=0;}
    if(llvm_steps_load(&steps,error,sizeof error))return snx_error("%s",error);
    CPU c={0};c.pc=0x1000;c.fpcr=1u<<24;
    for(unsigned lane=0;lane<insn->lanes;++lane) {
        if(insn->width==32){c.v[1].s[lane]=0x3fc00000;c.v[2].s[lane]=0x3dcccccdu;c.v[insn->addreg].s[lane]=0x3eaaaaabu;}
        else{c.v[1].d[lane]=0x3ff8000000000000ull;c.v[2].d[lane]=0x3fb999999999999aull;c.v[insn->addreg].d[lane]=0x3fd5555555555555ull;}
    }
    CPU initial=c;u64 checksum=0;double begin=snx_seconds();
    for(unsigned i=0;i<1000000;++i) {
        c.v[insn->addreg]=initial.v[insn->addreg];c.fpsr=strstr(workload,"sticky")?0x10:0;
        steps.steps[0](&c);checksum+=c.v[0].d[0]+(c.v[0].d[1]^c.fpsr);
    }
    double elapsed=snx_seconds()-begin;
    printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n",elapsed*1e9,(unsigned long long)checksum);
    llvm_steps_end(&steps);return 0;
}
int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--bench"))
        return bench(argv[2], argc > 3 ? argv[3][0] : '0', argc > 4 ? argv[4] : NULL);
    unsigned long long checked = 0;
    for (int jit = 0; jit < 2; ++jit)
        for (int portable = 0; portable < 2; ++portable)
            for (unsigned level = 0; level < 2; ++level)
                if (run(jit, portable, level ? '2' : '0', &checked)) return 1;
    printf("FMA residuals: %llu exact cases\n", checked);
    return 0;
}
