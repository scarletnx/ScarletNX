/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* All fixed-point estimate bins and their boundaries, plus seeded bits, without any FP status effects. */
#include "llvm_steps.h"
#include <stdio.h>
#include <string.h>
static int run(int jit,int portable,char opt,unsigned long long* count) {
    LlvmSteps steps;char error[1024];
    if(llvm_steps_begin_opt(&steps,jit,portable,opt,error,sizeof error))return snx_error("%s",error);
    for(unsigned op=0;op<2;++op)for(unsigned short_form=0;short_form<2;++short_form)for(unsigned alias=0;alias<2;++alias) {
        u32 word=(op?0x6EA1C820u:0x4EA1C820u)|alias;
        if(short_form)word&=~0x40000000u;
        llvm_steps_add(&steps,word,0x1000,0);
    }
    if(llvm_steps_load(&steps,error,sizeof error))return snx_error("%s",error);
    size_t index=0;int failed=0;
    for(unsigned op=0;op<2&&!failed;++op)for(unsigned short_form=0;short_form<2&&!failed;++short_form)
        for(unsigned alias=0;alias<2&&!failed;++alias,++index)
            for(unsigned bin=0;bin<512&&!failed;++bin)for(unsigned sample=0;sample<16&&!failed;++sample) {
                CPU expected={0};expected.pc=0x1000;expected.fpsr=0x0800009fu;expected.fpcr=0x07c00000u;
                for(unsigned lane=0;lane<4;++lane)expected.v[1].s[lane]=(bin<<23)|((sample*0x6c8e9cf5u+lane*0x1973u)&0x7fffffu);
                if(sample==0)expected.v[1].s[0]=bin<<23;
                if(sample==1)expected.v[1].s[0]=(bin<<23)|0x7fffffu;
                CPU actual=expected;expected.v[alias]=(V128){0};
                for(unsigned lane=0;lane<(short_form?2u:4u);++lane)
                    expected.v[alias].s[lane]=op?ursqrte32(actual.v[1].s[lane]):urecpe32(actual.v[1].s[lane]);
                if(steps.steps[index](&actual)!=0x1004||memcmp(&actual,&expected,sizeof actual)) {
                    fprintf(stderr,"Unsigned estimate op%u short%u alias%u bin%u sample%u O%c\n",op,short_form,alias,bin,sample,opt);
                    failed=1;
                }
                ++*count;
            }
    llvm_steps_end(&steps);return failed;
}
int main(void) {
    unsigned long long count=0;
    for(int jit=0;jit<2;++jit)for(int portable=0;portable<2;++portable)for(unsigned opt=0;opt<2;++opt)
        if(run(jit,portable,opt?'2':'0',&count))return 1;
    printf("Unsigned estimates: %llu exact cases\n",count);return 0;
}
