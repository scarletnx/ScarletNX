/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Texture samples, fetches, gathers and queries, the texture slots, and surface loads and stores */
#include "nx/gpu/shader/translate.h"

/* a texture instruction's array layer: the low 16 bits of its register */
u32 shader_array_layer(DirectCtx* c, u32 r) {
    return spv_emit_convert_u_to_f(&c->emit, c->f32_type,
                                   spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reg_read(c, r),
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff)));
}

u32 shader_texture_component(DirectCtx* c, u32 sampled, u32 scalar_type, u32 component) {
    u32 value = spv_emit_vec4_extract(&c->emit, scalar_type, sampled, component);
    return scalar_type == c->u32_type ? value : spv_emit_bitcast(&c->emit, c->u32_type, value);
}

/* A depth-compare sample of a color texture (ShaderInfo.tex_dc_color_mask): its compare function (TSC's, 0 NEVER to
   7 ALWAYS), or -1 for a slot that samples a depth texture */
int shader_dc_color_func(const ShaderInfo* si, int slot) {
    if (!(si->tex_dc_color_mask >> slot & 1)) return -1;
    u32 index = (u32)snx_popcount64(si->tex_dc_color_mask & ((1ull << slot) - 1));
    return index < SH_DC_COLOR_MAX ? (int)(si->tex_dc_color_funcs >> (3 * index) & 7) : 3;
}
/* 1.0 when reference <func> texel passes, as a compare sampler returns (GL's order: the reference on the left) */
u32 shader_dc_compare(DirectCtx* c, int func, u32 reference, u32 texel) {
    static const u32 condition[8] = {0, 1, 2, 3, 4, 5, 6, 15}; /* spv_emit_fcompare's codes for NEVER..ALWAYS */
    u32 pass = spv_emit_fcompare(&c->emit, c->bool_type, condition[func], reference, texel);
    return spv_emit_select(&c->emit, c->f32_type, pass, spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f),
                           spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f));
}
static u32 indexed_metadata(DirectCtx* c, int slot) {
    u32 limit = spv_emit_constant_u32(&c->emit, c->u32_type, SNX_TEX_ARRAY - 1);
    u32 index = c->active_index;
    index = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, limit, index), limit, index);
    u32 element =
        spv_emit_shift_right_u32(&c->emit, c->u32_type, index, spv_emit_constant_u32(&c->emit, c->u32_type, 2));
    u32 component =
        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, index, spv_emit_constant_u32(&c->emit, c->u32_type, 3));
    u32 func = spv_emit_uniform_component(&c->emit, c->tex_func_pointer, c->tex_func_buffer, c->zero, element,
                                          component, c->u32_type);
    return func;
}
u32 shader_indexed_is_depth(DirectCtx* c, int slot) {
    u32 bit = spv_emit_constant_u32(&c->emit, c->u32_type, 8);
    return spv_emit_i_equal(&c->emit, c->bool_type,
                            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, indexed_metadata(c, slot), bit), bit);
}
u32 shader_dc_compare_indexed(DirectCtx* c, int slot, u32 reference, u32 texel) {
    u32 func = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, indexed_metadata(c, slot),
                                        spv_emit_constant_u32(&c->emit, c->u32_type, 7));
    u32 result = shader_dc_compare(c, 3, reference, texel);
    for (int i = 0; i < 8; ++i) {
        if (i == 3) continue;
        u32 selected = spv_emit_i_equal(&c->emit, c->bool_type, func, spv_emit_constant_u32(&c->emit, c->u32_type, i));
        result = spv_emit_select(&c->emit, c->f32_type, selected, shader_dc_compare(c, i, reference, texel), result);
    }
    return result;
}
/* a sample with a depth reference (dref 0: none), implicit lod or lod: compared in the shader on a color texture */
u32 shader_sample_dref(DirectCtx* c, const ShaderInfo* si, int slot, u32 vec4_type, u32 coords, u32 dref, int use_lod,
                       u32 lod) {
    int func = dref ? shader_dc_color_func(si, slot) : -1;
    if (func < 0)
        return use_lod ? spv_emit_sample2d_lod(&c->emit, dref ? c->f32_type : vec4_type, c->sampled_type[slot],
                                               c->sampler[slot], coords, dref, lod)
                       : spv_emit_sample2d(&c->emit, dref ? c->f32_type : vec4_type, c->sampled_type[slot],
                                           c->sampler[slot], coords, dref);
    u32 texel =
        use_lod ? spv_emit_sample2d_lod(&c->emit, vec4_type, c->sampled_type[slot], c->sampler[slot], coords, 0, lod)
                : spv_emit_sample2d(&c->emit, vec4_type, c->sampled_type[slot], c->sampler[slot], coords, 0);
    return shader_dc_compare(c, func, dref, spv_emit_vec4_extract(&c->emit, c->f32_type, texel, 0));
}
void shader_scaled_coords(DirectCtx* c, u32 push, int slot, int size, u32 i32_type, u32* x, u32* y) {
    u32 one = spv_emit_constant_u32(&c->emit, c->u32_type, 1);
    u32 bits = spv_emit_push_component_f32(&c->emit, spv_emit_type_pointer(&c->emit, 9, c->u32_type), push, one,
                                           slot >= 32 ? one : c->zero, c->u32_type); /* PushConstant */
    u32 bit = spv_emit_bitwise_and_u32(
        &c->emit, c->u32_type,
        spv_emit_shift_right_u32(&c->emit, c->u32_type, bits, spv_emit_constant_u32(&c->emit, c->u32_type, slot & 31)),
        one);
    /* component 2 is the scale, 0 its reciprocal (frame_consts) */
    u32 factor =
        spv_emit_uniform_component(&c->emit, c->scale_pointer, c->scale_buffer, c->zero, c->zero,
                                   size ? c->zero : spv_emit_constant_u32(&c->emit, c->u32_type, 2), c->f32_type);
    factor = spv_emit_select(&c->emit, c->f32_type, spv_emit_i_not_equal(&c->emit, c->bool_type, bit, c->zero), factor,
                             spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f));
    u32 half = spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f);
    u32* v[2] = {x, y};
    for (int k = 0; k < 2; k++) {
        u32 f = size
                    ? spv_emit_convert_u_to_f(&c->emit, c->f32_type, *v[k])
                    : spv_emit_fadd(&c->emit, c->f32_type, spv_emit_convert_s_to_f(&c->emit, c->f32_type, *v[k]), half);
        spv_emit_no_contraction(&c->emit, f);
        f = spv_emit_fmul(&c->emit, c->f32_type, f, factor);
        spv_emit_no_contraction(&c->emit, f);
        if (size) { /* rounded, as vk_sc */
            f = spv_emit_fadd(&c->emit, c->f32_type, f, half);
            spv_emit_no_contraction(&c->emit, f);
            *v[k] = spv_emit_convert_f_to_u(&c->emit, c->u32_type, f);
        } else /* floored: below 0 stays outside */
            *v[k] = spv_emit_convert_f_to_s(&c->emit, i32_type,
                                            spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FLOOR, f));
    }
}

