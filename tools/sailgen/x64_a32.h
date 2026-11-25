/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's x86-64 back end, AArch32's part of its own (after mach_a32.h): the jumps to the block's labels (rel32s
   patched as it ends), the machine instructions' bytes (a condition's skip, the exits, the monitor's check, a call,
   the exclusive store), and the exits' and the monitor's stubs after the block's code. The state's address is in
   RBX. Windows' calling convention (SGX_F_WIN64) changes a call's registers, the exclusive store's address's, and where
   the monitor's stub keeps RAX */

/* ---- labels ---- */

/* a rel32 at b's end to label, patched when the block is done (sgx_a32_patch) */
static inline void sgx_a32_rel(SgCtx* g, SgXBuf* b, int label) {
    g->fix = (struct SgA32Fix*)sgx_a32_grow(g->fix, &g->cap_fix, g->nfix + 1, sizeof *g->fix);
    g->fix[g->nfix].at = b->n;
    g->fix[g->nfix].kind = 0;
    g->fix[g->nfix++].label = label;
    sgx_imm(b, 0, 4);
}
static inline void sgx_a32_jcc(SgCtx* g, SgXBuf* b, int cc, int label) {
    sgx_byte(b, 0x0F);
    sgx_byte(b, (unsigned char)(0x80 | (cc & 15)));
    sgx_a32_rel(g, b, label);
}
static inline void sgx_a32_jmp(SgCtx* g, SgXBuf* b, int label) {
    sgx_byte(b, 0xE9);
    sgx_a32_rel(g, b, label);
}
/* every jump to a label patched: 0, or -1 where one was never bound */
static inline int sgx_a32_patch(SgCtx* g, SgXBuf* b) {
    for (int k = 0; k < g->nfix; ++k) {
        long long to = g->label_at[g->fix[k].label];
        size_t at = g->fix[k].at;
        if (to < 0) return -1;
        long long d = to - (long long)(at + 4);
        if (at + 4 <= b->cap) {
            b->p[at] = (unsigned char)d;
            b->p[at + 1] = (unsigned char)(d >> 8);
            b->p[at + 2] = (unsigned char)(d >> 16);
            b->p[at + 3] = (unsigned char)(d >> 24);
        }
    }
    return 0;
}

/* ---- the machine's bytes ---- */

