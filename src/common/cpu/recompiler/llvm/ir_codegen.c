/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest functions: the IR of a function codegen.c found, and of one instruction alone (snx_ir_static_step) */
#include "common/cpu/recompiler/llvm/ir_int.h"
#include "common/cpu/recompiler/llvm/ir_ldst.h"
#include "common/cpu/recompiler/llvm/ir_fpu.h"
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include "common/runtime/interface.h" /* SNX_SVC_COUNT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define B ir->b

static void attribute(SnxIr* ir, LLVMValueRef function, LLVMAttributeIndex index, const char* name, SnxU64 value) {
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    LLVMAddAttributeAtIndex(function, index, LLVMCreateEnumAttribute(ir->ctx, kind, value));
}

/* What Clang gives a function of the generated C: no unwinding through it, asynchronous unwind tables (as Clang
   emits by default on x86-64), CPU* RESTRICT, and no __chkstk on Windows, which the module doesn't link */
static void function_attributes(SnxIr* ir, LLVMValueRef function) {
    attribute(ir, function, LLVMAttributeFunctionIndex, "nounwind", 0);
    attribute(ir, function, LLVMAttributeFunctionIndex, "uwtable", 2);
    attribute(ir, function, 1, "noalias", 0);
    if (ir->windows)
        LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex,
                                LLVMCreateStringAttribute(ir->ctx, "no-stack-arg-probe", 18, "", 0));
}

LLVMValueRef snx_ir_guest_function(SnxIr* ir, SnxU64 address) {
    char name[32];
    sprintf(name, "F_%llX", (unsigned long long)address);
    LLVMValueRef function = LLVMGetNamedFunction(ir->mod, name);
    if (function) return function;
    function = LLVMAddFunction(ir->mod, name, ir->t_guest);
    LLVMSetVisibility(function, LLVMHiddenVisibility); /* -fvisibility=hidden */
    function_attributes(ir, function);
    return function;
}

void snx_ir_begin(SnxIr* ir, LLVMValueRef function) {
    ir_state_invalidate(ir);
    ir->fn = function;
    LLVMPositionBuilderAtEnd(B, LLVMAppendBasicBlockInContext(ir->ctx, function, "entry"));
    ir->c = LLVMGetParam(function, 0);
    ir->M = ir_cpu_load(ir, ir->t_ptr, ir_off_mem());
}

/* code after a return in the C is unreachable until the next label: give it a block of its own */
static void open_block(SnxIr* ir) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(B))) LLVMPositionBuilderAtEnd(B, ir_block(ir, "dead"));
}

static void ret(SnxIr* ir) { LLVMBuildRetVoid(B); }

/* A call the C follows with return: guaranteed to become a jump (-foptimize-sibling-calls only makes it likely), so
   a chain of guest tail calls never grows the host stack, whatever the optimization level */
static void must_tail(SnxIr* ir, LLVMValueRef site) {
    LLVMSetTailCallKind(site, LLVMTailCallKindMustTail);
    LLVMBuildRetVoid(ir->b);
}

/* CALLDYN(t): snx_lookup(c,t)(c,t) */
static LLVMValueRef call_dynamic(SnxIr* ir, LLVMValueRef target) {
    LLVMValueRef args[2];
    args[0] = ir->c;
    args[1] = target;
    LLVMValueRef function = ir_call_helper(ir, SNX_IR_LOOKUP, args, 2);
    ir_state_invalidate(ir);
    return LLVMBuildCall2(B, ir->t_guest, function, args, 2, "");
}

/* call(): F_target(c,target) when the target is a recompiled function the module doesn't route (snx_codegen_routed: a
   hook names its symbol) and direct calls aren't hooked (--hook-direct-calls), CALLDYN(target) otherwise, so a hook in
   a routed function's slot sees every call to it; tail: then return. The runtime JIT also calls the AOT module's
   functions directly (ir->direct says which have one, jit_service.c), through snx_aot_<target>: a symbol its loader
   resolves at each load, so the object holds no host address and a later run can load it again (the JIT's cache) */
