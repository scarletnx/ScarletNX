/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Attributes and varyings, the stages' fixed outputs (position, depth, clip distances, transform
   feedback), and the fragment stage's alpha test and coordinates */
#include "nx/gpu/shader/translate.h"


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

/* Whether a plain IPA of the attribute word at addr reads one of the position pass's inputs (pos_in, a mask of
   generics) where the pass has its value: a perspective one's x, y or w */
int shader_ipa_position(u32 pos_in, const u32* sph, u32 addr) {
    u32 input = (addr - 0x80) >> 4;
    return addr >= 0x80 && addr < 0x280 && (pos_in >> input & 1) && (addr >> 2 & 3) != 2 &&
           shader_ipa_interpolation(sph, input) == 2;
}

void shader_alpha_test(DirectCtx* c, u32 push, u32 push_pointer, u32 alpha) {
    /* the function is the pipeline's (SH_SPEC_ALPHA: always, with the test off), so the driver keeps the one
       comparison of the draw, or none; the reference stays a push constant, as any value of a draw's */
    u32 function = shader_spec(c, SH_SPEC_ALPHA, SH_SPEC_ALPHA_ALWAYS);
    u32 reference = spv_emit_push_component_f32(&c->emit, push_pointer, push, c->zero, shader_u32(c, 2), c->f32_type);
    u32 pass = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, function, shader_u32(c, SH_SPEC_ALPHA_ALWAYS));
    for (u32 mode = 1; mode <= 6; mode++) {
        u32 selected = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, function, shader_u32(c, mode));
        u32 comparison = spv_emit_fcompare(&c->emit, c->bool_type, mode, alpha, reference);
        u32 result = spv_emit_bool_binary(&c->emit, c->bool_type, 0, selected, comparison);
        pass = spv_emit_bool_binary(&c->emit, c->bool_type, 1, pass, result);
    }
    u32 fail = spv_emit_unary(&c->emit, SPV_OP_LOGICAL_NOT, c->bool_type, pass);
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
    u32 one = shader_u32(c, 1);
    u32 pointer = spv_emit_type_pointer(&c->emit, SPV_STORAGE_PUSH_CONSTANT, c->u32_type);
    u32 lower_left = shader_spec(c, SH_SPEC_LOWER_LEFT, 0);
    u32 height = spv_emit_push_component_f32(&c->emit, pointer, push, one, shader_u32(c, 3), c->u32_type);
    u32 up = spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type,
                             spv_emit_unary(&c->emit, SPV_OP_CONVERT_U_TO_F, c->f32_type, height),
                             spv_emit_unary(&c->emit, SPV_OP_F_NEGATE, c->f32_type, y));
    spv_emit_no_contraction(&c->emit, up);
    return spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->f32_type,
                            spv_emit_binary(&c->emit, SPV_OP_I_NOT_EQUAL, c->bool_type, lower_left, c->zero), up, y);
}

/* The last stage's position as the rasterizer takes it. A viewport coordinate swizzle in the key (SNX_SH_SWIZZLE)
   gives each component the one its code names, negated by the code's bit 0, before clipping as the 3D engine does.
   With the viewport transform off (SNX_SH_WINDOW_XY), which no swizzle comes with, x and y are pixels of the surface
   clip, the viewport then (vk/render.c): x * 2 / width - 1 and y * 2 / height - 1 take them to clip space, the width
   and height in bits 0-15 and 16-31 of the push constant's w, as Eden's position_pass.cpp does with its render area.
   Then z goes from -w..w to 0..w where the push constant's y says the depth mode is -1 to 1 */
