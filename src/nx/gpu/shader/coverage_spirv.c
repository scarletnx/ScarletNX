/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/coverage_spirv.h"
#include "nx/gpu/shader/advanced_blend_spirv.h"
#include "nx/gpu/shader/capture_tiles_spirv.h"

static uint32_t alpha_coverage(SpvEmitter* e, uint32_t f32, uint32_t u32_type, uint32_t boolean,
                               uint32_t alpha, uint32_t samples, uint32_t guest_mask, uint32_t x, uint32_t y) {
    if (!samples || samples > 16 || (samples & (samples - 1)) || (!!x != !!y)) return 0;
    uint32_t zero = spv_emit_constant_f32(e, f32, 0), one = spv_emit_constant_f32(e, f32, 1);
    uint32_t positive = SPV(e, SPV_OP_F_ORD_GREATER_THAN, boolean, alpha, zero);
    uint32_t nonnegative = spv_emit_ternary(e, SPV_OP_SELECT, f32, positive, alpha, zero);
    uint32_t below_one = SPV(e, SPV_OP_F_ORD_LESS_THAN, boolean, nonnegative, one);
    uint32_t clamped = spv_emit_ternary(e, SPV_OP_SELECT, f32, below_one, nonnegative, one);
    /* Clamping precedes conversion: NaNs and infinities never reach OpConvertFToU. Multiplication by a power of
       two places every k/N threshold exactly; the resulting count is0..N<=16, within a32-bit shift's range. */
    uint32_t scaled = spv_emit_binary(e, SPV_OP_F_MUL, f32, clamped, spv_emit_constant_f32(e, f32, (float)samples));
    uint32_t count = spv_emit_unary(e, SPV_OP_CONVERT_F_TO_U, u32_type, scaled);
    uint32_t one_bit = spv_emit_constant_u32(e, u32_type, 1);
    uint32_t rotation = 0;
    if (x) {
        uint32_t four = spv_emit_constant_f32(e, f32, 4);
        uint32_t px = spv_emit_unary(e, SPV_OP_CONVERT_F_TO_U, u32_type, SPV(e, SPV_OP_F_MOD, f32, x, four));
        uint32_t py = spv_emit_unary(e, SPV_OP_CONVERT_F_TO_U, u32_type, SPV(e, SPV_OP_F_MOD, f32, y, four));
        uint32_t two_bits = spv_emit_constant_u32(e, u32_type, 3);
        px = SPV(e, SPV_OP_BITWISE_AND, u32_type, px, two_bits);
        py = SPV(e, SPV_OP_BITWISE_AND, u32_type, py, two_bits);
        uint32_t xy = SPV(e, SPV_OP_BITWISE_XOR, u32_type, px, py);
        uint32_t two = spv_emit_constant_u32(e, u32_type, 2);
        uint32_t rank = SPV(e, SPV_OP_SHIFT_LEFT_LOGICAL, u32_type,
            SPV(e, SPV_OP_BITWISE_AND, u32_type, xy, one_bit), spv_emit_constant_u32(e, u32_type, 3));
        rank = SPV(e, SPV_OP_BITWISE_OR, u32_type, rank,
            SPV(e, SPV_OP_SHIFT_LEFT_LOGICAL, u32_type, SPV(e, SPV_OP_BITWISE_AND, u32_type, py, one_bit), two));
        rank = SPV(e, SPV_OP_BITWISE_OR, u32_type, rank, SPV(e, SPV_OP_BITWISE_AND, u32_type, xy, two));
        rank = SPV(e, SPV_OP_BITWISE_OR, u32_type, rank, SPV(e, SPV_OP_SHIFT_RIGHT_LOGICAL, u32_type, py, one_bit));
        uint32_t threshold =
            spv_emit_unary(e, SPV_OP_CONVERT_U_TO_F, f32, SPV(e, SPV_OP_I_ADD, u32_type, rank, one_bit));
        threshold = spv_emit_binary(e, SPV_OP_F_MUL, f32, threshold, spv_emit_constant_f32(e, f32, 1.0f / 16.0f));
        uint32_t fraction = SPV(e, SPV_OP_F_SUB, f32, scaled, spv_emit_unary(e, SPV_OP_CONVERT_U_TO_F, f32, count));
        uint32_t extra = SPV(e, SPV_OP_F_ORD_GREATER_THAN_EQUAL, boolean, fraction, threshold);
        count =
            SPV(e, SPV_OP_I_ADD, u32_type, count,
                spv_emit_ternary(e, SPV_OP_SELECT, u32_type, extra, one_bit, spv_emit_constant_u32(e, u32_type, 0)));
        if (samples > 1) {
            rotation = SPV(e, SPV_OP_I_ADD, u32_type, px, SPV(e, SPV_OP_SHIFT_LEFT_LOGICAL, u32_type, py, two));
            rotation = SPV(e, SPV_OP_BITWISE_AND, u32_type, rotation, spv_emit_constant_u32(e, u32_type, samples - 1));
        }
    }
    uint32_t bits = SPV(e, SPV_OP_SHIFT_LEFT_LOGICAL, u32_type, one_bit, count);
    uint32_t mask = SPV(e, SPV_OP_I_SUB, u32_type, bits, one_bit);
    if (rotation) {
        uint32_t shifted = SPV(e, SPV_OP_SHIFT_LEFT_LOGICAL, u32_type, mask, rotation);
        uint32_t wrapped = SPV(e, SPV_OP_SHIFT_RIGHT_LOGICAL, u32_type, mask,
            SPV(e, SPV_OP_I_SUB, u32_type, spv_emit_constant_u32(e, u32_type, samples), rotation));
        mask = SPV(e, SPV_OP_BITWISE_AND, u32_type, SPV(e, SPV_OP_BITWISE_OR, u32_type, shifted, wrapped),
            spv_emit_constant_u32(e, u32_type, (1u << samples) - 1));
    }
    return guest_mask ? SPV(e, SPV_OP_BITWISE_AND, u32_type, mask, guest_mask) : mask;
}
uint32_t shader_alpha_coverage_spirv(SpvEmitter* e, uint32_t f32, uint32_t u32_type, uint32_t boolean,
                                    uint32_t alpha, uint32_t samples, uint32_t guest_mask) {
    return alpha_coverage(e, f32, u32_type, boolean, alpha, samples, guest_mask, 0, 0);
}
uint32_t shader_alpha_coverage_dither_spirv(SpvEmitter* e, uint32_t f32, uint32_t u32_type, uint32_t boolean,
                                           uint32_t alpha, uint32_t samples, uint32_t guest_mask,
                                           uint32_t x, uint32_t y) {
    return x && y ? alpha_coverage(e, f32, u32_type, boolean, alpha, samples, guest_mask, x, y) : 0;
}

