/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/aarch64/cpu.h"
#include <stdio.h>
#include <string.h>

/* ---- values ---- */

LLVMValueRef ir_i8(SnxIr* ir, unsigned v) { return LLVMConstInt(ir->t_i8, v & 0xFF, 0); }
LLVMValueRef ir_i32(SnxIr* ir, SnxU32 v) { return LLVMConstInt(ir->t_i32, v, 0); }
LLVMValueRef ir_i64(SnxIr* ir, SnxU64 v) { return LLVMConstInt(ir->t_i64, v, 0); }

LLVMValueRef ir_cast(SnxIr* ir, LLVMValueRef v, LLVMTypeRef to, int is_signed) {
    LLVMTypeRef source = LLVMTypeOf(v);
    if (source == to) return v;
    LLVMTypeRef source_lane = LLVMGetTypeKind(source) == LLVMVectorTypeKind ? LLVMGetElementType(source) : source;
    LLVMTypeRef target_lane = LLVMGetTypeKind(to) == LLVMVectorTypeKind ? LLVMGetElementType(to) : to;
    unsigned from = LLVMGetIntTypeWidth(source_lane), width = LLVMGetIntTypeWidth(target_lane);
    if (from == width) return v;
    if (from > width) return LLVMBuildTrunc(ir->b, v, to, "");
    return is_signed ? LLVMBuildSExt(ir->b, v, to, "") : LLVMBuildZExt(ir->b, v, to, "");
}
LLVMValueRef ir_u32(SnxIr* ir, LLVMValueRef v) { return ir_cast(ir, v, ir->t_i32, 0); }
LLVMValueRef ir_u64(SnxIr* ir, LLVMValueRef v) { return ir_cast(ir, v, ir->t_i64, 0); }
LLVMValueRef ir_bool32(SnxIr* ir, LLVMValueRef condition) { return LLVMBuildZExt(ir->b, condition, ir->t_i32, ""); }

/* ---- functions and calls ---- */

void ir_attribute(SnxIr* ir, LLVMValueRef function, LLVMAttributeIndex index, const char* name) {
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    LLVMAddAttributeAtIndex(function, index, LLVMCreateEnumAttribute(ir->ctx, kind, 0));
}

LLVMValueRef ir_call(SnxIr* ir, LLVMValueRef function, LLVMValueRef* args, unsigned count) {
    ir_state_invalidate(ir);
    return LLVMBuildCall2(ir->b, LLVMGlobalGetValueType(function), function, args, count, "");
}

LLVMValueRef ir_extern(SnxIr* ir, const char* name, LLVMTypeRef ret, LLVMTypeRef* params, unsigned count) {
    LLVMValueRef function = LLVMGetNamedFunction(ir->mod, name);
    if (function) return function;
    function = LLVMAddFunction(ir->mod, name, LLVMFunctionType(ret, params, count, 0));
    /* -fvisibility=hidden: everything the module links is local to it */
    LLVMSetVisibility(function, LLVMHiddenVisibility);
    ir_attribute(ir, function, LLVMAttributeFunctionIndex, "nounwind");
    return function;
}

static LLVMValueRef rt_pointer(SnxIr* ir) { return ir_cpu_load(ir, ir->t_ptr, offsetof(CPU, rt)); }
size_t ir_rt_svc(void) { return offsetof(RtAPI, svc); }
size_t ir_rt_svcs(void) { return offsetof(RtAPI, svcs); }
size_t ir_rt_undef(void) { return offsetof(RtAPI, undef); }
size_t ir_rt_brk(void) { return offsetof(RtAPI, brk); }
size_t ir_rt_sev(void) { return offsetof(RtAPI, sev); }
size_t ir_rt_wfe(void) { return offsetof(RtAPI, wfe); }
size_t ir_rt_poll(void) { return offsetof(RtAPI, poll); }
size_t ir_rt_miss(void) { return offsetof(RtAPI, miss); }
size_t ir_off_poll(void) { return offsetof(CPU, poll); }

LLVMValueRef ir_rt_call(SnxIr* ir, size_t offset, LLVMTypeRef ret, LLVMTypeRef* params, LLVMValueRef* args,
                        unsigned count) {
    LLVMValueRef index = ir_i64(ir, offset);
    LLVMValueRef slot = LLVMBuildInBoundsGEP2(ir->b, ir->t_i8, rt_pointer(ir), &index, 1, "");
    LLVMValueRef callee = ir_load(ir, ir->t_ptr, slot);
    ir_state_invalidate(ir);
    return LLVMBuildCall2(ir->b, LLVMFunctionType(ret, params, count, 0), callee, args, count, "");
}

/* SVC table entries are immutable ABI slots. The selected entry is resolved
   during translation, and calls can change any guest register. */
