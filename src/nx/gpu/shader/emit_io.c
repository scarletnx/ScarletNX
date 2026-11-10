/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Attributes and varyings, the stages' fixed outputs (position, depth, clip distances, transform
   feedback), and the fragment stage's alpha test and coordinates */
#include "nx/gpu/shader/translate.h"

/* A capture draw contains one original primitive. Keep its guest-visible ID while the native draw starts at0. */
static u32 primitive_read(DirectCtx* c, u32 variable) {
    u32 value = variable ? spv_emit_load(&c->emit, c->u32_type, variable) : c->zero;
    return c->primitive_base ? spv_emit_iadd_u32(&c->emit, c->u32_type, value, c->primitive_base) : value;
}

/* How a fragment shader's generic input interpolates (the SPH's imap: 1 constant, 2 perspective, 3 screen
   linear): as its first used component, and perspective when none is marked used, as Eden reads it */
u32 shader_ipa_interpolation(const u32* sph, u32 input) {
    u32 k, mode;
    for (k = 0; k < 4; k++) {
        mode = (sph[6 + input / 4] >> ((input % 4) * 8 + k * 2)) & 3;
        if (mode) return mode;
    }
    return 2;
}

void shader_alpha_test(DirectCtx* c, u32 push, u32 push_pointer, u32 alpha) {
    u32 index[3];
    for (u32 i = 0; i < 3; i++) index[i] = spv_emit_constant_u32(&c->emit, c->u32_type, i);
    u32 enabled = spv_emit_push_component_f32(&c->emit, push_pointer, push, index[0], index[0], c->f32_type);
    u32 function = spv_emit_push_component_f32(&c->emit, push_pointer, push, index[0], index[1], c->f32_type);
    u32 reference = spv_emit_push_component_f32(&c->emit, push_pointer, push, index[0], index[2], c->f32_type);
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    u32 active = spv_emit_fcompare(&c->emit, c->bool_type, 5, enabled, zero);
    u32 pass =
        spv_emit_fcompare(&c->emit, c->bool_type, 2, function, spv_emit_constant_f32(&c->emit, c->f32_type, 7.0f));
    for (u32 mode = 1; mode <= 6; mode++) {
        u32 tag = spv_emit_constant_f32(&c->emit, c->f32_type, (float)mode);
        u32 selected = spv_emit_fcompare(&c->emit, c->bool_type, 2, function, tag);
        u32 comparison = spv_emit_fcompare(&c->emit, c->bool_type, mode, alpha, reference);
        u32 result = spv_emit_bool_binary(&c->emit, c->bool_type, 0, selected, comparison);
        pass = spv_emit_bool_binary(&c->emit, c->bool_type, 1, pass, result);
    }
    u32 fail = spv_emit_bool_not(&c->emit, c->bool_type, pass);
    fail = spv_emit_bool_binary(&c->emit, c->bool_type, 0, active, fail);
    if (c->discarded) { /* in the flow loop: discarded after it */
        spv_emit_store(
            &c->emit, c->discarded,
            spv_emit_bool_binary(&c->emit, c->bool_type, 1, spv_emit_load(&c->emit, c->bool_type, c->discarded), fail));
        return;
    }
    u32 discard = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
    spv_emit_selection_merge(&c->emit, merge);
    spv_emit_branch_conditional(&c->emit, fail, discard, merge);
    spv_emit_label(&c->emit, discard);
    spv_emit_kill(&c->emit);
    spv_emit_label(&c->emit, merge);
}

u32 shader_frag_y_origin(DirectCtx* c, u32 push, u32 y) {
    u32 one = spv_emit_constant_u32(&c->emit, c->u32_type, 1);
    u32 pointer = spv_emit_type_pointer(&c->emit, 9, c->u32_type); /* PushConstant */
    u32 lower_left = spv_emit_push_component_f32(&c->emit, pointer, push, one,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 2), c->u32_type);
    u32 height = spv_emit_push_component_f32(&c->emit, pointer, push, one,
                                             spv_emit_constant_u32(&c->emit, c->u32_type, 3), c->u32_type);
    u32 up = spv_emit_fadd(&c->emit, c->f32_type, spv_emit_convert_u_to_f(&c->emit, c->f32_type, height),
                           spv_emit_fnegate(&c->emit, c->f32_type, y));
    spv_emit_no_contraction(&c->emit, up);
    return spv_emit_select(&c->emit, c->f32_type, spv_emit_i_not_equal(&c->emit, c->bool_type, lower_left, c->zero), up,
                           y);
}

/* The last stage's position as the rasterizer takes it. A viewport coordinate swizzle in the key (SNX_SH_SWIZZLE)
   gives each component the one its code names, negated by the code's bit 0, before clipping as the 3D engine does.
   With the viewport transform off (SNX_SH_WINDOW_XY), which no swizzle comes with, x and y are pixels of the surface
   clip, the viewport then (vk/render.c): x * 2 / width - 1 and y * 2 / height - 1 take them to clip space, the width
   and height in bits 0-15 and 16-31 of the push constant's w, as Eden's position_pass.cpp does with its render area.
   Then z goes from -w..w to 0..w where the push constant's y says the depth mode is -1 to 1 */
