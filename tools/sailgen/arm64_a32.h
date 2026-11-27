/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's arm64 back end, AArch32's part of its own (after mach_a32.h): the jumps to the block's labels (B and
   B.cond, their offsets patched as it ends), the machine instructions (a condition's skip, the exits, the monitor's
   check, a call, the exclusive store), and the exits' and the monitor's stubs after the block's code, each word an
   encoder's of the A64 Sail (a64_enc.inc). The state's address is in X19; X16 and X17 are the stubs' own */

/* ---- labels ---- */

enum { SGA_FIX_B, SGA_FIX_BCOND }; /* a jump's field: B's imm26, B.cond's and CBZ's imm19 */

static inline void sga_a32_fix(SgCtx* g, SgXBuf* b, int label, int kind) {
    g->fix = (struct SgA32Fix*)sgx_a32_grow(g->fix, &g->cap_fix, g->nfix + 1, sizeof *g->fix);
    g->fix[g->nfix].at = b->n;
    g->fix[g->nfix].kind = kind;
    g->fix[g->nfix++].label = label;
}
static inline void sgx_a32_jcc(SgCtx* g, SgXBuf* b, int cc, int label) {
    sga_a32_fix(g, b, label, SGA_FIX_BCOND);
    sga_w(b, sge_a64_b_cond((SnxU32)cc & 15, 0));
}
static inline void sgx_a32_jmp(SgCtx* g, SgXBuf* b, int label) {
    sga_a32_fix(g, b, label, SGA_FIX_B);
    sga_w(b, sge_a64_b_uncond(0));
}
/* CBZ or CBNZ of a W register to label */
static inline void sga_a32_cbz(SgCtx* g, SgXBuf* b, int nonzero, int rt, int label) {
    sga_a32_fix(g, b, label, SGA_FIX_BCOND);
    sga_w(b, nonzero ? sge_a64_cbnz((SnxU32)rt, 0, 0) : sge_a64_cbz((SnxU32)rt, 0, 0));
}
/* the offset field of the word at at, d words */
static inline int sga_a32_put(SgXBuf* b, size_t at, long long d, int kind) {
    SnxU32 w;
    if (at + 4 > b->cap) return 0;
    w = (SnxU32)b->p[at] | (SnxU32)b->p[at + 1] << 8 | (SnxU32)b->p[at + 2] << 16 | (SnxU32)b->p[at + 3] << 24;
    if (kind == SGA_FIX_B) {
        if (d < -(1ll << 25) || d >= (1ll << 25)) return -1;
        w = (w & ~0x3FFFFFFu) | ((SnxU32)d & 0x3FFFFFFu);
    } else {
        if (d < -(1ll << 18) || d >= (1ll << 18)) return -1;
        w = (w & ~(0x7FFFFu << 5)) | ((SnxU32)d & 0x7FFFFu) << 5;
    }
    b->p[at] = (unsigned char)w, b->p[at + 1] = (unsigned char)(w >> 8), b->p[at + 2] = (unsigned char)(w >> 16);
    b->p[at + 3] = (unsigned char)(w >> 24);
    return 0;
}
/* every jump to a label patched: 0, or -1 where one was never bound or is out of its reach */
static inline int sgx_a32_patch(SgCtx* g, SgXBuf* b) {
    for (int k = 0; k < g->nfix; ++k) {
        long long to = g->label_at[g->fix[k].label];
        if (to < 0 || sga_a32_put(b, g->fix[k].at, (to - (long long)g->fix[k].at) / 4, g->fix[k].kind)) return -1;
    }
    return 0;
}

/* ---- the machine's words ---- */

