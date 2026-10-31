/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/spirv_emit.h"
#include "spirv_emitter_fixture.h"

uint32_t* snx_test_spirv_compute(size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 1); /* force the function buffer's growth path */
    spv_emit_shader_capability(&e);
    uint32_t function = spv_emit_id(&e);
    uint32_t label = spv_emit_id(&e);
    uint32_t void_type = spv_emit_type_void(&e);
    uint32_t u32_type = spv_emit_type_u32(&e);
    uint32_t f32_type = spv_emit_type_f32(&e);
    uint32_t vec4_type = spv_emit_type_vec4(&e, f32_type);
    uint32_t function_type = spv_emit_type_function_void(&e, void_type);
    uint32_t array_type = spv_emit_type_runtime_array(&e, u32_type, 4);
    uint32_t struct_type = spv_emit_type_struct_one(&e, array_type);
    uint32_t buffer = spv_emit_storage_buffer(&e, struct_type, 0, 0);
    uint32_t pointer_type = spv_emit_type_storage_pointer(&e, u32_type);
    spv_emit_compute_entry_str(&e, function, "main", &buffer, 1);
    spv_emit_local_size(&e, function, 1, 1, 1);
    spv_emit_name_str(&e, function, "main");
    uint32_t one = spv_emit_constant_f32(&e, f32_type, 1.0f);
    uint32_t zero = spv_emit_constant_f32(&e, f32_type, 0.0f);
    uint32_t components[4] = {one, zero, zero, one};
    spv_emit_constant_vec4(&e, vec4_type, components);
    spv_emit_constant_u32(&e, u32_type, 42);
    spv_emit_type_bool(&e);
    uint32_t zero_index = spv_emit_constant_u32(&e, u32_type, 0);
    uint32_t forty = spv_emit_constant_u32(&e, u32_type, 40);
    uint32_t two = spv_emit_constant_u32(&e, u32_type, 2);
    spv_emit_function_void(&e, void_type, function, function_type);
    spv_emit_label(&e, label);
    uint32_t target = spv_emit_storage_access_u32(&e, pointer_type, buffer, zero_index, zero_index);
    uint32_t result = spv_emit_iadd_u32(&e, u32_type, forty, two);
    spv_emit_store_u32(&e, target, result);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    uint32_t* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}

uint32_t* snx_test_spirv_flush(uint32_t n, int signed_zeros, size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 64);
    spv_emit_shader_capability(&e);
    uint32_t glsl = spv_emit_glsl450_import(&e);
    uint32_t function = spv_emit_id(&e);
    uint32_t label = spv_emit_id(&e);
    uint32_t void_type = spv_emit_type_void(&e);
    uint32_t u32_type = spv_emit_type_u32(&e);
    uint32_t f32_type = spv_emit_type_f32(&e);
    uint32_t bool_type = spv_emit_type_bool(&e);
    uint32_t function_type = spv_emit_type_function_void(&e, void_type);
    uint32_t array_type = spv_emit_type_runtime_array(&e, u32_type, 4);
    uint32_t struct_type = spv_emit_type_struct_one(&e, array_type);
    uint32_t buffer = spv_emit_storage_buffer(&e, struct_type, 0, 0);
    uint32_t pointer_type = spv_emit_type_storage_pointer(&e, u32_type);
    spv_emit_compute_entry_str(&e, function, "main", &buffer, 1);
    spv_emit_local_size(&e, function, 1, 1, 1);
    spv_emit_float_controls(&e, function, 0, signed_zeros, 0);
    uint32_t member = spv_emit_constant_u32(&e, u32_type, 0);
    spv_emit_function_void(&e, void_type, function, function_type);
    spv_emit_label(&e, label);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t in = spv_emit_storage_access_u32(&e, pointer_type, buffer, member, spv_emit_constant_u32(&e, u32_type, k));
        uint32_t value = spv_emit_bitcast(&e, f32_type, spv_emit_load(&e, u32_type, in));
        uint32_t flushed = spv_emit_flush_subnormal(&e, f32_type, u32_type, bool_type, glsl, value, signed_zeros);
        uint32_t out =
            spv_emit_storage_access_u32(&e, pointer_type, buffer, member, spv_emit_constant_u32(&e, u32_type, n + k));
        spv_emit_store_u32(&e, out, spv_emit_bitcast(&e, u32_type, flushed));
    }
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    uint32_t* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}