void shader_vertex_position_fix(DirectCtx* c, u32 position, u32 push, u32 push_pointer, u32 vec4_type) {
    u32 member = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
    if (c->swizzle & SNX_SH_SWIZZLE) {
        u32 value = spv_emit_load(&c->emit, vec4_type, position), swizzled = value;
        for (u32 k = 0; k < 4; k++) {
            u32 code = (c->swizzle >> (SNX_SH_SWIZZLE_SHIFT + 4 * k)) & 7;
            u32 part = spv_emit_vec4_extract(&c->emit, c->f32_type, value, code >> 1);
            if (code & 1) part = spv_emit_fnegate(&c->emit, c->f32_type, part);
            swizzled = spv_emit_vec4_insert(&c->emit, vec4_type, swizzled, part, k);
        }
        spv_emit_store(&c->emit, position, swizzled);
    }
    if (c->window_xy) {
        u32 size =
            spv_emit_bitcast(&c->emit, c->u32_type,
                             spv_emit_push_component_f32(&c->emit, push_pointer, push, member,
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 3), c->f32_type));
        u32 extent[2] = {
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, size, spv_emit_constant_u32(&c->emit, c->u32_type, 0xFFFF)),
            spv_emit_shift_right_u32(&c->emit, c->u32_type, size, spv_emit_constant_u32(&c->emit, c->u32_type, 16))};
        u32 two = spv_emit_constant_f32(&c->emit, c->f32_type, 2.0f);
        u32 minus_one = spv_emit_constant_f32(&c->emit, c->f32_type, -1.0f);
        u32 value = spv_emit_load(&c->emit, vec4_type, position);
        for (u32 k = 0; k < 2; k++) {
            u32 scale =
                spv_emit_fdiv(&c->emit, c->f32_type, two, spv_emit_convert_u_to_f(&c->emit, c->f32_type, extent[k]));
            u32 pixels = spv_emit_vec4_extract(&c->emit, c->f32_type, value, k);
            u32 clip =
                spv_emit_fadd(&c->emit, c->f32_type, spv_emit_fmul(&c->emit, c->f32_type, pixels, scale), minus_one);
            value = spv_emit_vec4_insert(&c->emit, vec4_type, value, clip, k);
        }
        spv_emit_store(&c->emit, position, value);
    }
    u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, 1);
    u32 enabled = spv_emit_push_component_f32(&c->emit, push_pointer, push, member, component, c->f32_type);
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    u32 test = spv_emit_fcompare(&c->emit, c->bool_type, 5, enabled, zero);
    u32 adjust = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
    spv_emit_selection_merge(&c->emit, merge);
    spv_emit_branch_conditional(&c->emit, test, adjust, merge);
    spv_emit_label(&c->emit, adjust);
    u32 value = spv_emit_load(&c->emit, vec4_type, position);
    u32 z = spv_emit_vec4_extract(&c->emit, c->f32_type, value, 2);
    u32 w = spv_emit_vec4_extract(&c->emit, c->f32_type, value, 3);
    u32 sum = spv_emit_fadd(&c->emit, c->f32_type, z, w);
    spv_emit_no_contraction(&c->emit, sum);
    u32 half = spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f);
    u32 depth = spv_emit_fmul(&c->emit, c->f32_type, sum, half);
    spv_emit_no_contraction(&c->emit, depth);
    value = spv_emit_vec4_insert(&c->emit, vec4_type, value, depth, 2);
    spv_emit_store(&c->emit, position, value);
    spv_emit_branch(&c->emit, merge);
    spv_emit_label(&c->emit, merge);
}

/* a captured word's bits (ShXfb.layout), 0 for one with no output (left as it is, as the extension does) */
static u32 xfb_word(DirectCtx* c, const XfbOut* x, u32 word) {
    if (shader_legacy_group(word * 4) >= 0) word = shader_legacy_address(word * 4) / 4; /* where it moved */
    u32 var = 0, component = word & 3; /* after the move: the fog's one word moves to component 0 */
    if (word >= 28 && word < 32 && x->position_used)
        var = x->position;
    else if (word >= 32 && word < 160 && x->outputs_used[(word - 32) >> 2])
        var = x->output_attrs[(word - 32) >> 2];
    if (!var) return 0;
    u32 value = spv_emit_vec4_extract(&c->emit, c->f32_type, spv_emit_load(&c->emit, x->vec4, var), component);
    return spv_emit_bitcast(&c->emit, c->u32_type, value);
}

/* Transform feedback by stores: a vertex's place is VertexIndex - first + (InstanceIndex - first instance) * the
   vertices an instance captures, and below the draw's count each layout word goes to place * stride / 4 + its index */
