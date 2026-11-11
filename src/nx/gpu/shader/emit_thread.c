/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Geometry invocation information follows Citron's emit_spirv_context_get_set.cpp:
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project */
/* The system registers, the subgroup's votes and shuffles, and the barriers */
#include "nx/gpu/shader/translate.h"

/* the system registers, the subgroup's votes and shuffles, and the barriers */
void emit_thread(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b) {
    int stage = t->stage;
    int between = t->between;
    u32 in_vertices = t->in_vertices;
    int lane_model = t->lane_model;
    int lane_is_invocation = t->lane_is_invocation;
    DirectCtx* c = t->c;
    u32 vec4_u32 = t->vec4_u32;
    u32 vec2_u32 = t->vec2_u32;
    u32 vec3_u32 = t->vec3_u32;
    u32 invocation_id = t->invocation_id;
    u32 patch_vertices = t->patch_vertices;
    u32 local_input = t->local_input;
    u32 group_input = t->group_input;
    if (op == O_S2R) {
        u32 sr = B(w, 20, 8);
        u32 value = c->zero;
        if (stage == ST_CS && ((sr >= 0x21 && sr <= 0x23) || (sr >= 0x25 && sr <= 0x27))) {
            u32 local = sr <= 0x23;
            u32 group = spv_emit_load(&c->emit, vec3_u32, local ? local_input : group_input);
            value = spv_emit_vec4_extract(&c->emit, c->u32_type, group, local ? sr - 0x21 : sr - 0x25);
        } else if (sr == 0 || (sr >= 0x38 && sr <= 0x3c)) {
            /* SR_LANEID, and the EQ, LT, LE, GT and GE lane masks of a 32-lane warp */
            u32 lane = lane_is_invocation ? spv_emit_load(&c->emit, c->u32_type, invocation_id)
                       : lane_model       ? c->zero
                                          : shader_input(c, c->u32_type, c->subgroup_lane_input);
            lane = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, lane, shader_u32(c, 31));
            u32 one = shader_u32(c, 1);
            u32 eq = spv_emit_binary(&c->emit, SPV_OP_SHIFT_LEFT_LOGICAL, c->u32_type, one, lane);
            u32 lt = spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, eq, one);
            u32 le = spv_emit_binary(&c->emit, SPV_OP_BITWISE_OR, c->u32_type, lt, eq);
            value = sr == 0      ? lane
                    : sr == 0x38 ? eq
                    : sr == 0x39 ? lt
                    : sr == 0x3a ? le
                    : sr == 0x3b ? spv_emit_unary(&c->emit, SPV_OP_NOT, c->u32_type, le)
                                 : spv_emit_unary(&c->emit, SPV_OP_NOT, c->u32_type, lt);
        } else if (stage == ST_CS && sr == 0x20) {
            /* SR_TID packed: x in bits 0-15, y in 16-25 and z in 26-31, the widths of the largest block (Eden
               gives y 8 bits) */
            u32 tid = spv_emit_load(&c->emit, vec3_u32, local_input);
            value = spv_emit_vec4_extract(&c->emit, c->u32_type, tid, 0);
            value = spv_emit_bitfield_insert(&c->emit, c->u32_type, value,
                                             spv_emit_vec4_extract(&c->emit, c->u32_type, tid, 1), shader_u32(c, 16),
                                             shader_u32(c, 10));
            value = spv_emit_bitfield_insert(&c->emit, c->u32_type, value,
                                             spv_emit_vec4_extract(&c->emit, c->u32_type, tid, 2), shader_u32(c, 26),
                                             shader_u32(c, 6));
        } else if (stage == ST_FS && sr == 0x13) /* SR_THREAD_KILL: all ones in a lane KIL demoted */
            value = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, shader_lane_dead(c),
                                     shader_u32(c, 0xffffffffu), c->zero);
        else if (sr == 0x11) /* SR_INVOCATION_ID */
            value = shader_input(c, c->u32_type, invocation_id);
        else if (sr >= 0x50 && sr <= 0x53) {
            u32 scope = shader_u32(c, sr < 0x52 ? 3 : 1);
            u32 clock = spv_emit_unary(&c->emit, SPV_OP_READ_CLOCK_KHR, vec2_u32, scope);
            value = spv_emit_vec4_extract(&c->emit, c->u32_type, clock, sr & 1);
        } else if (sr == 0x1d && between) {
            /* SR_INVOCATION_INFO: the vertices of the input primitive in bits 16-23, with the primitive's index
               in the batch (bits 0-7) 0, which makes the index the program builds for ISBERD the vertex's */
            /* Citron, acfb1dcc1ad9adf25b960c4f8f25c5bf78849406:
               geometry's input topology is known when translating. Its
               invocation info is a constant; tessellation remains dynamic. */
            if (stage == ST_GS)
                value = shader_u32(c, in_vertices << 16);
            else
                value = spv_emit_binary(&c->emit, SPV_OP_SHIFT_LEFT_LOGICAL, c->u32_type,
                                        spv_emit_load(&c->emit, c->u32_type, patch_vertices), shader_u32(c, 16));
        } else
            /* Direct CBE writes are disabled: their enable and address registers (0x15..0x17) are zero. */
            value = sr == 0x12                     ? shader_u32(c, 0x3f800000u)
                    : sr == 0x1d && stage != ST_FS ? shader_u32(c, 0x00ff0000u)
                                                   : c->zero;
        reg_write(c, d, value);
    } else if (op == O_VOTE) {
        u32 value = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        u32 one = shader_u32(c, 1);
        /* the lane model's warp is lane 0 alone (spirv_translate.c's lone_lane) */
        u32 lane = lane_model ? c->zero : shader_input(c, c->u32_type, c->subgroup_lane_input);
        u32 mask = lane_model ? spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, value, one, c->zero)
                              : shader_subgroup_ballot_word(c, vec4_u32, lane, value);
        u32 mode = B(w, 48, 2);
        if (mode < 3) {
            u32 any = spv_emit_binary(&c->emit, SPV_OP_I_NOT_EQUAL, c->bool_type, mask, c->zero);
            u32 result = any;
            if (mode != 1) {
                u32 active = lane_model ? one : shader_subgroup_ballot_word(c, vec4_u32, lane, c->true_value);
                u32 all = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, mask, active);
                result = mode == 0
                             ? all
                             : spv_emit_bool_binary(&c->emit, c->bool_type, 1, all,
                                                    spv_emit_unary(&c->emit, SPV_OP_LOGICAL_NOT, c->bool_type, any));
            }
            pred_write(c, B(w, 45, 3), result);
        }
        reg_write(c, d, mask);
    } else if (op == O_SHFL && c->quad && shader_shfl_quad(w, NULL)) {
        u32 operand;
        int swap = shader_shfl_quad(w, &operand) == 2;
        u32 value = spv_emit_subgroup_quad_u32(&c->emit, c->u32_type, reg_read(c, a), swap, shader_u32(c, operand));
        pred_write(c, B(w, 48, 3), c->true_value);
        reg_write(c, d, value);
    } else if (op == O_SHFL) {
        u32 index = B(w, 28, 1) ? shader_u32(c, B(w, 20, 5)) : reg_read(c, b);
        u32 mask = B(w, 29, 1) ? shader_u32(c, B(w, 34, 13)) : reg_read(c, B(w, 39, 8));
        u32 lane_full = lane_model ? c->zero : shader_input(c, c->u32_type, c->subgroup_lane_input);
        u32 lane = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, lane_full, shader_u32(c, 31));
        u32 seg =
            spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type,
                            spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, mask, shader_u32(c, 8)),
                            shader_u32(c, 31));
        u32 min_lane = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, lane, seg);
        u32 max_lane =
            spv_emit_binary(&c->emit, SPV_OP_BITWISE_OR, c->u32_type, min_lane,
                            spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, mask,
                                            spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type,
                                                            spv_emit_unary(&c->emit, SPV_OP_NOT, c->u32_type, seg),
                                                            shader_u32(c, 31))));
        u32 mode = B(w, 30, 2), source_lane, valid;
        if (mode == 0) {
            source_lane = spv_emit_binary(&c->emit, SPV_OP_BITWISE_OR, c->u32_type, min_lane,
                                          spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, index,
                                                          spv_emit_unary(&c->emit, SPV_OP_NOT, c->u32_type, seg)));
        } else if (mode == 1) {
            source_lane = spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, lane, index);
        } else if (mode == 2) {
            source_lane = spv_emit_iadd_u32(&c->emit, c->u32_type, lane, index);
        } else {
            source_lane = spv_emit_binary(&c->emit, SPV_OP_BITWISE_XOR, c->u32_type, lane, index);
        }
        valid =
            mode == 1
                ? spv_emit_unary(&c->emit, SPV_OP_LOGICAL_NOT, c->bool_type,
                                 spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type,
                                                 spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, lane, min_lane),
                                                 index))
                : spv_emit_unary(&c->emit, SPV_OP_LOGICAL_NOT, c->bool_type,
                                 spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, max_lane, source_lane));
        u32 value = reg_read(c, a);
        pred_write(c, B(w, 48, 3), valid);
        if (lane_model) { /* lane 0 alone: its own value, from its lane or one that runs nothing */
            reg_write(c, d, value);
            return;
        }
        u32 host_lane =
            spv_emit_binary(&c->emit, SPV_OP_BITWISE_OR, c->u32_type,
                            spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, lane_full, shader_u32(c, ~31u)),
                            spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, source_lane, shader_u32(c, 31)));
        u32 shuffled = spv_emit_subgroup_shuffle_u32(&c->emit, c->u32_type, value, host_lane);
        reg_write(c, d, spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, valid, shuffled, value));
    } else if (op == O_BAR) {
        if (stage == ST_TCS)
            spv_emit_patch_barrier(&c->emit, c->u32_type);
        else
            spv_emit_control_barrier(&c->emit, c->u32_type);
    } else if (op == O_CS2R) {
        u32 sr = B(w, 20, 8);
        u32 clock = sr == 0xff
                        ? 0
                        : spv_emit_unary(&c->emit, SPV_OP_READ_CLOCK_KHR, vec2_u32, shader_u32(c, sr == 0x50 ? 3 : 1));
        reg_write(c, d, clock ? spv_emit_vec4_extract(&c->emit, c->u32_type, clock, 0) : c->zero);
        if (d < 254) reg_write(c, d + 1, clock ? spv_emit_vec4_extract(&c->emit, c->u32_type, clock, 1) : c->zero);
    } else if (op == O_MEMBAR) {
        /* Only compute has workgroup memory. Graphics memory barriers use Device scope even for MEMBAR.CTA;
           Vulkan does not permit Workgroup scope in those stages. */
        u32 scope = stage == ST_CS && !B(w, 8, 2) ? 2 : 1;
        spv_emit_memory_barrier(&c->emit, c->u32_type, scope, 0x848u | (stage == ST_CS ? 0x100u : 0));
    }
}
