/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Standalone emitter used by a64_fp_diff.py. Exercise the real decoder, AOT
   shortcuts and interpreter, not a second copy of the instruction templates. */
#include "common/cpu/aarch64/a64.h"
#include <stdlib.h>

int main(int argc, char** argv) {
    int i;
    char name[32];
    puts("#include \"cpu.h\"\n#include \"cpu_helpers.c\"");
    puts("static int trapped;\nstatic void undef(CPU*c,u64 p,u32 w){(void)c;(void)p;(void)w;trapped=1;}");
    snx_a64_interp(stdout, 1);
    for (i = 1; i < argc; ++i) {
        sprintf(name, "step_%d", i);
        snx_a64_static_step(stdout, (SnxU32)strtoul(argv[i], NULL, 16), 0x1000, name);
    }
    puts("int run(unsigned index,unsigned interp,u32 w,u32 fpcr,u32 fpsr,u32 nzcv,const u64*in,u64*out){");
    puts("CPU state,*c=&state;RtAPI rt;memset(c,0,sizeof(*c));memset(&rt,0,sizeof(rt));rt.undef=undef;c->rt=&rt;"
         "c->fpcr=fpcr;c->fpsr=fpsr;set_nzcv(c,nzcv>>28);trapped=0;"
         "memcpy(&c->v[0],in,48);if(interp)snx_step(c,w,0x1000);else switch(index){");
    for (i = 1; i < argc; ++i) printf("case %d:step_%d(c);break;\n", i - 1, i);
    puts("default:return -1;}memcpy(out,&c->v[0],16);out[2]=c->fpsr;out[3]=get_nzcv(c);return trapped;}");
    return 0;
}
