/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/depth_stencil_spirv.h"

typedef struct { SpvEmitter* e; uint32_t f, u, b, zero; } DepthEmit;
static uint32_t integer(DepthEmit* c, uint32_t value) {
    return spv_emit_constant_u32(c->e, c->u, value);
}
static uint32_t field(DepthEmit* c, uint32_t value, uint32_t shift, uint32_t mask) {
    if (shift) value = SPV(c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u, value, integer(c, shift));
    return SPV(c->e, SPV_OP_BITWISE_AND, c->u, value, integer(c, mask));
}
static uint32_t both(DepthEmit* c, uint32_t a, uint32_t b) {
    return SPV(c->e, SPV_OP_LOGICAL_AND, c->b, a, b);
}
static uint32_t enabled(DepthEmit* c, uint32_t flags, uint32_t bit) {
    return spv_emit_binary(c->e, SPV_OP_I_NOT_EQUAL, c->b, field(c, flags, 0, bit), c->zero);
}
static uint32_t choose(DepthEmit* c, uint32_t type, uint32_t predicate, uint32_t yes, uint32_t no) {
    return spv_emit_ternary(c->e, SPV_OP_SELECT, type, predicate, yes, no);
}
static uint32_t compare(DepthEmit* c, uint32_t operation, uint32_t reference, uint32_t stored, int floating) {
    static const uint32_t float_ops[6] = {SPV_OP_F_ORD_LESS_THAN, SPV_OP_F_ORD_EQUAL,
        SPV_OP_F_ORD_LESS_THAN_EQUAL, SPV_OP_F_ORD_GREATER_THAN, SPV_OP_F_UNORD_NOT_EQUAL,
        SPV_OP_F_ORD_GREATER_THAN_EQUAL};
    static const uint32_t uint_ops[6] = {SPV_OP_U_LESS_THAN, SPV_OP_I_EQUAL, SPV_OP_U_LESS_THAN_EQUAL,
        SPV_OP_U_GREATER_THAN, SPV_OP_I_NOT_EQUAL, SPV_OP_U_GREATER_THAN_EQUAL};
    uint32_t result = spv_emit_constant_bool(c->e, c->b, 0);
    for (uint32_t i = 0; i < 6; i++) {
        uint32_t match = spv_emit_binary(c->e, SPV_OP_I_EQUAL, c->b, operation, integer(c, i + 1));
        uint32_t value = SPV(c->e, floating ? float_ops[i] : uint_ops[i], c->b, reference, stored);
        result = choose(c, c->b, match, value, result);
    }
    return SPV(c->e, SPV_OP_LOGICAL_OR, c->b, result,
               spv_emit_binary(c->e, SPV_OP_I_EQUAL, c->b, operation, integer(c, 7)));
}
static uint32_t stencil_operation(DepthEmit* c, uint32_t operation, uint32_t old, uint32_t reference) {
    uint32_t increment = SPV(c->e, SPV_OP_I_ADD, c->u, old, integer(c, 1));
    uint32_t decrement = SPV(c->e, SPV_OP_I_SUB, c->u, old, integer(c, 1));
    uint32_t values[8] = {
        old,
        c->zero,
        reference,
        choose(c, c->u, spv_emit_binary(c->e, SPV_OP_I_EQUAL, c->b, old, integer(c, 255)), old, increment),
        choose(c, c->u, spv_emit_binary(c->e, SPV_OP_I_EQUAL, c->b, old, c->zero), old, decrement),
        SPV(c->e, SPV_OP_BITWISE_XOR, c->u, old, integer(c, 255)),
        field(c, increment, 0, 255),
        field(c, decrement, 0, 255)};
    uint32_t result = old;
    for (uint32_t i = 1; i < 8; i++)
        result =
            choose(c, c->u, spv_emit_binary(c->e, SPV_OP_I_EQUAL, c->b, operation, integer(c, i)), values[i], result);
    return result;
}
uint64_t shader_depth_stencil_layout(uint32_t width, uint32_t height, int stencil,
                                      uint32_t* depth_offset, uint32_t* stencil_offset) {
    if (depth_offset) *depth_offset = 0;
    if (stencil_offset) *stencil_offset = 0;
    uint64_t pixels = (uint64_t)width * height;
    if (!width || !height || pixels > UINT32_MAX - SH_DS_HEADER_WORDS) return 0;
    uint64_t words = SH_DS_HEADER_WORDS + pixels + (stencil ? (pixels + 3) / 4 : 0);
    if (words > UINT32_MAX) return 0;
    if (depth_offset) *depth_offset = SH_DS_HEADER_WORDS;
    if (stencil_offset && stencil) *stencil_offset = SH_DS_HEADER_WORDS + (uint32_t)pixels;
    return words * 4;
}
ShaderDepthStencilResult shader_depth_stencil_evaluate(SpvEmitter* e, uint32_t f32, uint32_t u32_type,
                                                        uint32_t boolean, const uint32_t header[SH_DS_HEADER_WORDS],
                                                        uint32_t incoming_depth, uint32_t old_depth,
                                                        uint32_t old_stencil, uint32_t front, uint32_t covered) {
    DepthEmit context = {e, f32, u32_type, boolean, spv_emit_constant_u32(e, u32_type, 0)};
    DepthEmit* c = &context;
    uint32_t flags = header[SH_DS_FLAGS], old_float = spv_emit_unary(e, SPV_OP_BITCAST, f32, old_depth);
    uint32_t control = choose(c, c->u, front, header[SH_DS_FRONT_CONTROL], header[SH_DS_BACK_CONTROL]);
    uint32_t values = choose(c, c->u, front, header[SH_DS_FRONT_VALUES], header[SH_DS_BACK_VALUES]);
    uint32_t reference = field(c, values, 0, 255), compare_mask = field(c, values, 8, 255);
    uint32_t write_mask = field(c, values, 16, 255);
    uint32_t clamp_min = spv_emit_unary(e, SPV_OP_BITCAST, f32, header[SH_DS_CLAMP_MIN]);
    uint32_t clamp_max = spv_emit_unary(e, SPV_OP_BITCAST, f32, header[SH_DS_CLAMP_MAX]);
    uint32_t clamped = choose(c, f32, SPV(e, SPV_OP_F_ORD_LESS_THAN, boolean, incoming_depth, clamp_min),
                                clamp_min, incoming_depth);
    clamped = choose(c, f32, SPV(e, SPV_OP_F_ORD_GREATER_THAN, boolean, clamped, clamp_max), clamp_max, clamped);
    incoming_depth = choose(c, f32, enabled(c, flags, SH_DS_DEPTH_CLAMP), clamped, incoming_depth);
    uint32_t in_bounds = both(c,
                              SPV(e, SPV_OP_F_ORD_GREATER_THAN_EQUAL, boolean, old_float,
                                  spv_emit_unary(e, SPV_OP_BITCAST, f32, header[SH_DS_BOUNDS_MIN])),
                              SPV(e, SPV_OP_F_ORD_LESS_THAN_EQUAL, boolean, old_float,
                                  spv_emit_unary(e, SPV_OP_BITCAST, f32, header[SH_DS_BOUNDS_MAX])));
    uint32_t bound_pass =
        SPV(e, SPV_OP_LOGICAL_OR, boolean,
            spv_emit_unary(e, SPV_OP_LOGICAL_NOT, boolean, enabled(c, flags, SH_DS_DEPTH_BOUNDS)), in_bounds);
    uint32_t stencil_test = enabled(c, flags, SH_DS_STENCIL_TEST);
    uint32_t stencil_pass =
        SPV(e, SPV_OP_LOGICAL_OR, boolean, spv_emit_unary(e, SPV_OP_LOGICAL_NOT, boolean, stencil_test),
            compare(c, field(c, control, 0, 7), SPV(e, SPV_OP_BITWISE_AND, c->u, reference, compare_mask),
                    SPV(e, SPV_OP_BITWISE_AND, c->u, old_stencil, compare_mask), 0));
    uint32_t depth_test = enabled(c, flags, SH_DS_DEPTH_TEST);
    uint32_t depth_pass = SPV(e, SPV_OP_LOGICAL_OR, boolean, spv_emit_unary(e, SPV_OP_LOGICAL_NOT, boolean, depth_test),
                              compare(c, field(c, flags, SH_DS_COMPARE_SHIFT, 7), incoming_depth, old_float, 1));
    uint32_t active = both(
        c, both(c, covered, spv_emit_unary(e, SPV_OP_LOGICAL_NOT, boolean, enabled(c, flags, SH_DS_FIXED_MASK_OFF))),
        bound_pass);
    uint32_t pass = both(c, active, both(c, stencil_pass, depth_pass));
    uint32_t write_depth = both(c, pass, both(c, depth_test, enabled(c, flags, SH_DS_DEPTH_WRITE)));
    uint32_t operation = choose(c, c->u, stencil_pass,
        choose(c, c->u, depth_pass, field(c, control, 6, 7), field(c, control, 9, 7)), field(c, control, 3, 7));
    uint32_t replacement = stencil_operation(c, operation, old_stencil, reference);
    uint32_t difference = SPV(e, SPV_OP_BITWISE_AND, c->u,
        SPV(e, SPV_OP_BITWISE_XOR, c->u, replacement, old_stencil), write_mask);
    uint32_t stencil = SPV(e, SPV_OP_BITWISE_XOR, c->u, old_stencil, difference);
    /* Outside the viewport range, incoming native depth is undefined unless it is clamped. Never export an
       invalid buffer-to-depth-image value: choose0 only for those undefined out-of-[0,1]/NaN write results. */
    uint32_t valid_depth = both(c,
        SPV(e, SPV_OP_F_ORD_GREATER_THAN_EQUAL, boolean, incoming_depth, spv_emit_constant_f32(e, f32, 0)),
        SPV(e, SPV_OP_F_ORD_LESS_THAN_EQUAL, boolean, incoming_depth, spv_emit_constant_f32(e, f32, 1)));
    uint32_t depth_bits =
        choose(c, c->u, valid_depth, spv_emit_unary(e, SPV_OP_BITCAST, c->u, incoming_depth), c->zero);
    return (ShaderDepthStencilResult){pass,
        choose(c, c->u, write_depth, depth_bits, old_depth),
        choose(c, c->u, both(c, active, stencil_test), stencil, old_stencil)};
}
static uint32_t word_pointer(SpvEmitter* e, uint32_t u, uint32_t buffer, uint32_t index) {
    return SPV(e, SPV_OP_ACCESS_CHAIN, spv_emit_type_storage_pointer(e, u), buffer,
                spv_emit_constant_u32(e, u, 0), index);
}
static uint32_t load_header(SpvEmitter* e, uint32_t u, uint32_t buffer, uint32_t word) {
    return spv_emit_load(e, u, word_pointer(e, u, buffer, spv_emit_constant_u32(e, u, word)));
}
uint32_t shader_depth_stencil_declare(SpvEmitter* e, uint32_t u32_type, uint32_t set, uint32_t binding) {
    uint32_t array = spv_emit_type_runtime_array(e, u32_type, 4);
    uint32_t buffer = spv_emit_storage_buffer(e, spv_emit_type_struct_one(e, array), set, binding);
    spv_emit_decorate(e, buffer, 23, NULL, 0); /* Coherent: interlock memory dependency includes this buffer. */
    return buffer;
}
uint32_t shader_depth_stencil_coordinates(SpvEmitter* e, uint32_t f32, uint32_t u32_type, uint32_t boolean,
                                           uint32_t buffer, uint32_t position, uint32_t* inside) {
    uint32_t width = load_header(e, u32_type, buffer, SH_DS_WIDTH);
    uint32_t height = load_header(e, u32_type, buffer, SH_DS_HEIGHT);
    uint32_t x = spv_emit_vec4_extract(e, f32, position, 0), y = spv_emit_vec4_extract(e, f32, position, 1);
    uint32_t zero = spv_emit_constant_f32(e, f32, 0);
    uint32_t valid_x =
        SPV(e, SPV_OP_LOGICAL_AND, boolean, SPV(e, SPV_OP_F_ORD_GREATER_THAN_EQUAL, boolean, x, zero),
            SPV(e, SPV_OP_F_ORD_LESS_THAN, boolean, x, spv_emit_unary(e, SPV_OP_CONVERT_U_TO_F, f32, width)));
    uint32_t valid_y =
        SPV(e, SPV_OP_LOGICAL_AND, boolean, SPV(e, SPV_OP_F_ORD_GREATER_THAN_EQUAL, boolean, y, zero),
            SPV(e, SPV_OP_F_ORD_LESS_THAN, boolean, y, spv_emit_unary(e, SPV_OP_CONVERT_U_TO_F, f32, height)));
    uint32_t valid = SPV(e, SPV_OP_LOGICAL_AND, boolean, valid_x, valid_y);
    if (inside) *inside = valid;
    x = spv_emit_unary(e, SPV_OP_CONVERT_F_TO_U, u32_type, spv_emit_ternary(e, SPV_OP_SELECT, f32, valid, x, zero));
    y = spv_emit_unary(e, SPV_OP_CONVERT_F_TO_U, u32_type, spv_emit_ternary(e, SPV_OP_SELECT, f32, valid, y, zero));
    return SPV(e, SPV_OP_I_ADD, u32_type, SPV(e, SPV_OP_I_MUL, u32_type, y, width), x);
}
uint32_t shader_depth_stencil_test(SpvEmitter* e, uint32_t f32, uint32_t u32_type, uint32_t boolean,
                                   uint32_t buffer, uint32_t index, uint32_t incoming_depth,
                                   uint32_t front, uint32_t covered) {
    uint32_t header[SH_DS_HEADER_WORDS];
    for (uint32_t i = 0; i < SH_DS_HEADER_WORDS; i++) header[i] = load_header(e, u32_type, buffer, i);
    uint32_t zero = spv_emit_constant_u32(e, u32_type, 0);
    uint32_t mask_off = SPV(e, SPV_OP_BITWISE_AND, u32_type, header[SH_DS_FLAGS],
        spv_emit_constant_u32(e, u32_type, SH_DS_FIXED_MASK_OFF));
    uint32_t active =
        SPV(e, SPV_OP_LOGICAL_AND, boolean, covered, spv_emit_binary(e, SPV_OP_I_EQUAL, boolean, mask_off, zero));
    uint32_t active_label = spv_emit_id(e), inactive_label = spv_emit_id(e), merge = spv_emit_id(e);
    spv_emit_selection_merge(e, merge);
    spv_emit_branch_conditional(e, active, active_label, inactive_label);
    spv_emit_label(e, inactive_label);
    spv_emit_branch(e, merge);
    spv_emit_label(e, active_label);
    /* An inactive/helper lane can have index0 without sharing pixel0's interlock. Do not even read a mutable
       plane from that lane: a select after the load would still race against a real pixel0 depth write. */
    uint32_t depth_pointer = word_pointer(e, u32_type, buffer,
        SPV(e, SPV_OP_I_ADD, u32_type, header[SH_DS_DEPTH_OFFSET], index));
    uint32_t depth = spv_emit_load(e, u32_type, depth_pointer);
    uint32_t has_stencil = spv_emit_binary(e, SPV_OP_I_NOT_EQUAL, boolean, header[SH_DS_STENCIL_OFFSET], zero);
    uint32_t stencil_index = SPV(e, SPV_OP_I_ADD, u32_type, header[SH_DS_STENCIL_OFFSET],
        SPV(e, SPV_OP_SHIFT_RIGHT_LOGICAL, u32_type, index, spv_emit_constant_u32(e, u32_type, 2)));
    /* No stencil uses a read-only header word, never a concurrently non-atomic depth word. */
    stencil_index = spv_emit_ternary(e, SPV_OP_SELECT, u32_type, has_stencil, stencil_index, zero);
    uint32_t stencil_pointer = word_pointer(e, u32_type, buffer, stencil_index);
    uint32_t shift = SPV(e, SPV_OP_I_MUL, u32_type,
        SPV(e, SPV_OP_BITWISE_AND, u32_type, index, spv_emit_constant_u32(e, u32_type, 3)),
        spv_emit_constant_u32(e, u32_type, 8));
    uint32_t packed = spv_emit_atomic_load_u32(e, u32_type, stencil_pointer);
    uint32_t stencil = SPV(e, SPV_OP_BITWISE_AND, u32_type,
        SPV(e, SPV_OP_SHIFT_RIGHT_LOGICAL, u32_type, packed, shift), spv_emit_constant_u32(e, u32_type, 255));
    ShaderDepthStencilResult result = shader_depth_stencil_evaluate(e, f32, u32_type, boolean, header,
                                                                      incoming_depth, depth, stencil, front, covered);
    uint32_t write_depth = spv_emit_id(e), depth_done = spv_emit_id(e);
    uint32_t changed = spv_emit_binary(e, SPV_OP_I_NOT_EQUAL, boolean, result.depth, depth);
    spv_emit_selection_merge(e, depth_done);
    spv_emit_branch_conditional(e, changed, write_depth, depth_done);
    spv_emit_label(e, write_depth);
    spv_emit_store(e, depth_pointer, result.depth);
    spv_emit_branch(e, depth_done);
    spv_emit_label(e, depth_done);
    uint32_t write_stencil = spv_emit_id(e), stencil_done = spv_emit_id(e);
    changed = SPV(e, SPV_OP_LOGICAL_AND, boolean, has_stencil,
                  spv_emit_binary(e, SPV_OP_I_NOT_EQUAL, boolean, result.stencil, stencil));
    spv_emit_selection_merge(e, stencil_done);
    spv_emit_branch_conditional(e, changed, write_stencil, stencil_done);
    spv_emit_label(e, write_stencil);
    uint32_t delta = SPV(e, SPV_OP_SHIFT_LEFT_LOGICAL, u32_type,
        SPV(e, SPV_OP_BITWISE_XOR, u32_type, result.stencil, stencil), shift);
    spv_emit_atomic_u32(e, u32_type, SPV_OP_ATOMIC_XOR, stencil_pointer, delta);
    spv_emit_branch(e, stencil_done);
    spv_emit_label(e, stencil_done);
    uint32_t active_exit = spv_emit_id(e);
    spv_emit_branch(e, active_exit);
    spv_emit_label(e, active_exit);
    spv_emit_branch(e, merge);
    spv_emit_label(e, merge);
    return SPV(e, SPV_OP_PHI, boolean, result.pass, active_exit,
        spv_emit_constant_bool(e, boolean, 0), inactive_label);
}
