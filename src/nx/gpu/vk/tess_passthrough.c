/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Native hardware can run a tessellation evaluation stage without a control program. Vulkan requires a control
   stage. Each invocation forwards its own vertex; invocation zero writes fixed tessellation levels from push
   constants. There are no CPU vertex reads, temporary vertex buffers, or per-draw shader compilations. */
#include "nx/gpu/vk/tess_passthrough.h"
#include "common/utilities/spirv_emit.h"
#include "common/utilities/bits.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>

u32* vk_tess_passthrough_spirv_full(u32 vertices, u32 attributes, int point_size, u32 clip_mask, size_t* words) {
    *words = 0;
    if (!vertices || vertices > 32 || clip_mask > 255) return NULL;
    SpvEmitter e;
    spv_emit_init(&e, 2048);
    spv_emit_shader_capability(&e);
    spv_emit_capability(&e, 3); /* Tessellation */
    u32 f32 = spv_emit_type_float(&e, 32), u32_type = spv_emit_type_u32(&e);
    u32 boolean = spv_emit_type_bool(&e), vec4 = spv_emit_type_vector(&e, f32, 4);
    u32 input_ptr = spv_emit_type_pointer(&e, 1, vec4), output_ptr = spv_emit_type_pointer(&e, 3, vec4);
    u32 interfaces[74], in[33] = {0}, out[33] = {0}, count = 0;
    for (u32 i = 0; i < 33; i++) {
        if (i && !(attributes & (1u << (i - 1)))) continue;
        in[i] = spv_emit_interface_variable(&e, 1, vec4, u32_type, 32, i ? SPV_NOT_BUILTIN : 0, i ? i - 1 : 0, 0);
        out[i] =
            spv_emit_interface_variable(&e, 3, vec4, u32_type, vertices, i ? SPV_NOT_BUILTIN : 0, i ? i - 1 : 0, 0);
        interfaces[count++] = in[i];
        interfaces[count++] = out[i];
    }
    u32 point_in = 0, point_out = 0, clips_in = 0, clips_out = 0;
    if (point_size) {
        spv_emit_capability(&e, 23); /* TessellationPointSize */
        point_in = spv_emit_interface_variable(&e, 1, f32, u32_type, 32, 1, 0, 0);
        point_out = spv_emit_interface_variable(&e, 3, f32, u32_type, vertices, 1, 0, 0);
        interfaces[count++] = point_in;
        interfaces[count++] = point_out;
    }
    u32 clip_count = clip_mask ? 8 : 0;
    if (clip_count) {
        spv_emit_clip_distance_capability(&e);
        u32 array = spv_emit_type_array(&e, f32, clip_count);
        clips_in = spv_emit_interface_variable(&e, 1, array, u32_type, 32, 3, 0, 0);
        clips_out = spv_emit_interface_variable(&e, 3, array, u32_type, vertices, 3, 0, 0);
        interfaces[count++] = clips_in;
        interfaces[count++] = clips_out;
    }
    u32 outer = spv_emit_interface_variable(&e, 3, f32, u32_type, 4, 11, 0, 1);
    u32 inner = spv_emit_interface_variable(&e, 3, f32, u32_type, 2, 12, 0, 1);
    u32 invocation = spv_emit_builtin_input_u32(&e, u32_type, 8);
    interfaces[count++] = outer;
    interfaces[count++] = inner;
    interfaces[count++] = invocation;
    u32 push_pointer;
    /* inner = pc[14..15], outer = pc[16..19], following point size in pc[12]. */
    u32 push = spv_emit_push_vec4(&e, vec4, f32, 48, vec4, &push_pointer);
    interfaces[count++] = push;
    u32 void_type = spv_emit_type_void(&e), fn_type = spv_emit_type_function_void(&e, void_type);
    u32 fn = spv_emit_id(&e);
    spv_emit_entry_str(&e, ST_TCS, fn, "main", interfaces, count);
    spv_emit_execution_mode(&e, fn, 26, 1, vertices); /* OutputVertices */
    spv_emit_function_void(&e, void_type, fn, fn_type);
    spv_emit_label(&e, spv_emit_id(&e));
    u32 vertex = spv_emit_load(&e, u32_type, invocation);
    if (point_size) {
        u32 in_ptr = spv_emit_type_pointer(&e, 1, f32), out_ptr = spv_emit_type_pointer(&e, 3, f32);
        u32 value = spv_emit_load(&e, f32, SPV(&e, SPV_OP_ACCESS_CHAIN, in_ptr, point_in, vertex));
        spv_emit_store(&e, SPV(&e, SPV_OP_ACCESS_CHAIN, out_ptr, point_out, vertex), value);
    }
    for (u32 i = 0; i < clip_count; i++) {
        u32 index = spv_emit_constant_u32(&e, u32_type, i);
        u32 source = spv_emit_interface_scalar_pointer(&e, 1, f32, clips_in, vertex, index);
        u32 destination = spv_emit_interface_scalar_pointer(&e, 3, f32, clips_out, vertex, index);
        spv_emit_store(&e, destination, spv_emit_load(&e, f32, source));
    }
    for (u32 i = 0; i < 33; i++) {
        if (!in[i]) continue;
        u32 source = SPV(&e, SPV_OP_ACCESS_CHAIN, input_ptr, in[i], vertex);
        u32 destination = SPV(&e, SPV_OP_ACCESS_CHAIN, output_ptr, out[i], vertex);
        spv_emit_store(&e, destination, spv_emit_load(&e, vec4, source));
    }
    u32 write = spv_emit_id(&e), done = spv_emit_id(&e), zero = spv_emit_constant_u32(&e, u32_type, 0);
    u32 first = spv_emit_i_equal(&e, boolean, vertex, zero);
    spv_emit_selection_merge(&e, done);
    spv_emit_branch_conditional(&e, first, write, done);
    spv_emit_label(&e, write);
    for (u32 i = 0; i < 6; i++) {
        u32 index = spv_emit_constant_u32(&e, u32_type, i < 4 ? i : i - 4);
        u32 value =
            spv_emit_push_component_f32(&e, push_pointer, push, spv_emit_constant_u32(&e, u32_type, i < 4 ? 1 : 0),
                                        spv_emit_constant_u32(&e, u32_type, i < 4 ? i : i - 2), f32);
        spv_emit_store(&e, spv_emit_output_element_pointer(&e, f32, i < 4 ? outer : inner, index), value);
    }
    spv_emit_branch(&e, done);
    spv_emit_label(&e, done);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    u32* result = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return result;
}
u32* vk_tess_passthrough_spirv(u32 vertices, u32 attributes, size_t* words) {
    return vk_tess_passthrough_spirv_full(vertices, attributes, 0, 0, words);
}

