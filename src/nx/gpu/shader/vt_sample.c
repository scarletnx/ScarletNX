/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A fragment program's sample of a texture that may be virtual (vt_sparse, vk/vt.c): shader.h's shader_vt_sample */
#include "nx/gpu/shader/vt_sample.h"

u64 g_shader_vt_address;

static u32 cu(SpvEmitter* e, const ShVtSample* s, u32 v) { return spv_emit_constant_u32(e, s->t_u32, v); }
static u32 cf(SpvEmitter* e, const ShVtSample* s, float v) { return spv_emit_constant_f32(e, s->t_f32, v); }
static u32 to_u(SpvEmitter* e, const ShVtSample* s, u32 f) { return SPV(e, SPV_OP_CONVERT_F_TO_U, s->t_u32, f); }
/* a coordinate's cell (VT_CELLS of them across the level, from its fraction) and whether it lies under 0 (1) or at 1
   and over (2) */
static void cell(SpvEmitter* e, const ShVtSample* s, u32 x, u32* at, u32* outside) {
    const u32 f = spv_emit_glsl_unary(e, s->t_f32, s->glsl, SPV_GLSL_FRACT, x);
    *at = spv_emit_glsl_binary(e, s->t_u32, s->glsl, SPV_GLSL_U_MIN,
                               to_u(e, s, SPV(e, SPV_OP_F_MUL, s->t_f32, f, cf(e, s, (float)SH_VT_CELLS))),
                               cu(e, s, SH_VT_CELLS - 1));
    const u32 under = SPV(e, SPV_OP_F_ORD_LESS_THAN, s->t_bool, x, cf(e, s, 0.0f));
    const u32 over = SPV(e, SPV_OP_F_ORD_GREATER_THAN_EQUAL, s->t_bool, x, cf(e, s, 1.0f));
    *outside = SPV(e, SPV_OP_SELECT, s->t_u32, under, cu(e, s, 1),
                   SPV(e, SPV_OP_SELECT, s->t_u32, over, cu(e, s, 2), cu(e, s, 0)));
}

