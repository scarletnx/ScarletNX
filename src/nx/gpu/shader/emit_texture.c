/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Texture samples, fetches, gathers and queries, the texture slots, and surface loads and stores */
#include "nx/gpu/shader/translate.h"
#include "nx/gpu/shader/minmax_spirv.h"

/* a texture instruction's array layer: the low 16 bits of its register */
u32 shader_array_layer(DirectCtx* c, u32 r) {
    return spv_emit_unary(
        &c->emit, SPV_OP_CONVERT_U_TO_F, c->f32_type,
        spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, reg_read(c, r), shader_u32(c, 0xffff)));
}

/* A sampled texel's component into register r, as the value of the texture's scalar type: a read of that type in
   the block takes it back, and its bits are made when something reads or stores them (reg_write_as) */
void shader_texture_write(DirectCtx* c, u32 r, u32 sampled, u32 scalar_type, u32 component) {
    if (r < 255) reg_write_as(c, r, spv_emit_vec4_extract(&c->emit, scalar_type, sampled, component), scalar_type);
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
    return spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->f32_type, pass,
                            spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f),
                            spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f));
}
static u32 indexed_metadata(DirectCtx* c, int slot) {
    u32 limit = shader_u32(c, SNX_TEX_ARRAY - 1);
    u32 index = c->active_index;
    index = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type,
                             spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, limit, index), limit, index);
    u32 element = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, index, shader_u32(c, 2));
    u32 component = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, index, shader_u32(c, 3));
    u32 func = spv_emit_uniform_component(&c->emit, c->tex_func_pointer, c->tex_func_buffer, c->zero, element,
                                          component, c->u32_type);
    return func;
}
u32 shader_indexed_is_depth(DirectCtx* c, int slot) {
    u32 bit = shader_u32(c, 8);
    return spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type,
                           spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, indexed_metadata(c, slot), bit),
                           bit);
}
u32 shader_dc_compare_indexed(DirectCtx* c, int slot, u32 reference, u32 texel) {
    u32 func = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, indexed_metadata(c, slot), shader_u32(c, 7));
    u32 result = shader_dc_compare(c, 3, reference, texel);
    for (int i = 0; i < 8; ++i) {
        if (i == 3) continue;
        u32 selected = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, func, shader_u32(c, i));
        result = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->f32_type, selected,
                                  shader_dc_compare(c, i, reference, texel), result);
    }
    return result;
}
/* what a filtered sample of slot takes of its texels in the program (ShaderInfo.tex_minmax_mask): 1 the least, 2 the
   greatest, 0 neither */
int shader_minmax_of(const ShaderInfo* si, int slot) {
    return shader_minmax_slot(si->tex_minmax_mask, si->tex_minmax_max, slot);
}
/* shader_minmax_sample of slot's texture, a float one, at coords (array: its third component the layer) moved by offset
   (0 for none; dynamic: not a constant), for reduce (shader_minmax_of) */
u32 shader_minmax_direct(DirectCtx* c, u32 vec4_type, u32 sampled_type, u32 sampler, u32 coords, int array, u32 offset,
                         int dynamic, int reduce) {
    ShMinmaxTypes t = {c->f32_type, c->u32_type, c->bool_type, vec4_type, c->glsl};
    return shader_minmax_sample(&c->emit, &t, sampled_type, sampler, coords, array, offset, dynamic, reduce == 2);
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
/* Whether a fragment program's texture slot is bound to a scaled render target (resolution= other than 1): its bit in
   the push constants' uvec4, x for slots 0 to 31 and y for 32 to 63 (vk/render.c's stage_set). With own, whether that
   target has the size of the one the draw renders to as well (render.c's own_size): its bit in the uvec4 at byte 96 */
static u32 scaled_slot(DirectCtx* c, u32 push, int slot, int own) {
    u32 one = spv_emit_constant_u32(&c->emit, c->u32_type, 1);
    u32 bits = spv_emit_push_component_f32(&c->emit, spv_emit_type_pointer(&c->emit, 9, c->u32_type), push,
                                           own ? spv_emit_constant_u32(&c->emit, c->u32_type, 3) : one,
                                           slot >= 32 ? one : c->zero, c->u32_type); /* PushConstant */
    u32 bit = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type,
        spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, bits, spv_emit_constant_u32(&c->emit, c->u32_type, slot & 31)),
        one);
    return spv_emit_binary(&c->emit, SPV_OP_I_NOT_EQUAL, c->bool_type, bit, c->zero);
}
/* component k of SNX_VKB_RS's vec4 (vk/render.c's frame_consts): 0 and 1 are 1 / the resolution scale, 2 the scale */
static u32 scale_component(DirectCtx* c, u32 k) {
    return spv_emit_uniform_component(&c->emit, c->scale_pointer, c->scale_buffer, c->zero, c->zero,
                                      spv_emit_constant_u32(&c->emit, c->u32_type, k), c->f32_type);
}
static u32 exact(DirectCtx* c, u32 value) {
    spv_emit_no_contraction(&c->emit, value);
    return value;
}
static u32 floor_of(DirectCtx* c, u32 value) {
    return spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FLOOR, value);
}
/* A fragment program's 2D texel coordinates *x and *y of texture slot, as its image has them. The image of a scaled
   target has texel p from pixel p times the scale on. Where the target has the size of the one drawn to, the pixel
   that lies where the fragment lies in its own guest pixel is fetched, as the Ounce translator's scaled_fetch does
   (sm86_translate.c) and Eden's rescaling pass (SubScale): at a whole scale a pass that fetches its own pixel reads
   that very pixel, not one for all the fragments of a guest pixel. It stays within the texel's pixels, which at
   another scale end before a whole one. In a lower-left window the fragment's y counts up (shader_frag_y_origin),
   and so does its place. A target of another size (a table, a smaller level) gives every fragment the pixel at the
   texel's centre. Another texture keeps p. fragcoord is the FragCoord input, a vec4 */