static void call(SnxIr* ir, const SnxCodegen* codegen, SnxU64 target, int tail) {
    LLVMValueRef args[2], site, aot;
    char name[40];
    args[0] = ir->c;
    args[1] = ir_i64(ir, target);
    int through_slot = codegen->hook_direct_calls || snx_codegen_routed(codegen, target);
    if (!through_slot && snx_codegen_function(codegen, target))
        site = LLVMBuildCall2(B, ir->t_guest, snx_ir_guest_function(ir, target), args, 2, "");
    else if (!through_slot && ir->direct && ir->direct(ir->direct_user, target) != 0) {
        sprintf(name, "snx_aot_%llx", (unsigned long long)target);
        aot = LLVMGetNamedFunction(ir->mod, name);
        if (!aot) aot = LLVMAddFunction(ir->mod, name, ir->t_guest);
        site = LLVMBuildCall2(B, ir->t_guest, aot, args, 2, "");
    } else
        site = call_dynamic(ir, args[1]);
    ir_state_invalidate(ir);
    if (tail) must_tail(ir, site);
}

static size_t index_of(const SnxFunction* function, SnxU64 address) {
    size_t lo = 0, hi = function->count, middle;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
        if (snx_function_address(function, middle) < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo;
}

/* if(UNLIKELY(__atomic_load_n(&c->poll,__ATOMIC_RELAXED))){ [c->pc=resume;] c->rt->poll(c); } (codegen.c): a loop
   that makes no SVC can still be taken off its core, for a waiter of higher priority.
   The compiler barrier after the flag's load (a single-thread fence: no instruction on any host) keeps every guest
   load of the loop in it. Guest loads aren't volatile (ir_state.c's ir_rd), so without it LLVM's GVN sees the call on
   the cold path as the loop's only write, and moves the load of an address the loop builds from constants (ADRP, MOV)
   onto that path: a spin on memory another thread writes then spins on the poll flag alone, and sees the store only
   when a waiter of higher priority flags its core, or never without emulated cores (tests/recompiler_library.c) */
static void poll(SnxIr* ir, const SnxU64* resume) {
    LLVMBasicBlockRef yes = ir_block(ir, "poll"), no = ir_block(ir, "run");
    LLVMValueRef flag = LLVMBuildLoad2(B, ir->t_i32, ir_cpu(ir, ir_off_poll()), ""), args[1];
    LLVMTypeRef param = ir->t_ptr;
    LLVMSetOrdering(flag, LLVMAtomicOrderingMonotonic);
    LLVMSetAlignment(flag, 4);
    ir_tag_cpu(ir, flag);
    LLVMBuildFence(B, LLVMAtomicOrderingAcquire, 1, "");
    ir_unlikely(ir, LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntNE, flag, ir_i32(ir, 0), ""), yes, no));
    LLVMPositionBuilderAtEnd(B, yes);
    if (resume) ir_set_pc(ir, ir_i64(ir, *resume));
    args[0] = ir->c;
    ir_rt_call(ir, ir_rt_poll(), ir->t_void, &param, args, 1);
    LLVMBuildBr(B, no);
    LLVMPositionBuilderAtEnd(B, no);
}

/* branch(): goto L_target inside the function (polling first when it goes backward), a tail call outside it */
static void branch(SnxIr* ir, const SnxCodegen* codegen, const SnxFunction* function, LLVMBasicBlockRef* labels,
                   SnxU64 address, SnxU64 target) {
    if (snx_function_contains(function, target)) {
        if (target <= address) poll(ir, &target);
        LLVMBuildBr(B, labels[index_of(function, target)]);
    } else
        call(ir, codegen, target, 1);
}

