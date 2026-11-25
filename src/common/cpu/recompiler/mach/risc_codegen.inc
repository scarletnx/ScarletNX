/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The RISC back ends' guest functions, the glue every one shares (after mach/mach_codegen.inc, included by a
   machine's code generator: rv64/rv64_codegen.c...): the flow, the calls, the entry's checks and the dispatch block,
   written from risc.h's operations and the machine's branches, calls and frame (its header's rk_j, rk_bz, rk_patch,
   with MG_FAR_JUMPS rk_j_far and rk_patch_far, rk_call_reloc, rk_tail_reloc, rk_patch_call, rk_call_reg, rk_jump_reg,
   rk_literal, rk_literal_value, rk_fn_prologue, rk_fn_epilogue, rk_fn_patch_frame), and its relocations (MG_R_CALL,
   MG_R_TAIL, MG_R_LITERAL, and MG_R_FAR where a JIT unit's literals hold addresses: MIPS64's).

   The function's ABI is GuestFn's: the state in the first argument's register, kept in SGX_BASE, the entry in the
   second's (SGX_FN_TARGET). A jump goes as far as the machine's long jump (rk_j; rk_j_far in a function its reach
   is too short for, Unit.far); a conditional one is a short branch of the opposite over it. An address of data
   (snx_stale) is a literal in the code, its distance from itself (rk_literal, MG_R_LITERAL). RK_S1 and RK_S2 are
   the glue's own, RK_SA the machine's assembler's */

static void fixup(Unit* u, int label, size_t at) {
    u->fixups = (Fixup*)grow(u->fixups, &u->cap_fixups, u->nfixups + 1, sizeof *u->fixups);
    u->fixups[u->nfixups].at = at;
    u->fixups[u->nfixups].kind = 0;
    u->fixups[u->nfixups++].label = label;
}
static void reloc(Unit* u, size_t at, const char* sym, unsigned type, long long addend) {
    size_t n = strlen(sym);
#ifdef MG_R_FAR
    if (u->b.absolute && type == MG_R_LITERAL) type = MG_R_FAR;
#endif
    u->relocs = (Reloc*)grow(u->relocs, &u->cap_relocs, u->nrelocs + 1, sizeof *u->relocs);
    u->relocs[u->nrelocs].at = at;
    u->relocs[u->nrelocs].sym = (char*)malloc(n + 1);
    memcpy(u->relocs[u->nrelocs].sym, sym, n + 1);
    u->relocs[u->nrelocs].type = type;
    u->relocs[u->nrelocs++].addend = addend;
}
/* the helpers' calls the emitters make (risc_a64.h's rk_call_reloc) */
static void unit_reloc(void* machine, size_t at, const char* sym, long long addend) {
    reloc((Unit*)machine, at, sym, MG_R_CALL, addend);
}

static void jmp(Unit* u, int label) {
#ifdef MG_FAR_JUMPS
    if (u->far) {
        fixup(u, label, rk_j_far(&u->b));
        return;
    }
#endif
    fixup(u, label, rk_j(&u->b));
}
/* to label where r is (nonzero) or isn't 0: the short branch of the opposite over a long jump */
static void jump_reg(Unit* u, int nonzero, int r, int label) {
    size_t skip = rk_bz(&u->b, !nonzero, r);
    jmp(u, label);
    rk_patch(&u->b, skip, u->b.n);
}
static void jcc(Unit* u, int cc, int label) {
    risc_cond(&u->b, RK_S1, cc);
    jump_reg(u, 1, RK_S1, label);
}
static void state_ldst(Unit* u, int load, int bytes, int rt, size_t off) { rk_state(&u->b, load, bytes, rt, off); }
static void mov_state_arg(Unit* u) { rk_mv(&u->b, sgx_arg_regs[0], SGX_BASE); }
static void set_pc(Unit* u, SnxU64 pc) {
    rk_li(&u->b, RK_S1, pc);
    state_ldst(u, 0, 8, RK_S1, offsetof(CPU, pc));
}
/* a call of c->rt's function at off */
static void call_rt(Unit* u, int off) {
    state_ldst(u, 1, 8, RK_S1, offsetof(CPU, rt));
    rk_ldst(&u->b, 1, 8, 0, RK_S1, RK_S1, -1, off);
    rk_call_reg(&u->b, RK_S1);
}

