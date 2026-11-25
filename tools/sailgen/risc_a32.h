/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's RISC back ends, AArch32's part (after mach_a32.h): the jumps to the block's labels (a short branch over the
   machine's long jump where conditional, the offsets patched as the block ends), the machine instructions (a
   condition's skip, the exits, the monitor's check, a call, the exclusive store) and the exits' and the monitor's
   stubs after the block's code, from risc.h's operations and the machine's branches. RK_S1 and RK_S2 are the
   sequences' own */

/* ---- labels ---- */

static inline void risc_a32_fix(SgCtx* g, size_t at, int label) {
    g->fix = (struct SgA32Fix*)sgx_a32_grow(g->fix, &g->cap_fix, g->nfix + 1, sizeof *g->fix);
    g->fix[g->nfix].at = at;
    g->fix[g->nfix].kind = 0;
    g->fix[g->nfix++].label = label;
}
static inline void sgx_a32_jmp(SgCtx* g, SgXBuf* b, int label) { risc_a32_fix(g, rk_j(b), label); }
/* to label where r is (nonzero) or isn't 0: the short branch of the opposite over a long jump */
static inline void risc_a32_jreg(SgCtx* g, SgXBuf* b, int nonzero, int r, int label) {
    size_t skip = rk_bz(b, !nonzero, r);
    sgx_a32_jmp(g, b, label);
    rk_patch(b, skip, b->n);
}
static inline void sgx_a32_jcc(SgCtx* g, SgXBuf* b, int cc, int label) {
    risc_cond(b, RK_S1, cc);
    risc_a32_jreg(g, b, 1, RK_S1, label);
}
/* every jump to a label patched: 0, or -1 where one was never bound or is out of its reach */
static inline int sgx_a32_patch(SgCtx* g, SgXBuf* b) {
    for (int k = 0; k < g->nfix; ++k) {
        long long to = g->label_at[g->fix[k].label];
        if (to < 0 || rk_patch(b, g->fix[k].at, (size_t)to)) return -1;
    }
    return 0;
}

/* ---- the machine's words ---- */