LLVMValueRef ir_rt_table_call(SnxIr* ir, size_t offset, unsigned entry, LLVMTypeRef ret, LLVMTypeRef* params,
                              LLVMValueRef* args, unsigned count) {
    LLVMValueRef index = ir_i64(ir, offset);
    LLVMValueRef table = ir_load(ir, ir->t_ptr, LLVMBuildInBoundsGEP2(ir->b, ir->t_i8, rt_pointer(ir), &index, 1, ""));
    index = ir_i64(ir, (SnxU64)entry);
    LLVMValueRef callee = ir_load(ir, ir->t_ptr, LLVMBuildInBoundsGEP2(ir->b, ir->t_ptr, table, &index, 1, ""));
    ir_state_invalidate(ir);
    return LLVMBuildCall2(ir->b, LLVMFunctionType(ret, params, count, 0), callee, args, count, "");
}

void ir_undef(SnxIr* ir, SnxU64 pc, SnxU32 word) {
    LLVMTypeRef params[3];
    LLVMValueRef args[3];
    params[0] = ir->t_ptr;
    params[1] = ir->t_i64;
    params[2] = ir->t_i32;
    args[0] = ir->c;
    args[1] = ir_i64(ir, pc);
    args[2] = ir_i32(ir, word);
    ir_rt_call(ir, ir_rt_undef(), ir->t_void, params, args, 3);
}

void ir_fence(SnxIr* ir) {
    /* Back-to-back seq_cst fences have no intervening memory effect. Keep
       their ordering, but emit only one even when LLVM optimization is off. */
    LLVMValueRef last = LLVMGetLastInstruction(LLVMGetInsertBlock(ir->b));
    if (last && LLVMGetInstructionOpcode(last) == LLVMFence &&
        LLVMGetOrdering(last) == LLVMAtomicOrderingSequentiallyConsistent)
        return;
    LLVMBuildFence(ir->b, LLVMAtomicOrderingSequentiallyConsistent, 0, "");
}

/* CBAR(): asm volatile("" ::: "memory") */
static void compiler_barrier(SnxIr* ir) {
    LLVMTypeRef type = LLVMFunctionType(ir->t_void, NULL, 0, 0);
    LLVMValueRef barrier = LLVMGetInlineAsm(type, "", 0, "~{memory}", 9, 1, 0, LLVMInlineAsmDialectATT, 0);
    LLVMBuildCall2(ir->b, type, barrier, NULL, 0, "");
}
void ir_yield(SnxIr* ir) {
    /* cpu.h: __builtin_ia32_pause() under __x86_64__, the low-priority hint under __powerpc64__, the compiler
       barrier alone elsewhere */
    const char* target = LLVMGetTarget(ir->mod);
    if (!strncmp(target, "x86_64", 6)) { /* PAUSE is baseline x86-64: no CPUID gate */
        static const char pause[] = "llvm.x86.sse2.pause";
        unsigned id = LLVMLookupIntrinsicID(pause, strlen(pause));
        LLVMValueRef function = LLVMGetIntrinsicDeclaration(ir->mod, id, NULL, 0);
        LLVMBuildCall2(ir->b, LLVMIntrinsicGetType(ir->ctx, id, NULL, 0), function, NULL, 0, "");
    } else if (!strncmp(target, "powerpc64", 9) || !strncmp(target, "ppc64", 5)) {
        static const char yield[] = "or 27,27,27"; /* yield: lower this hardware thread's priority for a moment */
        LLVMTypeRef type = LLVMFunctionType(ir->t_void, NULL, 0, 0);
        LLVMValueRef hint = LLVMGetInlineAsm(type, yield, strlen(yield), "", 0, 1, 0, LLVMInlineAsmDialectATT, 0);
        LLVMBuildCall2(ir->b, type, hint, NULL, 0, "");
    }
    compiler_barrier(ir);
}

static void ir_branch_weights(SnxIr* ir, LLVMValueRef branch, unsigned true_weight, unsigned false_weight) {
    LLVMMetadataRef md[3];
    unsigned kind = LLVMGetMDKindIDInContext(ir->ctx, "prof", 4);
    md[0] = LLVMMDStringInContext2(ir->ctx, "branch_weights", 14);
    md[1] = LLVMValueAsMetadata(ir_i32(ir, true_weight));
    md[2] = LLVMValueAsMetadata(ir_i32(ir, false_weight));
    LLVMSetMetadata(branch, kind, LLVMMetadataAsValue(ir->ctx, LLVMMDNodeInContext2(ir->ctx, md, 3)));
}

void ir_unlikely(SnxIr* ir, LLVMValueRef branch) { ir_branch_weights(ir, branch, 1, 2000); }
void ir_likely(SnxIr* ir, LLVMValueRef branch) { ir_branch_weights(ir, branch, 2000, 1); }

LLVMValueRef ir_intrinsic(SnxIr* ir, const char* name, LLVMTypeRef type, LLVMValueRef* args, unsigned count) {
    unsigned id = LLVMLookupIntrinsicID(name, strlen(name));
    LLVMValueRef function = LLVMGetIntrinsicDeclaration(ir->mod, id, type ? &type : NULL, type ? 1 : 0);
    return LLVMBuildCall2(ir->b, LLVMIntrinsicGetType(ir->ctx, id, type ? &type : NULL, type ? 1 : 0), function, args,
                          count, "");
}