/* the pins to the state (store) or from it; a call changes none of them (the callee's registers) */
static void pins_of(Unit* u, SgXBuf* b, int store, int caller) {
    int n;
    if (!u->pinned || caller) return;
    if (u->mempin >= 0 && !store) rk_state(b, 1, 8, u->mempin, offsetof(CPU, mem));
    for (n = 0; n < 32; ++n)
        if (u->pin[n] >= 0) rk_state(b, !store, 8, u->pin[n], offsetof(CPU, x) + 8 * (size_t)n);
}
static void prologue(Unit* u) {
    size_t at = rk_fn_prologue(&u->b, u->pinned);
    u->frame_at = (size_t*)grow(u->frame_at, &u->cap_frame, u->nframe + 1, sizeof *u->frame_at);
    u->frame_at[u->nframe++] = at;
    pins(u, 0);
}
static void epilogue(Unit* u) { rk_fn_epilogue(&u->b, u->pinned); }
static void leave(Unit* u) {
    pins(u, 1);
    epilogue(u);
    rk_ret(&u->b);
}

/* CALLDYN(SGX_FN_TARGET): the dispatch table's function for the target (CPU.disp, inside [code_lo, code_hi) and
   aligned), else the runtime's miss, called with the state and the target: snx_lookup's, inline. tail: the epilogue
   and a jump */
static void calldyn(Unit* u, int tail) {
    SgXBuf* b = &u->b;
    int miss = new_label(u), found = new_label(u);
    state_ldst(u, 1, 8, RK_S1, offsetof(CPU, code_lo));
    state_ldst(u, 1, 8, RK_S2, offsetof(CPU, code_hi));
    rk_sub(b, RK_S2, RK_S2, RK_S1, 1);          /* hi - lo */
    rk_sub(b, RK_S1, SGX_FN_TARGET, RK_S1, 1);  /* target - lo */
    rk_sltu(b, RK_S2, RK_S1, RK_S2);            /* inside */
    jump_reg(u, 0, RK_S2, miss);
    rk_andi(b, RK_S2, SGX_FN_TARGET, 3);
    jump_reg(u, 1, RK_S2, miss);
    rk_slli(b, RK_S1, RK_S1, 1, 1);             /* (target - lo) / 4 * 8 */
    state_ldst(u, 1, 8, RK_S2, offsetof(CPU, disp));
    rk_add(b, RK_S1, RK_S1, RK_S2, 1);
    rk_ldst(b, 1, 8, 0, RK_S1, RK_S1, -1, 0);
    jump_reg(u, 1, RK_S1, found);
    bind(u, miss);
    state_ldst(u, 1, 8, RK_S1, offsetof(CPU, rt));
    rk_ldst(b, 1, 8, 0, RK_S1, RK_S1, -1, (int)offsetof(RtAPI, miss));
    bind(u, found);
    pins(u, 1); /* the callee reads the guest's registers from the state */
    mov_state_arg(u);
    if (tail) {
        /* the target in a register the epilogue leaves alone: RK_S1 is the callee's where RK_TAIL says another */
        int to = RK_S1;
#ifdef RK_TAIL
        rk_mv(b, RK_TAIL, RK_S1);
        to = RK_TAIL;
#endif
        epilogue(u);
        rk_jump_reg(b, to);
    } else {
        rk_call_reg(b, RK_S1);
        pins(u, 0);
    }
}
static void set_target(Unit* u, SnxU64 target) { rk_li(&u->b, SGX_FN_TARGET, target); }
/* a call of sym (MG_R_CALL), or the epilogue and a jump (MG_R_TAIL): the pins stored first, the state in the first
   argument, the target in SGX_FN_TARGET */
