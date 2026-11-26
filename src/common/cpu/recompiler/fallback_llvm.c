/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The fallback made by LLVM (fallback.h), in builds with the backend. LLVM starts at the first thing asked of it, for
   the process. C in the target's GNU C23, for the LLVM-C headers */
#include "common/cpu/recompiler/fallback.h"
#include "common/cpu/arm/llvm.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/object/link.h"
#include "common/utilities/common.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int snx_fallback_available(void) { return 1; }
const char* snx_fallback_name(void) { return "LLVM"; }
SnxU64 snx_fallback_dump_pc(void) { return snx_ir_dump_pc(); }
int snx_fallback_stats(void) { return snx_ir_stats_on(); }

/* ---- A64 functions ---- */

/* The unit's functions [first, last) in an order that emits each one after the functions of the unit it calls
   directly (BL) or jumps to (B): a post-order of those calls, so ir_promote.c has a callee's summary when it promotes
   the calls of it. In a cycle of calls the first one reached goes before the callers it summarizes, which then call
   it as they call any other function. NULL out of memory (the address order then) */
static size_t* callees_first(SnxCodegen* codegen, size_t first, size_t last) {
    size_t n = last - first, top = 0, done = 0;
    size_t *order = (size_t*)malloc((n ? n : 1) * sizeof(size_t)),
           *stack = (size_t*)malloc((n ? n : 1) * sizeof(size_t));
    size_t* next = (size_t*)calloc(n ? n : 1, sizeof(size_t));   /* the instruction each function's walk goes on from */
    unsigned char* state = (unsigned char*)calloc(n ? n : 1, 1); /* 1 on the walk, 2 placed */
    if (!order || !stack || !next || !state) {
        free(order);
        order = NULL;
        goto end;
    }
    for (size_t root = 0; root < n; ++root) {
        if (state[root]) continue;
        state[root] = 1;
        stack[top++] = root;
        while (top) {
            size_t f = stack[top - 1];
            const SnxFunction* fn = &codegen->functions[first + f];
            size_t callee = n;
            while (next[f] < fn->count && callee == n) {
                SnxU64 address = snx_function_address(fn, next[f]++), target;
                SnxU32 word = 0;
                SnxA64Insn insn;
                const SnxFunction* to;
                if (!snx_codegen_word(codegen, address, &word)) continue;
                snx_a64_decode_word(word, codegen->features, &insn);
                if (insn.flow != SNX_A64_FLOW_BL && insn.flow != SNX_A64_FLOW_B) continue;
                target = snx_a64_branch_target(&insn, address);
                to = snx_codegen_function(codegen, target);
                if (!to || to->start != target) continue;
                if ((size_t)(to - codegen->functions) < first || (size_t)(to - codegen->functions) >= last) continue;
                if (!state[(size_t)(to - codegen->functions) - first])
                    callee = (size_t)(to - codegen->functions) - first;
            }
            if (callee < n) {
                state[callee] = 1;
                stack[top++] = callee;
            } else {
                state[f] = 2;
                order[done++] = first + f;
                --top;
            }
        }
    }
end:
    free(stack);
    free(next);
    free(state);
    return order;
}

/* name's IR kept in dir (--keep-ir): nonzero, with error saying which and where, when it could not be written */
static int save_ir(SnxIr* ir, const char* dir, const char* name, char* error, size_t error_size) {
    char *text = snx_ir_text(ir), *file = snx_format("%s.ll", name), *path = snx_path_join(dir, file);
    int rc = snx_write_file(path, text, strlen(text));
    if (rc) snprintf(error, error_size, "cannot save the IR of %s in %s", name, dir);
    LLVMDisposeMessage(text);
    free(file);
    free(path);
    return rc;
}

/* the unit's IR counts (ir_stats), as a recompile's line says them */
static char* stats_text(const SnxIrStats* s) {
    return snx_format(
        "%llu functions (%llu promoted), %llu registers made locals (%llu left in the CPU structure), "
        "%llu calls (%llu FP, %llu vector, %llu guest, %llu indirect), %llu sync stores (%llu at calls, "
        "%llu on edges, %llu bytes), %llu reloads (%llu bytes), %llu lookups, %llu polls, %llu stale "
        "checks, %llu return checks",
        (unsigned long long)s->functions, (unsigned long long)s->promoted_functions,
        (unsigned long long)s->promoted_slots, (unsigned long long)s->memory_slots, (unsigned long long)s->call_sites,
        (unsigned long long)s->fp_calls, (unsigned long long)s->vector_calls, (unsigned long long)s->guest_calls,
        (unsigned long long)s->indirect_calls, (unsigned long long)s->sync_stores, (unsigned long long)s->call_stores,
        (unsigned long long)s->edge_stores, (unsigned long long)s->store_bytes, (unsigned long long)s->sync_reloads,
        (unsigned long long)s->reload_bytes, (unsigned long long)s->lookups, (unsigned long long)s->polls,
        (unsigned long long)s->stale_checks, (unsigned long long)s->return_checks);
}

/* the unit's one global, an array of its functions' addresses in codegen's order: they are internal to it, so that two
   units' F_<address> do not clash in the loader's table, where every global stays. 0, or -1 out of memory */
