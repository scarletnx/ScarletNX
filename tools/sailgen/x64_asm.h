/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's x86-64 back end, its assembly (after mach.h, x64.h's README): the moves the register allocation makes,
   the XMM registers' forms and spills, the constant pool, the cold paths and the frame, and mach.h's hooks */

static inline const SgXSel* sgx_find(const SgXSel* t, unsigned n, int rm_is_out, int w_rm) {
    unsigned i;
    (void)w_rm;
    for (i = 0; i < n; ++i) {
        if (t[i].rm < 0 || t[i].feature) continue;
        if (rm_is_out ? t[i].out == t[i].rm : t[i].in[0] == t[i].rm) return &t[i];
    }
    sg_fail("no MOV form for a load or a store", rm_is_out, 0);
}

static inline void sgx_mov_rr(SgXAsm* a, int dst, int src) {
    const SgXSel* s;
    unsigned char r[4];
    if (dst == src) return;
    s = sgx_find(sgx_sel_mov_64, sgx_sel_mov_64_n, 0, 0);
    r[(int)s->out] = (unsigned char)dst;
    r[(int)s->in[0]] = (unsigned char)src;
    s->enc(a->b, r, 0, 0);
}
/* a register to or from memory at base + disp (64 bits, or 32: a 32-bit load zero extends) */
static inline void sgx_mov_mem(SgXAsm* a, int load, int w, int reg, int base, int disp) {
    const SgXSel* m = w == 64 ? sgx_find(sgx_sel_mov_64, sgx_sel_mov_64_n, !load, 0)
                              : sgx_find(sgx_sel_mov_32, sgx_sel_mov_32_n, !load, 0);
    unsigned char r[4] = {0, 0, 0, 0};
    if (load) {
        r[(int)m->out] = (unsigned char)reg;
        r[(int)m->in[0]] = (unsigned char)(16 + base);
    } else {
        r[(int)m->out] = (unsigned char)(16 + base);
        r[(int)m->in[0]] = (unsigned char)reg;
    }
    a->b->disp = disp;
    a->b->index = -1;
    m->enc(a->b, r, 0, 0);
}

/* MOVQ xmm, r64 (66 REX.W 0F 6E /r) and MOVQ r64, xmm (66 REX.W 0F 7E /r): a spill to an XMM register, as bytes (the
   selection reads general-purpose forms only) */
static inline void sgx_movq(SgXBuf* b, int to_xmm, int xmm, int gpr) {
    sgx_byte(b, 0x66);
    sgx_byte(b, (unsigned char)(0x48 | (xmm >> 3) << 2 | (gpr >> 3)));
    sgx_byte(b, 0x0F);
    sgx_byte(b, to_xmm ? 0x6E : 0x7E);
    sgx_byte(b, (unsigned char)(0xC0 | (xmm & 7) << 3 | (gpr & 7)));
}

/* a value spilled where no register is free: to a free XMM register where nothing calls out before it is last read
   (every XMM register is caller-saved), else to its slot of the frame, given once (an SSA value never changes, so a
   second spill stores nothing) */

/* ---- XMM registers: vectors' values and spilled general-purpose ones ---- */

/* ---- XMM registers ---- */