/* The fragment's y (a[0x74]) in a lower-left window (WINDOW_ORIGIN bit 0, pushed as the uvec4's z) counts up from the
   bottom of the surface clip, its height the w, as the viewport and the scissor measure it there: the guest's GL driver
   leaves its own gl_FragCoord transform the identity for the window it draws lower left */
/* A rectangle texture's coordinates (ShaderInfo.tex_rect_mask) are texels: x and y over its size at level 0, the
   size a fragment program's TXQ answers on a scaled target (shader_scaled_coords' unscaled size), which the guest's texels
   count. *gx and *gy, when not NULL, are gradients in texels, divided the same */
void shader_rect_coords(DirectCtx* c, u32 push, int scaled, int slot, u32 vec2_u32, u32 i32_type, u32* x, u32* y,
                        u32* gx, u32* gy) {
    u32 size = spv_emit_image_query_size_lod(&c->emit, vec2_u32, c->sampled_type[slot], c->sampler[slot], c->zero);
    u32 w = spv_emit_vec4_extract(&c->emit, c->u32_type, size, 0),
        h = spv_emit_vec4_extract(&c->emit, c->u32_type, size, 1);
    if (scaled) shader_scaled_coords(c, push, slot, 1, i32_type, &w, &h);
    u32 fw = spv_emit_convert_u_to_f(&c->emit, c->f32_type, w), fh = spv_emit_convert_u_to_f(&c->emit, c->f32_type, h);
    *x = spv_emit_fdiv(&c->emit, c->f32_type, *x, fw);
    *y = spv_emit_fdiv(&c->emit, c->f32_type, *y, fh);
    for (int k = 0; k < 2 && gx; k++) {
        gx[k] = spv_emit_fdiv(&c->emit, c->f32_type, gx[k], k ? fh : fw);
        gy[k] = spv_emit_fdiv(&c->emit, c->f32_type, gy[k], k ? fh : fw);
    }
}
/* x of a 1D texel fetch as the row and column of the 2D image a 1D texture is bound as (SH_TEXEL_ROW_SHIFT). A
   negative x lands in a negative row, out of the image as it is out of the texture */