static int unit_table(SnxIr* ir, SnxCodegen* codegen, const char* name) {
    LLVMValueRef* functions = (LLVMValueRef*)malloc(codegen->function_count * sizeof *functions);
    if (!functions) return -1;
    for (size_t i = 0; i < codegen->function_count; ++i) {
        functions[i] = snx_ir_guest_function(ir, codegen->functions[i].start);
        LLVMSetLinkage(functions[i], LLVMInternalLinkage);
        LLVMSetVisibility(functions[i], LLVMDefaultVisibility); /* a local symbol has no visibility */
    }
    LLVMTypeRef type = LLVMArrayType(ir->t_ptr, (unsigned)codegen->function_count);
    LLVMValueRef table = LLVMAddGlobal(ir->mod, type, name);
    LLVMSetInitializer(table, LLVMConstArray(ir->t_ptr, functions, (unsigned)codegen->function_count));
    LLVMSetGlobalConstant(table, 1);
    LLVMSetVisibility(table, LLVMHiddenVisibility);
    free(functions);
    return 0;
}

int snx_fallback_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                        const unsigned char* made, SnxFallbackUnit* unit, const char* name, char opt,
                        unsigned char** object, size_t* size, char* error, size_t error_size) {
    size_t left = 0;
    *object = NULL;
    *size = 0;
    if (unit) unit->stats = NULL;
    for (size_t i = first; i < last; ++i) left += !made || !made[i - first];
    if (!left) return 0;
    if (options->jit && (first || last != codegen->function_count || left != last - first)) {
        snprintf(error, error_size,
                 "%s: a JIT unit's functions are one object's, and %zu of codegen's %zu are asked for", name, left,
                 codegen->function_count);
        return -1;
    }
    const char* triple = unit ? unit->triple : NULL;
    SnxIr* ir = snx_ir_init(error, error_size) ? NULL
                : options->jit                 ? snx_ir_new_jit(name, opt, error, error_size)
                : triple                       ? snx_ir_new_target(name, opt, triple, error, error_size)
                                               : snx_ir_new(name, opt, error, error_size);
    if (!ir) return -1;
    if (unit) snx_ir_tune(ir, unit->tuning);
    ir->direct = options->direct;
    ir->direct_user = options->direct_user;
    ir->bound = options->bound;
    ir->bound_user = options->bound_user;
    if (options->stale) snx_ir_stale_checks(ir);
    size_t* order = unit && unit->callees_first ? callees_first(codegen, first, last) : NULL;
    for (size_t k = first; k < last; ++k) {
        size_t i = order ? order[k - first] : k;
        if (made && made[i - first]) continue;
        snx_ir_emit(ir, codegen, &codegen->functions[i]);
        if (unit && unit->rank && unit->rank[i] != SIZE_MAX) {
            char section[sizeof(SNX_LINK_HOT_TEXT) + 24];
            snprintf(section, sizeof section, SNX_LINK_HOT_TEXT "%zu", unit->rank[i]);
            LLVMSetSection(snx_ir_guest_function(ir, codegen->functions[i].start), section);
        }
    }
    free(order);
    int rc = 0;
    if (options->jit && unit_table(ir, codegen, options->table_name)) {
        snprintf(error, error_size, "out of memory");
        rc = -1;
    }
    if (unit && ir->stats.enabled) unit->stats = stats_text(&ir->stats);
    if (!rc && unit && unit->keep_ir) rc = save_ir(ir, unit->keep_ir, name, error, error_size);
    if (!rc) rc = snx_ir_object(ir, opt, object, size, error, error_size);
    snx_ir_free(ir);
    return rc;
}

int snx_fallback_rtlib(char opt, unsigned char** object, size_t* size, char* error, size_t error_size) {
    *object = NULL;
    *size = 0;
    return snx_ir_init(error, error_size) || snx_ir_rtlib_object(opt, object, size, error, error_size) ? -1 : 0;
}

int snx_fallback_start(char* error, size_t error_size) { return snx_ir_init(error, error_size); }

/* ---- AArch32 blocks ---- */

struct SnxFallbackArm {
    SnxIr* ir;            /* the context and target machine every block's module is made with, made for the first */
    LLVMModuleRef module; /* the blocks since the last object, NULL for none */
    /* the blocks made for their length alone, in a context of their own, made for the first of them */
    LLVMContextRef scratch_context;
    LLVMModuleRef scratch;
    char name[32];
    char opt;
    int jit;
};