uint32_t* shader_coverage_resolve_spirv(uint32_t flags, int signed_target, size_t* words) {
    uint32_t samples = flags & SH_BLEND_MULTISAMPLE ? (flags >> 8) & 63 : 1;
    *words = 0;
    if (!samples || samples > 32 || (samples & (samples - 1))) return NULL;
    int array = !!(flags & SH_BLEND_ARRAY), multisample = !!(flags & SH_BLEND_MULTISAMPLE);
    SpvEmitter e;
    spv_emit_init(&e, 4096);
    spv_emit_shader_capability(&e);
    spv_emit_capability(&e, 50); /* ImageQuery */
    spv_emit_capability(&e, 55); /* StorageImageReadWithoutFormat: validity */
    spv_emit_capability(&e, 56); /* StorageImageWriteWithoutFormat: integer destination */
    if (multisample) spv_emit_capability(&e, 27); /* StorageImageMultisample */
    if (multisample && array) spv_emit_capability(&e, 48); /* ImageMSArray */
    uint32_t f32 = spv_emit_type_f32(&e), boolean = spv_emit_type_bool(&e), u32 = spv_emit_type_u32(&e);
    uint32_t integer = signed_target ? spv_emit_type_i32(&e) : u32;
    uint32_t float4 = spv_emit_type_vec4(&e, f32), integer4 = spv_emit_type_vec4(&e, integer);
    uint32_t uint3 = spv_emit_type_vec3(&e, u32), coord_type = array ? uint3 : spv_emit_type_vec2(&e, u32);
    uint32_t validity_type = spv_emit_type_image(&e, f32, SPV_DIM_2D, 0, array, multisample, 2, 0);
    uint32_t target_type = spv_emit_type_image(&e, integer, SPV_DIM_2D, 0, array, multisample, 2, 0);
    uint32_t validity_variable = spv_emit_resource(&e, SPV_STORAGE_UNIFORM_CONSTANT, validity_type, 0, 1);
    uint32_t target_variable = spv_emit_resource(&e, SPV_STORAGE_UNIFORM_CONSTANT, target_type, 0, 2);
    spv_emit_decorate(&e, validity_variable, SPV_DECORATION_NON_WRITABLE, NULL, 0);
    spv_emit_decorate(&e, target_variable, 25, NULL, 0); /* NonReadable */
    int tiled = !!(flags & SH_CAPTURE_TILED);
    uint32_t invocation = spv_emit_builtin(&e, SPV_STORAGE_INPUT, uint3, tiled ? 26 : 28);
    uint32_t interfaces[6] = {invocation, validity_variable, target_variable}, interface_count = 3;
    uint32_t tiles = 0, local = 0;
    if (tiled) {
        tiles = shader_capture_tiles_declare(&e, u32, 0, 3);
        local = spv_emit_builtin(&e, SPV_STORAGE_INPUT, uint3, 27);
        interfaces[interface_count++] = tiles;
        interfaces[interface_count++] = local;
    }
    uint32_t function = spv_emit_function(&e, 0, spv_emit_type_void(&e), NULL, 0, NULL);
    uint32_t validity = spv_emit_load(&e, validity_type, validity_variable);
    uint32_t target = spv_emit_load(&e, target_type, target_variable);
    uint32_t xyz = spv_emit_load(&e, uint3, invocation);
    if (tiled)
        xyz = shader_capture_tile_coordinates(&e, u32, tiles, xyz,
                                                spv_emit_load(&e, uint3, local), !!(flags & SH_CAPTURE_ADAPTIVE));
    uint32_t size = SPV(&e, SPV_OP_IMAGE_QUERY_SIZE, coord_type, target);
    uint32_t inside = 0, component[3];
    for (uint32_t i = 0; i < (array ? 3u : 2u); i++) {
        component[i] = spv_emit_vec4_extract(&e, u32, xyz, i);
        uint32_t limit = spv_emit_vec4_extract(&e, u32, size, i);
        uint32_t fits = SPV(&e, SPV_OP_U_LESS_THAN, boolean, component[i], limit);
        inside = i ? SPV(&e, SPV_OP_LOGICAL_AND, boolean, inside, fits) : fits;
    }
    uint32_t coordinate = array ? xyz : SPV(&e, SPV_OP_COMPOSITE_CONSTRUCT, coord_type, component[0], component[1]);
    uint32_t apply = spv_emit_id(&e), done = spv_emit_id(&e);
    spv_emit_selection_merge(&e, done);
    spv_emit_branch_conditional(&e, inside, apply, done);
    spv_emit_label(&e, apply);
    uint32_t zero = spv_emit_constant_u32(&e, u32, 0), mask = zero, covered[32], sample_id[32];
    for (uint32_t sample = 0; sample < samples; sample++) {
        sample_id[sample] = spv_emit_constant_u32(&e, u32, sample);
        uint32_t value = multisample ? SPV(&e, SPV_OP_IMAGE_READ, float4, validity, coordinate, 64, sample_id[sample])
                                     : SPV(&e, SPV_OP_IMAGE_READ, float4, validity, coordinate);
        uint32_t red = spv_emit_vec4_extract(&e, f32, value, 0);
        uint32_t empty = SPV(&e, SPV_OP_F_ORD_EQUAL, boolean, red, spv_emit_constant_f32(&e, f32, 0));
        covered[sample] = spv_emit_unary(&e, SPV_OP_LOGICAL_NOT, boolean, empty);
        uint32_t bit = spv_emit_ternary(&e, SPV_OP_SELECT, u32, covered[sample],
                                        spv_emit_constant_u32(&e, u32, 1u << sample), zero);
        mask = SPV(&e, SPV_OP_BITWISE_OR, u32, mask, bit);
    }
    uint32_t red = signed_target ? spv_emit_unary(&e, SPV_OP_BITCAST, integer, mask) : mask;
    uint32_t integer_zero = spv_emit_constant_u32(&e, integer, 0);
    uint32_t result = spv_emit_vec4_construct(&e, integer4, (const uint32_t[4]){red, integer_zero, integer_zero,
                                                                                            integer_zero});
    /* The same union is written only after every validity sample has been read, so split host fragment shader
       invocations cannot narrow the final mask. A validity of zero leaves the destination's old sample intact. */
    for (uint32_t sample = 0; sample < samples; sample++) {
        uint32_t write = spv_emit_id(&e), next = spv_emit_id(&e);
        spv_emit_selection_merge(&e, next);
        spv_emit_branch_conditional(&e, covered[sample], write, next);
        spv_emit_label(&e, write);
        if (multisample)
            SPV_VOID(&e, SPV_OP_IMAGE_WRITE, target, coordinate, result, 64, sample_id[sample]);
        else
            SPV_VOID(&e, SPV_OP_IMAGE_WRITE, target, coordinate, result);
        spv_emit_branch(&e, next);
        spv_emit_label(&e, next);
    }
    spv_emit_branch(&e, done);
    spv_emit_label(&e, done);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    spv_emit_compute_entry_str(&e, function, "main", interfaces, interface_count);
    spv_emit_local_size(&e, function, 8, 8, 1);
    uint32_t* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}