void shader_vertex_position_fix(DirectCtx* c, u32 position, u32 push, u32 push_pointer, u32 vec4_type) {
    u32 member = shader_u32(c, 0);
    if (c->swizzle & SNX_SH_SWIZZLE) {
        u32 value = spv_emit_load(&c->emit, vec4_type, position), swizzled = value;
        for (u32 k = 0; k < 4; k++) {
            u32 code = (c->swizzle >> (SNX_SH_SWIZZLE_SHIFT + 4 * k)) & 7;
            u32 part = spv_emit_vec4_extract(&c->emit, c->f32_type, value, code >> 1);
            if (code & 1) part = spv_emit_unary(&c->emit, SPV_OP_F_NEGATE, c->f32_type, part);
            swizzled = spv_emit_vec4_insert(&c->emit, vec4_type, swizzled, part, k);
        }
        spv_emit_store(&c->emit, position, swizzled);
    }
    if (c->window_xy) {
        u32 size = spv_emit_unary(
            &c->emit, SPV_OP_BITCAST, c->u32_type,
            spv_emit_push_component_f32(&c->emit, push_pointer, push, member, shader_u32(c, 3), c->f32_type));
        u32 extent[2] = {spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, size, shader_u32(c, 0xFFFF)),
                         spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, size, shader_u32(c, 16))};
        u32 two = spv_emit_constant_f32(&c->emit, c->f32_type, 2.0f);
        u32 minus_one = spv_emit_constant_f32(&c->emit, c->f32_type, -1.0f);
        u32 value = spv_emit_load(&c->emit, vec4_type, position);
        for (u32 k = 0; k < 2; k++) {
            u32 scale = spv_emit_binary(&c->emit, SPV_OP_F_DIV, c->f32_type, two,
                                        spv_emit_unary(&c->emit, SPV_OP_CONVERT_U_TO_F, c->f32_type, extent[k]));
            u32 pixels = spv_emit_vec4_extract(&c->emit, c->f32_type, value, k);
            u32 clip = spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type,
                                       spv_emit_binary(&c->emit, SPV_OP_F_MUL, c->f32_type, pixels, scale), minus_one);
            value = spv_emit_vec4_insert(&c->emit, vec4_type, value, clip, k);
        }
        spv_emit_store(&c->emit, position, value);
    }
    u32 component = shader_u32(c, 1);
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
    u32 sum = spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type, z, w);
    spv_emit_no_contraction(&c->emit, sum);
    u32 half = spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f);
    u32 depth = spv_emit_binary(&c->emit, SPV_OP_F_MUL, c->f32_type, sum, half);
    spv_emit_no_contraction(&c->emit, depth);
    value = spv_emit_vec4_insert(&c->emit, vec4_type, value, depth, 2);
    spv_emit_store(&c->emit, position, value);
    spv_emit_branch(&c->emit, merge);
    spv_emit_label(&c->emit, merge);
    if (c->position_copy) spv_emit_store(&c->emit, c->position_copy, spv_emit_load(&c->emit, vec4_type, position));
}

/* The fragment's own x and y in clip space over its w, from gl_FragCoord and the draw's viewport 0 (x, y, width and
   height as the draw sets them, the push constant's third vector: vk/render.c): what a perspective input holding the
   position reads as over w, with the precision of the rasterizer's position rather than of the host's interpolation
   of a clipped primitive's inputs (shader.h, SH_VARYING_) */
void shader_frag_ndc(DirectCtx* c, u32 fragcoord, u32 vec4_type, u32 push, u32 push_pointer, u32 ndc[2]) {
    u32 member = spv_emit_constant_u32(&c->emit, c->u32_type, 2);
    u32 two = spv_emit_constant_f32(&c->emit, c->f32_type, 2.0f);
    u32 minus_one = spv_emit_constant_f32(&c->emit, c->f32_type, -1.0f);
    for (u32 k = 0; k < 2; k++) {
        u32 origin = spv_emit_push_component_f32(&c->emit, push_pointer, push, member,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, k), c->f32_type);
        u32 extent = spv_emit_push_component_f32(&c->emit, push_pointer, push, member,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, k + 2), c->f32_type);
        u32 from = spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type,
                                   shader_input_component(c, vec4_type, fragcoord, c->f32_type, k),
                                   spv_emit_unary(&c->emit, SPV_OP_F_NEGATE, c->f32_type, origin));
        u32 across = spv_emit_binary(&c->emit, SPV_OP_F_MUL, c->f32_type,
                                     spv_emit_binary(&c->emit, SPV_OP_F_DIV, c->f32_type, from, extent), two);
        spv_emit_no_contraction(&c->emit, across);
        ndc[k] = spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type, across, minus_one);
        spv_emit_no_contraction(&c->emit, ndc[k]);
    }
}

