/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Guest memory's atomics ported from QEMU (target/arm/tcg/translate-a64.c: gen_compare_and_swap,
   gen_compare_and_swap_pair, do_atomic_ld), through ir_ldst.c's, LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* sailgen's LLVM back end for A64, after core.h (the values and the builtins every machine's back end shares): it
   reads and writes the Sail's registers where ScarletNX's CPU keeps them (below, "the state"), in the per-block cache of
   their values that the emitters written by hand keep too, so the two share one view of each block, and lowers the
   machine's actions, guest memory and the floating point to the IR the recompiler's emitters have always made */

#include "common/cpu/aarch64/cpu.h" /* the CPU structure, where the registers are kept */
#include "common/cpu/recompiler/llvm/ir_fp.h" /* the floating-point pieces the recompiler's FP emitters share */
#include "common/cpu/recompiler/llvm/ir_vec.h" /* a SIMD&FP register read and written as lanes */

struct SgCtx {
    SnxIr* ir;
    SnxU32 word;
    SnxU64 pc;
    /* flags written alone (PSTATE.N = ...) and not committed yet: an i32 0 or 1 each, NULL for not written */
    LLVMValueRef flag[4];
    /* where a branch goes (BranchTo): its target, an i64, and when, an i1 (the run-time condition it is under, true
       when none); branched 0 until it does */
    LLVMValueRef taken, target;
    int branched, undefined;
    /* the block's last instruction before this one's: a clause that hands the word on (sg_see) has emitted nothing */
    LLVMValueRef start;
    /* the doublewords of SIMD&FP registers this instruction loaded (sg__V_read_bits): register n's doubleword k, so
       an element loop over them reads the register as lanes (sg_l_elem) */
    LLVMValueRef vload[8];
    unsigned char vload_n[8], vload_k[8];
    unsigned vload_count;
    /* the load-exclusive's values, i64 each (two for a pair of doublewords), and the next an exclusive read takes; the
       store-exclusive's status, an i32 0 or 1 (GuestLoadExclusive, GuestStoreExclusive) */
    LLVMValueRef exclusive[2], status;
    unsigned exclusive_count, exclusive_next;
    /* the instruction's first two plain 4-byte accesses, the load or store and its address each, and whether they are
       stores, for sg_accesses_combine (more than two: 3) */
    LLVMValueRef access[2], access_address[2];
    unsigned access_count, access_stores;
    /* the last run-time condition a lowered function ran under (sg_fp_when): the condition, the end of the way where it
       holds, and the empty block of the way where it does not, which both join */
    LLVMValueRef when_path;
    LLVMBasicBlockRef when_end, when_skip;
};

static inline LLVMBuilderRef sg_builder(SgCtx* g) { return g->ir->b; }
static inline LLVMContextRef sg_llvm_context(SgCtx* g) { return g->ir->ctx; }
static inline LLVMValueRef sg_intrinsic(SgCtx* g, const char* name, LLVMTypeRef type, LLVMValueRef* args, unsigned n) {
    return ir_intrinsic(g->ir, name, type, args, n);
}
/* select(c, x, y) as one phi, where c is the run-time condition a lowered function last ran under (sg_fp_when), x a
   phi this block starts with of what it gave where c held and of a constant from the empty block of the other way,
   and y a constant: what it gave, and y from that block (FCCMP's flags). NULL otherwise */
static inline LLVMValueRef sg_select_phi(SgCtx* g, LLVMValueRef c, LLVMValueRef x, LLVMValueRef y) {
    LLVMBasicBlockRef here = LLVMGetInsertBlock(SG_B), blocks[2];
    LLVMValueRef values[2], phi;
    unsigned k;
    if (c != g->when_path || !LLVMIsAPHINode(x) || LLVMGetInstructionParent(x) != here || LLVMCountIncoming(x) != 2 ||
        !LLVMIsAConstant(y))
        return NULL;
    k = LLVMGetIncomingBlock(x, 0) == g->when_skip ? 0 : 1;
    if (LLVMGetIncomingBlock(x, k) != g->when_skip || LLVMGetIncomingBlock(x, 1 - k) != g->when_end) return NULL;
    values[0] = LLVMGetIncomingValue(x, 1 - k);
    blocks[0] = g->when_end;
    values[1] = y;
    blocks[1] = g->when_skip;
    LLVMPositionBuilderBefore(SG_B, LLVMGetFirstInstruction(here));
    phi = LLVMBuildPhi(SG_B, LLVMTypeOf(x), "");
    LLVMAddIncoming(phi, values, blocks, 2);
    LLVMPositionBuilderAtEnd(SG_B, here);
    return phi;
}

/* AddWithCarry(a, b, cin): the result, and the NZCV the flags take when it is written to them (sg_PSTATE_write),
   with the IR the recompiler always made: a - z when b is NOT(z) and cin '1' (the flags' origin a compare, which
   ConditionHolds then reads as one), a + b when cin is '0', and the sum with a run-time carry otherwise */
static inline void sg_add_with_carry(SgCtx* g, SgD a, SgD b, SgD cin, SgD* result, SgD* nzcv) {
    LLVMValueRef x = sg_d_make(g, &a), y, r, c = NULL;
    unsigned w = a.w;
    SgD flags = sg_d_empty(4);
    int kind;
    if (sg_d_is_const(&cin) && cin.sel[0] == 1) {
        y = sg_d_make(g, &b);
        if (LLVMIsAConstantInt(y))
            y = LLVMConstNot(y);
        else if (LLVMIsAInstruction(y) && LLVMGetInstructionOpcode(y) == LLVMXor &&
                 LLVMIsAConstantInt(LLVMGetOperand(y, 1)) && LLVMConstIntGetSExtValue(LLVMGetOperand(y, 1)) == -1)
            y = LLVMGetOperand(y, 0);
        else
            y = LLVMBuildNot(SG_B, y, ""); /* a + b + 1 = a - NOT(b) */
        kind = SG_NZCV_SUB;
        r = LLVMBuildSub(SG_B, x, y, "");
    } else if (sg_d_is_const(&cin)) {
        y = sg_d_make(g, &b);
        kind = SG_NZCV_ADD;
        r = LLVMBuildAdd(SG_B, x, y, "");
    } else {
        y = sg_d_make(g, &b);
        c = cin.sel[0] >= 2 && cin.src_flag[cin.sel[0] - 2] ? cin.src[cin.sel[0] - 2] : sg_d_make(g, &cin);
        kind = SG_NZCV_CARRY;
        r = LLVMBuildAdd(SG_B, LLVMBuildAdd(SG_B, x, y, ""), sg_cast(g, c, w, 0), "");
    }
    *result = sg_d_from(g, r);
    flags.nzcv = (unsigned char)kind;
    flags.nzcv_a = x;
    flags.nzcv_b = y;
    flags.nzcv_cin = c;
    flags.nzcv_r = r;
    *nzcv = flags;
}
/* the four flags an NZCV not made yet stands for, as values: AddWithCarry's (the formulas of ir_state.c's
   ir_flags_adds and ir_flags_subs) or a logical result's (its ir_flags_logic) */
static inline void sg_nzcv_flags(SgCtx* g, const SgD* f, LLVMValueRef out[4]) {
    LLVMValueRef a = f->nzcv_a, b = f->nzcv_b, r = f->nzcv_r, cf, overflow;
    LLVMTypeRef type = LLVMTypeOf(r);
    LLVMValueRef sign = LLVMConstInt(type, LLVMGetIntTypeWidth(type) - 1, 0);
    if (f->nzcv == SG_NZCV_LOGIC) {
        out[0] = LLVMBuildLShr(SG_B, r, sign, "");
        out[1] = sg_bool32(g, LLVMBuildICmp(SG_B, LLVMIntEQ, r, LLVMConstNull(type), ""));
        out[2] = LLVMConstInt(sg_type(g, 32), 0, 0);
        out[3] = LLVMConstInt(sg_type(g, 32), 0, 0);
        return;
    }
    /* in the order ir_flags_subs and ir_flags_adds build them, so the same compare is the same IR */
    if (f->nzcv == SG_NZCV_SUB) {
        overflow = LLVMBuildAnd(SG_B, LLVMBuildXor(SG_B, a, b, ""), LLVMBuildXor(SG_B, a, r, ""), "");
        out[0] = LLVMBuildLShr(SG_B, r, sign, "");
        out[1] = sg_bool32(g, LLVMBuildICmp(SG_B, LLVMIntEQ, r, LLVMConstNull(type), ""));
        out[2] = sg_bool32(g, LLVMBuildICmp(SG_B, LLVMIntUGE, a, b, ""));
        out[3] = LLVMBuildLShr(SG_B, overflow, sign, "");
        return;
    }
    out[0] = LLVMBuildLShr(SG_B, r, sign, "");
    out[1] = sg_bool32(g, LLVMBuildICmp(SG_B, LLVMIntEQ, r, LLVMConstNull(type), ""));
    if (f->nzcv == SG_NZCV_CARRY) {
        LLVMValueRef has = LLVMBuildICmp(SG_B, LLVMIntNE, f->nzcv_cin, LLVMConstNull(LLVMTypeOf(f->nzcv_cin)), "");
        LLVMValueRef ule = LLVMBuildICmp(SG_B, LLVMIntULE, r, a, "");
        cf = LLVMBuildSelect(SG_B, has, ule, LLVMBuildICmp(SG_B, LLVMIntULT, r, a, ""), "");
    } else
        cf = LLVMBuildICmp(SG_B, LLVMIntULT, r, a, "");
    overflow =
        LLVMBuildAnd(SG_B, LLVMBuildNot(SG_B, LLVMBuildXor(SG_B, a, b, ""), ""), LLVMBuildXor(SG_B, a, r, ""), "");
    out[2] = sg_bool32(g, cf);
    out[3] = LLVMBuildLShr(SG_B, overflow, sign, "");
}

/* a guest's feature, a64_decode.h's SNX_A64_FEAT_X (the decoder's profile) */
#define SG_FEAT(x) SNX_A64_##x

static inline int sg_feature(SgCtx* g, SnxU64 feature) { return (g->ir->guest_features & feature) != 0; }
static inline void sg_undefined(SgCtx* g) {
    ir_set_pc(g->ir, ir_i64(g->ir, g->pc));
    ir_undef(g->ir, g->pc, g->word);
    g->undefined = 1;
}

/* ---- the flow ----
   BranchTo(target, type, conditional) is where the instruction goes: the recompiler's flow (ir_codegen.c) takes the
   target, and under a run-time condition that condition, as a branch's operand. Its other parts change nothing at the
   NX's EL0: no Statistical Profiling, no Branch Record Buffer, no top-byte ignore on a branch's address. A branch not
   taken (BranchNotTaken) records nothing */
static inline void sg_branch_to(SgCtx* g, SgD target, LLVMValueRef when) {
    if (g->branched) sg_fail("an instruction that branches twice", (long long)g->word, 0);
    if (target.w != 64) sg_fail("a branch to a target of other than 64 bits", target.w, 64);
    g->branched = 1;
    g->target = sg_d_make(g, &target);
    g->taken = when ? when : LLVMConstInt(g->ir->t_i1, 1, 0);
}
/* the operand an instruction gave: whether it branches and where to. One that branched nowhere is never taken; one
   that raised UNDEF goes nowhere, its own pc */
static inline void sg_branch_result(SgCtx* g, LLVMValueRef* taken, LLVMValueRef* target) {
    int go = g->branched && !g->undefined;
    if (taken) *taken = go ? g->taken : LLVMConstInt(g->ir->t_i1, 0, 0);
    if (target) *target = go ? g->target : ir_i64(g->ir, g->pc);
}

/* a decode function that throws Error_See hands the word on to the next clause that matches it (the hint space's
   PAuth hints): 0. Only its fields were read, so the block holds nothing of it yet */
static inline int sg_see(SgCtx* g) {
    if (LLVMGetLastInstruction(LLVMGetInsertBlock(g->ir->b)) != g->start || g->branched || g->undefined || g->flag[0] ||
        g->flag[1] || g->flag[2] || g->flag[3])
        sg_fail("a word handed on to the next clause after its own emitted IR", (long long)g->word, 0);
    return 0;
}

/* ---- the machine's actions ----
   The Sail's functions with no definition of their own that a hint, a barrier or the exclusive monitor calls, lowered
   as sg_NAME (sailgen's action_call): given the call's arguments, or the context alone when they mean nothing here */

/* sail-arm's MBReqTypes, in its order: the accesses a barrier orders */
enum { SG_MBReqTypes_Reads, SG_MBReqTypes_Writes, SG_MBReqTypes_All };
/* loads before loads and stores (Reads) is an acquire fence, stores before stores a release one (which also orders
   loads before stores), all of them a full one. The guest's memory is the host's, one domain */
static inline void sg_barrier(SgCtx* g, long long types) {
    if (types == SG_MBReqTypes_Reads)
        LLVMBuildFence(SG_B, LLVMAtomicOrderingAcquire, 0, "");
    else if (types == SG_MBReqTypes_Writes)
        LLVMBuildFence(SG_B, LLVMAtomicOrderingRelease, 0, "");
    else
        ir_fence(g->ir);
}
static inline void sg_DataMemoryBarrier(SgCtx* g, long long domain, long long types) {
    (void)domain;
    sg_barrier(g, types);
}
/* nXS (FEAT_XS) only exempts accesses that the host has no such kind of */
static inline void sg_DataSynchronizationBarrier(SgCtx* g, long long domain, long long types, long long nxs) {
    (void)domain;
    (void)nxs;
    sg_barrier(g, types);
}
/* SSBB and PSSBB (DSB #0 and #4): on the console's Armv8.0 cores those are DSB SY, as DMB #0 and #4 still are
   (decode_dmb's types All) */
static inline void sg_SpeculativeStoreBypassBarrierToVA(SgCtx* g) { ir_fence(g->ir); }
static inline void sg_SpeculativeStoreBypassBarrierToPA(SgCtx* g) { ir_fence(g->ir); }
/* ISB: what follows waits for what came before, a full fence */
static inline void sg_InstructionSynchronizationBarrier(SgCtx* g) { ir_fence(g->ir); }
/* CSDB limits the speculation on data values the guest's own core does: the host's is not the guest's to limit */
static inline void sg_ConsumptionOfSpeculativeDataBarrier(SgCtx* g) { (void)g; }
/* ESB's: no SError is ever pending, the runtime raises none */
static inline void sg_SynchronizeErrors(SgCtx* g) { (void)g; }
static inline void sg_TakeUnmaskedSErrorInterrupts(SgCtx* g) { (void)g; }
/* DGH: the accesses that follow need not be gathered with those before, and nothing gathers them */
static inline void sg_Hint_DGH(SgCtx* g) { (void)g; }
/* CLRBHB clears the branch history the guest's core predicts with: the host's is not the guest's */
static inline void sg_Hint_CLRBHB(SgCtx* g) { (void)g; }
/* PSB CSYNC and TSB CSYNC: no profiling and no trace unit, so nothing to synchronize */
static inline void sg_ProfilingSynchronizationBarrier(SgCtx* g) { (void)g; }
static inline void sg_TraceSynchronizationBarrier(SgCtx* g) { (void)g; }

/* YIELD: the host's spin-wait hint (ir_yield) */
static inline void sg_Hint_Yield(SgCtx* g) { ir_yield(g->ir); }
/* WFE, its timeout and kind aside: the runtime lets the core's other threads run, as the spin waits on one of them */
static inline void sg_Hint_WFE(SgCtx* g) {
    LLVMTypeRef param = g->ir->t_ptr;
    LLVMValueRef c = g->ir->c;
    ir_rt_call(g->ir, ir_rt_wfe(), g->ir->t_void, &param, &c, 1);
}
/* WFI at EL0 does not trap (SCTLR_EL1.nTWI) and waits for an interrupt the guest never takes: it goes on at once */
static inline void sg_Hint_WFI(SgCtx* g) { (void)g; }
/* SEV and SEVL: the runtime's one event (rt_sev), which wakes no thread since none sleeps in WFE */
static inline void sg_SendEvent(SgCtx* g) { ir_rt_call(g->ir, ir_rt_sev(), g->ir->t_void, NULL, NULL, 0); }
static inline void sg_SendEventLocal(SgCtx* g) { sg_SendEvent(g); }
/* CLREX: the running thread's local monitor (CPU.excl_on) is the one the processor's id names */
static inline void sg_ClearExclusiveLocal(SgCtx* g) { ir_cpu_store(g->ir, ir_i32(g->ir, 0), ir_off_excl_on()); }

/* ---- the state ----
   The Sail's registers where ScarletNX's CPU keeps them (cpu.h): nx.sail's GPR, X0 to X30 then SP_EL0, in CPU.x,
   and PSTATE's N, Z, C and V in CPU.n, z, cf and vf, each the i32 0 or 1. sailgen calls sg_NAME_read and
   sg_NAME_write for a register NAME: a vector's by element, a struct's by field (SG_NAME_FIELD, numbered as the
   struct's fields; a run of them written together as the first and how many). Every write is stored at once, and the
   values are cached for the basic block in ir->state (ir.h's SnxIrBlockState), with what the four flags came from
   when a compare or a logical result made them. That cache is every emitter's: the functions over the IR builder
   (sg_x, sg_set_x, sg_flag, sg_flags_store, sg_flags_origin, sg_origin_cond and the two forgets) are also ir.h's
   accessors for the emitters written by hand (ir_sail.c), so a block that mixes the two loads a register once, reads
   a write back as the value written, and reads a condition on flags a compare made as that compare. Another back end
   keeps them its own way behind the same names */

/* a load or a store of the CPU structure's field at offset, tagged as the CPU's: the guest's memory never aliases it */
static inline LLVMValueRef sg_cpu_load(SnxIr* ir, LLVMTypeRef type, size_t offset) {
    return ir_tag_cpu(ir, LLVMBuildLoad2(ir->b, type, ir_cpu(ir, offset), ""));
}
static inline void sg_cpu_store(SnxIr* ir, LLVMValueRef value, size_t offset) {
    ir_tag_cpu(ir, LLVMBuildStore(ir->b, value, ir_cpu(ir, offset)));
}

/* the registers' cache, emptied when the block changes, and dropped (an unknown call, a store that may alias it) */
static inline SnxIrRegisterCache* sg_registers(SnxIr* ir) {
    SnxIrRegisterCache* r = &ir->state.registers;
    LLVMBasicBlockRef block = LLVMGetInsertBlock(ir->b);
    if (r->block != block) {
        r->valid = 0;
        r->block = block;
    }
    return r;
}
static inline void sg_registers_forget(SnxIr* ir) {
    ir->state.registers.valid = 0;
    ir->state.registers.block = NULL;
}
/* X n (0 to 31, SP the last): loaded once a block */
static inline LLVMValueRef sg_x(SnxIr* ir, unsigned n) {
    SnxIrRegisterCache* r = sg_registers(ir);
    if (n > 31) sg_fail("an element of GPR it does not have", n, 31);
    if (!((r->valid >> n) & 1)) {
        r->x[n] = sg_cpu_load(ir, ir->t_i64, offsetof(CPU, x) + 8 * (size_t)n);
        r->valid |= (uint32_t)1 << n;
    }
    return r->x[n];
}
/* X n = bits (an i64): stored, and read back as bits */
static inline void sg_set_x(SnxIr* ir, unsigned n, LLVMValueRef bits) {
    SnxIrRegisterCache* r = sg_registers(ir);
    if (n > 31) sg_fail("an element of GPR it does not have", n, 31);
    sg_cpu_store(ir, bits, offsetof(CPU, x) + 8 * (size_t)n);
    r->x[n] = bits;
    r->valid |= (uint32_t)1 << n;
}
static inline SgD sg_GPR_read(SgCtx* g, long long n) {
    if (n < 0) sg_fail("an element of GPR it does not have", n, 31);
    return sg_d_from(g, sg_x(g->ir, (unsigned)n));
}
static inline void sg_GPR_write(SgCtx* g, long long n, SgD v) {
    LLVMValueRef bits;
    if (n < 0) sg_fail("an element of GPR it does not have", n, 31);
    if (v.w != 64) sg_fail("a GPR element written with other than 64 bits", n, v.w);
    bits = sg_d_make(g, &v);
    sg_set_x(g->ir, (unsigned)n, bits);
}

/* BTypeNext, the branch type the next instruction checks under BTI: no page is guarded (nx.sail's InGuardedPage), so
   nothing checks it and the runtime keeps it nowhere */
static inline void sg_BTypeNext_write(SgCtx* g, SgD v) {
    (void)g;
    (void)v;
}
/* BTypeCompatible, whether this instruction may be a branch's target under BTI: only a guarded page's check reads it */
static inline void sg_BTypeCompatible_write(SgCtx* g, LLVMValueRef v) {
    (void)g;
    (void)v;
}

/* PSTATE's fields this back end keeps, numbered as ProcState's */
enum { SG_PSTATE_N, SG_PSTATE_Z, SG_PSTATE_C, SG_PSTATE_V };
static const size_t sg_flag_offsets[4] = {offsetof(CPU, n), offsetof(CPU, z), offsetof(CPU, cf), offsetof(CPU, vf)};

/* the flags' cache, emptied, with what made them, when the block changes, and dropped */
static inline SnxIrFlags* sg_flags(SnxIr* ir) {
    SnxIrFlags* f = &ir->state.flags;
    LLVMBasicBlockRef block = LLVMGetInsertBlock(ir->b);
    if (f->block != block) {
        f->valid = 0;
        f->block = block;
        f->origin = SNX_IR_FLAGS_SOME;
    }
    return f;
}
static inline void sg_flags_forget(SnxIr* ir) {
    ir->state.flags.valid = 0;
    ir->state.flags.block = NULL;
    ir->state.flags.origin = SNX_IR_FLAGS_SOME;
}
/* flag i (N, Z, C, V), the i32 0 or 1: loaded once a block */
static inline LLVMValueRef sg_flag(SnxIr* ir, unsigned i) {
    SnxIrFlags* f = sg_flags(ir);
    if (!((f->valid >> i) & 1)) {
        f->value[i] = sg_cpu_load(ir, ir->t_i32, sg_flag_offsets[i]);
        f->valid |= 1u << i;
    }
    return f->value[i];
}
/* the four flags (any integer width) stored, each as the i32 0 or 1, and read back as those; made by anything until
   sg_flags_origin says what */
static inline void sg_flags_store(SnxIr* ir, LLVMValueRef v[4]) {
    SnxIrFlags* f = sg_flags(ir);
    unsigned i;
    for (i = 0; i < 4; ++i) {
        LLVMValueRef bits = ir_u32(ir, v[i]);
        sg_cpu_store(ir, bits, sg_flag_offsets[i]);
        f->value[i] = bits;
    }
    f->valid = 15;
    f->origin = SNX_IR_FLAGS_SOME;
}
/* what the four flags just stored came from: SNX_IR_FLAGS_SUB or ADD with the compare's operands, LOGIC with the
   result. The dump (ir->dump_active) says each */
static inline void sg_flags_origin(SnxIr* ir, unsigned origin, LLVMValueRef left, LLVMValueRef right) {
    if (ir->dump_active)
        fprintf(stderr, "IR flags pc %llx block %s origin %u\n", (unsigned long long)ir->dump_instruction_pc,
                LLVMGetBasicBlockName(LLVMGetInsertBlock(ir->b)), origin);
    ir->state.flags.origin = origin;
    ir->state.flags.left = left;
    ir->state.flags.right = right;
}
/* condition code (cond >> 1, but AL) on flags a compare made in this block, as that compare's i1: NULL when none
   did, or the condition needs the flags themselves (VS of SUB and ADD, and the ones of ADD a variable needs the
   overflow for). CMN with a constant k other than 0 and the most negative value gives the four flags of CMP with -k
   (its carry is a + k >= 2^n, so a >= -k; its overflow that of the same sum), and ANDS those of CMP of its result
   with 0, but for C, which ANDS clears */
static inline LLVMValueRef sg_origin_cond(SnxIr* ir, unsigned code) {
    static const LLVMIntPredicate sub[7] = {LLVMIntEQ,  LLVMIntUGE, LLVMIntSLT, LLVMIntEQ,
                                            LLVMIntUGT, LLVMIntSGE, LLVMIntSGT};
    SnxIrFlags* f = sg_flags(ir);
    unsigned origin = f->origin, bits;
    LLVMValueRef left = f->left, right = f->right, zero, r;
    LLVMTypeRef type;
    if (code > 6 || origin == SNX_IR_FLAGS_SOME) return NULL;
    type = LLVMTypeOf(left);
    bits = LLVMGetIntTypeWidth(type);
    zero = LLVMConstNull(type);
    if (origin == SNX_IR_FLAGS_ADD && LLVMIsAConstantInt(right)) {
        SnxU64 k = LLVMConstIntGetZExtValue(right), top = 1ull << (bits - 1);
        if (k != 0 && k != top) {
            origin = SNX_IR_FLAGS_SUB;
            right = LLVMConstNeg(right);
        }
    }
    switch (origin) {
        case SNX_IR_FLAGS_SUB:
            if (code == 3) return NULL;
            if (code == 2) return LLVMBuildICmp(ir->b, LLVMIntSLT, LLVMBuildSub(ir->b, left, right, ""), zero, "");
            return LLVMBuildICmp(ir->b, sub[code], left, right, "");
        case SNX_IR_FLAGS_ADD:
            if (code > 2) return NULL;
            r = LLVMBuildAdd(ir->b, left, right, "");
            if (code == 0) return LLVMBuildICmp(ir->b, LLVMIntEQ, r, zero, "");
            if (code == 1) return LLVMBuildICmp(ir->b, LLVMIntULT, r, left, ""); /* the carry out */
            return LLVMBuildICmp(ir->b, LLVMIntSLT, r, zero, "");
        case SNX_IR_FLAGS_LOGIC:
            if (code == 1 || code == 3 || code == 4) return LLVMConstInt(ir->t_i1, 0, 0); /* C and V are 0 */
            if (code == 2) return LLVMBuildICmp(ir->b, LLVMIntSLT, left, zero, "");
            return LLVMBuildICmp(ir->b, sub[code], left, zero, "");
        default: return NULL;
    }
}

/* the four flags committed, with what made them when a compare or a logical result did; the ones written alone are
   no longer waiting */
static inline void sg_flags_commit(SgCtx* g, LLVMValueRef v[4], unsigned origin, LLVMValueRef left,
                                   LLVMValueRef right) {
    unsigned i;
    for (i = 0; i < 4; ++i) g->flag[i] = NULL;
    sg_flags_store(g->ir, v);
    if (origin != SNX_IR_FLAGS_SOME) sg_flags_origin(g->ir, origin, left, right);
}
/* the flags written alone, committed with the ones not written as they are */
static inline void sg_flags_flush(SgCtx* g) {
    LLVMValueRef v[4];
    unsigned i, any = 0;
    for (i = 0; i < 4; ++i) any |= g->flag[i] != NULL;
    if (!any) return;
    for (i = 0; i < 4; ++i) v[i] = g->flag[i] ? g->flag[i] : sg_flag(g->ir, i);
    sg_flags_commit(g, v, SNX_IR_FLAGS_SOME, NULL, NULL);
}
/* the four flags of v, each a phi of the two ways under the last run-time condition a lowered function ran under
   (FCCMP's): stored at the end of each way, as an emitter that wrote them on each wrote them, and cached as the phis.
   0 when they are not that */
static inline int sg_flags_on_each_way(SgCtx* g, const SgD* v) {
    LLVMBasicBlockRef here = LLVMGetInsertBlock(SG_B), ways[2];
    LLVMValueRef f[4];
    SnxIrFlags* flags;
    unsigned i, k;
    if (!g->when_skip) return 0;
    ways[0] = g->when_end;
    ways[1] = g->when_skip;
    for (i = 0; i < 4; ++i) {
        unsigned s = v->sel[3 - i];
        if (s < 2 || !v->src_flag[s - 2] || v->pos[3 - i]) return 0;
        f[i] = v->src[s - 2];
        if (!LLVMIsAPHINode(f[i]) || LLVMGetInstructionParent(f[i]) != here || LLVMCountIncoming(f[i]) != 2 ||
            LLVMTypeOf(f[i]) != g->ir->t_i32)
            return 0;
        for (k = 0; k < 2; ++k)
            if (LLVMGetIncomingBlock(f[i], k) != ways[0] && LLVMGetIncomingBlock(f[i], k) != ways[1]) return 0;
    }
    for (k = 0; k < 2; ++k) {
        LLVMPositionBuilderBefore(SG_B, LLVMGetBasicBlockTerminator(ways[k]));
        for (i = 0; i < 4; ++i) {
            unsigned j = LLVMGetIncomingBlock(f[i], 0) == ways[k] ? 0 : 1;
            sg_cpu_store(g->ir, LLVMGetIncomingValue(f[i], j), sg_flag_offsets[i]);
        }
    }
    LLVMPositionBuilderAtEnd(SG_B, here);
    flags = sg_flags(g->ir);
    for (i = 0; i < 4; ++i) {
        flags->value[i] = f[i];
        g->flag[i] = NULL;
    }
    flags->valid = 15;
    flags->origin = SNX_IR_FLAGS_SOME;
    return 1;
}
static inline SgD sg_PSTATE_read(SgCtx* g, long long field) {
    if (field < 0 || field > 3) sg_fail("a field of PSTATE this back end does not keep", field, 3);
    return sg_d_flag(g->flag[field] ? g->flag[field] : sg_flag(g->ir, (unsigned)field), NULL);
}
/* PSTATE's fields first to first + count - 1 = v, v's top bit the first. The four at once are committed now, with
   what made them when that is a compare or a logical result; fewer wait for sg_flags_flush */
static inline void sg_PSTATE_write(SgCtx* g, long long first, long long count, SgD v) {
    LLVMValueRef f[4];
    unsigned i;
    if (first < 0 || count < 1 || first + count > 4)
        sg_fail("fields of PSTATE this back end does not keep", first, count);
    if (v.w != (unsigned)count) sg_fail("flags written from a value of another width", count, v.w);
    if (count == 4 && v.nzcv) {
        sg_nzcv_flags(g, &v, f);
        if (v.nzcv == SG_NZCV_SUB)
            sg_flags_commit(g, f, SNX_IR_FLAGS_SUB, v.nzcv_a, v.nzcv_b);
        else if (v.nzcv == SG_NZCV_ADD)
            sg_flags_commit(g, f, SNX_IR_FLAGS_ADD, v.nzcv_a, v.nzcv_b);
        else if (v.nzcv == SG_NZCV_LOGIC)
            sg_flags_commit(g, f, SNX_IR_FLAGS_LOGIC, v.nzcv_r, NULL);
        else
            sg_flags_commit(g, f, SNX_IR_FLAGS_SOME, NULL, NULL);
        return;
    }
    if (v.nzcv) sg_d_realize(g, &v);
    if (count == 4 && sg_flags_on_each_way(g, &v)) return;
    if (count == 4) {
        /* N the top bit of X, Z IsZeroBit(X), C and V 0: a logical result's flags */
        int logic = v.sel[3] >= 2 && v.sel[2] >= 2 && v.sel[1] == 0 && v.sel[0] == 0;
        if (logic) {
            unsigned k = v.sel[3] - 2u, z = v.sel[2] - 2u;
            logic = !v.src_flag[k] && v.pos[3] == v.src_w[k] - 1 && v.zero_of[z] == v.src[k];
        }
        for (i = 0; i < 4; ++i) f[i] = sg_d_flag_value(g, &v, 3 - i);
        if (logic)
            sg_flags_commit(g, f, SNX_IR_FLAGS_LOGIC, v.src[v.sel[3] - 2], NULL);
        else
            sg_flags_commit(g, f, SNX_IR_FLAGS_SOME, NULL, NULL);
        return;
    }
    for (i = 0; i < (unsigned)count; ++i)
        g->flag[first + i] = ir_u32(g->ir, sg_d_flag_value(g, &v, (unsigned)count - 1 - i));
}

/* ConditionHolds(cond) as the compare that set the flags, when one did in this block (sg_origin_cond): NULL when
   none did, or the condition needs the flags themselves (VS of a compare), so the Sail body reads them */
static inline LLVMValueRef sg_condition_origin(SgCtx* g, SgS cond) {
    LLVMValueRef r;
    sg_flags_flush(g);
    r = sg_origin_cond(g->ir, (unsigned)(cond.v >> 1));
    if (r && (cond.v & 1)) r = LLVMBuildNot(SG_B, r, "");
    return r;
}

/* ---- guest memory ----
   nx.sail's primitives: guest memory is the host's bytes at CPU.mem plus the address (ir.h's ir_rd and ir_wr, which
   make the access as unaligned and, with volatile_memory, volatile as every other guest access), an acquire load and a
   release store sequentially consistent host atomics (ir_ldacq, ir_strel), the exclusives cpu.h's value-based monitor
   through its helpers (ldx, stx, ldxp, stxp), and the LSE atomics host atomics of the access size */

static inline LLVMValueRef sg_address(SgCtx* g, SgD address) {
    if (address.w != 64) sg_fail("a guest address of other than 64 bits", address.w, 64);
    return sg_d_make(g, &address);
}
static inline void sg_access_size(long long size, long long most) {
    if (size != 1 && size != 2 && size != 4 && size != 8 && !(size == 16 && most == 16))
        sg_fail("a guest access of a size this back end does not make", size, most);
}
/* a plain 4-byte access just made (its instruction the block's last), for sg_accesses_combine */
static inline void sg_access_note(SgCtx* g, LLVMValueRef access, LLVMValueRef address, int store) {
    if (g->access_count < 2) {
        g->access[g->access_count] = access;
        g->access_address[g->access_count] = address;
        g->access_stores |= (unsigned)store << g->access_count;
    }
    if (g->access_count < 3) ++g->access_count;
}
/* address as base plus a constant: an add of a constant, else itself plus 0 */
static inline LLVMValueRef sg_address_base(LLVMValueRef address, long long* offset) {
    *offset = 0;
    if (LLVMIsAInstruction(address) && LLVMGetInstructionOpcode(address) == LLVMAdd &&
        LLVMIsAConstantInt(LLVMGetOperand(address, 1))) {
        *offset = LLVMConstIntGetSExtValue(LLVMGetOperand(address, 1));
        return LLVMGetOperand(address, 0);
    }
    return address;
}
/* An instruction's two plain 4-byte accesses of adjacent words, both loads or both stores with nothing in between that
   touches memory (LDPSW's two loads, STNP's two stores of words, which Arm makes two accesses on any CPU), become one
   8-byte access as the instruction ends, as ScarletNX's emitters always made a pair of words: a load of both before
   the first, whose halves take the two loads' place, or a store of both where the second was. A big-endian host's
   accesses (not one load or store each) stay two */
static inline void sg_accesses_combine(SgCtx* g) {
    SnxIr* ir = g->ir;
    LLVMValueRef first = g->access[0], second = g->access[1], i, base[2];
    LLVMBasicBlockRef block = LLVMGetInsertBlock(SG_B);
    long long offset[2];
    int stores = g->access_stores == 3;
    if (g->access_count != 2 || (g->access_stores != 0 && !stores)) return;
    if (stores ? !LLVMIsAStoreInst(first) || !LLVMIsAStoreInst(second)
               : !LLVMIsALoadInst(first) || !LLVMIsALoadInst(second))
        return;
    base[0] = sg_address_base(g->access_address[0], &offset[0]);
    base[1] = sg_address_base(g->access_address[1], &offset[1]);
    if (base[0] != base[1] || offset[1] != offset[0] + 4) return;
    if (LLVMGetInstructionParent(first) != LLVMGetInstructionParent(second)) return;
    for (i = LLVMGetNextInstruction(first); i && i != second; i = LLVMGetNextInstruction(i))
        if (LLVMIsALoadInst(i) || LLVMIsAStoreInst(i) || LLVMIsACallInst(i) || LLVMIsAFenceInst(i) ||
            LLVMIsAAtomicRMWInst(i) || LLVMIsAAtomicCmpXchgInst(i))
            return;
    if (!i) return;
    if (stores) {
        LLVMValueRef low, high;
        LLVMPositionBuilderBefore(SG_B, second);
        low = LLVMBuildZExt(SG_B, LLVMGetOperand(first, 0), ir->t_i64, "");
        high = LLVMBuildShl(SG_B, LLVMBuildZExt(SG_B, LLVMGetOperand(second, 0), ir->t_i64, ""), ir_i64(ir, 32), "");
        ir_wr(ir, g->access_address[0], 64, LLVMBuildOr(SG_B, low, high, ""));
    } else {
        LLVMValueRef both;
        LLVMPositionBuilderBefore(SG_B, first);
        both = ir_rd(ir, g->access_address[0], 64);
        LLVMReplaceAllUsesWith(first, LLVMBuildTrunc(SG_B, both, ir->t_i32, ""));
        LLVMReplaceAllUsesWith(second,
                               LLVMBuildTrunc(SG_B, LLVMBuildLShr(SG_B, both, ir_i64(ir, 32), ""), ir->t_i32, ""));
    }
    LLVMInstructionEraseFromParent(first);
    LLVMInstructionEraseFromParent(second);
    LLVMPositionBuilderAtEnd(SG_B, block);
}

/* GuestMemRead(address, size, acquire): size bytes; 16 (a pair of doublewords) are two 64-bit reads */
static inline SgD sg_GuestMemRead(SgCtx* g, SgD address, long long size, int acquire) {
    LLVMValueRef a = sg_address(g, address);
    sg_access_size(size, acquire ? 8 : 16);
    if (size == 16) {
        SgD low = sg_d_from(g, ir_rd(g->ir, a, 64));
        SgD high = sg_d_from(g, ir_rd(g->ir, LLVMBuildAdd(SG_B, a, ir_i64(g->ir, 8), ""), 64));
        return sg_d_concat(g, high, low);
    }
    if (acquire) return sg_d_from(g, ir_ldacq(g->ir, a, 8u * (unsigned)size));
    {
        LLVMValueRef v = ir_rd(g->ir, a, 8u * (unsigned)size);
        if (size == 4) sg_access_note(g, v, a, 0);
        return sg_d_from(g, v);
    }
}
/* GuestMemWrite(address, size, release, value); 16 bytes are two 64-bit writes */
static inline void sg_GuestMemWrite(SgCtx* g, SgD address, long long size, int release, SgD value) {
    LLVMValueRef a = sg_address(g, address), v;
    sg_access_size(size, release ? 8 : 16);
    if (value.w != 8u * (unsigned)size) sg_fail("a guest write of a value of another size", size, value.w);
    if (size == 16) {
        LLVMValueRef low = sg_d_value(g, sg_d_slice(g, value, 63, 0)),
                     high = sg_d_value(g, sg_d_slice(g, value, 127, 64));
        ir_wr(g->ir, a, 64, low);
        ir_wr(g->ir, LLVMBuildAdd(SG_B, a, ir_i64(g->ir, 8), ""), 64, high);
        return;
    }
    v = sg_d_make(g, &value);
    if (release)
        ir_strel(g->ir, a, 8u * (unsigned)size, v);
    else {
        ir_wr(g->ir, a, 8u * (unsigned)size, v);
        if (size == 4) sg_access_note(g, LLVMGetLastInstruction(LLVMGetInsertBlock(SG_B)), a, 1);
    }
}

/* the size's log2, as the exclusive helpers take it */
static inline LLVMValueRef sg_size_log2(SgCtx* g, long long size) {
    return ir_i32(g->ir, size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3);
}
/* GuestLoadExclusive(address, size): ldx of 1 to 8 bytes, or ldxp of a pair of doublewords (16), which arm the monitor
   with what they read; GuestExclusiveValue reads it */
static inline void sg_GuestLoadExclusive(SgCtx* g, SgD address, long long size) {
    SnxIr* ir = g->ir;
    LLVMValueRef a = sg_address(g, address);
    sg_access_size(size, 16);
    if (size == 16) {
        LLVMValueRef first = ir_local(ir, ir->t_i64), second = ir_local(ir, ir->t_i64), args[6];
        args[0] = ir->c;
        args[1] = ir->M;
        args[2] = a;
        args[3] = sg_size_log2(g, 8);
        args[4] = first;
        args[5] = second;
        ir_call_helper(ir, SNX_IR_LDXP, args, 6);
        g->exclusive[0] = ir_load(ir, ir->t_i64, first);
        g->exclusive[1] = ir_load(ir, ir->t_i64, second);
        g->exclusive_count = 2;
    } else {
        LLVMValueRef args[4];
        args[0] = ir->c;
        args[1] = ir->M;
        args[2] = a;
        args[3] = sg_size_log2(g, size);
        g->exclusive[0] = ir_call_helper(ir, SNX_IR_LDX, args, 4);
        g->exclusive_count = 1;
    }
    g->exclusive_next = 0;
}
/* GuestExclusiveValue(address, size): the next of what the load-exclusive read, its low size bytes */
static inline SgD sg_GuestExclusiveValue(SgCtx* g, SgD address, long long size) {
    (void)address;
    if (g->exclusive_next >= g->exclusive_count) sg_fail("an exclusive read with no load-exclusive before it", size, 0);
    if (size > 8) sg_fail("an exclusive read of over 8 bytes", size, 8);
    return sg_d_slice(g, sg_d_from(g, g->exclusive[g->exclusive_next++]), 8 * size - 1, 0);
}
/* GuestStoreExclusive(address, size, value): stx of 1 to 8 bytes, or stxp of a pair of doublewords (16, the first the
   low half); the status GuestExclusiveStatus gives */
static inline void sg_GuestStoreExclusive(SgCtx* g, SgD address, long long size, SgD value) {
    SnxIr* ir = g->ir;
    LLVMValueRef a = sg_address(g, address);
    sg_access_size(size, 16);
    if (value.w != 8u * (unsigned)size) sg_fail("a store-exclusive of a value of another size", size, value.w);
    if (size == 16) {
        LLVMValueRef args[6];
        args[0] = ir->c;
        args[1] = ir->M;
        args[2] = a;
        args[3] = sg_size_log2(g, 8);
        args[4] = sg_d_value(g, sg_d_slice(g, value, 63, 0));
        args[5] = sg_d_value(g, sg_d_slice(g, value, 127, 64));
        g->status = ir_call_helper(ir, SNX_IR_STXP, args, 6);
    } else {
        LLVMValueRef args[5];
        args[0] = ir->c;
        args[1] = ir->M;
        args[2] = a;
        args[3] = sg_size_log2(g, size);
        args[4] = sg_d_value(g, sg_d_zext(g, value, 64));
        g->status = ir_call_helper(ir, SNX_IR_STX, args, 5);
    }
}
static inline SgD sg_GuestExclusiveStatus(SgCtx* g) {
    if (!g->status) sg_fail("an exclusive status with no store-exclusive before it", 0, 0);
    return sg_d_flag(g->status, NULL);
}

/* the old value of an atomic read-modify-write of operand (iN) at address. Bitwise operations and exchanges commute
   with the byte order; on a big-endian host the arithmetic ones are a compare-and-swap loop on the guest's value. So
   are PowerPC64's byte and halfword UMAX and UMIN: its LLVM compares the operand in all 32 bits of its register
   (cmplw after lbarx or lharx, the operand never zero-extended), which bits above the byte or halfword change */
static inline LLVMValueRef sg_rmw_value(SgCtx* g, LLVMAtomicRMWBinOp op, LLVMValueRef old, LLVMValueRef operand) {
    LLVMIntPredicate p;
    switch (op) {
        case LLVMAtomicRMWBinOpAdd: return LLVMBuildAdd(SG_B, old, operand, "");
        case LLVMAtomicRMWBinOpMax: p = LLVMIntSGT; break;
        case LLVMAtomicRMWBinOpMin: p = LLVMIntSLT; break;
        case LLVMAtomicRMWBinOpUMax: p = LLVMIntUGT; break;
        default: p = LLVMIntULT; break;
    }
    return LLVMBuildSelect(SG_B, LLVMBuildICmp(SG_B, p, old, operand, ""), old, operand, "");
}
static inline SgD sg_guest_rmw(SgCtx* g, LLVMAtomicRMWBinOp op, SgD address, SgD value) {
    SnxIr* ir = g->ir;
    LLVMValueRef operand = sg_d_make(g, &value), pointer = ir_mp(ir, sg_address(g, address)), first, old, pair, seen;
    LLVMTypeRef type = LLVMTypeOf(operand);
    LLVMBasicBlockRef entry, loop, done;
    int bitwise = op == LLVMAtomicRMWBinOpXchg || op == LLVMAtomicRMWBinOpAnd || op == LLVMAtomicRMWBinOpOr ||
                  op == LLVMAtomicRMWBinOpXor;
    int partword_unsigned =
        ir->ppc && LLVMGetIntTypeWidth(type) < 32 && (op == LLVMAtomicRMWBinOpUMax || op == LLVMAtomicRMWBinOpUMin);
    sg_access_size(value.w / 8, 8);
    if ((!ir->big_endian || bitwise) && !partword_unsigned) {
        LLVMValueRef rmw = LLVMBuildAtomicRMW(SG_B, op, pointer, ir_guest_order(ir, operand),
                                              LLVMAtomicOrderingSequentiallyConsistent, 0);
        return sg_d_from(g, ir_guest_order(ir, ir_tag_guest(ir, rmw)));
    }
    first = LLVMBuildLoad2(SG_B, type, pointer, "");
    LLVMSetOrdering(first, LLVMAtomicOrderingSequentiallyConsistent);
    LLVMSetAlignment(first, LLVMGetIntTypeWidth(type) / 8);
    ir_tag_guest(ir, first);
    entry = LLVMGetInsertBlock(SG_B);
    loop = ir_block(ir, "rmw");
    done = ir_block(ir, "rmw_done");
    LLVMBuildBr(SG_B, loop);
    LLVMPositionBuilderAtEnd(SG_B, loop);
    old = LLVMBuildPhi(SG_B, type, "");
    pair = LLVMBuildAtomicCmpXchg(
        SG_B, pointer, old, ir_guest_order(ir, sg_rmw_value(g, op, ir_guest_order(ir, old), operand)),
        LLVMAtomicOrderingSequentiallyConsistent, LLVMAtomicOrderingSequentiallyConsistent, 0);
    ir_tag_guest(ir, pair);
    seen = LLVMBuildExtractValue(SG_B, pair, 0, "");
    LLVMAddIncoming(old, &first, &entry, 1);
    LLVMAddIncoming(old, &seen, &loop, 1);
    LLVMBuildCondBr(SG_B, LLVMBuildExtractValue(SG_B, pair, 1, ""), done, loop);
    LLVMPositionBuilderAtEnd(SG_B, done);
    return sg_d_from(g, ir_guest_order(ir, old));
}
static inline SgD sg_GuestAtomicAdd(SgCtx* g, SgD address, SgD v) {
    return sg_guest_rmw(g, LLVMAtomicRMWBinOpAdd, address, v);
}
static inline SgD sg_GuestAtomicAnd(SgCtx* g, SgD address, SgD v) {
    return sg_guest_rmw(g, LLVMAtomicRMWBinOpAnd, address, v);
}
static inline SgD sg_GuestAtomicXor(SgCtx* g, SgD address, SgD v) {
    return sg_guest_rmw(g, LLVMAtomicRMWBinOpXor, address, v);
}
static inline SgD sg_GuestAtomicOr(SgCtx* g, SgD address, SgD v) {
    return sg_guest_rmw(g, LLVMAtomicRMWBinOpOr, address, v);
}
static inline SgD sg_GuestAtomicSMax(SgCtx* g, SgD address, SgD v) {
    return sg_guest_rmw(g, LLVMAtomicRMWBinOpMax, address, v);
}
static inline SgD sg_GuestAtomicSMin(SgCtx* g, SgD address, SgD v) {
    return sg_guest_rmw(g, LLVMAtomicRMWBinOpMin, address, v);
}
static inline SgD sg_GuestAtomicUMax(SgCtx* g, SgD address, SgD v) {
    return sg_guest_rmw(g, LLVMAtomicRMWBinOpUMax, address, v);
}
static inline SgD sg_GuestAtomicUMin(SgCtx* g, SgD address, SgD v) {
    return sg_guest_rmw(g, LLVMAtomicRMWBinOpUMin, address, v);
}
static inline SgD sg_GuestAtomicSwap(SgCtx* g, SgD address, SgD v) {
    return sg_guest_rmw(g, LLVMAtomicRMWBinOpXchg, address, v);
}

/* GuestCompareSwap(address, expected, desired): the old value, desired stored where it equalled expected; seq_cst both
   ways. Of 16 bytes (CASP's doublewords) through snx_cas128, the exclusives' (cpu.h): the two halves read, the new value
   desired if they equal expected and else themselves, retried until the compare-and-swap of what was read succeeds, so
   the old value returned is one the location held */
static inline SgD sg_GuestCompareSwap(SgCtx* g, SgD address, SgD expected, SgD desired) {
    SnxIr* ir = g->ir;
    LLVMValueRef a = sg_address(g, address), lo, hi, equal, args[6], ok, next, e_lo, e_hi, d_lo, d_hi;
    LLVMTypeRef params[6];
    LLVMBasicBlockRef loop, done;
    SgD old;
    if (expected.w != desired.w) sg_fail("a compare-and-swap of values of two sizes", expected.w, desired.w);
    if (expected.w != 128) {
        LLVMValueRef pair;
        sg_access_size(expected.w / 8, 8);
        pair = LLVMBuildAtomicCmpXchg(
            SG_B, ir_mp(ir, a), ir_guest_order(ir, sg_d_make(g, &expected)), ir_guest_order(ir, sg_d_make(g, &desired)),
            LLVMAtomicOrderingSequentiallyConsistent, LLVMAtomicOrderingSequentiallyConsistent, 0);
        ir_tag_guest(ir, pair);
        return sg_d_from(g, ir_guest_order(ir, LLVMBuildExtractValue(SG_B, pair, 0, "")));
    }
    e_lo = sg_d_value(g, sg_d_slice(g, expected, 63, 0));
    e_hi = sg_d_value(g, sg_d_slice(g, expected, 127, 64));
    d_lo = sg_d_value(g, sg_d_slice(g, desired, 63, 0));
    d_hi = sg_d_value(g, sg_d_slice(g, desired, 127, 64));
    loop = ir_block(ir, "cas128");
    done = ir_block(ir, "cas128_done");
    next = LLVMBuildAdd(SG_B, a, ir_i64(ir, 8), "");
    params[0] = ir->t_ptr;
    params[1] = ir->t_ptr;
    params[2] = params[3] = params[4] = params[5] = ir->t_i64;
    LLVMBuildBr(SG_B, loop);
    LLVMPositionBuilderAtEnd(SG_B, loop);
    lo = ir_ldacq(ir, a, 64);
    hi = ir_ldacq(ir, next, 64);
    equal = LLVMBuildAnd(SG_B, LLVMBuildICmp(SG_B, LLVMIntEQ, lo, e_lo, ""),
                         LLVMBuildICmp(SG_B, LLVMIntEQ, hi, e_hi, ""), "");
    /* snx_cas128 takes the doublewords' values, as STXP's (cpu.h), and a big-endian host's puts them in memory order */
    args[0] = ir->c;
    args[1] = ir_mp(ir, a);
    args[2] = lo;
    args[3] = hi;
    args[4] = LLVMBuildSelect(SG_B, equal, d_lo, lo, "");
    args[5] = LLVMBuildSelect(SG_B, equal, d_hi, hi, "");
    ok = ir_call(ir, ir_extern(ir, "snx_cas128", ir->t_i32, params, 6), args, 6);
    LLVMBuildCondBr(SG_B, LLVMBuildICmp(SG_B, LLVMIntNE, ok, ir_i32(ir, 0), ""), done, loop);
    LLVMPositionBuilderAtEnd(SG_B, done);
    old = sg_d_from(g, hi);
    return sg_d_concat(g, old, sg_d_from(g, lo));
}

/* ---- CRC32 and CRC32C ----
   BitReverse(Poly32Mod2(BitReverse(acc) @ Zeros(size) EOR BitReverse(val) @ Zeros(32), poly)) is the bit-reflected
   CRC update without the pre- and post-inversion. On a host without the instruction it is computed by slicing:
   table k holds the CRC of a byte followed by k zero bytes, so the n bytes of (acc XOR value) are n independent
   lookups */
static inline LLVMValueRef sg_crc_table(SgCtx* g, int castagnoli) {
    SnxIr* ir = g->ir;
    const char* name = castagnoli ? "snx_crc32c_slices" : "snx_crc32_slices";
    LLVMValueRef table = LLVMGetNamedGlobal(ir->mod, name);
    if (!table) {
        SnxU32 entries[8 * 256], poly = castagnoli ? 0x82F63B78u : 0xEDB88320u, c;
        LLVMValueRef values[8 * 256];
        LLVMTypeRef type = LLVMArrayType(ir->t_i32, 8 * 256);
        unsigned i, k;
        for (i = 0; i < 256; ++i) {
            c = i;
            for (k = 0; k < 8; ++k) c = (c >> 1) ^ (poly & (0u - (c & 1)));
            entries[i] = c;
        }
        for (i = 256; i < 8 * 256; ++i) entries[i] = (entries[i - 256] >> 8) ^ entries[entries[i - 256] & 0xFF];
        for (i = 0; i < 8 * 256; ++i) values[i] = ir_i32(ir, entries[i]);
        table = LLVMAddGlobal(ir->mod, type, name);
        LLVMSetLinkage(table, LLVMPrivateLinkage);
        LLVMSetUnnamedAddress(table, LLVMGlobalUnnamedAddr);
        LLVMSetGlobalConstant(table, 1);
        LLVMSetInitializer(table, LLVMConstArray(ir->t_i32, values, 8 * 256));
    }
    return table;
}
static inline SgD sg_crc32(SgCtx* g, SgD acc_bits, SgD value_bits, long long size, SgS poly) {
    static const char* const arm[2][4] = {
        {"llvm.aarch64.crc32b", "llvm.aarch64.crc32h", "llvm.aarch64.crc32w", "llvm.aarch64.crc32x"},
        {"llvm.aarch64.crc32cb", "llvm.aarch64.crc32ch", "llvm.aarch64.crc32cw", "llvm.aarch64.crc32cx"}};
    static const char* const x86[4] = {"llvm.x86.sse42.crc32.32.8", "llvm.x86.sse42.crc32.32.16",
                                       "llvm.x86.sse42.crc32.32.32", "llvm.x86.sse42.crc32.64.64"};
    SnxIr* ir = g->ir;
    int castagnoli;
    unsigned esz, bytes, k;
    LLVMValueRef acc = sg_d_make(g, &acc_bits), value, args[2], table, m, r;
    LLVMTypeRef type = LLVMArrayType(ir->t_i32, 8 * 256);
    if (poly.v == 0x1EDC6F41u)
        castagnoli = 1;
    else if (poly.v == 0x04C11DB7u)
        castagnoli = 0;
    else
        sg_fail("a CRC of an unknown polynomial", (long long)poly.v, 0);
    if (size != 8 && size != 16 && size != 32 && size != 64) sg_fail("a CRC of this size", size, 0);
    esz = size == 8 ? 0 : size == 16 ? 1 : size == 32 ? 2 : 3;
    bytes = (unsigned)size / 8;
    /* the value as the register holds it (the low bits of one source: that source, whole) */
    value = sg_d_low_source(g, &value_bits);
    if (ir->features->aarch64 && ir->features->arm_crc) {
        args[0] = acc;
        args[1] = esz == 3 ? value : sg_cast(g, value, 32, 0);
        return sg_d_from(g, sg_intrinsic(g, arm[castagnoli][esz], NULL, args, 2));
    }
    if (castagnoli && ir->features->x86 && ir->features->crc32) {
        if (esz == 3) {
            args[0] = ir_u64(ir, acc);
            args[1] = value;
            return sg_d_from(g, ir_u32(ir, sg_intrinsic(g, x86[3], NULL, args, 2)));
        }
        args[0] = acc;
        args[1] = sg_cast(g, value, 8u << esz, 0);
        return sg_d_from(g, sg_intrinsic(g, x86[esz], NULL, args, 2));
    }
    table = sg_crc_table(g, castagnoli);
    value = sg_cast(g, value, 64, 0);
    m = LLVMBuildXor(SG_B, LLVMBuildAnd(SG_B, value, ir_i64(ir, sg_ones(8 * bytes)), ""), ir_u64(ir, acc), "");
    r = bytes < 4 ? LLVMBuildLShr(SG_B, acc, ir_i32(ir, 8 * bytes), "") : ir_i32(ir, 0);
    for (k = 0; k < bytes; ++k) {
        LLVMValueRef byte = LLVMBuildAnd(SG_B, LLVMBuildLShr(SG_B, m, ir_i64(ir, 8 * k), ""), ir_i64(ir, 0xFF), "");
        LLVMValueRef index[2];
        index[0] = ir_i64(ir, 0);
        index[1] = LLVMBuildAdd(SG_B, byte, ir_i64(ir, 256 * (bytes - 1 - k)), "");
        r = LLVMBuildXor(
            SG_B, r, LLVMBuildLoad2(SG_B, ir->t_i32, LLVMBuildInBoundsGEP2(SG_B, type, table, index, 2, ""), ""), "");
    }
    return sg_d_from(g, r);
}

/* ---- floating point ----
   The SIMD&FP registers (nx.sail's _V, the CPU's v) through ir.h's vector accessors, whose per-block cache of each
   register's low lanes every emitter shares; FPCR (nx.sail's, the CPU's fpcr); and the Arm architecture's
   floating-point functions the Sail calls (a64.roots' lower). Those work on real numbers, so they are lowered here as
   the recompiler's scalar emitters always made them: the host's IEEE arithmetic behind a guard proving it gives the
   console's result and FPSR flags, else cpu.h's soft-float helpers (helpers/fp.c: the Arm ARM's FPUnpack,
   FPProcessNaNs and FPRound), which raise their FPSR flags as FPProcessException does. Each reads FPCR from the value
   the Sail hands it. Halves always take the helpers. The helper calls are the hand-written scalar emitters'
   (tests/support/ir_fpu_reference.c), ported from QEMU (target/arm/tcg/translate-a64.c's do_fp3_scalar and its
   siblings), LGPL-2.1-or-later: Copyright (c) 2013 Alexander Graf <agraf@suse.de>.

   The fast paths: the host's IEEE operations in round to nearest give the helpers' exact result when the result is
   finite and above the smallest normal, or an exact zero, and under FZ no operand is a denormal (unflushed): then no
   NaN was processed (a NaN operand gives a NaN), no infinity was an operand (it gives an infinity or a NaN), a
   denormal operand is the IEEE value it is without FZ and raises nothing, nothing is flushed, tininess (before
   rounding, as the Arm ARM detects it) can't have occurred, and IXC is the only flag. Many games run with FZ set: a
   guard of FZ clear would send all of their arithmetic to the helpers. A result above the smallest normal rounds a
   value above it; a zero is exact for any sum (sums only cancel exactly), and for a product or quotient only with a
   zero operand. IXC is computed exactly where that is cheap (TwoSum for sums; for singles the double product,
   quotient remainder or square, all exact; for doubles an FMA residual where the host has FMA, kept to operands whose
   residual can't underflow), and otherwise the path is taken only once IXC is set: it is sticky, so an inexact result
   changes nothing then. Compares, FMAX and FMIN, widening and narrowing, the FRINTs and the integer conversions have
   guards of their own. Nothing reads the host's FP state, so this assumes only that the host thread never flushes
   denormals (x86's MXCSR.FTZ and DAZ, FPCR.FZ on an AArch64 host), which the runtime never sets. The slow paths call
   the helpers' snx_fp_host_* forms, which try the host's arithmetic on what these guards refuse (NaNs, infinities,
   underflows) before the soft-float */

/* FPRoundingMode's answer while recompiling: the mode FPCR holds when the guest runs, beyond FPRounding's members
   (TIEEVEN to ODD, 0 to 5) */
#define SG_FP_ROUNDING_FPCR 6

/* _V[n] bits hi..lo, an element's low lane of 8 to 64 bits as ir_ve loads it (once a block), the high doubleword as
   ir_vd does, else both doublewords */
static inline LLVMValueRef sg__V_loaded(SgCtx* g, LLVMValueRef v, unsigned n, unsigned k) {
    if (g->vload_count < 8) {
        g->vload[g->vload_count] = v;
        g->vload_n[g->vload_count] = (unsigned char)n;
        g->vload_k[g->vload_count++] = (unsigned char)k;
    }
    return v;
}
static inline SgD sg__V_read_bits(SgCtx* g, long long n, long long hi, long long lo) {
    long long w = hi - lo + 1;
    SgD whole;
    if (n < 0 || n > 31 || lo < 0 || hi > 127 || hi < lo) sg_fail("bits of a SIMD&FP register it does not have", n, hi);
    if (lo == 0 && w == 64) return sg_d_from(g, sg__V_loaded(g, ir_ve(g->ir, (unsigned)n, 3, 0), (unsigned)n, 0));
    if (lo == 0 && (w == 8 || w == 16 || w == 32))
        return sg_d_from(g, ir_ve(g->ir, (unsigned)n, w == 8 ? 0 : w == 16 ? 1 : 2, 0));
    if (lo == 64 && w == 64) return sg_d_from(g, sg__V_loaded(g, ir_vd(g->ir, (unsigned)n, 1), (unsigned)n, 1));
    whole = sg_d_concat(g, sg_d_from(g, sg__V_loaded(g, ir_vd(g->ir, (unsigned)n, 1), (unsigned)n, 1)),
                        sg_d_from(g, sg__V_loaded(g, ir_vd(g->ir, (unsigned)n, 0), (unsigned)n, 0)));
    return sg_d_slice(g, whole, hi, lo);
}
/* doubleword k of v is the value the register already holds, read in this block (Vpart_set's other half) */
static inline int sg__V_holds(SgCtx* g, unsigned n, unsigned k, const SgD* v) {
    const SnxIrVectorCache* cache = &g->ir->state.vectors;
    unsigned i;
    if (k || cache->block != LLVMGetInsertBlock(SG_B) || !((cache->valid[3] >> n) & 1) || v->sel[0] < 2) return 0;
    for (i = 0; i < 64; ++i)
        if (v->sel[i] != v->sel[0] || v->pos[i] != i) return 0;
    return v->src[v->sel[0] - 2] == cache->low[n][3];
}
/* the vector whose bits v's low bits are, the rest 0 (an element loop's lanes, sg_d_from_l): NULL when none */
static inline LLVMValueRef sg_lanes_of(const SgD* v) {
    unsigned s, w, i;
    LLVMValueRef src;
    if (v->sel[0] < 2) return NULL;
    s = v->sel[0] - 2;
    src = v->src[s];
    w = v->src_w[s];
    if (!LLVMIsABitCastInst(src) || LLVMGetTypeKind(LLVMTypeOf(LLVMGetOperand(src, 0))) != LLVMVectorTypeKind ||
        w > v->w)
        return NULL;
    for (i = 0; i < v->w; ++i)
        if (i < w ? v->sel[i] != v->sel[0] || v->pos[i] != i : v->sel[i] != 0) return NULL;
    return LLVMGetOperand(src, 0);
}
/* _V[n] = v, the 128 bits: an element loop's lanes stored as such, the rest zeroed (ir_vec_write); else each
   doubleword stored (ir_set_vd), but one that holds that value already */
static inline void sg__V_write(SgCtx* g, long long n, SgD v) {
    unsigned k;
    LLVMValueRef lanes;
    if (n < 0 || n > 31) sg_fail("an element of _V it does not have", n, 31);
    if (v.w != 128) sg_fail("a SIMD&FP register written with other than 128 bits", n, v.w);
    if ((lanes = sg_lanes_of(&v)) != NULL) {
        ir_vec_write(g->ir, (unsigned)n, lanes);
        return;
    }
    for (k = 0; k < 2; ++k) {
        SgD half = sg_d_slice(g, v, 64 * k + 63, 64 * k);
        if (!sg__V_holds(g, (unsigned)n, k, &half)) ir_set_vd(g->ir, (unsigned)n, k, sg_d_make(g, &half));
    }
}

/* ---- lanes ----
   An element loop walked as one vector operation (gen.c's lanes): each value it makes per element is an SgL, a vector
   of count lanes of w bits; an integer's lanes keep its signedness and widen as an exact result needs, and a slice of
   them narrows back. The IR is the one ir_vec.c builds by hand: a register's lanes are loaded and stored whole
   (ir_vec_read, ir_vec_write) */
typedef struct {
    LLVMValueRef v; /* <count x iw> */
    unsigned w, count;
    int is_signed;
} SgL;

static inline LLVMTypeRef sg_l_type(SgCtx* g, unsigned w, unsigned count) {
    return LLVMVectorType(LLVMIntTypeInContext(g->ir->ctx, w), count);
}
static inline SgL sg_l_of(LLVMValueRef v, int is_signed) {
    SgL l;
    l.v = v;
    l.count = LLVMGetVectorSize(LLVMTypeOf(v));
    l.w = LLVMGetIntTypeWidth(LLVMGetElementType(LLVMTypeOf(v)));
    l.is_signed = is_signed;
    return l;
}
/* esize-bit elements of x (its count of them), as lanes: a SIMD&FP register this instruction loaded whole, in its
   doublewords' order, is loaded as lanes instead (the doublewords' loads are left to die) */
static inline SgL sg_l_elem(SgCtx* g, SgD x, long long esize, unsigned count) {
    unsigned i, k, reg = 99;
    if (esize < 8 || esize > 64 || x.w != (unsigned)esize * count)
        sg_fail("lanes of other than the whole value", esize, x.w);
    for (i = 0; i < x.w; ++i) {
        unsigned s = x.sel[i];
        if (s < 2 || x.pos[i] != i % 64) break;
        for (k = 0; k < g->vload_count && g->vload[k] != x.src[s - 2]; ++k) {}
        if (k == g->vload_count || g->vload_k[k] != i / 64 || (reg != 99 && reg != g->vload_n[k])) break;
        reg = g->vload_n[k];
    }
    if (i == x.w) {
        unsigned esz = esize == 8 ? 0 : esize == 16 ? 1 : esize == 32 ? 2 : 3;
        return sg_l_of(ir_vec_read(g->ir, reg, esz, 0, count), 0);
    }
    return sg_l_of(LLVMBuildBitCast(SG_B, sg_d_make(g, &x), sg_l_type(g, (unsigned)esize, count), ""), 0);
}
/* the lanes as bits, the first lane the lowest: a bitcast, which sg__V_write knows for the lanes it is */
static inline SgD sg_d_from_l(SgCtx* g, SgL l) {
    if (!l.v) sg_fail("lanes read before any was set", 0, 0);
    return sg_d_from(g, LLVMBuildBitCast(SG_B, l.v, LLVMIntTypeInContext(g->ir->ctx, l.w * l.count), ""));
}
/* lanes of exactly w bits: an element set from them (Elem_set) */
static inline SgL sg_l_bits(SgCtx* g, SgL l, long long w) {
    (void)g;
    if (l.w != (unsigned)w) sg_fail("an element set from lanes of another width", l.w, w);
    l.is_signed = 0;
    return l;
}
static inline SgL sg_l_splat(SgCtx* g, LLVMValueRef v, unsigned count, int is_signed) {
    return sg_l_of(ir_vec_splat(g->ir, v, count), is_signed);
}
static inline SgL sg_l_splat_d(SgCtx* g, SgD d, unsigned count) { return sg_l_splat(g, sg_d_make(g, &d), count, 0); }
static inline SgL sg_l_splat_i(SgCtx* g, SgI i, unsigned count) { return sg_l_splat(g, i.v, count, i.is_signed); }
static inline SgL sg_l_splat_b(SgCtx* g, LLVMValueRef b, unsigned count) { return sg_l_splat(g, b, count, 0); }
/* l at w bits, extended by its signedness or cut */
static inline SgL sg_l_width(SgCtx* g, SgL l, unsigned w) {
    LLVMTypeRef t = sg_l_type(g, w, l.count);
    if (w > l.w)
        l.v = l.is_signed ? LLVMBuildSExt(SG_B, l.v, t, "") : LLVMBuildZExt(SG_B, l.v, t, "");
    else if (w < l.w)
        l.v = LLVMBuildTrunc(SG_B, l.v, t, "");
    l.w = w;
    return l;
}
/* two integers' lanes at one width that holds both exactly, signed when either is */
static inline void sg_l_common(SgCtx* g, SgL* a, SgL* b, unsigned extra) {
    unsigned wa = a->w + (!a->is_signed && b->is_signed), wb = b->w + (!b->is_signed && a->is_signed);
    unsigned w = (wa > wb ? wa : wb) + extra;
    int s = a->is_signed || b->is_signed;
    *a = sg_l_width(g, *a, w);
    *b = sg_l_width(g, *b, w);
    a->is_signed = b->is_signed = s;
}
static inline SgL sg_l_add(SgCtx* g, SgL a, SgL b) { return sg_l_of(LLVMBuildAdd(SG_B, a.v, b.v, ""), 0); }
static inline SgL sg_l_sub(SgCtx* g, SgL a, SgL b) { return sg_l_of(LLVMBuildSub(SG_B, a.v, b.v, ""), 0); }
static inline SgL sg_l_and(SgCtx* g, SgL a, SgL b) { return sg_l_of(LLVMBuildAnd(SG_B, a.v, b.v, ""), 0); }
static inline SgL sg_l_or(SgCtx* g, SgL a, SgL b) { return sg_l_of(LLVMBuildOr(SG_B, a.v, b.v, ""), 0); }
static inline SgL sg_l_xor(SgCtx* g, SgL a, SgL b) { return sg_l_of(LLVMBuildXor(SG_B, a.v, b.v, ""), 0); }
static inline SgL sg_l_not(SgCtx* g, SgL a) { return sg_l_of(LLVMBuildNot(SG_B, a.v, ""), 0); }
/* bits as an integer's lanes (UInt, SInt, asl_Int) */
static inline SgL sg_l_int(SgCtx* g, SgL a, int is_signed) {
    (void)g;
    a.is_signed = is_signed;
    return a;
}
/* + - * of integers: exact, one bit wider (or as wide as both for *) */
static inline SgL sg_l_iadd(SgCtx* g, SgL a, SgL b) {
    sg_l_common(g, &a, &b, 1);
    return sg_l_of(LLVMBuildAdd(SG_B, a.v, b.v, ""), a.is_signed);
}
static inline SgL sg_l_isub(SgCtx* g, SgL a, SgL b) {
    a.is_signed = 1; /* a difference may be below 0 */
    sg_l_common(g, &a, &b, 1);
    return sg_l_of(LLVMBuildSub(SG_B, a.v, b.v, ""), 1);
}
static inline SgL sg_l_imul(SgCtx* g, SgL a, SgL b) {
    unsigned w;
    sg_l_common(g, &a, &b, 0);
    w = a.w * 2;
    a = sg_l_width(g, a, w);
    b = sg_l_width(g, b, w);
    return sg_l_of(LLVMBuildMul(SG_B, a.v, b.v, ""), a.is_signed);
}
/* negate: one bit wider, signed, so it is exact */
static inline SgL sg_l_neg(SgCtx* g, SgL a) {
    a = sg_l_width(g, a, a.w + 1);
    return sg_l_of(LLVMBuildNeg(SG_B, a.v, ""), 1);
}
/* Abs: llvm.abs, read unsigned: the most negative lane's magnitude is its own bits that way */
static inline SgL sg_l_abs(SgCtx* g, SgL a) {
    LLVMValueRef args[2];
    if (!a.is_signed) return a;
    args[0] = a.v;
    args[1] = LLVMConstInt(g->ir->t_i1, 0, 0);
    return sg_l_of(sg_intrinsic(g, "llvm.abs", LLVMTypeOf(a.v), args, 2), 0);
}
/* the comparisons gen.c numbers L_EQ to L_GE, of bits (== and !=) or of integers: i1 lanes */
static inline SgL sg_l_cmp(SgCtx* g, unsigned op, SgL a, SgL b) {
    static const LLVMIntPredicate s[] = {LLVMIntEQ, LLVMIntNE, LLVMIntSLT, LLVMIntSLE, LLVMIntSGT, LLVMIntSGE};
    static const LLVMIntPredicate u[] = {LLVMIntEQ, LLVMIntNE, LLVMIntULT, LLVMIntULE, LLVMIntUGT, LLVMIntUGE};
    if (op > 5) sg_fail("a comparison gen.c doesn't number", op, 5);
    if (a.w != b.w || a.is_signed != b.is_signed) sg_l_common(g, &a, &b, 0);
    return sg_l_of(LLVMBuildICmp(SG_B, a.is_signed ? s[op] : u[op], a.v, b.v, ""), 0);
}
static inline SgL sg_l_is_zero(SgCtx* g, SgL a) {
    return sg_l_of(LLVMBuildICmp(SG_B, LLVMIntEQ, a.v, LLVMConstNull(LLVMTypeOf(a.v)), ""), 0);
}
static inline SgL sg_l_select(SgCtx* g, SgL c, SgL a, SgL b) {
    if (a.w != b.w || a.is_signed != b.is_signed) sg_l_common(g, &a, &b, 0);
    return sg_l_of(LLVMBuildSelect(SG_B, c.v, a.v, b.v, ""), a.is_signed);
}
static inline SgL sg_l_minmax(SgCtx* g, SgL a, SgL b, int minimum) {
    SgL c;
    sg_l_common(g, &a, &b, 0);
    c = sg_l_cmp(g, minimum ? 2 : 4, a, b);
    return sg_l_of(LLVMBuildSelect(SG_B, c.v, a.v, b.v, ""), a.is_signed);
}
/* bits hi..lo of each lane (of bits, or of an integer, extended by its sign past its width) */
static inline SgL sg_l_slice(SgCtx* g, SgL a, long long hi, long long lo) {
    unsigned w = (unsigned)(hi - lo + 1);
    if (lo < 0 || hi < lo) sg_fail("a slice of lanes out of order", hi, lo);
    if ((unsigned)hi >= a.w) a = sg_l_width(g, a, (unsigned)hi + 1);
    if (lo) a.v = LLVMBuildLShr(SG_B, a.v, ir_vec_k(g->ir, LLVMTypeOf(a.v), (SnxU64)lo), "");
    a.is_signed = 0;
    return sg_l_width(g, a, w);
}
static inline SgL sg_l_ext(SgCtx* g, SgL a, long long w, int is_signed) {
    a.is_signed = is_signed;
    a = sg_l_width(g, a, (unsigned)w);
    a.is_signed = 0;
    return a;
}
/* lanes picked from lo's, then hi's, by mask (count of them): a shufflevector, its mask made when it recompiles
   (ir_vec.c's unzip of the pairwise ops' Vm:Vn) */
static inline SgL sg_l_permute(SgCtx* g, SgL lo, SgL hi, const unsigned* mask, unsigned count) {
    unsigned i;
    if (count > 64) sg_fail("over 64 lanes picked", count, 64);
    if (lo.count != hi.count || lo.w != hi.w)
        sg_fail("lanes picked from two vectors of other shapes", lo.count, hi.count);
    for (i = 0; i < count; ++i)
        if (mask[i] >= 2 * lo.count) sg_fail("a lane picked past the two vectors' lanes", mask[i], 2 * lo.count);
    return sg_l_of(ir_vec_shuffle(g->ir, lo.v, hi.v, mask, count), 0);
}
/* every lane folded into one value, a vector of one lane as ir_vec.c's across() writes it: llvm.vector.reduce.add of
   an integer's lanes (exact: at the narrowest power of two of bits that holds the sum of them all, which a slice of
   twice a lane's width then leaves as it is) or of bits (modular, as wide as a lane) */
static inline SgL sg_l_reduce_add(SgCtx* g, SgL a, int exact) {
    LLVMValueRef sum;
    if (exact) {
        unsigned need = a.w, w = 8, k;
        for (k = 1; k < a.count; k *= 2) ++need;
        while (w < need) w *= 2;
        a = sg_l_width(g, a, w);
    }
    sum = sg_intrinsic(g, "llvm.vector.reduce.add", LLVMTypeOf(a.v), &a.v, 1);
    return sg_l_of(ir_vec_splat(g->ir, sum, 1), exact && a.is_signed);
}
/* every lane folded by Min or Max, by the lanes' signedness: llvm.vector.reduce.[su]{min,max}, as one lane */
static inline SgL sg_l_reduce_minmax(SgCtx* g, SgL a, int minimum) {
    static const char* const names[2][2] = {{"llvm.vector.reduce.umax", "llvm.vector.reduce.smax"},
                                            {"llvm.vector.reduce.umin", "llvm.vector.reduce.smin"}};
    LLVMValueRef r = sg_intrinsic(g, names[minimum != 0][a.is_signed != 0], LLVMTypeOf(a.v), &a.v, 1);
    return sg_l_of(ir_vec_splat(g->ir, r, 1), a.is_signed);
}

/* FPCR, as the CPU keeps it: 32 bits, the rest 0 */
static inline SgD sg_FPCR_read(SgCtx* g) {
    return sg_d_zext(g, sg_d_from(g, ir_cpu_load(g->ir, g->ir->t_i32, offsetof(CPU, fpcr))), 64);
}
/* the 32 bits of FPCR a function was handed: sg_FPCR_read's load itself */
static inline LLVMValueRef sg_fp_fpcr(SgCtx* g, SgD fpcr) {
    unsigned i;
    int low = fpcr.w >= 32 && fpcr.sel[0] >= 2 && fpcr.src_w[fpcr.sel[0] - 2] == 32;
    for (i = 0; low && i < 32; ++i) low = fpcr.sel[i] == fpcr.sel[0] && fpcr.pos[i] == i;
    if (low) return fpcr.src[fpcr.sel[0] - 2];
    return sg_cast(g, sg_d_make(g, &fpcr), 32, 0);
}
/* i1: none of FPCR's bits of mask is set */
static inline LLVMValueRef sg_fp_clear(SgCtx* g, LLVMValueRef fpcr, SnxU32 mask) {
    return LLVMBuildICmp(SG_B, LLVMIntEQ, LLVMBuildAnd(SG_B, fpcr, ir_i32(g->ir, mask), ""), ir_i32(g->ir, 0), "");
}
/* FPCR.RMode as the helpers' rounding mode (0-3) */
static inline LLVMValueRef sg_fp_rmode(SgCtx* g, LLVMValueRef fpcr) {
    return LLVMBuildAnd(SG_B, LLVMBuildLShr(SG_B, fpcr, ir_i32(g->ir, 22), ""), ir_i32(g->ir, 3), "");
}

/* where a lowered function runs: under gen.c's run-time condition (FCCMP's compare only where the condition holds),
   its own blocks, and an empty one the other way takes (where a state written after both ways can be written on each,
   sg_PSTATE_write), the result 0 where it did not run */
typedef struct {
    LLVMBasicBlockRef from, join;
    int never;
} SgWhen;
static inline int sg_fp_when(SgCtx* g, LLVMValueRef path, SgWhen* w) {
    LLVMBasicBlockRef run;
    w->from = w->join = NULL;
    w->never = path && LLVMIsAConstantInt(path) && !LLVMConstIntGetZExtValue(path);
    if (!path || LLVMIsAConstantInt(path)) return !w->never;
    run = ir_block(g->ir, "sg_when");
    w->from = ir_block(g->ir, "sg_when_skip");
    w->join = ir_block(g->ir, "sg_when_join");
    LLVMBuildCondBr(SG_B, path, run, w->from);
    LLVMPositionBuilderAtEnd(SG_B, w->from);
    LLVMBuildBr(SG_B, w->join);
    LLVMPositionBuilderAtEnd(SG_B, run);
    g->when_path = path;
    g->when_skip = w->from;
    return 1;
}
/* the end of the way that ran: to the join, where the builder goes */
static inline LLVMBasicBlockRef sg_fp_when_join(SgCtx* g, SgWhen* w) {
    LLVMBasicBlockRef end = LLVMGetInsertBlock(SG_B);
    LLVMBuildBr(SG_B, w->join);
    LLVMPositionBuilderAtEnd(SG_B, w->join);
    g->when_end = end;
    return end;
}
static inline LLVMValueRef sg_fp_when_end(SgCtx* g, SgWhen* w, LLVMValueRef r, LLVMTypeRef type) {
    LLVMValueRef phi, values[2];
    LLVMBasicBlockRef blocks[2];
    if (w->never) return LLVMConstNull(type);
    if (!w->join) return r;
    blocks[0] = sg_fp_when_join(g, w);
    phi = LLVMBuildPhi(SG_B, type, "");
    values[0] = r;
    values[1] = LLVMConstNull(type);
    blocks[1] = w->from;
    LLVMAddIncoming(phi, values, blocks, 2);
    return phi;
}

static inline LLVMTypeRef sg_fp_type(SnxIr* ir, unsigned bits) {
    return bits == 16   ? LLVMHalfTypeInContext(ir->ctx)
           : bits == 32 ? LLVMFloatTypeInContext(ir->ctx)
                        : LLVMDoubleTypeInContext(ir->ctx);
}
static inline LLVMValueRef sg_fp_kint(SnxIr* ir, unsigned bits, SnxU64 v) {
    return LLVMConstInt(ir_int_type(ir, bits), v, 0);
}
static inline LLVMValueRef sg_fp_kfloat(SnxIr* ir, unsigned bits, double v) {
    return LLVMConstReal(sg_fp_type(ir, bits), v);
}
static inline LLVMValueRef sg_fp_to_width(SnxIr* ir, LLVMValueRef bits, unsigned width) {
    return ir_cast(ir, bits, ir_int_type(ir, width), 0);
}

/* ---- the helpers (cpu.h): integer arithmetic on IEEE encodings, taking FPCR and returning the flags raised ---- */

static inline LLVMValueRef sg_fp_call_binary(SnxIr* ir, LLVMValueRef fpcr, LLVMValueRef a, LLVMValueRef b,
                                             unsigned width, unsigned op) {
    LLVMValueRef operands[4];
    operands[0] = u64_of(ir, a);
    operands[1] = u64_of(ir, b);
    operands[2] = ir_i32(ir, width);
    operands[3] = ir_i32(ir, op);
    return sg_fp_to_width(ir, ir_fp_reply_call_fpcr(ir, "snx_fp_host_binary", operands, 4, 1, fpcr), width);
}
static inline LLVMValueRef sg_fp_call_fma(SnxIr* ir, LLVMValueRef fpcr, LLVMValueRef add, LLVMValueRef a,
                                          LLVMValueRef b, unsigned width) {
    LLVMValueRef operands[4];
    operands[0] = u64_of(ir, add);
    operands[1] = u64_of(ir, a);
    operands[2] = u64_of(ir, b);
    operands[3] = ir_i32(ir, width);
    return sg_fp_to_width(ir, ir_fp_reply_call_fpcr(ir, "snx_fp_host_fma", operands, 4, 0, fpcr), width);
}
static inline LLVMValueRef sg_fp_call_unary(SnxIr* ir, LLVMValueRef fpcr, LLVMValueRef a, unsigned width, unsigned op) {
    LLVMValueRef operands[3];
    operands[0] = u64_of(ir, a);
    operands[1] = ir_i32(ir, width);
    operands[2] = ir_i32(ir, op);
    return sg_fp_to_width(ir, ir_fp_reply_call_fpcr(ir, "snx_fp_host_unary", operands, 3, 0, fpcr), width);
}
static inline LLVMValueRef sg_fp_call_minmax(SnxIr* ir, LLVMValueRef fpcr, LLVMValueRef a, LLVMValueRef b,
                                             unsigned width, unsigned minimum, unsigned numeric) {
    LLVMValueRef operands[5];
    operands[0] = u64_of(ir, a);
    operands[1] = u64_of(ir, b);
    operands[2] = ir_i32(ir, width);
    operands[3] = ir_i32(ir, minimum);
    operands[4] = ir_i32(ir, numeric);
    return sg_fp_to_width(ir, ir_fp_reply_call_fpcr(ir, "snx_fp_host_minmax", operands, 5, 0, fpcr), width);
}
/* the NZCV nibble (i32) */
static inline LLVMValueRef sg_fp_call_compare(SnxIr* ir, LLVMValueRef fpcr, LLVMValueRef a, LLVMValueRef b,
                                              unsigned width, unsigned signal) {
    LLVMValueRef operands[4];
    operands[0] = u64_of(ir, a);
    operands[1] = u64_of(ir, b);
    operands[2] = ir_i32(ir, width);
    operands[3] = ir_i32(ir, signal);
    return ir_cast(ir, ir_fp_reply_call_fpcr(ir, "snx_fp_host_compare", operands, 4, 0, fpcr), ir->t_i32, 0);
}
/* rm: an i32, FPCR's mode or the encoding's */
static inline LLVMValueRef sg_fp_call_convert(SnxIr* ir, LLVMValueRef fpcr, LLVMValueRef a, unsigned from, unsigned to,
                                              LLVMValueRef rm) {
    LLVMValueRef operands[4];
    operands[0] = u64_of(ir, a);
    operands[1] = ir_i32(ir, from);
    operands[2] = ir_i32(ir, to);
    operands[3] = rm;
    return sg_fp_to_width(ir, ir_fp_reply_call_fpcr(ir, "snx_fp_convert", operands, 4, 0, fpcr), to);
}
static inline LLVMValueRef sg_fp_call_round(SnxIr* ir, LLVMValueRef fpcr, LLVMValueRef a, unsigned width,
                                            LLVMValueRef rm, unsigned exact) {
    LLVMValueRef operands[4];
    operands[0] = u64_of(ir, a);
    operands[1] = ir_i32(ir, width);
    operands[2] = rm;
    operands[3] = ir_i32(ir, exact);
    return sg_fp_to_width(ir, ir_fp_reply_call_fpcr(ir, "snx_fp_round", operands, 4, 0, fpcr), width);
}
static inline LLVMValueRef sg_fp_call_round_int(SnxIr* ir, LLVMValueRef fpcr, LLVMValueRef a, unsigned width,
                                                LLVMValueRef rm, unsigned intsize) {
    LLVMValueRef operands[4];
    operands[0] = u64_of(ir, a);
    operands[1] = ir_i32(ir, width);
    operands[2] = rm;
    operands[3] = ir_i32(ir, intsize);
    return sg_fp_to_width(ir, ir_fp_reply_call_fpcr(ir, "snx_fp_round_int", operands, 4, 0, fpcr), width);
}
/* the integer (i64, masked to its width) */
static inline LLVMValueRef sg_fp_call_to_int(SnxIr* ir, LLVMValueRef fpcr, LLVMValueRef a, unsigned width,
                                             unsigned integer, unsigned uns, unsigned fbits, unsigned rm) {
    LLVMValueRef operands[6];
    operands[0] = u64_of(ir, a);
    operands[1] = ir_i32(ir, width);
    operands[2] = ir_i32(ir, integer);
    operands[3] = ir_i32(ir, uns);
    operands[4] = ir_i32(ir, fbits);
    operands[5] = ir_i32(ir, rm);
    return ir_fp_reply_call_fpcr(ir, "snx_fp_to_int", operands, 6, 0, fpcr);
}
static inline LLVMValueRef sg_fp_call_from_int(SnxIr* ir, LLVMValueRef fpcr, LLVMValueRef v, unsigned integer,
                                               unsigned width, unsigned uns, unsigned fbits) {
    LLVMValueRef operands[5];
    operands[0] = u64_of(ir, v);
    operands[1] = ir_i32(ir, integer);
    operands[2] = ir_i32(ir, width);
    operands[3] = ir_i32(ir, uns);
    operands[4] = ir_i32(ir, fbits);
    return sg_fp_to_width(ir, ir_fp_reply_call_fpcr(ir, "snx_fp_from_int", operands, 5, 0, fpcr), width);
}

/* ---- the fast paths' guards ---- */

static inline SnxU64 sg_fp_inf(unsigned width) {
    return width == 16 ? 0x7C00u : width == 32 ? 0x7F800000u : 0x7FF0000000000000ull;
}
static inline SnxU64 sg_fp_min_normal(unsigned width) {
    return width == 16 ? 0x0400u : width == 32 ? 0x00800000u : 0x0010000000000000ull;
}
/* doubles whose FMA residuals (products of two ulps at least 2^-1074 apart from the operand's) stay representable */
#define SG_FP_RESIDUAL_FLOOR 0x03F0000000000000ull /* 2^-960 */

static inline LLVMValueRef sg_fp_magnitude(SnxIr* ir, unsigned width, LLVMValueRef bits) {
    return LLVMBuildAnd(ir->b, bits, sg_fp_kint(ir, width, ~sign_mask(width) & (width == 32 ? 0xFFFFFFFFu : ~0ull)),
                        "");
}
static inline LLVMValueRef sg_fp_is_zero(SnxIr* ir, unsigned width, LLVMValueRef bits) {
    return LLVMBuildICmp(ir->b, LLVMIntEQ, sg_fp_magnitude(ir, width, bits), sg_fp_kint(ir, width, 0), "");
}
static inline LLVMValueRef sg_fp_is_nan(SnxIr* ir, unsigned width, LLVMValueRef bits) {
    return LLVMBuildICmp(ir->b, LLVMIntUGT, sg_fp_magnitude(ir, width, bits), sg_fp_kint(ir, width, sg_fp_inf(width)),
                         "");
}
static inline LLVMValueRef sg_fp_is_denormal(SnxIr* ir, unsigned width, LLVMValueRef bits) {
    LLVMValueRef m = sg_fp_magnitude(ir, width, bits);
    return both(ir, LLVMBuildICmp(ir->b, LLVMIntNE, m, sg_fp_kint(ir, width, 0), ""),
                LLVMBuildICmp(ir->b, LLVMIntULT, m, sg_fp_kint(ir, width, sg_fp_min_normal(width)), ""));
}
/* lo <= |bits| < hi */
static inline LLVMValueRef sg_fp_magnitude_in(SnxIr* ir, unsigned width, LLVMValueRef bits, SnxU64 lo, SnxU64 hi) {
    LLVMValueRef offset = LLVMBuildSub(ir->b, sg_fp_magnitude(ir, width, bits), sg_fp_kint(ir, width, lo), "");
    return LLVMBuildICmp(ir->b, LLVMIntULT, offset, sg_fp_kint(ir, width, hi - lo), "");
}
/* finite and above the smallest normal */
static inline LLVMValueRef sg_fp_roomy(SnxIr* ir, unsigned width, LLVMValueRef bits) {
    return sg_fp_magnitude_in(ir, width, bits, sg_fp_min_normal(width) + 1, sg_fp_inf(width));
}
/* a denormal operand is fine unless FPCR.FZ flushes it (which raises IDC) */
static inline LLVMValueRef sg_fp_unflushed(SgCtx* g, LLVMValueRef fpcr, unsigned width, LLVMValueRef bits) {
    return either(g->ir, sg_fp_clear(g, fpcr, FPCR_FZ), LLVMBuildNot(SG_B, sg_fp_is_denormal(g->ir, width, bits), ""));
}
static inline LLVMValueRef sg_fp_as_float(SnxIr* ir, LLVMValueRef bits, unsigned width) {
    return LLVMBuildBitCast(ir->b, bits, sg_fp_type(ir, width), "");
}
static inline LLVMValueRef sg_fp_as_bits(SnxIr* ir, LLVMValueRef f, unsigned width) {
    return LLVMBuildBitCast(ir->b, f, ir_int_type(ir, width), "");
}
static inline LLVMValueRef sg_fp_widen(SnxIr* ir, LLVMValueRef f) {
    return LLVMBuildFPExt(ir->b, f, LLVMDoubleTypeInContext(ir->ctx), "");
}

/* A value from the host where ok holds, from the helpers otherwise: sg_fp_split branches (positioning at the slow block
   for the caller's helper call), sg_fp_split_end joins them. The fast block sets IXC when inexact (NULL: never) */
typedef struct {
    LLVMBasicBlockRef fast_end, clear_end, join;
    LLVMValueRef fast;
} SgFpSplit;
typedef struct {
    LLVMValueRef a, b, add, result;
    unsigned width;
} SgFpResidual;
static inline void sg_fp_split_fma(SnxIr* ir, LLVMValueRef ok, LLVMValueRef fast, LLVMValueRef inexact,
                                   const SgFpResidual* residual, SgFpSplit* s) {
    LLVMBasicBlockRef on = ir_block(ir, "fp_fast"), off = ir_block(ir, "fp_slow");
    s->join = ir_block(ir, "fp_join");
    s->clear_end = NULL;
    s->fast = fast;
    if (residual) {
        LLVMBasicBlockRef check = ir_block(ir, "fp_fma_check"), clear = ir_block(ir, "fp_ixc");
        LLVMBasicBlockRef checked = ir_block(ir, "fp_residual_valid");
        LLVMValueRef valid;
        ir_likely(ir, LLVMBuildCondBr(ir->b, both(ir, ok, ixc_set(ir)), on, check));
        LLVMPositionBuilderAtEnd(ir->b, on);
        s->fast_end = on;
        LLVMBuildBr(ir->b, s->join);
        LLVMPositionBuilderAtEnd(ir->b, check);
        LLVMBuildCondBr(ir->b, ok, clear, off);
        LLVMPositionBuilderAtEnd(ir->b, clear);
        inexact = ir_fp_fma_error(ir, sg_fp_as_float(ir, residual->a, residual->width),
                                  sg_fp_as_float(ir, residual->b, residual->width),
                                  sg_fp_as_float(ir, residual->add, residual->width),
                                  sg_fp_as_float(ir, residual->result, residual->width), residual->width, &valid);
        LLVMBuildCondBr(ir->b, valid, checked, off);
        LLVMPositionBuilderAtEnd(ir->b, checked);
        ir_fp_raise_ixc(ir, inexact);
        s->clear_end = checked;
        LLVMBuildBr(ir->b, s->join);
        LLVMPositionBuilderAtEnd(ir->b, off);
        return;
    }
    ir_likely(ir, LLVMBuildCondBr(ir->b, ok, on, off));
    LLVMPositionBuilderAtEnd(ir->b, on);
    /* only while IXC is clear: it is sticky, and LLVM sinks the inexactness test into that block */
    if (inexact) {
        LLVMBasicBlockRef clear = ir_block(ir, "fp_ixc"), done = ir_block(ir, "fp_ixc_done");
        ir_unlikely(ir, LLVMBuildCondBr(ir->b, LLVMBuildNot(ir->b, ixc_set(ir), ""), clear, done));
        LLVMPositionBuilderAtEnd(ir->b, clear);
        ir_fp_raise_ixc(ir, inexact);
        LLVMBuildBr(ir->b, done);
        LLVMPositionBuilderAtEnd(ir->b, done);
    }
    s->fast_end = LLVMGetInsertBlock(ir->b);
    LLVMBuildBr(ir->b, s->join);
    LLVMPositionBuilderAtEnd(ir->b, off);
}
static inline void sg_fp_split(SnxIr* ir, LLVMValueRef ok, LLVMValueRef fast, LLVMValueRef inexact, SgFpSplit* s) {
    sg_fp_split_fma(ir, ok, fast, inexact, NULL, s);
}
static inline LLVMValueRef sg_fp_split_end(SnxIr* ir, SgFpSplit* s, LLVMValueRef slow) {
    LLVMBasicBlockRef slow_end = LLVMGetInsertBlock(ir->b), blocks[3];
    LLVMValueRef values[3], phi;
    LLVMBuildBr(ir->b, s->join);
    LLVMPositionBuilderAtEnd(ir->b, s->join);
    phi = LLVMBuildPhi(ir->b, LLVMTypeOf(slow), "");
    values[0] = s->fast;
    values[1] = slow;
    blocks[0] = s->fast_end;
    blocks[1] = slow_end;
    values[2] = s->fast;
    blocks[2] = s->clear_end;
    LLVMAddIncoming(phi, values, blocks, s->clear_end ? 3 : 2);
    return phi;
}

/* a op b, op SNX_FP_ADD, SUB, MUL or DIV, for singles and doubles: the host's result, with *ok and *inexact */
static inline LLVMValueRef sg_fp_fast_binary(SgCtx* g, LLVMValueRef fpcr, unsigned op, unsigned width, LLVMValueRef a,
                                             LLVMValueRef b, LLVMValueRef* ok, LLVMValueRef* inexact) {
    SnxIr* ir = g->ir;
    LLVMValueRef fa = sg_fp_as_float(ir, a, width), fb = sg_fp_as_float(ir, b, width), r, rb, exact_zero, cut = NULL;
    *inexact = NULL;
    switch (op) {
        case SNX_FP_ADD:
        case SNX_FP_SUB: { /* TwoSum: the rounding error of fa + fb, exactly */
            LLVMValueRef t, va, vb;
            if (op == SNX_FP_SUB) fb = LLVMBuildFNeg(SG_B, fb, "");
            r = LLVMBuildFAdd(SG_B, fa, fb, "");
            t = LLVMBuildFSub(SG_B, r, fa, "");
            va = LLVMBuildFSub(SG_B, fa, LLVMBuildFSub(SG_B, r, t, ""), "");
            vb = LLVMBuildFSub(SG_B, fb, t, "");
            *inexact = float_ne(ir, LLVMBuildFAdd(SG_B, va, vb, ""), sg_fp_kfloat(ir, width, 0.0));
            rb = sg_fp_as_bits(ir, r, width);
            exact_zero = sg_fp_is_zero(ir, width, rb);
            break;
        }
        case SNX_FP_MUL:
            r = LLVMBuildFMul(SG_B, fa, fb, "");
            rb = sg_fp_as_bits(ir, r, width);
            exact_zero = both(ir, sg_fp_is_zero(ir, width, rb),
                              either(ir, sg_fp_is_zero(ir, width, a), sg_fp_is_zero(ir, width, b)));
            if (width == 32)
                *inexact =
                    float_ne(ir, LLVMBuildFMul(SG_B, sg_fp_widen(ir, fa), sg_fp_widen(ir, fb), ""), sg_fp_widen(ir, r));
            else if (host_fma_scalar(ir)) {
                *inexact = float_ne(ir, fma3(ir, fa, fb, LLVMBuildFNeg(SG_B, r, "")), sg_fp_kfloat(ir, 64, 0.0));
                cut = either(ir, exact_zero, sg_fp_magnitude_in(ir, 64, rb, SG_FP_RESIDUAL_FLOOR, sg_fp_inf(64)));
            }
            break;
        default: /* SNX_FP_DIV */
            r = LLVMBuildFDiv(SG_B, fa, fb, "");
            rb = sg_fp_as_bits(ir, r, width);
            exact_zero = both(ir, sg_fp_is_zero(ir, width, rb), sg_fp_is_zero(ir, width, a));
            if (width == 32) {
                LLVMValueRef product = LLVMBuildFMul(SG_B, sg_fp_widen(ir, r), sg_fp_widen(ir, fb), "");
                *inexact =
                    float_ne(ir, LLVMBuildFSub(SG_B, sg_fp_widen(ir, fa), product, ""), sg_fp_kfloat(ir, 64, 0.0));
            } else if (host_fma_scalar(ir)) {
                *inexact = float_ne(ir, fma3(ir, LLVMBuildFNeg(SG_B, r, ""), fb, fa), sg_fp_kfloat(ir, 64, 0.0));
                cut = either(ir, sg_fp_is_zero(ir, 64, a),
                             sg_fp_magnitude_in(ir, 64, a, SG_FP_RESIDUAL_FLOOR, sg_fp_inf(64)));
            }
            /* a zero divided by an infinity is an exact zero, though its residual (0 * inf) is a NaN */
            if (*inexact) *inexact = both(ir, *inexact, LLVMBuildNot(SG_B, exact_zero, ""));
            break;
    }
    *ok = both(ir,
               both(ir, sg_fp_clear(g, fpcr, FPCR_RMODE),
                    both(ir, sg_fp_unflushed(g, fpcr, width, a), sg_fp_unflushed(g, fpcr, width, b))),
               either(ir, sg_fp_roomy(ir, width, rb), exact_zero));
    if (cut) *ok = both(ir, *ok, cut);
    if (!*inexact) *ok = both(ir, *ok, ixc_set(ir));
    return rb;
}

/* the square root, for singles and doubles on hosts with the instruction */
static inline LLVMValueRef sg_fp_fast_sqrt(SgCtx* g, LLVMValueRef fpcr, unsigned width, LLVMValueRef a,
                                           LLVMValueRef* ok, LLVMValueRef* inexact) {
    SnxIr* ir = g->ir;
    LLVMValueRef fa = sg_fp_as_float(ir, a, width), r = ir_intrinsic(ir, "llvm.sqrt", LLVMTypeOf(fa), &fa, 1);
    LLVMValueRef rb = sg_fp_as_bits(ir, r, width), zero = sg_fp_is_zero(ir, width, a);
    *inexact = NULL;
    *ok = both(ir, both(ir, sg_fp_clear(g, fpcr, FPCR_RMODE), sg_fp_unflushed(g, fpcr, width, a)),
               either(ir, sg_fp_roomy(ir, width, rb), zero));
    if (width == 32)
        *inexact = float_ne(ir, sg_fp_widen(ir, fa), LLVMBuildFMul(SG_B, sg_fp_widen(ir, r), sg_fp_widen(ir, r), ""));
    else if (host_fma_scalar(ir)) {
        *inexact = float_ne(ir, fma3(ir, LLVMBuildFNeg(SG_B, r, ""), r, fa), sg_fp_kfloat(ir, 64, 0.0));
        *ok = both(ir, *ok, either(ir, zero, sg_fp_magnitude_in(ir, 64, a, SG_FP_RESIDUAL_FLOOR, sg_fp_inf(64))));
    } else
        *ok = both(ir, *ok, ixc_set(ir));
    return rb;
}

/* add + n * m rounded once, with an exact residual checked only while IXC is clear */
static inline LLVMValueRef sg_fp_fast_fma(SgCtx* g, LLVMValueRef fpcr, unsigned width, LLVMValueRef add, LLVMValueRef n,
                                          LLVMValueRef m, LLVMValueRef* ok, SgFpResidual* residual) {
    SnxIr* ir = g->ir;
    LLVMValueRef a = sg_fp_as_float(ir, n, width), b = sg_fp_as_float(ir, m, width), d = sg_fp_as_float(ir, add, width);
    LLVMValueRef value = fma3(ir, a, b, d), r = sg_fp_as_bits(ir, value, width);
    LLVMValueRef zero_product = either(ir, sg_fp_is_zero(ir, width, n), sg_fp_is_zero(ir, width, m));
    LLVMValueRef result = either(ir, sg_fp_roomy(ir, width, r), both(ir, sg_fp_is_zero(ir, width, r), zero_product));
    LLVMValueRef operands = both(ir, sg_fp_unflushed(g, fpcr, width, add),
                                 both(ir, sg_fp_unflushed(g, fpcr, width, n), sg_fp_unflushed(g, fpcr, width, m)));
    LLVMValueRef control = sg_fp_clear(g, fpcr, FPCR_RMODE);
    /* at O0 the path stays the sticky one: no residual block for SROA to leave its FP temporaries in */
    if (!ir->promote) control = both(ir, control, ixc_set(ir));
    *ok = both(ir, control, both(ir, operands, result));
    /* the raw operands already live across the helper branch: their FP views are made again only in the cold
       residual block, so O0 spills no second set of FP temporaries on the sticky path */
    residual->a = n;
    residual->b = m;
    residual->add = add;
    residual->result = r;
    residual->width = width;
    return r;
}

/* without FPSR's flags (snx_ir_fpsr_flags): the host's result wherever it is not a NaN, in round to nearest without
   FZ (infinities, denormals and zeros are the IEEE values the console gives there too), and where the exact guard
   holds, as under FZ */
static inline void sg_fp_relax(SgCtx* g, LLVMValueRef fpcr, unsigned width, LLVMValueRef value, LLVMValueRef* ok,
                               LLVMValueRef* inexact) {
    if (snx_ir_fpsr_exact()) return;
    *ok = either(g->ir, *ok,
                 both(g->ir, sg_fp_clear(g, fpcr, FPCR_RMODE | FPCR_FZ),
                      LLVMBuildNot(SG_B, sg_fp_is_nan(g->ir, width, value), "")));
    if (inexact) *inexact = NULL;
}

/* ---- the lowered functions ---- */

/* FPAdd, FPSub, FPMul, FPDiv */
static inline SgD sg_fp_binary(SgCtx* g, LLVMValueRef path, SgD a, SgD b, SgD fpcr_bits, unsigned op) {
    SnxIr* ir = g->ir;
    unsigned width = a.w;
    LLVMValueRef n = sg_d_make(g, &a), m = sg_d_make(g, &b), fpcr = sg_fp_fpcr(g, fpcr_bits), r = NULL;
    SgWhen w;
    if (sg_fp_when(g, path, &w)) {
        if (width != 16) {
            LLVMValueRef ok, inexact, fast = sg_fp_fast_binary(g, fpcr, op, width, n, m, &ok, &inexact);
            SgFpSplit s;
            sg_fp_relax(g, fpcr, width, fast, &ok, &inexact);
            sg_fp_split(ir, ok, fast, inexact, &s);
            r = sg_fp_split_end(ir, &s, sg_fp_call_binary(ir, fpcr, n, m, width, op));
        } else
            r = sg_fp_call_binary(ir, fpcr, n, m, width, op);
    }
    return sg_d_from(g, sg_fp_when_end(g, &w, r, ir_int_type(ir, width)));
}
static inline SgD sg_FPAdd(SgCtx* g, LLVMValueRef path, SgD a, SgD b, SgD fpcr) {
    return sg_fp_binary(g, path, a, b, fpcr, SNX_FP_ADD);
}
static inline SgD sg_FPSub(SgCtx* g, LLVMValueRef path, SgD a, SgD b, SgD fpcr) {
    return sg_fp_binary(g, path, a, b, fpcr, SNX_FP_SUB);
}
static inline SgD sg_FPMul(SgCtx* g, LLVMValueRef path, SgD a, SgD b, SgD fpcr) {
    return sg_fp_binary(g, path, a, b, fpcr, SNX_FP_MUL);
}
static inline SgD sg_FPDiv(SgCtx* g, LLVMValueRef path, SgD a, SgD b, SgD fpcr) {
    return sg_fp_binary(g, path, a, b, fpcr, SNX_FP_DIV);
}

/* FPMax, FPMin and the Num forms. Without a NaN, and without a denormal FPCR.FZ flushes, they give the larger or
   smaller operand unchanged, and of equal ones their AND (max: +0 over -0) or OR (min), raising nothing */
static inline SgD sg_fp_minmax(SgCtx* g, LLVMValueRef path, SgD a, SgD b, SgD fpcr_bits, unsigned minimum,
                               unsigned numeric) {
    SnxIr* ir = g->ir;
    unsigned width = a.w;
    LLVMValueRef n = sg_d_make(g, &a), m = sg_d_make(g, &b), fpcr = sg_fp_fpcr(g, fpcr_bits), r = NULL;
    SgWhen w;
    if (sg_fp_when(g, path, &w)) {
        if (width != 16) {
            LLVMValueRef fn = sg_fp_as_float(ir, n, width), fm = sg_fp_as_float(ir, m, width), ok, fast;
            LLVMRealPredicate wins = minimum ? LLVMRealOLT : LLVMRealOGT;
            SgFpSplit s;
            ok = both(ir, LLVMBuildNot(SG_B, either(ir, sg_fp_is_nan(ir, width, n), sg_fp_is_nan(ir, width, m)), ""),
                      both(ir, sg_fp_unflushed(g, fpcr, width, n), sg_fp_unflushed(g, fpcr, width, m)));
            fast = minimum ? LLVMBuildOr(SG_B, n, m, "") : LLVMBuildAnd(SG_B, n, m, "");
            fast = LLVMBuildSelect(SG_B, LLVMBuildFCmp(SG_B, wins, fm, fn, ""), m, fast, "");
            fast = LLVMBuildSelect(SG_B, LLVMBuildFCmp(SG_B, wins, fn, fm, ""), n, fast, "");
            sg_fp_split(ir, ok, fast, NULL, &s);
            r = sg_fp_split_end(ir, &s, sg_fp_call_minmax(ir, fpcr, n, m, width, minimum, numeric));
        } else
            r = sg_fp_call_minmax(ir, fpcr, n, m, width, minimum, numeric);
    }
    return sg_d_from(g, sg_fp_when_end(g, &w, r, ir_int_type(ir, width)));
}
static inline SgD sg_FPMax(SgCtx* g, LLVMValueRef path, SgD a, SgD b, SgD fpcr) {
    return sg_fp_minmax(g, path, a, b, fpcr, 0, 0);
}
static inline SgD sg_FPMin(SgCtx* g, LLVMValueRef path, SgD a, SgD b, SgD fpcr) {
    return sg_fp_minmax(g, path, a, b, fpcr, 1, 0);
}
static inline SgD sg_FPMaxNum(SgCtx* g, LLVMValueRef path, SgD a, SgD b, SgD fpcr) {
    return sg_fp_minmax(g, path, a, b, fpcr, 0, 1);
}
static inline SgD sg_FPMinNum(SgCtx* g, LLVMValueRef path, SgD a, SgD b, SgD fpcr) {
    return sg_fp_minmax(g, path, a, b, fpcr, 1, 1);
}

/* FPMulAdd(addend, op1, op2): addend + op1 * op2 fused. The Sail negates an operand first (FPNeg) where the
   instruction says, so a NaN among them comes out with its sign flipped */
static inline SgD sg_FPMulAdd(SgCtx* g, LLVMValueRef path, SgD addend, SgD a, SgD b, SgD fpcr_bits) {
    SnxIr* ir = g->ir;
    unsigned width = a.w;
    LLVMValueRef add = sg_d_make(g, &addend), n = sg_d_make(g, &a), m = sg_d_make(g, &b);
    LLVMValueRef fpcr = sg_fp_fpcr(g, fpcr_bits), r = NULL;
    SgWhen w;
    if (sg_fp_when(g, path, &w)) {
        if (width != 16 && host_fma_scalar(ir)) {
            SgFpResidual residual;
            SgFpSplit s;
            LLVMValueRef ok, fast = sg_fp_fast_fma(g, fpcr, width, add, n, m, &ok, &residual);
            sg_fp_relax(g, fpcr, width, fast, &ok, NULL);
            sg_fp_split_fma(ir, ok, fast, NULL, ir->promote && snx_ir_fpsr_exact() ? &residual : NULL, &s);
            r = sg_fp_split_end(ir, &s, sg_fp_call_fma(ir, fpcr, add, n, m, width));
        } else
            r = sg_fp_call_fma(ir, fpcr, add, n, m, width);
    }
    return sg_d_from(g, sg_fp_when_end(g, &w, r, ir_int_type(ir, width)));
}

static inline SgD sg_FPSqrt(SgCtx* g, LLVMValueRef path, SgD a, SgD fpcr_bits) {
    SnxIr* ir = g->ir;
    unsigned width = a.w;
    LLVMValueRef n = sg_d_make(g, &a), fpcr = sg_fp_fpcr(g, fpcr_bits), r = NULL;
    SgWhen w;
    if (sg_fp_when(g, path, &w)) {
        if (width != 16 && host_sqrt(ir)) {
            LLVMValueRef ok, inexact, fast = sg_fp_fast_sqrt(g, fpcr, width, n, &ok, &inexact);
            SgFpSplit s;
            sg_fp_relax(g, fpcr, width, fast, &ok, &inexact);
            sg_fp_split(ir, ok, fast, inexact, &s);
            r = sg_fp_split_end(ir, &s, sg_fp_call_unary(ir, fpcr, n, width, SNX_FP_SQRT));
        } else
            r = sg_fp_call_unary(ir, fpcr, n, width, SNX_FP_SQRT);
    }
    return sg_d_from(g, sg_fp_when_end(g, &w, r, ir_int_type(ir, width)));
}

/* FPRoundingMode(fpcr): the mode FPCR holds when the guest runs, which the functions below read from their FPCR */
static inline int sg_FPRoundingMode(SgCtx* g, LLVMValueRef path, SgD fpcr) {
    (void)g;
    (void)path;
    (void)fpcr;
    return SG_FP_ROUNDING_FPCR;
}
/* a rounding the Sail hands a lowered function: 0-4 (FPRounding's TIEEVEN to TIEAWAY), or -1 for FPCR's mode */
static inline int sg_fp_rounding(int rounding) {
    if (rounding == SG_FP_ROUNDING_FPCR) return -1;
    if (rounding < 0 || rounding > 4) sg_fail("a floating-point rounding the helpers do not take", rounding, 4);
    return rounding;
}

/* FPRoundInt: FRINTN, P, M, Z, A (the encoding's mode), X and I (FPCR's; exact, FRINTX's IXC). The host's rounding
   (host_rint) gives the modes exactly for all but a NaN or a flushed denormal, and raises nothing; FRINTX's IXC is a
   value rounding changed */
static inline SgD sg_FPRoundInt(SgCtx* g, LLVMValueRef path, SgD a, SgD fpcr_bits, int rounding, int exact) {
    SnxIr* ir = g->ir;
    unsigned width = a.w;
    int rm = sg_fp_rounding(rounding);
    LLVMValueRef n = sg_d_make(g, &a), fpcr = sg_fp_fpcr(g, fpcr_bits), r = NULL;
    SgWhen w;
    if (sg_fp_when(g, path, &w)) {
        LLVMValueRef mode = rm < 0 ? sg_fp_rmode(g, fpcr) : ir_i32(ir, (SnxU32)rm);
        if (width != 16 && (rm < 0 ? host_round_dynamic(ir) : host_round_mode(ir, (unsigned)rm))) {
            LLVMValueRef value = sg_fp_as_float(ir, n, width), ok;
            LLVMValueRef rounded =
                rm < 0 ? ir_fp_round_value_in(ir, value, 5, sg_fp_rmode(g, fpcr)) : host_rint(ir, value, (unsigned)rm);
            SgFpSplit s;
            ok = both(ir, LLVMBuildNot(SG_B, sg_fp_is_nan(ir, width, n), ""), sg_fp_unflushed(g, fpcr, width, n));
            sg_fp_split(ir, ok, sg_fp_as_bits(ir, rounded, width),
                        exact && snx_ir_fpsr_exact() ? float_ne(ir, value, rounded) : NULL, &s);
            r = sg_fp_split_end(ir, &s, sg_fp_call_round(ir, fpcr, n, width, mode, (unsigned)exact));
        } else
            r = sg_fp_call_round(ir, fpcr, n, width, mode, (unsigned)exact);
    }
    return sg_d_from(g, sg_fp_when_end(g, &w, r, ir_int_type(ir, width)));
}

/* FPRoundIntN: FRINT32Z, FRINT32X, FRINT64Z, FRINT64X */
static inline SgD sg_FPRoundIntN(SgCtx* g, LLVMValueRef path, SgD a, SgD fpcr_bits, int rounding, long long intsize) {
    SnxIr* ir = g->ir;
    unsigned width = a.w;
    int rm = sg_fp_rounding(rounding);
    LLVMValueRef n = sg_d_make(g, &a), fpcr = sg_fp_fpcr(g, fpcr_bits), r = NULL;
    SgWhen w;
    if (sg_fp_when(g, path, &w)) {
        LLVMValueRef mode = rm < 0 ? sg_fp_rmode(g, fpcr) : ir_i32(ir, (SnxU32)rm);
        r = sg_fp_call_round_int(ir, fpcr, n, width, mode, (unsigned)intsize);
    }
    return sg_d_from(g, sg_fp_when_end(g, &w, r, ir_int_type(ir, width)));
}

/* FPConvert: FCVT between precisions, rounded per FPCR. A single widened to a double is exact unless it is a NaN or
   FPCR.FZ flushes it; a double narrowed in round to nearest is the host's when it isn't a NaN and the single is
   finite and above the smallest normal, or the double was a zero */
static inline SgD sg_FPConvert__1(SgCtx* g, LLVMValueRef path, SgD op, SgD fpcr_bits, long long to_bits) {
    SnxIr* ir = g->ir;
    unsigned from = op.w, to = (unsigned)to_bits;
    LLVMValueRef n = sg_d_make(g, &op), fpcr = sg_fp_fpcr(g, fpcr_bits), r = NULL;
    SgWhen w;
    if (to != 16 && to != 32 && to != 64) sg_fail("a conversion to a float of this width", to_bits, 0);
    if (sg_fp_when(g, path, &w)) {
        SgFpSplit s;
        if (from == 16 && (to == 32 || to == 64) && ir->features->x86 && ir->features->f16c) {
            /* Half inputs to conversions are not flushed by FZ16. AHP and NaNs retain the exact helper. */
            LLVMValueRef ok = both(ir, sg_fp_clear(g, fpcr, 1u << 26), LLVMBuildNot(SG_B, sg_fp_is_nan(ir, 16, n), ""));
            LLVMValueRef value = LLVMBuildFPExt(SG_B, sg_fp_as_float(ir, n, 16), sg_fp_type(ir, to), "");
            sg_fp_split(ir, ok, sg_fp_as_bits(ir, value, to), NULL, &s);
            r = sg_fp_split_end(ir, &s, sg_fp_call_convert(ir, fpcr, n, from, to, sg_fp_rmode(g, fpcr)));
        } else if (from == 32 && to == 16 && ir->features->x86 && ir->features->f16c) {
            LLVMValueRef value = sg_fp_as_float(ir, n, 32),
                         narrow = LLVMBuildFPTrunc(SG_B, value, sg_fp_type(ir, 16), "");
            LLVMValueRef bits = sg_fp_as_bits(ir, narrow, 16);
            LLVMValueRef ok = both(ir, sg_fp_clear(g, fpcr, FPCR_RMODE | (1u << 26)),
                                   both(ir, sg_fp_unflushed(g, fpcr, 32, n),
                                        either(ir, sg_fp_roomy(ir, 16, bits), sg_fp_is_zero(ir, 32, n))));
            sg_fp_split(ir, ok, bits, float_ne(ir, value, LLVMBuildFPExt(SG_B, narrow, sg_fp_type(ir, 32), "")), &s);
            r = sg_fp_split_end(ir, &s, sg_fp_call_convert(ir, fpcr, n, from, to, sg_fp_rmode(g, fpcr)));
        } else if (from == 32 && to == 64) {
            LLVMValueRef ok =
                both(ir, LLVMBuildNot(SG_B, sg_fp_is_nan(ir, 32, n), ""), sg_fp_unflushed(g, fpcr, 32, n));
            LLVMValueRef fast = sg_fp_as_bits(ir, sg_fp_widen(ir, sg_fp_as_float(ir, n, 32)), 64);
            sg_fp_split(ir, ok, fast, NULL, &s);
            r = sg_fp_split_end(ir, &s, sg_fp_call_convert(ir, fpcr, n, from, to, sg_fp_rmode(g, fpcr)));
        } else if (from == 64 && to == 32) {
            LLVMValueRef fn = sg_fp_as_float(ir, n, 64), narrow = LLVMBuildFPTrunc(SG_B, fn, sg_fp_type(ir, 32), "");
            LLVMValueRef fast = sg_fp_as_bits(ir, narrow, 32), ok;
            ok = both(ir, both(ir, sg_fp_clear(g, fpcr, FPCR_RMODE), LLVMBuildNot(SG_B, sg_fp_is_nan(ir, 64, n), "")),
                      either(ir, sg_fp_roomy(ir, 32, fast), sg_fp_is_zero(ir, 64, n)));
            sg_fp_split(ir, ok, fast, float_ne(ir, sg_fp_widen(ir, narrow), fn), &s);
            r = sg_fp_split_end(ir, &s, sg_fp_call_convert(ir, fpcr, n, from, to, sg_fp_rmode(g, fpcr)));
        } else
            r = sg_fp_call_convert(ir, fpcr, n, from, to, sg_fp_rmode(g, fpcr));
    }
    return sg_d_from(g, sg_fp_when_end(g, &w, r, ir_int_type(ir, to)));
}

/* FPCompare: the NZCV nibble of FCMP or FCMPE (signal), bits(4). Without a NaN, and without a denormal FPCR.FZ
   flushes, it raises nothing and orders as the host does: N for less, ZC for equal, C for greater */
static inline SgD sg_FPCompare(SgCtx* g, LLVMValueRef path, SgD a, SgD b, int signal, SgD fpcr_bits) {
    SnxIr* ir = g->ir;
    unsigned width = a.w;
    LLVMValueRef n = sg_d_make(g, &a), m = sg_d_make(g, &b), fpcr = sg_fp_fpcr(g, fpcr_bits), r = NULL;
    SgWhen w;
    if (sg_fp_when(g, path, &w)) {
        if (width == 16)
            r = sg_fp_call_compare(ir, fpcr, n, m, width, (unsigned)signal);
        else {
            LLVMValueRef fa = sg_fp_as_float(ir, n, width), fb = sg_fp_as_float(ir, m, width), ok, fast;
            SgFpSplit s;
            ok = both(ir, sg_fp_unflushed(g, fpcr, width, n), sg_fp_unflushed(g, fpcr, width, m));
            ok = both(ir, LLVMBuildFCmp(SG_B, LLVMRealORD, fa, fb, ""), ok);
            fast =
                LLVMBuildSelect(SG_B, LLVMBuildFCmp(SG_B, LLVMRealOEQ, fa, fb, ""), ir_i32(ir, 6), ir_i32(ir, 2), "");
            fast = LLVMBuildSelect(SG_B, LLVMBuildFCmp(SG_B, LLVMRealOLT, fa, fb, ""), ir_i32(ir, 8), fast, "");
            sg_fp_split(ir, ok, fast, NULL, &s);
            r = sg_fp_split_end(ir, &s, sg_fp_call_compare(ir, fpcr, n, m, width, (unsigned)signal));
        }
    }
    { /* each flag a 0-or-1 source, as ir_flags_set_nzcv takes them apart, where the compare ran: under a run-time
         condition a phi each, which the select of FCCMP's flags makes one phi of what ran and its nzcv */
        SgD nzcv = sg_d_empty(4);
        LLVMValueRef flag[4];
        unsigned k;
        for (k = 4; k-- > 0;) {
            LLVMValueRef bit;
            if (w.never) {
                flag[k] = ir_i32(ir, 0);
                continue;
            }
            bit = k ? LLVMBuildLShr(SG_B, r, ir_i32(ir, k), "") : r;
            flag[k] = LLVMBuildAnd(SG_B, bit, ir_i32(ir, 1), "");
        }
        if (w.join) {
            LLVMBasicBlockRef blocks[2];
            LLVMValueRef values[2];
            blocks[0] = sg_fp_when_join(g, &w);
            blocks[1] = w.from;
            values[1] = ir_i32(ir, 0);
            for (k = 4; k-- > 0;) {
                values[0] = flag[k];
                flag[k] = LLVMBuildPhi(SG_B, ir->t_i32, "");
                LLVMAddIncoming(flag[k], values, blocks, 2);
            }
        }
        for (k = 0; k < 4; ++k) sg_d_put_any(g, &nzcv, k, sg_d_flag(flag[k], NULL), 0, 1);
        return nzcv;
    }
}

/* FixedToFP: SCVTF and UCVTF from Wn or Xn, fixed point with fbits fraction bits, in FPCR's mode. Exact on the host
   when the integer's magnitude fits the significand, whatever the mode (the scaling by a power of two is exact: the
   result is at least 2^-64); otherwise once IXC is set, in round to nearest */
static inline SgD sg_FixedToFP(SgCtx* g, LLVMValueRef path, SgD op, long long fbits_in, int uns, SgD fpcr_bits,
                               int rounding, long long width_in) {
    SnxIr* ir = g->ir;
    unsigned width = (unsigned)width_in, integer = op.w, fbits = (unsigned)fbits_in;
    unsigned precision = width == 32 ? 24 : 53;
    LLVMValueRef v = sg_d_make(g, &op), fpcr = sg_fp_fpcr(g, fpcr_bits), r = NULL;
    SgWhen w;
    if (sg_fp_rounding(rounding) >= 0)
        sg_fail("FixedToFP in a mode of its own: the helper rounds as FPCR says", rounding, 0);
    if (sg_fp_when(g, path, &w)) {
        if (width != 16) {
            LLVMTypeRef type = sg_fp_type(ir, width);
            LLVMValueRef f, ok, fast;
            SgFpSplit s;
            if (integer <= precision)
                ok = LLVMConstInt(ir->t_i1, 1, 0);
            else if (uns)
                ok = LLVMBuildICmp(SG_B, LLVMIntULT, v, sg_fp_kint(ir, integer, (SnxU64)1 << precision), "");
            else
                ok = LLVMBuildICmp(SG_B, LLVMIntULT,
                                   LLVMBuildAdd(SG_B, v, sg_fp_kint(ir, integer, (SnxU64)1 << precision), ""),
                                   sg_fp_kint(ir, integer, (SnxU64)1 << (precision + 1)), "");
            /* an unsigned value that fits converts as a signed one, unless the host converts unsigned values itself:
               LLVM's sum of two exact halves on x86 before AVX-512 is -0 for 0 when the host rounds toward minus
               infinity */
            if (uns && integer > precision && !host_unsigned_to_float(ir, integer, 0))
                f = LLVMBuildSelect(SG_B, ok, LLVMBuildSIToFP(SG_B, v, type, ""), LLVMBuildUIToFP(SG_B, v, type, ""),
                                    "");
            else
                f = uns ? LLVMBuildUIToFP(SG_B, v, type, "") : LLVMBuildSIToFP(SG_B, v, type, "");
            if (fbits)
                f = LLVMBuildFMul(SG_B, f, sg_fp_kfloat(ir, width, 1.0 / (double)((SnxU64)1 << (fbits - 1)) / 2.0), "");
            fast = sg_fp_as_bits(ir, f, width);
            ok = either(ir, ok, both(ir, sg_fp_clear(g, fpcr, FPCR_RMODE), ixc_set(ir)));
            sg_fp_split(ir, ok, fast, NULL, &s);
            r = sg_fp_split_end(ir, &s, sg_fp_call_from_int(ir, fpcr, v, integer, width, (unsigned)uns, fbits));
        } else
            r = sg_fp_call_from_int(ir, fpcr, v, integer, width, (unsigned)uns, fbits);
    }
    return sg_d_from(g, sg_fp_when_end(g, &w, r, ir_int_type(ir, width)));
}

/* FPToFixed: FCVT[NPMZA][SU] to Wd or Xd, fixed point with fbits fraction bits, in the encoding's mode. On the host
   when the scaled value (exact: a product by a power of two that overflows only out of range) rounds to an integer in
   range and no denormal is flushed; IXC when rounding changed it. Toward zero is the conversion's own rounding, the
   other modes need the host's rounding (host_rint) */
static inline SgD sg_FPToFixed(SgCtx* g, LLVMValueRef path, SgD op, long long fbits_in, int uns, SgD fpcr_bits,
                               int rounding, long long integer_in) {
    SnxIr* ir = g->ir;
    unsigned width = op.w, integer = (unsigned)integer_in, fbits = (unsigned)fbits_in;
    unsigned precision = width == 32 ? 24 : 53;
    int rm_in = sg_fp_rounding(rounding);
    unsigned rm = (unsigned)rm_in;
    LLVMValueRef n = sg_d_make(g, &op), fpcr = sg_fp_fpcr(g, fpcr_bits), r = NULL;
    SgWhen w;
    if (rm_in < 0) sg_fail("FPToFixed in FPCR's mode: the helper takes the encoding's", rounding, 0);
    if (integer != 32 && integer != 64) sg_fail("a conversion to an integer of this width", integer_in, 0);
    if (sg_fp_when(g, path, &w)) {
        if (width != 16 && (rm == 3 || host_round_mode(ir, rm))) {
            LLVMTypeRef type = ir_int_type(ir, integer);
            LLVMValueRef f = sg_fp_as_float(ir, n, width), low, high, ok, fast, rounded, inexact;
            SgFpSplit s;
            double top = (double)((SnxU64)1 << (integer - 1)) * (uns ? 2.0 : 1.0);
            if (fbits)
                f = LLVMBuildFMul(SG_B, f, sg_fp_kfloat(ir, width, (double)((SnxU64)1 << (fbits - 1)) * 2.0), "");
            rounded = rm == 3 ? f : host_rint(ir, f, rm);
            /* the lowest integer in range less one, where the format holds it (trunc of anything above it is in
               range), else the lowest integer itself */
            if (uns)
                low = LLVMBuildFCmp(SG_B, LLVMRealOGT, rounded, sg_fp_kfloat(ir, width, -1.0), "");
            else if (integer < precision)
                low = LLVMBuildFCmp(SG_B, LLVMRealOGT, rounded, sg_fp_kfloat(ir, width, -top - 1.0), "");
            else
                low = LLVMBuildFCmp(SG_B, LLVMRealOGE, rounded, sg_fp_kfloat(ir, width, -top), "");
            high = LLVMBuildFCmp(SG_B, LLVMRealOLT, rounded, sg_fp_kfloat(ir, width, top), "");
            ok = both(ir, both(ir, low, high), sg_fp_unflushed(g, fpcr, width, n));
            fast = uns ? LLVMBuildFPToUI(SG_B, rounded, type, "") : LLVMBuildFPToSI(SG_B, rounded, type, "");
            if (rm == 3)
                rounded = uns ? LLVMBuildUIToFP(SG_B, fast, LLVMTypeOf(f), "")
                              : LLVMBuildSIToFP(SG_B, fast, LLVMTypeOf(f), "");
            inexact = float_ne(ir, rounded, f);
            sg_fp_split(ir, ok, ir_cast(ir, fast, ir->t_i64, 0), inexact, &s);
            r = sg_fp_split_end(ir, &s, sg_fp_call_to_int(ir, fpcr, n, width, integer, (unsigned)uns, fbits, rm));
        } else
            r = sg_fp_call_to_int(ir, fpcr, n, width, integer, (unsigned)uns, fbits, rm);
    }
    r = sg_fp_when_end(g, &w, r, ir->t_i64);
    return sg_d_from(g, integer < 64 ? LLVMBuildTrunc(SG_B, r, ir_int_type(ir, integer), "") : r);
}

/* FPToFixedJS: FJCVTZS's Wd, and whether the conversion was exact, Z of the NZCV 0Z00 it sets */
static inline void sg_FPToFixedJS(SgCtx* g, LLVMValueRef path, SgD op, SgD fpcr_bits, int is64, long long n,
                                  SgD* result, SgD* z) {
    SnxIr* ir = g->ir;
    LLVMValueRef operand = sg_d_make(g, &op), fpcr = sg_fp_fpcr(g, fpcr_bits), r = NULL;
    SgD reply;
    SgWhen w;
    if (!is64 || n != 32 || op.w != 64) sg_fail("FPToFixedJS other than a double to 32 bits", n, op.w);
    if (sg_fp_when(g, path, &w)) r = ir_fp_reply_call_fpcr(ir, "snx_fp_jcvt", &operand, 1, 0, fpcr);
    reply = sg_d_from(g, sg_fp_when_end(g, &w, r, ir->t_i64));
    *result = sg_d_slice(g, reply, 31, 0);
    *z = sg_d_slice(g, reply, 32, 32);
}

/* FPConvertBF: BFCVT, a single to a BFloat16 (FEAT_BF16) in FPCR's mode */
static inline SgD sg_FPConvertBF__1(SgCtx* g, LLVMValueRef path, SgD op, SgD fpcr_bits) {
    SnxIr* ir = g->ir;
    LLVMValueRef operand = sg_d_make(g, &op), fpcr = sg_fp_fpcr(g, fpcr_bits), r = NULL;
    SgWhen w;
    if (sg_fp_when(g, path, &w))
        r = sg_fp_to_width(ir, ir_fp_reply_call_fpcr(ir, "snx_fp_bfcvt", &operand, 1, 0, fpcr), 16);
    return sg_d_from(g, sg_fp_when_end(g, &w, r, ir->t_i16));
}

/* ---- the entry ---- */

/* the dispatcher sailgen generates after this file (the decode functions' clauses) */
static int sg_decode(SgCtx* g);
/* the word's instruction emitted at pc, when it is one of the generated ones: 1, else 0. A branch's condition (an i1)
   and target (an i64) in *taken and *target when they are not NULL (sg_branch_to) */
static int sg_emit(SnxIr* ir, SnxU32 word, SnxU64 pc, LLVMValueRef* taken, LLVMValueRef* target) {
    SgCtx context, *g = &context;
    memset(g, 0, sizeof *g);
    g->ir = ir;
    g->word = word;
    g->pc = pc;
    g->start = LLVMGetLastInstruction(LLVMGetInsertBlock(ir->b));
    if (!sg_decode(g)) return 0;
    sg_accesses_combine(g);
    sg_flags_flush(g);
    sg_branch_result(g, taken, target);
    return 1;
}
