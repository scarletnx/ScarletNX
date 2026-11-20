/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SM86's state for sailgen --gpu (after tools/sailgen/spirv.h): sm86.sail's vals over sm86_translate.c's accessors,
   which the file sailgen writes is compiled with (sm86_translate.c includes it after them). An instruction's
   context is the translator's and the instruction; sg86_begin makes it */

struct SgG {
    SgSpv spv;
    Ctx* c;
    const Sm86Insn* in;
    const Sm86Insn* at[4]; /* a fusion's instructions, at[0] the one being translated */
    int uniform;
};
static SgSpv* sg_spv(SgG* g) { return &g->spv; }
[[maybe_unused]] static uint32_t sg86_t_u64(SgG* g) { return u64_type(g->c); }
[[maybe_unused]] static void sg86_begin(SgG* g, Ctx* c, const Sm86Insn* in) {
    g->c = c;
    g->in = in;
    g->at[0] = in;
    g->uniform = sm86_ops[in->op].kind == SM86_K_UALU;
    g->spv.e = &c->e;
    g->spv.t_u32 = c->t_u32;
    g->spv.t_i32 = c->t_i32;
    g->spv.t_f32 = c->t_f32;
    g->spv.t_bool = c->t_bool;
    g->spv.glsl = c->glsl;
    g->spv.true_v = c->true_v;
    g->spv.false_v = c->false_v;
    g->spv.t_u64 = sg86_t_u64;
    g->spv.ncast = 0;
}

[[maybe_unused]] static uint64_t sg_Field(SgG* g, int64_t lo, int64_t n) { return sm86_field(g->in, (u32)lo, (u32)n); }
[[maybe_unused]] static int sg_Uniform(SgG* g) { return g->uniform; }
[[maybe_unused]] static int64_t sg_Dst(SgG* g) { return g->in->dst; }
[[maybe_unused]] static const Sm86Src* sg86_src(SgG* g, int64_t s) {
    switch (s) {
        case SGE_A: return &g->in->a;
        case SGE_B: return &g->in->b;
        case SGE_C: return &g->in->c;
        default: return second(g->in);
    }
}
[[maybe_unused]] static int sg86_zero(const Sm86Src* x) {
    return (x->kind == SM86_SRC_REG && x->reg >= SM86_RZ) || (x->kind == SM86_SRC_UREG && x->reg >= SM86_URZ) ||
           (x->kind == SM86_SRC_IMM && x->value == 0);
}
[[maybe_unused]] static int sg_IsZero(SgG* g, int64_t s) { return sg86_zero(sg86_src(g, s)); }
[[maybe_unused]] static int sg_SrcIsImm(SgG* g, int64_t s) { return sg86_src(g, s)->kind == SM86_SRC_IMM; }
[[maybe_unused]] static int64_t sg_SrcImm(SgG* g, int64_t s) { return sg86_src(g, s)->value; }
/* a fusion's instruction k (sg86_fuse's), its operands as the first's are read */
[[maybe_unused]] static const Sm86Src* sg86_src_at(SgG* g, int64_t k, int64_t s) {
    const Sm86Insn* in = g->at[k];
    switch (s) {
        case SGE_A: return &in->a;
        case SGE_B: return &in->b;
        case SGE_C: return &in->c;
        default: return in->form == 2 || in->form == 3 || in->form == 7 ? &in->c : &in->b;
    }
}
[[maybe_unused]] static uint64_t sg_FieldAt(SgG* g, int64_t k, int64_t lo, int64_t n) {
    return sm86_field(g->at[k], (u32)lo, (u32)n);
}
[[maybe_unused]] static int sg_SrcNegAt(SgG* g, int64_t k, int64_t s) {
    const Sm86Src* x = sg86_src_at(g, k, s);
    return x->neg && x->kind != SM86_SRC_IMM;
}
[[maybe_unused]] static int sg_IsZeroAt(SgG* g, int64_t k, int64_t s) { return sg86_zero(sg86_src_at(g, k, s)); }
/* the operand of instruction k is a register (a uniform one for a uniform instruction) that instruction j writes */
[[maybe_unused]] static int sg_ReadsDstAt(SgG* g, int64_t k, int64_t s, int64_t j) {
    const Sm86Src* x = sg86_src_at(g, k, s);
    const Sm86Insn* w = g->at[j];
    int uniform = sm86_ops[w->op].kind == SM86_K_UALU;
    if (uniform ? x->kind != SM86_SRC_UREG || x->reg >= SM86_URZ : x->kind != SM86_SRC_REG || x->reg >= SM86_RZ)
        return 0;
    return x->reg == w->dst;
}
[[maybe_unused]] static SgV sg_SrcBits(SgG* g, int64_t s) { return sg_v(src_bits(g->c, sg86_src(g, s)), SGT_U32); }
[[maybe_unused]] static int sg_SrcNeg(SgG* g, int64_t s) {
    const Sm86Src* x = sg86_src(g, s);
    return x->neg && x->kind != SM86_SRC_IMM;
}
[[maybe_unused]] static void sg_D_write(SgG* g, SgV v) { dwr(g->c, g->in, sg_u32(g, v)); }
[[maybe_unused]] static SgV sg_R_read(SgG* g, int64_t r) { return sg_v(rd(g->c, (u32)r), SGT_U32); }
[[maybe_unused]] static void sg_R_write(SgG* g, int64_t r, SgV v) { wr(g->c, (u32)r, sg_u32(g, v)); }
[[maybe_unused]] static SgV sg_PredRead(SgG* g, int64_t p, int64_t negate, int64_t u) {
    return sg_v(prd(g->c, (u32)p, (u32)negate, (int)u), SGT_BOOL);
}
[[maybe_unused]] static void sg_PredWrite(SgG* g, int64_t p, SgV v, int64_t u) { pwr(g->c, (u32)p, v.id, (int)u); }
[[maybe_unused]] static void sg_throw_Unsupported(SgG* g, const char* why) { fail(g->c, "%s", why); }
[[maybe_unused]] static SgV sg_SrcBitsAt(SgG* g, int64_t k, int64_t s) {
    return sg_v(src_bits(g->c, sg86_src_at(g, k, s)), SGT_U32);
}
[[maybe_unused]] static void sg_D_writeAt(SgG* g, int64_t k, SgV v) { dwr(g->c, g->at[k], sg_u32(g, v)); }
