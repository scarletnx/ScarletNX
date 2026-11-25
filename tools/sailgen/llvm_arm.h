/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's LLVM back end for AArch32, after core.h (the values and the builtins every machine's back end shares). The
   file sailgen writes from it is included by the AArch32 block compiler (src/common/cpu/arm/llvm_thumb.inc), whose
   builder (ArmIR) and helpers it builds on: the registers and CPSR where SnxArmCPU keeps them (arm.h), guest memory
   through readmem and writemem (flat, checked or hosted, and the ARM7TDMI's and the ARM946E-S's bus rules), and the
   exits through trap. A register's read is a load and its write a store of the CPU structure, as the emitters written
   by hand did: LLVM keeps them in registers across a block (a hosted block's cpu is noalias). What the block compiler
   decides when it compiles a block it answers as values the recompiler knows: the instruction set, ITSTATE and the
   condition of the instruction (CurrentCond), and the guest's architecture (ArchVersion) */

struct SgCtx {
    ArmIR* a;
    SnxU32 word; /* the instruction: a T16 halfword, or an A32 or T32 word */
    SnxU64 pc;
    unsigned itstate; /* ITSTATE before the instruction (llvm_thumb.inc's), 0 outside an IT block */
    int it_set;       /* IT set ITSTATE for the instructions after it: it_next */
    unsigned it_next;
    /* PSTATE's fields written and not stored yet, an i32 0 or 1 each (NULL: not written), stored together in one write
       of CPSR as the instruction ends (sg_flags_flush), as the emitters written by hand wrote them (llvm.c's nz) */
    LLVMValueRef field[32];
    /* the fields as the instruction read them from CPSR: one written back with its own value is not written (MVN's C,
       Shift_C's carry_in for a shift of 0), as the emitters written by hand left it */
    LLVMValueRef read[32];
    /* the registers and CPSR as the instruction read or wrote them: each is loaded once an instruction, as the emitters
       written by hand read an operand once (a guest store never reaches the CPU structure: llvm.h's contract) */
    LLVMValueRef reg[15], cpsr;
    /* the condition the rest of the instruction runs under (sg_when): the block it skips to where it fails */
    LLVMBasicBlockRef skip;
    int branched, undefined;
    /* the block's last instruction before this one's: a clause that hands the word on (sg_see) has emitted nothing */
    LLVMValueRef start;
    LLVMBasicBlockRef start_block;
    /* a multiple transfer's range, checked whole before its first access (GuestTransfer): its accesses are within it,
       and a store's is committed before anything after it is written */
    int group, group_write, group_committed;
    LLVMValueRef group_start;
    /* the registers a multiple load wrote, stored as the instruction ends (sg_registers_flush) */
    unsigned deferred;
    /* an A32 word whose skip the block's trace records (llvm.c's timed_condition, SnxArmCodegenOptions.skipped) */
    int timed;
    int iset; /* the instruction set it selected (SelectInstrSet), 0 for the block's */
};

static inline LLVMBuilderRef sg_builder(SgCtx* g) { return g->a->b; }
static inline LLVMContextRef sg_llvm_context(SgCtx* g) { return g->a->ctx; }
static inline LLVMValueRef sg_intrinsic(SgCtx* g, const char* name, LLVMTypeRef type, LLVMValueRef* args, unsigned n) {
    unsigned id = LLVMLookupIntrinsicID(name, strlen(name));
    LLVMValueRef function = LLVMGetIntrinsicDeclaration(g->a->mod, id, type ? &type : NULL, type ? 1 : 0);
    return LLVMBuildCall2(SG_B, LLVMIntrinsicGetType(g->a->ctx, id, type ? &type : NULL, type ? 1 : 0), function, args,
                          n, "");
}
/* no lowered function here runs under a run-time condition of its own (A64's FCCMP) */
static inline LLVMValueRef sg_select_phi(SgCtx* g, LLVMValueRef c, LLVMValueRef x, LLVMValueRef y) {
    (void)g;
    (void)c;
    (void)x;
    (void)y;
    return NULL;
}

/* ---- what the block compiler decides ---- */

/* the guest's architecture version, as the ARMv7 ARM's ASL asks it (arm.sail's ArchVersion): the block's profile's */
static inline long long sg_ArchVersion(SgCtx* g) {
    switch (g->a->profile) {
        case SNX_ARM_PROFILE_ARM7TDMI: return 4;
        case SNX_ARM_PROFILE_ARM946E: return 5;
        case SNX_ARM_PROFILE_ARM11: return 6;
        case SNX_ARM_PROFILE_A9: return 7;
        default: return 8;
    }
}
/* the instruction set the instruction runs in, InstrSet's member (InstrSet_A64, InstrSet_A32, InstrSet_T32): the
   block's, until it selects the other (SelectInstrSet: BLX's target is aligned as the set it goes to) */
static inline int sg_CurrentInstrSet(SgCtx* g) { return g->iset ? g->iset : g->a->thumb ? 2 : 1; }
static inline void sg_SetInstrSet(SgCtx* g, int iset) { g->iset = iset; }
/* ITSTATE before the instruction */
static inline SgS sg_ITSTATE(SgCtx* g) { return sg_s(g->itstate, 8); }
/* CurrentCond (the Arm ARM's shared AArch32.CurrentCond): an A32 word's condition, the T1 and T3 B<c>'s own, else the
   IT block's while one is open, else AL */
static inline SgS sg_AArch32_CurrentCond(SgCtx* g) {
    if (!g->a->thumb) return sg_s(g->word >> 28, 4);
    if (g->word < 0x10000 && (g->word & 0xF000) == 0xD000 && ((g->word >> 9) & 7) != 7) /* T1: 1101 cccc, not 111x */
        return sg_s(g->word >> 8, 4);
    if (g->word >= 0x10000 && (g->word & 0xF800D000u) == 0xF0008000u && ((g->word >> 23) & 7) != 7) /* T3 */
        return sg_s(g->word >> 22, 4);
    return sg_s(g->itstate & 15 ? g->itstate >> 4 : 14, 4);
}
/* ConditionHolds(cond) as the compare that set the flags: none is kept here (the flags are CPSR's, loaded where they
   are read), so the Sail's ConditionHolds reads them */
static inline LLVMValueRef sg_condition_origin(SgCtx* g, SgS cond) {
    (void)g;
    (void)cond;
    return NULL;
}
/* IT's new ITSTATE, which the block compiler takes for the instructions after it */
static inline void sg_ITSTATE_set(SgCtx* g, SgD v) {
    LLVMValueRef c = sg_d_make(g, &v);
    if (!LLVMIsAConstantInt(c)) sg_fail("an ITSTATE known only at run time", (long long)g->word, 0);
    g->it_set = 1;
    g->it_next = (unsigned)LLVMConstIntGetZExtValue(c);
}

/* ---- the flow ---- */

/* the rest of the instruction runs where c holds (sailgen's decode body: `if ConditionPassed() then ...`): a branch
   past it to skip, which the instruction's end joins (sg_arm_finish), as the block compilers ran a conditional
   instruction */
static inline void sg_when(SgCtx* g, LLVMValueRef c) {
    LLVMBasicBlockRef execute;
    if (LLVMIsAConstantInt(c) && LLVMConstIntGetZExtValue(c)) return;
    if (g->skip) sg_fail("an instruction under two conditions", (long long)g->word, 0);
    execute = block(g->a, "execute");
    g->skip = block(g->a, "skip");
    LLVMBuildCondBr(SG_B, c, execute, g->skip);
    LLVMPositionBuilderAtEnd(SG_B, execute);
}
static inline void sg_flags_flush(SgCtx* g);
static inline void sg_transfer_commit(SgCtx* g);
/* BranchTo(target, type, conditional): R15 the target, where the instruction ends the block; under a run-time
   condition (CBZ's), the target where it holds and the next instruction where it does not */
static inline void sg_branch_to(SgCtx* g, SgD target, LLVMValueRef when) {
    LLVMValueRef t;
    if (g->branched) sg_fail("an instruction that branches twice", (long long)g->word, 0);
    if (target.w != 32) sg_fail("a branch to a target of other than 32 bits", target.w, 32);
    sg_transfer_commit(g);
    g->branched = 1;
    t = sg_d_make(g, &target);
    if (when) t = LLVMBuildSelect(SG_B, when, t, c32(g->a, g->a->next), "");
    /* an A32 branch's CPSR before R15, as llvm.c's branch_exchange writes them: LLVM then drops a store of CPSR's own
       value (an ALU write of the PC whose bit 0 the condition fixes) */
    if (!g->a->thumb) sg_flags_flush(g);
    setreg(g->a, 15, t);
}
/* UNDEFINED: the backend's to run or stop at (llvm.c's trap) */
static inline void sg_undefined(SgCtx* g) {
    trap(g->a, SNX_ARM_UNDEFINED, c32(g->a, g->a->word));
    g->undefined = 1;
}
/* a decode function that throws Error_See hands the word on to the next clause that matches it: 0. Only its fields
   were read, and its condition where it has one (ADD (register) T2's, which hands SP's words to ADD (SP plus
   register) inside its ConditionPassed): that condition and its branch are taken back, so the block holds nothing of
   the clause, and the next one runs under its own */
static inline int sg_see(SgCtx* g) {
    if (g->skip) {
        LLVMBasicBlockRef execute = LLVMGetInsertBlock(SG_B);
        LLVMValueRef i, before;
        LLVMValueRef branch = LLVMGetBasicBlockTerminator(g->start_block);
        if (LLVMGetFirstInstruction(execute) || LLVMGetFirstInstruction(g->skip) || g->branched || g->undefined ||
            !branch || LLVMGetNumSuccessors(branch) != 2 || LLVMGetSuccessor(branch, 0) != execute)
            sg_fail("a word handed on to the next clause after its own emitted IR", (long long)g->word, 0);
        LLVMDeleteBasicBlock(execute);
        LLVMDeleteBasicBlock(g->skip);
        g->skip = NULL;
        for (i = LLVMGetLastInstruction(g->start_block); i && i != g->start; i = before) {
            before = LLVMGetPreviousInstruction(i);
            LLVMInstructionEraseFromParent(i);
        }
        LLVMPositionBuilderAtEnd(SG_B, g->start_block);
        memset(g->reg, 0, sizeof g->reg); /* what it loaded went with it */
        memset(g->read, 0, sizeof g->read);
        g->cpsr = NULL;
    }
    if (LLVMGetInsertBlock(SG_B) != g->start_block || LLVMGetLastInstruction(g->start_block) != g->start ||
        g->branched || g->undefined)
        sg_fail("a word handed on to the next clause after its own emitted IR", (long long)g->word, 0);
    return 0;
}
/* SVC and BKPT: R15 past the instruction, the ITSTATE after it left in CPSR inside an IT block (llvm_thumb.inc's
   it_leave), and the exit with the immediate as its syndrome */
static inline void sg_exception(SgCtx* g, SnxArmExit code, SgD imm) {
    LLVMValueRef v = sg_d_make(g, &imm);
    sg_flags_flush(g);
    setreg(g->a, 15, c32(g->a, g->a->next));
    if (g->a->it_next) it_leave(g->a, g->a->it_next);
    trap(g->a, code, LLVMBuildZExt(SG_B, v, g->a->i32, ""));
}
/* the ARM7TDMI's MULS's C: the runtime library's, from the operands before the result lands (llvm.c's multiply) */
static inline void sg_MultiplyCarry(SgCtx* g) {
    LLVMValueRef args[2] = {c32(g->a, g->a->word), c32(g->a, (uint32_t)g->a->thumb)};
    rt_call(g->a, "snx_arm_multiply_carry", args, 2);
    g->cpsr = NULL; /* its C */
    memset(g->read, 0, sizeof g->read);
}
/* the multiplier, in the instruction's trace slot when the block keeps a trace (codegen.h's SnxArmCodegenOptions) */
static inline void sg_TraceMultiplier(SgCtx* g, SgD multiplier) {
    if (g->a->trace) trace_value(g->a, sg_d_make(g, &multiplier));
}
static inline void sg_AArch32_CallSupervisor(SgCtx* g, SgD imm) { sg_exception(g, SNX_ARM_SVC, imm); }
static inline void sg_AArch32_SoftwareBreakpoint(SgCtx* g, SgD imm) { sg_exception(g, SNX_ARM_BREAKPOINT, imm); }

/* ---- the state ---- */

/* R0 to R14, where SnxArmCPU keeps them (R15 is the pc, which R_read makes) */
static inline SgD sg_GPR_read(SgCtx* g, long long n) {
    if (n < 0 || n > 14) sg_fail("a register past R14 read", n, 14);
    if (!g->reg[n]) g->reg[n] = get(g->a, offsetof(SnxArmCPU, r) + (size_t)n * 4, g->a->i32);
    return sg_d_from(g, g->reg[n]);
}
static inline void sg_GPR_write(SgCtx* g, long long n, SgD v) {
    if (n < 0 || n > 14) sg_fail("a register past R14 written", n, 14);
    if (v.w != 32) sg_fail("a register written from a value of another width", n, v.w);
    sg_transfer_commit(g);
    g->reg[n] = sg_d_make(g, &v);
    if (g->group && !g->group_write) { /* a multiple load's: every word read before a register is written, as the */
        g->deferred |= 1u << n;        /* block compilers read them, so LLVM may gather the reads */
        return;
    }
    setreg(g->a, (unsigned)n, g->reg[n]);
}
/* the registers a multiple load wrote, in their order */
static inline void sg_registers_flush(SgCtx* g) {
    unsigned n;
    for (n = 0; n < 15; ++n)
        if (g->deferred & (1u << n)) setreg(g->a, n, g->reg[n]);
    g->deferred = 0;
}
/* CPSR as the instruction found it */
static inline LLVMValueRef sg_cpsr(SgCtx* g) {
    if (!g->cpsr) g->cpsr = GET(g->a, cpsr);
    return g->cpsr;
}

/* AddWithCarry(x, y, cin): llvm.c's arithmetic, x + y + cin in 64 bits, the result its low word; and its NZCV, made
   when it is written to the flags (sg_nzcv_flags): the carry the sum's bit 32, the overflow the operands' and the
   result's signs. A subtraction is x + NOT(y) + 1, the NOT the Sail's */
static inline LLVMValueRef sg_arm_wide(SgCtx* g, LLVMValueRef x, LLVMValueRef y, LLVMValueRef cin) {
    ArmIR* a = g->a;
    return add(a, add(a, z64(a, x), z64(a, y)), z64(a, cin));
}
/* v is NOT of a value (sg_d_not's xor with all ones) */
static inline int sg_arm_is_not(LLVMValueRef v) {
    LLVMValueRef k;
    if (!LLVMIsAInstruction(v) || LLVMGetInstructionOpcode(v) != LLVMXor) return 0;
    k = LLVMGetOperand(v, 1);
    return LLVMIsAConstantInt(k) && LLVMConstIntGetSExtValue(k) == -1;
}
static inline void sg_add_with_carry(SgCtx* g, SgD a, SgD b, SgD cin, SgD* result, SgD* nzcv) {
    LLVMValueRef x = sg_d_make(g, &a), y = sg_d_make(g, &b), c = sg_cast(g, sg_d_make(g, &cin), 32, 0), r;
    SgD flags = sg_d_empty(4);
    if (a.w != 32) sg_fail("AArch32's AddWithCarry of other than 32 bits", a.w, 32);
    if (sg_arm_is_not(x) && !sg_arm_is_not(y)) { /* RSB's and RSC's NOT(R[n]) + operand: the NOT second, as llvm.c's */
        LLVMValueRef t = x;                      /* reverse subtractions add it, so V is the same IR */
        x = y;
        y = t;
    }
    r = LLVMBuildTrunc(SG_B, sg_arm_wide(g, x, y, c), g->a->i32, "");
    *result = sg_d_from(g, r);
    flags.nzcv = SG_NZCV_CARRY;
    flags.nzcv_a = x;
    flags.nzcv_b = y;
    flags.nzcv_cin = c;
    flags.nzcv_r = r;
    *nzcv = flags;
}
static inline void sg_nzcv_flags(SgCtx* g, const SgD* f, LLVMValueRef out[4]) {
    ArmIR* a = g->a;
    LLVMValueRef r = f->nzcv_r;
    out[0] = lsr(a, r, 31);
    out[1] = z32(a, cmp(a, LLVMIntEQ, r, c32(a, 0)));
    if (f->nzcv == SG_NZCV_LOGIC) {
        out[2] = c32(a, 0);
        out[3] = c32(a, 0);
        return;
    }
    if (f->nzcv != SG_NZCV_CARRY) sg_fail("an NZCV this back end does not make", f->nzcv, SG_NZCV_CARRY);
    out[2] = band(a,
                  LLVMBuildTrunc(SG_B,
                                 LLVMBuildLShr(SG_B, sg_arm_wide(g, f->nzcv_a, f->nzcv_b, f->nzcv_cin), c64(a, 32), ""),
                                 a->i32, ""),
                  c32(a, 1));
    out[3] = lsr(a, band(a, LLVMBuildNot(SG_B, bxor(a, f->nzcv_a, f->nzcv_b), ""), bxor(a, f->nzcv_a, r)), 31);
}

/* PSTATE's fields this back end keeps, numbered as ProcState's, and their bits in CPSR */
enum { SG_PSTATE_N = 0, SG_PSTATE_Z = 1, SG_PSTATE_C = 2, SG_PSTATE_V = 3, SG_PSTATE_T = 29, SG_PSTATE_E = 30 };
static inline unsigned sg_pstate_bit(long long field) {
    switch (field) {
        case SG_PSTATE_N: return 31;
        case SG_PSTATE_Z: return 30;
        case SG_PSTATE_C: return 29;
        case SG_PSTATE_V: return 28;
        case SG_PSTATE_T: return 5;
        case SG_PSTATE_E: return 9;
        default: sg_fail("a field of PSTATE this back end does not keep", field, 0);
    }
}
static inline SgD sg_PSTATE_read(SgCtx* g, long long field) {
    unsigned bit = sg_pstate_bit(field);
    if (g->field[field]) return sg_d_flag(g->field[field], NULL);
    if (!g->read[field]) g->read[field] = band(g->a, lsr(g->a, sg_cpsr(g), bit), c32(g->a, 1));
    return sg_d_flag(g->read[field], NULL);
}
/* PSTATE's fields first to first + count - 1 = v, v's top bit the first: kept for sg_flags_flush. Four flags from
   AddWithCarry or a logical result are what llvm.c's arithmetic and nz make of them */
static inline void sg_PSTATE_write(SgCtx* g, long long first, long long count, SgD v) {
    unsigned i;
    if (first < 0 || count < 1 || first + count > 32)
        sg_fail("fields of PSTATE this back end does not keep", first, count);
    if (v.w != (unsigned)count) sg_fail("fields written from a value of another width", count, v.w);
    if (count == 4 && first == SG_PSTATE_N && v.nzcv) {
        LLVMValueRef f[4];
        sg_nzcv_flags(g, &v, f);
        for (i = 0; i < 4; ++i) g->field[SG_PSTATE_N + i] = f[i];
        return;
    }
    if (v.nzcv) sg_d_realize(g, &v);
    if (first == SG_PSTATE_T && count == 1 && !g->field[SG_PSTATE_T] && sg_d_is_const(&v) &&
        (sg_d_const_word(&v, 0) & 1) == (unsigned)g->a->thumb)
        return; /* the block's own instruction set (A32 BL's SelectInstrSet): CPSR keeps it */
    for (i = 0; i < (unsigned)count; ++i) {
        LLVMValueRef f = sg_cast(g, sg_d_flag_value(g, &v, (unsigned)count - 1 - i), 32, 0);
        (void)sg_pstate_bit(first + i);
        if (!g->field[first + i] && f == g->read[first + i]) continue; /* CPSR's own */
        g->field[first + i] = f;
    }
}
/* the fields written, in one write of CPSR, each ORed into it in turn, N first, as llvm.c's nz wrote them */
static inline void sg_flags_flush(SgCtx* g) {
    ArmIR* a = g->a;
    LLVMValueRef cpsr;
    uint32_t mask = 0;
    unsigned k;
    for (k = 0; k < 32; ++k)
        if (g->field[k]) mask |= 1u << sg_pstate_bit(k);
    if (!mask) return;
    cpsr = band(a, sg_cpsr(g), c32(a, ~mask));
    for (k = 0; k < 32; ++k) {
        if (!g->field[k]) continue;
        cpsr = bor(a, cpsr, shl(a, g->field[k], sg_pstate_bit(k)));
        g->field[k] = NULL;
    }
    PUT(a, cpsr, cpsr);
    g->cpsr = cpsr;
    memset(g->read, 0, sizeof g->read);
}

/* ---- guest memory ---- */

/* a multiple transfer's range, checked before its first access (arm.sail's GuestTransfer): llvm.c's transfer_begin of
   all its bytes, word aligned but on the ARM7TDMI and the ARM946E-S, whose buses take an unaligned base */
static inline void sg_GuestTransfer(SgCtx* g, SgD address, long long bytes, int write) {
    if (g->group) sg_fail("a second multiple transfer in one instruction", (long long)g->word, 0);
    g->group_start = sg_d_make(g, &address);
    transfer_begin(g->a, g->group_start, (unsigned)bytes, legacy(g->a) ? 1 : 4, write, 1);
    g->group = 1;
    g->group_write = write;
}
/* an address of a multiple transfer, the Sail's running address + 4 after each word: the range's start plus the
   offset, as the block compilers wrote each word's address, so LLVM finds the same arithmetic; else addr itself */
static inline LLVMValueRef sg_group_address(SgCtx* g, LLVMValueRef addr) {
    LLVMValueRef v = addr;
    unsigned long long offset = 0;
    while (v != g->group_start && LLVMIsAInstruction(v) && LLVMGetInstructionOpcode(v) == LLVMAdd &&
           LLVMIsAConstantInt(LLVMGetOperand(v, 1))) {
        offset += LLVMConstIntGetZExtValue(LLVMGetOperand(v, 1));
        v = LLVMGetOperand(v, 0);
    }
    return v == g->group_start ? add(g->a, g->group_start, c32(g->a, (uint32_t)offset)) : addr;
}
/* whether guest memory takes a transfer of no bytes (arm.sail's GuestTransferEmpty): flat and hosted memory check its
   address and move nothing; checked memory's bus has no transaction of no bytes */
static inline int sg_GuestTransferEmpty(SgCtx* g) { return g->a->memory != SNX_ARM_MEMORY_CHECKED; }
/* a multiple store's bytes go out before anything after them is written (checked memory's one transaction) */
static inline void sg_transfer_commit(SgCtx* g) {
    if (!g->group || !g->group_write || g->group_committed) return;
    transfer_commit(g->a);
    g->group_committed = 1;
}
/* one access of size bytes: a multiple transfer's within its range (readmem_list, its bus rules), else one checked by
   itself at any address (readmem) */
static inline SgD sg_GuestRead(SgCtx* g, SgD address, long long size, int aligned) {
    ArmIR* a = g->a;
    LLVMValueRef addr = sg_d_make(g, &address), v;
    (void)aligned;
    if (size != 1 && size != 2 && size != 4) sg_fail("a guest read of this size", size, 0);
    if (g->group)
        v = readmem_list(a, sg_group_address(g, addr), (unsigned)size);
    else {
        transfer_begin(a, addr, (unsigned)size, 1, 0, 0);
        v = readmem(a, addr, (unsigned)size, 0);
    }
    if (size < 4) {
        /* readmem's i32 holds a narrow load zero extended (the bus hook answers with the width's value, arm.h): the
           map's source, so ZeroExtend of it is that i32, as the emitters written by hand used it. The ARM7TDMI's
           halfword at an odd address is the aligned one rotated through all 32 bits: the map takes its low 16 */
        SgD d = sg_d_from(g, v);
        unsigned i;
        d.w = (unsigned)size * 8;
        if (size == 1 || !arm7tdmi(a)) d.src_w[0] = (unsigned char)d.w;
        for (i = d.w; i < 32; ++i) d.sel[i] = 0;
        d.value = NULL;
        return d;
    }
    return sg_d_from(g, v);
}
/* an ARMv4T halfword load's 32 bits as its bus gives them (arm.sail's GuestReadHalfwordV4): readmem's, which rotates
   the aligned halfword at an odd address, or reads the byte there sign extended for LDRSH */
static inline SgD sg_GuestReadHalfwordV4(SgCtx* g, SgD address, int sign) {
    LLVMValueRef addr = sg_d_make(g, &address);
    if (g->group) sg_fail("a halfword load in a multiple transfer", (long long)g->word, 0);
    transfer_begin(g->a, addr, 2, 1, 0, 0);
    return sg_d_from(g, readmem(g->a, addr, 2, sign));
}
/* the value a store of size bytes gives writemem: the 32-bit register it is the low bits of, as the emitters written by
   hand gave it (the bus hook sees the register), else the value widened */
static inline LLVMValueRef sg_store_value(SgCtx* g, SgD v) {
    unsigned i;
    int low = v.w < 32;
    for (i = 0; i < v.w && low; ++i)
        low = v.sel[i] >= 2 && v.sel[i] == v.sel[0] && v.pos[i] == i && v.src_w[v.sel[0] - 2] == 32;
    if (low) return v.src[v.sel[0] - 2];
    return sg_cast(g, sg_d_make(g, &v), 32, 0);
}
static inline void sg_GuestWrite(SgCtx* g, SgD address, long long size, int aligned, SgD value) {
    ArmIR* a = g->a;
    LLVMValueRef addr = sg_d_make(g, &address), v = sg_store_value(g, value);
    (void)aligned;
    if (size != 1 && size != 2 && size != 4) sg_fail("a guest write of this size", size, 0);
    if (g->group) {
        writemem_list(a, sg_group_address(g, addr), (unsigned)size, v);
        return;
    }
    transfer_begin(a, addr, (unsigned)size, 1, 1, 0);
    writemem(a, addr, (unsigned)size, v);
    transfer_commit(a);
}

/* ---- the entry ---- */

/* the T16 and A32 dispatchers sailgen generates after this file (a32.roots' decoders) */
static int sg_decode_t16(SgCtx* g);
static int sg_decode_a32(SgCtx* g);
/* the instruction's end: what it wrote stored, and the way where its condition failed joined, R15 the next
   instruction's where it ran on (llvm_thumb.inc's driver did both for the emitters written by hand). 1 when it ends the
   block: it branched, or stopped the block (an exit) */
static inline int sg_arm_finish(SgCtx* g) {
    ArmIR* a = g->a;
    int stop = g->branched;
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(SG_B))) {
        sg_registers_flush(g);
        sg_transfer_commit(g);
        sg_flags_flush(g);
    }
    if (g->skip) {
        LLVMBasicBlockRef next = block(a, "next");
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(SG_B))) {
            if (!stop) setreg(a, 15, c32(a, a->next));
            LLVMBuildBr(SG_B, next);
        }
        LLVMPositionBuilderAtEnd(SG_B, g->skip);
        setreg(a, 15, c32(a, a->next));
        if (g->timed && a->skipped) { /* the skipped instruction's bit, as llvm.c's A32 driver sets it */
            uint32_t i = (a->pc - a->start) / 4, at = a->skipped + (i >> 5) * 4;
            put(a, at, bor(a, get(a, at, a->i32), c32(a, 1u << (i & 31))));
        }
        LLVMBuildBr(SG_B, next);
        LLVMPositionBuilderAtEnd(SG_B, next);
        return stop;
    }
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(SG_B))) return 1; /* it stopped the block */
    if (!stop) setreg(a, 15, c32(a, a->next));
    return stop;
}
/* a T16 halfword at a->pc, with ITSTATE before it: emitted and *stop set (sg_arm_finish), 1; or 0 when the Sail has no
   clause for it, with nothing emitted. *it_next is ITSTATE for the next instruction when it is an IT */