static void call_sym(Unit* u, const char* sym, int tail) {
    pins(u, 1);
    mov_state_arg(u);
    if (tail) epilogue(u);
    reloc(u, tail ? rk_tail_reloc(&u->b) : rk_call_reloc(&u->b), sym, tail ? MG_R_TAIL : MG_R_CALL, 0);
    if (!tail) pins(u, 0);
}
/* RK_S1 = snx_stale[index], the module's */
static void stale_test(Unit* u, size_t index) {
    reloc(u, rk_literal(&u->b, RK_S1), "snx_stale", MG_R_LITERAL, (long long)index);
    rk_literal_value(&u->b, RK_S1, RK_S1, RK_SA);
    rk_ldst(&u->b, 1, 1, 0, RK_S1, RK_S1, -1, 0);
}
/* if (c->pc != next) return: the callee changed the code it returns to, or left for another pc */
static void check_return(Unit* u, SnxU64 next) {
    int stay = new_label(u), go = new_label(u);
    rk_li(&u->b, RK_S1, next);
    state_ldst(u, 1, 8, RK_S2, offsetof(CPU, pc));
    rk_xor(&u->b, RK_S1, RK_S1, RK_S2);
    jump_reg(u, 1, RK_S1, go);
    jmp(u, stay);
    bind(u, go);
    leave(u);
    bind(u, stay);
    if (u->options->stale) {
        stale_test(u, u->index);
        jump_reg(u, 1, RK_S1, go);
    }
}
/* the poll of a backward branch: if (c->poll) { c->pc = resume; c->rt->poll(c); F_start(c, resume); return; } */
static void poll_then(Unit* u, SnxU64 resume) {
    int run = new_label(u);
    char sym[40];
    state_ldst(u, 1, 4, RK_S1, offsetof(CPU, poll));
    jump_reg(u, 0, RK_S1, run);
    pins(u, 1);
    set_pc(u, resume);
    mov_state_arg(u);
    call_rt(u, (int)offsetof(RtAPI, poll));
    mov_state_arg(u);
    epilogue(u);
    rk_li(&u->b, SGX_FN_TARGET, resume);
    fname(sym, sizeof sym, "F_", u->f->start);
    reloc(u, rk_tail_reloc(&u->b), sym, MG_R_TAIL, 0);
    bind(u, run);
}
static void mg_undef(Unit* u, SnxU64 address, SnxU32 word) {
    pins(u, 1);
    set_pc(u, address);
    mov_state_arg(u);
    rk_li(&u->b, sgx_arg_regs[1], address);
    rk_li(&u->b, sgx_arg_regs[2], word);
    call_rt(u, (int)offsetof(RtAPI, undef));
}
static void mg_svc(Unit* u, SnxU64 next, unsigned n, int direct) {
    pins(u, 1);
    set_pc(u, next);
    mov_state_arg(u);
    rk_li(&u->b, sgx_arg_regs[1], n);
    if (direct) {
        state_ldst(u, 1, 8, RK_S1, offsetof(CPU, rt));
        rk_ldst(&u->b, 1, 8, 0, RK_S1, RK_S1, -1, (int)offsetof(RtAPI, svcs));
        rk_ldst(&u->b, 1, 8, 0, RK_S1, RK_S1, -1, (int)(8 * n));
        rk_call_reg(&u->b, RK_S1);
    } else
        call_rt(u, (int)offsetof(RtAPI, svc));
    pins(u, 0);
}
static void mg_brk(Unit* u, SnxU64 address, unsigned imm16) {
    pins(u, 1);
    set_pc(u, address);
    mov_state_arg(u);
    rk_li(&u->b, sgx_arg_regs[1], address);
    rk_li(&u->b, sgx_arg_regs[2], imm16);
    call_rt(u, (int)offsetof(RtAPI, brk));
}
static void mg_entry(Unit* u, size_t index, SnxU64 start) {
    if (u->options->stale) {
        int live = new_label(u);
        stale_test(u, index);
        jump_reg(u, 0, RK_S1, live);
        state_ldst(u, 0, 8, SGX_FN_TARGET, offsetof(CPU, pc));
        leave(u);
        bind(u, live);
    }
    rk_li(&u->b, RK_S1, start);
    rk_xor(&u->b, RK_S1, RK_S1, SGX_FN_TARGET);
    jump_reg(u, 1, RK_S1, u->mismatch_label);
}
static void mg_dispatch(Unit* u, int lab0) {
    const SnxFunction* f = u->f;
    size_t i;
    if (!u->dispatch) {
        calldyn(u, 1);
        return;
    }
    state_ldst(u, 0, 8, SGX_FN_TARGET, offsetof(CPU, pc));
    jmp(u, u->d_label);
    {
        int run = new_label(u);
        bind(u, u->d_label);
        state_ldst(u, 1, 4, RK_S1, offsetof(CPU, poll));
        jump_reg(u, 0, RK_S1, run);
        pins(u, 1);
        mov_state_arg(u);
        call_rt(u, (int)offsetof(RtAPI, poll));
        pins(u, 0);
        bind(u, run);
        state_ldst(u, 1, 8, RK_S2, offsetof(CPU, pc));
        for (i = 0; i < f->count; ++i)
            if (f->leaders[i]) {
                rk_li(&u->b, RK_S1, snx_function_address(f, i));
                rk_xor(&u->b, RK_S1, RK_S1, RK_S2);
                jump_reg(u, 0, RK_S1, lab0 + (int)i);
            }
        rk_mv(&u->b, SGX_FN_TARGET, RK_S2);
        calldyn(u, 1);
    }
}
static int mg_finish(Unit* u, size_t fix0) {
    size_t i;
    long long size = ((long long)u->a.slots_max * 8 + 15) & ~15ll;
    for (i = 0; i < u->nframe; ++i)
        if (!rk_fn_patch_frame(&u->b, u->frame_at[i], size)) return 0;
    for (i = fix0; i < u->nfixups; ++i) {
        long long t = u->label_at[u->fixups[i].label];
        if (t < 0) return 0;
#ifdef MG_FAR_JUMPS
        if (u->far) {
            if (rk_patch_far(&u->b, u->fixups[i].at, (size_t)t)) return 0;
            continue;
        }
#endif
        if (rk_patch(&u->b, u->fixups[i].at, (size_t)t)) return 0;
    }
    return 1;
}
/* a call or tail call of the unit's own function, within the machine's call's reach */
static int mg_resolve_local(Unit* u, const Reloc* r, size_t target_at) {
    if (r->type != MG_R_CALL && r->type != MG_R_TAIL) return 0;
    return !rk_patch_call(&u->b, r->at, target_at);
}