/* snx_stale[ir->stale_index] != 0, read with a relaxed load (the runtime sets it while guest threads run) */
static LLVMValueRef stale_flag(SnxIr* ir) {
    LLVMValueRef at = ir_i64(ir, ir->stale_index);
    LLVMValueRef flag = LLVMBuildLoad2(B, ir->t_i8, LLVMBuildInBoundsGEP2(B, ir->t_i8, ir->stale, &at, 1, ""), "");
    LLVMSetOrdering(flag, LLVMAtomicOrderingMonotonic);
    LLVMSetAlignment(flag, 1);
    return LLVMBuildICmp(B, LLVMIntNE, flag, LLVMConstInt(ir->t_i8, 0, 0), "");
}

/* if(UNLIKELY(c->pc!=pc+4)) return; and in the AOT build if(UNLIKELY(snx_stale[index])) return; with c->pc at pc+4:
   the guest changed this function's code while the call ran (a mod's hooks, written from inside the call: ozone's
   MountRom hook loads the plugins that patch its caller). The callers return as for any other pc, and the thread's
   loop runs pc+4 through dispatch, compiled from the code there now, as the console's CPU fetches it */
static void check_return(SnxIr* ir, SnxU64 next) {
    LLVMBasicBlockRef leave = ir_block(ir, "leave"), stay = ir_block(ir, "stay");
    ir_unlikely(ir, LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntNE, ir_pc(ir), ir_i64(ir, next), ""), leave, stay));
    LLVMPositionBuilderAtEnd(B, leave);
    ret(ir);
    LLVMPositionBuilderAtEnd(B, stay);
    if (ir->stale) {
        LLVMBasicBlockRef gone = ir_block(ir, "stale_return"), live = ir_block(ir, "live_return");
        ir_unlikely(ir, LLVMBuildCondBr(B, stale_flag(ir), gone, live));
        LLVMPositionBuilderAtEnd(B, gone);
        ret(ir); /* c->pc is pc+4, which the callee's return set */
        LLVMPositionBuilderAtEnd(B, live);
    }
}

/* SVC #imm: c->rt->svcs[imm](c, imm), the SVC's own entry, in an NX module (codegen->direct_svc), else
   c->rt->svc(c, imm) */
static void svc(SnxIr* ir, const SnxCodegen* codegen, SnxU64 pc, SnxU32 word, LLVMValueRef imm) {
    LLVMTypeRef params[2];
    LLVMValueRef args[2];
    unsigned n = (word >> 5) & 0xFFFF;
    ir_set_pc(ir, ir_i64(ir, pc + 4));
    params[0] = ir->t_ptr;
    params[1] = ir->t_i32;
    args[0] = ir->c;
    args[1] = imm;
    if (codegen && codegen->direct_svc && n < SNX_SVC_COUNT)
        ir_rt_table_call(ir, ir_rt_svcs(), n, ir->t_void, params, args, 2);
    else
        ir_rt_call(ir, ir_rt_svc(), ir->t_void, params, args, 2);
}

static void brk(SnxIr* ir, SnxU64 pc, LLVMValueRef imm) {
    LLVMTypeRef params[3];
    LLVMValueRef args[3];
    params[0] = ir->t_ptr;
    params[1] = ir->t_i64;
    params[2] = ir->t_i32;
    args[0] = ir->c;
    args[1] = ir_i64(ir, pc);
    args[2] = imm;
    ir_rt_call(ir, ir_rt_brk(), ir->t_void, params, args, 3);
}

/* The body of a PLAIN instruction: ir_int.c's, ir_ldst.c's, ir_fpu.c's or ir_vec.c's (with ir_vec_fp.c and
   ir_vec_crypto.c). One without an emitter is noted, and snx_ir_object refuses the module; decode makes such a word
   UNDEF first */
static void plain(SnxIr* ir, SnxU32 word, SnxU64 pc, const SnxA64Insn* insn) {
    if (snx_ir_int_handles(insn->op)) {
        snx_ir_int(ir, insn, word, pc);
        return;
    }
    if (snx_ir_ldst_handles(insn->op)) {
        snx_ir_ldst(ir, insn, word, pc);
        return;
    }
    if (snx_ir_fpu_handles(insn->op)) {
        snx_ir_fpu(ir, insn, word, pc);
        return;
    }
    if (snx_ir_vec_handles(insn->op)) {
        snx_ir_vec(ir, insn, word, pc);
        return;
    }
    if (!ir->fallbacks++) {
        ir->fallback_word = word;
        ir->fallback_pc = pc;
    }
}

