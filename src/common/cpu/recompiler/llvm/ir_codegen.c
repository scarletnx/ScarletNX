/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest functions: the IR of what snx_codegen_emit (codegen.c) prints for a function, and of snx_a64_static_step */
#include "common/cpu/recompiler/llvm/ir.h"
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
    LLVMValueRef function;
    if (address >> 32)
        sprintf(name, "F_%lX%08lX", (unsigned long)(address >> 32), (unsigned long)(address & 0xFFFFFFFFUL));
    else
        sprintf(name, "F_%lX", (unsigned long)address);
    function = LLVMGetNamedFunction(ir->mod, name);
    if (function) return function;
    function = LLVMAddFunction(ir->mod, name, ir->t_guest);
    LLVMSetVisibility(function, LLVMHiddenVisibility); /* -fvisibility=hidden */
    function_attributes(ir, function);
    return function;
}

void snx_ir_begin(SnxIr* ir, LLVMValueRef function) {
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
    LLVMValueRef args[2], function;
    args[0] = ir->c;
    args[1] = target;
    function = ir_call_helper(ir, SNX_IR_LOOKUP, args, 2);
    return LLVMBuildCall2(B, ir->t_guest, function, args, 2, "");
}

/* call(): F_target(c,target) when the target is a recompiled function and direct calls aren't hooked
   (--hook-direct-calls), CALLDYN(target) otherwise; tail: then return */
static void call(SnxIr* ir, const SnxCodegen* codegen, SnxU64 target, int tail) {
    LLVMValueRef args[2], site;
    args[0] = ir->c;
    args[1] = ir_i64(ir, target);
    if (snx_codegen_function(codegen, target) && !codegen->hook_direct_calls)
        site = LLVMBuildCall2(B, ir->t_guest, snx_ir_guest_function(ir, target), args, 2, "");
    else
        site = call_dynamic(ir, args[1]);
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
   that makes no SVC can still be taken off its core, for a waiter of higher priority */
static void poll(SnxIr* ir, const SnxU64* resume) {
    LLVMBasicBlockRef yes = ir_block(ir, "poll"), no = ir_block(ir, "run");
    LLVMValueRef flag = LLVMBuildLoad2(B, ir->t_i32, ir_cpu(ir, ir_off_poll()), ""), args[1];
    LLVMTypeRef param = ir->t_ptr;
    LLVMSetOrdering(flag, LLVMAtomicOrderingMonotonic);
    LLVMSetAlignment(flag, 4);
    ir_tag_cpu(ir, flag);
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

/* if(UNLIKELY(c->pc!=pc+4)) return; */
static void check_return(SnxIr* ir, SnxU64 next) {
    LLVMBasicBlockRef leave = ir_block(ir, "leave"), stay = ir_block(ir, "stay");
    ir_unlikely(ir, LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntNE, ir_pc(ir), ir_i64(ir, next), ""), leave, stay));
    LLVMPositionBuilderAtEnd(B, leave);
    ret(ir);
    LLVMPositionBuilderAtEnd(B, stay);
}

