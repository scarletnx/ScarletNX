/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_ADVANCED_BLEND_SPIRV_H
#define SNX_TEST_ADVANCED_BLEND_SPIRV_H
#include "nx/gpu/shader/advanced_blend_spirv.h"

/* Input holds alternating source/destination vec4 values. Invocation i writes the equation's vec4 to output i. */
static inline u32* nvn_blend_compute_spirv(u32 mode, int premultiplied, u32 overlap, size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 4096);
    spv_emit_shader_capability(&e);
    u32 f = spv_emit_type_f32(&e), b = spv_emit_type_bool(&e), u = spv_emit_type_u32(&e);
    u32 vec4 = spv_emit_type_vec4(&e, f), uv3 = spv_emit_type_vec3(&e, u);
    u32 array = spv_emit_type_runtime_array(&e, vec4, 16), structure = spv_emit_type_struct_one(&e, array);
    u32 input = spv_emit_storage_buffer(&e, structure, 0, 0), output = spv_emit_storage_buffer(&e, structure, 0, 1);
    u32 invocation = spv_emit_variable(&e, 1, uv3), builtin = 28;
    spv_emit_decorate(&e, invocation, 11, &builtin, 1);
    u32 fn = spv_emit_id(&e), interfaces[3] = {invocation, input, output};
    u32 v = spv_emit_type_void(&e), ft = spv_emit_type_function_void(&e, v);
    spv_emit_compute_entry_str(&e, fn, "main", interfaces, 3);
    spv_emit_local_size(&e, fn, 1, 1, 1);
    spv_emit_function_void(&e, v, fn, ft);
    spv_emit_label(&e, spv_emit_id(&e));
    u32 index = spv_emit_vec4_extract(&e, u, spv_emit_load(&e, uv3, invocation), 0);
    u32 ptr = spv_emit_type_storage_pointer(&e, vec4), zero = spv_emit_constant_u32(&e, u, 0);
    u32 first = SPV(&e, SPV_OP_I_MUL, u, index, spv_emit_constant_u32(&e, u, 2));
    u32 second = SPV(&e, SPV_OP_I_ADD, u, first, spv_emit_constant_u32(&e, u, 1));
    u32 source = spv_emit_load(&e, vec4, SPV(&e, SPV_OP_ACCESS_CHAIN, ptr, input, zero, first));
    u32 destination = spv_emit_load(&e, vec4, SPV(&e, SPV_OP_ACCESS_CHAIN, ptr, input, zero, second));
    u32 result = shader_advanced_blend_spirv(&e, f, b, vec4, source, destination, mode, premultiplied, overlap);
    if (result) spv_emit_store(&e, SPV(&e, SPV_OP_ACCESS_CHAIN, ptr, output, zero, index), result);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    u32* code = result ? spv_emit_finish(&e, words) : NULL;
    spv_emit_destroy(&e);
    return code;
}
#endif
