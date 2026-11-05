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

typedef struct {
    u32 vertices, attributes;
    VkSh shader;
} TessPass;
static struct {
    TessPass** entries; /* Runtime owners stay at stable addresses while the lookup array grows. */
    u32 count, capacity;
} g_tess_pass;

u32* vk_tess_passthrough_spirv(u32 vertices, u32 attributes, size_t* words) {
    *words = 0;
    if (!vertices || vertices > 32) return NULL;
    SpvEmitter e;
    spv_emit_init(&e, 2048);
    spv_emit_shader_capability(&e);
    spv_emit_capability(&e, 3); /* Tessellation */
    u32 f32 = spv_emit_type_float(&e, 32), u32_type = spv_emit_type_u32(&e);
    u32 boolean = spv_emit_type_bool(&e), vec4 = spv_emit_type_vector(&e, f32, 4);
    u32 input_ptr = spv_emit_type_pointer(&e, 1, vec4), output_ptr = spv_emit_type_pointer(&e, 3, vec4);
    u32 interfaces[70], in[33] = {0}, out[33] = {0}, count = 0;
    for (u32 i = 0; i < 33; i++) {
        if (i && !(attributes & (1u << (i - 1)))) continue;
        in[i] = spv_emit_interface_variable(&e, 1, vec4, u32_type, 32, i ? SPV_NOT_BUILTIN : 0, i ? i - 1 : 0, 0);
        out[i] =
            spv_emit_interface_variable(&e, 3, vec4, u32_type, vertices, i ? SPV_NOT_BUILTIN : 0, i ? i - 1 : 0, 0);
        interfaces[count++] = in[i];
        interfaces[count++] = out[i];
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

VkSh* vk_tess_passthrough(u32 vertices, const VkSh* vertex, const VkSh* evaluation) {
    if (!vertex || vertex->failed || !evaluation || evaluation->failed || !g_vk.feat.tessellationShader || !vertices ||
        vertices > 32 || vertices > g_vk.props.limits.maxTessellationPatchSize)
        return NULL;
    if (vertex->si.writes_layer || vertex->si.writes_viewport || vertex->si.writes_point_size ||
        vertex->si.uses_clip_mask) {
        LOG_LIMIT(2, "vulkan: evaluation-only tessellation needs unsupported per-vertex builtin forwarding\n");
        return NULL;
    }
    u32 attributes = vertex->si.out_mask & evaluation->si.in_mask;
    u32 components = 4 * (1 + snx_popcount64(attributes));
    const VkPhysicalDeviceLimits* limits = &g_vk.props.limits;
    if (components > limits->maxTessellationControlPerVertexInputComponents ||
        components > limits->maxTessellationControlPerVertexOutputComponents ||
        limits->maxTessellationControlPerPatchOutputComponents < 6 ||
        components * vertices + 6 > limits->maxTessellationControlTotalOutputComponents) {
        LOG_LIMIT(2, "vulkan: evaluation-only control shader exceeds the device's interface limits\n");
        return NULL;
    }
    for (u32 i = 0; i < g_tess_pass.count; i++)
        if (g_tess_pass.entries[i]->vertices == vertices && g_tess_pass.entries[i]->attributes == attributes)
            return &g_tess_pass.entries[i]->shader;
    if (g_tess_pass.count == g_tess_pass.capacity) {
        u32 capacity = g_tess_pass.capacity ? g_tess_pass.capacity * 2 : 16;
        if (capacity < g_tess_pass.capacity || (size_t)capacity > SIZE_MAX / sizeof *g_tess_pass.entries) return NULL;
        TessPass** grown = realloc(g_tess_pass.entries, (size_t)capacity * sizeof *grown);
        if (!grown) return NULL;
        g_tess_pass.entries = grown;
        g_tess_pass.capacity = capacity;
    }
    TessPass* made = calloc(1, sizeof *made);
    if (!made) return NULL;
    size_t words;
    u32* code = vk_tess_passthrough_spirv(vertices, attributes, &words);
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = code};
    if (!code || !VK_OK(vkCreateShaderModule(g_vk.dev, &info, NULL, &made->shader.mod))) {
        free(code);
        free(made);
        return NULL;
    }
    made->vertices = vertices;
    made->attributes = attributes;
    made->shader.stage = made->shader.si.stage = ST_TCS;
    made->shader.si.in_mask = made->shader.si.out_mask = attributes;
    u32 key[3] = {0x54435350, vertices, attributes};
    made->shader.hash = vk_hash(key, sizeof key);
    g_tess_pass.entries[g_tess_pass.count++] = made;
    free(code);
    return &made->shader;
}

void vk_tess_passthrough_free(void) {
    for (u32 i = 0; i < g_tess_pass.count; i++) {
        vkDestroyShaderModule(g_vk.dev, g_tess_pass.entries[i]->shader.mod, NULL);
        free(g_tess_pass.entries[i]);
    }
    free(g_tess_pass.entries);
    g_tess_pass.entries = NULL;
    g_tess_pass.count = g_tess_pass.capacity = 0;
}
