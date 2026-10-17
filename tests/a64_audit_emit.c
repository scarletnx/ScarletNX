/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Real decoder inventory and AOT/interpreter bridge for a64_audit.py. */
#include "common/cpu/aarch64/a64.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    int i;
    char name[32];
    if (argc == 2 && !strcmp(argv[1], "--inventory")) {
        size_t count, p;
        const SnxA64Pattern* patterns = snx_a64_patterns(&count);
        for (p = 0; p < count; ++p) {
            const SnxA64Pattern* t = &patterns[p];
            unsigned f;
            printf("%s %08lx %08lx %lu", t->name, (unsigned long)t->mask, (unsigned long)t->value,
                   (unsigned long)t->case_count);
            for (f = 0; f < t->field_count; ++f) printf(" %u:%u", t->fields[f].lo, t->fields[f].width);
            puts("");
        }
        return 0;
    }
    puts("#include \"cpu.h\"\n#include \"cpu_helpers.c\"\n#include \"a64_audit_state.h\"");
    puts("static int trapped;static void undef(CPU*c,u64 p,u32 w){(void)c;(void)p;(void)w;trapped=1;}");
    snx_a64_interp(stdout, 1);
    for (i = 1; i < argc; ++i) {
        sprintf(name, "step_%d", i);
        snx_a64_static_step(stdout, (SnxU32)strtoul(argv[i], NULL, 16), 0x1000, name);
    }
    /* Wire state: X0..X30,SP; 64 V halves; NZCV nibble,FPCR,FPSR,PC. */
    puts("int run(unsigned index,unsigned interp,u32 w,const u64*in,u64*out,u8*mem){"
         "CPU state,*c=&state;RtAPI rt;u64 pc,saved=audit_host_save();memset(c,0,sizeof(*c));memset(&rt,0,sizeof(rt));"
         "rt.undef=undef;rt.fpcr_changed=audit_fpcr;c->rt=&rt;c->mem=mem;memcpy(c->x,in,256);memcpy(c->v,in+32,512);"
         "set_nzcv(c,in[96]);c->fpcr=in[97];c->fpsr=in[98];audit_fpcr(c);trapped=0;"
         /* Odd states prime a reservation; even states exercise failed STXR/STXP.
            This setup deliberately does not claim a multi-PE monitor test. */
         "if((in[96]&1)&&(w&0x3FC00000)==0x08000000&&(!(w&0x200000)||(w>>30)>=2)){"
         "u64 a=c->x[(w>>5)&31],v1,v2;if(w&0x200000)ldxp(c,mem,a,w>>30,&v1,&v2);else ldx(c,mem,a,w>>30);}"
         "if(interp)pc=snx_step(c,w,0x1000);else switch(index){");
    for (i = 1; i < argc; ++i) printf("case %d:pc=step_%d(c);break;\n", i - 1, i);
    puts(
        "default:return -1;}memcpy(out,c->x,256);memcpy(out+32,c->v,512);"
        "out[96]=get_nzcv(c)>>28;out[97]=c->fpcr;out[98]=c->fpsr;out[99]=pc;audit_host_restore(saved);return trapped;}");
    return 0;
}