uint32_t* snx_test_spirv_graphics(int fragment, size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 64);
    spv_emit_shader_capability(&e);
    uint32_t glsl = spv_emit_glsl450_import(&e);
    uint32_t function = spv_emit_id(&e);
    uint32_t label = spv_emit_id(&e);
    uint32_t void_type = spv_emit_type_void(&e);
    uint32_t f32_type = spv_emit_type_f32(&e);
    uint32_t u32_type = spv_emit_type_u32(&e);
    uint32_t vec4_type = spv_emit_type_vec4(&e, f32_type);
    uint32_t vec2_type = spv_emit_type_vec2(&e, f32_type);
    uint32_t reg_type = spv_emit_type_function_pointer(&e, u32_type);
    uint32_t function_type = spv_emit_type_function_void(&e, void_type);
    uint32_t output = fragment ? spv_emit_output_vec4(&e, vec4_type, 0) : spv_emit_position_output(&e, vec4_type);
    uint32_t output2 = fragment ? spv_emit_output_vec4(&e, vec4_type, 1) : 0;
    uint32_t input = fragment ? spv_emit_input_vec4(&e, vec4_type, 0, 2) : 0;
    uint32_t fragcoord = fragment ? spv_emit_fragcoord_input(&e, vec4_type) : 0;
    uint32_t component_ptr = 0;
    uint32_t cbuf =
        fragment ? spv_emit_uniform_vec4_array(&e, vec4_type, f32_type, u32_type, 1, 0, 4096, &component_ptr) : 0;
    uint32_t sampled_type = 0, shadow_type = 0;
    uint32_t sampler = fragment ? spv_emit_sampler2d(&e, f32_type, 1, 34, 0, &sampled_type) : 0;
    uint32_t shadow = fragment ? spv_emit_sampler2d(&e, f32_type, 1, 35, 1, &shadow_type) : 0;
    uint32_t interfaces[7] = {output, output2, input, fragcoord, cbuf, sampler, shadow};
    spv_emit_entry_str(&e, fragment ? SPV_STAGE_FRAGMENT : SPV_STAGE_VERTEX, function, "main", interfaces,
                       fragment ? 7 : 1);
    if (fragment) spv_emit_fragment_origin_upper_left(&e, function);
    if (fragment) spv_emit_float_controls(&e, function, 1, 1, 0);
    uint32_t zero = spv_emit_constant_f32(&e, f32_type, 0.0f);
    uint32_t one = spv_emit_constant_f32(&e, f32_type, 1.0f);
    uint32_t half_bits = spv_emit_constant_u32(&e, u32_type, 0x3f000000);
    uint32_t idx0 = spv_emit_constant_u32(&e, u32_type, 0);
    uint32_t vertex_components[4] = {zero, zero, zero, one};
    uint32_t vertex_color = spv_emit_constant_vec4(&e, vec4_type, vertex_components);
    spv_emit_function_void(&e, void_type, function, function_type);
    spv_emit_label(&e, label);
    uint32_t color = vertex_color;
    if (fragment) {
        uint32_t reg = spv_emit_function_variable(&e, reg_type);
        spv_emit_store(&e, reg, half_bits);
        uint32_t bits = spv_emit_load(&e, u32_type, reg);
        uint32_t half = spv_emit_bitcast(&e, f32_type, bits);
        uint32_t sum = spv_emit_fadd(&e, f32_type, half, half);
        uint32_t product = spv_emit_fmul(&e, f32_type, half, half);
        uint32_t neg = spv_emit_fnegate(&e, f32_type, product);
        uint32_t absval = spv_emit_glsl_unary(&e, f32_type, glsl, SPV_GLSL_F_ABS, neg);
        uint32_t fused = spv_emit_glsl_ternary(&e, f32_type, glsl, SPV_GLSL_FMA, half, half, absval);
        uint32_t varying = spv_emit_load(&e, vec4_type, input);
        uint32_t coord = spv_emit_load(&e, vec4_type, fragcoord);
        uint32_t x = spv_emit_vec4_extract(&e, f32_type, varying, 0);
        uint32_t w = spv_emit_vec4_extract(&e, f32_type, coord, 3);
        uint32_t perspective = spv_emit_fdiv(&e, f32_type, x, w);
        uint32_t constant = spv_emit_uniform_component(&e, component_ptr, cbuf, idx0, idx0, idx0, f32_type);
        uint32_t uv = spv_emit_vec2_construct(&e, vec2_type, half, half);
        uint32_t texel = spv_emit_sample2d(&e, vec4_type, sampled_type, sampler, uv, 0);
        uint32_t compare = spv_emit_sample2d(&e, f32_type, shadow_type, shadow, uv, half);
        uint32_t texel_x = spv_emit_vec4_extract(&e, f32_type, texel, 0);
        uint32_t mix = spv_emit_fadd(&e, f32_type, perspective, constant);
        mix = spv_emit_fadd(&e, f32_type, mix, texel_x);
        spv_emit_no_contraction(&e, sum);
        spv_emit_no_contraction(&e, product);
        uint32_t components[4] = {sum, fused, mix, compare};
        color = spv_emit_vec4_construct(&e, vec4_type, components);
    }
    if (fragment) {
        uint32_t bool_type = spv_emit_type_bool(&e);
        uint32_t half_value = spv_emit_bitcast(&e, f32_type, half_bits);
        uint32_t cond_a = spv_emit_fcompare(&e, bool_type, 9, half_value, one);
        uint32_t cond_b = spv_emit_i_equal(&e, bool_type, half_bits, half_bits);
        uint32_t negated = spv_emit_bool_not(&e, bool_type, cond_a);
        uint32_t condition = spv_emit_bool_binary(&e, bool_type, 0, negated, cond_b);
        uint32_t yes = spv_emit_id(&e), no = spv_emit_id(&e), merge = spv_emit_id(&e);
        spv_emit_selection_merge(&e, merge);
        spv_emit_branch_conditional(&e, condition, yes, no);
        spv_emit_label(&e, yes);
        spv_emit_store(&e, output, color);
        spv_emit_branch(&e, merge);
        spv_emit_label(&e, no);
        spv_emit_store(&e, output2, color);
        spv_emit_branch(&e, merge);
        spv_emit_label(&e, merge);
    } else {
        spv_emit_store(&e, output, color);
    }
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    uint32_t* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}