/* f between XMM register xr and the memory at base + disp (MOVDQU, MOVQ to or from it) */
static inline void sgx_vmem(SgXAsm* a, const SgXVForm* f, int xr, int base, int disp) {
    unsigned char r[4] = {0, 0, 0, 0};
    r[(int)f->reg] = (unsigned char)xr;
    r[(int)f->rm] = (unsigned char)(16 + base);
    a->b->disp = disp;
    a->b->index = -1;
    f->enc(a->b, r, 0, 0);
}
static inline void sgx_vrr(SgXAsm* a, const SgXVForm* f, int dst, int src) {
    unsigned char r[4] = {0, 0, 0, 0};
    r[(int)f->reg] = (unsigned char)dst;
    r[(int)f->rm] = (unsigned char)src;
    f->enc(a->b, r, 0, 0);
}
/* the pool after the code, 16-aligned, and the operands' displacements to it; emptied for the next function */
static inline void sgx_pool_emit(SgXAsm* a) {
    SgXBuf* b = a->b;
    size_t at;
    int k;
    if (!a->npool) {
        a->nrips = 0;
        return;
    }
    while (b->n % 16) sgx_byte(b, 0xCC);
    at = b->n;
    for (k = 0; k < a->npool; ++k) {
        sgx_imm(b, (long long)a->pool[k][0], 8);
        sgx_imm(b, (long long)a->pool[k][1], 8);
    }
    for (k = 0; k < a->nrips; ++k) {
        long long d = (long long)(at + 16 * (size_t)a->rips[k].idx) - (long long)a->rips[k].end;
        size_t p = a->rips[k].at;
        if (p + 4 <= b->cap) {
            b->p[p] = (unsigned char)d;
            b->p[p + 1] = (unsigned char)(d >> 8);
            b->p[p + 2] = (unsigned char)(d >> 16);
            b->p[p + 3] = (unsigned char)(d >> 24);
        }
    }
    a->npool = a->nrips = 0;
}

/* ---- cold paths ---- */

static inline void sgx_cjump(SgXAsm* a, size_t at, size_t to, int in_cold) {
    if (a->ncjumps == a->cap_cjumps) {
        a->cap_cjumps = a->cap_cjumps ? 2 * a->cap_cjumps : 32;
        a->cjumps = (struct SgXCJump*)realloc(a->cjumps, (size_t)a->cap_cjumps * sizeof *a->cjumps);
        if (!a->cjumps) sg_fail("out of memory", 0, 0);
    }
    a->cjumps[a->ncjumps].at = at;
    a->cjumps[a->ncjumps].to = to;
    a->cjumps[a->ncjumps++].in_cold = (unsigned char)in_cold;
}
static inline void sgx_cold_room(SgXAsm* a, size_t need) {
    if (a->cold.cap - a->cold.n >= need) return;
    a->cold.cap = a->cold.cap * 2 + need;
    a->cold.p = (unsigned char*)realloc(a->cold.p, a->cold.cap);
    if (!a->cold.p) sg_fail("out of memory", 0, 0);
}

/* SGX_K_COLD at at: the hot code jumps out where its condition holds; the cold code keeps the registers the call
   changes that are read after (and the arguments) in scratch slots of the frame above the region's, calls with the
   arguments from where they are, puts RAX in the result's register, takes the others back and jumps in again */
/* RAX = a value of the cold path's (v; -1: the result, in reg out, an XMM one where cls), its doubled magnitude's
   flags then (ADD of itself, 32 bits where w32): RAX was pushed, so a value in RAX is [rsp], a slot 8 further */
