/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_ir_object's check of the symbols an object names: a call LLVM makes at code generation that nothing a module
   links defines fails the object, and the ones the runtime library defines (libc.inc's copies, 128-bit division) or
   the module declares pass. On x86-64's baseline CPU, which has no rounding instruction, so llvm.floor is a call to
   libm's floor; Windows and Apple objects are COFF and Mach-O, which the check does not read. */
#include "common/utilities/common.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(__APPLE__)
int main(void) {
    printf("llvm_object_symbols skipped: the object check reads ELF objects, and this host writes COFF or Mach-O\n");
    return 77;
}
#else
/* name(double) or name(ptr, i64) as one function of a fresh x86-64 module; 0 when snx_ir_object made the object, -1
   with its error when it refused */
static int object_of(const char* what, char* error, size_t error_size) {
    SnxIr* ir = snx_ir_new_target("symbols", '2', "x86_64-unknown-linux-gnu", error, error_size);
    if (!ir) return -2;
    LLVMTypeRef params[2] = {ir->t_ptr, ir->t_i64};
    LLVMValueRef fn = LLVMAddFunction(ir->mod, "f", LLVMFunctionType(ir->t_void, params, 2, 0));
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"));
    if (!strcmp(what, "floor")) {
        /* the rounding of a double the guest stores: llvm.floor, which the baseline CPU has no instruction for */
        LLVMTypeRef f64 = LLVMDoubleTypeInContext(ir->ctx);
        LLVMValueRef v = LLVMBuildLoad2(ir->b, f64, LLVMGetParam(fn, 0), "");
        v = ir_intrinsic(ir, "llvm.floor", f64, &v, 1);
        LLVMBuildStore(ir->b, v, LLVMGetParam(fn, 0));
    } else if (!strcmp(what, "memset")) {
        /* a store of a variable length, which LLVM makes a call to memset (the runtime library's) */
        LLVMBuildMemSet(ir->b, LLVMGetParam(fn, 0), LLVMConstInt(ir->t_i8, 0, 0), LLVMGetParam(fn, 1), 1);
    } else {
        /* a call to a helper the module declares */
        LLVMValueRef helper = ir_extern(ir, what, ir->t_void, params, 2);
        LLVMValueRef args[2] = {LLVMGetParam(fn, 0), LLVMGetParam(fn, 1)};
        LLVMBuildCall2(ir->b, LLVMGlobalGetValueType(helper), helper, args, 2, "");
    }
    LLVMBuildRetVoid(ir->b);
    unsigned char* data = NULL;
    size_t size = 0;
    int rc = snx_ir_object(ir, '2', &data, &size, error, error_size);
    free(data);
    snx_ir_free(ir);
    return rc ? -1 : 0;
}

int main(void) {
    char error[1024] = {0};
    int failed = 0, rc;
    if (snx_ir_init(error, sizeof error)) return snx_error("%s", error);
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    rc = object_of("floor", error, sizeof error);
    if (rc == -2) {
        printf("llvm_object_symbols skipped: this LLVM has no x86-64 target (%s)\n", error);
        return 77;
    }
    if (rc != -1 || !strstr(error, "floor")) {
        if (rc)
            fprintf(stderr, "an object calling libm's floor was refused, but the error names no floor: %s\n", error);
        else
            fprintf(stderr, "an object calling libm's floor was made: nothing a module links defines floor\n");
        failed = 1;
    } else
        printf("refused as it should be: %s\n", error);
    error[0] = 0;
    if (object_of("memset", error, sizeof error)) {
        fprintf(stderr, "an object calling the runtime library's memset was refused: %s\n", error);
        failed = 1;
    }
    error[0] = 0;
    if (object_of("snx_test_helper", error, sizeof error)) {
        fprintf(stderr, "an object calling a declared helper was refused: %s\n", error);
        failed = 1;
    }
    return failed;
}
#endif
