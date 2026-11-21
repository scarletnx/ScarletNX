/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's x86-64 back end, A64's part (after x64.h): the Sail's registers where cpu.h's CPU keeps them (x[32]: X0
   to X30 and SP; n, z, cf, vf: PSTATE.NZCV, each a 32-bit 0 or 1), its address in RBX for the instruction's code.
   A register read once is the block's value of it after (a load, or what was written: every write is stored at once,
   and the stores a later one overwrites are dropped as the block ends). NZCV written from an instruction's flags is
   four SETcc of them (the selection's map), and ConditionHolds on flags that instruction made, while the x86 flags
   still are them, the condition code the selection gave Arm's condition on its form.

   What isn't an instruction the selection chose is the ABI's, written as bytes here: the prologue (the state's
   address from RDI into RBX, kept as RBX is the callee's), the runtime's UNDEF (cpu.h's RtAPI.undef(state, pc, word))
   and the return (the next pc in RAX) */

#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

struct SgCtx {
    SgX x;
    SnxU32 word;
    SnxU64 pc;
    SnxU64 features; /* the guest's (SNX_A64_FEAT_*) */
    int branched, undefined;
    SgD target; /* BranchTo's: where, and when (0: always) */
    SgB taken;
    int gpr[32];     /* the virtual register of each X register the block knows, 0 for none */
    int origin;      /* the instruction whose flags NZCV was last written from, -1 for none */
    int start;       /* the instructions before this word's */
};

static inline SgX* sgx(SgCtx* g) { return &g->x; }

/* a guest's feature, a64_decode.h's SNX_A64_FEAT_X (the decoder's profile) */
#define SG_FEAT(x) SNX_A64_##x
static inline int sg_feature(SgCtx* g, SnxU64 feature) { return (g->features & feature) != 0; }

static inline void sg_undefined(SgCtx* g) {
    SgXIns* i = sgx_append(sgx(g), SGX_K_UNDEF);
    i->imm = (long long)g->pc;
    i->disp = (int)g->word;
    g->undefined = 1;
}
static inline int sg_see(SgCtx* g) {
    if (g->x.n != g->start || g->undefined) sg_fail("a word handed on to the next clause after its own code", g->word, 0);
    return 0;
}
/* BTI's next branch type: no guest of the NX's runs with branch targets guarded */
static inline void sg_BTypeNext_write(SgCtx* g, SgD v) {
    (void)g;
    (void)v;
}

static inline SgD sg_GPR_read(SgCtx* g, long long n) {
    SgX* x = sgx(g);
    if (n < 0 || n > 31) sg_fail("a GPR this back end does not keep", n, 31);
    if (!g->gpr[n]) {
        SgXIns* i = sgx_append(x, SGX_K_LOAD);
        i->w = 64;
        i->disp = (int)(offsetof(CPU, x) + 8 * (size_t)n);
        i->def = sgx_vreg(x, x->n - 1, 64);
        g->gpr[n] = i->def;
    }
    return sgx_val(64, g->gpr[n]);
}
static inline void sg_GPR_write(SgCtx* g, long long n, SgD v) {
    SgX* x = sgx(g);
    SgXIns* i;
    int r;
    if (n < 0 || n > 31) sg_fail("a GPR this back end does not keep", n, 31);
    if (v.w != 64) sg_fail("a GPR written from other than 64 bits", v.w, 64);
    r = sgx_reg(x, &v);
    i = sgx_append(x, SGX_K_STORE);
    i->w = 64;
    i->in[0] = r;
    i->disp = (int)(offsetof(CPU, x) + 8 * (size_t)n);
    g->gpr[n] = r;
}

/* PSTATE's fields (0 N, 1 Z, 2 C, 3 V): cpu.h's n, z, cf and vf */
enum { SG_PSTATE_N, SG_PSTATE_Z, SG_PSTATE_C, SG_PSTATE_V };
static inline int sgx_flag_offset(long long field) {
    switch (field) {
    case 0: return (int)offsetof(CPU, n);
    case 1: return (int)offsetof(CPU, z);
    case 2: return (int)offsetof(CPU, cf);
    default: return (int)offsetof(CPU, vf);
    }
}
static inline SgD sg_PSTATE_read(SgCtx* g, long long field) {
    SgX* x = sgx(g);
    SgXIns* i;
    if (field < 0 || field > 3) sg_fail("a field of PSTATE this back end does not keep", field, 3);
    i = sgx_append(x, SGX_K_LOAD);
    i->w = 32;
    i->disp = sgx_flag_offset(field);
    i->def = sgx_vreg(x, x->n - 1, 1);
    return sgx_val(1, i->def);
}
static inline void sg_PSTATE_write(SgCtx* g, long long first, long long count, SgD v) {
    SgX* x = sgx(g);
    long long f;
    if (first < 0 || count < 1 || first + count > 4) sg_fail("fields of PSTATE this back end does not keep", first, count);
    if (v.w != (unsigned)count) sg_fail("flags written from a value of another width", count, v.w);
    if (count == 4 && v.nzcv) {
        /* the four from the flags of the instruction that made them, each the condition code that is that flag */
        int from = v.nzcv - 1;
        const SgXSel* fs = x->ins[from].fsel;
        if (x->flags_ins != from) sg_fail("NZCV written after the x86 flags that are them were overwritten", from, 0);
        for (f = 0; f < 4; ++f) {
            int cc = sgx_arm_flag_cc(fs, (int)f);
            SgXIns* i;
            if (cc < 0) {
                /* a flag the form gives as a constant (ANDS's C and V): stored as it */
                SgD k = sgx_const(32, 0);
                SgXIns* st;
                int r = sgx_reg(x, &k);
                st = sgx_append(x, SGX_K_STORE);
                st->w = 32;
                st->in[0] = r;
                st->disp = sgx_flag_offset(f);
                continue;
            }
            i = sgx_append(x, SGX_K_STORE_FLAG);
            i->cc = (unsigned char)cc;
            i->flags_from = from;
            i->disp = sgx_flag_offset(f);
        }
        g->origin = from;
        return;
    }
    for (f = 0; f < count; ++f) {
        SgD bit = sg_d_zext(g, sg_d_slice(g, v, count - 1 - f, count - 1 - f), 32);
        SgXIns* st;
        int r = sgx_reg(x, &bit);
        st = sgx_append(x, SGX_K_STORE);
        st->w = 32;
        st->in[0] = r;
        st->disp = sgx_flag_offset(first + f);
    }
    g->origin = -1;
}

/* ConditionHolds(cond) on the flags NZCV was last written from, while the x86 flags are still them: the condition
   code the selection gave Arm's cond on that form's flags. 0 otherwise (the Sail then reads PSTATE) */
static inline SgB sg_condition_origin(SgCtx* g, SgS cond) {
    SgX* x = sgx(g);
    int cc;
    if (g->origin < 0 || x->flags_ins != g->origin || cond.v >= 14) return 0;
    cc = x->ins[g->origin].fsel->cc_of[cond.v];
    if (cc < 0) return 0;
    return sgx_bool(x, SGX_B_CC, g->origin, cc);
}

/* BranchTo(target, ...): where the instruction goes, under the run-time condition when (0: always) */
static inline void sg_branch_to(SgCtx* g, SgD target, SgB when) {
    if (g->branched) sg_fail("an instruction that branches twice", (long long)g->word, 0);
    if (target.w != 64) sg_fail("a branch to a target of other than 64 bits", target.w, 64);
    g->branched = 1;
    g->target = target;
    g->taken = when;
}

/* ---- the ABI ---- */

static inline void sgx_machine_prologue(SgXBuf* b) {
    sgx_byte(b, 0x53); /* push rbx */
    sgx_byte(b, 0x48); /* mov rbx, rdi */
    sgx_byte(b, 0x89);
    sgx_byte(b, 0xFB);
}
static inline void sgx_machine_return(SgXBuf* b) {
    sgx_byte(b, 0x5B); /* pop rbx */
    sgx_byte(b, 0xC3); /* ret */
}
/* rt->undef(state, pc, word): RDI, RSI, EDX, through RAX = state->rt (the stack 16-aligned after the push) */
static inline void sgx_machine_undef(SgXBuf* b, const SgXIns* i) {
    sgx_byte(b, 0x48); /* mov rdi, rbx */
    sgx_byte(b, 0x89);
    sgx_byte(b, 0xDF);
    sgx_byte(b, 0x48); /* mov rsi, imm64 */
    sgx_byte(b, 0xBE);
    sgx_imm(b, i->imm, 8);
    sgx_byte(b, 0xBA); /* mov edx, imm32 */
    sgx_imm(b, i->disp, 4);
    sgx_byte(b, 0x48); /* mov rax, [rbx + rt] */
    sgx_byte(b, 0x8B);
    sgx_byte(b, 0x83);
    sgx_imm(b, (long long)offsetof(CPU, rt), 4);
    sgx_byte(b, 0xFF); /* call [rax + undef] */
    sgx_byte(b, 0x90);
    sgx_imm(b, (long long)offsetof(RtAPI, undef), 4);
}

/* the dispatcher sailgen writes after this */
static int sg_decode(SgCtx* g);

/* one word's code as a step, u64 step(CPU*) returning the next pc (the word's own after UNDEF): its size in code, or
   0 when no clause takes the word, a primitive it needs has no form this host runs, or it needs more than cap bytes */
static size_t sg_x64_step(SgCtx* g, SnxU32 word, SnxU64 pc, SnxU64 features, unsigned host_features,
                          unsigned char* code, size_t cap) {
    SgXBuf b;
    SgXIns* r;
    int k;
    memset(g, 0, sizeof *g);
    g->word = word;
    g->pc = pc;
    g->features = features;
    g->origin = -1;
    g->x.features = host_features;
    g->x.flags_ins = -1;
    g->x.base = 3; /* RBX */
    g->x.nv = 1;   /* 0 is none */
    g->x.nb = 1;
    for (k = 0; k < 32; ++k) g->gpr[k] = 0;
    if (!sg_decode(g) || g->x.unmade) return 0;
    if (g->branched && !g->undefined) {
        /* the target, or where it isn't taken the next word: CMOVcc on the condition */
        SgD next = g->taken ? sg_d_select(g, g->taken, g->target, sgx_const(64, pc + 4)) : g->target;
        int v = sgx_reg(&g->x, &next);
        r = sgx_append(&g->x, SGX_K_RET);
        r->in[0] = v;
    } else {
        r = sgx_append(&g->x, SGX_K_RET);
        r->imm = (long long)(g->undefined ? pc : pc + 4);
    }
    b.p = code;
    b.n = 0;
    b.cap = cap;
    b.disp = 0;
    sgx_assemble(&g->x, &b);
    return b.n <= cap ? b.n : 0;
}