static inline void sgx_cold_zf(SgXAsm* a, int v, int out, int cls, int w32) {
    SgXBuf* c = a->b;
    int reg = v < 0 ? out : a->x->v[v].reg, xcls = v < 0 ? cls : a->x->v[v].cls;
    if (xcls && reg >= 0) {
        const SgXVForm* f = w32 ? &sgx_v_movd_rx : &sgx_v_movq_rx;
        unsigned char r[4] = {0, 0, 0, 0};
        r[(int)f->rm] = 0;
        r[(int)f->reg] = (unsigned char)reg;
        f->enc(c, r, 0, 0);
    } else if (reg == 0)
        sgx_mov_mem(a, 1, 64, 0, SGX_RSP, 0);
    else if (reg > 0)
        sgx_mov_rr(a, 0, reg);
    else if (!xcls && a->x->v[v].xmm >= 0)
        sgx_movq(c, 0, a->x->v[v].xmm, 0);
    else
        sgx_mov_mem(a, 1, 64, 0, SGX_RSP, 8 + 8 * a->x->v[v].slot);
    if (!w32) sgx_byte(c, 0x48);
    sgx_byte(c, 0x01); /* add eax, eax */
    sgx_byte(c, 0xC0);
}
/* op reg, value v: reg (its low w bits) with v in a register or its slot, an opcode of reg, r/m's form (CMP 3B, SUB 2B) */
static inline void sgx_op_v(SgXAsm* a, int op, unsigned w, int reg, int v) {
    SgXBuf* b = a->b;
    const SgXV* vv = &a->x->v[v];
    int rm = vv->reg;
    unsigned char rex = (unsigned char)((w == 64) << 3 | (reg >> 3) << 2 | (rm >= 0 ? rm >> 3 : 0));
    if (rex) sgx_byte(b, (unsigned char)(0x40 | rex));
    sgx_byte(b, (unsigned char)op);
    if (rm >= 0)
        sgx_byte(b, (unsigned char)(0xC0 | (reg & 7) << 3 | (rm & 7)));
    else {
        sgx_byte(b, (unsigned char)(0x84 | (reg & 7) << 3)); /* [rsp + disp32] */
        sgx_byte(b, 0x24);
        sgx_imm(b, 8 * vv->slot, 4);
    }
}
/* op reg, the pool's doubleword k */
static inline void sgx_op_pool(SgXAsm* a, int op, int reg, SnxU64 k) {
    SgXBuf* b = a->b;
    sgx_byte(b, (unsigned char)(0x48 | (reg >> 3) << 2));
    sgx_byte(b, (unsigned char)op);
    sgx_byte(b, (unsigned char)((reg & 7) << 3 | 5)); /* [rip + disp32] */
    if (a->nrips == a->cap_rips) {
        a->cap_rips = a->cap_rips ? 2 * a->cap_rips : 32;
        a->rips = (struct SgXRip*)realloc(a->rips, (size_t)a->cap_rips * sizeof *a->rips);
        if (!a->rips) sg_fail("out of memory", 0, 0);
    }
    a->rips[a->nrips].at = b->n;
    sgx_imm(b, 0, 4);
    a->rips[a->nrips].end = b->n;
    a->rips[a->nrips++].idx = sgx_pool_index(a, k, 0);
}
/* the jumps of a cold path's hot code into it: the condition's on the flags it has, then each test's. The result's
   register is out (an XMM one where cls). What the tests need in registers is put there before the first jump, as the
   cold code is made for the registers as they are at all of them */