SnxFallbackArm* snx_fallback_arm_new(const char* name, char opt, int jit) {
    SnxFallbackArm* f = (SnxFallbackArm*)calloc(1, sizeof *f);
    if (!f) return NULL;
    snprintf(f->name, sizeof f->name, "%s", name);
    f->opt = opt;
    f->jit = jit;
    return f;
}
void snx_fallback_arm_free(SnxFallbackArm* f) {
    if (!f) return;
    if (f->module) LLVMDisposeModule(f->module);
    if (f->ir) snx_ir_free(f->ir);
    if (f->scratch) LLVMDisposeModule(f->scratch);
    if (f->scratch_context) LLVMContextDispose(f->scratch_context);
    free(f);
}
int snx_fallback_arm_start(SnxFallbackArm* f, char* error, size_t error_size) {
    if (!f->ir && !snx_ir_init(error, error_size))
        f->ir = f->jit ? snx_ir_new_jit(f->name, f->opt, error, error_size)
                       : snx_ir_new(f->name, f->opt, error, error_size);
    return f->ir ? 0 : -1;
}
int snx_fallback_arm_block(SnxFallbackArm* f, const char* name, const void* code, size_t count, uint32_t pc, int thumb,
                           const SnxArmCodegenOptions* options, const char* why, size_t* consumed, char* error,
                           size_t error_size) {
    (void)why;
    *consumed = 0;
    if (snx_fallback_arm_start(f, error, error_size)) return -1;
    const int first = !f->module;
    if (first) {
        f->module = LLVMModuleCreateWithNameInContext(f->jit ? name : f->name, f->ir->ctx);
        LLVMSetTarget(f->module, LLVMGetTarget(f->ir->mod));
        LLVMSetDataLayout(f->module, LLVMGetDataLayoutStr(f->ir->mod));
    }
    LLVMValueRef fn =
        thumb ? snx_thumb_llvm_block_ex(f->module, name, (const uint16_t*)code, count, pc, consumed, options)
              : snx_arm_llvm_block_ex(f->module, name, (const uint32_t*)code, count, pc, consumed, options);
    if (fn) {
        /* an AOT module exports what its linker is told to, the descriptor or the blocks by name */
        if (!f->jit) LLVMSetVisibility(fn, LLVMHiddenVisibility);
        return 0;
    }
    if (first) {
        LLVMDisposeModule(f->module);
        f->module = NULL;
    }
    snprintf(error, error_size, "LLVM takes no such block");
    return -1;
}
int snx_fallback_arm_length(SnxFallbackArm* f, const void* code, size_t count, uint32_t pc, int thumb,
                            const SnxArmCodegenOptions* options, const char* why, size_t* consumed, char* error,
                            size_t error_size) {
    (void)why;
    *consumed = 0;
    if (!f->scratch) {
        if (!f->scratch_context) f->scratch_context = LLVMContextCreate();
        f->scratch = LLVMModuleCreateWithNameInContext(f->name, f->scratch_context);
    }
    LLVMValueRef fn =
        thumb ? snx_thumb_llvm_block_ex(f->scratch, "block", (const uint16_t*)code, count, pc, consumed, options)
              : snx_arm_llvm_block_ex(f->scratch, "block", (const uint32_t*)code, count, pc, consumed, options);
    if (!fn) {
        snprintf(error, error_size, "LLVM takes no such block");
        return -1;
    }
    LLVMDeleteFunction(fn);
    return 0;
}

/* Chaining, as the native back ends' chained blocks go on (cpu/arm/codegen.h's SnxArmChainedOptions): each return of
   the block becomes a return when it set an exit (an SVC, a fault), else for each successor a musttail call when r15
   and the Thumb state are its own, else the lookup. A successor at or before the block's start (every loop has one)
   first counts down the poll, and the block returns when it runs out, so the loop gets to run the rest */
static LLVMValueRef cpu_field(SnxIr* ir, LLVMValueRef cpu, size_t offset) {
    return LLVMBuildGEP2(ir->b, ir->t_i8, cpu, (LLVMValueRef[]){ir_i64(ir, offset)}, 1, "");
}
/* the function of the block named, declared where the module lacks it, for a musttail call. It keeps the caller's
   parameter attributes, and a hosted block's cpu is noalias */
static LLVMValueRef block_target(SnxIr* ir, LLVMModuleRef mod, const char* name) {
    LLVMValueRef target = LLVMGetNamedFunction(mod, name);
    if (!target) {
        LLVMTypeRef block_type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
        target = LLVMAddFunction(mod, name, block_type);
        LLVMSetVisibility(target, LLVMHiddenVisibility);
        LLVMAddAttributeAtIndex(target, 1 /* first parameter */,
                                LLVMCreateEnumAttribute(ir->ctx, LLVMGetEnumAttributeKindForName("noalias", 7), 0));
    }
    return target;
}
/* The end of a block's tail when no successor matched, an indirect branch. The target is looked up in the runtime's
   block table (SnxArmLookup), which its loop fills as it dispatches, and the block goes on to it by a musttail call
   when it is there and no IT block is under way. Else, or with no table, it returns to the loop. A target at or before
   the block's own start counts down the poll as a backward successor does: every cycle of blocks has one such edge, so
   a loop made of indirect branches still ends the slice */
