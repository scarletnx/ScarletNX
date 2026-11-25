/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* LLVM's SLP vectorizer, the setting slp_vectorizer (common/cpu/recompiler/llvm/ir.c): on as LLVM has it until
   snx_ir_slp_vectorizer(0), which stops it in default<O2> and default<O3> as the pass builder's SLPVectorization does
   not, once for the process; and the JIT cache's key says which */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/jit/jit_cache.h"
#include <assert.h>
#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* b[0] = a[0] + 1, b[1] = a[1] + 2 run through pipeline: whether it came out as one <2 x double> add */
static int packed(const char* pipeline) {
    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("slp", ctx);
    LLVMTypeRef f64 = LLVMDoubleTypeInContext(ctx), ptr = LLVMPointerTypeInContext(ctx, 0), params[2] = {ptr, ptr};
    LLVMValueRef fn = LLVMAddFunction(mod, "f", LLVMFunctionType(LLVMVoidTypeInContext(ctx), params, 2, 0));
    LLVMBuilderRef b = LLVMCreateBuilderInContext(ctx);
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    char *triple = LLVMGetDefaultTargetTriple(), *cpu = LLVMGetHostCPUName(), *features = LLVMGetHostCPUFeatures(),
         *why = NULL, *text;
    LLVMTargetRef target;
    LLVMTargetMachineRef tm;
    LLVMValueRef value[2];
    LLVMErrorRef failed;
    int i, vector, found;
    LLVMPositionBuilderAtEnd(b, LLVMAppendBasicBlockInContext(ctx, fn, "entry"));
    /* both loads before the stores: b may overlap a, so a load after a store would have to stay there */
    for (i = 0; i < 2; ++i) {
        LLVMValueRef at = LLVMConstInt(LLVMInt64TypeInContext(ctx), (unsigned long long)i, 0);
        value[i] = LLVMBuildLoad2(b, f64, LLVMBuildGEP2(b, f64, LLVMGetParam(fn, 0), &at, 1, ""), "");
    }
    for (i = 0; i < 2; ++i) {
        LLVMValueRef at = LLVMConstInt(LLVMInt64TypeInContext(ctx), (unsigned long long)i, 0);
        LLVMBuildStore(b, LLVMBuildFAdd(b, value[i], LLVMConstReal(f64, i + 1), ""),
                       LLVMBuildGEP2(b, f64, LLVMGetParam(fn, 1), &at, 1, ""));
    }
    LLVMBuildRetVoid(b);
    found = !LLVMGetTargetFromTriple(triple, &target, &why);
    assert(found);
    tm = LLVMCreateTargetMachine(target, triple, cpu, features, LLVMCodeGenLevelDefault, LLVMRelocDefault,
                                 LLVMCodeModelDefault);
    LLVMSetTarget(mod, triple);
    failed = LLVMRunPasses(mod, pipeline, tm, options);
    assert(!failed);
    text = LLVMPrintModuleToString(mod);
    vector = strstr(text, "<2 x double>") != NULL;
    LLVMDisposeMessage(text);
    LLVMDisposePassBuilderOptions(options);
    LLVMDisposeBuilder(b);
    LLVMDisposeTargetMachine(tm);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    LLVMDisposeMessage(triple);
    LLVMDisposeMessage(cpu);
    LLVMDisposeMessage(features);
    return vector;
}

int main(void) {
    char error[256], *on, *off;
    assert(!snx_ir_init(error, sizeof error));
    /* LLVM's own: on, and the pass builder's switch leaves it on */
    on = snx_jit_cache_key('2', 0, 0, 0);
    assert(snx_ir_slp() == 1 && on && strstr(on, " slp=1 "));
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
    assert(packed("default<O2>")); /* SIMD for two doubles: the hosts the CI runs this on */
#endif
    snx_ir_slp_vectorizer(1); /* on is LLVM's own: nothing to set */
    assert(snx_ir_slp() == 1);
    snx_ir_slp_vectorizer(0);
    assert(!snx_ir_slp() && !packed("default<O2>") && !packed("default<O3>"));
    snx_ir_slp_vectorizer(0); /* once only: LLVM refuses its option given twice */
    off = snx_jit_cache_key('2', 0, 0, 0);
    assert(!snx_ir_slp() && off && strstr(off, " slp=0 ") && strcmp(on, off));
    free(on);
    free(off);
    puts("PASS slp_vectorizer");
    return 0;
}
