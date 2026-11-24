/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SM53's state for sailgen --gpu (after tools/sailgen/spirv.h): sm53.sail's vals over translate.h's accessors, which
   sm53_sail.c, the file that includes what sailgen writes, is compiled with. Each family's own (sm53_<family>_spirv.h)
   follows. An instruction's context is to_spirv's (Tr) and the instruction's word and fields; sg53_begin makes it */

struct SgG {
    SgSpv spv;
    const Tr* t;
    DirectCtx* c;
    u32 i;   /* the instruction's index */
    u64 w;   /* its word */
    int op;  /* its opcode */
    u32 d, a, b;
    int form; /* its opcode's place in its roots line */
    u32 out[4]; /* what a back end's function of several results gave, for the functions that read them */
};
static SgSpv* sg_spv(SgG* g) { return &g->spv; }
[[maybe_unused]] static uint32_t sg53_t_u64(SgG* g) { return g->t->u64_type; }
[[maybe_unused]] static void sg53_begin(SgG* g, const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b) {
    DirectCtx* c = t->c;
    g->t = t;
    g->c = c;
    g->i = i;
    g->w = w;
    g->op = op;
    g->d = d;
    g->a = a;
    g->b = b;
    g->form = 0;
    g->spv.e = &c->emit;
    g->spv.t_u32 = c->u32_type;
    g->spv.t_i32 = t->i32_type;
    g->spv.t_f32 = c->f32_type;
    g->spv.t_bool = c->bool_type;
    g->spv.glsl = c->glsl;
    g->spv.true_v = c->true_value;
    g->spv.false_v = c->false_value;
    g->spv.t_u64 = sg53_t_u64;
    g->spv.ncast = 0;
    g->spv.fold = 1;        /* cc_and, cc_or, cc_not and shader_select fold the constants */
    g->spv.fresh_casts = 1; /* the families bitcast each time (reg_read_as keeps its own) */
    g->spv.known = 0;
}
[[maybe_unused]] static void sg_set_form(SgG* g, int k) { g->form = k; }
[[maybe_unused]] static uint64_t sg_Field(SgG* g, int64_t lo, int64_t n) { return B(g->w, (u32)lo, (u32)n); }
[[maybe_unused]] static int64_t sg_Form(SgG* g) { return g->form; }
[[maybe_unused]] static int64_t sg_Dst(SgG* g) { return g->d; }
[[maybe_unused]] static int64_t sg_RegA(SgG* g) { return g->a; }
[[maybe_unused]] static int64_t sg_RegB(SgG* g) { return g->b; }
/* the program's stage (shader.h's ST_*), whether it runs between the vertex and fragment stages, and its input
   primitive's vertices */
[[maybe_unused]] static int64_t sg_Stage(SgG* g) { return g->t->stage; }
[[maybe_unused]] static int sg_Between(SgG* g) { return g->t->between; }
[[maybe_unused]] static int64_t sg_InVertices(SgG* g) { return g->t->in_vertices; }
[[maybe_unused]] static SgV sg_R_read(SgG* g, int64_t r) { return sg_v(reg_read(g->c, (u32)r), SGT_U32); }
[[maybe_unused]] static void sg_R_write(SgG* g, int64_t r, SgV v) { reg_write(g->c, (u32)r, sg_u32(g, v)); }
[[maybe_unused]] static void sg_R_copy(SgG* g, int64_t d, int64_t r) { reg_copy(g->c, (u32)d, (u32)r); }
[[maybe_unused]] static SgV sg_R_readF(SgG* g, int64_t r) { return sg_v(reg_read_as(g->c, (u32)r, g->c->f32_type), SGT_F32); }
[[maybe_unused]] static void sg_R_writeF(SgG* g, int64_t r, SgV v) {
    reg_write_as(g->c, (u32)r, sg_as(g, v, SGT_F32), g->c->f32_type);
}
[[maybe_unused]] static SgV sg_PredRead(SgG* g, int64_t p, int64_t negative) {
    return sg_v(pred_read(g->c, (u32)p, (int)negative), SGT_BOOL);
}
[[maybe_unused]] static void sg_PredWrite(SgG* g, int64_t p, SgV v) { pred_write(g->c, (u32)p, v.id); }
[[maybe_unused]] static uint32_t sg53_flag(SgG* g, int64_t k) {
    DirectCtx* c = g->c;
    return k == 0 ? c->cc_z : k == 1 ? c->cc_s : k == 2 ? c->cc_c : c->cc_o;
}
[[maybe_unused]] static SgV sg_FlagRead(SgG* g, int64_t k) { return sg_v(flag_read(g->c, sg53_flag(g, k)), SGT_BOOL); }
[[maybe_unused]] static void sg_FlagWrite(SgG* g, int64_t k, SgV v) { cc_store(g->c, sg53_flag(g, k), v.id); }
[[maybe_unused]] static SgV sg_Right(SgG* g, int64_t kind) {
    return sg_v(shader_integer_right(g->c, g->w, (int)kind), SGT_U32);
}
[[maybe_unused]] static SgV sg_CbufWord(SgG* g) { return sg_v(shader_cbuf_word(g->c, g->w), SGT_U32); }
[[maybe_unused]] static void sg_throw_Unsupported(SgG* g, const char* f, ...) {
    ShaderInfo* si = g->t->si;
    va_list ap;
    si->unsupported = 1;
    va_start(ap, f);
    vsnprintf(si->first_unsupported, sizeof si->first_unsupported, f, ap);
    va_end(ap);
}
