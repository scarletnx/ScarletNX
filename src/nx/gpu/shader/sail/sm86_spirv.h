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
[[maybe_unused]] static void sg_throw_Unsupported(SgG* g, const char* f, ...) {
    char why[sizeof g->c->si->first_unsupported];
    va_list ap;
    va_start(ap, f);
    vsnprintf(why, sizeof why, f, ap);
    va_end(ap);
    fail(g->c, "%s", why);
}
[[maybe_unused]] static SgV sg_SrcBitsAt(SgG* g, int64_t k, int64_t s) {
    return sg_v(src_bits(g->c, sg86_src_at(g, k, s)), SGT_U32);
}
[[maybe_unused]] static void sg_D_writeAt(SgG* g, int64_t k, SgV v) { dwr(g->c, g->at[k], sg_u32(g, v)); }

/* ---- floating point: the translator's helpers, which keep its FTZ, rounding and perspective folds ---- */
[[maybe_unused]] static int sg_Form(SgG* g) { return g->in->form; }
[[maybe_unused]] static int sg_SrcAbs(SgG* g, int64_t s) {
    const Sm86Src* x = sg86_src(g, s);
    return x->abs && x->kind != SM86_SRC_IMM;
}
[[maybe_unused]] static int sg_SrcAbsRaw(SgG* g, int64_t s) { return sg86_src(g, s)->abs; }
[[maybe_unused]] static int sg_SrcNegRaw(SgG* g, int64_t s) { return sg86_src(g, s)->neg; }
[[maybe_unused]] static int sg_SrcIsReg(SgG* g, int64_t s) { return sg86_src(g, s)->kind == SM86_SRC_REG; }
[[maybe_unused]] static int64_t sg_SrcReg(SgG* g, int64_t s) { return sg86_src(g, s)->reg; }
[[maybe_unused]] static const char* sg_OpName(SgG* g) { return sm86_ops[g->in->op].name; }
[[maybe_unused]] static SgV sg_SrcF(SgG* g, int64_t s) { return sg_fv(src_f(g->c, sg86_src(g, s))); }
[[maybe_unused]] static SgV sg_Ftz(SgG* g, SgV x, int64_t on) { return sg_fv(ftz(g->c, sg_f(g, x), (int)on)); }
[[maybe_unused]] static void sg_NoteFtz(SgG* g, int64_t on) { note_ftz(g->c, (int)on); }
[[maybe_unused]] static SgV sg_FloatAdd(SgG* g, int64_t mode, SgV x, SgV y) {
    uint32_t a = sg_f(g, x), b = sg_f(g, y);
    return sg_fv(float_add(g->c, (u32)mode, a, b));
}
[[maybe_unused]] static SgV sg_FloatMul(SgG* g, int64_t mode, int64_t scale, SgV x, SgV y) {
    uint32_t a = sg_f(g, x), b = sg_f(g, y);
    return sg_fv(float_mul(g->c, (u32)mode, (u32)scale, a, b));
}
[[maybe_unused]] static SgV sg_FloatFma(SgG* g, int64_t mode, SgV x, SgV y, SgV z) {
    uint32_t a = sg_f(g, x), b = sg_f(g, y), d = sg_f(g, z);
    return sg_fv(float_fma(g->c, (u32)mode, a, b, d));
}
[[maybe_unused]] static SgV sg_Saturate(SgG* g, SgV x, int64_t on) { return sg_fv(saturate(g->c, sg_f(g, x), (int)on)); }
[[maybe_unused]] static SgV sg_NoContract(SgG* g, SgV x) { return sg_fv(nocontract(g->c, sg_f(g, x))); }
[[maybe_unused]] static int64_t sg_FmulOverW(SgG* g) { return fmul_over_w(g->c, g->c->cur); }
[[maybe_unused]] static SgV sg_ValueF(SgG* g, int64_t id) { return (void)g, sg_fv((uint32_t)id); }
[[maybe_unused]] static SgV sg_SystemValue(SgG* g, int64_t sr) { return sg_v(system_value(g->c, (u32)sr), SGT_U32); }
[[maybe_unused]] static SgV sg_PackHalf(SgG* g, SgV pair) { return sg_v(pack_half(g->c, pair.id), SGT_U32); }
[[maybe_unused]] static SgV sg_Bf16(SgG* g, SgV x) { return sg_v(bf16(g->c, sg_f(g, x)), SGT_U32); }
[[maybe_unused]] static SgV sg_IntToF32Nearest(SgG* g, SgV x, int64_t is_signed) {
    return sg_fv(spv_emit_int_to_f32_nearest(&g->c->e, sg_u32(g, x), (int)is_signed));
}
[[maybe_unused]] static int sg_Float64(SgG* g) { return (void)g, g_spirv_translate.float64 != 0; }
[[maybe_unused]] static int64_t sg_I2fNearest(SgG* g) { return (void)g, g_spirv_translate.i2f_nearest; }
[[maybe_unused]] static SgV sg_ToF64(SgG* g, SgV x, int64_t is_signed) {
    uint32_t t = f64_type(g->c);
    uint32_t v = is_signed ? spv_emit_unary(&g->c->e, SPV_OP_CONVERT_S_TO_F, t, sg_as(g, x, SGT_I32))
                           : spv_emit_unary(&g->c->e, SPV_OP_CONVERT_U_TO_F, t, sg_u32(g, x));
    return sg_vt(v, t);
}
[[maybe_unused]] static SgV sg_RoundF64(SgG* g, int64_t mode, SgV d) { return sg_fv(round_f64(g->c, (u32)mode, d.id)); }

