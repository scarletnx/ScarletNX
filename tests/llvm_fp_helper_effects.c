/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vector FP declarations must have the same effects regardless of whether a scalar FP declaration came first. */
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include "common/cpu/aarch64/cpu.h"
#include <llvm-c/Analysis.h>
#include <stdio.h>

static int noregs(LLVMValueRef function) {
    return function && LLVMGetStringAttributeAtIndex(function, LLVMAttributeFunctionIndex, "snx-noregs", 10) != NULL;
}
static unsigned cpu_loads(LLVMValueRef fn, size_t offset) {
    LLVMValueRef cpu = LLVMGetParam(fn, 0);
    unsigned count = 0;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb))
        for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst; inst = LLVMGetNextInstruction(inst)) {
            if (!LLVMIsALoadInst(inst)) continue;
            LLVMValueRef p = LLVMGetOperand(inst, 0);
            if (LLVMIsAGetElementPtrInst(p) && LLVMGetOperand(p, 0) == cpu && LLVMGetNumOperands(p) == 2 &&
                LLVMIsAConstantInt(LLVMGetOperand(p, 1)) && LLVMConstIntGetZExtValue(LLVMGetOperand(p, 1)) == offset)
                ++count;
        }
    return count;
}
int main(void) {
    char error[1024], *message = NULL;
    if (snx_ir_init(error, sizeof error)) return snx_error("%s", error);
    SnxIr* ir = snx_ir_new("fp_helper_effects", '2', error, sizeof error);
    if (!ir) return snx_error("%s", error);
    ir->stats.enabled = 1;
    /* Exercise the helper classification independently of the host's inline estimate support. */
    SnxIrFeatures plain = *ir->features;
    plain.avx2 = 0;
    ir->features = &plain;
    LLVMTypeRef params[] = {ir->t_ptr, ir->t_i64};
    LLVMValueRef fn = LLVMAddFunction(ir->mod, "effects", LLVMFunctionType(ir->t_void, params, 2, 0));
    snx_ir_begin(ir, fn);
    ir_set_x(ir, 4, ir_i64(ir, 0xc0ffee));
    SnxA64Insn insn;
    u32 word = 0x4EA1D820u; /* FRECPE V0.4S,V1.4S: four exact lane-helper calls. */
    snx_a64_decode_word(word, 0, &insn);
    snx_ir_vec(ir, &insn, word, 0x1000);
    ir_set_x(ir, 5, ir_x(ir, 4));
    LLVMBuildRetVoid(ir->b);
    ir_promote_registers(ir, fn);
    ir_stats_function(ir, fn);
    unsigned loads = cpu_loads(fn, ir_off_x(4));
    int marked = noregs(LLVMGetNamedFunction(ir->mod, "snx_fp_unary"));
    LLVMValueRef other = LLVMAddFunction(ir->mod, "writes_vectors", LLVMFunctionType(ir->t_void, params, 2, 0));
    snx_ir_begin(ir, other);
    LLVMValueRef operands[] = {ir_i32(ir, 0), ir_i32(ir, 1), ir_i32(ir, 2), ir_i32(ir, 0)};
    ir_vec_call(ir, "snx_vec_shl", ir->t_void, operands, 4);
    LLVMBuildRetVoid(ir->b);
    int unsafe = noregs(LLVMGetNamedFunction(ir->mod, "snx_vec_shl"));
    int counts = ir->stats.functions == 1 && ir->stats.promoted_functions == 1 && ir->stats.fp_calls == 4 &&
                 ir->stats.call_sites == 4 && ir->stats.sync_stores == 3 && ir->stats.call_stores == 0 &&
                 ir->stats.store_bytes == 32 && ir->stats.sync_reloads == 0;
    ir_stats_function(ir, other);
    LLVMValueRef calls = LLVMAddFunction(ir->mod, "calls", LLVMFunctionType(ir->t_void, params, 2, 0));
    snx_ir_begin(ir, calls);
    LLVMTypeRef guest_type = LLVMFunctionType(ir->t_void, params, 2, 0);
    LLVMValueRef args[] = {ir->c, ir_i64(ir, 0x1000)};
    LLVMBuildCall2(ir->b, guest_type, snx_ir_guest_function(ir, 0x1000), args, 2, "");
    LLVMBuildCall2(ir->b, guest_type, LLVMAddFunction(ir->mod, "snx_aot_2000", guest_type), args, 2, "");
    LLVMValueRef pointer = LLVMBuildIntToPtr(ir->b, LLVMGetParam(calls, 1), ir->t_ptr, "");
    LLVMBuildCall2(ir->b, guest_type, pointer, args, 2, "");
    LLVMBuildRetVoid(ir->b);
    ir_stats_function(ir, calls);
    counts = counts && ir->stats.functions == 3 && ir->stats.call_sites == 8 && ir->stats.fp_calls == 4 &&
             ir->stats.vector_calls == 1 && ir->stats.guest_calls == 2 && ir->stats.indirect_calls == 1;
    int invalid = LLVMVerifyModule(ir->mod, LLVMReturnStatusAction, &message);
    printf("FP-only helper marked %d, X4 reloads across four lane calls %u, vector-writing helper marked %d\n",
           marked, loads, unsafe);
    if (invalid && message) fprintf(stderr, "%s\n", message);
    if (message) LLVMDisposeMessage(message);
    snx_ir_free(ir);
    return !marked || loads != 0 || unsafe || invalid || !counts;
}