/* the "memory" attribute: 2 bits per location (argument memory, inaccessible memory, other), each none/read/write */
static void memory_effects(SnxIr* ir, LLVMValueRef function, unsigned value) {
    unsigned kind = LLVMGetEnumAttributeKindForName("memory", 6);
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(ir->ctx, kind, value));
    kind = LLVMGetEnumAttributeKindForName("willreturn", 10);
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(ir->ctx, kind, 0));
}
void ir_pure(SnxIr* ir, LLVMValueRef function) { memory_effects(ir, function, 0); }
void ir_argmem(SnxIr* ir, LLVMValueRef function) { memory_effects(ir, function, 3); }
void ir_noregs(SnxIr* ir, LLVMValueRef function) {
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex,
                            LLVMCreateStringAttribute(ir->ctx, "snx-noregs", 10, "", 0));
}

void ir_stats_function(SnxIr* ir, LLVMValueRef fn) {
    if (!ir->stats.enabled) return;
    ++ir->stats.functions;
    for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(fn); block; block = LLVMGetNextBasicBlock(block))
        for (LLVMValueRef inst = LLVMGetFirstInstruction(block); inst; inst = LLVMGetNextInstruction(inst)) {
            if (!LLVMIsACallInst(inst)) continue;
            LLVMValueRef callee = LLVMGetCalledValue(inst);
            if (LLVMIsAFunction(callee) && LLVMGetIntrinsicID(callee)) continue;
            ++ir->stats.call_sites;
            if (!LLVMIsAFunction(callee)) {
                ++ir->stats.indirect_calls;
                continue;
            }
            size_t length;
            const char* name = LLVMGetValueName2(callee, &length);
            if (length >= 7 && !memcmp(name, "snx_fp_", 7)) ++ir->stats.fp_calls;
            else if (length >= 8 && !memcmp(name, "snx_vec_", 8)) ++ir->stats.vector_calls;
            else if ((length >= 2 && !memcmp(name, "F_", 2)) ||
                     (length >= 8 && !memcmp(name, "snx_aot_", 8))) ++ir->stats.guest_calls;
        }
}

LLVMBasicBlockRef ir_block(SnxIr* ir, const char* name) { return LLVMAppendBasicBlockInContext(ir->ctx, ir->fn, name); }

/* A local of the C ({ V128 r; ... } in the instruction's block) as an alloca of the whole function, zeroed where it is
   made. Without that, SROA reads the local's previous contents wherever the first write leaves part of it (a lane,
   r.s[i]=...) or has another type than the one SROA gives the local (a <16 x i8> store into a local it makes
   <4 x i32>), and mem2reg then carries the local around the whole function: a PHI per local in every block that
   joins paths (labels, the dispatch, loop heads). One game's large functions got 1388 PHIs in each such
   block, which InstCombine compares pairwise, for hours per code unit. The zeroing is a copy from a constant because
   SROA turns a copy of the whole local into a store of its own type, while a store or memset of another vector type
   loads the old value again. Every emitter writes all of a local before reading it, so the zero folds away */
LLVMValueRef ir_local(SnxIr* ir, LLVMTypeRef type) {
    LLVMBasicBlockRef here = LLVMGetInsertBlock(ir->b), entry = LLVMGetEntryBasicBlock(ir->fn);
    LLVMValueRef first = LLVMGetFirstInstruction(entry);
    unsigned long size = (unsigned long)LLVMABISizeOfType(LLVMGetModuleDataLayout(ir->mod), type);
    char name[32];
    if (first)
        LLVMPositionBuilderBefore(ir->b, first);
    else
        LLVMPositionBuilderAtEnd(ir->b, entry);
    LLVMValueRef slot = LLVMBuildAlloca(ir->b, type, "");
    LLVMSetAlignment(slot, 8);
    LLVMPositionBuilderAtEnd(ir->b, here);
    sprintf(name, "snx_zero%lu", size); /* one per size in each module */
    LLVMValueRef zero = LLVMGetNamedGlobal(ir->mod, name);
    if (!zero) {
        LLVMTypeRef bytes = LLVMArrayType(ir->t_i8, (unsigned)size);
        zero = LLVMAddGlobal(ir->mod, bytes, name);
        LLVMSetLinkage(zero, LLVMPrivateLinkage);
        LLVMSetUnnamedAddress(zero, LLVMGlobalUnnamedAddr);
        LLVMSetGlobalConstant(zero, 1);
        LLVMSetInitializer(zero, LLVMConstNull(bytes));
    }
    LLVMBuildMemCpy(ir->b, slot, 8, zero, 1, ir_i64(ir, (SnxU64)size));
    return slot;
}