/* A per-vertex input's component at one of the triangle's vertices: at[vertex] is the input's array of the three
   (PerVertexKHR), or with flat the flat input that holds that vertex's value (SH_VARYING_GEOMETRY) */
u32 shader_vertex_value(DirectCtx* c, u32 vec4_type, const u32 at[3], int flat, u32 vertex, u32 component) {
    SpvEmitter* e = &c->emit;
    if (flat) return shader_input_component(c, vec4_type, at[vertex], c->f32_type, component);
    return spv_emit_load(e, c->f32_type,
                         spv_emit_interface_scalar_pointer(e, SPV_STORAGE_INPUT, c->f32_type, at[vertex],
                                                           spv_emit_constant_u32(e, c->u32_type, vertex),
                                                           spv_emit_constant_u32(e, c->u32_type, component)));
}

/* The fragment's weights of its triangle's second and third vertices (the first's is what is left of 1), from the
   three vertices' clip space positions (position, the per-vertex input that holds them) and the fragment's own place
   (ndc, shader_frag_ndc): with q = (ndc.x, ndc.y, 1) and v the vertices' (x, y, w), vertex i's is q . (vj x vk) over
   the sum of the three, as Eden's BarycentricVaryingPass has them. They are the weights the console interpolates
   with, of the primitive as the program gave it and not of what the host's clipping left of it */
void shader_frag_weights(DirectCtx* c, u32 vec4_type, const u32 position[3], int flat, const u32 ndc[2],
                         u32 weight[2]) {
    SpvEmitter* e = &c->emit;
    u32 f = c->f32_type, v[3][3], edge[3];
    for (u32 k = 0; k < 3; k++)
        for (u32 i = 0; i < 3; i++) v[k][i] = shader_vertex_value(c, vec4_type, position, flat, k, i == 2 ? 3 : i);
    for (u32 k = 0; k < 3; k++) {
        const u32 *a = v[(k + 1) % 3], *b = v[(k + 2) % 3];
        u32 cross[3];
        for (u32 i = 0; i < 3; i++) {
            u32 j = (i + 1) % 3, l = (i + 2) % 3;
            cross[i] =
                spv_emit_binary(e, SPV_OP_F_ADD, f, spv_emit_binary(e, SPV_OP_F_MUL, f, a[j], b[l]),
                                spv_emit_unary(e, SPV_OP_F_NEGATE, f, spv_emit_binary(e, SPV_OP_F_MUL, f, a[l], b[j])));
        }
        edge[k] = spv_emit_binary(
            e, SPV_OP_F_ADD, f, spv_emit_binary(e, SPV_OP_F_MUL, f, cross[0], ndc[0]),
            spv_emit_binary(e, SPV_OP_F_ADD, f, spv_emit_binary(e, SPV_OP_F_MUL, f, cross[1], ndc[1]), cross[2]));
    }
    u32 sum = spv_emit_binary(e, SPV_OP_F_ADD, f, edge[0], spv_emit_binary(e, SPV_OP_F_ADD, f, edge[1], edge[2]));
    for (u32 k = 0; k < 2; k++) weight[k] = spv_emit_binary(e, SPV_OP_F_DIV, f, edge[k + 1], sum);
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
    return spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type, value);
}

/* Transform feedback by stores: a vertex's place is VertexIndex - first + (InstanceIndex - first instance) * the
   vertices an instance captures, and below the draw's count each layout word goes to place * stride / 4 + its index */