void shader_scaled_fetch(DirectCtx* c, u32 push, u32 fragcoord, u32 vec4, int slot, u32 i32_type, u32* x, u32* y) {
    u32 scaled = scaled_slot(c, push, slot, 0), own = scaled_slot(c, push, slot, 1);
    u32 scale = scale_component(c, 2), over = scale_component(c, 0);
    u32 lower_left =
        spv_emit_binary(&c->emit, SPV_OP_I_NOT_EQUAL, c->bool_type, shader_spec(c, SH_SPEC_LOWER_LEFT, 0), c->zero);
    u32 half = spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f);
    u32* v[2] = {x, y};
    for (u32 k = 0; k < 2; k++) {
        u32 p = spv_emit_unary(&c->emit, SPV_OP_CONVERT_S_TO_F, c->f32_type, *v[k]);
        u32 first = exact(c, spv_emit_binary(&c->emit, SPV_OP_F_MUL, c->f32_type, p, scale));
        u32 next = floor_of(c, exact(c, spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type, first, scale)));
        u32 last = spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_MAX, floor_of(c, first),
                                        exact(c, SPV(&c->emit, SPV_OP_F_SUB, c->f32_type, next,
                                                     spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f))));
        /* the fragment's place in its guest pixel, in the image's pixels: 0 up to the scale */
        u32 pixel = shader_input_component(c, vec4, fragcoord, c->f32_type, k);
        u32 corner = exact(c, spv_emit_binary(&c->emit, SPV_OP_F_MUL, c->f32_type, scale,
                                            floor_of(c, exact(c, spv_emit_binary(&c->emit, SPV_OP_F_MUL, c->f32_type, pixel, over)))));
        u32 place = exact(c, SPV(&c->emit, SPV_OP_F_SUB, c->f32_type, pixel, corner));
        if (k)
            place = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->f32_type, lower_left,
                                    exact(c, SPV(&c->emit, SPV_OP_F_SUB, c->f32_type, scale, place)), place);
        u32 at = spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_MIN,
                                      floor_of(c, exact(c, spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type, first, place))), last);
        u32 centre =
            floor_of(c, exact(c, spv_emit_binary(&c->emit, SPV_OP_F_MUL, c->f32_type,
                                               exact(c, spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type, p, half)), scale)));
        at = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->f32_type, own, at, centre);
        *v[k] = spv_emit_ternary(&c->emit, SPV_OP_SELECT, i32_type, scaled, spv_emit_unary(&c->emit, SPV_OP_CONVERT_F_TO_S, i32_type, at), *v[k]);
    }
}
/* The size *x by *y of texture slot's image as the guest has it: a scaled target's over the scale, rounded as vk_sc
   rounds a scaled size */
void shader_scaled_size(DirectCtx* c, u32 push, int slot, u32* x, u32* y) {
    u32 factor = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->f32_type, scaled_slot(c, push, slot, 0), scale_component(c, 0),
                                 spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f));
    u32* v[2] = {x, y};
    for (u32 k = 0; k < 2; k++) {
        u32 f = exact(
            c, spv_emit_binary(&c->emit, SPV_OP_F_MUL, c->f32_type, spv_emit_unary(&c->emit, SPV_OP_CONVERT_U_TO_F, c->f32_type, *v[k]), factor));
        *v[k] = spv_emit_unary(&c->emit, SPV_OP_CONVERT_F_TO_U, c->u32_type,
            exact(c, spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type, f, spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f))));
    }
}

/* The fragment's y (a[0x74]) in a lower-left window (WINDOW_ORIGIN bit 0, pushed as the uvec4's z) counts up from the
   bottom of the surface clip, its height the w, as the viewport and the scissor measure it there: the guest's GL driver
   leaves its own gl_FragCoord transform the identity for the window it draws lower left */
/* A rectangle texture's coordinates (ShaderInfo.tex_rect_mask) are texels: x and y over its size at level 0, the
   size a fragment program's TXQ answers on a scaled target (shader_scaled_size), which the guest's texels
   count. *gx and *gy, when not NULL, are gradients in texels, divided the same */
void shader_rect_coords(DirectCtx* c, u32 push, int scaled, int slot, u32 vec2_u32, u32 i32_type, u32* x, u32* y,
                        u32* gx, u32* gy) {
    u32 size = spv_emit_image_query_size_lod(&c->emit, vec2_u32, c->sampled_type[slot], c->sampler[slot], c->zero);
    u32 w = spv_emit_vec4_extract(&c->emit, c->u32_type, size, 0),
        h = spv_emit_vec4_extract(&c->emit, c->u32_type, size, 1);
    if (scaled) shader_scaled_size(c, push, slot, &w, &h);
    u32 fw = spv_emit_unary(&c->emit, SPV_OP_CONVERT_U_TO_F, c->f32_type, w), fh = spv_emit_unary(&c->emit, SPV_OP_CONVERT_U_TO_F, c->f32_type, h);
    *x = spv_emit_binary(&c->emit, SPV_OP_F_DIV, c->f32_type, *x, fw);
    *y = spv_emit_binary(&c->emit, SPV_OP_F_DIV, c->f32_type, *y, fh);
    for (int k = 0; k < 2 && gx; k++) {
        gx[k] = spv_emit_binary(&c->emit, SPV_OP_F_DIV, c->f32_type, gx[k], k ? fh : fw);
        gy[k] = spv_emit_binary(&c->emit, SPV_OP_F_DIV, c->f32_type, gy[k], k ? fh : fw);
    }
}
/* x of a 1D texel fetch as the row and column of the 2D image a 1D texture is bound as (SH_TEXEL_ROW_SHIFT). A
   negative x lands in a negative row, out of the image as it is out of the texture */