static void chain_lookup(SnxIr* ir, const SnxArmChainedOptions* chained, LLVMValueRef fn, LLVMValueRef cpu,
                         uint32_t key, LLVMValueRef pc, LLVMValueRef cpsr, LLVMValueRef thumb, LLVMBasicBlockRef out) {
    LLVMTypeRef block_type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
    LLVMBasicBlockRef probe = LLVMAppendBasicBlockInContext(ir->ctx, fn, "probe"),
                      hit = LLVMAppendBasicBlockInContext(ir->ctx, fn, "hit"),
                      run = LLVMAppendBasicBlockInContext(ir->ctx, fn, "run");
    LLVMValueRef table = LLVMBuildLoad2(ir->b, ir->t_ptr, cpu_field(ir, cpu, chained->lookup), "");
    LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntNE, table, LLVMConstNull(ir->t_ptr), ""), probe, out);
    LLVMPositionBuilderAtEnd(ir->b, probe);
    LLVMValueRef target = LLVMBuildOr(ir->b, pc,
                                      LLVMBuildSelect(ir->b, LLVMBuildICmp(ir->b, LLVMIntNE, thumb, ir_i32(ir, 0), ""),
                                                      ir_i32(ir, 1), ir_i32(ir, 0), ""),
                                      "");
    LLVMValueRef slot = LLVMBuildZExt(
        ir->b,
        LLVMBuildAnd(ir->b, LLVMBuildLShr(ir->b, target, ir_i32(ir, 1), ""), ir_i32(ir, SNX_ARM_LOOKUP_SLOTS - 1), ""),
        ir->t_i64, "");
    LLVMValueRef entry =
        LLVMBuildGEP2(ir->b, ir->t_i8, table,
                      (LLVMValueRef[]){LLVMBuildMul(ir->b, slot, ir_i64(ir, sizeof(SnxArmLookup)), "")}, 1, "");
    LLVMValueRef seen = LLVMBuildLoad2(ir->b, ir->t_i32, entry, "");
    LLVMValueRef block = LLVMBuildLoad2(ir->b, ir->t_ptr, cpu_field(ir, entry, offsetof(SnxArmLookup, block)), "");
    /* ITSTATE sits in CPSR bits 26 and 25 and bits 15 to 10, and a hosted block is entered with it clear (llvm.h) */
    LLVMValueRef plain =
        LLVMBuildICmp(ir->b, LLVMIntEQ, LLVMBuildAnd(ir->b, cpsr, ir_i32(ir, 0x0600fc00u), ""), ir_i32(ir, 0), "");
    LLVMValueRef found = LLVMBuildAnd(ir->b, LLVMBuildICmp(ir->b, LLVMIntEQ, seen, target, ""),
                                      LLVMBuildICmp(ir->b, LLVMIntNE, block, LLVMConstNull(ir->t_ptr), ""), "");
    LLVMBuildCondBr(ir->b, LLVMBuildAnd(ir->b, found, plain, ""), hit, out);
    LLVMPositionBuilderAtEnd(ir->b, hit);
    LLVMValueRef at = cpu_field(ir, cpu, chained->poll);
    LLVMValueRef back = LLVMBuildICmp(ir->b, LLVMIntULE, pc, ir_i32(ir, key & ~1u), "");
    LLVMValueRef left =
        LLVMBuildSub(ir->b, LLVMBuildLoad2(ir->b, ir->t_i32, at, ""), LLVMBuildZExt(ir->b, back, ir->t_i32, ""), "");
    LLVMBuildStore(ir->b, left, at);
    LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntSGT, left, ir_i32(ir, 0), ""), run, out);
    LLVMPositionBuilderAtEnd(ir->b, run);
    LLVMValueRef site = LLVMBuildCall2(ir->b, block_type, block, &cpu, 1, "");
    LLVMSetTailCallKind(site, LLVMTailCallKindMustTail);
    LLVMAddCallSiteAttribute(site, 1 /* first parameter */,
                             LLVMCreateEnumAttribute(ir->ctx, LLVMGetEnumAttributeKindForName("noalias", 7), 0));
    LLVMBuildRetVoid(ir->b);
}
/* the block's returns through one tail, to its successors, the lookup or the loop: 0, -1 out of memory */
static int chain_block(SnxIr* ir, LLVMModuleRef mod, const SnxArmChainedOptions* chained, LLVMValueRef fn, uint32_t key,
                       const uint32_t* next, const char* const* names, size_t count) {
    LLVMTypeRef block_type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
    LLVMValueRef cpu = LLVMGetParam(fn, 0);
    LLVMValueRef* rets = malloc((LLVMCountBasicBlocks(fn) + 1) * sizeof *rets);
    size_t nrets = 0;
    if (!rets) return -1;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
        LLVMValueRef t = LLVMGetBasicBlockTerminator(bb);
        if (t && LLVMGetInstructionOpcode(t) == LLVMRet) rets[nrets++] = t;
    }
    /* one tail for all of them, which takes a block's exits back out */
    LLVMBasicBlockRef tail = LLVMAppendBasicBlockInContext(ir->ctx, fn, "chain"),
                      go = LLVMAppendBasicBlockInContext(ir->ctx, fn, "next"),
                      out = LLVMAppendBasicBlockInContext(ir->ctx, fn, "out");
    for (size_t i = 0; i < nrets; ++i) {
        LLVMPositionBuilderBefore(ir->b, rets[i]);
        LLVMBuildBr(ir->b, tail);
        LLVMInstructionEraseFromParent(rets[i]);
    }
    free(rets);
    LLVMPositionBuilderAtEnd(ir->b, out);
    LLVMBuildRetVoid(ir->b);
    LLVMPositionBuilderAtEnd(ir->b, tail);
    LLVMValueRef c = LLVMBuildLoad2(ir->b, ir->t_i32, cpu_field(ir, cpu, offsetof(SnxArmCPU, exit)), "");
    c = LLVMBuildICmp(ir->b, LLVMIntEQ, c, ir_i32(ir, SNX_ARM_OK), "");
    LLVMBuildCondBr(ir->b, c, go, out);
    LLVMPositionBuilderAtEnd(ir->b, go);
    LLVMValueRef pc = LLVMBuildLoad2(ir->b, ir->t_i32, cpu_field(ir, cpu, offsetof(SnxArmCPU, r) + 15 * 4), "");
    LLVMValueRef cpsr = LLVMBuildLoad2(ir->b, ir->t_i32, cpu_field(ir, cpu, offsetof(SnxArmCPU, cpsr)), "");
    LLVMValueRef t = LLVMBuildAnd(ir->b, cpsr, ir_i32(ir, SNX_ARM_T), "");
    for (size_t k = 0; k < count; ++k) {
        LLVMBasicBlockRef call = LLVMAppendBasicBlockInContext(ir->ctx, fn, "call"),
                          miss = LLVMAppendBasicBlockInContext(ir->ctx, fn, "other");
        c = LLVMBuildAnd(ir->b, LLVMBuildICmp(ir->b, LLVMIntEQ, pc, ir_i32(ir, next[k] & ~1u), ""),
                         LLVMBuildICmp(ir->b, LLVMIntEQ, t, ir_i32(ir, next[k] & 1 ? SNX_ARM_T : 0), ""), "");
        LLVMBuildCondBr(ir->b, c, call, miss);
        LLVMPositionBuilderAtEnd(ir->b, call);
        if (chained->poll && (next[k] & ~1u) <= (key & ~1u)) {
            LLVMBasicBlockRef run = LLVMAppendBasicBlockInContext(ir->ctx, fn, "run");
            LLVMValueRef at = cpu_field(ir, cpu, chained->poll);
            LLVMValueRef left = LLVMBuildSub(ir->b, LLVMBuildLoad2(ir->b, ir->t_i32, at, ""), ir_i32(ir, 1), "");
            LLVMBuildStore(ir->b, left, at);
            LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntSGT, left, ir_i32(ir, 0), ""), run, out);
            LLVMPositionBuilderAtEnd(ir->b, run);
        }
        LLVMValueRef site = LLVMBuildCall2(ir->b, block_type, block_target(ir, mod, names[k]), &cpu, 1, "");
        LLVMSetTailCallKind(site, LLVMTailCallKindMustTail);
        LLVMBuildRetVoid(ir->b);
        LLVMPositionBuilderAtEnd(ir->b, miss);
    }
    if (chained->lookup)
        chain_lookup(ir, chained, fn, cpu, key, pc, cpsr, t, out);
    else
        LLVMBuildBr(ir->b, out);
    return 0;
}
/* The block of an import's stub, which is the stub's svc instruction. Where the loop would take the SVC and then run
   the stub's bx lr, this calls the import through the cpu's direct function and goes on to the bx lr block (next, NULL
   where the module lacks it) when the call needs nothing from the loop. Without the function it is the SVC block as
   the block compiler makes it, with the immediate (the import's index) in the syndrome */