void shader_texel_row_coord(DirectCtx* c, u32 i32_type, u32* x, u32* y) {
    u32 shift = spv_emit_constant_u32(&c->emit, c->u32_type, SH_TEXEL_ROW_SHIFT);
    u32 column = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, spv_emit_bitcast(&c->emit, c->u32_type, *x),
                                          spv_emit_constant_u32(&c->emit, c->u32_type, (1u << SH_TEXEL_ROW_SHIFT) - 1));
    *y = spv_emit_shift_right_s32(&c->emit, i32_type, *x, shift);
    *x = spv_emit_bitcast(&c->emit, i32_type, column);
}

/* The storage image slot for a surface operation: images of one handle share a slot when they have the same
   dimension (3 a 2D image, 4 a 2D array, 5 a 3D image, 1 a buffer) and are both R32ui (atomics, and loads from buffers) or both
   not. */
int shader_direct_image_slot(const ShaderInfo* info, u32 off, int bindless, u32 bank, int r32ui, int type) {
    int slot = 0;
    while (slot < info->nimg && (info->img[slot].cbuf_off != off || info->img[slot].bindless != bindless ||
                                 (info->img[slot].kind == 1) != r32ui || info->img[slot].type != type ||
                                 (bindless && info->img[slot].bl_cbuf != bank)))
        slot++;
    return slot;
}

/* The 2D texture slot a TXQ queries, bound or by a tracked handle, when nothing else says what the texture is
   (shader_direct_query_type). */