void shader_texel_row_coord(DirectCtx* c, u32 i32_type, u32* x, u32* y) {
    u32 shift = shader_u32(c, SH_TEXEL_ROW_SHIFT);
    u32 column = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type,
                                 spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type, *x),
                                 shader_u32(c, (1u << SH_TEXEL_ROW_SHIFT) - 1));
    *y = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_ARITHMETIC, i32_type, *x, shift);
    *x = spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, column);
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
    u32 limit = shader_u32(c, SNX_TEX_ARRAY - 1);
    u32 index = spv_emit_load(&c->emit, c->u32_type, c->tex_index[slot]);
    index = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type,
                             spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, limit, index), limit, index);
    if (c->tex_map_buffer) {
        u32 base = shader_u32(c, (u32)table * SNX_TEX_ARRAY);
        u32 flat = spv_emit_iadd_u32(&c->emit, c->u32_type, base, index);
        u32 element = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, flat, shader_u32(c, 2));
        u32 component = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, flat, shader_u32(c, 3));
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
    return scalar == i32_type ? spv_emit_unary(&c->emit, SPV_OP_CONVERT_S_TO_F, c->f32_type, value)
                              : spv_emit_unary(&c->emit, SPV_OP_CONVERT_U_TO_F, c->f32_type, value);
}

u32 shader_pack_texture_halves(DirectCtx* c, u32 vec2, u32 first, u32 second) {
    return shader_pack_half_rte(c, spv_emit_vec2_construct(&c->emit, vec2, first, second));
}

/* Gather AOFFI stores signed six-bit x/y offsets in the low bits of two consecutive bytes. */
u32 shader_direct_gather_offset(DirectCtx* c, u32 raw, u32 shift, int zero_y, u32 i32_type, u32 vec2_i32) {
    u32 bits = spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, raw);
    u32 width = shader_u32(c, 6);
    u32 x = spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type, bits, shader_u32(c, shift), width);
    u32 y = zero_y ? spv_emit_constant_u32(&c->emit, i32_type, 0)
                   : spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type, bits, shader_u32(c, shift + 8),
                                      width);
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
            if (shadow)
                reg_write_as(c, regs[k], comps[k] == 3 ? spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f) : sample,
                             c->f32_type);
            else
                shader_texture_write(c, regs[k], sample, scalar, comps[k]);
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

typedef struct {
    DirectCtx* c;
    u32 result_type, i32_type, offset_type, sampled_type, sampler;
    u32 coords, dx, dy, count, levels, mask[3], selector[3];
    u32 regular, mode, level, depth_reference, min_lod;
    int reduce, array; /* shader_minmax_of the slot's, and whether it is an array */
} TextureOffsetTree;
static u32 texture_offset_sample(const TextureOffsetTree* tree, u32 offset, int dynamic) {
    DirectCtx* c = tree->c;
    if (tree->reduce && !tree->depth_reference)
        return shader_minmax_direct(c, tree->result_type, tree->sampled_type, tree->sampler, tree->coords, tree->array,
                                    offset, dynamic, tree->reduce);
    return tree->regular
        ? spv_emit_sample_image_min_lod(&c->emit, tree->result_type, tree->sampled_type, tree->sampler,
            tree->coords, tree->depth_reference, tree->mode, tree->level, offset, dynamic, tree->min_lod)
        : spv_emit_sample_grad_offset(&c->emit, tree->result_type, tree->sampled_type, tree->sampler,
            tree->coords, tree->dx, tree->dy, offset, dynamic, tree->min_lod);
}
static u32 texture_offset_leaf(const TextureOffsetTree* tree, u32 value) {
    DirectCtx* c = tree->c;
    u32 o[3];
    for (u32 axis = 0; axis < 3; axis++) {
        u32 nibble = axis < tree->count ? value >> (4 * axis) & 15 : 0;
        o[axis] = spv_emit_constant_u32(&c->emit, tree->i32_type, nibble >= 8 ? nibble - 16 : nibble);
    }
    u32 offset = tree->count == 3 ? spv_emit_constant_vec3(&c->emit, tree->offset_type, o[0], o[1], o[2])
                                  : spv_emit_constant_vec2(&c->emit, tree->offset_type, o[0], o[1]);
    return texture_offset_sample(tree, offset, 0);
}
/* Explicit samples have at most16 successors and16 Phi inputs per selection. A flat4096-input merge makes some drivers spend
   gigabytes constructing liveness/SSA state. The high-bit selections contain lower-bit selections, so the leaf
   enumeration and its exact ConstOffset samples stay unchanged. Only one explicit leaf executes per invocation. */
