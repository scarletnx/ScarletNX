/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT loader's code can be unwound through, as a module's can: a loaded function with a stack frame of its own
   calls back into this program, which longjmps out across it. On x86-64 Windows longjmp unwinds every frame between
   (RtlUnwindEx), which needs the loaded code's function table (.pdata, registered by code_loader_coff.c), and
   RtlLookupFunctionEntry finds the loaded function. Elsewhere it checks the same load and call */
#include "common/cpu/recompiler/llvm/code_loader.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static jmp_buf target;
static volatile int jumped;

/* called by the loaded function, deep in its frame */
static void jumper(void) {
    jumped = 1;
    longjmp(target, 7);
}

int main(void) {
    char error[1024];
    const char* names[1] = {"snx_test_frame"};
    SnxU64 address = 0;
    SnxIr* ir;
    SnxCodeLoader* loader;
    SnxLinkInput input;
    unsigned char* object;
    size_t size;
    LLVMTypeRef callback, params[1], frame_type;
    LLVMValueRef frame, slot, call_args[1];
    void (*volatile run)(void (*)(void));
    volatile int result;
    if (snx_ir_init(error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    ir = snx_ir_new("unwind", '2', error, sizeof(error));
    if (!ir) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    /* void snx_test_frame(void (*cb)(void)): a 512-byte buffer written through a volatile store, so the frame stays,
       then the call, then a store after it, so the call is no tail call */
    callback = LLVMFunctionType(ir->t_void, NULL, 0, 0);
    params[0] = ir->t_ptr;
    frame_type = LLVMFunctionType(ir->t_void, params, 1, 0);
    frame = LLVMAddFunction(ir->mod, "snx_test_frame", frame_type);
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, frame, "entry"));
    slot = LLVMBuildArrayAlloca(ir->b, ir->t_i8, LLVMConstInt(ir->t_i64, 512, 0), "buffer");
    LLVMSetVolatile(LLVMBuildStore(ir->b, LLVMConstInt(ir->t_i8, 1, 0), slot), 1);
    call_args[0] = NULL;
    LLVMBuildCall2(ir->b, callback, LLVMGetParam(frame, 0), call_args, 0, "");
    LLVMSetVolatile(LLVMBuildStore(ir->b, LLVMConstInt(ir->t_i8, 2, 0), slot), 1);
    LLVMBuildRetVoid(ir->b);
    if (snx_ir_object(ir, '2', &object, &size, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    snx_ir_free(ir);
    loader = snx_code_loader_new(0, error, sizeof(error));
    if (!loader) {
        fprintf(stderr, "loader: %s\n", error);
        return 1;
    }
    input.data = object;
    input.size = size;
    input.name = "unwind";
    if (snx_code_load(loader, &input, 1, names, 1, &address, error, sizeof(error))) {
        fprintf(stderr, "load: %s\n", error);
        return 1;
    }
    free(object);
#ifdef _WIN32
    {
        DWORD64 image = 0;
        if (!RtlLookupFunctionEntry((DWORD64)address, &image, NULL)) {
            fprintf(stderr, "RtlLookupFunctionEntry finds no entry for the loaded function at %llx\n",
                    (unsigned long long)address);
            return 1;
        }
    }
#endif
    run = (void (*)(void (*)(void)))(size_t)address;
    result = setjmp(target);
    if (!result) {
        run(jumper);
        fprintf(stderr, "the loaded function returned instead of being unwound\n");
        return 1;
    }
    if (result != 7 || !jumped) {
        fprintf(stderr, "longjmp came back with %d\n", result);
        return 1;
    }
    snx_code_loader_free(loader);
    printf("code loader unwind: passed (longjmp across loaded code)\n");
    return 0;
}