uint32_t* snx_test_spirv_sample(size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 64);
    spv_emit_shader_capability(&e);
    uint32_t function = spv_emit_id(&e);
    uint32_t label = spv_emit_id(&e);
    uint32_t void_type = spv_emit_type_void(&e);
    uint32_t u32_type = spv_emit_type_u32(&e);
    uint32_t f32_type = spv_emit_type_f32(&e);
    uint32_t vec2_type = spv_emit_type_vec2(&e, f32_type);
    uint32_t vec4_type = spv_emit_type_vec4(&e, f32_type);
    uint32_t function_type = spv_emit_type_function_void(&e, void_type);
    uint32_t array_type = spv_emit_type_runtime_array(&e, u32_type, 4);
    uint32_t struct_type = spv_emit_type_struct_one(&e, array_type);
    uint32_t sampled_type, interfaces[2];
    interfaces[0] = spv_emit_storage_buffer(&e, struct_type, 0, 0);
    interfaces[1] = spv_emit_sampler2d(&e, f32_type, 0, 1, 0, &sampled_type);
    uint32_t pointer_type = spv_emit_type_storage_pointer(&e, u32_type);
    spv_emit_compute_entry_str(&e, function, "main", interfaces, 2);
    spv_emit_local_size(&e, function, 1, 1, 1);
    uint32_t half = spv_emit_constant_f32(&e, f32_type, 0.5f);
    uint32_t zero = spv_emit_constant_f32(&e, f32_type, 0.0f);
    uint32_t at = spv_emit_constant_vec2(&e, vec2_type, half, half);
    uint32_t member = spv_emit_constant_u32(&e, u32_type, 0);
    spv_emit_function_void(&e, void_type, function, function_type);
    spv_emit_label(&e, label);
    uint32_t texel = spv_emit_sample2d_lod(&e, vec4_type, sampled_type, interfaces[1], at, 0, zero);
    uint32_t red = spv_emit_bitcast(&e, u32_type, spv_emit_vec4_extract(&e, f32_type, texel, 0));
    spv_emit_store_u32(&e, spv_emit_storage_access_u32(&e, pointer_type, interfaces[0], member, member), red);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    uint32_t* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}
