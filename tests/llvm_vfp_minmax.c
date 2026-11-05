/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vector min/max: every half encoding, mixed exceptional lanes, payload priority, aliases and sticky FPSR. */
#include "llvm_steps.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { u32 word; unsigned width, lanes, minimum, numeric; } Insn;
static Insn instruction(unsigned index) {
    unsigned form = index / 4, op = index % 4;
    Insn insn = {0, form < 2 ? 16u : form < 4 ? 32u : 64u, form == 0 ? 8u : form == 1 ? 4u : form == 2 ? 4u : 2u,
                 op & 1, op >> 1};
    insn.word = insn.width == 16 ? 0x4e423420u : insn.width == 32 ? 0x4e22f420u : 0x4e62f420u;
    if (insn.minimum) insn.word |= 1u << 23;
    if (insn.numeric) insn.word &= ~(3u << 12);
    if (form == 1 || form == 3) insn.word &= ~(1u << 30);
    return insn;
}
static u64 next_random(u64* x) { *x ^= *x << 13; *x ^= *x >> 7; *x ^= *x << 17; return *x; }
static u64 get(const V128* v, unsigned at, unsigned width) { return width == 16 ? v->h[at] : width == 32 ? v->s[at] : v->d[at]; }
static void put(V128* v, unsigned at, unsigned width, u64 x) {
    if (width == 16) v->h[at] = (u16)x;
    else if (width == 32) v->s[at] = (u32)x;
    else v->d[at] = x;
}
static u64 edge(unsigned at, unsigned width) {
    unsigned bits = width == 16 ? 10 : width == 32 ? 23 : 52;
    u64 sign = 1ull << (width - 1), exponent = (sign - 1) & ~((1ull << bits) - 1);
    u64 values[] = {0,sign,1,sign | 1,(1ull << bits)-1,1ull << bits,exponent,sign | exponent,
                   exponent | 1,sign | exponent | 1,exponent | (1ull << (bits - 1)) | 123,
                   sign | exponent | (1ull << (bits - 1)) | 321,exponent-1,(sign | exponent)-1};
    return values[at % (sizeof values / sizeof *values)];
}
static int check(LlvmStep step, const Insn* insn, unsigned rd, const CPU* input) {
    CPU expected = *input, actual = *input;
    expected.v[rd] = (V128){0};
    for (unsigned lane = 0; lane < insn->lanes; lane++) {
        SnxFpReply result = snx_fp_minmax(get(&input->v[1],lane,insn->width),get(&input->v[2],lane,insn->width),
                                         (int)insn->width,(int)insn->minimum,(int)insn->numeric,input->fpcr);
        expected.fpsr |= result.flags;
        put(&expected.v[rd],lane,insn->width,result.value);
    }
    u64 pc = step(&actual);
    if (pc == 0x1004 && !memcmp(&expected,&actual,sizeof actual)) return 0;
    fprintf(stderr,"minmax %08x rd%u FPCR%x FPSR%x/%x result%llx/%llx\n",insn->word,rd,input->fpcr,
            actual.fpsr,expected.fpsr,(unsigned long long)actual.v[rd].d[0],(unsigned long long)expected.v[rd].d[0]);
    return 1;
}
static int run(int jit, int portable, char opt, unsigned long long* count) {
    LlvmSteps steps;char error[1024];
    if (llvm_steps_begin_opt(&steps,jit,portable,opt,error,sizeof error)) return snx_error("%s",error);
    for (unsigned i=0;i<20;i++)for(unsigned rd=0;rd<3;rd++)llvm_steps_add(&steps,instruction(i).word|rd,0x1000,SNX_A64_TARGET_MAX);
    if (llvm_steps_load(&steps,error,sizeof error)) return snx_error("%s",error);
    int failed=0;u64 random=0x192fed2146acb037ull;
    for(unsigned i=0;i<20&&!failed;i++) {
        Insn insn=instruction(i);
        for(unsigned mode=0;mode<8&&!failed;mode++)for(unsigned a=0;a<14&&!failed;a++)for(unsigned b=0;b<14&&!failed;b++) {
            CPU c={0};c.pc=0x1000;c.fpcr=(mode&1)<<24|((mode>>1)&1)<<19|((mode>>2)&1)<<25;c.fpsr=a&1?0x0800009fu:0;
            for(unsigned r=0;r<32;r++)for(unsigned h=0;h<2;h++)c.v[r].d[h]=next_random(&random);
            for(unsigned lane=0;lane<insn.lanes;lane++) {put(&c.v[1],lane,insn.width,edge(a+lane,insn.width));put(&c.v[2],lane,insn.width,edge(b+lane,insn.width));}
            for(unsigned rd=0;rd<3&&!failed;rd++){failed=check(steps.steps[i*3+rd],&insn,rd,&c);++*count;}
        }
        unsigned limit=insn.width==16?65536:32768;
        for(unsigned n=0;n<limit&&!failed;n++) {
            CPU c={0};c.pc=0x1000;c.fpcr=(n&1)<<24|((n>>1)&1)<<19|((n>>2)&1)<<25;c.fpsr=n&8?0x0800009fu:0;
            for(unsigned lane=0;lane<insn.lanes;lane++) {
                put(&c.v[1],lane,insn.width,insn.width==16?n:next_random(&random));
                put(&c.v[2],lane,insn.width,edge(n+lane,insn.width));
            }
            for(unsigned rd=0;rd<3&&!failed;rd++){failed=check(steps.steps[i*3+rd],&insn,rd,&c);++*count;}
            V128 swap=c.v[1];c.v[1]=c.v[2];c.v[2]=swap;
            if(!failed){failed=check(steps.steps[i*3],&insn,0,&c);++*count;}
        }
    }
    llvm_steps_end(&steps);return failed;
}
static int bench(unsigned index, unsigned pattern, char opt, int portable, const char* dump) {
    if(index>=20||pattern>2)return 2;
    Insn insn=instruction(index);LlvmSteps steps;char error[1024];
    if(llvm_steps_begin_opt(&steps,1,portable,opt,error,sizeof error))return snx_error("%s",error);
    llvm_steps_add(&steps,insn.word,0x1000,SNX_A64_TARGET_MAX);
    if(dump) {
        int failed=llvm_steps_dump(&steps,dump,error,sizeof error);
        if(failed)fprintf(stderr,"%s\n",error);
        llvm_steps_end(&steps);return failed!=0;
    }
    if(llvm_steps_load(&steps,error,sizeof error))return snx_error("%s",error);
    CPU c={0};c.pc=0x1000;
    unsigned fb=insn.width==16?10:insn.width==32?23:52,bias=insn.width==16?15:insn.width==32?127:1023;
    for(unsigned lane=0;lane<insn.lanes;lane++) {put(&c.v[1],lane,insn.width,(u64)bias<<fb);put(&c.v[2],lane,insn.width,((u64)bias<<fb)|123);}
    if(pattern)put(&c.v[1],0,insn.width,edge(8,insn.width));
    if(pattern==2)for(unsigned lane=0;lane<insn.lanes;lane++)put(&c.v[1],lane,insn.width,edge(lane+6,insn.width));
    u64 sum=0;double start=snx_seconds();
    for(unsigned n=0;n<1000000;n++){steps.steps[0](&c);sum+=c.v[0].d[0]^(c.v[0].d[1]+c.fpsr);}
    printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n",(snx_seconds()-start)*1e9,(unsigned long long)sum);
    llvm_steps_end(&steps);return 0;
}
int main(int argc,char**argv) {
    if(argc>3&&!strcmp(argv[1],"--bench"))return bench((unsigned)strtoul(argv[2],NULL,10),(unsigned)strtoul(argv[3],NULL,10),argc>4?argv[4][0]:'2',argc>5?atoi(argv[5]):0,argc>6?argv[6]:NULL);
    unsigned long long count=0;
    for(int jit=0;jit<2;jit++)for(int portable=0;portable<2;portable++)for(unsigned level=0;level<2;level++)
        if(run(jit,portable,level?'2':'0',&count))return 1;
    printf("Vector min/max: %llu exact cases\n",count);return 0;
}