int shader_direct_query_slot(const ShTex* textures, int count, u32 off, int bindless, u32 bank, int handle_count,
                             u32 bank2, u32 off2, u32 indexed, u32 ldc) {
    int slot = 0;
    while (
        slot < count &&
        (textures[slot].cbuf_off != off || textures[slot].type != 1 || textures[slot].array || textures[slot].shadow ||
         (textures[slot].flags & 3) || textures[slot].bindless != bindless ||
         (bindless && !shader_direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
        slot++;
    return slot;
}

/* The slot of a texture with this type (Maxwell's type field halved), array, shadow and kind (sampled or
   multisample), read through that handle. Raw surfaces have their own slot lookup. */
int shader_direct_typed_slot(const ShTex* textures, int count, u32 off, u32 type, u32 array, u32 shadow, u32 kind,
                             int bindless, u32 bank, int handle_count, u32 bank2, u32 off2, u32 indexed, u32 ldc) {
    int slot = 0;
    while (
        slot < count &&
        (textures[slot].cbuf_off != off || textures[slot].type != type || textures[slot].array != array ||
         textures[slot].shadow != shadow || textures[slot].bindless != bindless || (textures[slot].flags & 3) != kind ||
         (bindless && !shader_direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
        slot++;
    return slot;
}

/* shader_direct_typed_slot's slot, added when there is none; -1 when the 32 slots are taken */
int shader_direct_add_typed_slot(ShTex* textures, int* count, u32 off, u32 type, u32 array, u32 shadow, u32 kind,
                                 int bindless, u32 bank, int handle_count, u32 bank2, u32 off2, u32 indexed, u32 ldc) {
    int slot = shader_direct_typed_slot(textures, *count, off, type, array, shadow, kind, bindless, bank, handle_count,
                                        bank2, off2, indexed, ldc);
    if (slot < *count) return slot;
    if (*count >= SH_MAXTEX) return -1;
    memset(&textures[slot], 0, sizeof textures[slot]);
    textures[slot].cbuf_off = off;
    textures[slot].type = (u8)type;
    textures[slot].array = (u8)array;
    textures[slot].shadow = (u8)shadow;
    textures[slot].flags = (u8)kind;
    textures[slot].bindless = (u8)bindless;
    textures[slot].bl_cbuf = (u8)bank;
    textures[slot].bl2 = handle_count == 2;
    textures[slot].bl2_cbuf = (u8)bank2;
    textures[slot].bl2_off = off2;
    if (indexed) {
        textures[slot].flags |= (u8)indexed;
        textures[slot].bl2_off = ldc;
    }
    (*count)++;
    return slot;
}

/* For this instruction, an indexed texture's sampler is the array element its LDC chose, clamped to the array */
void shader_direct_swap_element(DirectCtx* c, int slot) {
    int table = c->tex_table[slot];
    u32 limit = spv_emit_constant_u32(&c->emit, c->u32_type, SNX_TEX_ARRAY - 1);
    u32 index = spv_emit_load(&c->emit, c->u32_type, c->tex_index[slot]);
    index = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, limit, index), limit, index);
    if (c->tex_map_buffer) {
        u32 base = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)table * SNX_TEX_ARRAY);
        u32 flat = spv_emit_iadd_u32(&c->emit, c->u32_type, base, index);
        u32 element =
            spv_emit_shift_right_u32(&c->emit, c->u32_type, flat, spv_emit_constant_u32(&c->emit, c->u32_type, 2));
        u32 component =
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, flat, spv_emit_constant_u32(&c->emit, c->u32_type, 3));
        index = spv_emit_uniform_component(&c->emit, c->tex_map_pointer, c->tex_map_buffer, c->zero, element, component,
                                           c->u32_type);
    }
    c->active_index = index;
    c->sampler[slot] = spv_emit_sampler_array_element(&c->emit, c->sampled_type[slot], c->tex_array[slot], index);
    c->depth_type = c->tex_depth_type[slot];
    c->depth_sampler =
        c->tex_depth[slot] ? spv_emit_sampler_array_element(&c->emit, c->depth_type, c->tex_depth[slot], index) : 0;
    c->swapped = (u32)slot + 1;
}

u32 shader_texture_float_component(DirectCtx* c, u32 value, u32 scalar, u32 i32_type) {
    if (scalar == c->f32_type) return value;
    return scalar == i32_type ? spv_emit_convert_s_to_f(&c->emit, c->f32_type, value)
                              : spv_emit_convert_u_to_f(&c->emit, c->f32_type, value);
}

u32 shader_pack_texture_halves(DirectCtx* c, u32 vec2, u32 first, u32 second) {
    return spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16,
                               spv_emit_vec2_construct(&c->emit, vec2, first, second));
}

