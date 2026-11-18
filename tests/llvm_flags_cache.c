/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The flags' cache within a basic block, the back end's (tools/sailgen/llvm.h) that every emitter keeps its state in
   through ir.h's accessors (ir_sail.c): a second read is the first one's value, a commit is read back as the values
   committed and keeps the registers cached, a compare's flags give a condition as that compare (its origin), and an
   explicit invalidation and a new block drop the values and the origin */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include <llvm-c/Analysis.h>
#include <stdio.h>

static int failures;
static void check(int ok, const char* what) {
    if (ok) return;
    fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

/* c is ir_bool32 of icmp predicate (left, right) */
static int is_compare(LLVMValueRef c, LLVMIntPredicate predicate, LLVMValueRef left, LLVMValueRef right) {
    LLVMValueRef i;
    if (!LLVMIsAZExtInst(c)) return 0;
    i = LLVMGetOperand(c, 0);
    return LLVMIsAICmpInst(i) && LLVMGetICmpPredicate(i) == predicate && LLVMGetOperand(i, 0) == left &&
           LLVMGetOperand(i, 1) == right;
}

int main(void) {
    char error[1024], *message = NULL;
    SnxIr* ir;
    LLVMValueRef fn, x0, x1, c, v[4], zero;
    LLVMTypeRef params[2];
    unsigned i;
    int same;
    if (snx_ir_init(error, sizeof(error))) return snx_error("%s", error);
    ir = snx_ir_new("flags", '2', error, sizeof(error));
    if (!ir) return snx_error("%s", error);
    params[0] = ir->t_ptr;
    params[1] = ir->t_i64;
    fn = LLVMAddFunction(ir->mod, "flags", LLVMFunctionType(ir->t_void, params, 2, 0));
    snx_ir_begin(ir, fn);
    x0 = ir_x(ir, 0);
    x1 = ir_x(ir, 1);
    c = ir_flag_read(ir, 2);
    check(ir_flag_read(ir, 2) == c, "a second read of C in the block is the first's value");
    for (i = 0; i < 4; ++i) v[i] = LLVMBuildTrunc(ir->b, LLVMBuildLShr(ir->b, x0, ir_i64(ir, i), ""), ir->t_i32, "");
    ir_flags_commit(ir, v[0], v[1], v[2], v[3]);
    for (same = 1, i = 0; i < 4; ++i) same &= ir_flag_read(ir, i) == v[i];
    check(same, "a flag read after a commit is the value committed, C read before it too");
    check(ir_x(ir, 0) == x0, "a flags commit keeps the registers cached");
    check(!is_compare(ir_flags_cond(ir, 8), LLVMIntUGT, x0, x1), "flags committed as values give no compare");
    (void)ir_flags_subs(ir, 64, x0, x1);
    check(is_compare(ir_flags_cond(ir, 8), LLVMIntUGT, x0, x1),
          "HI after SUBS is the compare of its operands, unsigned greater (the origin)");
    check(is_compare(ir_flags_cond(ir, 10), LLVMIntSGE, x0, x1), "GE after SUBS is the signed compare");
    zero = LLVMConstNull(ir->t_i64);
    ir_flags_logic(ir, x1, 64);
    check(is_compare(ir_flags_cond(ir, 0), LLVMIntEQ, x1, zero), "EQ after a logical result is that result against 0");
    (void)ir_flags_subs(ir, 64, x0, x1);
    ir_flags_invalidate(ir);
    check(!is_compare(ir_flags_cond(ir, 8), LLVMIntUGT, x0, x1), "an explicit invalidation drops the origin");
    c = ir_flag_read(ir, 2);
    ir_cpu_store(ir, ir_i32(ir, 1), offsetof(CPU, cf));
    check(ir_flag_read(ir, 2) != c, "a direct store into the flags drops their values");
    (void)ir_flags_subs(ir, 64, x0, x1);
    c = ir_flag_read(ir, 2);
    LLVMBuildRetVoid(ir->b);
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "second"));
    check(!is_compare(ir_flags_cond(ir, 8), LLVMIntUGT, x0, x1), "the origin does not cross into another block");
    check(ir_flag_read(ir, 2) != c, "the flags' values do not cross into another block");
    LLVMBuildRetVoid(ir->b);
    check(!LLVMVerifyModule(ir->mod, LLVMReturnStatusAction, &message), "the IR verifies");
    if (message) {
        if (failures) fprintf(stderr, "%s\n", message);
        LLVMDisposeMessage(message);
    }
    snx_ir_free(ir);
    printf("flags cache: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
