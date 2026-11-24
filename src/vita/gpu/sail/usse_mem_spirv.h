/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The USSE's loads', texture reads' and special instructions' back end (usse_mem.sail): the literal buffer and the
   uniform buffers' storage buffer VLDST reads, the samplers, formats and registers of SMP's texture reads (SMP's
   gather, moved here from usse.c as it was), KILL's discard and DEPTHF's output */

[[maybe_unused]] static int64_t sg_LiteralRegister(SgG* g) { return g->t->literal_register; }
[[maybe_unused]] static int64_t sg_LiteralBase(SgG* g) { return g->t->literal_base; }
[[maybe_unused]] static int sg_HasLiterals(SgG* g) { return g->t->literals != 0; }
[[maybe_unused]] static int sg_LiteralFits(SgG* g, int64_t offset) {
    const unsigned char* data = gxp_at(g->t->gxp, GXP_LITERAL_DATA);
    return !((size_t)(data - g->t->gxp) + (uint32_t)offset + 4 > g->t->size);
}
[[maybe_unused]] static int64_t sg_LiteralData(SgG* g, int64_t offset) {
    return snx_ld_le32(gxp_at(g->t->gxp, GXP_LITERAL_DATA) + (uint32_t)offset);
}
[[maybe_unused]] static int sg_HasBuffer(SgG* g) { return g->t->buffer != 0; }
[[maybe_unused]] static void sg_LiteralsBegin(SgG* g) {
    g->pointer = spv_emit_type_pointer(&g->t->e, SPV_STORAGE_PRIVATE, g->t->t_u32);
}
[[maybe_unused]] static void sg_BufferBegin(SgG* g) {
    g->pointer = spv_emit_type_pointer(&g->t->e, SPV_STORAGE_STORAGE_BUFFER, g->t->t_u32);
}
/* the literal array's word at, clamped to the array */
[[maybe_unused]] static SgV sg_LiteralRead(SgG* g, SgV at) {
    T* t = g->t;
    uint32_t i = ext2(t, t->t_u32, SPV_GLSL_U_MIN, sg_u32(g, at), cu(t, t->literal_words - 1));
    return sg_v(SPV(&t->e, SPV_OP_LOAD, t->t_u32, SPV(&t->e, SPV_OP_ACCESS_CHAIN, g->pointer, t->literals, i)), SGT_U32);
}
[[maybe_unused]] static SgV sg_BufferRead(SgG* g, SgV at) {
    T* t = g->t;
    uint32_t i = sg_u32(g, at);
    return sg_v(SPV(&t->e, SPV_OP_LOAD, t->t_u32, SPV(&t->e, SPV_OP_ACCESS_CHAIN, g->pointer, t->buffer, cu(t, 0), i)),
                SGT_U32);
}
[[maybe_unused]] static void sg_WordStore(SgG* g, int64_t bank, int64_t n, SgV v) {
    store_word(g->t, (Bank)bank, (unsigned)n, sg_u32(g, v));
}

[[maybe_unused]] static int64_t sg_Dependent(SgG* g, int64_t reg) { return reg < BANK_WORDS ? g->t->dependent[reg] : 0; }
[[maybe_unused]] static int64_t sg_TextureFormat(SgG* g, int64_t resource) { return g->t->hints->texture_formats[resource]; }
[[maybe_unused]] static int64_t sg_TextureType(SgG* g, int64_t format) { return (void)g, texture_component_type((uint32_t)format); }
[[maybe_unused]] static int64_t sg_TextureComponents(SgG* g, int64_t format) { return (void)g, texture_components((uint32_t)format); }
[[maybe_unused]] static SgV sg_Sampler(SgG* g, int64_t resource, int64_t cube) {
    return sg_vt(sampler_of(g->t, (unsigned)resource, cube ? SPV_DIM_CUBE : SPV_DIM_2D), 0);
}
/* SMP's gather4: the four texels' components pixel by pixel, four a register group; with uv, then the four bilinear
   weights as halves, in the order the SDK's shaders read them ((1-u)(1-v), u(1-v), uv, (1-u)v) */