u32 shader_vt_sample(SpvEmitter* e, ShVtIds* ids, const ShVtSample* s) {
    if (!ids->pair) {
        ids->pair = spv_emit_type_struct_pair(e, s->t_i32, s->t_v4f);
        spv_emit_capability(e, 5347); /* PhysicalStorageBufferAddresses */
        spv_emit_extension(e, "SPV_KHR_physical_storage_buffer");
        e->physical_storage_buffer = 1;
        const u32 table = spv_emit_type_struct_one(e, spv_emit_type_runtime_array(e, s->t_u32, 4));
        ids->table_ptr = spv_emit_type_pointer(e, 5349, table); /* PhysicalStorageBuffer */
        ids->word_ptr = spv_emit_type_pointer(e, 5349, s->t_u32);
        ids->address =
            spv_emit_constant_vec2(e, spv_emit_type_vector(e, s->t_u32, 2), cu(e, s, (u32)g_shader_vt_address),
                                   cu(e, s, (u32)(g_shader_vt_address >> 32)));
        spv_emit_capability(e, SPV_CAPABILITY_IMAGE_QUERY);
    }
    const u32 zero = cu(e, s, 0), uv = spv_emit_vec2_construct(e, s->t_v2f, s->x, s->y);
    /* a block of this sample's own, so that the joins below know where each value comes from */
    const u32 pre = spv_emit_id(e), miss = spv_emit_id(e), tail = spv_emit_id(e), back = spv_emit_id(e),
              join = spv_emit_id(e), mark = spv_emit_id(e), done = spv_emit_id(e);
    spv_emit_branch(e, pre);
    spv_emit_label(e, pre);
    const u32 first =
        spv_emit_sample_image_sparse(e, ids->pair, s->sampled_type, s->sampler, s->coord, s->mode, s->level, s->offset);
    const u32 texel = SPV(e, SPV_OP_COMPOSITE_EXTRACT, s->t_v4f, first, 1);
    const u32 resident =
        SPV(e, SPV_OP_IMAGE_SPARSE_TEXELS_RESIDENT, s->t_bool, SPV(e, SPV_OP_COMPOSITE_EXTRACT, s->t_i32, first, 0));
    /* the coordinates' change across the quad, taken here: past the branch a lane's neighbours may not be there */
    u32 dx = 0, dy = 0;
    if (s->mode != 1) {
        dx = SPV(e, SPV_OP_DPDX, s->t_v2f, uv);
        dy = SPV(e, SPV_OP_DPDY, s->t_v2f, uv);
    }
    spv_emit_selection_merge(e, join);
    spv_emit_branch_conditional(e, resident, join, miss);

    /* A texel without memory: the sample again at a level coarser than the one it wanted, as the size and the
       derivatives estimate it (no anisotropy, no sampler bias: it decides only how coarse this frame reads), and where
       that has no memory either, at the last level, which a sparse image holds always (its mip tail) */
    spv_emit_label(e, miss);
    u32 coarser;
    if (s->mode == 1)
        coarser = s->level;
    else {
        const u32 size =
            spv_emit_image_query_size_lod(e, spv_emit_type_vector(e, s->t_i32, s->array ? 3 : 2), s->sampled_type,
                                          s->sampler, spv_emit_constant_u32(e, s->t_i32, 0));
        u32 rho = 0;
        for (u32 k = 0; k < 2; k++) {
            u32 sum = 0;
            for (u32 axis = 0; axis < 2; axis++) {
                const u32 extent =
                    SPV(e, SPV_OP_CONVERT_S_TO_F, s->t_f32, spv_emit_vec4_extract(e, s->t_i32, size, axis));
                const u32 d =
                    SPV(e, SPV_OP_F_MUL, s->t_f32, spv_emit_vec4_extract(e, s->t_f32, k ? dy : dx, axis), extent);
                const u32 sq = SPV(e, SPV_OP_F_MUL, s->t_f32, d, d);
                sum = sum ? SPV(e, SPV_OP_F_ADD, s->t_f32, sum, sq) : sq;
            }
            rho = rho ? spv_emit_glsl_binary(e, s->t_f32, s->glsl, SPV_GLSL_F_MAX, rho, sum) : sum;
        }
        /* log2 of the longer side's length: half the log2 of its square */
        coarser =
            SPV(e, SPV_OP_F_MUL, s->t_f32, cf(e, s, 0.5f),
                spv_emit_glsl_unary(e, s->t_f32, s->glsl, SPV_GLSL_LOG2,
                                    spv_emit_glsl_binary(e, s->t_f32, s->glsl, SPV_GLSL_F_MAX, rho, cf(e, s, 1e-12f))));
        if (s->mode == 2) coarser = SPV(e, SPV_OP_F_ADD, s->t_f32, coarser, s->level);
    }
    /* no coarser than the last level: the device clamps a level, but lavapipe's sparse sample (Mesa 26.2) reads past
       the image's levels with one that isn't */
    const u32 top = SPV(e, SPV_OP_CONVERT_U_TO_F, s->t_f32,
                        SPV(e, SPV_OP_I_SUB, s->t_u32,
                            spv_emit_image_query_levels(e, s->t_u32, s->sampled_type, s->sampler), cu(e, s, 1)));
    coarser = spv_emit_glsl_ternary(e, s->t_f32, s->glsl, SPV_GLSL_F_CLAMP,
                                    SPV(e, SPV_OP_F_ADD, s->t_f32, coarser, cf(e, s, 1.0f)), cf(e, s, 0.0f), top);
    const u32 second =
        spv_emit_sample_image_sparse(e, ids->pair, s->sampled_type, s->sampler, s->coord, 1, coarser, s->offset);
    const u32 texel2 = SPV(e, SPV_OP_COMPOSITE_EXTRACT, s->t_v4f, second, 1);
    const u32 resident2 =
        SPV(e, SPV_OP_IMAGE_SPARSE_TEXELS_RESIDENT, s->t_bool, SPV(e, SPV_OP_COMPOSITE_EXTRACT, s->t_i32, second, 0));
    spv_emit_selection_merge(e, back);
    spv_emit_branch_conditional(e, resident2, back, tail);
    spv_emit_label(e, tail);
    const u32 texel3 =
        spv_emit_sample_image(e, s->t_v4f, s->sampled_type, s->sampler, s->coord, 0, 1, top, s->offset, 0);
    spv_emit_branch(e, back);
    spv_emit_label(e, back);
    const u32 fallback = spv_emit_phi2(e, s->t_v4f, texel2, miss, texel3, tail);
    spv_emit_branch(e, join);
    spv_emit_label(e, join);
    const u32 result = spv_emit_phi2(e, s->t_v4f, texel, pre, fallback, back);

    /* The mark, in one quad of SH_VT_QUADS, another each frame (phase): every lane of a quad takes the branch or none
       does, so the level the hardware takes (OpImageQueryLod, with the sampler's anisotropy and bias) is defined in
       it. Both levels of a trilinear sample, with whether the sample found memory */
    const u32 qx = SPV(e, SPV_OP_SHIFT_RIGHT_LOGICAL, s->t_u32, to_u(e, s, s->pixel_x), cu(e, s, 1));
    const u32 qy = SPV(e, SPV_OP_SHIFT_RIGHT_LOGICAL, s->t_u32, to_u(e, s, s->pixel_y), cu(e, s, 1));
    const u32 turn =
        SPV(e, SPV_OP_BITWISE_AND, s->t_u32,
            SPV(e, SPV_OP_I_ADD, s->t_u32,
                SPV(e, SPV_OP_I_ADD, s->t_u32, qx, SPV(e, SPV_OP_I_MUL, s->t_u32, qy, cu(e, s, 3))), s->phase),
            cu(e, s, SH_VT_QUADS - 1));
    const u32 marks = SPV(e, SPV_OP_I_EQUAL, s->t_bool, turn, zero);
    spv_emit_selection_merge(e, done);
    spv_emit_branch_conditional(e, marks, mark, done);
    spv_emit_label(e, mark);
    const u32 levels = spv_emit_image_query_levels(e, s->t_u32, s->sampled_type, s->sampler);
    const u32 last = SPV(e, SPV_OP_I_SUB, s->t_u32, levels, cu(e, s, 1));
    u32 lod;
    if (s->mode == 1)
        lod = s->level;
    else {
        const u32 q = spv_emit_image_query_lod(e, s->t_v2f, s->sampled_type, s->sampler, uv);
        lod = spv_emit_vec4_extract(e, s->t_f32, q, s->mode == 2 ? 1 : 0);
        if (s->mode == 2) lod = SPV(e, SPV_OP_F_ADD, s->t_f32, lod, s->level);
    }
    lod = spv_emit_glsl_ternary(e, s->t_f32, s->glsl, SPV_GLSL_F_CLAMP, lod, cf(e, s, 0.0f),
                                SPV(e, SPV_OP_CONVERT_U_TO_F, s->t_f32, last));
    const u32 m0 = to_u(e, s, spv_emit_glsl_unary(e, s->t_f32, s->glsl, SPV_GLSL_FLOOR, lod));
    const u32 between = SPV(e, SPV_OP_F_ORD_GREATER_THAN, s->t_bool,
                            spv_emit_glsl_unary(e, s->t_f32, s->glsl, SPV_GLSL_FRACT, lod), cf(e, s, 0.0f));
    const u32 m1 = SPV(e, SPV_OP_SELECT, s->t_u32, between,
                       spv_emit_glsl_binary(e, s->t_u32, s->glsl, SPV_GLSL_U_MIN,
                                            SPV(e, SPV_OP_I_ADD, s->t_u32, m0, cu(e, s, 1)), last),
                       m0);
    u32 u, v, uo, vo;
    cell(e, s, s->x, &u, &uo);
    cell(e, s, s->y, &v, &vo);
    const u32 layer =
        s->array ? SPV(e, SPV_OP_BITWISE_AND, s->t_u32, to_u(e, s, s->layer), cu(e, s, SH_VT_LAYERS - 1)) : zero;
#define OR(a, b)  SPV(e, SPV_OP_BITWISE_OR, s->t_u32, (a), (b))
#define SHL(a, n) SPV(e, SPV_OP_SHIFT_LEFT_LOGICAL, s->t_u32, (a), cu(e, s, (n)))
#define SHR(a, n) SPV(e, SPV_OP_SHIFT_RIGHT_LOGICAL, s->t_u32, (a), cu(e, s, (n)))
    const u32 w1 = OR(OR(u, SHL(v, 10)), SHL(layer, 20));
    const u32 place = OR(OR(SHR(u, 4), SHL(SHR(v, 4), 6)), SHL(layer, 12));
    const u32 flags = OR(OR(SPV(e, SPV_OP_BITWISE_AND, s->t_u32, s->id, cu(e, s, SH_VT_IDS - 1)),
                            SPV(e, SPV_OP_SELECT, s->t_u32, resident, cu(e, s, 1u << 24), zero)),
                         OR(OR(SHL(uo, 25), SHL(vo, 27)), cu(e, s, 1u << 31)));
    const u32 base = spv_emit_op(e, SPV_OP_BITCAST, ids->table_ptr, (u32[]){ids->address}, 1);
    /* only a virtual texture's samples take a slot of the table: its identity's bit, in the words after the table */
    const u32 identity = SPV(e, SPV_OP_BITWISE_AND, s->t_u32, s->id, cu(e, s, SH_VT_IDS - 1));
    const u32 named = spv_emit_op(
        e, SPV_OP_ATOMIC_LOAD, s->t_u32,
        (u32[]){spv_emit_op(
                    e, SPV_OP_ACCESS_CHAIN, ids->word_ptr,
                    (u32[]){base, zero, SPV(e, SPV_OP_I_ADD, s->t_u32, SHR(identity, 5), cu(e, s, 2 * SH_VT_SLOTS))},
                    3),
                cu(e, s, 1), zero},
        3);
    const u32 is_virtual = SPV(e, SPV_OP_I_NOT_EQUAL, s->t_bool,
                               SPV(e, SPV_OP_BITWISE_AND, s->t_u32,
                                   SPV(e, SPV_OP_SHIFT_RIGHT_LOGICAL, s->t_u32, named,
                                       SPV(e, SPV_OP_BITWISE_AND, s->t_u32, identity, cu(e, s, 31))),
                                   cu(e, s, 1)),
                               zero);
    const u32 write = spv_emit_id(e), written = spv_emit_id(e);
    spv_emit_selection_merge(e, written);
    spv_emit_branch_conditional(e, is_virtual, write, written);
    spv_emit_label(e, write);
    for (u32 k = 0; k < 2; k++) {
        const u32 w0 = OR(flags, SHL(k ? m1 : m0, 20));
        /* shader_vt_slot's hash */
        const u32 h =
            SPV(e, SPV_OP_I_MUL, s->t_u32,
                SPV(e, SPV_OP_BITWISE_XOR, s->t_u32, w0, SPV(e, SPV_OP_I_MUL, s->t_u32, place, cu(e, s, 0x85EBCA6Bu))),
                cu(e, s, 0x9E3779B1u));
        const u32 word = SHL(SHR(h, 32 - SH_VT_SLOT_BITS), 1);
        const u32 p0 = spv_emit_op(e, SPV_OP_ACCESS_CHAIN, ids->word_ptr, (u32[]){base, zero, word}, 3);
        const u32 p1 =
            spv_emit_op(e, SPV_OP_ACCESS_CHAIN, ids->word_ptr, (u32[]){base, zero, OR(word, cu(e, s, 1))}, 3);
        /* an exchange: a store through a physical pointer wants an alignment the emitter's stores don't carry */
        spv_emit_atomic_u32(e, s->t_u32, SPV_OP_ATOMIC_EXCHANGE, p0, w0);
        spv_emit_atomic_u32(e, s->t_u32, SPV_OP_ATOMIC_EXCHANGE, p1, w1);
    }
#undef OR
#undef SHL
#undef SHR
    spv_emit_branch(e, written);
    spv_emit_label(e, written);
    spv_emit_branch(e, done);
    spv_emit_label(e, done);
    return result;
}