uint32_t* shader_coverage_vertex_spirv(size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 512);
    spv_emit_shader_capability(&e);
    uint32_t f = spv_emit_type_f32(&e), u = spv_emit_type_u32(&e), vec4 = spv_emit_type_vec4(&e, f);
    uint32_t vertex = spv_emit_builtin_input_u32(&e, u, 42), position = spv_emit_position_output(&e, vec4);
    uint32_t interfaces[] = {vertex, position};
    uint32_t function = spv_emit_function(&e, 0, spv_emit_type_void(&e), NULL, 0, NULL);
    uint32_t index = spv_emit_load(&e, u, vertex), one = spv_emit_constant_u32(&e, u, 1);
    uint32_t x = spv_emit_binary(&e, SPV_OP_BITWISE_AND, u, index, one);
    uint32_t y = spv_emit_binary(&e, SPV_OP_SHIFT_RIGHT_LOGICAL, u, index, one);
    uint32_t output[4];
    for (uint32_t axis = 0; axis < 2; axis++) {
        uint32_t value = spv_emit_unary(&e, SPV_OP_CONVERT_U_TO_F, f, axis ? y : x);
        output[axis] = spv_emit_binary(&e, SPV_OP_F_ADD, f,
                                       spv_emit_binary(&e, SPV_OP_F_MUL, f, value, spv_emit_constant_f32(&e, f, 4)),
                                       spv_emit_constant_f32(&e, f, -1));
    }
    output[2] = spv_emit_constant_f32(&e, f, 0);
    output[3] = spv_emit_constant_f32(&e, f, 1);
    spv_emit_store(&e, position, spv_emit_vec4_construct(&e, vec4, output));
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    spv_emit_entry_str(&e, SPV_STAGE_VERTEX, function, "main", interfaces, 2);
    uint32_t* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}