static void xfb_write(DirectCtx* c, const XfbOut* x) {
    SpvEmitter* e = &c->emit;
    u32 u = c->u32_type, p[5];
    for (u32 i = 0; i < 5; i++)
        p[i] = spv_emit_load(
            e, u, spv_emit_storage_access_u32(e, x->pointer, x->params, c->zero, spv_emit_constant_u32(e, u, i)));
    u32 vertex = spv_emit_isub_u32(e, u, spv_emit_load(e, u, x->vertex_index), p[0]);
    u32 instance = spv_emit_isub_u32(e, u, spv_emit_load(e, u, x->instance_index), p[1]);
    u32 place = spv_emit_iadd_u32(e, u, vertex, spv_emit_imul_u32(e, u, instance, p[2]));
    u32 in = spv_emit_bool_binary(e, c->bool_type, 0, spv_emit_u_less(e, c->bool_type, vertex, p[2]), /* and */
                                  spv_emit_u_less(e, c->bool_type, place, p[3]));
    u32 yes = spv_emit_id(e), done = spv_emit_id(e);
    spv_emit_selection_merge(e, done);
    spv_emit_branch_conditional(e, in, yes, done);
    spv_emit_label(e, yes);
    for (u32 b = 0; b < 4; b++) {
        if (!x->buffer[b]) continue;
        /* a buffer that the guest left off has a 16-byte placeholder binding: no store goes there (p[4], the mask) */
        u32 bit = spv_emit_bitwise_and_u32(e, u, p[4], spv_emit_constant_u32(e, u, 1u << b));
        u32 on = spv_emit_i_not_equal(e, c->bool_type, bit, c->zero), store = spv_emit_id(e), next = spv_emit_id(e);
        spv_emit_selection_merge(e, next); /* the merge must come just before its branch */
        spv_emit_branch_conditional(e, on, store, next);
        spv_emit_label(e, store);
        u32 base = spv_emit_imul_u32(e, u, place, spv_emit_constant_u32(e, u, x->key->stride[b] / 4u));
        for (u32 w = 0; w < x->key->count[b] && w < 128; w++) {
            u32 value = xfb_word(c, x, x->key->layout[b][w]);
            if (!value) continue;
            u32 at = spv_emit_iadd_u32(e, u, base, spv_emit_constant_u32(e, u, w));
            spv_emit_store(e, spv_emit_storage_access_u32(e, x->pointer, x->buffer[b], c->zero, at), value);
        }
        spv_emit_branch(e, next);
        spv_emit_label(e, next);
    }
    spv_emit_branch(e, done);
    spv_emit_label(e, done);
}

/* Transform feedback at an exit or emitted vertex: the stores, or each captured output's private vec4 copied into
   its component outputs, the variables that carry the extension's offsets */
void shader_xfb_store(DirectCtx* c, const XfbOut* x) {
    if (x->stores) {
        xfb_write(c, x);
        return;
    }
    for (u32 i = 0; i < 32; i++) {
        if (!x->attr_private[i]) continue;
        u32 value = spv_emit_load(&c->emit, x->vec4, x->output_attrs[i]);
        for (u32 k = 0; k < 4; k++)
            spv_emit_store(&c->emit, x->components[i][k], spv_emit_vec4_extract(&c->emit, c->f32_type, value, k));
    }
    for (u32 k = 0; k < 4; k++) {
        if (!x->position_copy[k]) continue;
        u32 value = spv_emit_load(&c->emit, x->vec4, x->position);
        spv_emit_store(&c->emit, x->position_copy[k], spv_emit_vec4_extract(&c->emit, c->f32_type, value, k));
    }
}

/* Hardware passthrough emits once at program exit. Save generated outputs before EmitVertex,
   which makes every output undefined, and merge forwarded components afresh for each vertex. */