void snx_ir_stale_checks(SnxIr* ir) {
    LLVMValueRef g = LLVMGetNamedGlobal(ir->mod, SNX_IR_STALE);
    if (!g) {
        g = LLVMAddGlobal(ir->mod, LLVMArrayType(ir->t_i8, 0), SNX_IR_STALE);
        LLVMSetLinkage(g, LLVMExternalLinkage);
        LLVMSetVisibility(g, LLVMHiddenVisibility); /* the module's own: reached PC-relative, not through the GOT */
    }
    ir->stale = g;
}

/* if(UNLIKELY(__atomic_load_n(&snx_stale[index],__ATOMIC_RELAXED))){ CALLDYN(e); return; }: the guest changed code
   this function was compiled from (RtAPI.code_changed), and the runtime has taken its entries out of dispatch, where
   a lookup of e compiles the code there now */
static void stale_check(SnxIr* ir, LLVMValueRef e) {
    LLVMBasicBlockRef stale = ir_block(ir, "stale"), live = ir_block(ir, "live");
    ir_unlikely(ir, LLVMBuildCondBr(B, stale_flag(ir), stale, live));
    LLVMPositionBuilderAtEnd(B, stale);
    must_tail(ir, call_dynamic(ir, e));
    LLVMPositionBuilderAtEnd(B, live);
}

/* the operand of a flow instruction: decode lets through only the ops ir_int.c has one for */
static LLVMValueRef flow_operand(SnxIr* ir, const SnxA64Insn* insn) { return snx_ir_int_operand(ir, insn); }

/* What word does to the flow for a guest with these features, insn its decode. ir_ldst.c, ir_fpu.c and ir_vec.c
   implement their ops for every feature the decode allows (the LSE and RCpc atomics, FEAT_FP16's halves, FEAT_JSCVT,
   FEAT_FRINTTS, BFCVT, the AdvSIMD extensions); ir_int.c implements the Switch's instructions, so one of its ops that
   needs another feature is UNDEF until it does, as is an op no emitter has (the coverage report lists them): as a
   word no row matches */
static SnxA64Flow decode(SnxU32 word, SnxU64 features, SnxA64Insn* insn) {
    snx_a64_decode_word(word, features, insn);
    switch (insn->flow) {
        case SNX_A64_FLOW_UNDEF: return SNX_A64_FLOW_UNDEF;
        case SNX_A64_FLOW_B:
        case SNX_A64_FLOW_BL: return insn->flow;
        default: break;
    }
    if (snx_ir_ldst_handles(insn->op) || snx_ir_fpu_handles(insn->op) || snx_ir_vec_handles(insn->op))
        return insn->flow;
    if (snx_ir_int_handles(insn->op) && !(insn->feature & ~(SnxU64)SNX_A64_TARGET_SWITCH)) return insn->flow;
    return SNX_A64_FLOW_UNDEF;
}

