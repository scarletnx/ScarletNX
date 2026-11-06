/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/capture_tiles_spirv.h"

static uint32_t tile_word(SpvEmitter* e, uint32_t u32_type, uint32_t buffer, uint32_t index) {
    uint32_t pointer = spv_emit_type_storage_pointer(e, u32_type);
    return SPV(e, SPV_OP_ACCESS_CHAIN, pointer, buffer, spv_emit_constant_u32(e, u32_type, 0), index);
}
static uint32_t tile_header(SpvEmitter* e, uint32_t u32_type, uint32_t buffer, uint32_t word) {
    return spv_emit_load(e, u32_type, tile_word(e, u32_type, buffer, spv_emit_constant_u32(e, u32_type, word)));
}
uint32_t shader_capture_tiles_declare(SpvEmitter* e, uint32_t u32_type, uint32_t set, uint32_t binding) {
    uint32_t array = spv_emit_type_runtime_array(e, u32_type, 4);
    uint32_t block = spv_emit_type_struct_one(e, array);
    uint32_t buffer = spv_emit_storage_buffer(e, block, set, binding);
    spv_emit_decorate(e, buffer, 23, NULL, 0); /* Coherent */
    return buffer;
}
void shader_capture_tile_mark(SpvEmitter* e, uint32_t f32, uint32_t u32_type, uint32_t boolean, uint32_t buffer,
                              uint32_t position, uint32_t layer, uint32_t live) {
    uint32_t tiles_x = tile_header(e, u32_type, buffer, 3), tiles_y = tile_header(e, u32_type, buffer, 4);
    uint32_t width = tile_header(e, u32_type, buffer, 5), height = tile_header(e, u32_type, buffer, 6);
    uint32_t layers = tile_header(e, u32_type, buffer, 7);
    uint32_t x = spv_emit_vec4_extract(e, f32, position, 0), y = spv_emit_vec4_extract(e, f32, position, 1);
    uint32_t zero = spv_emit_constant_u32(e, u32_type, 0), zero_f = spv_emit_constant_f32(e, f32, 0);
    uint32_t inside = SPV(e, SPV_OP_LOGICAL_AND, boolean, live, SPV(e, SPV_OP_U_LESS_THAN, boolean, layer, layers));
    inside = SPV(e, SPV_OP_LOGICAL_AND, boolean, inside, SPV(e, SPV_OP_F_ORD_GREATER_THAN_EQUAL, boolean, x, zero_f));
    inside = SPV(e, SPV_OP_LOGICAL_AND, boolean, inside, SPV(e, SPV_OP_F_ORD_GREATER_THAN_EQUAL, boolean, y, zero_f));
    inside = SPV(e, SPV_OP_LOGICAL_AND, boolean, inside,
                 SPV(e, SPV_OP_F_ORD_LESS_THAN, boolean, x, spv_emit_convert_u_to_f(e, f32, width)));
    inside = SPV(e, SPV_OP_LOGICAL_AND, boolean, inside,
                 SPV(e, SPV_OP_F_ORD_LESS_THAN, boolean, y, spv_emit_convert_u_to_f(e, f32, height)));
    uint32_t mark = spv_emit_id(e), done = spv_emit_id(e);
    spv_emit_selection_merge(e, done);
    spv_emit_branch_conditional(e, inside, mark, done);
    spv_emit_label(e, mark);
    uint32_t shift = spv_emit_constant_u32(e, u32_type, 3);
    uint32_t tx = SPV(e, SPV_OP_SHIFT_RIGHT_LOGICAL, u32_type, spv_emit_convert_f_to_u(e, u32_type, x), shift);
    uint32_t ty = SPV(e, SPV_OP_SHIFT_RIGHT_LOGICAL, u32_type, spv_emit_convert_f_to_u(e, u32_type, y), shift);
    uint32_t row = SPV(e, SPV_OP_I_ADD, u32_type, SPV(e, SPV_OP_I_MUL, u32_type, layer, tiles_y), ty);
    uint32_t tile = SPV(e, SPV_OP_I_ADD, u32_type, SPV(e, SPV_OP_I_MUL, u32_type, row, tiles_x), tx);
    uint32_t header = spv_emit_constant_u32(e, u32_type, SH_CAPTURE_TILE_HEADER);
    uint32_t flag = tile_word(e, u32_type, buffer, SPV(e, SPV_OP_I_ADD, u32_type, header, tile));
    uint32_t prior = spv_emit_atomic_u32(e, u32_type, SPV_OP_ATOMIC_OR, flag,
                                         spv_emit_constant_u32(e, u32_type, 1));
    uint32_t first = spv_emit_i_equal(e, boolean, prior, zero);
    uint32_t append = spv_emit_id(e), next = spv_emit_id(e);
    spv_emit_selection_merge(e, next);
    spv_emit_branch_conditional(e, first, append, next);
    spv_emit_label(e, append);
    uint32_t slot = spv_emit_atomic_u32(e, u32_type, SPV_OP_ATOMIC_I_ADD, tile_word(e, u32_type, buffer, zero),
                                        spv_emit_constant_u32(e, u32_type, 1));
    uint32_t count = SPV(e, SPV_OP_I_MUL, u32_type, SPV(e, SPV_OP_I_MUL, u32_type, tiles_x, tiles_y), layers);
    uint32_t index = SPV(e, SPV_OP_I_ADD, u32_type, SPV(e, SPV_OP_I_ADD, u32_type, header, count), slot);
    spv_emit_store(e, tile_word(e, u32_type, buffer, index), tile);
    spv_emit_branch(e, next);
    spv_emit_label(e, next);
    spv_emit_branch(e, done);
    spv_emit_label(e, done);
}
uint32_t shader_capture_tile_coordinates(SpvEmitter* e, uint32_t u32_type, uint32_t buffer,
                                         uint32_t workgroup_xyz, uint32_t local_xyz, int adaptive) {
    uint32_t tiles_x = tile_header(e, u32_type, buffer, 3), tiles_y = tile_header(e, u32_type, buffer, 4);
    uint32_t layers = tile_header(e, u32_type, buffer, 7);
    uint32_t count = SPV(e, SPV_OP_I_MUL, u32_type, SPV(e, SPV_OP_I_MUL, u32_type, tiles_x, tiles_y), layers);
    uint32_t header = spv_emit_constant_u32(e, u32_type, SH_CAPTURE_TILE_HEADER);
    uint32_t workgroup_x = spv_emit_vec4_extract(e, u32_type, workgroup_xyz, 0);
    uint32_t full = 0, listed = 0, merge = 0, full_coordinates = 0, full_tile = 0;
    uint32_t vector = spv_emit_type_vec3(e, u32_type);
    if (adaptive) {
        full = spv_emit_id(e); listed = spv_emit_id(e); merge = spv_emit_id(e);
        uint32_t mode = tile_header(e, u32_type, buffer, 11);
        uint32_t dense = spv_emit_i_not_equal(e, spv_emit_type_bool(e), mode,
                                              spv_emit_constant_u32(e, u32_type, 0));
        spv_emit_selection_merge(e, merge);
        spv_emit_branch_conditional(e, dense, full, listed);
        spv_emit_label(e, full);
        uint32_t row = SPV(e, SPV_OP_I_ADD, u32_type,
            SPV(e, SPV_OP_I_MUL, u32_type, spv_emit_vec4_extract(e, u32_type, workgroup_xyz, 2), tiles_y),
            spv_emit_vec4_extract(e, u32_type, workgroup_xyz, 1));
        full_tile = SPV(e, SPV_OP_I_ADD, u32_type, SPV(e, SPV_OP_I_MUL, u32_type, row, tiles_x), workgroup_x);
        full_coordinates = workgroup_xyz;
        spv_emit_branch(e, merge);
        spv_emit_label(e, listed);
    }
    uint32_t index = SPV(e, SPV_OP_I_ADD, u32_type, SPV(e, SPV_OP_I_ADD, u32_type, header, count), workgroup_x);
    uint32_t tile = spv_emit_load(e, u32_type, tile_word(e, u32_type, buffer, index));
    uint32_t row = SPV(e, SPV_OP_U_DIV, u32_type, tile, tiles_x);
    uint32_t tx = SPV(e, SPV_OP_U_MOD, u32_type, tile, tiles_x);
    uint32_t ty = SPV(e, SPV_OP_U_MOD, u32_type, row, tiles_y);
    uint32_t layer = SPV(e, SPV_OP_U_DIV, u32_type, row, tiles_y);
    if (adaptive) {
        uint32_t list_coordinates = spv_emit_vec3_construct(e, vector, tx, ty, layer);
        spv_emit_branch(e, merge);
        spv_emit_label(e, merge);
        tile = SPV(e, SPV_OP_PHI, u32_type, full_tile, full, tile, listed);
        uint32_t coordinates = SPV(e, SPV_OP_PHI, vector, full_coordinates, full, list_coordinates, listed);
        tx = spv_emit_vec4_extract(e, u32_type, coordinates, 0);
        ty = spv_emit_vec4_extract(e, u32_type, coordinates, 1);
        layer = spv_emit_vec4_extract(e, u32_type, coordinates, 2);
    }
    uint32_t x = spv_emit_vec4_extract(e, u32_type, local_xyz, 0);
    uint32_t y = spv_emit_vec4_extract(e, u32_type, local_xyz, 1);
    uint32_t z = spv_emit_vec4_extract(e, u32_type, local_xyz, 2);
    uint32_t local = SPV(e, SPV_OP_BITWISE_OR, u32_type, SPV(e, SPV_OP_BITWISE_OR, u32_type, x, y), z);
    uint32_t first = spv_emit_i_equal(e, spv_emit_type_bool(e), local, spv_emit_constant_u32(e, u32_type, 0));
    uint32_t reset = spv_emit_id(e), done = spv_emit_id(e);
    spv_emit_selection_merge(e, done);
    spv_emit_branch_conditional(e, first, reset, done);
    spv_emit_label(e, reset);
    spv_emit_store(e, tile_word(e, u32_type, buffer, SPV(e, SPV_OP_I_ADD, u32_type, header, tile)),
                   spv_emit_constant_u32(e, u32_type, 0));
    spv_emit_branch(e, done);
    spv_emit_label(e, done);
    uint32_t shift = spv_emit_constant_u32(e, u32_type, 3);
    x = SPV(e, SPV_OP_I_ADD, u32_type, SPV(e, SPV_OP_SHIFT_LEFT_LOGICAL, u32_type, tx, shift), x);
    y = SPV(e, SPV_OP_I_ADD, u32_type, SPV(e, SPV_OP_SHIFT_LEFT_LOGICAL, u32_type, ty, shift), y);
    return spv_emit_vec3_construct(e, spv_emit_type_vec3(e, u32_type), x, y, layer);
}

