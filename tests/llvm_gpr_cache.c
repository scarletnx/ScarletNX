/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LLVM backend's cache of the guest's X registers within a basic block (ir_state.c): a second read is the first
   one's value, a write is forwarded to later reads and leaves other registers cached, a store to the flags keeps
   them, a direct store into the X array, an explicit invalidation and a new block drop them */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include <llvm-c/Analysis.h>
#include <stddef.h>
#include <stdio.h>

static int failures;
static void check(int ok, const char* what) {
    if (ok) return;
    fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

int main(void) {
    char error[1024], *message = NULL;
    SnxIr* ir;
    LLVMValueRef fn, a, added, sp;
    LLVMTypeRef params[2];
    if (snx_ir_init(error, sizeof(error))) return snx_error("%s", error);
    ir = snx_ir_new("regs", '2', error, sizeof(error));
    if (!ir) return snx_error("%s", error);
    params[0] = ir->t_ptr;
    params[1] = ir->t_i64;
    fn = LLVMAddFunction(ir->mod, "regs", LLVMFunctionType(ir->t_void, params, 2, 0));
    snx_ir_begin(ir, fn);
    a = ir_x(ir, 0);
    check(ir_x(ir, 0) == a, "a second read of X0 in the block is the first's value");
    ir_set_x(ir, 1, a);
    check(ir_x(ir, 0) == a, "a write of X1 keeps X0 cached");
    ir_cpu_store(ir, ir_i64(ir, 1), offsetof(CPU, z));
    check(ir_x(ir, 0) == a, "a store to the flags keeps X0 cached");
    added = LLVMBuildAdd(ir->b, a, ir_i64(ir, 1), "");
    ir_set_x(ir, 0, added);
    check(ir_x(ir, 0) == added, "a read after a write is the value written");
    ir_cpu_store(ir, ir_i64(ir, 13), ir_off_x(0));
    check(ir_x(ir, 0) != added, "a direct store into the X array drops the cache");
    sp = ir_i64(ir, 77);
    ir_set_x(ir, 31, sp);
    check(ir_x(ir, 31) == sp, "SP (X31) is cached as the others are");
    ir_registers_invalidate(ir);
    check(ir_x(ir, 31) != sp, "an explicit invalidation drops the cache");
    LLVMValueRef low = ir_ve(ir, 1, 2, 0);
    check(ir_ve(ir, 1, 2, 0) == low, "repeated low scalar reads share the SSA value");
    ir_wr(ir, ir_x(ir, 0), 32, low);
    check(ir_ve(ir, 1, 2, 0) == low, "a guest store cannot alias the vector register cache");
    ir_cpu_store(ir, ir_i32(ir, 0), offsetof(CPU, fpsr));
    check(ir_ve(ir, 1, 2, 0) == low, "an FPSR store does not change vector bytes");
    check(ir_ve(ir, 1, 3, 0) != low, "different scalar widths have distinct snapshots");
    ir_set_ve(ir, 1, 0, 0, ir_i8(ir, 3));
    check(ir_ve(ir, 1, 2, 0) != low, "a byte lane write invalidates a wider scalar snapshot");
    low = ir_ve(ir, 1, 2, 0);
    ir_vec_write_high(ir, 1, LLVMConstNull(LLVMVectorType(ir->t_i64, 1)));
    check(ir_ve(ir, 1, 2, 0) != low, "vector writes conservatively invalidate scalar snapshots");
    low = ir_ve(ir, 1, 2, 0);
    ir_state_invalidate(ir);
    check(ir_ve(ir, 1, 2, 0) != low, "unknown call or store invalidation drops scalar snapshots");
    low = ir_ve(ir, 1, 2, 0);
    added = ir_x(ir, 0);
    LLVMBuildRetVoid(ir->b);
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "second"));
    check(ir_x(ir, 0) != added, "the cache does not cross into another block");
    check(ir_ve(ir, 1, 2, 0) != low, "scalar snapshots do not cross basic blocks");
    LLVMBuildRetVoid(ir->b);
    check(!LLVMVerifyModule(ir->mod, LLVMReturnStatusAction, &message), "the IR verifies");
    if (message) {
        if (failures) fprintf(stderr, "%s\n", message);
        LLVMDisposeMessage(message);
    }
    snx_ir_free(ir);
    printf("GPR cache: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
