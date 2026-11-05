/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Half conversions across the full input encoding space, AHP/DN/FZ16 modes, scalar/vector forms and aliasing. */
#include "llvm_steps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { u32 word; unsigned from, to, lanes, first_in, first_out; } Insn;
static const Insn instructions[] = {
    {0x1EE24020u,16,32,1,0,0}, {0x1EE2C020u,16,64,1,0,0}, {0x1E23C020u,32,16,1,0,0},
    {0x0E217820u,16,32,4,0,0}, {0x4E217820u,16,32,4,4,0},
    {0x0E216820u,32,16,4,0,0}, {0x4E216820u,32,16,4,0,4},
};
static u64 random_next(u64* rng) { *rng ^= *rng << 13; *rng ^= *rng >> 7; *rng ^= *rng << 17; return *rng; }
/* Model the guest-order byte swaps on this host. A narrowing high-half write preserves the source-width
   storage below it. This checks lane packing, not another host's ABI. */
static void reverse_elements(V128* value, unsigned start, unsigned size, unsigned bits) {
    unsigned bytes=bits/8;
    for(unsigned at=start;at<start+size;at+=bytes)for(unsigned i=0;i<bytes/2;i++) {
        u8 x=value->b[at+i];value->b[at+i]=value->b[at+bytes-i-1];value->b[at+bytes-i-1]=x;
    }
}
static int check(LlvmStep step,const Insn* insn,unsigned destination,const CPU* input,int reversed) {
    CPU expected = *input,actual = *input;
    if(!insn->first_out)expected.v[destination]=(V128){0};
    for(unsigned lane=0;lane<insn->lanes;++lane) {
        unsigned at=insn->first_in+lane;
        u64 bits=insn->from==16?input->v[1].h[at]:input->v[1].s[at];
        u64 value = snx_fp_convert_c(&expected, bits, (int)insn->from, (int)insn->to, (int)((input->fpcr >> 22) & 3));
        at=insn->first_out+lane;
        if(insn->to==16)expected.v[destination].h[at]=(u16)value;
        else if(insn->to==32)expected.v[destination].s[at]=(u32)value;
        else expected.v[destination].d[at]=value;
    }
    if(reversed)for(unsigned reg=0;reg<32;reg++)reverse_elements(&actual.v[reg],0,16,insn->from);
    u64 pc=step(&actual);
    if(reversed)for(unsigned reg=0;reg<32;reg++) {
        if(reg!=destination)reverse_elements(&actual.v[reg],0,16,insn->from);
        else if(insn->first_out) {
            reverse_elements(&actual.v[reg],0,8,insn->from);
            reverse_elements(&actual.v[reg],8,8,insn->to);
        } else reverse_elements(&actual.v[reg],0,16,insn->to);
    }
    if(pc==0x1004&&!memcmp(&actual,&expected,sizeof actual))return 0;
    fprintf(stderr,"Half conversion %08x dst%u FPCR%x input %llx output %llx/%llx FPSR%x/%x\n",insn->word,
        destination,input->fpcr,(unsigned long long)input->v[1].d[0],(unsigned long long)actual.v[destination].d[0],
        (unsigned long long)expected.v[destination].d[0],actual.fpsr,expected.fpsr);
    return 1;
}
static int run(int jit,int portable,char opt,unsigned long long* checks,int reversed) {
    LlvmSteps steps;char error[1024];
    if(llvm_steps_begin_opt(&steps,jit,portable,opt,error,sizeof error))return snx_error("%s",error);
    if(reversed&&steps.ir->big_endian){llvm_steps_end(&steps);return 0;}
    if(reversed)steps.ir->big_endian=1;
    for(unsigned i=0;i<sizeof instructions/sizeof instructions[0];++i)
        for(unsigned alias=0;alias<2;++alias)llvm_steps_add(&steps,instructions[i].word|alias,0x1000,0);
    if(llvm_steps_load(&steps,error,sizeof error))return snx_error("%s",error);
    u64 rng=0x194e8a6b27c35fd0ull;int failed=0;
    for(unsigned i=0;i<sizeof instructions/sizeof instructions[0]&&!failed;++i)
        for(unsigned alias=0;alias<2&&!failed;++alias) {
            const Insn* insn=&instructions[i];
            if(reversed&&i<3)continue;
            for(unsigned bits=0;bits<65536&&!failed;++bits)
                for(unsigned mode=0;mode<8&&!failed;++mode) {
                    CPU c={0};c.pc=0x1000;
                    for(unsigned reg=0;reg<32;++reg)for(unsigned h=0;h<2;++h)c.v[reg].d[h]=random_next(&rng);
                    c.fpcr=(bits&3)<<22|(mode&1)<<26|((mode>>1)&1)<<25|((mode>>2)&1)<<19;
                    c.fpsr=bits&16?0x0800009fu:0;
                    CPU convert={0};
                    u32 expanded = (u32)snx_fp_convert_c(&convert, bits, 16, 32, 0);
                    for(unsigned lane=0;lane<insn->lanes;++lane) {
                        if(insn->from==16)c.v[1].h[insn->first_in+lane]=(u16)bits;
                        else c.v[1].s[lane]=expanded+(lane&1?0x1000u:0);
                    }
                    failed=check(steps.steps[i*2+alias],insn,alias,&c,reversed);++*checks;
                }
            for(unsigned sample=0;sample<32768&&!failed;++sample) {
                CPU c={0};c.pc=0x1000;
                for(unsigned reg=0;reg<32;++reg)for(unsigned h=0;h<2;++h)c.v[reg].d[h]=random_next(&rng);
                c.fpcr=(sample&3)<<22|((sample>>2)&1)<<24|((sample>>3)&1)<<25|((sample>>4)&1)<<26;
                c.fpsr=sample&32?0x0800009fu:0;
                failed=check(steps.steps[i*2+alias],insn,alias,&c,reversed);++*checks;
            }
        }
    llvm_steps_end(&steps);return failed;
}
static int bench(unsigned index,char opt,int reference, const char* dump, unsigned pattern) {
    if(index>=sizeof instructions/sizeof instructions[0] || pattern>2)return 2;
    LlvmSteps steps;char error[1024];
    if(llvm_steps_begin_opt(&steps,1,reference,opt,error,sizeof error))return snx_error("%s",error);
    llvm_steps_add(&steps,instructions[index].word,0x1000,0);
    if (dump && strcmp(dump, "-")) {
        int failed=llvm_steps_dump(&steps,dump,error,sizeof error);
        if(failed)fprintf(stderr,"%s\n",error);
        llvm_steps_end(&steps);return failed!=0;
    }
    if(llvm_steps_load(&steps,error,sizeof error))return snx_error("%s",error);
    CPU c={0};c.pc=0x1000;
    if(instructions[index].from==16)for(unsigned lane=0;lane<8;++lane)c.v[1].h[lane]=(u16)(0x3555+lane*123);
    else c.v[1]=(V128){.s={0x3fc00000,0xc0200000,0x3faaaaab,0xbf000000}};
    const Insn* insn=&instructions[index];
    if(pattern==1) {
        if(insn->from==16)c.v[1].h[insn->first_in]=0x7d01;
        else c.v[1].s[0]=0x7f800001;
    }
    if(pattern==2) {
        c.fpcr=1u<<26;
        if(insn->from==16)c.v[1].h[insn->first_in]=0x7fff;
    }
    u64 checksum=0;double begin=snx_seconds();
    for(unsigned n=0;n<1000000;++n){steps.steps[0](&c);checksum+=c.v[0].d[0]+(c.v[0].d[1]^c.fpsr);}
    double elapsed=snx_seconds()-begin;
    printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n",elapsed*1e9,(unsigned long long)checksum);
    llvm_steps_end(&steps);return 0;
}
int main(int argc,char**argv) {
    if(argc>=3&&!strcmp(argv[1],"--bench"))
        return bench((unsigned)strtoul(argv[2],NULL,10),argc>3?argv[3][0]:'0',argc>4&&!strcmp(argv[4],"--reference"),argc>5?argv[5]:NULL,argc>6?(unsigned)strtoul(argv[6],NULL,10):0);
    unsigned long long checks=0;
    for(int jit=0;jit<2;++jit)for(int portable=0;portable<2;++portable)for(unsigned opt=0;opt<2;++opt)
        if(run(jit,portable,opt?'2':'0',&checks,0))return 1;
    if(run(1,1,'0',&checks,1)||run(1,1,'2',&checks,1))return 1;
    printf("Half conversions: %llu exact cases\n",checks);return 0;
}
