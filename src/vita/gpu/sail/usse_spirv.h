/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The USSE's state for sailgen --gpu (after tools/sailgen/spirv.h): usse.sail's vals over usse.c's translator (T),
   its operand model and its loads and stores, which usse.c includes with the file sailgen writes after them. Each
   family's own (usse_<family>_spirv.h) follows. An instruction's context is the translator and the instruction's
   word, its operands and its values; sgu_begin makes it */

struct SgG {
    SgSpv spv;
    T* t;
    uint64_t w;      /* the instruction's word */
    const void* arg; /* its fields, as usse.decode's pattern decodes them (arg_<pattern>) */
    int form;   /* its opcode's place in its roots line */
    Operand op[8];
    Value val[8];
    uint8_t layout[2][3]; /* VDUAL's sources' layouts */
    uint32_t test[4];     /* a test's results */
    uint32_t pointer;     /* VLDST's reads' pointer type */
};
static SgSpv* sg_spv(SgG* g) { return &g->spv; }
[[maybe_unused]] static uint32_t sgu_t_u64(SgG* g) { return (void)g, 0; }
[[maybe_unused]] static void sgu_begin(SgG* g, T* t, uint64_t w, const void* arg) {
    memset(g, 0, sizeof *g);
    g->t = t;
    g->w = w;
    g->arg = arg;
    g->spv.e = &t->e;
    g->spv.t_u32 = t->t_u32;
    g->spv.t_i32 = t->t_i32;
    g->spv.t_f32 = t->t_f32;
    g->spv.t_bool = t->t_bool;
    g->spv.glsl = t->glsl;
    g->spv.t_u64 = sgu_t_u64;
    g->spv.fold = 0;
    g->spv.fresh_casts = 1; /* usse.c bitcasts a value each time it takes it as another type */
    g->spv.known = 1;       /* its constants are the module's known ones (spv_emit_const_*) */
}
[[maybe_unused]] static void sg_set_form(SgG* g, int k) { g->form = k; }
[[maybe_unused]] static uint64_t sg_Field(SgG* g, int64_t lo, int64_t n) {
    return (g->w >> lo) & (n >= 64 ? ~0ull : (1ull << n) - 1);
}
[[maybe_unused]] static int64_t sg_Form(SgG* g) { return g->form; }
[[maybe_unused]] static int sg_Failed(SgG* g) { return g->t->failed; }
[[maybe_unused]] static int sg_Fragment(SgG* g) { return g->t->fragment; }
[[maybe_unused]] static int sg_Secondary(SgG* g) { return g->t->secondary; }
[[maybe_unused]] static void sg_throw_Refuse(SgG* g, const char* format, ...) {
    T* t = g->t;
    if (!t->failed) {
        va_list list;
        va_start(list, format);
        vsnprintf(t->error, t->error_size, format, list);
        va_end(list);
    }
    t->failed = true;
}
[[maybe_unused]] static void sg_throw_Stop(SgG* g) { (void)g; }

/* ---- the operands ---- */
[[maybe_unused]] static void sg_Dest(SgG* g, int64_t k, int64_t n, int64_t bank, int64_t ext, int64_t doubled, int64_t bits) {
    g->op[k] = decode_dest(g->t, (unsigned)n, (unsigned)bank, ext, doubled, (unsigned)bits);
}
[[maybe_unused]] static void sg_Src0(SgG* g, int64_t k, int64_t n, int64_t bank, int64_t ext, int64_t doubled, int64_t bits) {
    g->op[k] = decode_src0(g->t, (unsigned)n, (unsigned)bank, ext, doubled, (unsigned)bits);
}
[[maybe_unused]] static void sg_Src12(SgG* g, int64_t k, int64_t n, int64_t bank, int64_t ext, int64_t doubled, int64_t bits) {
    g->op[k] = decode_src12(g->t, (unsigned)n, (unsigned)bank, ext, doubled, (unsigned)bits);
}
[[maybe_unused]] static void sg_Internal(SgG* g, int64_t k, int64_t n, int64_t flags, int64_t type) {
    g->op[k] = (Operand){(unsigned)n, B_INTERNAL, (unsigned)flags, {SW_X, SW_Y, SW_Z, SW_W}, (Type)type};
}
[[maybe_unused]] static void sg_CopyOperand(SgG* g, int64_t k, int64_t j) { g->op[k] = g->op[j]; }
[[maybe_unused]] static void sg_SetType(SgG* g, int64_t k, int64_t type) { g->op[k].type = (Type)type; }
[[maybe_unused]] static void sg_SetFlags(SgG* g, int64_t k, int64_t flags) { g->op[k].flags = (unsigned)flags; }
[[maybe_unused]] static void sg_AddFlags(SgG* g, int64_t k, int64_t flags) { g->op[k].flags |= (unsigned)flags; }
[[maybe_unused]] static void sg_SetNum(SgG* g, int64_t k, int64_t n) { g->op[k].num = (unsigned)n; }
[[maybe_unused]] static void sg_SetSwizzle(SgG* g, int64_t k, int64_t x, int64_t y, int64_t z, int64_t w) {
    uint8_t s[4] = {(uint8_t)x, (uint8_t)y, (uint8_t)z, (uint8_t)w};
    memcpy(g->op[k].swizzle, s, 4);
}
[[maybe_unused]] static void sg_CopySwizzle(SgG* g, int64_t k, int64_t j) { memcpy(g->op[k].swizzle, g->op[j].swizzle, 4); }
[[maybe_unused]] static void sg_Vec34Swizzle(SgG* g, int64_t k, int64_t swizzle, int64_t extended, int64_t type) {
    vec34_swizzle(g->op[k].swizzle, (unsigned)swizzle, extended, (int)type);
}
[[maybe_unused]] static void sg_Swizzle4(SgG* g, int64_t k, int64_t encoded) { swizzle4(g->op[k].swizzle, (unsigned)encoded); }
[[maybe_unused]] static int64_t sg_BankOf(SgG* g, int64_t k) { return g->op[k].bank; }
[[maybe_unused]] static int64_t sg_NumOf(SgG* g, int64_t k) { return g->op[k].num; }
[[maybe_unused]] static int64_t sg_TypeOf(SgG* g, int64_t k) { return g->op[k].type; }
[[maybe_unused]] static int64_t sg_ChannelOf(SgG* g, int64_t k, int64_t i) { return g->op[k].swizzle[i]; }
[[maybe_unused]] static int64_t sg_WriteMask(SgG* g, int64_t k, int64_t mask, int64_t f16) {
    return decode_write_mask(g->op[k].bank, (unsigned)mask, f16);
}

