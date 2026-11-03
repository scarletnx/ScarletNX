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

/* The texture slot a TXQ queries, bound or by a tracked handle, when nothing else says what the texture is
   (shader_direct_query_type): of the type the key's hint gave it (ShaderInfo.tex_query_hint), 2D without one */
int shader_direct_query_slot(const ShTex* textures, int count, u32 off, int bindless, u32 bank, int handle_count,
                             u32 bank2, u32 off2, u32 indexed, u32 ldc) {
    int slot = 0;
    while (slot < count && (textures[slot].cbuf_off != off || textures[slot].shadow || (textures[slot].flags & 3) ||
                            textures[slot].bindless != bindless ||
                            (bindless && !shader_direct_bindless_matches(&textures[slot], bank, handle_count, bank2,
                                                                         off2, indexed, ldc))))
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

/* the texture and surface instructions */
void emit_texture(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b) {
    const u64* insn = t->insn;
    int stage = t->stage;
    ShaderInfo* si = t->si;
    u32 n = t->n;
    ShTex* textures = t->textures;
    int texture_count = t->texture_count;
    const ShaderInfo* image_info = t->image_info;
    int needs_texel_scale = t->needs_texel_scale;
    u64 rect_slots = t->rect_slots;
    DirectCtx* c = t->c;
    u32 i32_type = t->i32_type;
    u32 vec4 = t->vec4;
    u32 vec4_u32 = t->vec4_u32;
    u32 vec4_i32 = t->vec4_i32;
    u32 vec2 = t->vec2;
    u32 vec2f16 = t->vec2f16;
    u32 vec2_u32 = t->vec2_u32;
    u32 vec2_i32 = t->vec2_i32;
    u32 vec3 = t->vec3;
    u32 vec3_u32 = t->vec3_u32;
    u32 vec3_i32 = t->vec3_i32;
    u32 zero_f = t->zero_f;
    u32* texture_vec4 = t->texture_vec4;
    u32* texture_scalar = t->texture_scalar;
    u32 push = t->push;
    u32 atomic_old_var = t->atomic_old_var;
    if (op == O_TLD || op == O_TLD_B) {
        u32 indexed = 0, ldc = 0;
        u32 off = B(w, 36, 13), ty = B(w, 28, 3);
        u32 bank = 0, bank2 = 0, off2 = 0;
        int handle_count =
            op == O_TLD_B ? shader_direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
        if (handle_count == 1) indexed = shader_direct_indexed_handle(insn, n, i, b, stage, &ldc);
        int type = ty >> 1, array = ty & 1, multisample = (int)B(w, 50, 1), slot = 0;
        while (slot < texture_count &&
               (textures[slot].cbuf_off != off || textures[slot].type != type || textures[slot].array != array ||
                textures[slot].shadow || textures[slot].bindless != (op == O_TLD_B) ||
                !!(textures[slot].flags & 2) != multisample ||
                (op == O_TLD_B &&
                 !shader_direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
            slot++;
        if (textures[slot].flags & SNX_TEX_INDEXED) shader_direct_swap_element(c, slot);
        u32 x = reg_read_as(c, a + array, i32_type);
        u32 y = type == 0 ? spv_emit_constant_u32(&c->emit, i32_type, 0) : reg_read_as(c, a + array + 1, i32_type);
        /* Rb's operands, in order: TLD.B's handle, the level (bit 55), the offsets (bit 35) and the sample (bit 50) */
        u32 meta = b + (op == O_TLD_B), level = B(w, 55, 1) ? meta++ : 255, packed_reg = B(w, 35, 1) ? meta++ : 255;
        u32 z = type <= 1 && !array ? 0
                : type == 0 || array
                    ? spv_emit_bitcast(&c->emit, i32_type,
                                       spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reg_read(c, a),
                                                                spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff)))
                    : reg_read_as(c, a + 2, i32_type);
        if (packed_reg < 255) { /* 4-bit signed offsets: x, then y but for 1D, then z for 3D */
            u32 packed = reg_read_as(c, packed_reg, i32_type);
            u32 four = spv_emit_constant_u32(&c->emit, c->u32_type, 4);
            x = spv_emit_iadd_u32(&c->emit, i32_type, x,
                                  spv_emit_bitfield_sextract(&c->emit, i32_type, packed, c->zero, four));
            if (type >= 1)
                y = spv_emit_iadd_u32(&c->emit, i32_type, y,
                                      spv_emit_bitfield_sextract(&c->emit, i32_type, packed, four, four));
            if (type == 2)
                z = spv_emit_iadd_u32(&c->emit, i32_type, z,
                                      spv_emit_bitfield_sextract(&c->emit, i32_type, packed,
                                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 8),
                                                                 four));
        }
        if (type == 0 && !array) shader_texel_row_coord(c, i32_type, &x, &y);
        if (type == 1 && needs_texel_scale) shader_scaled_coords(c, push, slot, 0, i32_type, &x, &y);
        u32 coord = z ? spv_emit_vec3_construct(&c->emit, vec3_i32, x, y, z)
                      : spv_emit_vec2_construct(&c->emit, vec2_i32, x, y);
        u32 sample = multisample ? spv_emit_image_fetch_sample(&c->emit, texture_vec4[slot], c->sampled_type[slot],
                                                               c->sampler[slot], coord, reg_read_as(c, meta, i32_type))
                                 : spv_emit_image_fetch(&c->emit, texture_vec4[slot], c->sampled_type[slot],
                                                        c->sampler[slot], coord,
                                                        level < 255 ? reg_read_as(c, level, i32_type)
                                                                    : spv_emit_constant_u32(&c->emit, i32_type, 0));
        u32 mask = B(w, 31, 4), target = d;
        for (u32 k = 0; k < 4; k++)
            if (mask & (1u << k)) {
                reg_write(c, target, shader_texture_component(c, sample, texture_scalar[slot], k));
                if (target < 255) target++;
            }
    } else if (op == O_SULD || op == O_SUST || op == O_SURED || op == O_SUATOM) {
        int dim = (int)B(w, 33, 3), buffer = dim == 1;
        u32 bank = 0, off = B(w, 36, 13);
        int bindless = op == O_SUATOM ? (int)B(w, 54, 1) : !B(w, 51, 1);
        if (bindless) shader_direct_track_image_handle(insn, n, i, B(w, 39, 8), &bank, &off, 0);
        u32 x = reg_read_as(c, a, i32_type), coord = x;
        if (!buffer) {
            u32 y = reg_read_as(c, a + 1, i32_type);
            /* a 2D array's layer is the low 16 bits of the third register, as TLD's is, and a 3D surface's z the
               whole register (Eden's MakeCoords in surface_load_store.cpp reads them the same way) */
            coord = dim == 4   ? spv_emit_vec3_construct(
                                     &c->emit, vec3_i32, x, y,
                                     spv_emit_bitcast(
                                         &c->emit, i32_type,
                                         spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reg_read(c, a + 2),
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff))))
                    : dim == 5 ? spv_emit_vec3_construct(&c->emit, vec3_i32, x, y, reg_read_as(c, a + 2, i32_type))
                               : spv_emit_vec2_construct(&c->emit, vec2_i32, x, y);
        }
        int raw = op == O_SULD && B(w, 52, 1);
        u32 mask = raw ? 1u : B(w, 20, 4), source = d;
        if (op != O_SULD || buffer) {
            int slot =
                shader_direct_image_slot(image_info, off, bindless, bank, op != O_SUST && (op != O_SULD || raw), dim);
            int kind = image_info->img[slot].kind;
            if (op == O_SULD && kind == 1) {
                u32 texel = spv_emit_image_read(&c->emit, vec4_u32, c->image_type[slot], c->image[slot], coord);
                reg_write(c, d, spv_emit_vec4_extract(&c->emit, c->u32_type, texel, 0));
            } else if (op == O_SULD) { /* formatted: the view's format converts each component, as a load does */
                u32 scalar = kind == 3 ? i32_type : kind == 2 ? c->u32_type : c->f32_type, target = d;
                spv_emit_capability(&c->emit, SPV_CAPABILITY_STORAGE_IMAGE_READ_WITHOUT_FORMAT);
                u32 texel = spv_emit_image_read(&c->emit,
                                                kind == 3   ? vec4_i32
                                                : kind == 2 ? vec4_u32
                                                            : vec4,
                                                c->image_type[slot], c->image[slot], coord);
                for (u32 q = 0; q < 4; q++)
                    if (mask & (1u << q)) {
                        reg_write_as(c, target, spv_emit_vec4_extract(&c->emit, scalar, texel, q), scalar);
                        if (target < 255) target++;
                    }
            } else if (kind == 1) {
                /* SURED's operations (bits 24-26) and SUATOM's (29-32) are numbered alike: ADD, MIN, MAX, INC,
                   DEC, AND, OR, XOR, and SUATOM's EXCH. Only S32 (size 1 in both) compares signed */
                static const u32 opcodes[9] = {234, 237, 239, 0, 0, 240, 241, 242, 229};
                u32 operation = op == O_SURED ? B(w, 24, 3) : B(w, 29, 4);
                u32 opcode = opcodes[operation];
                if (B(w, op == O_SURED ? 20 : 51, 3) == 1 && (operation == 1 || operation == 2)) opcode--;
                u32 pointer = spv_emit_image_texel_pointer(&c->emit, c->u32_type, c->image[slot], coord, c->zero);
                u32 value = reg_read(c, op == O_SUATOM ? b : source);
                u32 prior = operation == 3 || operation == 4
                                ? shader_atomic_wrap_loop(c, atomic_old_var, pointer, value, operation == 4)
                                : spv_emit_atomic_u32(&c->emit, c->u32_type, opcode, pointer, value);
                if (op == O_SUATOM) reg_write(c, d, prior);
            } else {
                u32 components[4];
                u32 scalar = kind == 3 ? i32_type : kind == 2 ? c->u32_type : c->f32_type;
                for (u32 q = 0; q < 4; q++) {
                    components[q] = kind ? spv_emit_constant_u32(&c->emit, scalar, q == 3 ? 1 : 0)
                                         : spv_emit_constant_f32(&c->emit, c->f32_type, q == 3 ? 1.0f : 0.0f);
                    if (mask & (1u << q)) {
                        components[q] = reg_read_as(c, source, scalar);
                        if (source < 255) source++;
                    }
                }
                u32 texel = spv_emit_vec4_construct(&c->emit,
                                                    kind == 3   ? vec4_i32
                                                    : kind == 2 ? vec4_u32
                                                                : vec4,
                                                    components);
                spv_emit_image_write(&c->emit, c->image_type[slot], c->image[slot], coord, texel);
            }
        } else {
            int slot = 0;
            int sample_type = dim == 5 ? 2 : 1, arrayed = dim == 4;
            while (slot < texture_count &&
                   (textures[slot].cbuf_off != off || textures[slot].type != sample_type ||
                    textures[slot].array != arrayed || textures[slot].shadow || textures[slot].bindless != bindless ||
                    (textures[slot].flags & 3) != (B(w, 52, 1) ? 1 : 0) ||
                    (bindless && textures[slot].bl_cbuf != bank)))
                slot++;
            u32 sample = spv_emit_image_fetch(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot],
                                              coord, spv_emit_constant_u32(&c->emit, i32_type, 0));
            for (u32 q = 0; q < 4; q++)
                if (mask & (1u << q)) {
                    reg_write(c, source, shader_texture_component(c, sample, texture_scalar[slot], q));
                    if (source < 255) source++;
                }
        }
    } else if (op == O_TLDS) {
        u32 off = B(w, 36, 13);
        u32 encoding = B(w, 53, 4);
        int array = encoding == 8, type = encoding == 0 || encoding == 1 ? 0 : encoding == 7 ? 2 : 1;
        int multisample = encoding == 6, slot = 0;
        u32 x;
        u32 y;
        while (slot < texture_count &&
               (textures[slot].cbuf_off != off || textures[slot].type != type || textures[slot].array != array ||
                textures[slot].shadow || textures[slot].bindless || textures[slot].flags != (multisample ? 2 : 0)))
            slot++;
        /* the 2D array form takes x and y from Rb and Rb + 1, the layer from Ra's low 16 bits */
        x = reg_read_as(c, array ? b : a, i32_type);
        y = type == 0 ? spv_emit_constant_u32(&c->emit, i32_type, 0)
                      : spv_emit_bitcast(&c->emit, i32_type,
                                         reg_read(c, array ? b + 1
                                                     : encoding == 4 || encoding == 5 || encoding == 6 ||
                                                             encoding == 7 || encoding == 12
                                                         ? a + 1
                                                         : b));
        if (encoding == 4 || encoding == 12) {
            u32 packed = spv_emit_bitcast(&c->emit, i32_type, reg_read(c, b + (encoding == 12)));
            u32 four = spv_emit_constant_u32(&c->emit, c->u32_type, 4);
            u32 ox = spv_emit_bitfield_sextract(&c->emit, i32_type, packed, c->zero, four);
            u32 oy = spv_emit_bitfield_sextract(&c->emit, i32_type, packed, four, four);
            x = spv_emit_iadd_u32(&c->emit, i32_type, x, ox);
            y = spv_emit_iadd_u32(&c->emit, i32_type, y, oy);
        }
        if (type == 0) shader_texel_row_coord(c, i32_type, &x, &y);
        if (type == 1 && needs_texel_scale) shader_scaled_coords(c, push, slot, 0, i32_type, &x, &y);
        u32 coord =
            type == 2 ? spv_emit_vec3_construct(&c->emit, vec3_i32, x, y, reg_read_as(c, b, i32_type))
            : array
                ? spv_emit_vec3_construct(
                      &c->emit, vec3_i32, x, y,
                      spv_emit_bitcast(&c->emit, i32_type,
                                       spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reg_read(c, a),
                                                                spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff))))
                : spv_emit_vec2_construct(&c->emit, vec2_i32, x, y);
        u32 lod = encoding == 1 || encoding == 5 || encoding == 12 ? reg_read(c, b) : c->zero;
        u32 sample = multisample ? spv_emit_image_fetch_sample(&c->emit, texture_vec4[slot], c->sampled_type[slot],
                                                               c->sampler[slot], coord, reg_read(c, b))
                                 : spv_emit_image_fetch(&c->emit, texture_vec4[slot], c->sampled_type[slot],
                                                        c->sampler[slot], coord, lod);
        if (texture_scalar[slot] == c->f32_type && !B(w, 59, 1)) {
            u32 regs[4], comps[4], halves[4];
            int outputs = shader_texs_outputs(w, regs, comps);
            for (int k = 0; k < outputs; k++)
                halves[k] = spv_emit_vec4_extract(&c->emit, c->f32_type, sample, comps[k]);
            u32 lo = spv_emit_vec2_construct(&c->emit, vec2, outputs > 0 ? halves[0] : zero_f,
                                             outputs > 1 ? halves[1] : zero_f);
            lo = spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_convert_f_to_f(&c->emit, vec2f16, lo));
            if (d < 255) reg_write(c, d, lo);
            if (outputs > 2) {
                u32 db = B(w, 28, 8);
                u32 hi = spv_emit_vec2_construct(&c->emit, vec2, halves[2], outputs > 3 ? halves[3] : zero_f);
                hi = spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_convert_f_to_f(&c->emit, vec2f16, hi));
                if (db < 255) reg_write(c, db, hi);
            }
        } else {
            shader_texs_store(c, w, sample, texture_scalar[slot], 0, i32_type, vec2, zero_f);
        }
    } else if (op == O_TEXS) {
        u32 off = B(w, 36, 13);
        u32 encoding = B(w, 53, 4);
        int shadow = shader_texs_shadow(encoding), type = shader_texs_type(encoding),
            array = shader_texs_array(encoding);
        int slot = 0;
        while (slot < texture_count &&
               (textures[slot].cbuf_off != off || textures[slot].shadow != shadow || textures[slot].type != type ||
                textures[slot].array != array || textures[slot].bindless || textures[slot].flags))
            slot++;
        u32 x = reg_read_as(c, array ? a + 1 : a, c->f32_type);
        u32 y = encoding == 0 ? spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f)
                              : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, shader_texs_y(encoding, a, b)));
        if (rect_slots >> slot & 1)
            shader_rect_coords(c, push, stage == ST_FS, slot, vec2_u32, i32_type, &x, &y, NULL, NULL);
        u32 uv = array       ? spv_emit_vec3_construct(&c->emit, vec3, x, y, shader_array_layer(c, a))
                 : type >= 2 ? spv_emit_vec3_construct(&c->emit, vec3, x, y, reg_read_as(c, b, c->f32_type))
                             : spv_emit_vec2_construct(&c->emit, vec2, x, y);
        u32 dref = shadow ? spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, shader_texs_dref(encoding, b))) : 0;
        u32 sample;
        int explicit_lod = shader_texs_explicit(encoding) || stage != ST_FS;
        u32 level = shader_texs_explicit(encoding) ? shader_texs_level(encoding, b) : 255;
        u32 lod = level < 255 ? reg_read_as(c, level, c->f32_type) : zero_f;
        sample = shader_sample_dref(c, si, slot, texture_vec4[slot], uv, dref, explicit_lod, lod);
        shader_texs_store(c, w, sample, texture_scalar[slot], shadow, i32_type, vec2, zero_f);
    } else if (op == O_TEX || op == O_TEX_B) {
        int bindless = op == O_TEX_B;
        u32 mode = bindless ? B(w, 37, 3) : B(w, 55, 3);
        u32 offset_enabled = B(w, bindless ? 36 : 54, 1);
        u32 source = b + bindless;
        u32 has_level = mode == 2 || mode == 3 || mode == 6 || mode == 7;
        u32 offset_source = source + has_level;
        u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
        int handle_count =
            bindless ? shader_direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
        int type = B(w, 28, 3) >> 1;
        int array = B(w, 28, 1), shadow = B(w, 50, 1);
        int slot = 0;
        if (handle_count == 1) indexed = shader_direct_indexed_handle(insn, n, i, b, stage, &ldc);
        while (slot < texture_count &&
               (textures[slot].cbuf_off != off || textures[slot].shadow != shadow || textures[slot].type != type ||
                textures[slot].array != array || textures[slot].bindless != bindless || (textures[slot].flags & 3) ||
                (bindless &&
                 !shader_direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
            slot++;
        if (textures[slot].flags & SNX_TEX_INDEXED) shader_direct_swap_element(c, slot);
        u32 x = reg_read_as(c, a + array, c->f32_type);
        u32 y =
            type == 0 ? spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f) : reg_read_as(c, a + array + 1, c->f32_type);
        if (rect_slots >> slot & 1)
            shader_rect_coords(c, push, stage == ST_FS, slot, vec2_u32, i32_type, &x, &y, NULL, NULL);
        u32 coords =
            type == 3 && array
                ? spv_emit_vec4_construct(&c->emit, vec4,
                                          (u32[4]){x, y, reg_read_as(c, a + 3, c->f32_type), shader_array_layer(c, a)})
            : array     ? spv_emit_vec3_construct(&c->emit, vec3, x, y, shader_array_layer(c, a))
            : type >= 2 ? spv_emit_vec3_construct(&c->emit, vec3, x, y, reg_read_as(c, a + 2, c->f32_type))
                        : spv_emit_vec2_construct(&c->emit, vec2, x, y);
        u32 level = mode == 1 ? zero_f : has_level ? reg_read_as(c, source, c->f32_type) : 0;
        u32 dref = shadow ? reg_read_as(c, offset_source + offset_enabled, c->f32_type) : 0;
        u32 offset = 0;
        int dynamic_offset = 0;
        if (offset_enabled) {
            u32 packed = 0;
            int known = shader_direct_track_const(insn, n, i, offset_source, &packed, 0);
            u32 bits;
            dynamic_offset = !known;
            bits = known ? 0 : reg_read(c, offset_source);
            if (!known) bits = spv_emit_bitcast(&c->emit, i32_type, bits);
            u32 ox = known ? spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)(packed << 28) >> 28))
                           : spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                        spv_emit_constant_u32(&c->emit, c->u32_type, 0),
                                                        spv_emit_constant_u32(&c->emit, c->u32_type, 4));
            u32 oy = known ? spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)(packed << 24) >> 28))
                           : spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                        spv_emit_constant_u32(&c->emit, c->u32_type, 4),
                                                        spv_emit_constant_u32(&c->emit, c->u32_type, 4));
            if (type == 2) {
                u32 oz = known ? spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)(packed << 20) >> 28))
                               : spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 8),
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 4));
                offset = known ? spv_emit_constant_vec3(&c->emit, vec3_i32, ox, oy, oz)
                               : spv_emit_vec3_construct(&c->emit, vec3_i32, ox, oy, oz);
            } else {
                if (type == 0) oy = spv_emit_constant_u32(&c->emit, i32_type, 0);
                offset = known ? spv_emit_constant_vec2(&c->emit, vec2_i32, ox, oy)
                               : spv_emit_vec2_construct(&c->emit, vec2_i32, ox, oy);
            }
        }
        /* Vulkan takes a non-constant Offset on gathers only: a sample moves its coordinates instead, by the
           offset over the level's size (1D textures are 2D images one texel high, their y offset 0) */
        if (dynamic_offset) {
            u32 moved = type == 2 ? 3 : 2, components = moved + (u32)array;
            /* Bias is not a mip level. Explicit negative levels query the base level. */
            u32 size_level = mode == 3 || mode == 7 ? level : zero_f;
            size_level =
                spv_emit_select(&c->emit, c->f32_type, spv_emit_fcompare(&c->emit, c->bool_type, 6, size_level, zero_f),
                                size_level, zero_f);
            u32 lod_index = spv_emit_convert_f_to_s(&c->emit, i32_type, size_level);
            coords = shader_direct_offset_coords(c, coords, offset, components, moved, slot, lod_index, vec2, vec3,
                                                 vec2_i32, vec3_i32, i32_type);
            offset = 0;
            dynamic_offset = 0;
        }
        /* outside the fragment stage there are no derivatives: LOD 0, or the bias as the level */
        u32 sample_mode = mode == 1 || mode == 3 || mode == 7 || stage != ST_FS ? 1 : mode == 2 || mode == 6 ? 2 : 0;
        if (sample_mode == 1 && !level) level = zero_f;
        /* a depth compare of a color texture samples it plainly and compares its first component here */
        int indexed_dc = dref && shadow && (textures[slot].flags & SNX_TEX_INDEXED);
        int dc_func = indexed_dc ? 3 : dref ? shader_dc_color_func(si, slot) : -1;
        u32 sample =
            dc_func < 0
                ? spv_emit_sample_image(&c->emit, shadow ? c->f32_type : texture_vec4[slot], c->sampled_type[slot],
                                        c->sampler[slot], coords, dref, sample_mode, level, offset, dynamic_offset)
                : (indexed_dc ? shader_dc_compare_indexed : shader_dc_compare)(
                      c, indexed_dc ? slot : dc_func, dref,
                      spv_emit_vec4_extract(&c->emit, c->f32_type,
                                            spv_emit_sample_image(&c->emit, texture_vec4[slot], c->sampled_type[slot],
                                                                  c->sampler[slot], coords, 0, sample_mode, level,
                                                                  offset, dynamic_offset),
                                            0));
        if (indexed_dc) {
            u32 depth = spv_emit_sample_image(&c->emit, c->f32_type, c->depth_type, c->depth_sampler, coords, dref,
                                              sample_mode, level, offset, dynamic_offset);
            sample = spv_emit_select(&c->emit, c->f32_type, shader_indexed_is_depth(c, slot), depth, sample);
        }
        u32 mask = B(w, 31, 4), dst = d;
        for (u32 component = 0; component < 4; component++)
            if (mask & (1u << component)) {
                u32 value =
                    shadow
                        ? spv_emit_bitcast(&c->emit, c->u32_type,
                                           component == 3 ? spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f) : sample)
                        : shader_texture_component(c, sample, texture_scalar[slot], component);
                reg_write(c, dst, value);
                if (dst < 255) dst++;
            }
    } else if (op == O_TXD || op == O_TXD_B) {
        int bindless = op == O_TXD_B;
        u32 ty = B(w, 28, 3), type = ty >> 1, array = ty & 1, count = type == 0 ? 1 : type == 1 ? 2 : 3;
        u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
        int handle_count =
            bindless ? shader_direct_track_image_handle_pair(insn, n, i, a, &bank, &off, &bank2, &off2, 0) : 0;
        if (handle_count == 1) indexed = shader_direct_indexed_handle(insn, n, i, a, stage, &ldc);
        int slot = shader_direct_typed_slot(textures, texture_count, off, type, array, 0, 0, bindless, bank,
                                            handle_count, bank2, off2, indexed, ldc);
        if (textures[slot].flags & SNX_TEX_INDEXED) shader_direct_swap_element(c, slot);
        u32 coord = a + (u32)bindless, packed_reg = coord + count, values[4], dx[3], dy[3], k;
        /* a 1D texture is bound as a 2D one of height 1: y 0.5, and no change along it */
        u32 dims = type == 0 ? 2 : count;
        for (k = 0; k < count; k++) {
            values[k] = reg_read_as(c, coord + k, c->f32_type);
            dx[k] = reg_read_as(c, b + 2 * k, c->f32_type);
            dy[k] = reg_read_as(c, b + 2 * k + 1, c->f32_type);
        }
        if (type == 0) {
            values[1] = spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f);
            dx[1] = dy[1] = zero_f;
        }
        if (rect_slots >> slot & 1)
            shader_rect_coords(c, push, stage == ST_FS, slot, vec2_u32, i32_type, &values[0], &values[1], dx, dy);
        if (array) values[dims] = shader_array_layer(c, packed_reg);
        u32 coords = dims + array == 4   ? spv_emit_vec4_construct(&c->emit, vec4, values)
                     : dims + array == 3 ? spv_emit_vec3_construct(&c->emit, vec3, values[0], values[1], values[2])
                                         : spv_emit_vec2_construct(&c->emit, vec2, values[0], values[1]);
        u32 offset = 0;
        if (B(w, 35, 1)) { /* signed 4-bit offsets from bit 16 of the packed register */
            u32 packed = 0, o[3];
            if (shader_direct_track_const(insn, n, i, packed_reg, &packed, 0)) {
                for (k = 0; k < 3; k++)
                    o[k] = spv_emit_constant_u32(&c->emit, i32_type,
                                                 k < count ? (u32)((i32)(packed << (12 - 4 * k)) >> 28) : 0);
                offset = dims == 3 ? spv_emit_constant_vec3(&c->emit, vec3_i32, o[0], o[1], o[2])
                                   : spv_emit_constant_vec2(&c->emit, vec2_i32, o[0], o[1]);
            } else {
                /* Vulkan takes a non-constant offset on gathers only. TXD moves coordinates by the offset over
                   the base level's size as an approximation. */
                u32 bits = reg_read_as(c, packed_reg, i32_type), o[3];
                for (k = 0; k < dims; k++)
                    o[k] = k < count
                               ? spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 16 + 4 * k),
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 4))
                               : spv_emit_constant_u32(&c->emit, i32_type, 0);
                u32 dynamic_offset = dims == 3 ? spv_emit_vec3_construct(&c->emit, vec3_i32, o[0], o[1], o[2])
                                               : spv_emit_vec2_construct(&c->emit, vec2_i32, o[0], o[1]);
                coords = shader_direct_offset_coords(c, coords, dynamic_offset, dims + array, count, slot,
                                                     spv_emit_constant_u32(&c->emit, i32_type, 0), vec2, vec3, vec2_i32,
                                                     vec3_i32, i32_type);
            }
        }
        u32 gradient_x = dims == 3 ? spv_emit_vec3_construct(&c->emit, vec3, dx[0], dx[1], dx[2])
                                   : spv_emit_vec2_construct(&c->emit, vec2, dx[0], dx[1]);
        u32 gradient_y = dims == 3 ? spv_emit_vec3_construct(&c->emit, vec3, dy[0], dy[1], dy[2])
                                   : spv_emit_vec2_construct(&c->emit, vec2, dy[0], dy[1]);
        u32 sample = spv_emit_sample_grad(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot], coords,
                                          gradient_x, gradient_y, offset);
        u32 mask = B(w, 31, 4), dst = d;
        for (u32 component = 0; component < 4; component++)
            if (mask & (1u << component)) {
                reg_write(c, dst, shader_texture_component(c, sample, texture_scalar[slot], component));
                if (dst < 255) dst++;
            }
    } else if (op == O_TXQ || op == O_TXQ_B) {
        int bindless = op == O_TXQ_B;
        u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
        int handle_count =
            bindless ? shader_direct_track_image_handle_pair(insn, n, i, a, &bank, &off, &bank2, &off2, 0) : 0;
        int slot;
        u32 qtype = 1, qarray = 0;
        if (handle_count == 1) indexed = shader_direct_indexed_handle(insn, n, i, a, stage, &ldc);
        if (!indexed && shader_direct_query_type(insn, n, bindless, bank, off, &qtype, &qarray))
            slot = shader_direct_typed_slot(textures, texture_count, off, qtype, qarray, 0, 0, bindless, bank,
                                            handle_count, bank2, off2, indexed, ldc);
        else {
            slot = shader_direct_query_slot(textures, texture_count, off, bindless, bank, handle_count, bank2, off2,
                                            indexed, ldc);
            if (slot < texture_count) { /* the key's hint typed it (tex_query_hint); 2D without one */
                qtype = textures[slot].type;
                qarray = textures[slot].array;
            }
        }
        if (textures[slot].flags & SNX_TEX_INDEXED) shader_direct_swap_element(c, slot);
        /* a 3D texture's depth, or an array's layers, is the third component; a 1D texture is bound as a 2D one of
           height 1, a cube's faces are no layers */
        int third = qtype == 2 || qarray;
        u32 dimensions = spv_emit_image_query_size_lod(&c->emit, third ? vec3_u32 : vec2_u32, c->sampled_type[slot],
                                                       c->sampler[slot], reg_read(c, a + (u32)bindless));
        u32 values[4] = {spv_emit_vec4_extract(&c->emit, c->u32_type, dimensions, 0),
                         spv_emit_vec4_extract(&c->emit, c->u32_type, dimensions, 1),
                         third ? spv_emit_vec4_extract(&c->emit, c->u32_type, dimensions, 2)
                               : spv_emit_constant_u32(&c->emit, c->u32_type, 1),
                         spv_emit_image_query_levels(&c->emit, c->u32_type, c->sampled_type[slot], c->sampler[slot])};
        if (qtype == 1 && needs_texel_scale) shader_scaled_coords(c, push, slot, 1, i32_type, &values[0], &values[1]);
        u32 dst = d;
        for (u32 k = 0; k < 4; k++)
            if (B(w, 31 + k, 1)) {
                reg_write(c, dst, values[k]);
                if (dst < 255) dst++;
            }
    } else if (op == O_TMML || op == O_TMML_B) {
        int bindless = op == O_TMML_B;
        u32 ty = B(w, 28, 3), type = ty >> 1, array = ty & 1;
        u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
        int handle_count =
            bindless ? shader_direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
        if (handle_count == 1) indexed = shader_direct_indexed_handle(insn, n, i, b, stage, &ldc);
        int slot = shader_direct_typed_slot(textures, texture_count, off, type, array, 0, 0, bindless, bank,
                                            handle_count, bank2, off2, indexed, ldc);
        if (textures[slot].flags & SNX_TEX_INDEXED) shader_direct_swap_element(c, slot);
        /* a 1D texture is bound as a 2D one of height 1 */
        u32 x = reg_read_as(c, a + array, c->f32_type);
        u32 y =
            type == 0 ? spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f) : reg_read_as(c, a + array + 1, c->f32_type);
        if (rect_slots >> slot & 1)
            shader_rect_coords(c, push, stage == ST_FS, slot, vec2_u32, i32_type, &x, &y, NULL, NULL);
        u32 coords = type < 2
                         ? spv_emit_vec2_construct(&c->emit, vec2, x, y)
                         : spv_emit_vec3_construct(&c->emit, vec3, x, y, reg_read_as(c, a + array + 2, c->f32_type));
        u32 lod = spv_emit_image_query_lod(&c->emit, vec2, c->sampled_type[slot], c->sampler[slot], coords);
        u32 mask = B(w, 31, 4), dst = d;
        for (u32 component = 0; component < 4; component++) {
            if (!(mask & (1u << component))) continue;
            u32 value = c->zero;
            if (component < 2) {
                u32 f = spv_emit_vec4_extract(&c->emit, c->f32_type, lod, 1 - component);
                f = spv_emit_fmul(&c->emit, c->f32_type, f, spv_emit_constant_f32(&c->emit, c->f32_type, 256.0f));
                value = spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_convert_f_to_s(&c->emit, i32_type, f));
            }
            reg_write(c, dst, value);
            if (dst < 255) dst++;
        }
    } else if (op == O_TLD4S || op == O_TLD4 || op == O_TLD4_B) {
        u32 indexed = 0, ldc = 0;
        int bindless = op == O_TLD4_B;
        u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0;
        int handle_count =
            bindless ? shader_direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
        if (handle_count == 1) indexed = shader_direct_indexed_handle(insn, n, i, b, stage, &ldc);
        u32 ty = op == O_TLD4S ? 2 : B(w, 28, 3);
        u32 type = ty >> 1, array = ty & 1;
        u32 shadow = B(w, 50, 1);
        int slot = 0;
        while (slot < texture_count &&
               (textures[slot].cbuf_off != off || textures[slot].type != type || textures[slot].array != array ||
                textures[slot].shadow != shadow || textures[slot].bindless != bindless ||
                (bindless &&
                 !shader_direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
            slot++;
        if (textures[slot].flags & SNX_TEX_INDEXED) shader_direct_swap_element(c, slot);
        u32 x = reg_read_as(c, a + array, c->f32_type);
        u32 y = type == 0 ? spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f)
                          : spv_emit_bitcast(&c->emit, c->f32_type,
                                             reg_read(c, op == O_TLD4S && !shadow && !B(w, 51, 1) ? b : a + array + 1));
        if (rect_slots >> slot & 1)
            shader_rect_coords(c, push, stage == ST_FS, slot, vec2_u32, i32_type, &x, &y, NULL, NULL);
        u32 coords;
        if (!array && type != 3)
            coords = spv_emit_vec2_construct(&c->emit, vec2, x, y);
        else {
            u32 z = type == 3 ? reg_read_as(c, a + array + 2, c->f32_type) : shader_array_layer(c, a);
            if (type == 3 && array) {
                u32 four[4];
                four[0] = x;
                four[1] = y;
                four[2] = z;
                four[3] = shader_array_layer(c, a);
                coords = spv_emit_vec4_construct(&c->emit, vec4, four);
            } else
                coords = spv_emit_vec3_construct(&c->emit, vec3, x, y, z);
        }
        u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, op == O_TLD4S ? 52 : bindless ? 38 : 56, 2));
        u32 offset = 0, dref = 0;
        int per_tap_offset = 0;
        u32 ptp_source = 0;
        if (op != O_TLD4S) {
            u32 source = b + bindless;
            u32 offset_mode = B(w, bindless ? 36 : 54, 2);
            if (offset_mode == 1) {
                /* a 1D texture is a 2D one a texel high, which doesn't move in y */
                offset = shader_direct_gather_offset(c, reg_read(c, source++), 0, type == 0, i32_type, vec2_i32);
            } else if (offset_mode == 2) {
                per_tap_offset = 1;
                ptp_source = source;
                source += 2;
            }
            if (shadow) dref = reg_read_as(c, source, c->f32_type);
        } else {
            if (B(w, 51, 1)) { offset = shader_direct_gather_offset(c, reg_read(c, b), 0, 0, i32_type, vec2_i32); }
            if (shadow) dref = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b + B(w, 51, 1)));
        }
        /* a compare gather of a color texture gathers its first components and compares them in the shader */
        int indexed_dc = dref && shadow && (textures[slot].flags & SNX_TEX_INDEXED);
        int dc_func = indexed_dc ? 3 : dref ? shader_dc_color_func(si, slot) : -1;
        if (dc_func >= 0) component = c->zero;
        u32 gathered;
        if (per_tap_offset) {
            u32 taps[4];
            /* tap k's offset is (x, y) in bytes 2(k % 2) and 2(k % 2) + 1 of register k / 2, and its texel is the
               i0j0 one of a gather at that offset, its w */
            for (u32 tap = 0; tap < 4; tap++) {
                u32 tap_offset = shader_direct_gather_offset(c, reg_read(c, ptp_source + tap / 2), (tap & 1) * 16,
                                                             type == 0, i32_type, vec2_i32);
                u32 sampled =
                    spv_emit_image_gather_ex(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot],
                                             coords, component, dc_func < 0 ? dref : 0, tap_offset);
                taps[tap] = spv_emit_vec4_extract(&c->emit, texture_scalar[slot], sampled, 3);
                if (dc_func >= 0)
                    taps[tap] = indexed_dc ? shader_dc_compare_indexed(c, slot, dref, taps[tap])
                                           : shader_dc_compare(c, dc_func, dref, taps[tap]);
                if (indexed_dc) {
                    u32 depth = spv_emit_image_gather_ex(&c->emit, texture_vec4[slot], c->depth_type, c->depth_sampler,
                                                         coords, component, dref, tap_offset);
                    taps[tap] = spv_emit_select(&c->emit, c->f32_type, shader_indexed_is_depth(c, slot),
                                                spv_emit_vec4_extract(&c->emit, c->f32_type, depth, 3), taps[tap]);
                }
            }
            gathered = spv_emit_vec4_construct(&c->emit, texture_vec4[slot], taps);
        } else {
            gathered = spv_emit_image_gather_ex(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot],
                                                coords, component, dc_func < 0 ? dref : 0, offset);
            if (dc_func >= 0) { /* each gathered first component against the reference */
                u32 lanes[4];
                for (u32 lane = 0; lane < 4; lane++)
                    lanes[lane] = indexed_dc
                                      ? shader_dc_compare_indexed(
                                            c, slot, dref, spv_emit_vec4_extract(&c->emit, c->f32_type, gathered, lane))
                                      : shader_dc_compare(c, dc_func, dref,
                                                          spv_emit_vec4_extract(&c->emit, c->f32_type, gathered, lane));
                gathered = spv_emit_vec4_construct(&c->emit, texture_vec4[slot], lanes);
            }
        }
        if (indexed_dc && !per_tap_offset) {
            u32 depth = spv_emit_image_gather_ex(&c->emit, texture_vec4[slot], c->depth_type, c->depth_sampler, coords,
                                                 component, dref, offset);
            gathered = spv_emit_select(&c->emit, texture_vec4[slot], shader_indexed_is_depth(c, slot), depth, gathered);
        }
        if (op == O_TLD4S) {
            u32 db = B(w, 28, 8);
            if (B(w, 55, 1)) {
                u32 components[4];
                for (u32 k = 0; k < 4; k++)
                    components[k] = shader_texture_float_component(
                        c, spv_emit_vec4_extract(&c->emit, texture_scalar[slot], gathered, k), texture_scalar[slot],
                        i32_type);
                reg_write(c, d, shader_pack_texture_halves(c, vec2, components[0], components[1]));
                reg_write(c, db, shader_pack_texture_halves(c, vec2, components[2], components[3]));
            } else {
                u32 regs[4] = {d, d < 254 ? d + 1 : 255, db, db < 254 ? db + 1 : 255};
                for (u32 k = 0; k < 4; k++)
                    if (regs[k] < 255)
                        reg_write(c, regs[k], shader_texture_component(c, gathered, texture_scalar[slot], k));
            }
        } else {
            u32 mask = B(w, 31, 4), dst = d;
            for (u32 k = 0; k < 4; k++)
                if (mask & (1u << k)) {
                    reg_write(c, dst, shader_texture_component(c, gathered, texture_scalar[slot], k));
                    if (dst < 255) dst++;
                }
        }
    }
}
