/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A sampler's min/max reduction made in the program (minmax_spirv.h) */
#include "nx/gpu/shader/minmax_spirv.h"

uint32_t shader_minmax_sample(SpvEmitter* e, const ShMinmaxTypes* t, uint32_t sampled_type, uint32_t sampler,
                              uint32_t coordinates, int array, uint32_t offset, int dynamic_offset, int greatest) {
    spv_emit_capability(e, SPV_CAPABILITY_IMAGE_QUERY);
    uint32_t size = spv_emit_image_query_size_lod(e, spv_emit_type_vector(e, t->u32, array ? 3 : 2), sampled_type,
                                                  sampler, spv_emit_constant_u32(e, t->u32, 0));
    uint32_t zero = spv_emit_constant_f32(e, t->f32, 0.0f), half = spv_emit_constant_f32(e, t->f32, 0.5f);
    /* the texel the footprint starts from is floor(p) and the next one's weight fract(p), p the coordinate in the
       base level's texels less a half: zero there leaves the next column or row out. An offset moves whole texels */
    uint32_t next[2];
    for (uint32_t k = 0; k < 2; k++) {
        uint32_t texels = spv_emit_unary(e, SPV_OP_CONVERT_U_TO_F, t->f32, spv_emit_vec4_extract(e, t->u32, size, k));
        uint32_t p = SPV(
            e, SPV_OP_F_SUB, t->f32,
            spv_emit_binary(e, SPV_OP_F_MUL, t->f32, spv_emit_vec4_extract(e, t->f32, coordinates, k), texels), half);
        /* 13: spv_emit_fcompare's unordered not-equal */
        next[k] =
            spv_emit_fcompare(e, t->boolean, 13, spv_emit_glsl_unary(e, t->f32, t->glsl, SPV_GLSL_FRACT, p), zero);
    }
    uint32_t both = spv_emit_bool_binary(e, t->boolean, 0, next[0], next[1]); /* 0: and */
    uint32_t result[4];
    for (uint32_t k = 0; k < 4; k++) {
        /* a gather's x, y, z and w: texels (i, j + 1), (i + 1, j + 1), (i + 1, j) and (i, j); (i, j) always weighs
           something, and stands for those that don't */
        uint32_t g = spv_emit_image_gather_offset(e, t->vec4, sampled_type, sampler, coordinates,
                                                  spv_emit_constant_u32(e, t->u32, k), offset, dynamic_offset);
        uint32_t base = spv_emit_vec4_extract(e, t->f32, g, 3);
        uint32_t right =
            spv_emit_ternary(e, SPV_OP_SELECT, t->f32, next[0], spv_emit_vec4_extract(e, t->f32, g, 2), base);
        uint32_t up = spv_emit_ternary(e, SPV_OP_SELECT, t->f32, next[1], spv_emit_vec4_extract(e, t->f32, g, 0), base);
        uint32_t corner =
            spv_emit_ternary(e, SPV_OP_SELECT, t->f32, both, spv_emit_vec4_extract(e, t->f32, g, 1), base);
        uint32_t op = greatest ? SPV_GLSL_F_MAX : SPV_GLSL_F_MIN;
        result[k] =
            spv_emit_glsl_binary(e, t->f32, t->glsl, op, spv_emit_glsl_binary(e, t->f32, t->glsl, op, base, right),
                                 spv_emit_glsl_binary(e, t->f32, t->glsl, op, up, corner));
    }
    return spv_emit_vec4_construct(e, t->vec4, result);
}