uint32_t* shader_capture_tiles_finalize_spirv(uint32_t binding, size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 2048);
    spv_emit_shader_capability(&e);
    uint32_t u = spv_emit_type_u32(&e), b = spv_emit_type_bool(&e);
    uint32_t buffer = shader_capture_tiles_declare(&e, u, 0, binding);
    uint32_t function = spv_emit_function(&e, 0, spv_emit_type_void(&e), NULL, 0, NULL);
    uint32_t active = tile_header(&e, u, buffer, 0), x = tile_header(&e, u, buffer, 3);
    uint32_t y = tile_header(&e, u, buffer, 4), layers = tile_header(&e, u, buffer, 7);
    uint32_t total = SPV(&e, SPV_OP_I_MUL, u, SPV(&e, SPV_OP_I_MUL, u, x, y), layers);
    uint32_t full = spv_emit_i_equal(&e, b, active, total);
    uint32_t one = spv_emit_constant_u32(&e, u, 1), zero = spv_emit_constant_u32(&e, u, 0);
    uint32_t values[4] = {spv_emit_select(&e, u, full, x, active), spv_emit_select(&e, u, full, y, one),
                          spv_emit_select(&e, u, full, layers, one), spv_emit_select(&e, u, full, one, zero)};
    for (uint32_t i = 0; i < 4; i++)
        spv_emit_store(&e, tile_word(&e, u, buffer, spv_emit_constant_u32(&e, u, 8 + i)), values[i]);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    spv_emit_compute_entry_str(&e, function, "main", &buffer, 1);
    spv_emit_local_size(&e, function, 1, 1, 1);
    uint32_t* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}