static inline int sgx_state_bytes(int disp) {
    (void)disp;
    return 0; /* SnxArmCPU's fields are all numbers */
}
static inline void sgx_a32_state(SgXBuf* b, int load, int bytes, int rt, size_t off) {
    rk_ldst(b, load, bytes, 0, rt, SGX_BASE, -1, (int)off);
}
/* the state's 32-bit field off = v (RK_S2 the constant's) */
static inline void sgx_a32_state_imm(SgXBuf* b, size_t off, SnxU32 v) {
    rk_li(b, RK_S2, v);
    sgx_a32_state(b, 0, 4, RK_S2, off);
}
/* a 32-bit constant in a host register */
static inline void sgx_a32_movi(SgXBuf* b, int reg, SnxU32 v) { rk_li(b, reg, v); }
/* a call of the runtime library's sym, the state its first argument */
static inline void sgx_a32_call_out(SgXAsm* a, SgXBuf* b, const char* sym) {
    rk_mv(b, sgx_arg_regs[0], SGX_BASE);
    if (a->reloc) {
        size_t lit = rk_call_reloc(b);
        a->reloc(a->machine, lit, sym, 0);
    } else {
        SnxU64 to = a->resolve ? a->resolve(sym) : 0;
        if (!to) sg_fail("a helper nothing resolves", 0, 0);
        rk_li(b, RK_SA, to);
        rk_call_reg(b, RK_SA);
    }
}
/* the block's return where the backend's call set an exit */
static inline void sgx_a32_leave(SgCtx* g, SgXBuf* b) {
    sgx_a32_state(b, 1, 4, RK_S1, offsetof(SnxArmCPU, exit));
    risc_a32_jreg(g, b, 1, RK_S1, g->ret);
}
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);
static inline void sgx_machine_ins(SgXAsm* a, int at, const unsigned char* r) {
    SgXIns* i = &a->x->ins[at];
    SgCtx* g = (SgCtx*)a->machine;
    SgXBuf* b = a->b;
    if (i->kind != SGX_K_MACHINE) sg_fail("a machine instruction of another back end", i->kind, 0);
    switch (i->op) {
        case SGX_A32_JCC: sgx_a32_jcc(g, b, i->cc, i->disp); return;
        case SGX_A32_JMP: sgx_a32_jmp(g, b, i->disp); return;
        case SGX_A32_EXIT: {
            SgA32Exit e = {
                .label = sgx_a32_label(g),
                .exit = (unsigned)i->imm,
                .reg = i->kv[0] & 1 ? (signed char)-1 : (signed char)r[0],
                .pc_kept = (signed char)(i->kv[0] >> 1 & 1),
                .syndrome = (SnxU32)(i->kv[0] >> 32),
                .pc = (SnxU32)i->kv[1],
            };
            g->exits = (SgA32Exit*)sgx_a32_grow(g->exits, &g->cap_exits, g->nexits + 1, sizeof *g->exits);
            g->exits[g->nexits++] = e;
            sgx_a32_jcc(g, b, i->cc, e.label);
            return;
        }
        case SGX_A32_MONITOR: {
            /* armed: the cold path's check of the store's range against the reservation */
            SgA32Monitor m = {.label = sgx_a32_label(g), .reg = (signed char)r[0], .bytes = (unsigned)i->imm};
            sgx_a32_state(b, 1, 4, RK_S1, offsetof(SnxArmCPU, exclusive_valid));
            risc_a32_jreg(g, b, 1, RK_S1, m.label);
            m.back = b->n;
            g->monitors =
                (SgA32Monitor*)sgx_a32_grow(g->monitors, &g->cap_monitors, g->nmonitors + 1, sizeof *g->monitors);
            g->monitors[g->nmonitors++] = m;
            return;
        }
        case SGX_A32_FENCE: rk_fence(b); return;
        case SGX_A32_CALL: sgx_a32_call_out(a, b, i->sym); return;
        case SGX_A32_STREX: {
            /* llvm.c's exclusive(): the monitor disarmed, its address and size checked, then a compare-and-swap of the
               value the load-exclusive read at the access's size: SGX_RET 0 where it took, 1 otherwise. The address,
               the value and the memory's base in sgx_a32_strex's registers; the clobbered two its own, and RK_S1, RK_S2,
               RK_SA and RK_FA (the flags are none across it) */
            unsigned bytes = (unsigned)i->imm, word = bytes < 4 ? 4 : bytes;
            int ad = sgx_a32_strex[0], val = sgx_a32_strex[1], base = sgx_a32_strex[2];
            int t1 = __builtin_ctz(SGX_A32_STREX_CLOBBER), t2 = 31 - __builtin_clz(SGX_A32_STREX_CLOBBER);
            int fail = sgx_a32_label(g), done = sgx_a32_label(g);
            size_t loop, differ;
            sgx_a32_state(b, 1, 4, RK_S1, offsetof(SnxArmCPU, exclusive_valid));
            rk_li(b, RK_S2, 0);
            sgx_a32_state(b, 0, 4, RK_S2, offsetof(SnxArmCPU, exclusive_valid));
            risc_a32_jreg(g, b, 0, RK_S1, fail);
            sgx_a32_state(b, 1, 4, RK_S1, offsetof(SnxArmCPU, exclusive_address));
            rk_zext(b, RK_S2, ad, 32);
            rk_xor(b, RK_S1, RK_S1, RK_S2);
            risc_a32_jreg(g, b, 1, RK_S1, fail);
            sgx_a32_state(b, 1, 4, RK_S1, offsetof(SnxArmCPU, exclusive_size));
            rk_xori(b, RK_S1, RK_S1, bytes);
            risc_a32_jreg(g, b, 1, RK_S1, fail);
            /* t1 = the host address of the aligned word or doubleword; RK_FA the field's bit offset in it */
            rk_zext(b, t1, ad, 32);
            rk_add(b, t1, t1, base, 1);
            if (bytes < 4) {
                rk_andi(b, RK_FA, t1, 3);
                if (RK_BIG_ENDIAN) rk_xori(b, RK_FA, RK_FA, (long long)(4 - bytes)); /* the word's other end */
                rk_slli(b, RK_FA, RK_FA, 3, 1);
                rk_andi(b, t1, t1, -4);
            }
            /* RK_FB = the expected value (the load-exclusive's, at the access's size); a big-endian host's compare
               and store in its own order, the expected and the value byte-reversed once */
            sgx_a32_state(b, 1, 8, RK_FB, offsetof(SnxArmCPU, exclusive_value));
            if (bytes < 8) rk_zext(b, RK_FB, RK_FB, (int)(8 * bytes));
            if (RK_BIG_ENDIAN && bytes > 1) {
                rk_bswap(b, RK_FB, RK_FB, (int)bytes);
                rk_bswap(b, t2, val, (int)bytes);
                val = t2;
            }
            loop = b->n;
            rk_lr(b, RK_S1, t1, (int)word);
            /* old: the access's bits */
            if (bytes < 4) {
                rk_srl(b, RK_S2, RK_S1, RK_FA, 1);
                rk_zext(b, RK_S2, RK_S2, (int)(8 * bytes));
            } else if (bytes == 4)
                rk_zext(b, RK_S2, RK_S1, 32);
            else
                rk_mv(b, RK_S2, RK_S1);
            rk_xor(b, RK_S2, RK_S2, RK_FB);
            differ = rk_bz(b, 1, RK_S2);
            /* new: the value (the field of the word replaced) */
            if (bytes < 4) {
                rk_li(b, RK_S2, bytes == 1 ? 0xffu : 0xffffu);
                rk_and(b, t2, val, RK_S2);
                rk_sll(b, t2, t2, RK_FA, 1);
                rk_sll(b, RK_S2, RK_S2, RK_FA, 1);
                rk_xori(b, RK_S2, RK_S2, -1);
                rk_and(b, RK_S1, RK_S1, RK_S2);
                rk_or(b, t2, t2, RK_S1);
                rk_sc(b, RK_S2, t1, t2, (int)word);
            } else
                rk_sc(b, RK_S2, t1, val, (int)word);
            rk_patch(b, rk_bz(b, 1, RK_S2), loop);
            rk_li(b, SGX_RET, 0);
            sgx_a32_jmp(g, b, done);
            rk_patch(b, differ, b->n);
            rk_fence(b);
            sgx_a32_bind(g, b, fail);
            rk_li(b, SGX_RET, 1);
            sgx_a32_bind(g, b, done);
            return;
        }
        case SGX_A32_CHECK: sgx_a32_leave(g, b); return;
        case SGX_A32_UNDEF_END:
            sgx_a32_leave(g, b);                                               /* the backend stopped there */
            sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, (SnxU32)i->imm); /* else R15 = the next instruction */
            sgx_a32_jmp(g, b, g->ret);
            return;
        default: sgx_machine_op(a, at, r); return;
    }
}

