/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The write marks (snx_ir_write_marks): the pages a function's guest stores write, each marked by a store of 1 into
   its byte of the array below M (cpu.h's SNX_WMARK_BELOW), so the runtime finds the pages the game wrote by reading
   those bytes (memory/mem.c's mem_marks_scan) instead of asking the host for its written-page tracking, whose system
   calls were most of the time the GPU thread spent on its checks in one game.

   It runs on each function as it is emitted, before its promotion, while every guest store still carries the guest
   scope (ir_tag_guest) and its address is MP's GEP of M. The stores of one block are grouped by their address's base
   (the value it adds constants to): the marks of a group are the pages of its lowest and highest byte, made once,
   where the group's pages can't be more than two (it spans 4 KiB at most). They go after the group's stores and
   before anything that can make them visible to another thread first: a call (an SVC, another guest function, a
   helper), a fence, an atomic access of the guest's (a store-release, a read-modify-write, which order the stores
   before them), or the block's end. A store is visible to another thread before its mark is, so the runtime, which
   clears a mark before it reads the page, never takes a page in before a store it missed: the store's mark comes
   after.

   A store whose base is the stack pointer is not marked: a thread's stack is never memory the GPU reads, and those
   stores (frames, spills) are about a third of a game's. */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/aarch64/cpu.h"
#include <stdatomic.h>

typedef LLVMValueRef V;
#define B ir->b

static _Atomic SnxU64 g_mark_stores, g_mark_stack, g_mark_marks;

void snx_ir_write_marks_counts(SnxU64* stores, SnxU64* stack, SnxU64* marks) {
    *stores = atomic_exchange_explicit(&g_mark_stores, 0, memory_order_relaxed);
    *stack = atomic_exchange_explicit(&g_mark_stack, 0, memory_order_relaxed);
    *marks = atomic_exchange_explicit(&g_mark_marks, 0, memory_order_relaxed);
}

void snx_ir_write_marks_add(SnxU64 stores, SnxU64 stack, SnxU64 marks) {
    atomic_fetch_add_explicit(&g_mark_stores, stores, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_mark_stack, stack, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_mark_marks, marks, memory_order_relaxed);
}

/* the mark of the page of address (an i64) through M */
static void mark_page(SnxIr* ir, V M, V address) {
    V index =
        LLVMBuildAdd(B, LLVMBuildLShr(B, address, ir_i64(ir, 12), ""), ir_i64(ir, (SnxU64)0 - SNX_WMARK_BELOW), "");
    V store = LLVMBuildStore(B, LLVMConstInt(ir->t_i8, 1, 0), LLVMBuildGEP2(B, ir->t_i8, M, &index, 1, ""));
    LLVMSetAlignment(store, 1);
    /* after the stores before it to whoever reads it: x86-64 orders one thread's stores, AArch64 does not (STLRB) */
    LLVMSetOrdering(store, LLVMAtomicOrderingRelease);
    ir_tag_guest(ir, store); /* never the CPU structure: the promotion keeps registers across it */
    atomic_fetch_add_explicit(&g_mark_marks, 1, memory_order_relaxed);
}

void ir_mark_write(SnxIr* ir, V M, V address, unsigned bytes) {
    mark_page(ir, M, address);
    if (bytes > 1) mark_page(ir, M, LLVMBuildAdd(B, address, ir_i64(ir, bytes - 1), ""));
}

/* address as a base plus a constant, through the adds and subtracts of constants that made it: the base (NULL for a
   constant address) */
static V base_of(V address, SnxU64* offset) {
    *offset = 0;
    for (int depth = 0; depth < 16; ++depth) {
        if (LLVMIsAConstantInt(address)) {
            *offset += LLVMConstIntGetZExtValue(address);
            return NULL;
        }
        if (!LLVMIsAInstruction(address)) break;
        LLVMOpcode op = LLVMGetInstructionOpcode(address);
        V a = LLVMGetOperand(address, 0), b = op == LLVMAdd || op == LLVMSub ? LLVMGetOperand(address, 1) : NULL;
        if (op == LLVMAdd && LLVMIsAConstantInt(b)) {
            *offset += LLVMConstIntGetZExtValue(b);
            address = a;
        } else if (op == LLVMAdd && LLVMIsAConstantInt(a)) {
            *offset += LLVMConstIntGetZExtValue(a);
            address = b;
        } else if (op == LLVMSub && LLVMIsAConstantInt(b)) {
            *offset -= LLVMConstIntGetZExtValue(b);
            address = a;
        } else
            break;
    }
    return address;
}

/* whether base is the stack pointer as the function read it from the CPU structure (X31 there) */
static int stack_pointer(SnxIr* ir, V base) {
    if (!base || !LLVMIsALoadInst(base)) return 0;
    V pointer = LLVMGetOperand(base, 0);
    if (!LLVMIsAGetElementPtrInst(pointer) || LLVMGetNumOperands(pointer) != 2 || LLVMGetOperand(pointer, 0) != ir->c)
        return 0;
    V index = LLVMGetOperand(pointer, 1);
    return LLVMIsAConstantInt(index) && LLVMConstIntGetZExtValue(index) == offsetof(CPU, x) + 8 * 31;
}

/* the stores of a block not marked yet: per base, the bytes [lo, hi) from it they wrote */
#define GROUPS 16
typedef struct {
    V base, M;
    i64 lo, hi;
} Group;
typedef struct {
    Group g[GROUPS];
    int n;
} Groups;

static void emit_group(SnxIr* ir, const Group* g) {
    V address = g->base ? g->base : ir_i64(ir, 0);
    if (g->lo) address = LLVMBuildAdd(B, address, ir_i64(ir, (SnxU64)g->lo), "");
    ir_mark_write(ir, g->M, address, (unsigned)(g->hi - g->lo));
}
/* the marks of every group, before inst */
static void flush(SnxIr* ir, Groups* groups, V inst) {
    if (!groups->n) return;
    LLVMPositionBuilderBefore(B, inst);
    for (int i = 0; i < groups->n; ++i) emit_group(ir, &groups->g[i]);
    groups->n = 0;
}
/* a write of bytes at base + offset, before inst's place (inst is the write itself: a group flushed for it is marked
   before it, which is after its own stores) */
static void add_write(SnxIr* ir, Groups* groups, V inst, V M, V base, SnxU64 offset, unsigned bytes) {
    i64 lo = (i64)offset, hi = lo + (i64)bytes;
    /* a far offset (the high bits of an address the guest made by adding a constant) is a base of its own */
    if (lo < -((i64)1 << 40) || lo > ((i64)1 << 40)) {
        LLVMPositionBuilderBefore(B, inst);
        V address = base ? LLVMBuildAdd(B, base, ir_i64(ir, offset), "") : ir_i64(ir, offset);
        base = address;
        lo = 0;
        hi = bytes;
    }
    for (int i = 0; i < groups->n; ++i) {
        Group* g = &groups->g[i];
        if (g->base != base || g->M != M) continue;
        i64 nlo = lo < g->lo ? lo : g->lo, nhi = hi > g->hi ? hi : g->hi;
        if (nhi - nlo <= 4096) {
            g->lo = nlo;
            g->hi = nhi;
            return;
        }
        /* too wide for two marks: the group so far is marked here, and this write starts it again */
        LLVMPositionBuilderBefore(B, inst);
        emit_group(ir, g);
        g->lo = lo;
        g->hi = hi;
        return;
    }
    if (groups->n == GROUPS) flush(ir, groups, inst);
    groups->g[groups->n++] = (Group){base, M, lo, hi};
}

/* whether pointer is MP's GEP of the function's M: a guest address */
static int guest_gep(SnxIr* ir, V pointer) {
    return LLVMIsAGetElementPtrInst(pointer) && LLVMGetNumOperands(pointer) == 2 && LLVMGetOperand(pointer, 0) == ir->M;
}

/* whether a call may make the stores before it visible to another thread, or write guest memory itself: any but an
   intrinsic that touches no memory */
static int flushing_call(V call) {
    V callee = LLVMGetCalledValue(call);
    if (!callee || !LLVMIsAFunction(callee) || !LLVMGetIntrinsicID(callee)) return 1;
    size_t length = 0;
    const char* name = LLVMGetValueName2(callee, &length);
    return length >= 9 && !strncmp(name, "llvm.mem", 8);
}

void ir_write_marks(SnxIr* ir, V fn) {
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(ir->mod);
    LLVMBasicBlockRef at = LLVMGetInsertBlock(B);
    Groups groups;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
        groups.n = 0;
        for (V inst = LLVMGetFirstInstruction(bb); inst; inst = LLVMGetNextInstruction(inst)) {
            LLVMOpcode op = LLVMGetInstructionOpcode(inst);
            if (op == LLVMStore || op == LLVMAtomicRMW || op == LLVMAtomicCmpXchg) {
                V pointer = LLVMGetOperand(inst, op == LLVMStore ? 1 : 0), M, address;
                /* the guest's: its scope, or MP's GEP of M (an access an emitter left untagged) */
                if (LLVMGetMetadata(inst, ir->kind_scope) != ir->scope_guest && !guest_gep(ir, pointer)) continue;
                LLVMTypeRef type = LLVMTypeOf(LLVMGetOperand(inst, 1 - (op == LLVMStore)));
                unsigned bytes = (unsigned)LLVMStoreSizeOfType(layout, type);
                if (op != LLVMStore || LLVMGetOrdering(inst) != LLVMAtomicOrderingNotAtomic)
                    flush(ir, &groups, inst); /* what it orders after the stores before it comes after their marks */
                if (LLVMIsAGetElementPtrInst(pointer) && LLVMGetNumOperands(pointer) == 2) {
                    M = LLVMGetOperand(pointer, 0);
                    address = LLVMGetOperand(pointer, 1);
                } else { /* not MP's GEP: its offset from the function's M */
                    LLVMPositionBuilderBefore(B, inst);
                    M = ir->M;
                    address = LLVMBuildSub(B, LLVMBuildPtrToInt(B, pointer, ir->t_i64, ""),
                                           LLVMBuildPtrToInt(B, M, ir->t_i64, ""), "");
                }
                SnxU64 offset;
                V base = base_of(address, &offset);
                atomic_fetch_add_explicit(&g_mark_stores, 1, memory_order_relaxed);
                if (snx_ir_write_marks_on() < 2 && stack_pointer(ir, base)) {
                    atomic_fetch_add_explicit(&g_mark_stack, 1, memory_order_relaxed);
                    continue;
                }
                add_write(ir, &groups, inst, M, base, offset, bytes ? bytes : 1);
            } else if (op == LLVMCall && flushing_call(inst)) {
                flush(ir, &groups, inst);
                /* a call given a guest pointer (CASP's snx_cas128) writes there: 16 bytes, marked after it */
                for (unsigned i = 0, n = (unsigned)LLVMGetNumArgOperands(inst); i < n; ++i) {
                    V arg = LLVMGetOperand(inst, i);
                    if (!guest_gep(ir, arg)) continue;
                    SnxU64 offset;
                    V base = base_of(LLVMGetOperand(arg, 1), &offset);
                    atomic_fetch_add_explicit(&g_mark_stores, 1, memory_order_relaxed);
                    add_write(ir, &groups, inst, LLVMGetOperand(arg, 0), base, offset, 16);
                }
            } else if (op == LLVMFence || LLVMIsATerminatorInst(inst))
                flush(ir, &groups, inst);
        }
    }
    if (at) LLVMPositionBuilderAtEnd(B, at);
}