static u32 texture_offset_select(const TextureOffsetTree* tree, u32 level, u32 value, u32* last_label) {
    DirectCtx* c = tree->c;
    u32 mask = tree->mask[level], cases = 1u << snx_popcount64(mask);
    u32 labels[16], literals[15], incoming[32];
    for (u32 k = 0; k < cases; k++) {
        if (!tree->regular || tree->mode == 1) labels[k] = spv_emit_id(&c->emit);
        if (k) {
            /* Retain sparse selector positions: no per-invocation bit compression is needed. */
            u32 number = k, subset = 0;
            for (u32 bit = 0; bit < 4 * tree->count; bit++)
                if (mask & (1u << bit)) { subset |= (number & 1) << bit; number >>= 1; }
            literals[k - 1] = subset;
        }
    }
    u32 result_type = tree->result_type;
    if (tree->regular && tree->mode != 1) {
        /* Implicit derivatives must not enter offset-dependent control flow. Evaluate the finite constant-offset
           domain in the original block, then select its result. Bias, filtering and minimum LOD stay native. */
        u32 result = 0;
        for (u32 k = 0; k < cases; k++) {
            u32 subset = k ? literals[k - 1] : 0;
            u32 sample = level + 1 < tree->levels
                ? texture_offset_select(tree, level + 1, value | subset, last_label)
                : texture_offset_leaf(tree, value | subset);
            result = k ? spv_emit_ternary(&c->emit, SPV_OP_SELECT, result_type,
                                          spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, tree->selector[level],
                                                          shader_u32(c, subset)),
                                          sample, result)
                       : sample;
        }
        return result;
    }
    u32 merge = spv_emit_id(&c->emit);
    /* DontFlatten is a hint: keep the exact offset selection as control flow instead of speculating thousands
       of native samples. Correctness is unchanged on implementations which ignore it. */
    const u32 selection[2] = {merge, 2};
    spv_emit_void_op(&c->emit, SPV_OP_SELECTION_MERGE, selection, 2);
    spv_emit_switch_u32(&c->emit, tree->selector[level], labels[0], literals, labels + 1, cases - 1);
    for (u32 k = 0; k < cases; k++) {
        spv_emit_label(&c->emit, labels[k]);
        u32 offset_bits = value | (k ? literals[k - 1] : 0), predecessor = labels[k], sample;
        if (level + 1 < tree->levels)
            sample = texture_offset_select(tree, level + 1, offset_bits, &predecessor);
        else sample = texture_offset_leaf(tree, offset_bits);
        incoming[2 * k] = sample;
        incoming[2 * k + 1] = predecessor;
        spv_emit_branch(&c->emit, merge);
    }
    spv_emit_label(&c->emit, merge);
    *last_label = merge;
    return spv_emit_op(&c->emit, SPV_OP_PHI, result_type, incoming, 2 * cases);
}
/* Maxwell packs signed4-bit texel offsets. Native samples preserve filtering, wrap and anisotropy;
   moving normalized coordinates by a base-level texel cannot replace an offset at the sampled mip level. */
static u32 texture_offset_dispatch(TextureOffsetTree tree, u32 packed, u32 shift, u32 known, u32 value) {
    DirectCtx* c = tree.c;
    u32 unknown = ((1u << (4 * tree.count)) - 1) & ~known, groups[3] = {0};
    if (!unknown) return texture_offset_leaf(&tree, value);
    for (u32 bit = 0, used = 0; bit < 4 * tree.count; bit++) {
        if (!(unknown & (1u << bit))) continue;
        groups[used / 4] |= 1u << bit;
        used++;
        tree.levels = (used + 3) / 4;
    }
    u32 raw = shift ? spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, packed, shader_u32(c, shift))
                    : packed;
    for (u32 level = 0; level < tree.levels; level++) {
        tree.mask[level] = groups[tree.levels - level - 1];
        tree.selector[level] =
            spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, raw, shader_u32(c, tree.mask[level]));
    }
    u32 last_label = 0;
    return texture_offset_select(&tree, 0, value, &last_label);
}
static u32 texture_gradient_offset(const Tr* t, int slot, u32 coords, u32 dx, u32 dy, u32 packed, u32 count,
                                    u32 known, u32 value) {
    TextureOffsetTree tree = {.c = t->c,
                              .coords = coords,
                              .dx = dx,
                              .dy = dy,
                              .count = count,
                              .result_type = t->texture_vec4[slot],
                              .i32_type = t->i32_type,
                              .offset_type = count == 3 ? t->vec3_i32 : t->vec2_i32,
                              .sampled_type = t->c->sampled_type[slot],
                              .sampler = t->c->sampler[slot],
                              .reduce = shader_minmax_of(t->si, slot),
                              .array = t->textures[slot].array};
    return texture_offset_dispatch(tree, packed, 16, known, value);
}
/* a surface instruction's coordinates x and y with what its dimension adds: a 2D array's layer is the low 16 bits of
   the third register (a + 2), as TLD's is, and a 3D surface's z the whole register (Eden's MakeCoords in
   surface_load_store.cpp reads them the same way) */