static void svc(SnxIr* ir, SnxU64 pc, LLVMValueRef imm) {
    LLVMTypeRef params[2];
    LLVMValueRef args[2];
    ir_set_pc(ir, ir_i64(ir, pc + 4));
    params[0] = ir->t_ptr;
    params[1] = ir->t_i32;
    args[0] = ir->c;
    args[1] = imm;
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

/* The body of a PLAIN instruction; without an emitter, the interpreter's step function runs it */
static void plain(SnxIr* ir, SnxU32 word, SnxU64 pc, const SnxA64Case* instruction) {
    LLVMTypeRef params[3];
    LLVMValueRef args[3];
    if (snx_ir_plain(ir, word, pc, instruction)) return;
    params[0] = ir->t_ptr;
    params[1] = ir->t_i32;
    params[2] = ir->t_i64;
    args[0] = ir->c;
    args[1] = ir_i32(ir, word);
    args[2] = ir_i64(ir, pc);
    ir_call(ir, ir_extern(ir, "snx_step", ir->t_i64, params, 3), args, 3);
    ++ir->fallbacks;
}

void snx_ir_emit(SnxIr* ir, SnxCodegen* codegen, const SnxFunction* function) {
    LLVMValueRef fn = snx_ir_guest_function(ir, function->start), e, sw;
    LLVMBasicBlockRef *labels, mismatch, body, dispatch_block = NULL;
    size_t i;
    SnxU64 address, next, target;
    SnxU32 word;
    const SnxA64Case* c;
    int dispatch, terminal;
    labels = (LLVMBasicBlockRef*)calloc(function->count ? function->count : 1, sizeof(*labels));
    snx_ir_begin(ir, fn);
    e = LLVMGetParam(fn, 1);
    for (i = 0; i < function->count; ++i)
        if (function->leaders[i]) labels[i] = ir_block(ir, "L");
    dispatch = function->leader_count > 1;
    if (dispatch) dispatch_block = ir_block(ir, "D");
    /* if(UNLIKELY(e!=start)){ c->pc=e; goto D_; } or { CALLDYN(e); return; } */
    mismatch = ir_block(ir, "entry_mismatch");
    body = ir_block(ir, "body");
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
    for (i = 0; i < function->count; ++i) {
        address = snx_function_address(function, i);
        next = address + 4;
        if (function->leaders[i]) {
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(B))) LLVMBuildBr(B, labels[i]);
            LLVMPositionBuilderAtEnd(B, labels[i]);
        }
        open_block(ir);
        word = 0;
        if (!snx_codegen_word(codegen, address, &word))
            c = snx_a64_decode(0);
        else
            c = snx_a64_decode(word);
        if (c->kind == SNX_FLOW_UNDEF) {
            ir_set_pc(ir, ir_i64(ir, address));
            ir_undef(ir, address, word);
            ret(ir);
            continue;
        }
        terminal = 0;
        switch (c->kind) {
            case SNX_FLOW_PLAIN: plain(ir, word, address, c); break;
            case SNX_FLOW_B:
                branch(ir, codegen, function, labels, address, snx_a64_target(word, address));
                terminal = 1;
                break;
            case SNX_FLOW_BC: {
                LLVMBasicBlockRef taken = ir_block(ir, "taken"), fall = ir_block(ir, "fall");
                LLVMBuildCondBr(B, snx_ir_flow_operand(ir, word, address, c), taken, fall);
                LLVMPositionBuilderAtEnd(B, taken);
                branch(ir, codegen, function, labels, address, snx_a64_target(word, address));
                LLVMPositionBuilderAtEnd(B, fall);
                break;
            }
            case SNX_FLOW_BL:
                target = snx_a64_target(word, address);
                ir_set_x(ir, 30, ir_i64(ir, next));
                call(ir, codegen, target, 0);
                check_return(ir, next);
                break;
            case SNX_FLOW_BLR: {
                LLVMValueRef t = snx_ir_flow_operand(ir, word, address, c);
                ir_set_x(ir, 30, ir_i64(ir, next));
                call_dynamic(ir, t);
                check_return(ir, next);
                break;
            }
            case SNX_FLOW_BR: {
                LLVMValueRef t = snx_ir_flow_operand(ir, word, address, c);
                if (dispatch) {
                    ir_set_pc(ir, t);
                    LLVMBuildBr(B, dispatch_block);
                } else {
                    must_tail(ir, call_dynamic(ir, t));
                }
                terminal = 1;
                break;
            }
            case SNX_FLOW_RET:
                ir_set_pc(ir, snx_ir_flow_operand(ir, word, address, c));
                ret(ir);
                terminal = 1;
                break;
            case SNX_FLOW_SVC: svc(ir, address, snx_ir_flow_operand(ir, word, address, c)); break;
            case SNX_FLOW_BRK:
                ir_set_pc(ir, ir_i64(ir, address));
                brk(ir, address, snx_ir_flow_operand(ir, word, address, c));
                ret(ir);
                terminal = 1;
                break;
            case SNX_FLOW_UNDEF: break;
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
        LLVMValueRef pc;
        LLVMPositionBuilderAtEnd(B, dispatch_block);
        poll(ir, NULL);
        pc = ir_pc(ir);
        sw = LLVMBuildSwitch(B, pc, other, (unsigned)function->leader_count);
        for (i = 0; i < function->count; ++i)
            if (function->leaders[i]) LLVMAddCase(sw, ir_i64(ir, snx_function_address(function, i)), labels[i]);
        LLVMPositionBuilderAtEnd(B, other);
        must_tail(ir, call_dynamic(ir, ir_pc(ir)));
    }
    free(labels);
}

LLVMValueRef snx_ir_static_step(SnxIr* ir, SnxU32 word, SnxU64 pc, const char* name) {
    LLVMTypeRef param = ir->t_ptr;
    LLVMValueRef fn = LLVMAddFunction(ir->mod, name, LLVMFunctionType(ir->t_i64, &param, 1, 0)), operand;
    const SnxA64Case* c = snx_a64_decode(word);
    function_attributes(ir, fn);
    snx_ir_begin(ir, fn);
    switch (c->kind) {
        case SNX_FLOW_PLAIN:
            plain(ir, word, pc, c);
            LLVMBuildRet(B, ir_i64(ir, pc + 4));
            break;
        case SNX_FLOW_UNDEF:
            ir_undef(ir, pc, word);
            LLVMBuildRet(B, ir_i64(ir, pc));
            break;
        case SNX_FLOW_B: LLVMBuildRet(B, ir_i64(ir, snx_a64_target(word, pc))); break;
        case SNX_FLOW_BR:
        case SNX_FLOW_RET: LLVMBuildRet(B, snx_ir_flow_operand(ir, word, pc, c)); break;
        case SNX_FLOW_BC:
            LLVMBuildRet(B, LLVMBuildSelect(B, snx_ir_flow_operand(ir, word, pc, c),
                                            ir_i64(ir, snx_a64_target(word, pc)), ir_i64(ir, pc + 4), ""));
            break;
        case SNX_FLOW_BL:
        case SNX_FLOW_BLR:
            /* { u64 t_=<target>; c->x[30]=pc+4; return t_; } */
            operand =
                c->kind == SNX_FLOW_BL ? ir_i64(ir, snx_a64_target(word, pc)) : snx_ir_flow_operand(ir, word, pc, c);
            ir_set_x(ir, 30, ir_i64(ir, pc + 4));
            LLVMBuildRet(B, operand);
            break;
        case SNX_FLOW_SVC:
            svc(ir, pc, snx_ir_flow_operand(ir, word, pc, c));
            LLVMBuildRet(B, ir_i64(ir, pc + 4));
            break;
        case SNX_FLOW_BRK:
            brk(ir, pc, snx_ir_flow_operand(ir, word, pc, c));
            LLVMBuildRet(B, ir_i64(ir, pc));
            break;
    }
    return fn;
}