static inline void sgx_cold_jumps(SgXAsm* a, int at, int out, int cls) {
    SgX* x = a->x;
    SgXIns* i = &x->ins[at];
    SgXBuf* b = a->b;
    unsigned avoid = 0;
    int k, t = -1;
    /* the result is in out: no value moved out of the way goes there */
    if (cls)
        a->xowner[out] = -1;
    else
        a->owner[out] = -1;
    for (k = 0; k < i->ntests; ++k) {
        int l = i->test[k].limit ? i->in[i->test[k].limit] : 0;
        if (l && x->v[l].reg < 0 && x->v[l].slot < 0) sgx_in_reg(a, l, at, avoid);
        if (l && x->v[l].reg >= 0) avoid |= 1u << x->v[l].reg;
    }
    if (i->ntests) t = sgx_free_reg(a, at, avoid);
    if (i->flags_from >= 0) {
        sgx_byte(b, 0x0F); /* Jcc rel32 */
        sgx_byte(b, (unsigned char)(0x80 | (i->cc & 15)));
        sgx_cjump(a, b->n, a->cold.n, 0);
        sgx_imm(b, 0, 4);
    }
    for (k = 0; k < i->ntests; ++k) {
        unsigned w = i->test[k].w;
        int v = i->in[i->test[k].of], reg = i->test[k].of ? x->v[v].reg : out, cc;
        SnxU64 normal = w == 32 ? 0x00800000u : 0x0010000000000000ull,
               inf = w == 32 ? 0x7F800000u : 0x7FF0000000000000ull;
        SnxU64 less = i->test[k].kind == SGX_T_NORMAL ? (normal + 1) << 1 : i->test[k].kind == SGX_T_DENORMAL ? 2 : 0;
        unsigned char r[4] = {0, 0, 0, 0};
        if (reg >= 0) {
            const SgXVForm* f = w == 32 ? &sgx_v_movd_rx : &sgx_v_movq_rx;
            r[(int)f->rm] = (unsigned char)t;
            r[(int)f->reg] = (unsigned char)reg;
            f->enc(b, r, 0, 0);
        } else
            sgx_mov_mem(a, 1, (int)w, t, SGX_RSP, 8 * x->v[v].slot);
        r[0] = r[1] = r[2] = (unsigned char)t;
        /* twice the bits less the constant, one LEA's displacement but for a double's smallest normal */
        sgx_enc_lea(b, r, less >> 31 ? 0 : -(long long)less, w == 64, 1);
        if (less >> 31) sgx_op_pool(a, 0x2B, t, less); /* sub */
        if (i->test[k].limit)
            sgx_op_v(a, 0x3B, w, t, i->in[i->test[k].limit]); /* cmp */
        else if (w == 64)
            sgx_op_pool(a, 0x3B, t, inf << 1);
        else {
            if (t >> 3) sgx_byte(b, 0x41);
            sgx_byte(b, 0x81); /* cmp t, imm32 */
            sgx_byte(b, (unsigned char)(0xF8 | (t & 7)));
            sgx_imm(b, (long long)(inf << 1), 4);
        }
        cc = i->test[k].kind == SGX_T_NORMAL ? 3 : i->test[k].kind == SGX_T_DENORMAL ? 2 : 7; /* AE, B, A */
        sgx_byte(b, 0x0F);
        sgx_byte(b, (unsigned char)(0x80 | cc));
        sgx_cjump(a, b->n, a->cold.n, 0);
        sgx_imm(b, 0, 4);
    }
}
static inline void sgx_assemble_cold(SgXAsm* a, int at) {
    static const signed char args_reg[5] = {6, 2, 1, 8, 9}; /* RSI RDX RCX R8 R9, RDI the state */
    SgX* x = a->x;
    SgXIns* i = &x->ins[at];
    SgXBuf* b = a->b;
    SgXBuf* c;
    int tv = i->in[0], cls = x->v[tv].cls, from, outreg, k, n = 0, nargs = i->w;
    int gslot[16], xslot[16], argslot[5];
    unsigned char saved_g[16] = {0}, saved_x[16] = {0};
    /* the result's register: the tied value's, which dies here, or a copy of it */
    from = cls ? sgx_xin(a, tv, at, 0) : sgx_in_reg(a, tv, at, 0);
    if (x->v[tv].last == at && !x->v[tv].pinned) {
        outreg = from;
        if (cls) a->xowner[from] = 0;
        else
            a->owner[from] = 0;
        x->v[tv].reg = -1;
    } else if (cls) {
        outreg = sgx_xfree(a, at, 1u << from);
        sgx_vrr(a, &sgx_v_movaps, outreg, from);
    } else {
        outreg = sgx_free_reg(a, at, 1u << from);
        sgx_mov_rr(a, outreg, from);
    }
    sgx_cold_jumps(a, at, outreg, cls);
    sgx_cold_room(a, 1024);
    {
        SgXBuf* hot = a->b;
        size_t back = hot->n;
        a->b = &a->cold;
        c = a->b;
        if ((i->zacc || i->cfpcr) && x->base != 3) sg_fail("a cold call's state field off RBX", x->base, 3);
        if (i->zacc & 0x80) {
            /* a zero result that is exact: back to the hot code without the call */
            size_t to_call[8];
            int ncall = 0, nok = 0, j;
            size_t to_ok[5];
            sgx_byte(c, 0x50); /* push rax */
            sgx_cold_zf(a, -1, outreg, cls, i->cw32 >> 7 & 1);
            sgx_byte(c, 0x75), sgx_byte(c, 0), to_call[ncall++] = c->n; /* jnz call */
            sgx_byte(c, 0xF7), sgx_byte(c, 0x83), sgx_imm(c, i->disp, 4), sgx_imm(c, (long long)i->kv[0], 4); /* test */
            sgx_byte(c, 0x75), sgx_byte(c, 0), to_call[ncall++] = c->n;
            if (i->zacc & 0x40) {
                /* under FZ a denormal argument is flushed, raising IDC: the call's. Its doubled magnitude is not zero
                   and its exponent is (SHR of it) */
                const int w32 = (i->imm & 0xFF) == 32;
                size_t no_fz;
                sgx_byte(c, 0xF7), sgx_byte(c, 0x83), sgx_imm(c, i->disp, 4), sgx_imm(c, 1ll << 24, 4); /* test FZ */
                sgx_byte(c, 0x74), sgx_byte(c, 0), no_fz = c->n;                                        /* jz */
                for (j = 0; j + 1 < i->cfpcr; ++j) {
                    size_t zero;
                    sgx_cold_zf(a, i->in[1 + j], 0, 0, w32);
                    sgx_byte(c, 0x74), sgx_byte(c, 0), zero = c->n; /* jz: a zero */
                    if (!w32) sgx_byte(c, 0x48);
                    sgx_byte(c, 0xC1), sgx_byte(c, 0xE8), sgx_byte(c, w32 ? 24 : 53); /* shr eax or rax: the exponent */
                    sgx_byte(c, 0x74), sgx_byte(c, 0), to_call[ncall++] = c->n;       /* jz call: a denormal */
                    c->p[zero - 1] = (unsigned char)(c->n - zero);
                }
                c->p[no_fz - 1] = (unsigned char)(c->n - no_fz);
            }
            if (i->zacc & 7) {
                for (j = 0; j < 3; ++j)
                    if (i->zacc >> j & 1) {
                        sgx_cold_zf(a, i->in[1 + j], 0, 0, i->cw32 >> j & 1);
                        sgx_byte(c, 0x74), sgx_byte(c, 0), to_ok[nok++] = c->n; /* jz ok */
                    }
                sgx_byte(c, 0xEB), sgx_byte(c, 0), to_call[ncall++] = c->n; /* jmp call */
            }
            for (j = 0; j < nok; ++j) c->p[to_ok[j] - 1] = (unsigned char)(c->n - to_ok[j]);
            sgx_byte(c, 0x58); /* ok: pop rax, back */
            sgx_byte(c, 0xE9);
            sgx_cjump(a, c->n, back, 1);
            sgx_imm(c, 0, 4);
            for (j = 0; j < ncall; ++j) c->p[to_call[j] - 1] = (unsigned char)(c->n - to_call[j]);
            sgx_byte(c, 0x58); /* call: pop rax */
        }
        /* the caller-saved general-purpose registers read after at, or an argument's */
        for (k = 0; k < 16; ++k) {
            int o = a->owner[k], keep = 0, j;
            if (o <= 0 || k == (cls ? -1 : outreg) || !((SGX_CALLER_SAVED >> k) & 1)) continue;
            keep = x->v[o].last > at;
            for (j = 0; j < nargs; ++j) keep |= i->in[1 + j] == o;
            if (!keep) continue;
            gslot[k] = a->slots + n++;
            saved_g[k] = 1;
            sgx_mov_mem(a, 0, 64, k, SGX_RSP, 8 * gslot[k]);
        }
        for (k = 0; k < 16; ++k) {
            int o = a->xowner[k];
            if (o <= 0 || (cls && k == outreg) || x->v[o].last <= at) continue;
            if (n % 2) ++n;
            xslot[k] = a->slots + n;
            n += 2;
            saved_x[k] = 1;
            sgx_vmem(a, &sgx_v_movdqu_st, k, SGX_RSP, 8 * xslot[k]);
        }
        if (a->slots + n > a->slots_max) a->slots_max = a->slots + n;
        if (i->vflags == 2) {
            /* the vector form: each XMM argument's 16 bytes in memory (its slot where it was spilled), the result's
               16 bytes, their addresses in RSI (the result), RDX, RCX, R8, desc in R9D */
            int outslot, aslot[3];
            if (n % 2) ++n;
            outslot = a->slots + n;
            n += 2;
            for (k = 0; k < 3; ++k) {
                SgXV* vv = &x->v[i->in[1 + k]];
                if (vv->reg >= 0) {
                    aslot[k] = a->slots + n;
                    n += 2;
                    sgx_vmem(a, &sgx_v_movdqu_st, vv->reg, SGX_RSP, 8 * aslot[k]);
                } else
                    aslot[k] = vv->slot;
            }
            if (a->slots + n > a->slots_max) a->slots_max = a->slots + n;
            {
                static const signed char lea_to[4] = {6, 2, 1, 8};
                for (k = 0; k < 4; ++k) {
                    int dst = lea_to[k], at8 = 8 * (k ? aslot[k - 1] : outslot);
                    sgx_byte(c, (unsigned char)(0x48 | (dst >> 3) << 2)); /* lea dst, [rsp + at8] */
                    sgx_byte(c, 0x8D);
                    sgx_byte(c, (unsigned char)(0x84 | (dst & 7) << 3));
                    sgx_byte(c, 0x24);
                    sgx_imm(c, at8, 4);
                }
                sgx_byte(c, 0x41); /* mov r9d, desc */
                sgx_byte(c, 0xB9);
                sgx_imm(c, i->imm, 4);
            }
            nargs = 0;
            argslot[0] = outslot;
        }
        /* the arguments: from a saved register's slot, an XMM register, or the value's own slot */
        for (k = 0; k < nargs; ++k) {
            int v = i->in[1 + k], dst = args_reg[k];
            SgXV* vv = &x->v[v];
            argslot[k] = -1;
            if (k + 1 == i->cfpcr) {
                /* mov dst32, [rbx + disp] */
                if (dst >= 8) sgx_byte(c, 0x44);
                sgx_byte(c, 0x8B);
                sgx_byte(c, (unsigned char)(0x80 | (dst & 7) << 3 | 3));
                sgx_imm(c, i->disp, 4);
            } else if (k + 1 == i->ckat) {
                /* mov dst32, imm32, or where the constant has bits past 32 (an AArch32 helper's param) mov dst, imm64 */
                int wide = ((SnxU64)i->imm >> 32) != 0;
                if (dst >= 8 || wide) sgx_byte(c, (unsigned char)(0x40 | (wide ? 8 : 0) | (dst >= 8)));
                sgx_byte(c, (unsigned char)(0xB8 | (dst & 7)));
                sgx_imm(c, i->imm, wide ? 8 : 4);
            } else if (vv->cls) {
                if (vv->reg >= 0) {
                    const SgXVForm* f = i->cw32 >> k & 1 ? &sgx_v_movd_rx : &sgx_v_movq_rx;
                    unsigned char r[4] = {0, 0, 0, 0};
                    r[(int)f->rm] = (unsigned char)dst;
                    r[(int)f->reg] = (unsigned char)vv->reg;
                    f->enc(c, r, 0, 0);
                } else
                    sgx_mov_mem(a, 1, i->cw32 >> k & 1 ? 32 : 64, dst, SGX_RSP, 8 * vv->slot);
            } else if (vv->reg >= 0 && saved_g[vv->reg])
                sgx_mov_mem(a, 1, 64, dst, SGX_RSP, 8 * gslot[(int)vv->reg]);
            else if (vv->reg >= 0)
                sgx_mov_rr(a, dst, vv->reg); /* a callee-saved one */
            else if (vv->xmm >= 0)
                sgx_movq(c, 0, vv->xmm, dst);
            else
                sgx_mov_mem(a, 1, 64, dst, SGX_RSP, 8 * vv->slot);
        }
        if (a->pin_cs) a->pin_cs(a->machine, 1); /* the caller-saved pins, which the call changes, to the state */
        sgx_byte(c, 0x48); /* mov rdi, rbx */
        sgx_byte(c, 0x89);
        sgx_byte(c, 0xDF);
        if (a->reloc) {
            sgx_byte(c, 0xE8);
            if (a->nccalls == a->cap_ccalls) {
                a->cap_ccalls = a->cap_ccalls ? 2 * a->cap_ccalls : 32;
                a->ccalls = (struct SgXCCall*)realloc(a->ccalls, (size_t)a->cap_ccalls * sizeof *a->ccalls);
                if (!a->ccalls) sg_fail("out of memory", 0, 0);
            }
            a->ccalls[a->nccalls].at = c->n;
            a->ccalls[a->nccalls++].sym = i->sym;
            sgx_imm(c, 0, 4);
        } else {
            SnxU64 to = a->resolve ? a->resolve(i->sym) : 0;
            if (!to) sg_fail("a helper nothing resolves", 0, 0);
            sgx_byte(c, 0x48); /* mov rax, imm64; call rax */
            sgx_byte(c, 0xB8);
            sgx_imm(c, (long long)to, 8);
            sgx_byte(c, 0xFF);
            sgx_byte(c, 0xD0);
        }
        /* the pins back before the result, whose register may be one of them (sgx_pin_target's) */
        if (a->pin_cs) a->pin_cs(a->machine, 0);
        if (i->vflags == 2)
            sgx_vmem(a, &sgx_v_movdqu_ld, outreg, SGX_RSP, 8 * argslot[0]);
        else if (cls) {
            unsigned char r[4] = {0, 0, 0, 0};
            r[(int)sgx_v_movq_xr.reg] = (unsigned char)outreg;
            r[(int)sgx_v_movq_xr.rm] = 0; /* RAX */
            sgx_v_movq_xr.enc(c, r, 0, 0);
        } else
            sgx_mov_rr(a, outreg, 0);
        for (k = 0; k < 16; ++k) {
            if (saved_g[k] && x->v[a->owner[k]].last > at) sgx_mov_mem(a, 1, 64, k, SGX_RSP, 8 * gslot[k]);
            if (saved_x[k]) sgx_vmem(a, &sgx_v_movdqu_ld, k, SGX_RSP, 8 * xslot[k]);
        }
        sgx_byte(c, 0xE9); /* jmp back */
        sgx_cjump(a, c->n, back, 1);
        sgx_imm(c, 0, 4);
        a->b = hot;
    }
    if (cls) a->xowner[outreg] = i->def;
    else
        a->owner[outreg] = i->def;
    x->v[i->def].reg = (signed char)outreg;
}