/* ---- the repeats ---- */
[[maybe_unused]] static int64_t sg_Rep(SgG* g, int64_t k, int64_t r, int64_t which, int64_t mode) {
    return repeat_offset(g->t, &g->op[k], (unsigned)r, (unsigned)which, (int)mode);
}
[[maybe_unused]] static void sg_Multiplier(SgG* g, int64_t a, int64_t b, int64_t c, int64_t d) {
    set_multiplier(g->t, (int)a, (int)b, (int)c, (int)d);
}
[[maybe_unused]] static void sg_ResetMultiplier(SgG* g) { reset_repeat_multiplier(g->t); }
[[maybe_unused]] static void sg_SetIncrease(SgG* g, int64_t which, int64_t i, int64_t v) {
    g->t->repeat_increase[which][i] = (int)v;
}

/* ---- the values ---- */
static int sgu_kind_t(int kind) { return kind == K_FLOAT ? SGT_F32 : kind == K_UINT ? SGT_U32 : SGT_I32; }
[[maybe_unused]] static void sg_Load(SgG* g, int64_t v, int64_t k, int64_t mask, int64_t shift) {
    g->val[v] = load(g->t, g->op[k], (unsigned)mask, (int)shift);
}
[[maybe_unused]] static void sg_Store(SgG* g, int64_t k, int64_t v, int64_t mask, int64_t shift) {
    store(g->t, g->op[k], g->val[v], (unsigned)mask, (int)shift);
}
[[maybe_unused]] static int64_t sg_Count(SgG* g, int64_t v) { return g->val[v].n; }
[[maybe_unused]] static int64_t sg_KindOf(SgG* g, int64_t v) { return g->val[v].kind; }
[[maybe_unused]] static SgV sg_At(SgG* g, int64_t v, int64_t i) { return sg_v(g->val[v].c[i], sgu_kind_t(g->val[v].kind)); }
[[maybe_unused]] static void sg_Put(SgG* g, int64_t v, int64_t i, SgV x) {
    g->val[v].c[i] = sg_as(g, x, sgu_kind_t(g->val[v].kind));
}
[[maybe_unused]] static void sg_Reset(SgG* g, int64_t v, int64_t n, int64_t kind) {
    g->val[v] = (Value){{0}, (unsigned)n, (int)kind};
}
[[maybe_unused]] static void sg_Push(SgG* g, int64_t v, SgV x) {
    Value* d = &g->val[v];
    d->c[d->n++] = sg_as(g, x, sgu_kind_t(d->kind));
}
[[maybe_unused]] static void sg_SetCount(SgG* g, int64_t v, int64_t n) { g->val[v].n = (unsigned)n; }
[[maybe_unused]] static void sg_SetKind(SgG* g, int64_t v, int64_t kind) { g->val[v].kind = (int)kind; }
[[maybe_unused]] static void sg_CopyValue(SgG* g, int64_t w, int64_t v) { g->val[w] = g->val[v]; }
[[maybe_unused]] static void sg_ToFloat(SgG* g, int64_t v, int64_t type, int64_t normal) {
    g->val[v] = to_float(g->t, g->val[v], (Type)type, normal);
}
[[maybe_unused]] static void sg_ToInt(SgG* g, int64_t v, int64_t type, int64_t normal) {
    g->val[v] = to_int(g->t, g->val[v], (Type)type, normal);
}