[[maybe_unused]] static bool gather(T* t, uint32_t sampler, uint32_t coord, uint32_t format, Type type, Bank bank, unsigned dest,
                   bool uv) {
    unsigned n = texture_components(format);
    uint32_t texels[16];
    for (unsigned c = 0; c < n; ++c) {
        uint32_t g = SPV(&t->e, SPV_OP_IMAGE_GATHER, t->t_v4f, sampler, coord, ci(t, (int32_t)c));
        for (unsigned pixel = 0; pixel < 4; ++pixel)
            texels[pixel * n + c] = SPV(&t->e, SPV_OP_COMPOSITE_EXTRACT, t->t_f32, g, pixel);
    }
    unsigned step = type_size(type); /* registers four components take */
    for (unsigned at = 0; at < 4 * n; at += 4, dest += step) {
        Value v = {{texels[at], texels[at + 1], texels[at + 2], texels[at + 3]}, 4, K_FLOAT};
        store_input(t, bank, dest, type, v);
    }
    if (!uv) return !t->failed;
    spv_emit_image_query_capability(&t->e);
    uint32_t v2i = spv_emit_type_vector(&t->e, t->t_i32, 2);
    uint32_t lod = t->fragment ? SPV(&t->e, SPV_OP_COMPOSITE_EXTRACT, t->t_f32,
                                     SPV(&t->e, SPV_OP_IMAGE_QUERY_LOD, t->t_v2f, sampler, coord), 0)
                               : cf(t, 0);
    uint32_t image_type = spv_emit_type_image(&t->e, t->t_f32, SPV_DIM_2D, 0, 0, 0, 1, SPV_IMAGE_FORMAT_UNKNOWN);
    uint32_t image = op1(t, SPV_OP_IMAGE, image_type, sampler);
    uint32_t size = SPV(&t->e, SPV_OP_IMAGE_QUERY_SIZE_LOD, v2i, image, op1(t, SPV_OP_CONVERT_F_TO_S, t->t_i32, lod));
    uint32_t texel = op2(t, SPV_OP_F_MUL, t->t_v2f, coord, op1(t, SPV_OP_CONVERT_S_TO_F, t->t_v2f, size));
    uint32_t half = SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v2f, cf(t, 0.5f), cf(t, 0.5f));
    uint32_t f = ext1(t, t->t_v2f, SPV_GLSL_FRACT, op2(t, SPV_OP_F_SUB, t->t_v2f, texel, half));
    uint32_t u = SPV(&t->e, SPV_OP_COMPOSITE_EXTRACT, t->t_f32, f, 0), w = SPV(&t->e, SPV_OP_COMPOSITE_EXTRACT, t->t_f32, f, 1);
    uint32_t nu = op2(t, SPV_OP_F_SUB, t->t_f32, cf(t, 1), u), nw = op2(t, SPV_OP_F_SUB, t->t_f32, cf(t, 1), w);
    Value weights = {{op2(t, SPV_OP_F_MUL, t->t_f32, nu, nw), op2(t, SPV_OP_F_MUL, t->t_f32, u, nw),
                      op2(t, SPV_OP_F_MUL, t->t_f32, u, w), op2(t, SPV_OP_F_MUL, t->t_f32, nu, w)},
                     4, K_FLOAT};
    store_input(t, bank, dest, T_F16, weights);
    return !t->failed;
}
[[maybe_unused]] static void sg_Gather(SgG* g, SgV sampler, SgV coord, int64_t format, int64_t type, int64_t bank, int64_t dest,
                                       int64_t uv) {
    gather(g->t, sampler.id, coord.id, (uint32_t)format, (Type)type, (Bank)bank, (unsigned)dest, uv);
}
[[maybe_unused]] static void sg_StoreInput(SgG* g, int64_t bank, int64_t dest, int64_t type, SgV v, int64_t n) {
    store_input(g->t, (Bank)bank, (unsigned)dest, (Type)type, vector_value(g->t, v.id, (unsigned)n, K_FLOAT));
}

/* KILL: the fragment discarded, which ends its block */
[[maybe_unused]] static void sg_Kill(SgG* g) {
    spv_emit_void_op(&g->t->e, SPV_OP_KILL, NULL, 0);
    g->t->terminated = 1;
}
[[maybe_unused]] static int sg_HasDepth(SgG* g) { return g->t->frag_depth != 0; }
[[maybe_unused]] static void sg_DepthWrite(SgG* g, SgV v) { SPV_VOID(&g->t->e, SPV_OP_STORE, g->t->frag_depth, sg_f(g, v)); }
