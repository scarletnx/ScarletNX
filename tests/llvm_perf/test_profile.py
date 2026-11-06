#!/usr/bin/env python3
"""Test the production offline profile reader and emitted LLVM branch weights."""
from pathlib import Path
import sys
src=(Path(__file__).resolve().parents[2]/'src/common/cpu/recompiler/llvm/ir_profile.c').read_text()
src=src.replace('#include "common/cpu/recompiler/llvm/ir.h"','').replace('#include "common/utilities/common.h"','')
source=r'''
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint64_t SnxU64;
typedef uint32_t SnxU32;
typedef void* LLVMValueRef;
typedef void* LLVMMetadataRef;
typedef void* LLVMContextRef;
typedef void* LLVMModuleRef;
typedef void* LLVMBuilderRef;
typedef void* LLVMTypeRef;
typedef struct { LLVMContextRef ctx; LLVMModuleRef mod; LLVMBuilderRef b; LLVMTypeRef t_i32; } SnxIr;
extern LLVMMetadataRef LLVMMDStringInContext2(LLVMContextRef,const char*,size_t),LLVMValueAsMetadata(LLVMValueRef),LLVMMDNodeInContext2(LLVMContextRef,LLVMMetadataRef*,size_t);
extern LLVMValueRef LLVMMetadataAsValue(LLVMContextRef,LLVMMetadataRef),LLVMConstInt(LLVMTypeRef,uint64_t,int);
extern void LLVMSetMetadata(LLVMValueRef,unsigned,LLVMValueRef);
extern unsigned LLVMGetMDKindIDInContext(LLVMContextRef,const char*,unsigned);
static LLVMValueRef ir_i32(SnxIr* ir,SnxU32 v){return LLVMConstInt(ir->t_i32,v,0);}
static int snx_error(const char* fmt,...){va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);fputc('\n',stderr);return 1;}
''' + src + r'''
extern LLVMContextRef LLVMContextCreate(void);
extern LLVMModuleRef LLVMModuleCreateWithNameInContext(const char*,LLVMContextRef);
extern LLVMBuilderRef LLVMCreateBuilderInContext(LLVMContextRef);
extern LLVMTypeRef LLVMInt1TypeInContext(LLVMContextRef),LLVMInt32TypeInContext(LLVMContextRef),LLVMVoidTypeInContext(LLVMContextRef),LLVMFunctionType(LLVMTypeRef,LLVMTypeRef*,unsigned,int);
extern LLVMValueRef LLVMAddFunction(LLVMModuleRef,const char*,LLVMTypeRef),LLVMBuildCondBr(LLVMBuilderRef,LLVMValueRef,void*,void*),LLVMConstInt(LLVMTypeRef,uint64_t,int),LLVMBuildRetVoid(LLVMBuilderRef);
extern void* LLVMAppendBasicBlockInContext(LLVMContextRef,LLVMValueRef,const char*);
extern void LLVMPositionBuilderAtEnd(LLVMBuilderRef,void*);
extern int LLVMVerifyModule(LLVMModuleRef,int,char**);
extern char* LLVMPrintModuleToString(LLVMModuleRef);
int main(int argc,char**argv) {
    if(argc!=2)return 1;
    snx_ir_branch_profile(argv[1]);
    SnxIr ir={0};ir.ctx=LLVMContextCreate();ir.mod=LLVMModuleCreateWithNameInContext("profiles",ir.ctx);
    ir.b=LLVMCreateBuilderInContext(ir.ctx);ir.t_i32=LLVMInt32TypeInContext(ir.ctx);
    for(unsigned i=0;i<3;++i){
       char name[32];sprintf(name,"fn_%u",i);
       LLVMValueRef fn=LLVMAddFunction(ir.mod,name,LLVMFunctionType(LLVMVoidTypeInContext(ir.ctx),NULL,0,0));
       void* entry=LLVMAppendBasicBlockInContext(ir.ctx,fn,"entry");
       void* yes=LLVMAppendBasicBlockInContext(ir.ctx,fn,"yes");
       void* no=LLVMAppendBasicBlockInContext(ir.ctx,fn,"no");
       LLVMPositionBuilderAtEnd(ir.b,entry);
       LLVMValueRef cond=LLVMConstInt(LLVMInt1TypeInContext(ir.ctx),1,0);
       LLVMValueRef branch=LLVMBuildCondBr(ir.b,cond,yes,no);
       ir_profile_branch(&ir,branch,i<2?0x1000:0x3000,i==1?0xD65F03C1:0xD65F03C0);
       LLVMPositionBuilderAtEnd(ir.b,yes);LLVMBuildRetVoid(ir.b);
       LLVMPositionBuilderAtEnd(ir.b,no);LLVMBuildRetVoid(ir.b);
    }
    char*err=NULL;
    if(LLVMVerifyModule(ir.mod,2,&err)){fprintf(stderr,"invalid LLVM IR: %s\n",err);return 3;}
    char*irtext=LLVMPrintModuleToString(ir.mod);
    const char*needle="!\"branch_weights\", i32 100, i32 20";
    if(!strstr(irtext,needle)){fprintf(stderr,"profile branch metadata missing:\n%s\n",irtext);return 4;}
    unsigned sites=0;
    for(char*p=irtext;(p=strstr(p,"!prof !"));++p)++sites;
    if(sites!=1){fprintf(stderr,"stale word or unknown PC accepted: %u\n%s\n",sites,irtext);return 5;}
    puts("PASS optional PGO: matched PC/word, exact branch weights, rejects stale and unknown identities");
    return 0;
}
'''
Path(sys.argv[1]).write_text(source)