static int sg_emit_t16(ArmIR* a, uint16_t halfword, unsigned itstate, int* stop, int* it_set, unsigned* it_next) {
    SgCtx context, *g = &context;
    memset(g, 0, sizeof *g);
    g->a = a;
    g->word = halfword;
    g->pc = a->pc;
    g->itstate = itstate;
    g->start_block = LLVMGetInsertBlock(a->b);
    g->start = LLVMGetLastInstruction(g->start_block);
    if (!sg_decode_t16(g)) return 0;
    *stop = sg_arm_finish(g);
    *it_set = g->it_set;
    *it_next = g->it_next;
    return 1;
}
/* an A32 word at a->pc: emitted and *stop set (sg_arm_finish), 1; or 0 when no clause of the Sail's takes it, with
   nothing emitted, for the block compiler's emitters. timed: its skip goes in the trace */
static int sg_emit_a32(ArmIR* a, uint32_t word, int timed, int* stop) {
    SgCtx context, *g = &context;
    memset(g, 0, sizeof *g);
    g->a = a;
    g->word = word;
    g->pc = a->pc;
    g->timed = timed;
    g->start_block = LLVMGetInsertBlock(a->b);
    g->start = LLVMGetLastInstruction(g->start_block);
    if (!sg_decode_a32(g)) return 0;
    *stop = sg_arm_finish(g);
    return 1;
}