/* the block's exits and the monitor's cold paths, after its code: each exit's fields stored, then the block's return */
static inline void sgx_a32_stubs(SgCtx* g, SgXBuf* b) {
    for (int k = 0; k < g->nexits; ++k) {
        SgA32Exit* e = &g->exits[k];
        sgx_a32_bind(g, b, e->label);
        if (!e->pc_kept) sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, e->pc);
        sgx_a32_state_imm(b, offsetof(SnxArmCPU, exit), e->exit);
        if (e->reg < 0)
            sgx_a32_state_imm(b, offsetof(SnxArmCPU, syndrome), e->syndrome);
        else
            sgx_a32_state(b, 0, 4, e->reg, offsetof(SnxArmCPU, syndrome));
        sgx_a32_jmp(g, b, g->ret);
    }
    for (int k = 0; k < g->nmonitors; ++k) {
        /* (address - reserved) < size, or (reserved - address) < bytes, at 32 bits: the two ranges overlap */
        SgA32Monitor* m = &g->monitors[k];
        size_t keep, clear;
        sgx_a32_bind(g, b, m->label);
        sgx_a32_state(b, 1, 4, RK_S2, offsetof(SnxArmCPU, exclusive_address));
        rk_sub(b, RK_S1, m->reg, RK_S2, 1);
        rk_zext(b, RK_S1, RK_S1, 32);
        sgx_a32_state(b, 1, 4, RK_S2, offsetof(SnxArmCPU, exclusive_size));
        rk_sltu(b, RK_S1, RK_S1, RK_S2);
        clear = rk_bz(b, 1, RK_S1);
        sgx_a32_state(b, 1, 4, RK_S2, offsetof(SnxArmCPU, exclusive_address));
        rk_sub(b, RK_S1, RK_S2, m->reg, 1);
        rk_zext(b, RK_S1, RK_S1, 32);
        rk_li(b, RK_S2, m->bytes);
        rk_sltu(b, RK_S1, RK_S1, RK_S2);
        keep = rk_bz(b, 0, RK_S1);
        rk_patch(b, clear, b->n);
        rk_li(b, RK_S2, 0);
        sgx_a32_state(b, 0, 4, RK_S2, offsetof(SnxArmCPU, exclusive_valid));
        rk_patch(b, keep, b->n);
        rk_patch(b, rk_j(b), m->back);
    }
    g->nexits = g->nmonitors = 0;
}