/* the cold paths after the code (and the jumps between, and their calls' relocations); emptied for the next */
static inline void sgx_cold_emit(SgXAsm* a) {
    SgXBuf* b = a->b;
    size_t base = b->n, k;
    for (k = 0; k < a->cold.n; ++k) sgx_byte(b, a->cold.p[k]);
    for (k = 0; k < (size_t)a->ncjumps; ++k) {
        size_t at = a->cjumps[k].in_cold ? base + a->cjumps[k].at : a->cjumps[k].at;
        size_t to = a->cjumps[k].in_cold ? a->cjumps[k].to : base + a->cjumps[k].to;
        long long d = (long long)to - (long long)(at + 4);
        if (at + 4 <= b->cap) {
            b->p[at] = (unsigned char)d;
            b->p[at + 1] = (unsigned char)(d >> 8);
            b->p[at + 2] = (unsigned char)(d >> 16);
            b->p[at + 3] = (unsigned char)(d >> 24);
        }
    }
    for (k = 0; k < (size_t)a->nccalls; ++k) a->reloc(a->machine, base + a->ccalls[k].at, a->ccalls[k].sym, -4);
    a->cold.n = 0;
    a->ncjumps = a->nccalls = 0;
}


/* the frame's size, patched into the prologue and every epilogue once the function is done: its slots, 16-aligned
   (RSP is 16-aligned after the prologue's push of RBX, for calls) */