/* the rel32 at at, to b's end */
static inline void sgx_a32_patch32(SgXBuf* b, size_t at) {
    long long d = (long long)b->n - (long long)(at + 4);
    if (at + 4 <= b->cap) {
        b->p[at] = (unsigned char)d;
        b->p[at + 1] = (unsigned char)(d >> 8);
        b->p[at + 2] = (unsigned char)(d >> 16);
        b->p[at + 3] = (unsigned char)(d >> 24);
    }
}
/* the rel8 at at, to b's end (a short jump of the cold paths) */
static inline void sgx_a32_patch8(SgXBuf* b, size_t at) {
    if (at < b->cap) b->p[at] = (unsigned char)(b->n - (at + 1));
}
/* an instruction [rbx + disp32] of the state: op8 (and REX.W where w64), ModRM's reg field reg */
static inline void sgx_a32_state(SgXBuf* b, int w64, unsigned op8, int reg, size_t disp) {
    if (w64 || reg >= 8) sgx_byte(b, (unsigned char)(0x40 | (w64 ? 8 : 0) | (reg >> 3) << 2));
    sgx_byte(b, (unsigned char)op8);
    if (disp < 128) { /* [rbx + disp8] */
        sgx_byte(b, (unsigned char)(0x43 | (reg & 7) << 3));
        sgx_byte(b, (unsigned char)disp);
        return;
    }
    sgx_byte(b, (unsigned char)(0x83 | (reg & 7) << 3)); /* [rbx + disp32] */
    sgx_imm(b, (long long)disp, 4);
}
/* mov dword [rbx + disp], imm32 */
static inline void sgx_a32_state_imm(SgXBuf* b, size_t disp, SnxU32 v) {
    sgx_a32_state(b, 0, 0xC7, 0, disp);
    sgx_imm(b, (long long)v, 4);
}
/* a 32-bit constant in a host register */
static inline void sgx_a32_movi(SgXBuf* b, int reg, SnxU32 v) {
    if (reg >= 8) sgx_byte(b, 0x41);
    sgx_byte(b, (unsigned char)(0xB8 | (reg & 7))); /* mov r32, imm32 */
    sgx_imm(b, (long long)v, 4);
}
/* a call of the runtime library's sym, the state its first argument */
static inline void sgx_a32_call_out(SgXAsm* a, SgXBuf* b, const char* sym) {
    sgx_call_room(a);
    sgx_state_arg(a, b);
    if (a->reloc) {
        sgx_byte(b, 0xE8); /* call rel32, which the object's relocation fills */
        a->reloc(a->machine, b->n, sym, -4);
        sgx_imm(b, 0, 4);
    } else {
        SnxU64 to = a->resolve ? a->resolve(sym) : 0;
        if (!to) sg_fail("a helper nothing resolves", 0, 0);
        sgx_bytes(b, "48 B8"); /* mov rax, imm64; call rax */
        sgx_imm(b, (long long)to, 8);
        sgx_bytes(b, "FF D0");
    }
}
/* the block's return where the backend's call set an exit */
static inline void sgx_a32_leave(SgCtx* g, SgXBuf* b) {
    sgx_a32_state(b, 0, 0x83, 7, offsetof(SnxArmCPU, exit)); /* cmp dword [rbx + exit], 0 */
    sgx_byte(b, 0);
    sgx_a32_jcc(g, b, 5, g->ret);
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
            sgx_a32_state(b, 0, 0x83, 7, offsetof(SnxArmCPU, exclusive_valid)); /* cmp dword [rbx + valid], imm8 */
            sgx_byte(b, 0);
            if (SGX_WIN64(a->x)) sgx_call_room(a); /* the stub keeps RAX in the callees' bytes of the frame */
            SgA32Monitor m = {.label = sgx_a32_label(g), .reg = (signed char)r[0], .bytes = (unsigned)i->imm};
            sgx_a32_jcc(g, b, 5, m.label); /* jne: armed */
            m.back = b->n;
            g->monitors =
                (SgA32Monitor*)sgx_a32_grow(g->monitors, &g->cap_monitors, g->nmonitors + 1, sizeof *g->monitors);
            g->monitors[g->nmonitors++] = m;
            return;
        }
        case SGX_A32_FENCE: sgx_bytes(b, "0F AE F0"); return; /* mfence */
        case SGX_A32_CALL: sgx_a32_call_out(a, b, i->sym); return;
        case SGX_A32_STREX: {
            /* llvm.c's exclusive(): the reservation's address and size checked and the monitor cleared, then a
           compare-and-swap of the value the load-exclusive read: status 0 where it took, 1 otherwise */
            unsigned bytes = (unsigned)i->imm;
            const int ad = SGX_A32_STREX_REGS(a->x)[0];
            size_t fail[3];
            sgx_a32_state(b, 0, 0x8B, 1, offsetof(SnxArmCPU, exclusive_valid)); /* mov ecx, [rbx + valid] */
            sgx_a32_state_imm(b, offsetof(SnxArmCPU, exclusive_valid), 0);
            sgx_bytes(b, "85 C9 0F 84"), fail[0] = b->n, sgx_imm(b, 0, 4);        /* test ecx, ecx; jz */
            sgx_a32_state(b, 0, 0x3B, ad, offsetof(SnxArmCPU, exclusive_address)); /* cmp esi, [rbx + address] */
            sgx_bytes(b, "0F 85"), fail[1] = b->n, sgx_imm(b, 0, 4);               /* jne */
            sgx_a32_state(b, 0, 0x83, 7, offsetof(SnxArmCPU, exclusive_size));     /* cmp dword [rbx + size], bytes */
            sgx_byte(b, (unsigned char)bytes);
            sgx_bytes(b, "0F 85"), fail[2] = b->n, sgx_imm(b, 0, 4);            /* jne */
            sgx_a32_state(b, 1, 0x8B, 0, offsetof(SnxArmCPU, exclusive_value)); /* mov rax, [rbx + value] */
            /* lock cmpxchg [r10 + rsi], dl / dx / edx / rdx */
            sgx_byte(b, 0xF0);
            if (bytes == 2) sgx_byte(b, 0x66);
            sgx_byte(b, (unsigned char)(0x41 | (bytes == 8 ? 8 : 0) | (ad >> 3) << 1));
            sgx_bytes(b, bytes == 1 ? "0F B0" : "0F B1");
            sgx_byte(b, 0x14), sgx_byte(b, (unsigned char)(0x02 | (ad & 7) << 3));
            sgx_bytes(b, "0F 95 C0 0F B6 C0 E9"); /* setne al; movzx eax, al; jmp done */
            size_t done = b->n;
            sgx_imm(b, 0, 4);
            sgx_a32_patch32(b, fail[0]), sgx_a32_patch32(b, fail[1]), sgx_a32_patch32(b, fail[2]);
            sgx_bytes(b, "B8 01 00 00 00"); /* mov eax, 1 */
            sgx_a32_patch32(b, done);
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
            sgx_a32_state(b, 0, 0x89, e->reg, offsetof(SnxArmCPU, syndrome)); /* mov [rbx + syndrome], reg32 */
        sgx_a32_jmp(g, b, g->ret);
    }
    for (int k = 0; k < g->nmonitors; ++k) {
        SgA32Monitor* m = &g->monitors[k];
        sgx_a32_bind(g, b, m->label);
        const int win64 = SGX_WIN64(sgx(g));
        sgx_bytes(b, win64 ? "48 89 44 24 20" : "50"); /* push rax; mov [rsp + 32], rax */
        if (m->reg != 0) {                             /* mov eax, reg */
            if (m->reg >= 8) sgx_byte(b, 0x44);
            sgx_byte(b, 0x89);
            sgx_byte(b, (unsigned char)(0xC0 | (m->reg & 7) << 3));
        }
        /* (address - reserved) < size, or (reserved - address) < bytes: the two ranges overlap, 32-bit wrapping */
        sgx_a32_state(b, 0, 0x2B, 0, offsetof(SnxArmCPU, exclusive_address)); /* sub eax, [rbx + address] */
        sgx_a32_state(b, 0, 0x3B, 0, offsetof(SnxArmCPU, exclusive_size));    /* cmp eax, [rbx + size] */
        sgx_bytes(b, "72");                                                   /* jb clear */
        size_t clear = b->n;
        sgx_byte(b, 0);
        sgx_a32_state(b, 0, 0x8B, 0, offsetof(SnxArmCPU, exclusive_address)); /* mov eax, [rbx + address] */
        if (m->reg == 0)
            sgx_bytes(b, win64 ? "2B 44 24 20" : "2B 04 24"); /* sub eax, [rsp]: the address, kept there */
        else {
            if (m->reg >= 8) sgx_byte(b, 0x44);
            sgx_byte(b, 0x29); /* sub eax, reg */
            sgx_byte(b, (unsigned char)(0xC0 | (m->reg & 7) << 3));
        }
        sgx_byte(b, 0x3D), sgx_imm(b, m->bytes, 4); /* cmp eax, bytes */
        sgx_bytes(b, "73");                         /* jae keep */
        size_t keep = b->n;
        sgx_byte(b, 0);
        sgx_a32_patch8(b, clear);
        sgx_a32_state_imm(b, offsetof(SnxArmCPU, exclusive_valid), 0);
        sgx_a32_patch8(b, keep);
        sgx_bytes(b, win64 ? "48 8B 44 24 20 E9" : "58 E9"); /* pop rax; jmp back */
        sgx_imm(b, (long long)m->back - (long long)(b->n + 4), 4);
    }
    g->nexits = g->nmonitors = 0;
}