static u32 surface_coord(DirectCtx* c, int dim, u32 x, u32 y, u32 a, u32 i32_type, u32 vec2_i32, u32 vec3_i32) {
    if (dim == 4)
        return spv_emit_vec3_construct(
            &c->emit, vec3_i32, x, y,
            spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type,
                           spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, reg_read(c, a + 2),
                                           shader_u32(c, 0xffff))));
    if (dim == 5) return spv_emit_vec3_construct(&c->emit, vec3_i32, x, y, reg_read_as(c, a + 2, i32_type));
    return spv_emit_vec2_construct(&c->emit, vec2_i32, x, y);
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
            op == O_TLD_B ? shader_direct_track_image_handle_pair(t->flow, i, b, &bank, &off, &bank2, &off2, 0) : 0;
        if (handle_count == 1) indexed = shader_direct_indexed_handle(t->flow, i, b, stage, &ldc);
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
        u32 z = type <= 1 && !array  ? 0
                : type == 0 || array ? spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type,
                                                      spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type,
                                                                      reg_read(c, a), shader_u32(c, 0xffff)))
                                     : reg_read_as(c, a + 2, i32_type);
        if (packed_reg < 255) { /* 4-bit signed offsets: x, then y but for 1D, then z for 3D */
            u32 packed = reg_read_as(c, packed_reg, i32_type);
            u32 four = shader_u32(c, 4);
            x = spv_emit_iadd_u32(
                &c->emit, i32_type, x,
                spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type, packed, c->zero, four));
            if (type >= 1)
                y = spv_emit_iadd_u32(
                    &c->emit, i32_type, y,
                    spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type, packed, four, four));
            if (type == 2)
                z = spv_emit_iadd_u32(
                    &c->emit, i32_type, z,
                    spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type, packed, shader_u32(c, 8), four));
        }
        if (type == 0 && !array) shader_texel_row_coord(c, i32_type, &x, &y);
        if (type == 1 && needs_texel_scale) shader_scaled_fetch(c, push, t->fragcoord, vec4, slot, i32_type, &x, &y);
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
                shader_texture_write(c, target, sample, texture_scalar[slot], k);
                if (target < 255) target++;
            }
    } else if (op == O_SULD || op == O_SUST || op == O_SURED || op == O_SUATOM) {
        int dim = (int)B(w, 33, 3), buffer = dim == 1;
        u32 bank = 0, off = B(w, 36, 13);
        int bindless = op == O_SUATOM ? (int)B(w, 54, 1) : !B(w, 51, 1);
        if (bindless) shader_direct_track_image_handle(t->flow, i, B(w, 39, 8), &bank, &off, 0);
        u32 x = reg_read_as(c, a, i32_type), y = buffer ? 0 : reg_read_as(c, a + 1, i32_type);
        u32 coord = buffer ? x : surface_coord(c, dim, x, y, a, i32_type, vec2_i32, vec3_i32);
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
            /* a load is a fetch at a texel: of a scaled target it takes TLD's pixel (shader_scaled_fetch) */
            if (sample_type == 1 && needs_texel_scale) {
                shader_scaled_fetch(c, push, t->fragcoord, vec4, slot, i32_type, &x, &y);
                coord = surface_coord(c, dim, x, y, a, i32_type, vec2_i32, vec3_i32);
            }
            u32 sample = spv_emit_image_fetch(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot],
                                              coord, spv_emit_constant_u32(&c->emit, i32_type, 0));
            for (u32 q = 0; q < 4; q++)
                if (mask & (1u << q)) {
                    shader_texture_write(c, source, sample, texture_scalar[slot], q);
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
                      : spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type,
                                       reg_read(c, array ? b + 1
                                                   : encoding == 4 || encoding == 5 || encoding == 6 || encoding == 7 ||
                                                           encoding == 12
                                                       ? a + 1
                                                       : b));
        if (encoding == 4 || encoding == 12) {
            u32 packed = spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, reg_read(c, b + (encoding == 12)));
            u32 four = shader_u32(c, 4);
            u32 ox = spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type, packed, c->zero, four);
            u32 oy = spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type, packed, four, four);
            x = spv_emit_iadd_u32(&c->emit, i32_type, x, ox);
            y = spv_emit_iadd_u32(&c->emit, i32_type, y, oy);
        }
        if (type == 0) shader_texel_row_coord(c, i32_type, &x, &y);
        if (type == 1 && needs_texel_scale) shader_scaled_fetch(c, push, t->fragcoord, vec4, slot, i32_type, &x, &y);
        u32 coord =
            type == 2 ? spv_emit_vec3_construct(&c->emit, vec3_i32, x, y, reg_read_as(c, b, i32_type))
            : array ? spv_emit_vec3_construct(&c->emit, vec3_i32, x, y,
                                              spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type,
                                                             spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type,
                                                                             reg_read(c, a), shader_u32(c, 0xffff))))
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
            lo = shader_pack_half_rte(c, lo);
            if (d < 255) reg_write(c, d, lo);
            if (outputs > 2) {
                u32 db = B(w, 28, 8);
                u32 hi = spv_emit_vec2_construct(&c->emit, vec2, halves[2], outputs > 3 ? halves[3] : zero_f);
                hi = shader_pack_half_rte(c, hi);
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
                              : reg_read_as(c, shader_texs_y(encoding, a, b), c->f32_type);
        if (rect_slots >> slot & 1)
            shader_rect_coords(c, push, stage == ST_FS, slot, vec2_u32, i32_type, &x, &y, NULL, NULL);
        u32 uv = array       ? spv_emit_vec3_construct(&c->emit, vec3, x, y, shader_array_layer(c, a))
                 : type >= 2 ? spv_emit_vec3_construct(&c->emit, vec3, x, y, reg_read_as(c, b, c->f32_type))
                             : spv_emit_vec2_construct(&c->emit, vec2, x, y);
        u32 dref = shadow ? reg_read_as(c, shader_texs_dref(encoding, b), c->f32_type) : 0;
        u32 sample;
        int explicit_lod = shader_texs_explicit(encoding) || stage != ST_FS;
        u32 level = shader_texs_explicit(encoding) ? shader_texs_level(encoding, b) : 255;
        u32 lod = level < 255 ? reg_read_as(c, level, c->f32_type) : zero_f;
        int reduce = shadow ? 0 : shader_minmax_of(si, slot);
        sample = reduce ? shader_minmax_direct(c, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot], uv,
                                               array, 0, 0, reduce)
                        : shader_sample_dref(c, si, slot, texture_vec4[slot], uv, dref, explicit_lod, lod);
        shader_texs_store(c, w, sample, texture_scalar[slot], shadow, i32_type, vec2, zero_f);
    } else if (op == O_TEX || op == O_TEX_B) {
        int bindless = op == O_TEX_B;
        u32 mode = bindless ? B(w, 37, 3) : B(w, 55, 3);
        u32 offset_enabled = B(w, bindless ? 36 : 54, 1);
        u32 clamp = B(w, bindless ? 40 : 58, 1);
        u32 source = b + bindless;
        u32 has_level = mode == 2 || mode == 3 || mode == 6 || mode == 7;
        u32 offset_source = source + has_level;
        u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
        int handle_count =
            bindless ? shader_direct_track_image_handle_pair(t->flow, i, b, &bank, &off, &bank2, &off2, 0) : 0;
        int type = B(w, 28, 3) >> 1;
        int array = B(w, 28, 1), shadow = B(w, 50, 1);
        int slot = 0;
        if (handle_count == 1) indexed = shader_direct_indexed_handle(t->flow, i, b, stage, &ldc);
        /* a handle the writes don't agree on: each constant buffer handle that can reach it is sampled, and the sample
           of the one whose word equals the register is kept (zeros when none does: a handle from memory, unbound) */
        u32 cand_bank[SH_HANDLE_CANDIDATES], cand_off[SH_HANDLE_CANDIDATES];
        int candidates = bindless && !handle_count
                             ? shader_direct_handle_candidates(t->flow, i, b, cand_bank, cand_off, SH_HANDLE_CANDIDATES)
                             : 1;
        int choose = bindless && !handle_count;
        u32 handle = choose ? reg_read(c, b) : 0, chosen = 0, chosen_type = 0, sample = 0;
        int chosen_slot = -1;
        for (int cand = 0; cand < candidates; cand++) {
            if (choose) {
                bank = cand_bank[cand];
                off = cand_off[cand];
            }
            int handle_words = handle_count ? handle_count : 1;
            slot = 0;
            while (slot < texture_count &&
                   (textures[slot].cbuf_off != off || textures[slot].shadow != shadow || textures[slot].type != type ||
                    textures[slot].array != array || textures[slot].bindless != bindless ||
                    (textures[slot].flags & 3) ||
                    (bindless &&
                     !shader_direct_bindless_matches(&textures[slot], bank, handle_words, bank2, off2, indexed, ldc))))
                slot++;
            if (textures[slot].flags & SNX_TEX_INDEXED) shader_direct_swap_element(c, slot);
            /* LC adds a packed word before coordinates, even for a non-array texture. Its low half is the layer and
           its high half is an unsigned minimum LOD with eight fractional bits (Mesa nak_nir_lower_tex.c). */
            u32 coord = a + (array || clamp);
            u32 min_lod =
                clamp ? spv_emit_binary(&c->emit, SPV_OP_F_MUL, c->f32_type,
                                        spv_emit_unary(&c->emit, SPV_OP_CONVERT_U_TO_F, c->f32_type,
                                                       spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL,
                                                                       c->u32_type, reg_read(c, a), shader_u32(c, 16))),
                                        spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f / 256.0f))
                      : 0;
            u32 x = reg_read_as(c, coord, c->f32_type);
            u32 y =
                type == 0 ? spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f) : reg_read_as(c, coord + 1, c->f32_type);
            if (rect_slots >> slot & 1)
                shader_rect_coords(c, push, stage == ST_FS, slot, vec2_u32, i32_type, &x, &y, NULL, NULL);
            u32 coords =
                type == 3 && array ? spv_emit_vec4_construct(&c->emit, vec4,
                                                             (u32[4]){x, y, reg_read_as(c, coord + 2, c->f32_type),
                                                                      shader_array_layer(c, a)})
                : array            ? spv_emit_vec3_construct(&c->emit, vec3, x, y, shader_array_layer(c, a))
                : type >= 2 ? spv_emit_vec3_construct(&c->emit, vec3, x, y, reg_read_as(c, coord + 2, c->f32_type))
                            : spv_emit_vec2_construct(&c->emit, vec2, x, y);
            u32 level = mode == 1 ? zero_f : has_level ? reg_read_as(c, source, c->f32_type) : 0;
            u32 dref = shadow ? reg_read_as(c, offset_source + offset_enabled, c->f32_type) : 0;
            u32 offset = 0, varying_offset = 0, offset_known = 0, offset_value = 0;
            u32 offset_count = type == 0 ? 1 : type == 2 ? 3 : 2;
            int dynamic_offset = 0;
            if (offset_enabled) {
                u32 packed = 0, domain = (1u << (4 * offset_count)) - 1;
                int known = shader_direct_track_const(t->flow, i, offset_source, &packed, 0);
                if (!known && !g_shader_dynamic_texture_offsets && g_shader_cb_read) {
                    u32 offset_bank, offset_word;
                    if (shader_direct_track_cbuf_word(t->flow, i, offset_source, &offset_bank, &offset_word, 0))
                        known = g_shader_cb_read(offset_bank, offset_word * 4, &packed);
                }
                if (!known) {
                    shader_direct_track_known_bits(t->flow, i, offset_source, &offset_known, &offset_value, 0);
                    offset_known &= domain;
                    offset_value &= offset_known;
                    known = offset_known == domain;
                    if (known) packed = offset_value;
                }
                if (!known) varying_offset = reg_read(c, offset_source);
                if (known || g_shader_dynamic_texture_offsets) {
                    u32 o[3],
                        signed_bits = known ? 0 : spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, varying_offset);
                    for (u32 axis = 0; axis < 3; axis++) {
                        u32 nibble = packed >> (4 * axis) & 15;
                        o[axis] = axis >= offset_count ? spv_emit_constant_u32(&c->emit, i32_type, 0)
                                  : known
                                      ? spv_emit_constant_u32(&c->emit, i32_type, nibble >= 8 ? nibble - 16 : nibble)
                                      : spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type, signed_bits,
                                                         shader_u32(c, 4 * axis), shader_u32(c, 4));
                    }
                    offset = offset_count == 3 ? known ? spv_emit_constant_vec3(&c->emit, vec3_i32, o[0], o[1], o[2])
                                                       : spv_emit_vec3_construct(&c->emit, vec3_i32, o[0], o[1], o[2])
                             : known           ? spv_emit_constant_vec2(&c->emit, vec2_i32, o[0], o[1])
                                               : spv_emit_vec2_construct(&c->emit, vec2_i32, o[0], o[1]);
                    dynamic_offset = !known;
                    varying_offset = 0;
                }
            }
            /* outside the fragment stage there are no derivatives: LOD 0, or the bias as the level */
            u32 sample_mode = mode == 1 || mode == 3 || mode == 7 || stage != ST_FS ? 1
                              : mode == 2 || mode == 6                              ? 2
                                                                                    : 0;
            if (sample_mode == 1 && !level) level = zero_f;
            if (min_lod && sample_mode == 1) {
                level = spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_MAX, level, min_lod);
                min_lod = 0;
            }
            /* a depth compare of a color texture samples it plainly and compares its first component here */
            int indexed_dc = dref && shadow && (textures[slot].flags & SNX_TEX_INDEXED);
            int dc_func = indexed_dc ? 3 : dref ? shader_dc_color_func(si, slot) : -1;
            TextureOffsetTree sampling = {.c = c,
                                          .regular = 1,
                                          .mode = sample_mode,
                                          .level = level,
                                          .result_type = dc_func < 0 && shadow ? c->f32_type : texture_vec4[slot],
                                          .i32_type = i32_type,
                                          .offset_type = offset_count == 3 ? vec3_i32 : vec2_i32,
                                          .sampled_type = c->sampled_type[slot],
                                          .sampler = c->sampler[slot],
                                          .coords = coords,
                                          .depth_reference = dc_func < 0 ? dref : 0,
                                          .min_lod = min_lod,
                                          .count = offset_count,
                                          .reduce = shadow ? 0 : shader_minmax_of(si, slot),
                                          .array = (int)array};
            sample = varying_offset ? texture_offset_dispatch(sampling, varying_offset, 0, offset_known, offset_value)
                                    : texture_offset_sample(&sampling, offset, dynamic_offset);
            if (dc_func >= 0)
                sample = (indexed_dc ? shader_dc_compare_indexed : shader_dc_compare)(
                    c, indexed_dc ? slot : dc_func, dref, spv_emit_vec4_extract(&c->emit, c->f32_type, sample, 0));
            if (indexed_dc) {
                sampling.result_type = c->f32_type;
                sampling.sampled_type = c->depth_type;
                sampling.sampler = c->depth_sampler;
                sampling.depth_reference = dref;
                u32 depth = varying_offset
                                ? texture_offset_dispatch(sampling, varying_offset, 0, offset_known, offset_value)
                                : texture_offset_sample(&sampling, offset, dynamic_offset);
                sample = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->f32_type, shader_indexed_is_depth(c, slot), depth,
                                          sample);
            }
            if (choose) {
                /* the first candidate's result type is the instruction's: one of another kind (an integer texture
               beside float ones) can't be the same texture as it, so it is left out */
                u32 type_of = shadow ? c->f32_type : texture_vec4[slot];
                if (chosen_slot < 0) {
                    u32 zero = spv_emit_constant_u32(&c->emit, shadow ? c->f32_type : texture_scalar[slot], 0);
                    chosen =
                        shadow ? zero : spv_emit_constant_vec4(&c->emit, type_of, (u32[4]){zero, zero, zero, zero});
                    chosen_type = type_of;
                    chosen_slot = slot;
                }
                if (type_of == chosen_type) {
                    u32 word = shader_cbuf_read_dynamic(c, bank, shader_u32(c, off * 4));
                    u32 match = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, handle, word);
                    chosen = spv_emit_ternary(&c->emit, SPV_OP_SELECT, chosen_type, match, sample, chosen);
                }
            }
        }
        if (choose) {
            sample = chosen;
            slot = chosen_slot;
        }
        u32 mask = B(w, 31, 4), dst = d;
        for (u32 component = 0; component < 4; component++)
            if (mask & (1u << component)) {
                if (shadow)
                    reg_write_as(c, dst, component == 3 ? spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f) : sample,
                                 c->f32_type);
                else
                    shader_texture_write(c, dst, sample, texture_scalar[slot], component);
                if (dst < 255) dst++;
            }
    } else if (op == O_TXD || op == O_TXD_B) {
        int bindless = op == O_TXD_B;
        u32 ty = B(w, 28, 3), type = ty >> 1, array = ty & 1, count = type == 0 ? 1 : type == 1 ? 2 : 3;
        u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
        int handle_count =
            bindless ? shader_direct_track_image_handle_pair(t->flow, i, a, &bank, &off, &bank2, &off2, 0) : 0;
        if (handle_count == 1) indexed = shader_direct_indexed_handle(t->flow, i, a, stage, &ldc);
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
        u32 offset = 0, varying_offset = 0, varying_known = 0, varying_value = 0, dynamic_offset = 0;
        if (B(w, 35, 1)) { /* signed 4-bit offsets from bit 16 of the packed register */
            u32 packed = 0, o[3];
            int known = shader_direct_track_const(t->flow, i, packed_reg, &packed, 0);
            if (!known && !g_shader_dynamic_texture_offsets && g_shader_cb_read) {
                u32 offset_bank, offset_word;
                /* The shader cache records this word and makes another variant when it changes. A ConstOffset
                   applies in the selected mip's texels; moving normalized coordinates by the base size does not. */
                if (shader_direct_track_cbuf_word(t->flow, i, packed_reg, &offset_bank, &offset_word, 0))
                    known = g_shader_cb_read(offset_bank, offset_word * 4, &packed);
            }
            if (!known) {
                u32 known_bits = 0, known_value = 0, domain = (1u << (4 * count)) - 1;
                shader_direct_track_known_bits(t->flow, i, packed_reg, &known_bits, &known_value, 0);
                varying_known = known_bits >> 16 & domain;
                varying_value = known_value >> 16 & varying_known;
                /* A varying layer cannot force offset dispatch when every spatial offset bit is constant. */
                known = varying_known == domain;
                if (known) packed = known_value;
            }
            if (known) {
                for (k = 0; k < 3; k++)
                    o[k] = spv_emit_constant_u32(&c->emit, i32_type,
                                                 k < count ? (u32)((i32)(packed << (12 - 4 * k)) >> 28) : 0);
                offset = dims == 3 ? spv_emit_constant_vec3(&c->emit, vec3_i32, o[0], o[1], o[2])
                                   : spv_emit_constant_vec2(&c->emit, vec2_i32, o[0], o[1]);
            } else {
                varying_offset = reg_read(c, packed_reg);
                if (g_shader_dynamic_texture_offsets) {
                    /* maintenance8 keeps the offset in texels at the selected mip. Coordinates, explicit
                       gradients, sampler filtering and the array layer all retain their original values. */
                    u32 signed_bits = spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, varying_offset);
                    u32 four = shader_u32(c, 4);
                    for (k = 0; k < dims; k++)
                        o[k] = k < count ? spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type, signed_bits,
                                                            shader_u32(c, 16 + 4 * k), four)
                                         : spv_emit_constant_u32(&c->emit, i32_type, 0);
                    offset = dims == 3 ? spv_emit_vec3_construct(&c->emit, vec3_i32, o[0], o[1], o[2])
                                       : spv_emit_vec2_construct(&c->emit, vec2_i32, o[0], o[1]);
                    dynamic_offset = 1;
                    varying_offset = 0;
                }
            }
        }
        u32 gradient_x = dims == 3 ? spv_emit_vec3_construct(&c->emit, vec3, dx[0], dx[1], dx[2])
                                   : spv_emit_vec2_construct(&c->emit, vec2, dx[0], dx[1]);
        u32 gradient_y = dims == 3 ? spv_emit_vec3_construct(&c->emit, vec3, dy[0], dy[1], dy[2])
                                   : spv_emit_vec2_construct(&c->emit, vec2, dy[0], dy[1]);
        int reduce = shader_minmax_of(si, slot);
        u32 sample = varying_offset ? texture_gradient_offset(t, slot, coords, gradient_x, gradient_y, varying_offset,
                                                              count, varying_known, varying_value)
                     : reduce ? shader_minmax_direct(c, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot],
                                                     coords, (int)array, offset, dynamic_offset, reduce)
                              : spv_emit_sample_grad_offset(&c->emit, texture_vec4[slot], c->sampled_type[slot],
                                                            c->sampler[slot], coords, gradient_x, gradient_y, offset,
                                                            dynamic_offset, 0);
        u32 mask = B(w, 31, 4), dst = d;
        for (u32 component = 0; component < 4; component++)
            if (mask & (1u << component)) {
                shader_texture_write(c, dst, sample, texture_scalar[slot], component);
                if (dst < 255) dst++;
            }
    } else if (op == O_TXQ || op == O_TXQ_B) {
        int bindless = op == O_TXQ_B;
        u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
        int handle_count =
            bindless ? shader_direct_track_image_handle_pair(t->flow, i, a, &bank, &off, &bank2, &off2, 0) : 0;
        int slot;
        u32 qtype = 1, qarray = 0;
        if (handle_count == 1) indexed = shader_direct_indexed_handle(t->flow, i, a, stage, &ldc);
        if (!indexed && shader_direct_query_type(t->flow, bindless, bank, off, &qtype, &qarray))
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
                         third ? spv_emit_vec4_extract(&c->emit, c->u32_type, dimensions, 2) : shader_u32(c, 1),
                         spv_emit_image_query_levels(&c->emit, c->u32_type, c->sampled_type[slot], c->sampler[slot])};
        if (qtype == 1 && needs_texel_scale) shader_scaled_size(c, push, slot, &values[0], &values[1]);
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
            bindless ? shader_direct_track_image_handle_pair(t->flow, i, b, &bank, &off, &bank2, &off2, 0) : 0;
        if (handle_count == 1) indexed = shader_direct_indexed_handle(t->flow, i, b, stage, &ldc);
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
                f = spv_emit_binary(&c->emit, SPV_OP_F_MUL, c->f32_type, f,
                                    spv_emit_constant_f32(&c->emit, c->f32_type, 256.0f));
                value = spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type,
                                       spv_emit_unary(&c->emit, SPV_OP_CONVERT_F_TO_S, i32_type, f));
            }
            reg_write(c, dst, value);
            if (dst < 255) dst++;
        }
    } else if (op == O_TLD4S || op == O_TLD4 || op == O_TLD4_B) {
        u32 indexed = 0, ldc = 0;
        int bindless = op == O_TLD4_B;
        u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0;
        int handle_count =
            bindless ? shader_direct_track_image_handle_pair(t->flow, i, b, &bank, &off, &bank2, &off2, 0) : 0;
        if (handle_count == 1) indexed = shader_direct_indexed_handle(t->flow, i, b, stage, &ldc);
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
                          : reg_read_as(c, op == O_TLD4S && !shadow && !B(w, 51, 1) ? b : a + array + 1, c->f32_type);
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
        u32 component = shader_u32(c, B(w, op == O_TLD4S ? 52 : bindless ? 38 : 56, 2));
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
            if (shadow) dref = reg_read_as(c, b + B(w, 51, 1), c->f32_type);
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
                    taps[tap] = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->f32_type, shader_indexed_is_depth(c, slot),
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
            gathered = spv_emit_ternary(&c->emit, SPV_OP_SELECT, texture_vec4[slot], shader_indexed_is_depth(c, slot),
                                        depth, gathered);
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
                    if (regs[k] < 255) shader_texture_write(c, regs[k], gathered, texture_scalar[slot], k);
            }
        } else {
            u32 mask = B(w, 31, 4), dst = d;
            for (u32 k = 0; k < 4; k++)
                if (mask & (1u << k)) {
                    shader_texture_write(c, dst, gathered, texture_scalar[slot], k);
                    if (dst < 255) dst++;
                }
        }
    }
}
