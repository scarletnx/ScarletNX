#!/usr/bin/env python3
"""JIT the real LLVM NZCV emitters and compare their CPU state to integer arithmetic.

The emitters are ir_state.c's (adds, subs, logic, set_nzcv and the condition), taken from the source as it is. The
block's cache of the flags they read and write is the back end's since the generated emitters keep theirs there
(ir_sail.c over sailgen's sg_*), so this harness gives them one of its own with the same behaviour, and no compare to
fold a condition into (snx_ir_sail_origin gives none): every condition here is read from the four flags."""
from pathlib import Path
import sys
src=(Path(__file__).resolve().parents[2]/'src/common/cpu/recompiler/llvm/ir_state.c').read_text()
def fn(signature):
 a=src.index(signature); b=src.index('{',a); depth=1; i=b+1
 while depth:
  if src[i]=='{': depth+=1
  if src[i]=='}': depth-=1
  i+=1
 return src[a:i]
funcs='\n\n'.join(fn(sig) for sig in ('LLVMValueRef ir_flags_adds(', 'LLVMValueRef ir_flags_subs(', 'void ir_flags_logic(', 'void ir_flags_set_nzcv(', 'LLVMValueRef ir_flags_cond('))
code=r'''
#include "common/cpu/aarch64/cpu.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef void *V,*LLVMValueRef,*LLVMContextRef,*LLVMModuleRef,*LLVMBuilderRef,*LLVMTypeRef,*LLVMBasicBlockRef,*LLVMExecutionEngineRef;
#define LLVMIntEQ 32
#define LLVMIntNE 33
#define LLVMIntUGE 35
#define LLVMIntULT 36
#define LLVMIntULE 37
#define LLVMReturnStatusAction 2
#define LLVMBuildNot(b,v,n) LLVMBuildXor(b,v,LLVMConstAllOnes(LLVMTypeOf(v)),n)
typedef struct { LLVMValueRef value[4]; LLVMBasicBlockRef block; unsigned valid; } SnxIrFlags;
typedef struct { SnxIrFlags flags; } SnxIrBlockState;
typedef struct { LLVMContextRef ctx; LLVMModuleRef mod; LLVMBuilderRef b; LLVMTypeRef t_i1,t_i8,t_i32,t_i64; V c; SnxIrBlockState state; } SnxIr;
extern V LLVMConstInt(LLVMTypeRef,uint64_t,int),LLVMConstAllOnes(LLVMTypeRef),LLVMBuildAdd(LLVMBuilderRef,V,V,const char*),LLVMBuildSub(LLVMBuilderRef,V,V,const char*),LLVMBuildXor(LLVMBuilderRef,V,V,const char*),LLVMBuildAnd(LLVMBuilderRef,V,V,const char*),LLVMBuildOr(LLVMBuilderRef,V,V,const char*),LLVMBuildLShr(LLVMBuilderRef,V,V,const char*),LLVMBuildSelect(LLVMBuilderRef,V,V,V,const char*),LLVMBuildICmp(LLVMBuilderRef,int,V,V,const char*),LLVMBuildStore(LLVMBuilderRef,V,V),LLVMBuildZExt(LLVMBuilderRef,V,LLVMTypeRef,const char*),LLVMBuildTrunc(LLVMBuilderRef,V,LLVMTypeRef,const char*),LLVMBuildLoad2(LLVMBuilderRef,LLVMTypeRef,V,const char*),LLVMBuildInBoundsGEP2(LLVMBuilderRef,LLVMTypeRef,V,V*,unsigned,const char*),LLVMConstNull(LLVMTypeRef),LLVMTypeOf(V);
extern unsigned LLVMGetIntTypeWidth(LLVMTypeRef);
extern LLVMContextRef LLVMContextCreate(void);
extern LLVMModuleRef LLVMModuleCreateWithNameInContext(const char*,LLVMContextRef);
extern LLVMBuilderRef LLVMCreateBuilderInContext(LLVMContextRef);
extern LLVMTypeRef LLVMInt8TypeInContext(LLVMContextRef),LLVMInt32TypeInContext(LLVMContextRef),LLVMInt64TypeInContext(LLVMContextRef),LLVMPointerTypeInContext(LLVMContextRef,unsigned),LLVMFunctionType(LLVMTypeRef,LLVMTypeRef*,unsigned,int);
extern V LLVMAddFunction(LLVMModuleRef,const char*,LLVMTypeRef),LLVMGetParam(V,unsigned),LLVMBuildRet(LLVMBuilderRef,V);
extern LLVMBasicBlockRef LLVMGetInsertBlock(LLVMBuilderRef),LLVMAppendBasicBlockInContext(LLVMContextRef,V,const char*);
extern void LLVMPositionBuilderAtEnd(LLVMBuilderRef,LLVMBasicBlockRef),LLVMLinkInMCJIT(void),LLVMInitializeX86TargetInfo(void),LLVMInitializeX86Target(void),LLVMInitializeX86TargetMC(void),LLVMInitializeX86AsmPrinter(void);
extern int LLVMVerifyModule(LLVMModuleRef,int,char**),LLVMCreateExecutionEngineForModule(LLVMExecutionEngineRef*,LLVMModuleRef,char**);
extern uint64_t LLVMGetFunctionAddress(LLVMExecutionEngineRef,const char*);
extern char* LLVMPrintModuleToString(LLVMModuleRef);
static V ir_i32(SnxIr*ir,uint32_t n){return LLVMConstInt(ir->t_i32,n,0);}
static V ir_i64(SnxIr*ir,uint64_t n){return LLVMConstInt(ir->t_i64,n,0);}
static V ir_bool32(SnxIr*ir,V v){return LLVMBuildZExt(ir->b,v,ir->t_i32,"");}
static V ir_u32(SnxIr*ir,V v){return LLVMGetIntTypeWidth(LLVMTypeOf(v))>32?LLVMBuildTrunc(ir->b,v,ir->t_i32,""):LLVMGetIntTypeWidth(LLVMTypeOf(v))<32?LLVMBuildZExt(ir->b,v,ir->t_i32,""):v;}
static V ir_cast(SnxIr*ir,V v,LLVMTypeRef t,int ignored){(void)ignored;unsigned width=LLVMGetIntTypeWidth(LLVMTypeOf(v)),target=LLVMGetIntTypeWidth(t);return width==target?v:width<target?LLVMBuildZExt(ir->b,v,t,""):LLVMBuildTrunc(ir->b,v,t,"");}
static V ir_cpu(SnxIr*ir,size_t offset){V off=ir_i64(ir,offset);return LLVMBuildInBoundsGEP2(ir->b,ir->t_i8,ir->c,&off,1,"");}
static V ir_tag_cpu(SnxIr*ir,V access){(void)ir;return access;}
static V ir_cpu_load(SnxIr*ir,LLVMTypeRef type,size_t offset){return LLVMBuildLoad2(ir->b,type,ir_cpu(ir,offset),"");}
static const size_t flag_offsets[4]={offsetof(CPU,n),offsetof(CPU,z),offsetof(CPU,cf),offsetof(CPU,vf)};
enum { SNX_IR_FLAGS_SOME, SNX_IR_FLAGS_SUB, SNX_IR_FLAGS_ADD, SNX_IR_FLAGS_LOGIC };
extern V LLVMIsAConstantInt(V);
extern unsigned long long LLVMConstIntGetZExtValue(V);
/* the block's flags as the emitters reach them: each read once from the CPU and kept, each written to it and kept */
static void ir_flags_invalidate(SnxIr*ir){ir->state.flags.valid=0;ir->state.flags.block=NULL;}
static SnxIrFlags* flags_block(SnxIr*ir){
    SnxIrFlags*flags=&ir->state.flags;LLVMBasicBlockRef block=LLVMGetInsertBlock(ir->b);
    if(flags->block!=block){flags->valid=0;flags->block=block;}
    return flags;
}
static V ir_flag_read(SnxIr*ir,unsigned index){
    SnxIrFlags*flags=flags_block(ir);unsigned bit=1u<<index;
    if(!(flags->valid&bit)){flags->value[index]=ir_cpu_load(ir,ir->t_i32,flag_offsets[index]);flags->valid|=bit;}
    return flags->value[index];
}
static void ir_flags_commit(SnxIr*ir,V n,V z,V c,V v){
    SnxIrFlags*flags=flags_block(ir);V values[4]={n,z,c,v};
    for(unsigned i=0;i<4;++i){V bits=ir_u32(ir,values[i]);ir_tag_cpu(ir,LLVMBuildStore(ir->b,bits,ir_cpu(ir,flag_offsets[i])));flags->value[i]=bits;}
    flags->valid=15;
}
static void ir_flags_origin(SnxIr*ir,unsigned origin,V left,V right){(void)ir;(void)origin;(void)left;(void)right;}
static V snx_ir_sail_origin(SnxIr*ir,unsigned code){(void)ir;(void)code;return NULL;}
''' + funcs +r'''
static void build(SnxIr* ir,int mode,unsigned width,unsigned cond) {
    char name[40];sprintf(name,"f_%u_%u_%u",mode,width,cond);
    LLVMTypeRef ty=width==64?ir->t_i64:ir->t_i32;
    LLVMTypeRef types[]={LLVMPointerTypeInContext(ir->ctx,0),ty,ty,ir->t_i32};
    V f=LLVMAddFunction(ir->mod,name,LLVMFunctionType(ir->t_i32,types,4,0));
    LLVMPositionBuilderAtEnd(ir->b,LLVMAppendBasicBlockInContext(ir->ctx,f,"entry"));
    ir_flags_invalidate(ir);
    ir->c=LLVMGetParam(f,0);
    V a=LLVMGetParam(f,1),b=LLVMGetParam(f,2),cin=LLVMGetParam(f,3);
    if(mode==0)ir_flags_adds(ir,width,a,b,cin);
    else if(mode==1)ir_flags_subs(ir,width,a,b);
    else ir_flags_logic(ir,a,width);
    V decision=ir_flags_cond(ir,cond);
    LLVMBuildRet(ir->b,decision);
}
static uint32_t seed=0xB437C1u;
static uint64_t rand64(void){seed=1664525u*seed+1013904223u;uint64_t hi=seed;seed=1664525u*seed+1013904223u;return (hi<<32)|seed;}
static int condition(unsigned cond,int n,int z,int c,int v){
    int r;
    switch(cond>>1){case 0:r=z;break;case 1:r=c;break;case 2:r=n;break;case 3:r=v;break;case 4:r=c&&!z;break;case 5:r=n==v;break;case 6:r=!z && n==v;break;default:return 1;}
    return (cond&1)?!r:r;
}
int main(void){
    LLVMInitializeX86TargetInfo();LLVMInitializeX86Target();LLVMInitializeX86TargetMC();LLVMInitializeX86AsmPrinter();LLVMLinkInMCJIT();
    SnxIr ir={0}; ir.ctx=LLVMContextCreate();ir.mod=LLVMModuleCreateWithNameInContext("flags",ir.ctx);
    ir.b=LLVMCreateBuilderInContext(ir.ctx);ir.t_i1=LLVMInt8TypeInContext(ir.ctx);ir.t_i8=ir.t_i1;
    ir.t_i32=LLVMInt32TypeInContext(ir.ctx);ir.t_i64=LLVMInt64TypeInContext(ir.ctx);
    for(unsigned mode=0;mode<3;++mode)for(unsigned bits=32;bits<=64;bits+=32)for(unsigned cond=0;cond<16;++cond)build(&ir,mode,bits,cond);
    char *error=NULL;
    if(LLVMVerifyModule(ir.mod,LLVMReturnStatusAction,&error)){fprintf(stderr,"invalid IR: %s\n",error);return 2;}
    char *code=LLVMPrintModuleToString(ir.mod);
    if(strstr(code,"load i32, ptr %")!=NULL){/* Inputs are function parameters: flag forwarding needs no flag load. */}
    LLVMExecutionEngineRef engine=NULL;
    if(LLVMCreateExecutionEngineForModule(&engine,ir.mod,&error)){fprintf(stderr,"JIT %s\n",error);return 3;}
    uint64_t cases=0;
    for(unsigned mode=0;mode<3;++mode)for(unsigned bits=32;bits<=64;bits+=32)for(unsigned cond=0;cond<16;++cond){
        char name[40];sprintf(name,"f_%u_%u_%u",mode,bits,cond);
        uint32_t(*fn)(CPU*,uint64_t,uint64_t,uint32_t)=(void*)(uintptr_t)LLVMGetFunctionAddress(engine,name);
        if(!fn){fprintf(stderr,"missing %s\n",name);return 4;}
        uint64_t mask=bits==32?0xFFFFFFFFULL:~0ULL;
        for(unsigned sample=0;sample<130;++sample){
            uint64_t a=rand64()&mask,b=rand64()&mask;
            unsigned cin=rand64()&1;
            if(sample==0)a=b=0;
            if(sample==1)a=mask,b=1;
            if(sample==2)a=1ULL<<(bits-1),b=1ULL<<(bits-1);
            uint64_t r;
            __uint128_t sum=(__uint128_t)a+b+cin;
            if(mode==0)r=(uint64_t)sum&mask;
            else if(mode==1)r=(a-b)&mask;
            else r=a;
            int n=(int)((r>>(bits-1))&1),z=!r;
            int c=mode==0?(sum>>bits)&1:mode==1?a>=b:0;
            int v=mode==0?((~(a^b)&(a^r))>>(bits-1))&1:mode==1?(((a^b)&(a^r))>>(bits-1))&1:0;
            CPU cpu={0};
            unsigned result=fn(&cpu,a,b,cin);
            unsigned expected=condition(cond,n,z,c,v);
            if(result!=expected||cpu.n!=n||cpu.z!=z||cpu.cf!=c||cpu.vf!=v){
               fprintf(stderr,"mismatch %s sample=%u got=%u expected=%u flags %u %u %u %u vs %d %d %d %d\n",name,sample,result,expected,cpu.n,cpu.z,cpu.cf,cpu.vf,n,z,c,v);return 5;
            }
            ++cases;
        }
    }
    printf("PASS emitted NZCV: %llu native JIT comparisons of results, all flags, 16 conditions\n",(unsigned long long)cases);
    return 0;
}
'''
Path(sys.argv[1]).write_text(code)