VkSh* vk_tess_passthrough(u32 vertices, const VkSh* vertex, const VkSh* evaluation) {
    if (!vertex || vertex->failed || !evaluation || evaluation->failed || !g_vk.feat.tessellationShader || !vertices ||
        vertices > 32 || vertices > g_vk.props.limits.maxTessellationPatchSize)
        return NULL;
    u32 attributes = vertex->si.out_mask & evaluation->si.in_mask;
    u32 point_size = vertex->si.writes_point_size && evaluation->si.reads_point_size;
    u32 clip_mask = evaluation->si.clip_input_mask ? vertex->si.uses_clip_mask : 0;
    u32 components = 4 * (1 + snx_popcount64(attributes)) + point_size + (clip_mask ? 8 : 0);
    if (point_size && !g_vk.feat.shaderTessellationAndGeometryPointSize) return NULL;
    const VkPhysicalDeviceLimits* limits = &g_vk.props.limits;
    if (components > limits->maxTessellationControlPerVertexInputComponents ||
        components > limits->maxTessellationControlPerVertexOutputComponents ||
        limits->maxTessellationControlPerPatchOutputComponents < 6 ||
        components * vertices + 6 > limits->maxTessellationControlTotalOutputComponents) {
        LOG_LIMIT(2, "vulkan: evaluation-only control shader exceeds the device's interface limits\n");
        return NULL;
    }
    u32 key[26] = {0x54435350, vertices, attributes, point_size, clip_mask, 1};
    u64 hash = vk_hash(key, sizeof key);
    VkSh* cached = vk_shader_generated_find(hash, ST_TCS, key);
    if (cached) return cached;
    VkSh* made = calloc(1, sizeof *made);
    if (!made) return NULL;
    size_t words;
    u32* code = vk_tess_passthrough_spirv_full(vertices, attributes, (int)point_size, clip_mask, &words);
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = code};
    if (!code || !VK_OK(vkCreateShaderModule(g_vk.dev, &info, NULL, &made->mod))) {
        free(code);
        free(made);
        return NULL;
    }
    made->stage = made->si.stage = ST_TCS;
    made->si.in_mask = made->si.out_mask = attributes;
    made->si.reads_point_size = made->si.writes_point_size = (int)point_size;
    made->si.clip_input_mask = made->si.uses_clip_mask = clip_mask;
    made->hash = hash;
    memcpy(made->mask, key, sizeof key);
    u8 source[24];
    for (u32 i = 0; i < 6; i++) snx_st_le32(source + i * 4, key[i]);
    vk_shader_generated_record(made, code, words, source, sizeof source);
    free(code);
    return made;
}

void vk_tess_passthrough_free(void) {
    /* The shader cache owns generated modules, including modules loaded from earlier runs. */
}