static void stub_block(SnxIr* ir, LLVMModuleRef mod, const SnxArmChainedOptions* chained, LLVMValueRef fn,
                       uint32_t after, const char* next) {
    LLVMTypeRef block_type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
    LLVMTypeRef params[2] = {ir->t_ptr, ir->t_i32};
    LLVMTypeRef call_type = LLVMFunctionType(ir->t_i32, params, 2, 0);
    /* the function stays, as blocks of the object may already call it. Its body goes, last instruction first */
    for (LLVMBasicBlockRef bb = LLVMGetLastBasicBlock(fn); bb; bb = LLVMGetPreviousBasicBlock(bb))
        for (LLVMValueRef in = LLVMGetLastInstruction(bb); in;) {
            LLVMValueRef before = LLVMGetPreviousInstruction(in);
            if (LLVMGetTypeKind(LLVMTypeOf(in)) != LLVMVoidTypeKind)
                LLVMReplaceAllUsesWith(in, LLVMGetUndef(LLVMTypeOf(in)));
            LLVMInstructionEraseFromParent(in);
            in = before;
        }
    while (LLVMGetFirstBasicBlock(fn)) LLVMDeleteBasicBlock(LLVMGetFirstBasicBlock(fn));
    LLVMValueRef cpu = LLVMGetParam(fn, 0);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"),
                      plain = LLVMAppendBasicBlockInContext(ir->ctx, fn, "svc"),
                      fast = LLVMAppendBasicBlockInContext(ir->ctx, fn, "call"),
                      go = LLVMAppendBasicBlockInContext(ir->ctx, fn, "next"),
                      out = LLVMAppendBasicBlockInContext(ir->ctx, fn, "out");
    LLVMPositionBuilderAtEnd(ir->b, entry);
    LLVMValueRef hook = LLVMBuildLoad2(ir->b, ir->t_ptr, cpu_field(ir, cpu, chained->direct), "");
    LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntEQ, hook, LLVMConstNull(ir->t_ptr), ""), plain, fast);
    LLVMPositionBuilderAtEnd(ir->b, plain);
    LLVMBuildStore(ir->b, ir_i32(ir, chained->import), cpu_field(ir, cpu, offsetof(SnxArmCPU, syndrome)));
    LLVMBuildStore(ir->b, ir_i32(ir, SNX_ARM_SVC), cpu_field(ir, cpu, offsetof(SnxArmCPU, exit)));
    LLVMBuildStore(ir->b, ir_i32(ir, after), cpu_field(ir, cpu, offsetof(SnxArmCPU, r) + 15 * 4));
    LLVMBuildRetVoid(ir->b);
    LLVMPositionBuilderAtEnd(ir->b, fast);
    /* r15 is past the svc while the import runs, as it is when the loop takes the SVC */
    LLVMBuildStore(ir->b, ir_i32(ir, after), cpu_field(ir, cpu, offsetof(SnxArmCPU, r) + 15 * 4));
    LLVMValueRef args[2] = {cpu, ir_i32(ir, chained->import)};
    LLVMValueRef done = LLVMBuildCall2(ir->b, call_type, hook, args, 2, "");
    LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntNE, done, ir_i32(ir, 0), ""), next ? go : out, out);
    LLVMPositionBuilderAtEnd(ir->b, go);
    if (next) {
        LLVMValueRef site = LLVMBuildCall2(ir->b, block_type, block_target(ir, mod, next), &cpu, 1, "");
        LLVMSetTailCallKind(site, LLVMTailCallKindMustTail);
    }
    LLVMBuildRetVoid(ir->b);
    LLVMPositionBuilderAtEnd(ir->b, out);
    LLVMBuildRetVoid(ir->b);
}
int snx_fallback_arm_chain(SnxFallbackArm* f, const char* name, uint32_t key, const SnxArmChainedOptions* chained,
                           const uint32_t* next, const char* const* names, size_t count, char* error,
                           size_t error_size) {
    LLVMValueRef fn = f->module ? LLVMGetNamedFunction(f->module, name) : NULL;
    if (!fn || !LLVMCountBasicBlocks(fn)) {
        snprintf(error, error_size, "%s: the block %s to chain is none of the object's", f->name, name);
        return -1;
    }
    if (!snx_arm_chained_options_valid(chained)) {
        snprintf(error, error_size, "%s: the block %s: chaining options no block compiler takes", f->name, name);
        return -1;
    }
    if (chained->direct && count > 1) {
        snprintf(error, error_size, "%s: the stub block %s goes on to %zu blocks, of 1 at most", f->name, name, count);
        return -1;
    }
    if (chained->direct) {
        const uint32_t after = (key & ~1u) + (key & 1 ? 2u : 4u);
        stub_block(f->ir, f->module, chained, fn, after, count ? names[0] : NULL);
        return 0;
    }
    if (chain_block(f->ir, f->module, chained, fn, key, next, names, count)) {
        snprintf(error, error_size, "%s: out of memory chaining the block %s", f->name, name);
        return -1;
    }
    return 0;
}
int snx_fallback_arm_ir(SnxFallbackArm* f, char** text, char* error, size_t error_size) {
    *text = NULL;
    if (!f->module) {
        snprintf(error, error_size, "%s: no block was made", f->name);
        return -1;
    }
    char* printed = LLVMPrintModuleToString(f->module);
    *text = snx_strdup(printed);
    LLVMDisposeMessage(printed);
    if (*text) return 0;
    snprintf(error, error_size, "%s: out of memory for the IR of its blocks", f->name);
    return -1;
}
int snx_fallback_arm_object(SnxFallbackArm* f, unsigned char** object, size_t* size, char* error, size_t error_size) {
    *object = NULL;
    *size = 0;
    if (!f->module) {
        snprintf(error, error_size, "%s: no block was made", f->name);
        return -1;
    }
    int rc = snx_ir_module_object(f->ir, f->module, f->opt, object, size, error, error_size);
    LLVMDisposeModule(f->module);
    f->module = NULL;
    return rc;
}

