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

void shader_vertex_depth_fix(DirectCtx* c, u32 position, u32 push, u32 push_pointer, u32 vec4_type) {
    u32 member = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
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

/* Transform feedback: each captured output's private vec4 (ShaderInfo.xfb) copied into its component outputs, the
   variables that carry the capture's offsets, before the program ends or emits a vertex */
void shader_xfb_store(DirectCtx* c, u32 vec4, const u32* output_attrs, const u8* attr_private, u32 (*components)[4]) {
    for (u32 i = 0; i < 32; i++) {
        if (!attr_private[i]) continue;
        u32 value = spv_emit_load(&c->emit, vec4, output_attrs[i]);
        for (u32 k = 0; k < 4; k++)
            spv_emit_store(&c->emit, components[i][k], spv_emit_vec4_extract(&c->emit, c->f32_type, value, k));
    }
}

/* Hardware passthrough emits once at program exit. Save generated outputs before EmitVertex,
   which makes every output undefined, and merge forwarded components afresh for each vertex. */
void shader_geometry_passthrough(DirectCtx* c, const ShaderInfo* si, u32 vertices, u32 vec4, u32 position,
                                 u32 position_in, const u32* outputs, const u32* inputs, u32 point_size, u32 layer,
                                 u32 layer_type, u32 clip, u32 clip_count, u32 primitive_id, u32 primitive_id_out,
                                 int depth_fix, u32 push, u32 push_pointer) {
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
        if (depth_fix) shader_vertex_depth_fix(c, position, push, push_pointer, vec4);
        spv_emit_emit_vertex(&c->emit);
    }
    spv_emit_end_primitive(&c->emit);
}
