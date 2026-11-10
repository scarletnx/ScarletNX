/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A generated function whose frame is a page or more probes its stack on Windows x86-64. Windows commits a thread's
   stack through the guard page below the part in use, and a frame that skips that page faults in reserved memory, where
   the process ends with no handler run (rtlib.c's snx_stack_probe). On any host: a guest function made for
   x86_64-w64-windows-gnu calls the runtime library's probe, so its object alone does not link. On a Windows x86-64
   host also: that object links with the runtime library the AOT modules get, and the JIT loader runs one on a new
   thread whose stack has only its first page committed. Without the probe the process ends there */
#include "common/cpu/recompiler/llvm/code_loader.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/rtlib_object.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define EXIT_SKIPPED 77
/* larger than the stack a new thread has committed, and than the guard page below it */
#define FRAME        (256u << 10)

/* F_1000(void** out, pc): a FRAME-byte buffer whose last and first bytes are stored to, volatile, and whose address
   goes to *out, so the optimizer can neither split it into the two bytes nor drop it: the frame of a guest function
   with a large spill area */
static void big_frame(SnxIr* ir) {
    LLVMValueRef fn = snx_ir_guest_function(ir, 0x1000);
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"));
    LLVMValueRef buffer = LLVMBuildAlloca(ir->b, LLVMArrayType(ir->t_i8, FRAME), "buffer");
    LLVMValueRef index = LLVMConstInt(ir->t_i64, FRAME - 1, 0);
    LLVMValueRef last = LLVMBuildGEP2(ir->b, ir->t_i8, buffer, &index, 1, "last");
    LLVMSetVolatile(LLVMBuildStore(ir->b, LLVMConstInt(ir->t_i8, 1, 0), last), 1);
    LLVMSetVolatile(LLVMBuildStore(ir->b, LLVMConstInt(ir->t_i8, 2, 0), buffer), 1);
    LLVMSetVolatile(LLVMBuildStore(ir->b, buffer, LLVMGetParam(fn, 0)), 1);
    LLVMBuildRetVoid(ir->b);
}

/* the object of big_frame for Windows x86-64, linked alone and (on such a host) with the runtime library */
static int windows_object(char opt) {
    static const char* const exports[] = {"F_1000"};
    char error[512];
    SnxIr* ir = snx_ir_new_target("probe", opt, "x86_64-w64-windows-gnu", error, sizeof error);
    if (!ir) {
        fprintf(stderr, "skipped: %s\n", error);
        return EXIT_SKIPPED;
    }
    big_frame(ir);
    unsigned char* data = NULL;
    size_t size = 0;
    if (snx_ir_object(ir, opt, &data, &size, error, sizeof error)) {
        fprintf(stderr, "FAIL: -O%c: %s\n", opt, error);
        snx_ir_free(ir);
        return 1;
    }
    snx_ir_free(ir);
    SnxLinkInput inputs[2] = {{data, size, "probe"}, {NULL, 0, "rtlib"}};
    SnxLinkOutput module = {0};
    int failed = 0;
    if (!snx_link(inputs, 1, exports, 1, "probe.dll", &module, error, sizeof error)) {
        fprintf(stderr, "FAIL: -O%c: the object links without the runtime library: its frame is not probed\n", opt);
        failed = 1;
    } else if (!strstr(error, "undefined symbol " SNX_IR_STACK_PROBE)) {
        fprintf(stderr, "FAIL: -O%c: the object alone fails to link for another reason: %s\n", opt, error);
        failed = 1;
    }
    free(module.data);
    module.data = NULL;
#if defined(_WIN32) && defined(__x86_64__)
    /* the runtime library of this host's AOT modules defines it (only a Windows build has it) */
    unsigned char* rtlib = NULL;
    size_t rtlib_size = 0;
    if (!failed && snx_ir_rtlib_object(opt, &rtlib, &rtlib_size, error, sizeof error)) {
        fprintf(stderr, "FAIL: -O%c: the runtime library: %s\n", opt, error);
        failed = 1;
    } else if (!failed) {
        inputs[0] = (SnxLinkInput){rtlib, rtlib_size, "rtlib"};
        inputs[1] = (SnxLinkInput){data, size, "probe"};
        if (snx_link(inputs, 2, exports, 1, "probe.dll", &module, error, sizeof error)) {
            fprintf(stderr, "FAIL: -O%c: the object does not link with the runtime library: %s\n", opt, error);
            failed = 1;
        }
    }
    free(rtlib);
    free(module.data);
#endif
    free(data);
    return failed;
}

#if defined(_WIN32) && defined(__x86_64__)
static DWORD WINAPI run_frame(LPVOID p) {
    void (*fn)(void*, SnxU64);
    void* out = NULL;
    memcpy(&fn, &p, sizeof fn); /* ISO C has no conversion from an object pointer to a function's */
    fn(&out, 0x1000);
    return out ? 7 : 1;
}

/* big_frame through the JIT loader, after the runtime library as the JIT loads it, run on a new thread */
static int jit_run(void) {
    char error[1024];
    const char* names[1] = {"F_1000"};
    SnxU64 address = 0;
    SnxIr* ir = snx_ir_new_jit("probe", '2', error, sizeof error);
    if (!ir) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    big_frame(ir);
    unsigned char* object = NULL;
    size_t size = 0;
    int failed = snx_ir_object(ir, '2', &object, &size, error, sizeof error);
    snx_ir_free(ir);
    if (failed) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    SnxCodeLoader* loader = snx_code_loader_new(0, error, sizeof error);
    if (!loader) {
        fprintf(stderr, "FAIL: loader: %s\n", error);
        free(object);
        return 1;
    }
    SnxLinkInput rtlib = {snx_rtlib_object, snx_rtlib_object_size, "rtlib"};
    SnxLinkInput unit = {object, size, "probe"};
    if (snx_code_load(loader, &rtlib, 1, NULL, 0, NULL, error, sizeof error) ||
        snx_code_load(loader, &unit, 1, names, 1, &address, error, sizeof error)) {
        fprintf(stderr, "FAIL: load: %s\n", error);
        free(object);
        snx_code_loader_free(loader);
        return 1;
    }
    free(object);
    /* a 1 MiB reservation, of which the system commits the executable's default (a page or so) and the guard page */
    void* p = NULL;
    void (*fn)(void*, SnxU64) = (void (*)(void*, SnxU64))(size_t)address;
    memcpy(&p, &fn, sizeof p);
    HANDLE thread = CreateThread(NULL, (SIZE_T)1 << 20, run_frame, p, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    DWORD code = 0;
    if (!thread || WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &code) ||
        code != 7) {
        fprintf(stderr, "FAIL: the thread did not return from the loaded function (exit code %lu)\n",
                (unsigned long)code);
        failed = 1;
    }
    if (thread) CloseHandle(thread);
    snx_code_loader_free(loader);
    return failed;
}
#endif

int main(void) {
    char error[512];
    if (snx_ir_init(error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    int rc0 = windows_object('0');
    int rc2 = windows_object('2');
    if (rc0 == 1 || rc2 == 1) return 1;
#if defined(_WIN32) && defined(__x86_64__)
    if (jit_run()) return 1;
    puts("stack probe: a frame of 256 KiB ran on a new thread's stack");
#endif
    if (rc0 == EXIT_SKIPPED || rc2 == EXIT_SKIPPED) return EXIT_SKIPPED;
    puts("stack probe: a large frame for Windows x86-64 calls " SNX_IR_STACK_PROBE);
    return 0;
}