void snx_ir_emit(SnxIr* ir, SnxCodegen* codegen, const SnxFunction* function) {
    LLVMValueRef fn = snx_ir_guest_function(ir, function->start), sw;
    LLVMBasicBlockRef dispatch_block = NULL;
    SnxU64 address, next, target;
    SnxU32 word;
    SnxA64Insn insn;
    SnxA64Flow flow;
    int terminal;
    LLVMBasicBlockRef* labels = (LLVMBasicBlockRef*)calloc(function->count ? function->count : 1, sizeof(*labels));
    ir->guest_features = snx_a64_features(codegen->features);
    snx_ir_begin(ir, fn);
    LLVMValueRef e = LLVMGetParam(fn, 1);
    if (ir->stale) {
        ir->stale_index = (size_t)(function - codegen->functions);
        stale_check(ir, e);
    }
    for (size_t i = 0; i < function->count; ++i)
        if (function->leaders[i]) labels[i] = ir_block(ir, "L");
    int dispatch = function->leader_count > 1;
    if (dispatch) dispatch_block = ir_block(ir, "D");
    /* if(UNLIKELY(e!=start)){ c->pc=e; goto D_; } or { CALLDYN(e); return; } */
    LLVMBasicBlockRef mismatch = ir_block(ir, "entry_mismatch");
    LLVMBasicBlockRef body = ir_block(ir, "body");
    ir_unlikely(ir,
                LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntNE, e, ir_i64(ir, function->start), ""), mismatch, body));
    LLVMPositionBuilderAtEnd(B, mismatch);
    if (dispatch) {
        ir_set_pc(ir, e);
        LLVMBuildBr(B, dispatch_block);
    } else {
        must_tail(ir, call_dynamic(ir, e));
    }
    LLVMPositionBuilderAtEnd(B, body);
    if (function->count && snx_function_address(function, 0) != function->start)
        LLVMBuildBr(B, labels[index_of(function, function->start)]);
    for (size_t i = 0; i < function->count; ++i) {
        address = snx_function_address(function, i);
        next = address + 4;
        if (function->leaders[i]) {
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(B))) LLVMBuildBr(B, labels[i]);
            LLVMPositionBuilderAtEnd(B, labels[i]);
        }
        open_block(ir);
        word = 0;
        (void)snx_codegen_word(codegen, address, &word);
        flow = decode(word, codegen->features, &insn);
        if (flow == SNX_A64_FLOW_UNDEF) {
            ir_set_pc(ir, ir_i64(ir, address));
            ir_undef(ir, address, word);
            ret(ir);
            continue;
        }
        terminal = 0;
        switch (flow) {
            case SNX_A64_FLOW_PLAIN: plain(ir, word, address, &insn); break;
            case SNX_A64_FLOW_B:
                branch(ir, codegen, function, labels, address, snx_a64_branch_target(&insn, address));
                terminal = 1;
                break;
            case SNX_A64_FLOW_BC: {
                LLVMBasicBlockRef taken = ir_block(ir, "taken"), fall = ir_block(ir, "fall");
                LLVMValueRef site = LLVMBuildCondBr(B, flow_operand(ir, &insn), taken, fall);
                ir_profile_branch(ir, site, address, word);
                LLVMPositionBuilderAtEnd(B, taken);
                branch(ir, codegen, function, labels, address, snx_a64_branch_target(&insn, address));
                LLVMPositionBuilderAtEnd(B, fall);
                break;
            }
            case SNX_A64_FLOW_BL:
                target = snx_a64_branch_target(&insn, address);
                ir_set_x(ir, 30, ir_i64(ir, next));
                call(ir, codegen, target, 0);
                check_return(ir, next);
                break;
            case SNX_A64_FLOW_BLR: {
                LLVMValueRef t = flow_operand(ir, &insn);
                ir_set_x(ir, 30, ir_i64(ir, next));
                call_dynamic(ir, t);
                check_return(ir, next);
                break;
            }
            case SNX_A64_FLOW_BR: {
                LLVMValueRef t = flow_operand(ir, &insn);
                if (dispatch) {
                    ir_set_pc(ir, t);
                    LLVMBuildBr(B, dispatch_block);
                } else {
                    must_tail(ir, call_dynamic(ir, t));
                }
                terminal = 1;
                break;
            }
            case SNX_A64_FLOW_RET:
                ir_set_pc(ir, flow_operand(ir, &insn));
                ret(ir);
                terminal = 1;
                break;
            case SNX_A64_FLOW_SVC:
                svc(ir, codegen, address, word, flow_operand(ir, &insn));
                break;
            case SNX_A64_FLOW_BRK:
                ir_set_pc(ir, ir_i64(ir, address));
                brk(ir, address, flow_operand(ir, &insn));
                ret(ir);
                terminal = 1;
                break;
            case SNX_A64_FLOW_UNDEF:
            case SNX_A64_FLOW_ERET: /* EL1: UNDEF at EL0 (snx_a64_features) */
            case SNX_A64_FLOW_EXC: break;
        }
        if (!terminal && (!snx_function_contains(function, next) ||
                          (i + 1 < function->count && snx_function_address(function, i + 1) != next))) {
            open_block(ir);
            call(ir, codegen, next, 1);
        }
    }
    /* the C function falls off its end */
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(B))) ret(ir);
    if (dispatch) {
        /* D_: if(UNLIKELY(poll)) c->rt->poll(c); switch(c->pc){ case <leader>: goto L_<leader>; ...
           default: CALLDYN(c->pc); return; } */
        LLVMBasicBlockRef other = ir_block(ir, "D_default");
        LLVMPositionBuilderAtEnd(B, dispatch_block);
        poll(ir, NULL);
        LLVMValueRef pc = ir_pc(ir);
        sw = LLVMBuildSwitch(B, pc, other, (unsigned)function->leader_count);
        for (size_t i = 0; i < function->count; ++i)
            if (function->leaders[i]) LLVMAddCase(sw, ir_i64(ir, snx_function_address(function, i)), labels[i]);
        LLVMPositionBuilderAtEnd(B, other);
        must_tail(ir, call_dynamic(ir, ir_pc(ir)));
    }
    /* At O0 LLVM runs no SROA or mem2reg, so the locals promotion makes stay stack slots and its site copies remain:
       call-heavy code regresses and the JIT's batch arena grows. O1 and above only (ir_promote.c) */
    if (ir->promote) ir_promote_registers(ir, fn);
    free(labels);
}