/* LDR or STR of a W (or X) register at the state's field off */
static inline void sgx_a32_state(SgXBuf* b, int load, int bytes, int rt, size_t off) {
    sga_ldst(b, load, bytes, rt, SGX_BASE, -1, (int)off);
}
/* the state's 32-bit field off = v (W16 the constant's) */
static inline void sgx_a32_state_imm(SgXBuf* b, size_t off, SnxU32 v) {
    if (!v) {
        sgx_a32_state(b, 0, 4, SGA_ZR, off);
        return;
    }
    sga_movi(b, SGA_X16, v, 0);
    sgx_a32_state(b, 0, 4, SGA_X16, off);
}
/* the exclusive load and store of bytes at [X17] (LDAXRB... and STLXRB...) */
static inline SnxU32 sga_a32_ldaxr(unsigned bytes, int rt) {
    return bytes == 1   ? sge_a64_ldaxrb((SnxU32)rt, 17)
           : bytes == 2 ? sge_a64_ldaxrh((SnxU32)rt, 17)
                        : sge_a64_ldaxr((SnxU32)rt, 17, bytes == 8 ? 3 : 2);
}
static inline SnxU32 sga_a32_stlxr(unsigned bytes, int rs, int rt) {
    return bytes == 1   ? sge_a64_stlxrb((SnxU32)rt, 17, (SnxU32)rs)
           : bytes == 2 ? sge_a64_stlxrh((SnxU32)rt, 17, (SnxU32)rs)
                        : sge_a64_stlxr((SnxU32)rt, 17, (SnxU32)rs, bytes == 8 ? 3 : 2);
}
/* a 32-bit constant in a host register */
static inline void sgx_a32_movi(SgXBuf* b, int reg, SnxU32 v) { sga_movi(b, reg, v, 0); }
/* a call of the runtime library's sym, the state its first argument */
static inline void sgx_a32_call_out(SgXAsm* a, SgXBuf* b, const char* sym) {
    sga_state_out(b);
    if (a->reloc) {
        a->reloc(a->machine, b->n, sym, 0); /* BL, which the object's CALL26 fills */
        sga_w(b, sge_a64_bl(0));
    } else {
        SnxU64 to = a->resolve ? a->resolve(sym) : 0;
        if (!to) sg_fail("a helper nothing resolves", 0, 0);
        sga_movi(b, SGA_X16, to, 1);
        sga_w(b, sge_a64_blr(SGA_X16));
    }
}
/* the block's return where the backend's call set an exit */
static inline void sgx_a32_leave(SgCtx* g, SgXBuf* b) {
    sgx_a32_state(b, 1, 4, SGA_X16, offsetof(SnxArmCPU, exit));
    sga_a32_cbz(g, b, 1, SGA_X16, g->ret);
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
            sgx_a32_state(b, 1, 4, SGA_X16, offsetof(SnxArmCPU, exclusive_valid));
            sga_a32_cbz(g, b, 1, SGA_X16, m.label);
            m.back = b->n;
            g->monitors =
                (SgA32Monitor*)sgx_a32_grow(g->monitors, &g->cap_monitors, g->nmonitors + 1, sizeof *g->monitors);
            g->monitors[g->nmonitors++] = m;
            return;
        }
        case SGX_A32_FENCE: sga_w(b, sge_a64_dmb(11)); return; /* DMB ISH */
        case SGX_A32_CALL: sgx_a32_call_out(a, b, i->sym); return;
        case SGX_A32_STREX: {
            /* the store-exclusive: the monitor disarmed, its address and size checked, then a compare-and-swap of the
               value the load-exclusive read at the access's size: W0 0 where it took, 1 otherwise. The address in
               X1, the value in X2, the memory's base in X3 (sgx_a32_strex); X4 and X5 its own */
            unsigned bytes = (unsigned)i->imm;
            int fail = sgx_a32_label(g), differ = sgx_a32_label(g), done = sgx_a32_label(g);
            size_t loop;
            sgx_a32_state(b, 1, 4, SGA_X16, offsetof(SnxArmCPU, exclusive_valid));
            sgx_a32_state(b, 0, 4, SGA_ZR, offsetof(SnxArmCPU, exclusive_valid));
            sga_a32_cbz(g, b, 0, SGA_X16, fail);
            sgx_a32_state(b, 1, 4, SGA_X16, offsetof(SnxArmCPU, exclusive_address));
            sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, 1, 0, SGA_X16, 0, 0)); /* CMP W1, W16 */
            sgx_a32_jcc(g, b, 1, fail);
            sgx_a32_state(b, 1, 4, SGA_X16, offsetof(SnxArmCPU, exclusive_size));
            sga_w(b, sge_a64_subs_addsub_imm(SGA_ZR, SGA_X16, bytes, 0, 0)); /* CMP W16, #bytes */
            sgx_a32_jcc(g, b, 1, fail);
            sgx_a32_state(b, 1, 8, SGA_X16, offsetof(SnxArmCPU, exclusive_value));
            if (bytes < 8) sga_w(b, sge_a64_ubfm(SGA_X16, SGA_X16, 8 * bytes - 1, 0, 1, 1)); /* at the access's size */
            sga_w(b, sge_a64_add_addsub_shift(SGA_X17, 3, 0, 1, 0, 1));                    /* X17 = base + address */
            loop = b->n;
            sga_w(b, sga_a32_ldaxr(bytes, 4));
            sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, 4, 0, SGA_X16, 0, 1));
            sgx_a32_jcc(g, b, 1, differ);
            sga_w(b, sga_a32_stlxr(bytes, 5, 2));
            sga_w(b, sge_a64_cbnz(5, (SnxU32)((long long)(loop - b->n) / 4) & 0x7FFFF, 0));
            sga_w(b, sge_a64_movz(0, 0, 0, 0));
            sgx_a32_jmp(g, b, done);
            sgx_a32_bind(g, b, differ);
            sga_w(b, sge_a64_clrex(15));
            sgx_a32_bind(g, b, fail);
            sga_w(b, sge_a64_movz(0, 1, 0, 0));
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
        /* (address - reserved) < size, or (reserved - address) < bytes: the two ranges overlap, 32-bit wrapping */
        SgA32Monitor* m = &g->monitors[k];
        int clear = sgx_a32_label(g), keep = sgx_a32_label(g);
        sgx_a32_bind(g, b, m->label);
        sgx_a32_state(b, 1, 4, SGA_X17, offsetof(SnxArmCPU, exclusive_address));
        sga_w(b, sge_a64_sub_addsub_shift(SGA_X16, (SnxU32)m->reg, 0, SGA_X17, 0, 0));
        sgx_a32_state(b, 1, 4, SGA_X17, offsetof(SnxArmCPU, exclusive_size));
        sga_w(b, sge_a64_subs_addsub_shift(SGA_ZR, SGA_X16, 0, SGA_X17, 0, 0));
        sgx_a32_jcc(g, b, 3, clear); /* B.LO */
        sgx_a32_state(b, 1, 4, SGA_X17, offsetof(SnxArmCPU, exclusive_address));
        sga_w(b, sge_a64_sub_addsub_shift(SGA_X16, SGA_X17, 0, (SnxU32)m->reg, 0, 0));
        sga_w(b, sge_a64_subs_addsub_imm(SGA_ZR, SGA_X16, m->bytes, 0, 0));
        sgx_a32_jcc(g, b, 2, keep); /* B.HS */
        sgx_a32_bind(g, b, clear);
        sgx_a32_state(b, 0, 4, SGA_ZR, offsetof(SnxArmCPU, exclusive_valid));
        sgx_a32_bind(g, b, keep);
        sga_w(b, sge_a64_b_uncond((SnxU32)((long long)(m->back - b->n) / 4) & 0x3FFFFFF));
    }
    g->nexits = g->nmonitors = 0;
}