/* no vector registers: the AdvSIMD instructions are the Sail's emitters', in doublewords */
static int vec(Unit* u, const SnxA64Insn* insn, SnxU32 word) {
    (void)u, (void)insn, (void)word;
    return 0;
}
static int vread(SgCtx* g, unsigned n) {
    (void)g;
    sg_fail("a vector register read on a machine without vector values", n, 0);
    return 0;
}
static void vwrite(SgCtx* g, unsigned d, int r, int q) {
    (void)g, (void)r, (void)q;
    sg_fail("a vector register written on a machine without vector values", d, 0);
}
static int vzero(SgCtx* g) {
    (void)g;
    sg_fail("a vector zero on a machine without vector values", 0, 0);
    return 0;
}
static SgD lane_get(SgCtx* g, unsigned size, int a, int i) {
    (void)g, (void)size, (void)i;
    sg_fail("a vector lane on a machine without vector values", a, 0);
    return sgx_const(1, 0);
}
static int lane_put(SgCtx* g, unsigned size, int a, int i, SgD v) {
    (void)g, (void)size, (void)i, (void)v;
    sg_fail("a vector lane on a machine without vector values", a, 0);
    return 0;
}
static int vdup(SgCtx* g, unsigned size, int a, int e) {
    (void)g, (void)size, (void)e;
    sg_fail("a vector duplicate on a machine without vector values", a, 0);
    return 0;
}