LLVMValueRef snx_ir_static_step(SnxIr* ir, SnxU32 word, SnxU64 pc, SnxU64 features, const char* name) {
    LLVMTypeRef param = ir->t_ptr;
    LLVMValueRef fn = LLVMAddFunction(ir->mod, name, LLVMFunctionType(ir->t_i64, &param, 1, 0)), operand;
    SnxA64Insn insn;
    SnxA64Flow flow = decode(word, features, &insn);
    function_attributes(ir, fn);
    ir->guest_features = snx_a64_features(features);
    snx_ir_begin(ir, fn);
    switch (flow) {
        case SNX_A64_FLOW_PLAIN:
            plain(ir, word, pc, &insn);
            LLVMBuildRet(B, ir_i64(ir, pc + 4));
            break;
        case SNX_A64_FLOW_UNDEF:
        case SNX_A64_FLOW_ERET:
        case SNX_A64_FLOW_EXC:
            ir_undef(ir, pc, word);
            LLVMBuildRet(B, ir_i64(ir, pc));
            break;
        case SNX_A64_FLOW_B: LLVMBuildRet(B, ir_i64(ir, snx_a64_branch_target(&insn, pc))); break;
        case SNX_A64_FLOW_BR:
        case SNX_A64_FLOW_RET: LLVMBuildRet(B, flow_operand(ir, &insn)); break;
        case SNX_A64_FLOW_BC:
            LLVMBuildRet(B, LLVMBuildSelect(B, flow_operand(ir, &insn),
                                            ir_i64(ir, snx_a64_branch_target(&insn, pc)), ir_i64(ir, pc + 4), ""));
            break;
        case SNX_A64_FLOW_BL:
        case SNX_A64_FLOW_BLR:
            /* { u64 t_=<target>; c->x[30]=pc+4; return t_; } */
            operand = flow == SNX_A64_FLOW_BL ? ir_i64(ir, snx_a64_branch_target(&insn, pc))
                                              : flow_operand(ir, &insn);
            ir_set_x(ir, 30, ir_i64(ir, pc + 4));
            LLVMBuildRet(B, operand);
            break;
        case SNX_A64_FLOW_SVC:
            svc(ir, NULL, pc, word, flow_operand(ir, &insn));
            LLVMBuildRet(B, ir_i64(ir, pc + 4));
            break;
        case SNX_A64_FLOW_BRK:
            brk(ir, pc, flow_operand(ir, &insn));
            LLVMBuildRet(B, ir_i64(ir, pc));
            break;
    }
    return fn;
}
