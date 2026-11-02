/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The FP helpers touch no memory, and the vector one (snx_fp_host_lanes) only the locals it is given, so neither is
   a site for ir_promote_registers: a lane-wide instruction's X live across its helper call is not stored and
   reloaded, and nothing declares the helpers snx-noregs. On x86-64 and AArch64 the vector one is called with
   preserve_all, through snx_fp_host_lanes_kept. */
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include "common/cpu/aarch64/cpu.h"
#include <llvm-c/Analysis.h>
#include <stdio.h>

/* the "memory" attribute's value, or -1 without one */
static long long memory(LLVMValueRef function) {
    if (!function) return -1;
    LLVMAttributeRef attribute =
        LLVMGetEnumAttributeAtIndex(function, LLVMAttributeFunctionIndex, LLVMGetEnumAttributeKindForName("memory", 6));
    return attribute ? (long long)LLVMGetEnumAttributeValue(attribute) : -1;
}
static int noregs(LLVMValueRef function) {
    return function && LLVMGetStringAttributeAtIndex(function, LLVMAttributeFunctionIndex, "snx-noregs", 10);
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
    u32 word = 0x4EA1D820u; /* FRECPE V0.4S,V1.4S: without AVX2, one call of the lane helper for the vector */
    snx_a64_decode_word(word, 0, &insn);
    snx_ir_vec(ir, &insn, word, 0x1000);
    ir_set_x(ir, 5, ir_x(ir, 4));
    LLVMBuildRetVoid(ir->b);
    ir_promote_registers(ir, fn);
    ir_stats_function(ir, fn);
    unsigned reloads = (unsigned)ir->stats.sync_reloads, call_stores = (unsigned)ir->stats.call_stores;
    /* on x86-64 and AArch64 the call keeps what preserve_all keeps, through snx_fp_host_lanes_kept (cpu.h) */
    int kept = ir->features->x86 || ir->features->aarch64;
    const char* name = !kept               ? "snx_fp_host_lanes"
                       : ir->features->avx ? "snx_fp_host_lanes_kept_avx"
                                           : "snx_fp_host_lanes_kept";
    LLVMValueRef lanes = LLVMGetNamedFunction(ir->mod, name);
    int marked = memory(lanes) == 3 && !noregs(lanes); /* its arguments' memory alone */
    unsigned convention = kept ? LLVMPreserveAllCallConv : LLVMCCallConv, call_convention = ~0u;
    for (LLVMUseRef use = lanes ? LLVMGetFirstUse(lanes) : NULL; use; use = LLVMGetNextUse(use)) {
        LLVMValueRef user = LLVMGetUser(use);
        if (LLVMIsACallInst(user)) call_convention = LLVMGetInstructionCallConv(user);
    }
    int convention_right = lanes && LLVMGetFunctionCallConv(lanes) == convention && call_convention == convention;
    if (!convention_right)
        fprintf(stderr, "FAIL: %s is %s with convention %u, called with %u, where %u is right\n", name,
                lanes ? "declared" : "not declared", lanes ? LLVMGetFunctionCallConv(lanes) : 0, call_convention,
                convention);
    LLVMValueRef other = LLVMAddFunction(ir->mod, "writes_vectors", LLVMFunctionType(ir->t_void, params, 2, 0));
    snx_ir_begin(ir, other);
    LLVMValueRef operands[] = {ir_i32(ir, 0), ir_i32(ir, 1), ir_i32(ir, 2), ir_i32(ir, 0)};
    ir_vec_call(ir, "snx_vec_shl", ir->t_void, operands, 4);
    LLVMBuildRetVoid(ir->b);
    int unsafe = memory(LLVMGetNamedFunction(ir->mod, "snx_vec_shl")) == 0 ||
                 noregs(LLVMGetNamedFunction(ir->mod, "snx_vec_shl"));
    /* the vector's one lanes call and the entry's snx_fp_env; the stores are X4, X5, V0 and the accumulated FPSR */
    int counts = ir->stats.functions == 1 && ir->stats.promoted_functions == 1 && ir->stats.fp_calls == 2 &&
                 ir->stats.call_sites == 2 && ir->stats.sync_stores == 4 && ir->stats.call_stores == 0 &&
                 ir->stats.store_bytes == 36 && ir->stats.sync_reloads == 0;
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
    /* each function's entry calls snx_fp_env: 2 + 2 + 4 calls, 2 + 1 + 1 of them snx_fp_* */
    counts = counts && ir->stats.functions == 3 && ir->stats.call_sites == 8 && ir->stats.fp_calls == 4 &&
             ir->stats.vector_calls == 1 && ir->stats.guest_calls == 2 && ir->stats.indirect_calls == 1;
    int invalid = LLVMVerifyModule(ir->mod, LLVMReturnStatusAction, &message);
    printf("lanes helper argmem %d, reloads after its call %u, stores before it %u, vector helper marked %d\n", marked,
           reloads, call_stores, unsafe);
    if (invalid && message) fprintf(stderr, "%s\n", message);
    if (message) LLVMDisposeMessage(message);
    snx_ir_free(ir);
    return !marked || unsafe || invalid || !counts || !convention_right;
}
