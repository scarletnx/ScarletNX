/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A fragment program that interpolates its own inputs (shader.h, SH_VARYING_) needs their values at the triangle's
   three vertices. Where the device has no per-vertex fragment inputs, a geometry program passes each triangle on
   and writes those values on all three of its vertices, so that they reach the fragment program flat: the three
   positions, and for each generic of the pass its first vertex's value in the generic itself and the other two in
   generics the programs leave free. As Eden's GeneratePerVertexGeometry does (PR 4575, GPL-3.0-or-later). */
#include "nx/gpu/vk/varying_geometry.h"
#include "common/utilities/spirv_emit.h"
#include "common/utilities/bits.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>

u32* vk_varying_geometry_spirv(u32 outputs, u32 per_vertex, u32 slot, size_t* words) {
    *words = 0;
    u32 end = shader_varying_copy(per_vertex, slot, 32, 0);
    if (!per_vertex || (per_vertex & ~outputs) || end > 32 || (slot < 32 && outputs >> slot)) return NULL;
    SpvEmitter e;
    spv_emit_init(&e, 4096);
    spv_emit_shader_capability(&e);
    spv_emit_capability(&e, 2); /* Geometry */
    u32 f32 = spv_emit_type_float(&e, 32), u32_type = spv_emit_type_u32(&e);
    u32 vec4 = spv_emit_type_vector(&e, f32, 4), input_ptr = spv_emit_type_pointer(&e, 1, vec4);
    u32 interfaces[4 + 3 * 32], in[33] = {0}, out[33] = {0}, place[32] = {0}, count = 0;
    /* 0 the position, 1 + g generic g */
    for (u32 i = 0; i < 33; i++) {
        if (i && !(outputs >> (i - 1) & 1)) continue;
        in[i] = spv_emit_interface_variable(&e, 1, vec4, u32_type, 3, i ? SPV_NOT_BUILTIN : 0, i ? i - 1 : 0, 0);
        out[i] = i ? spv_emit_output_vec4(&e, vec4, i - 1) : spv_emit_position_output(&e, vec4);
        interfaces[count++] = in[i];
        interfaces[count++] = out[i];
    }
    for (u32 g = slot; g < end; g++) interfaces[count++] = place[g] = spv_emit_output_vec4(&e, vec4, g);
    /* a fragment program's primitive number is the geometry program's to give */
    u32 primitive_in = spv_emit_builtin_input_u32(&e, u32_type, 7);
    u32 primitive_out = spv_emit_interface_variable(&e, 3, u32_type, u32_type, 0, 7, 0, 0);
    interfaces[count++] = primitive_in;
    interfaces[count++] = primitive_out;
    u32 void_type = spv_emit_type_void(&e), fn_type = spv_emit_type_function_void(&e, void_type);
    u32 fn = spv_emit_id(&e);
    spv_emit_entry_str(&e, ST_GS, fn, "main", interfaces, count);
    spv_emit_execution_mode(&e, fn, 22, 0, 0); /* Triangles */
    spv_emit_execution_mode(&e, fn, 29, 0, 0); /* OutputTriangleStrip */
    spv_emit_execution_mode(&e, fn, 26, 1, 3); /* OutputVertices */
    spv_emit_execution_mode(&e, fn, 0, 1, 1);  /* Invocations */
    spv_emit_function_void(&e, void_type, fn, fn_type);
    spv_emit_label(&e, spv_emit_id(&e));
    u32 value[33][3], primitive = spv_emit_load(&e, u32_type, primitive_in);
    for (u32 i = 0; i < 33; i++)
        for (u32 k = 0; k < 3 && in[i]; k++)
            value[i][k] = spv_emit_load(
                &e, vec4, SPV(&e, SPV_OP_ACCESS_CHAIN, input_ptr, in[i], spv_emit_constant_u32(&e, u32_type, k)));
    for (u32 vertex = 0; vertex < 3; vertex++) {
        spv_emit_store(&e, out[0], value[0][vertex]);
        for (u32 k = 0; k < 3; k++) spv_emit_store(&e, place[slot + k], value[0][k]);
        for (u32 g = 0; g < 32; g++) {
            if (!in[1 + g]) continue;
            int taken = per_vertex >> g & 1;
            spv_emit_store(&e, out[1 + g], value[1 + g][taken ? 0 : vertex]);
            for (u32 k = 0; k < 2 && taken; k++)
                spv_emit_store(&e, place[shader_varying_copy(per_vertex, slot, g, k)], value[1 + g][1 + k]);
        }
        spv_emit_store(&e, primitive_out, primitive);
        spv_emit_emit_vertex(&e);
    }
    spv_emit_end_primitive(&e);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    u32* result = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return result;
}

VkSh* vk_varying_geometry(const VkSh* vertex, u32 per_vertex, u32 slot) {
    if (!vertex || vertex->failed || !g_vk.feat.geometryShader) return NULL;
    const VkPhysicalDeviceLimits* limits = &g_vk.props.limits;
    u32 outputs = vertex->si.out_mask, end = shader_varying_copy(per_vertex, slot, 32, 0);
    /* the position with them: a device may count it among an interface's components */
    u32 in_components = 4 * (1 + snx_popcount64(outputs)), out_components = in_components + 4 * (end - slot) + 1;
    if (in_components > limits->maxGeometryInputComponents || out_components > limits->maxGeometryOutputComponents ||
        3 * out_components > limits->maxGeometryTotalOutputComponents || limits->maxGeometryOutputVertices < 3 ||
        out_components > limits->maxFragmentInputComponents) {
        LOG_LIMIT(2,
                  "vulkan: a per-vertex pass's geometry program of %u input and %u output components is past the "
                  "device's %u and %u: the draw's inputs are the driver's interpolation\n",
                  in_components, out_components, limits->maxGeometryInputComponents,
                  limits->maxGeometryOutputComponents);
        return NULL;
    }
    u32 key[26] = {0x56475350, outputs, per_vertex, slot, 1};
    u64 hash = vk_hash(key, sizeof key);
    VkSh* cached = vk_shader_generated_find(hash, ST_GS, key);
    if (cached) return cached;
    VkSh* made = calloc(1, sizeof *made);
    if (!made) return NULL;
    size_t words;
    u32* code = vk_varying_geometry_spirv(outputs, per_vertex, slot, &words);
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = code};
    if (!code || !VK_OK(vkCreateShaderModule(g_vk.dev, &info, NULL, &made->mod))) {
        free(code);
        free(made);
        return NULL;
    }
    made->stage = made->si.stage = ST_GS;
    made->si.in_mask = outputs;
    made->si.out_mask = outputs;
    for (u32 g = slot; g < end; g++) made->si.out_mask |= 1u << g;
    made->hash = hash;
    memcpy(made->mask, key, sizeof key);
    u8 source[16];
    for (u32 i = 0; i < 4; i++) snx_st_le32(source + i * 4, key[i]);
    vk_shader_generated_record(made, code, words, source, sizeof source);
    free(code);
    return made;
}