void shader_geometry_passthrough(DirectCtx* c, const ShaderInfo* si, u32 vertices, u32 vec4, u32 position,
                                 u32 position_in, const u32* outputs, const u32* inputs, u32 point_size, u32 layer,
                                 u32 layer_type, u32 clip, u32 clip_count, u32 primitive_id, u32 primitive_id_out,
                                 int position_fix, u32 push, u32 push_pointer) {
    u32 saved[33] = {0}, saved_clip[8] = {0};
    for (u32 a = 0; a < 33; a++) {
        u32 out = a ? outputs[a - 1] : position;
        if (out) saved[a] = spv_emit_load(&c->emit, vec4, out);
    }
    u32 size = point_size ? spv_emit_load(&c->emit, c->f32_type, point_size) : 0;
    u32 saved_layer = layer ? spv_emit_load(&c->emit, layer_type, layer) : 0;
    for (u32 k = 0; k < clip_count; k++)
        saved_clip[k] = spv_emit_load(&c->emit, c->f32_type,
                                      spv_emit_output_element_pointer(&c->emit, c->f32_type, clip,
                                                                      spv_emit_constant_u32(&c->emit, c->u32_type, k)));
    u32 id = spv_emit_load(&c->emit, c->u32_type, primitive_id);
    u32 count = vertices == 4 ? 2 : vertices == 6 ? 3 : vertices;
    for (u32 v = 0; v < count; v++) {
        u32 index = spv_emit_constant_u32(&c->emit, c->u32_type, vertices == 4 ? v + 1 : vertices == 6 ? v * 2 : v);
        for (u32 a = 0; a < 33; a++) {
            u32 out = a ? outputs[a - 1] : position;
            if (!out) continue;
            u32 in = a ? inputs[a - 1] : position_in;
            u32 attr = a ? 32 + (a - 1) * 4 : 28;
            u32 mask = (~si->gp_skip_mask[attr / 32] >> (attr % 32)) & 15;
            u32 value = saved[a];
            for (u32 k = 0; k < 4; k++) {
                if (!(mask & (1u << k))) continue;
                u32 ptr = spv_emit_interface_scalar_pointer(&c->emit, 1, c->f32_type, in, index,
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, k));
                value = spv_emit_vec4_insert(&c->emit, vec4, value, spv_emit_load(&c->emit, c->f32_type, ptr), k);
            }
            spv_emit_store(&c->emit, out, value);
        }
        if (point_size) spv_emit_store(&c->emit, point_size, size);
        if (layer) spv_emit_store(&c->emit, layer, saved_layer);
        for (u32 k = 0; k < clip_count; k++)
            spv_emit_store(&c->emit,
                           spv_emit_output_element_pointer(&c->emit, c->f32_type, clip,
                                                           spv_emit_constant_u32(&c->emit, c->u32_type, k)),
                           saved_clip[k]);
        spv_emit_store(&c->emit, primitive_id_out, id);
        if (position_fix) shader_vertex_position_fix(c, position, push, push_pointer, vec4);
        int streams = 0;
        for (u32 buffer = 0; buffer < 4; buffer++) streams |= si->xfb.count[buffer] && si->xfb.stream[buffer];
        if (streams) spv_emit_void_op(&c->emit, 220, &c->zero, 1);
        else spv_emit_emit_vertex(&c->emit);
    }
    int streams = 0;
    for (u32 buffer = 0; buffer < 4; buffer++) streams |= si->xfb.count[buffer] && si->xfb.stream[buffer];
    if (streams) spv_emit_void_op(&c->emit, 221, &c->zero, 1);
    else spv_emit_end_primitive(&c->emit);
}