/* ---- data ---- */

struct SnxFallbackData {
    SnxIr* ir; /* made for the first global, with the module */
    LLVMModuleRef module;
    char opt;
    size_t module_name; /* where the module's name starts in names */
    char names[];       /* the unit's, then the module's */
};

SnxFallbackData* snx_fallback_data_new(const char* unit, const char* module, char opt, const char* why) {
    const size_t n = strlen(unit) + 1, m = strlen(module) + 1;
    SnxFallbackData* d = (SnxFallbackData*)calloc(1, sizeof *d + n + m);
    (void)why;
    if (!d) return NULL;
    memcpy(d->names, unit, n);
    memcpy(d->names + n, module, m);
    d->module_name = n;
    d->opt = opt;
    return d;
}
void snx_fallback_data_free(SnxFallbackData* d) {
    if (!d) return;
    if (d->module) LLVMDisposeModule(d->module);
    if (d->ir) snx_ir_free(d->ir);
    free(d);
}
/* the module, made for this machine as the unit's code is (ir.c): the target, CPU and ABI the helpers are compiled for,
   which the object is linked with. 0, or -1 with why not in error */
static int data_start(SnxFallbackData* d, char* error, size_t error_size) {
    if (d->module) return 0;
    if (snx_ir_init(error, error_size) || !(d->ir = snx_ir_new(d->names, d->opt, error, error_size))) return -1;
    d->module = LLVMModuleCreateWithNameInContext(d->names + d->module_name, d->ir->ctx);
    LLVMSetTarget(d->module, LLVMGetTarget(d->ir->mod));
    LLVMSetDataLayout(d->module, LLVMGetDataLayoutStr(d->ir->mod));
    return 0;
}
/* an array of count values of type, constant and internal to the object */
static void data_array(SnxFallbackData* d, const char* name, LLVMTypeRef type, LLVMValueRef* values, unsigned count) {
    LLVMValueRef g = LLVMAddGlobal(d->module, LLVMArrayType(type, count), name);
    LLVMSetInitializer(g, LLVMConstArray(type, values, count));
    LLVMSetGlobalConstant(g, 1);
    LLVMSetLinkage(g, LLVMInternalLinkage);
}
/* the values the arrays hold: count of them, 0 with the room in *values, -1 with the reason in error */
static int data_room(SnxFallbackData* d, const char* name, size_t count, LLVMValueRef** values, char* error,
                     size_t error_size) {
    *values = NULL;
    if (count > UINT32_MAX) {
        snprintf(error, error_size, "%s: %s of %zu values, of %u at most", d->names, name, count, UINT32_MAX);
        return -1;
    }
    if (data_start(d, error, error_size)) return -1;
    if ((*values = (LLVMValueRef*)malloc((count ? count : 1) * sizeof **values))) return 0;
    snprintf(error, error_size, "%s: out of memory for %s, of %zu values", d->names, name, count);
    return -1;
}
int snx_fallback_data_blocks(SnxFallbackData* d, const char* name, size_t count,
                             int (*name_of)(void* user, size_t i, char* out, size_t size), void* user, char* error,
                             size_t error_size) {
    LLVMValueRef* values;
    if (data_room(d, name, count, &values, error, error_size)) return -1;
    LLVMTypeRef ptr = d->ir->t_ptr, block_type = LLVMFunctionType(d->ir->t_void, &ptr, 1, 0);
    LLVMValueRef none = LLVMConstNull(ptr);
    for (size_t i = 0; i < count; ++i) {
        char id[64];
        if (!name_of(user, i, id, sizeof id)) {
            values[i] = none;
            continue;
        }
        if (!(values[i] = LLVMGetNamedFunction(d->module, id))) {
            values[i] = LLVMAddFunction(d->module, id, block_type);
            LLVMSetVisibility(values[i], LLVMHiddenVisibility);
        }
    }
    data_array(d, name, ptr, values, (unsigned)count);
    free(values);
    return 0;
}
/* the type of integers of bits bits, NULL for another width */
static LLVMTypeRef data_integer(SnxIr* ir, unsigned bits) {
    return bits == 8 ? ir->t_i8 : bits == 32 ? ir->t_i32 : bits == 64 ? ir->t_i64 : NULL;
}
int snx_fallback_data_integers(SnxFallbackData* d, const char* name, unsigned bits, size_t count, const void* values,
                               char* error, size_t error_size) {
    LLVMValueRef* items;
    if (data_room(d, name, count, &items, error, error_size)) return -1;
    LLVMTypeRef type = data_integer(d->ir, bits);
    if (!type) {
        snprintf(error, error_size, "%s: %s of integers of %u bits, which are 8, 32 or 64", d->names, name, bits);
        free(items);
        return -1;
    }
    for (size_t i = 0; i < count; ++i)
        items[i] = LLVMConstInt(type,
                                bits == 8    ? ((const uint8_t*)values)[i]
                                : bits == 32 ? ((const uint32_t*)values)[i]
                                             : ((const uint64_t*)values)[i],
                                0);
    data_array(d, name, type, items, (unsigned)count);
    free(items);
    return 0;
}
/* the field's type and, where value is not NULL, its value: 0, or -1 with the reason in error */
static int data_field(SnxFallbackData* d, const char* name, const SnxFallbackField* f, LLVMTypeRef* type,
                      LLVMValueRef* value, char* error, size_t error_size) {
    SnxIr* ir = d->ir;
    if (f->bits) {
        if (!(*type = data_integer(ir, f->bits))) {
            snprintf(error, error_size, "%s: %s has a field of %u bits, which are 8, 32 or 64", d->names, name,
                     f->bits);
            return -1;
        }
        if (value) *value = LLVMConstInt(*type, f->value, 0);
        return 0;
    }
    if (!f->bytes) {
        *type = ir->t_ptr;
        if (!value) return 0;
        *value = f->global ? LLVMGetNamedGlobal(d->module, f->global) : LLVMConstNull(ir->t_ptr);
        if (*value) return 0;
        snprintf(error, error_size, "%s: %s points to %s, which no global before it is", d->names, name, f->global);
        return -1;
    }
    *type = LLVMArrayType(ir->t_i8, (unsigned)f->count);
    if (!value) return 0;
    LLVMValueRef* bytes = (LLVMValueRef*)malloc((f->count ? f->count : 1) * sizeof *bytes);
    if (!bytes) {
        snprintf(error, error_size, "%s: out of memory for %s", d->names, name);
        return -1;
    }
    for (size_t i = 0; i < f->count; ++i) bytes[i] = LLVMConstInt(ir->t_i8, f->bytes[i], 0);
    *value = LLVMConstArray(ir->t_i8, bytes, (unsigned)f->count);
    free(bytes);
    return 0;
}
/* the struct type of the nfields fields, as C lays them out in size bytes: NULL with the reason in error */
static LLVMTypeRef data_type(SnxFallbackData* d, const char* name, const SnxFallbackField* fields, size_t nfields,
                             size_t size, char* error, size_t error_size) {
    LLVMTypeRef* types = (LLVMTypeRef*)malloc((nfields ? nfields : 1) * sizeof *types);
    LLVMTypeRef type = NULL;
    int rc = types ? 0 : -1;
    if (!types) snprintf(error, error_size, "%s: out of memory for %s", d->names, name);
    for (size_t j = 0; !rc && j < nfields; ++j)
        rc = data_field(d, name, &fields[j], &types[j], NULL, error, error_size);
    if (!rc) {
        type = LLVMStructTypeInContext(d->ir->ctx, types, (unsigned)nfields, 0);
        LLVMTargetDataRef layout = LLVMGetModuleDataLayout(d->module);
        size_t j = 0;
        while (j < nfields && LLVMOffsetOfElement(layout, type, (unsigned)j) == fields[j].offset) ++j;
        if (LLVMABISizeOfType(layout, type) != size || j < nfields) {
            snprintf(error, error_size,
                     "%s: %s is laid out otherwise for the target LLVM makes it for (%llu bytes, field %zu at %llu) "
                     "than in C (%zu bytes, field %zu at %zu)",
                     d->names, name, (unsigned long long)LLVMABISizeOfType(layout, type), j,
                     j < nfields ? (unsigned long long)LLVMOffsetOfElement(layout, type, (unsigned)j) : 0ull, size, j,
                     j < nfields ? fields[j].offset : 0);
            type = NULL;
        }
    }
    free(types);
    return type;
}
/* count structs of the type, from fields: their values into out, 0 or -1 with the reason in error */
static int data_values(SnxFallbackData* d, const char* name, LLVMTypeRef type, size_t count,
                       const SnxFallbackField* fields, size_t nfields, LLVMValueRef* out, char* error,
                       size_t error_size) {
    LLVMValueRef* values = (LLVMValueRef*)malloc((nfields ? nfields : 1) * sizeof *values);
    int rc = values ? 0 : -1;
    if (!values) snprintf(error, error_size, "%s: out of memory for %s", d->names, name);
    for (size_t i = 0; !rc && i < count; ++i) {
        LLVMTypeRef field;
        for (size_t j = 0; !rc && j < nfields; ++j)
            rc = data_field(d, name, &fields[i * nfields + j], &field, &values[j], error, error_size);
        if (!rc) out[i] = LLVMConstNamedStruct(type, values, (unsigned)nfields);
    }
    free(values);
    return rc;
}
int snx_fallback_data_structs(SnxFallbackData* d, const char* name, size_t count, const SnxFallbackField* fields,
                              size_t nfields, size_t size, char* error, size_t error_size) {
    LLVMValueRef* items;
    if (data_room(d, name, count, &items, error, error_size)) return -1;
    LLVMTypeRef type = data_type(d, name, fields, nfields, size, error, error_size);
    int rc = type ? data_values(d, name, type, count, fields, nfields, items, error, error_size) : -1;
    if (!rc) data_array(d, name, type, items, (unsigned)count);
    free(items);
    return rc;
}
int snx_fallback_data_descriptor(SnxFallbackData* d, const char* name, const SnxFallbackField* fields, size_t nfields,
                                 size_t size, char* error, size_t error_size) {
    LLVMValueRef value;
    if (data_start(d, error, error_size)) return -1;
    LLVMTypeRef type = data_type(d, name, fields, nfields, size, error, error_size);
    if (!type || data_values(d, name, type, 1, fields, nfields, &value, error, error_size)) return -1;
    LLVMValueRef global = LLVMAddGlobal(d->module, type, name);
    LLVMSetInitializer(global, value);
    LLVMSetGlobalConstant(global, 1);
    return 0;
}
int snx_fallback_data_ir(SnxFallbackData* d, char** text, char* error, size_t error_size) {
    *text = NULL;
    if (data_start(d, error, error_size)) return -1;
    char* printed = LLVMPrintModuleToString(d->module);
    *text = snx_strdup(printed);
    LLVMDisposeMessage(printed);
    if (*text) return 0;
    snprintf(error, error_size, "%s: out of memory for the IR of %s", d->names, d->names + d->module_name);
    return -1;
}
int snx_fallback_data_object(SnxFallbackData* d, unsigned char** object, size_t* size, char* error, size_t error_size) {
    *object = NULL;
    *size = 0;
    /* a module of data alone has no code to optimize */
    return data_start(d, error, error_size)
               ? -1
               : snx_ir_module_object(d->ir, d->module, '0', object, size, error, error_size);
}