static inline void sgx_patch_frame(SgXAsm* a) {
    int k, size = (a->slots_max * 8 + 15) & ~15;
    for (k = 0; k < a->nframe; ++k) {
        size_t at = a->frame_at[k];
        if (at + 4 <= a->b->cap) {
            a->b->p[at] = (unsigned char)size;
            a->b->p[at + 1] = (unsigned char)(size >> 8);
            a->b->p[at + 2] = (unsigned char)(size >> 16);
            a->b->p[at + 3] = (unsigned char)(size >> 24);
        }
    }
}

/* ---- mach.h's hooks: x86-64's ---- */

static inline void sgx_spill_x(SgXAsm* a, int to_x, int xr, int gr) { sgx_movq(a->b, to_x, xr, gr); }
/* MOVZX r32, byte [base + disp32]: a flag a SETcc stored (a wider load wouldn't forward from it) */
static inline void sgx_load8(SgXAsm* a, int d, int base, int disp) {
    SgXBuf* b = a->b;
    if (d >= 8 || base >= 8) sgx_byte(b, (unsigned char)(0x40 | (d >> 3) << 2 | (base >> 3)));
    sgx_byte(b, 0x0F);
    sgx_byte(b, 0xB6);
    if ((base & 7) == 4) {
        sgx_byte(b, (unsigned char)(0x84 | (d & 7) << 3));
        sgx_byte(b, 0x24);
    } else
        sgx_byte(b, (unsigned char)(0x80 | (d & 7) << 3 | (base & 7)));
    sgx_imm(b, disp, 4);
}
/* SETcc on the field's low byte: the others are 0 already, the field being 0 or 1 */
static inline void sgx_store_flag(SgXAsm* a, int at, int base, int disp, int cc) {
    const SgXSel* m = &sgx_sel_cset_32[0];
    unsigned char r[5] = {0, 0, 0, 0, 0};
    (void)at;
    r[(int)m->out] = (unsigned char)SGX_MEMR(base);
    a->b->disp = disp;
    a->b->index = -1;
    m->enc(a->b, r, 0, cc);
}
static inline void sgx_xmov(SgXAsm* a, int d, int s) { sgx_vrr(a, &sgx_v_movaps, d, s); }
/* MOVDQU and MOVQ of the frame's slots */
static inline void sgx_xmem(SgXAsm* a, int store, int bytes, int xr, int base, int disp) {
    sgx_vmem(a,
             bytes == 16 ? (store ? &sgx_v_movdqu_st : &sgx_v_movdqu_ld) : (store ? &sgx_v_movq_st : &sgx_v_movq_ld),
             xr, base, disp);
}
/* the primitives' immediates are 32 bits, sign extended (the SDM's imm32 and imm8 forms) */
static inline int sgx_imm_ok(SgX* x, int kind, SnxU64 k, unsigned w) {
    (void)x;
    (void)kind;
    return sgx_fits(&(SgXSel){.fit_bits = 32, .fit_kind = 'S'}, k, w);
}
/* PSHUFD: the doubleword at lo (or the quadword at 64) to the bottom */
static inline int sgx_xdown(SgX* x, int v, unsigned lo) {
    unsigned e = lo / 32, imm = lo == 64 ? 0xEE : e * 0x55;
    return sgx_v1(x, &sgx_v_pshufd, v, imm);
}
/* PUNPCKLQDQ: the low's high bits the high's low ones */
static inline int sgx_xconcat(SgX* x, int lo, int hi) { return sgx_v2(x, &sgx_v_punpcklqdq, lo, hi, 0); }
/* an XMM value's bits w and up, to 128, made 0: MOVQ for a doubleword, INSERTPS for a word, byte shifts below */
static inline int sgx_xclear(SgX* x, int v, unsigned w) {
    if (w >= 128) return v;
    if (w == 64) return sgx_v1(x, &sgx_v_movq_ld, v, 0);
    if (w == 32 && (x->features & SGX_F_SSE41)) {
        int in[4] = {v, v, 0, 0};
        return sgx_vins(x, &sgx_v_insertps, 0, 1, in, 0x0E, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
    if (w % 8 || w > 64) sg_fail("an XMM value cleared above a width not of bytes", w, 0);
    {
        int in[4] = {v, 0, 0, 0}, t;
        t = sgx_vins(x, &sgx_v_pslldq, 0, 1, in, 16 - w / 8, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        in[0] = t;
        return sgx_vins(x, &sgx_v_psrldq, 0, 1, in, 16 - w / 8, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    }
}