uint32_t* shader_coverage_fragment_spirv(uint32_t samples, uint32_t components, size_t* words) {
    *words = 0;
    if (samples < 2 || samples > 16 || (samples & (samples - 1)) || components > 15) return NULL;
    SpvEmitter e;
    spv_emit_init(&e, 4096);
    spv_emit_shader_capability(&e);
    uint32_t f = spv_emit_type_f32(&e), u = spv_emit_type_u32(&e), b = spv_emit_type_bool(&e);
    uint32_t vec4 = spv_emit_type_vec4(&e, f), coord_type = spv_emit_type_vec2(&e, u);
    uint32_t image_type = spv_emit_type_image(&e, f, SPV_DIM_2D, 0, 0, 1, 1, 0);
    uint32_t source_variable = spv_emit_resource(&e, SPV_STORAGE_UNIFORM_CONSTANT, image_type, 0, 0);
    uint32_t valid_variable = spv_emit_resource(&e, SPV_STORAGE_UNIFORM_CONSTANT, image_type, 0, 1);
    uint32_t array = spv_emit_type_array(&e, f, 16), stride = 4, offset = 0;
    spv_emit_decorate(&e, array, SPV_DECORATION_ARRAY_STRIDE, &stride, 1);
    uint32_t block = spv_emit_type_struct(&e, &array, &offset, 1);
    spv_emit_decorate(&e, block, SPV_DECORATION_BLOCK, NULL, 0);
    uint32_t push = spv_emit_variable(&e, SPV_STORAGE_PUSH_CONSTANT, block);
    uint32_t fragcoord = spv_emit_fragcoord_input(&e, vec4), output = spv_emit_output_vec4(&e, vec4, 0);
    uint32_t interfaces[] = {source_variable, valid_variable, fragcoord, output, push};
    uint32_t function = spv_emit_function(&e, 0, spv_emit_type_void(&e), NULL, 0, NULL);
    uint32_t xyzw = spv_emit_load(&e, vec4, fragcoord), xy[2];
    for (uint32_t axis = 0; axis < 2; axis++)
        xy[axis] = spv_emit_unary(&e, SPV_OP_CONVERT_F_TO_U, u, spv_emit_vec4_extract(&e, f, xyzw, axis));
    uint32_t coordinate = SPV(&e, SPV_OP_COMPOSITE_CONSTRUCT, coord_type, xy[0], xy[1]);
    uint32_t source = spv_emit_load(&e, image_type, source_variable), valid = spv_emit_load(&e, image_type, valid_variable);
    uint32_t zero = spv_emit_constant_u32(&e, u, 0), one = spv_emit_constant_u32(&e, u, 1);
    uint32_t zero_f = spv_emit_constant_f32(&e, f, 0), count = zero, chosen[4] = {zero_f, zero_f, zero_f, zero_f};
    for (uint32_t sample = 0; sample < samples; sample++) {
        uint32_t index = spv_emit_constant_u32(&e, u, sample);
        uint32_t validity = SPV(&e, SPV_OP_IMAGE_FETCH, vec4, valid, coordinate, 64, index);
        uint32_t red = spv_emit_vec4_extract(&e, f, validity, 0);
        uint32_t covered = spv_emit_unary(&e, SPV_OP_LOGICAL_NOT, b, SPV(&e, SPV_OP_F_ORD_EQUAL, b, red, zero_f));
        uint32_t first = SPV(&e, SPV_OP_LOGICAL_AND, b, covered, spv_emit_binary(&e, SPV_OP_I_EQUAL, b, count, zero));
        uint32_t color = SPV(&e, SPV_OP_IMAGE_FETCH, vec4, source, coordinate, 64, index);
        for (uint32_t component = 0; component < 4; component++)
            chosen[component] = spv_emit_ternary(&e, SPV_OP_SELECT, f, first,
                                                 spv_emit_vec4_extract(&e, f, color, component), chosen[component]);
        count = spv_emit_iadd_u32(&e, u, count, spv_emit_ternary(&e, SPV_OP_SELECT, u, covered, one, zero));
    }
    uint32_t discard = spv_emit_id(&e), apply = spv_emit_id(&e);
    uint32_t empty = spv_emit_binary(&e, SPV_OP_I_EQUAL, b, count, zero);
    spv_emit_selection_merge(&e, apply);
    spv_emit_branch_conditional(&e, empty, discard, apply);
    spv_emit_label(&e, discard);
    spv_emit_kill(&e);
    spv_emit_label(&e, apply);
    if (components) {
        uint32_t index = SPV(&e, SPV_OP_I_SUB, u, count, one);
        uint32_t pointer = SPV(&e, SPV_OP_ACCESS_CHAIN, spv_emit_type_pointer(&e, SPV_STORAGE_PUSH_CONSTANT, f),
                                push, zero, index);
        uint32_t factor = spv_emit_load(&e, f, pointer);
        for (uint32_t component = 0; component < 4; component++)
            if (components & (1u << component))
                chosen[component] = spv_emit_binary(&e, SPV_OP_F_MUL, f, chosen[component], factor);
    }
    spv_emit_store(&e, output, spv_emit_vec4_construct(&e, vec4, chosen));
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    spv_emit_entry_str(&e, SPV_STAGE_FRAGMENT, function, "main", interfaces, 5);
    spv_emit_fragment_origin_upper_left(&e, function);
    spv_emit_float_controls(&e, function, 0, 1, 0);
    uint32_t* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}