/* ---- the rest of the integer ALU, the uniform registers, the system registers, the warp ---- */
[[maybe_unused]] static int sg_SrcIsCbuf(SgG* g, int64_t s) { return sg86_src(g, s)->kind == SM86_SRC_CBUF; }
[[maybe_unused]] static SgV sg_SrcNext(SgG* g, int64_t s) {
    const Sm86Src* x = sg86_src(g, s);
    u32 r = x->reg;
    return sg_v(x->kind == SM86_SRC_UREG ? urd(g->c, r >= SM86_URZ ? r : r + 1) : rd(g->c, r >= SM86_RZ ? r : r + 1), SGT_U32);
}
[[maybe_unused]] static int sg_DstHasNext(SgG* g) { return g->in->dst < (g->uniform ? SM86_URZ : SM86_RZ); }
[[maybe_unused]] static void sg_D_writeNext(SgG* g, SgV v) {
    if (g->uniform)
        uwr(g->c, g->in->dst + 1, sg_u32(g, v));
    else
        wr(g->c, g->in->dst + 1, sg_u32(g, v));
}
[[maybe_unused]] static void sg_U_write(SgG* g, int64_t r, SgV v) { uwr(g->c, (u32)r, sg_u32(g, v)); }
[[maybe_unused]] static void sg_Int64(SgG* g) { u64_type(g->c); }
[[maybe_unused]] static SgV sg_ProgramSr(SgG* g, int64_t sr) { return sg_v(program_sr(g->c, (u32)sr), SGT_U32); }
[[maybe_unused]] static SgV sg_Ballot32(SgG* g, SgV p) { return sg_v(ballot32(g->c, p.id), SGT_U32); }
[[maybe_unused]] static int64_t sg_InsnIndex(SgG* g) { return g->c->cur; }
[[maybe_unused]] static int64_t sg_Stage(SgG* g) { return g->c->stage; }
[[maybe_unused]] static int sg_Passthrough(SgG* g) { return g->c->passthrough; }
[[maybe_unused]] static void sg_GsEmit(SgG* g) { gs_emit(g->c); }
[[maybe_unused]] static void sg_EndPrimitive(SgG* g) { spv_emit_end_primitive(&g->c->e); }
[[maybe_unused]] static void sg_MemoryBarrierDevice(SgG* g) {
    /* the device scope, for every scope: acquire and release of buffer, workgroup and image memory */
    spv_emit_memory_barrier(&g->c->e, g->c->t_u32, SPV_SCOPE_DEVICE, 0x8 | 0x40 | 0x100 | 0x800);
}
