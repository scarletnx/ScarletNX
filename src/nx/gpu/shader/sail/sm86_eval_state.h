/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SM86's state for sailgen --gpu's evaluating back end (after tools/sailgen/eval.h): sm86.sail's vals on the values of
   sm86_eval's state (sm86.h's Sm86EvalState), for the tests that hold the Sail to sm86_eval and each fusion to its
   instructions (tests/shader_sail_sm86.c). The operands are read as sm86_translate.c's src_bits reads them */

struct SgG {
    SgEval ev;
    Sm86EvalState* st;
    const Sm86Insn* in;
    const Sm86EvalIo* io;
    int uniform;
};
static SgEval* sg_eval(SgG* g) { return &g->ev; }
[[maybe_unused]] static void sg86_begin(SgG* g, Sm86EvalState* st, const Sm86Insn* in, const Sm86EvalIo* io) {
    g->ev.undefined = g->ev.thrown = 0;
    g->st = st;
    g->in = in;
    g->io = io;
    g->uniform = sm86_ops[in->op].kind == SM86_K_UALU;
}

[[maybe_unused]] static uint64_t sg_Field(SgG* g, int64_t lo, int64_t n) { return sm86_field(g->in, (u32)lo, (u32)n); }
[[maybe_unused]] static int sg_Uniform(SgG* g) { return g->uniform; }
[[maybe_unused]] static int64_t sg_Dst(SgG* g) { return g->in->dst; }
[[maybe_unused]] static const Sm86Src* sg86_src(SgG* g, int64_t s) {
    const Sm86Insn* in = g->in;
    switch (s) {
        case SGE_A: return &in->a;
        case SGE_B: return &in->b;
        case SGE_C: return &in->c;
        default: return in->form == 2 || in->form == 3 || in->form == 7 ? &in->c : &in->b;
    }
}
[[maybe_unused]] static SgV sg86_bits(SgG* g, const Sm86Src* x);
[[maybe_unused]] static SgV sg_SrcBits(SgG* g, int64_t s) { return sg86_bits(g, sg86_src(g, s)); }
[[maybe_unused]] static SgV sg86_bits(SgG* g, const Sm86Src* x) {
    switch (x->kind) {
        case SM86_SRC_REG: return sg_ev(x->reg < SM86_RZ ? g->st->r[x->reg] : 0);
        case SM86_SRC_UREG: return sg_ev(x->reg < SM86_URZ ? g->st->ur[x->reg] : 0);
        case SM86_SRC_IMM: return sg_ev(x->value);
        case SM86_SRC_CBUF:
            if (x->bindless) {
                g->ev.thrown = 1;
                return sg_ev(0);
            }
            return sg_ev(g->io->cbuf(g->io->user, x->bank, x->value));
        default: return sg_ev(0);
    }
}
[[maybe_unused]] static int sg_SrcNeg(SgG* g, int64_t s) {
    const Sm86Src* x = sg86_src(g, s);
    return x->neg && x->kind != SM86_SRC_IMM;
}
[[maybe_unused]] static void sg_D_write(SgG* g, SgV v) {
    if (g->uniform) {
        if (g->in->dst < SM86_URZ) g->st->ur[g->in->dst] = (u32)v.v;
    } else if (g->in->dst < SM86_RZ)
        g->st->r[g->in->dst] = (u32)v.v;
}
[[maybe_unused]] static SgV sg_R_read(SgG* g, int64_t r) { return sg_ev(r < SM86_RZ ? g->st->r[r] : 0); }
[[maybe_unused]] static void sg_R_write(SgG* g, int64_t r, SgV v) {
    if (r < SM86_RZ) g->st->r[r] = (u32)v.v;
}
[[maybe_unused]] static SgV sg_PredRead(SgG* g, int64_t p, int64_t negate, int64_t u) {
    int v = p >= SM86_PT ? 1 : (u ? g->st->up : g->st->p)[p];
    return sg_ev((uint64_t)(negate ? !v : v));
}
[[maybe_unused]] static void sg_PredWrite(SgG* g, int64_t p, SgV v, int64_t u) {
    if (p < SM86_PT) (u ? g->st->up : g->st->p)[p] = (u8)(v.v != 0);
}
[[maybe_unused]] static void sg_throw_Unsupported(SgG* g, const char* why) {
    (void)why;
    g->ev.thrown = 1;
}