static void xfb_write(DirectCtx* c, const XfbOut* x) {
    SpvEmitter* e = &c->emit;
    u32 u = c->u32_type, p[5];
    for (u32 i = 0; i < 5; i++)
        p[i] = spv_emit_load(
            e, u, spv_emit_storage_access_u32(e, x->pointer, x->params, c->zero, spv_emit_constant_u32(e, u, i)));
    u32 vertex = spv_emit_binary(e, SPV_OP_I_SUB, u, spv_emit_load(e, u, x->vertex_index), p[0]);
    u32 instance = spv_emit_binary(e, SPV_OP_I_SUB, u, spv_emit_load(e, u, x->instance_index), p[1]);
    u32 place = spv_emit_iadd_u32(e, u, vertex, spv_emit_binary(e, SPV_OP_I_MUL, u, instance, p[2]));
    u32 in = spv_emit_bool_binary(e, c->bool_type, 0,
                                  spv_emit_binary(e, SPV_OP_U_LESS_THAN, c->bool_type, vertex, p[2]), /* and */
                                  spv_emit_binary(e, SPV_OP_U_LESS_THAN, c->bool_type, place, p[3]));
    u32 yes = spv_emit_id(e), done = spv_emit_id(e);
    spv_emit_selection_merge(e, done);
    spv_emit_branch_conditional(e, in, yes, done);
    spv_emit_label(e, yes);
    for (u32 b = 0; b < 4; b++) {
        if (!x->buffer[b]) continue;
        /* a buffer that the guest left off has a 16-byte placeholder binding: no store goes there (p[4], the mask) */
        u32 bit = spv_emit_binary(e, SPV_OP_BITWISE_AND, u, p[4], spv_emit_constant_u32(e, u, 1u << b));
        u32 on = spv_emit_binary(e, SPV_OP_I_NOT_EQUAL, c->bool_type, bit, c->zero), store = spv_emit_id(e),
            next = spv_emit_id(e);
        spv_emit_selection_merge(e, next); /* the merge must come just before its branch */
        spv_emit_branch_conditional(e, on, store, next);
        spv_emit_label(e, store);
        u32 base = spv_emit_binary(e, SPV_OP_I_MUL, u, place, spv_emit_constant_u32(e, u, x->key->stride[b] / 4u));
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
                                      spv_emit_output_element_pointer(&c->emit, c->f32_type, clip, shader_u32(c, k)));
    u32 id = spv_emit_load(&c->emit, c->u32_type, primitive_id);
    u32 count = vertices == 4 ? 2 : vertices == 6 ? 3 : vertices;
    for (u32 v = 0; v < count; v++) {
        u32 index = shader_u32(c, vertices == 4 ? v + 1 : vertices == 6 ? v * 2 : v);
        for (u32 a = 0; a < 33; a++) {
            u32 out = a ? outputs[a - 1] : position;
            if (!out) continue;
            u32 in = a ? inputs[a - 1] : position_in;
            u32 attr = a ? 32 + (a - 1) * 4 : 28;
            u32 mask = (~si->gp_skip_mask[attr / 32] >> (attr % 32)) & 15;
            u32 value = saved[a];
            for (u32 k = 0; k < 4; k++) {
                if (!(mask & (1u << k))) continue;
                u32 ptr = spv_emit_interface_scalar_pointer(&c->emit, SPV_STORAGE_INPUT, c->f32_type, in, index,
                                                            shader_u32(c, k));
                value = spv_emit_vec4_insert(&c->emit, vec4, value, spv_emit_load(&c->emit, c->f32_type, ptr), k);
            }
            spv_emit_store(&c->emit, out, value);
        }
        if (point_size) spv_emit_store(&c->emit, point_size, size);
        if (layer) spv_emit_store(&c->emit, layer, saved_layer);
        for (u32 k = 0; k < clip_count; k++)
            spv_emit_store(&c->emit, spv_emit_output_element_pointer(&c->emit, c->f32_type, clip, shader_u32(c, k)),
                           saved_clip[k]);
        spv_emit_store(&c->emit, primitive_id_out, id);
        if (position_fix) shader_vertex_position_fix(c, position, push, push_pointer, vec4);
        int streams = 0;
        for (u32 buffer = 0; buffer < 4; buffer++) streams |= si->xfb.count[buffer] && si->xfb.stream[buffer];
        if (streams)
            spv_emit_void_op(&c->emit, SPV_OP_EMIT_STREAM_VERTEX, &c->zero, 1);
        else spv_emit_emit_vertex(&c->emit);
    }
    int streams = 0;
    for (u32 buffer = 0; buffer < 4; buffer++) streams |= si->xfb.count[buffer] && si->xfb.stream[buffer];
    if (streams)
        spv_emit_void_op(&c->emit, SPV_OP_END_STREAM_PRIMITIVE, &c->zero, 1);
    else spv_emit_end_primitive(&c->emit);
}

/* the attribute loads and stores, interpolation, geometry output and PIXLD */
