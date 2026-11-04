/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Half/single/double scalar and vector estimates: exponent/table boundaries, exceptional lanes and random bits. */
#include "llvm_steps.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { u32 word; unsigned width, lanes, op; } Insn;
static const Insn instructions[] = {
    {0x4ef9d820u,16,8,SNX_FP_RECPE},{0x0ef9d820u,16,4,SNX_FP_RECPE},{0x5ef9d820u,16,1,SNX_FP_RECPE},
    {0x6ef9d820u,16,8,SNX_FP_RSQRTE},{0x2ef9d820u,16,4,SNX_FP_RSQRTE},{0x7ef9d820u,16,1,SNX_FP_RSQRTE},
    {0x5ef9f820u,16,1,SNX_FP_RECPX},
    {0x5EA1D820u,32,1,SNX_FP_RECPE},{0x5EE1D820u,64,1,SNX_FP_RECPE},{0x4EE1D820u,64,2,SNX_FP_RECPE},
    {0x7EA1D820u,32,1,SNX_FP_RSQRTE},{0x7EE1D820u,64,1,SNX_FP_RSQRTE},{0x6EE1D820u,64,2,SNX_FP_RSQRTE},
};
static u64 random_next(u64* rng) { *rng ^= *rng << 13; *rng ^= *rng >> 7; *rng ^= *rng << 17; return *rng; }
static int check(LlvmStep step,const Insn* insn,const CPU* input) {
    CPU expected = *input, actual = *input;
    expected.v[0] = (V128){0};
    for (unsigned lane=0;lane<insn->lanes;++lane) {
        u64 bits=insn->width==16 ? input->v[1].h[lane] : insn->width==32 ? input->v[1].s[lane] : input->v[1].d[lane];
        u64 value = snx_fp_unary_c(&expected, bits, (int)insn->width, (int)insn->op);
        if(insn->width==16)expected.v[0].h[lane]=(u16)value;else if(insn->width==32)expected.v[0].s[lane]=(u32)value;else expected.v[0].d[lane]=value;
    }
    if(step(&actual)==0x1004&&!memcmp(&actual,&expected,sizeof actual))return 0;
    fprintf(stderr,"Estimate %08x input %llx result %llx/%llx flags %x/%x FPCR %x\n",insn->word,
            (unsigned long long)input->v[1].d[0],(unsigned long long)actual.v[0].d[0],
            (unsigned long long)expected.v[0].d[0],actual.fpsr,expected.fpsr,input->fpcr);
    return 1;
}
static int run(int jit,int portable,char opt,unsigned long long*count) {
    LlvmSteps steps;char error[1024];
    if(llvm_steps_begin_opt(&steps,jit,portable,opt,error,sizeof error))return snx_error("%s",error);
    for(unsigned i=0;i<sizeof instructions/sizeof instructions[0];++i)llvm_steps_add(&steps,instructions[i].word,0x1000,SNX_A64_TARGET_MAX);
    if(!steps.ir->big_endian&&(steps.ir->features->x86||steps.ir->features->aarch64)&&LLVMGetNamedFunction(steps.ir->mod,"snx_fp_unary"))
        return snx_error("an estimate still contains a lane helper");
    if(llvm_steps_load(&steps,error,sizeof error))return snx_error("%s",error);
    u64 rng=0x729e4a13dcbf5608ull;int failed=0;
    for(unsigned i=0;i<sizeof instructions/sizeof instructions[0]&&!failed;++i) {
        const Insn* insn=&instructions[i];unsigned fb=insn->width==16?10:insn->width==32?23:52,emax=insn->width==16?31:insn->width==32?255:2047;
        for(unsigned exponent=0;exponent<=emax&&!failed;++exponent)
            for(unsigned bin=0;bin<256&&!failed;++bin) {
                CPU c={0};c.pc=0x1000;
                u64 first=(u64)exponent<<fb|(u64)bin<<(fb-8);
                c.v[0]=(V128){.d={~0ull,~0ull}};
                if(insn->width==16) {for(unsigned lane=0;lane<8;lane++)c.v[1].h[lane]=(u16)(first|(lane&3));}
                else if(insn->width==32)c.v[1]=(V128){.s={(u32)first,(u32)first|0x7fff,0,0}};
                else c.v[1]=(V128){.d={first,first|((1ull<<(fb-8))-1)}};
                c.fpcr=(bin&3)<<22|((bin>>2)&1)<<24|((bin>>3)&1)<<25|((bin>>4)&1)<<19;
                c.fpsr=bin&16?0x0800009fu:0;
                failed=check(steps.steps[i],insn,&c);++*count;
                if(insn->width==16){for(unsigned lane=0;lane<8;lane++)c.v[1].h[lane]|=0x8000;}
                else if(insn->width==32)c.v[1].s[0]|=0x80000000u;
                else {c.v[1].d[0]|=0x8000000000000000ull;c.v[1].d[1]|=0x8000000000000000ull;}
                if(!failed){failed=check(steps.steps[i],insn,&c);++*count;}
            }
        for(unsigned bit=0;bit<fb&&!failed;++bit)
            for(unsigned mode=0;mode<32&&!failed;++mode)
                for(unsigned delta=0;delta<3&&!failed;++delta) {
                    CPU c={0};c.pc=0x1000;
                    u64 value=(1ull<<bit)+delta-1;
                    if(mode&16)value|=1ull<<(insn->width-1);
                    if(insn->width==16){for(unsigned lane=0;lane<8;lane++)c.v[1].h[lane]=(u16)value;}
                    else if(insn->width==32)c.v[1]=(V128){.s={(u32)value,(u32)value,0,0}};
                    else c.v[1]=(V128){.d={value,value}};
                    c.fpcr=(mode&3)<<22|((mode>>2)&1)<<24|((mode>>3)&1)<<25|((mode>>2)&1)<<19;c.fpsr=bit&1?0x0800009fu:0;
                    failed=check(steps.steps[i],insn,&c);++*count;
                }
        for(unsigned n=0;n<32768&&!failed;++n) {
            CPU c={0};c.pc=0x1000;
            for(unsigned reg=0;reg<32;++reg)for(unsigned h=0;h<2;++h)c.v[reg].d[h]=random_next(&rng);
            c.fpcr=(n&3)<<22|((n>>2)&1)<<24|((n>>3)&1)<<25|((n>>4)&1)<<19;c.fpsr=n&16?0x0800009fu:0;
            failed=check(steps.steps[i],insn,&c);++*count;
        }
    }
    llvm_steps_end(&steps);return failed;
}
static int bench(unsigned index, unsigned pattern, char opt, int portable) {
    if (index >= sizeof instructions / sizeof instructions[0] || pattern > 2) return 2;
    const Insn* insn = &instructions[index];
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps, 1, portable, opt, error, sizeof error)) return snx_error("%s", error);
    llvm_steps_add(&steps, insn->word, 0x1000, SNX_A64_TARGET_MAX);
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    CPU cpu = {0};
    cpu.pc = 0x1000;
    for (unsigned lane = 0; lane < 8; lane++) cpu.v[1].h[lane] = (u16)(0x3c00 + lane * 37);
    if (pattern == 1) cpu.v[1].h[0] = 0x7d23;
    if (pattern == 2) for (unsigned lane = 0; lane < 8; lane++) cpu.v[1].h[lane] = (u16)(lane + 1);
    u64 checksum = 0;
    double start = snx_seconds();
    for (unsigned n = 0; n < 1000000; n++) {
        steps.steps[0](&cpu);
        checksum += cpu.v[0].d[0] ^ cpu.v[0].d[1] ^ cpu.fpsr;
    }
    double elapsed = snx_seconds() - start;
    printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n", elapsed * 1e9, (unsigned long long)checksum);
    llvm_steps_end(&steps);
    return 0;
}
int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "--bench"))
        return bench((unsigned)strtoul(argv[2], NULL, 10), (unsigned)strtoul(argv[3], NULL, 10),
                     argc > 4 ? argv[4][0] : '2', argc > 5 ? atoi(argv[5]) : 0);
    unsigned long long count = 0;
    for (int jit = 0; jit < 2; ++jit)
        for (int portable = 0; portable < 2; ++portable)
            for (unsigned level = 0; level < 2; ++level)
                if (run(jit, portable, level ? '2' : '0', &count)) return 1;
    printf("FP estimates: %llu exact cases\n", count);
    return 0;
}