/* the attribute loads and stores, interpolation, geometry output and PIXLD */
void emit_io(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b) {
    int stage = t->stage;
    u32* sph = t->sph;
    int point_size_used = t->point_size_used;
    int between = t->between;
    u32 in_vertices = t->in_vertices;
    u32 out_vertices = t->out_vertices;
    u32 clip_used = t->clip_used;
    int needs_scale = t->needs_scale;
    DirectCtx* c = t->c;
    u32 i32_type = t->i32_type;
    u32 vec4 = t->vec4;
    u32 vec2 = t->vec2;
    u32 zero_f = t->zero_f;
    u32* inputs = t->inputs;
    u32* input_component_type = t->input_component_type;
    u32* input_vec_type = t->input_vec_type;
    u32 position = t->position;
    u32* output_attrs = t->output_attrs;
    u32 point_size = t->point_size;
    u32 layer_output = t->layer_output;
    u32 layer_input = t->layer_input;
    u32 viewport_output = t->viewport_output;
    u8* attr_private = t->attr_private;
    int xfb_on = t->xfb_on;
    u32 clip_output = t->clip_output;
    u32 position_in = t->position_in;
    u32* patch_vars = t->patch_vars;
    u32 tess_outer = t->tess_outer;
    u32 tess_inner = t->tess_inner;
    u32 tess_coord = t->tess_coord;
    u32 primitive_id = t->primitive_id;
    u32 primitive_id_out = t->primitive_id_out;
    u32 invocation_id = t->invocation_id;
    int position_fix = t->position_fix;
    u32 sample_id = t->sample_id;
    u32 coverage = t->coverage;
    u32 fragcoord = t->fragcoord;
    u32 frontface = t->frontface;
    u32 point_coord = t->point_coord;
    u32 instance_index = t->instance_index;
    u32 base_instance = t->base_instance;
    u32 vertex_index = t->vertex_index;
    u32 push_pointer = t->push_pointer;
    u32 push = t->push;
    if (op == O_ALD && a != 255) {
        /* indexed: each word at the register plus addr (and 4 for the next) is a declared input's component,
           the input picked by the address's vector (0x80 the first generic one, 0x70 the position after the
           vertex stage) and the component by its word. An address of no declared input reads 0, as in Eden */
        u32 width = B(w, 47, 2) + 1;
        u32 vertex = between ? shader_shared_index(c, reg_read(c, B(w, 39, 8)), in_vertices) : 0;
        u32 base = spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a),
                                     spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 10)));
        u32 two = spv_emit_constant_u32(&c->emit, c->u32_type, 2),
            three = spv_emit_constant_u32(&c->emit, c->u32_type, 3);
        u32 four = spv_emit_constant_u32(&c->emit, c->u32_type, 4);
        for (u32 k = 0; k < width; k++) {
            u32 offset =
                k ? spv_emit_iadd_u32(&c->emit, c->u32_type, base, spv_emit_constant_u32(&c->emit, c->u32_type, k * 4))
                  : base;
            u32 vector = spv_emit_shift_right_u32(&c->emit, c->u32_type, offset, four);
            u32 component = spv_emit_bitwise_and_u32(
                &c->emit, c->u32_type, spv_emit_shift_right_u32(&c->emit, c->u32_type, offset, two), three);
            u32 value = c->zero;
            for (u32 v = 0; v <= 32; v++) {
                u32 variable = v < 32 ? inputs[v] : between ? position_in : 0;
                if (!variable) continue;
                u32 type = v < 32 ? input_component_type[v] : c->f32_type;
                u32 bits = spv_emit_load(
                    &c->emit, type, spv_emit_interface_scalar_pointer(&c->emit, 1, type, variable, vertex, component));
                if (type != c->u32_type) bits = spv_emit_bitcast(&c->emit, c->u32_type, bits);
                u32 at = spv_emit_constant_u32(&c->emit, c->u32_type, v < 32 ? 8 + v : 7);
                value = spv_emit_select(&c->emit, c->u32_type, spv_emit_i_equal(&c->emit, c->bool_type, vector, at),
                                        bits, value);
            }
            reg_write(c, d + k, value);
        }
    } else if (op == O_ALD && (between || B(w, 31, 1))) {
        u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1, vertex = 0, k;
        int patch = B(w, 31, 1);
        int output = stage == ST_TCS && B(w, 32, 1) && !patch;
        if (!patch) vertex = shader_shared_index(c, reg_read(c, B(w, 39, 8)), output ? out_vertices : in_vertices);
        for (k = 0; k < width; k++) {
            u32 location = addr + k * 4, value = c->zero, pointer = 0, storage;
            u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, (location >> 2) & 3);
            if (patch) { /* the control program reads back what it wrote */
                storage = stage == ST_TCS ? 3 : 1;
                if (location >= 0x20)
                    pointer = spv_emit_interface_scalar_pointer(&c->emit, storage, c->f32_type,
                                                                patch_vars[(location - 0x20) >> 4], 0, component);
                else if (location < 0x18)
                    pointer = spv_emit_interface_scalar_pointer(
                        &c->emit, storage, c->f32_type, location < 0x10 ? tess_outer : tess_inner, 0,
                        spv_emit_constant_u32(&c->emit, c->u32_type, (location & 0xf) / 4));
            } else if (location == 0x2f0 || location == 0x2f4)
                pointer = spv_emit_interface_scalar_pointer(
                    &c->emit, 1, c->f32_type, tess_coord, 0,
                    spv_emit_constant_u32(&c->emit, c->u32_type, (location - 0x2f0) / 4));
            else if (location == 0x60)
                value = primitive_read(c, primitive_id);
            else if (location == 0x6c) {
                u32 variable = output ? point_size : t->point_size_in;
                if (variable) {
                    u32 type = spv_emit_type_pointer(&c->emit, output ? 3 : 1, c->f32_type);
                    pointer = SPV(&c->emit, SPV_OP_ACCESS_CHAIN, type, variable, vertex);
                }
            } else if (location >= 0x2c0 && location < 0x2e0) {
                u32 variable = output ? clip_output : t->clip_input;
                if (variable)
                    pointer = spv_emit_interface_scalar_pointer(&c->emit, output ? 3 : 1, c->f32_type, variable,
                         vertex, spv_emit_constant_u32(&c->emit, c->u32_type, (location - 0x2c0) / 4));
            }
            else if (output)
                pointer = spv_emit_interface_scalar_pointer(
                    &c->emit, location >= 0x80 && attr_private[(location - 0x80) >> 4] ? 6 : 3, c->f32_type,
                    location < 0x80 ? position : output_attrs[(location - 0x80) >> 4], vertex, component);
            else
                pointer = spv_emit_interface_scalar_pointer(
                    &c->emit, 1, c->f32_type, location < 0x80 ? position_in : inputs[(location - 0x80) >> 4], vertex,
                    component);
            if (pointer) value = spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_load(&c->emit, c->f32_type, pointer));
            reg_write(c, d + k, value);
        }
    } else if (op == O_ALD) {
        u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1;
        for (u32 k = 0; k < width; k++) {
            u32 location = addr + k * 4, index = (location - 0x80) >> 4;
            if (location == 0x2f8 || location == 0x2fc) {
                u32 instance;
                if (location == 0x2f8) {
                    instance = spv_emit_load(&c->emit, c->u32_type, instance_index);
                    u32 base_bits =
                        spv_emit_push_component_f32(&c->emit, push_pointer, push, c->zero, c->zero, c->f32_type);
                    u32 base = spv_emit_bitcast(&c->emit, c->u32_type, base_bits);
                    /* with draw parameters the push holds what the draw's first instance adds to the base
                       instance (render.c), which BaseInstance gives */
                    if (base_instance)
                        base = spv_emit_isub_u32(&c->emit, c->u32_type,
                                                 spv_emit_load(&c->emit, c->u32_type, base_instance), base);
                    instance = spv_emit_isub_u32(&c->emit, c->u32_type, instance, base);
                } else
                    instance = spv_emit_load(&c->emit, c->u32_type, vertex_index);
                reg_write(c, d + k, instance);
            } else {
                u32 vector = spv_emit_load(&c->emit, input_vec_type[index], inputs[index]);
                u32 value = spv_emit_vec4_extract(&c->emit, input_component_type[index], vector, (location >> 2) & 3);
                reg_write(c, d + k,
                          input_component_type[index] == c->u32_type ? value
                                                                     : spv_emit_bitcast(&c->emit, c->u32_type, value));
            }
        }
    } else if (op == O_AST && (stage == ST_TCS || B(w, 31, 1))) {
        /* a control program stores its invocation's output vertex, and the patch's levels and vectors */
        u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1, k;
        u32 vertex = B(w, 31, 1) ? 0 : spv_emit_load(&c->emit, c->u32_type, invocation_id);
        for (k = 0; k < width; k++) {
            u32 location = addr + k * 4, pointer = 0;
            /* A later evaluation stage makes the control program's layer and viewport outputs inert. */
            if (!B(w, 31, 1) && (location == 0x64 || location == 0x68)) continue;
            u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, (location >> 2) & 3);
            if (!B(w, 31, 1) && location == 0x6c && point_size) {
                u32 type = spv_emit_type_pointer(&c->emit, 3, c->f32_type);
                pointer = SPV(&c->emit, SPV_OP_ACCESS_CHAIN, type, point_size, vertex);
            } else if (!B(w, 31, 1) && location >= 0x2c0 && location < 0x2e0) {
                if (clip_output && (clip_used & (1u << ((location - 0x2c0) / 4))))
                    pointer = spv_emit_interface_scalar_pointer(&c->emit, 3, c->f32_type, clip_output, vertex,
                        spv_emit_constant_u32(&c->emit, c->u32_type, (location - 0x2c0) / 4));
            } else if (!B(w, 31, 1))
                pointer = spv_emit_interface_scalar_pointer(
                    &c->emit, location >= 0x80 && attr_private[(location - 0x80) >> 4] ? 6 : 3, c->f32_type,
                    location < 0x80 ? position : output_attrs[(location - 0x80) >> 4], vertex, component);
            else if (location >= 0x20)
                pointer = spv_emit_interface_scalar_pointer(&c->emit, 3, c->f32_type,
                                                            patch_vars[(location - 0x20) >> 4], 0, component);
            else if (location < 0x18)
                pointer = spv_emit_interface_scalar_pointer(
                    &c->emit, 3, c->f32_type, location < 0x10 ? tess_outer : tess_inner, 0,
                    spv_emit_constant_u32(&c->emit, c->u32_type, (location & 0xf) / 4));
            if (pointer) spv_emit_store(&c->emit, pointer, reg_read_as(c, d + k, c->f32_type));
        }
    } else if (op == O_AST) {
        u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1;
        for (u32 k = 0; k < width; k++) {
            u32 location = addr + k * 4;
            if (location == 0x6c) {
                if (point_size) spv_emit_store(&c->emit, point_size, reg_read_as(c, d + k, c->f32_type));
                continue;
            }
            if (location == 0x68) {
                spv_emit_store(&c->emit, viewport_output, reg_read_as(c, d + k, i32_type));
                continue;
            }
            if (location == 0x64) {
                if (layer_output) /* none: the device can't take this stage's layer */
                    spv_emit_store(&c->emit, layer_output, reg_read_as(c, d + k, i32_type));
                continue;
            }
            /* the viewport mask, 0 or viewport 0 alone (the prescan's check): no broadcast, the primitive keeps the
               viewport its index selects, as Eden and Ryujinx draw it on hosts without NV_viewport_array2 */
            if (location == 0x3a0) continue;
            if (location >= 0x2c0) {
                u32 index = (location - 0x2c0) / 4;
                if (clip_used & (1u << index)) {
                    u32 ptr = spv_emit_output_element_pointer(&c->emit, c->f32_type, clip_output,
                                                              spv_emit_constant_u32(&c->emit, c->u32_type, index));
                    spv_emit_store(&c->emit, ptr, reg_read_as(c, d + k, c->f32_type));
                }
                continue;
            }
            u32 variable = location < 0x80 ? position : output_attrs[(location - 0x80) >> 4];
            u32 vector = spv_emit_load(&c->emit, vec4, variable);
            u32 value = reg_read_as(c, d + k, c->f32_type);
            vector = spv_emit_vec4_insert(&c->emit, vec4, vector, value, (location >> 2) & 3);
            spv_emit_store(&c->emit, variable, vector);
        }
    } else if (op == O_OUT_R || op == O_OUT_C || op == O_OUT_I) {
        u32 stream = op == O_OUT_I ? spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 2))
                     : op == O_OUT_C ? shader_cbuf_word(c, w)
                                        : reg_read(c, b);
        stream = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, stream,
                                           spv_emit_constant_u32(&c->emit, c->u32_type, 3));
        for (u32 target = 0; target < 4; target++) {
            if (target && !(t->stream_mask & (1u << target))) continue;
            u32 literal = spv_emit_constant_u32(&c->emit, c->u32_type, target);
            u32 emit_label = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
            u32 selected = spv_emit_i_equal(&c->emit, c->bool_type, stream, literal);
            spv_emit_selection_merge(&c->emit, merge);
            spv_emit_branch_conditional(&c->emit, selected, emit_label, merge);
            spv_emit_label(&c->emit, emit_label);
            if (B(w, 39, 1)) {
                if (!target) {
                    if (xfb_on) shader_xfb_store(c, t->xfb_out); /* the position as written, before its fix */
                    if (position_fix) shader_vertex_position_fix(c, position, push, push_pointer, vec4);
                    spv_emit_store(&c->emit, primitive_id_out, spv_emit_load(&c->emit, c->u32_type, primitive_id));
                } else {
                    for (u32 buffer = 0; buffer < 4; buffer++) {
                        if (t->si->xfb.stream[buffer] != target) continue;
                        for (u32 word = 0; word < t->si->xfb.count[buffer] && word < 128; word++) {
                            u32 variable = t->stream_capture[buffer][word];
                            if (!variable) continue;
                            u32 attr = t->si->xfb.layout[buffer][word], value = zero_f;
                            if (shader_legacy_group(attr * 4) >= 0) attr = shader_legacy_address(attr * 4) / 4;
                            if (attr >= 32 && output_attrs[(attr - 32) / 4])
                                value = spv_emit_vec4_extract(&c->emit, c->f32_type,
                                                               spv_emit_load(&c->emit, vec4,
                                                                               output_attrs[(attr - 32) / 4]),
                                                               attr & 3);
                            else if (attr >= 28 && attr <= 31 && position)
                                value = spv_emit_vec4_extract(&c->emit, c->f32_type,
                                                               spv_emit_load(&c->emit, vec4, position), attr - 28);
                            else if (attr == 27 && point_size)
                                value = spv_emit_load(&c->emit, c->f32_type, point_size);
                            else if (attr == 25 && layer_output)
                                value = spv_emit_bitcast(&c->emit, c->f32_type,
                                                          spv_emit_load(&c->emit, i32_type, layer_output));
                            else if (attr == 26 && viewport_output)
                                value = spv_emit_bitcast(&c->emit, c->f32_type,
                                                          spv_emit_load(&c->emit, i32_type, viewport_output));
                            spv_emit_store(&c->emit, variable, value);
                        }
                    }
                }
                if (t->stream_mask) spv_emit_void_op(&c->emit, 220, &literal, 1);
                else spv_emit_emit_vertex(&c->emit);
                if (point_size && !point_size_used) spv_emit_store(&c->emit, point_size, t->default_point_size);
            }
            if (B(w, 40, 1)) {
                if (t->stream_mask) spv_emit_void_op(&c->emit, 221, &literal, 1);
                else spv_emit_end_primitive(&c->emit);
            }
            spv_emit_branch(&c->emit, merge);
            spv_emit_label(&c->emit, merge);
        }
        reg_write(c, d, c->zero);
    } else if (op == O_AL2P) {
        u32 offset = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 11));
        reg_write(c, d, spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), offset));
    } else if (op == O_ISBERD) {
        reg_write(c, d, reg_read(c, a));
    } else if (op == O_IPA && B(w, 38, 1)) {
        /* indexed: the generic input at the index register's address, picked among those the header gives a
           mode by its vector (one nothing feeds reads 0, 0, 0, 1) and interpolated in its own mode, then the
           instruction's mode and saturation, as below */
        u32 offset = reg_read(c, a);
        u32 vector =
            spv_emit_shift_right_u32(&c->emit, c->u32_type, offset, spv_emit_constant_u32(&c->emit, c->u32_type, 4));
        u32 component = spv_emit_bitwise_and_u32(
            &c->emit, c->u32_type,
            spv_emit_shift_right_u32(&c->emit, c->u32_type, offset, spv_emit_constant_u32(&c->emit, c->u32_type, 2)),
            spv_emit_constant_u32(&c->emit, c->u32_type, 3));
        u32 last = spv_emit_i_equal(&c->emit, c->bool_type, component, spv_emit_constant_u32(&c->emit, c->u32_type, 3));
        u32 wcoord =
            fragcoord ? spv_emit_vec4_extract(&c->emit, c->f32_type, spv_emit_load(&c->emit, vec4, fragcoord), 3) : 0;
        u32 value = zero_f;
        for (u32 v = 0; v < 32; v++) {
            u32 bits;
            if (!((sph[6 + v / 4] >> (v % 4 * 8)) & 0xff)) continue;
            if (inputs[v]) {
                bits =
                    spv_emit_load(&c->emit, c->f32_type,
                                  spv_emit_interface_scalar_pointer(&c->emit, 1, c->f32_type, inputs[v], 0, component));
                if (shader_ipa_interpolation(sph, v) == 2 && wcoord) {
                    bits = spv_emit_fmul(&c->emit, c->f32_type, bits, wcoord);
                    spv_emit_no_contraction(&c->emit, bits);
                }
            } else /* no output feeds it (in_unfed_mask) */
                bits = spv_emit_select(&c->emit, c->f32_type, last, spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f),
                                       zero_f);
            value = spv_emit_select(
                &c->emit, c->f32_type,
                spv_emit_i_equal(&c->emit, c->bool_type, vector, spv_emit_constant_u32(&c->emit, c->u32_type, 8 + v)),
                bits, value);
        }
        if (B(w, 54, 2) == 1) {
            value = spv_emit_fmul(&c->emit, c->f32_type, value, reg_read_as(c, b, c->f32_type));
            spv_emit_no_contraction(&c->emit, value);
        }
        value = shader_float_saturate(c, value, B(w, 51, 1));
        reg_write_as(c, d, value, c->f32_type);
    } else if (op == O_IPA) {
        u32 addr = B(w, 30, 8) * 4;
        u32 component = (addr >> 2) & 3;
        u32 value;
        u32 interpolation = 0;
        if (addr == 0x64) {
            value = layer_input ? spv_emit_bitcast(&c->emit, c->f32_type,
                                                   spv_emit_bitcast(&c->emit, c->u32_type,
                                                                    spv_emit_load(&c->emit, i32_type, layer_input)))
                                : spv_emit_bitcast(&c->emit, c->f32_type, c->zero); /* no capability: layer 0 */
        } else if (addr == 0x3fc) {
            u32 facing = spv_emit_load(&c->emit, c->bool_type, frontface);
            u32 bits = spv_emit_select(&c->emit, c->u32_type, facing,
                                       spv_emit_constant_u32(&c->emit, c->u32_type, 0xffffffffu), c->zero);
            value = spv_emit_bitcast(&c->emit, c->f32_type, bits);
        } else if (addr == 0x60) {
            value = spv_emit_bitcast(&c->emit, c->f32_type,
                                     primitive_read(c, primitive_id));
        } else if (addr == 0x2e0 || addr == 0x2e4) {
            value = spv_emit_vec4_extract(&c->emit, c->f32_type, spv_emit_load(&c->emit, vec2, point_coord), component);
            if (addr == 0x2e4) { /* T flips with the sprite's origin, a push constant (see vk/render.c) */
                u32 flip = spv_emit_push_component_f32(&c->emit, push_pointer, push, c->zero,
                                                       spv_emit_constant_u32(&c->emit, c->u32_type, 3), c->f32_type);
                u32 flipped = spv_emit_fadd(&c->emit, c->f32_type, spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f),
                                            spv_emit_fnegate(&c->emit, c->f32_type, value));
                value = spv_emit_select(&c->emit, c->f32_type,
                                        spv_emit_fcompare(&c->emit, c->bool_type, 5, flip, zero_f), flipped, value);
            }
        } else if (addr >= 0x80) {
            u32 input = (addr - 0x80) >> 4;
            if (inputs[input]) {
                value = spv_emit_load(&c->emit, vec4, inputs[input]);
                value = spv_emit_vec4_extract(&c->emit, c->f32_type, value, component);
            } else /* no output feeds it (in_unfed_mask) */
                value = spv_emit_constant_f32(&c->emit, c->f32_type, component == 3 ? 1.0f : 0.0f);
            interpolation = shader_ipa_interpolation(sph, input);
        }
        if (addr >= 0x70 && addr < 0x80) {
            u32 coord = spv_emit_load(&c->emit, vec4, fragcoord);
            value = spv_emit_vec4_extract(&c->emit, c->f32_type, coord, component);
            /* a[0x7c] is the interpolated 1/w, FragCoord.w as it is: shaders take its reciprocal for
               IPA.MUL */
            if (addr != 0x7c && needs_scale && component < 2) {
                u32 zero_index = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
                u32 scale = spv_emit_uniform_component(&c->emit, c->scale_pointer, c->scale_buffer, zero_index,
                                                       zero_index, zero_index, c->f32_type);
                value = spv_emit_fmul(&c->emit, c->f32_type, value, scale);
                spv_emit_no_contraction(&c->emit, value);
            }
            if (addr == 0x74) value = shader_frag_y_origin(c, push, value);
        } else if (interpolation == 2) {
            /* The hardware interpolates attr/w for a perspective input, and the shader multiplies by w
               (IPA.MUL with the reciprocal of a[0x7c]). Vulkan's input is attr itself: times the
               interpolated 1/w, FragCoord.w, it is what the hardware gives. */
            u32 coord = spv_emit_load(&c->emit, vec4, fragcoord);
            u32 wcoord = spv_emit_vec4_extract(&c->emit, c->f32_type, coord, 3);
            value = spv_emit_fmul(&c->emit, c->f32_type, value, wcoord);
            spv_emit_no_contraction(&c->emit, value);
        }
        if (B(w, 54, 2) == 1) {
            u32 rhs = reg_read_as(c, b, c->f32_type);
            value = spv_emit_fmul(&c->emit, c->f32_type, value, rhs);
            spv_emit_no_contraction(&c->emit, value);
        }
        value = shader_float_saturate(c, value, B(w, 51, 1));
        reg_write_as(c, d, value, c->f32_type);
    } else if (op == O_PIXLD) {
        reg_write(c, d,
                  B(w, 31, 3) == 5 ? spv_emit_load(&c->emit, c->u32_type, sample_id)
                                   : spv_emit_load(&c->emit, c->u32_type,
                                                   spv_emit_interface_scalar_pointer(&c->emit, 1, c->u32_type, coverage,
                                                                                     0, c->zero)));
    }
}