/* Sampling with a register offset uses normalized coordinates because SPIR-V requires a constant sample Offset. */
u32 shader_direct_offset_coords(DirectCtx* c, u32 coords, u32 offset, u32 coord_count, u32 offset_count, int slot,
                                u32 level, u32 vec2, u32 vec3, u32 vec2_i32, u32 vec3_i32, u32 i32_type) {
    u32 size = spv_emit_image_query_size_lod(&c->emit, coord_count == 3 ? vec3_i32 : vec2_i32, c->sampled_type[slot],
                                             c->sampler[slot], level);
    if (c->depth_sampler && c->swapped == (u32)slot + 1) {
        u32 depth_size = spv_emit_image_query_size_lod(&c->emit, coord_count == 3 ? vec3_i32 : vec2_i32, c->depth_type,
                                                       c->depth_sampler, level);
        size = spv_emit_select(&c->emit, coord_count == 3 ? vec3_i32 : vec2_i32, shader_indexed_is_depth(c, slot),
                               depth_size, size);
    }
    u32 moved[4];
    for (u32 k = 0; k < coord_count; k++) {
        u32 coord = spv_emit_vec4_extract(&c->emit, c->f32_type, coords, k);
        if (k >= offset_count) {
            moved[k] = coord;
            continue;
        }
        u32 delta = spv_emit_vec4_extract(&c->emit, i32_type, offset, k);
        u32 extent = spv_emit_vec4_extract(&c->emit, i32_type, size, k);
        moved[k] =
            spv_emit_fadd(&c->emit, c->f32_type, coord,
                          spv_emit_fdiv(&c->emit, c->f32_type, spv_emit_convert_s_to_f(&c->emit, c->f32_type, delta),
                                        spv_emit_convert_s_to_f(&c->emit, c->f32_type, extent)));
    }
    return coord_count == 3 ? spv_emit_vec3_construct(&c->emit, vec3, moved[0], moved[1], moved[2])
                            : spv_emit_vec2_construct(&c->emit, vec2, moved[0], moved[1]);
}

/* Gather AOFFI stores signed six-bit x/y offsets in the low bits of two consecutive bytes. */
u32 shader_direct_gather_offset(DirectCtx* c, u32 raw, u32 shift, int zero_y, u32 i32_type, u32 vec2_i32) {
    u32 bits = spv_emit_bitcast(&c->emit, i32_type, raw);
    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 6);
    u32 x = spv_emit_bitfield_sextract(&c->emit, i32_type, bits, spv_emit_constant_u32(&c->emit, c->u32_type, shift),
                                       width);
    u32 y = zero_y ? spv_emit_constant_u32(&c->emit, i32_type, 0)
                   : spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, shift + 8), width);
    return spv_emit_vec2_construct(&c->emit, vec2_i32, x, y);
}

/* TEXS's and TLDS's results: each component to its own register (bit 59), or as halves two to a register, the
   first pair to Rd and the second to bits 28-35's. A depth compare's one result is x, y and z, with 1.0 in alpha. */
void shader_texs_store(DirectCtx* c, u64 w, u32 sample, u32 scalar, int shadow, u32 i32_type, u32 vec2, u32 zero_f) {
    u32 regs[4], comps[4], d = B(w, 0, 8);
    int outputs = shader_texs_outputs(w, regs, comps);
    if (B(w, 59, 1)) {
        for (int k = 0; k < outputs; k++) {
            if (regs[k] >= 255) continue;
            reg_write(
                c, regs[k],
                shadow ? spv_emit_bitcast(&c->emit, c->u32_type,
                                          comps[k] == 3 ? spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f) : sample)
                       : shader_texture_component(c, sample, scalar, comps[k]));
        }
        return;
    }
    for (int k = 0; k < outputs; k += 2) {
        u32 pair[2];
        for (int lane = 0; lane < 2; lane++) {
            u32 value = k + lane >= outputs ? zero_f
                        : shadow ? (comps[k + lane] == 3 ? spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f) : sample)
                                 : spv_emit_vec4_extract(&c->emit, scalar, sample, comps[k + lane]);
            pair[lane] =
                shadow || k + lane >= outputs ? value : shader_texture_float_component(c, value, scalar, i32_type);
        }
        u32 packed = shader_pack_texture_halves(c, vec2, pair[0], pair[1]);
        u32 target = k == 0 ? d : B(w, 28, 8);
        if (target < 255) reg_write(c, target, packed);
    }
}
