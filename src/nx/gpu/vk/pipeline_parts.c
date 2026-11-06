/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Graphics pipeline libraries: the choice and the dynamic pre-rasterization state (pipeline_parts.h) */
#include "nx/gpu/vk/pipeline_parts.h"
#include <stdio.h>

int vk_gpl_choose(const VkGplDevice* d, u32* dyn_pr, char* line, size_t n) {
    /* a link without fast linking compiles, so it would save nothing; without independent interpolation the
       pre-rasterization and fragment parts must agree on decorations the translator doesn't promise, and
       vkd3d-proton leaves the extension off without it too */
    const char* off = !d->setting                     ? "pipeline_libraries=0"
                      : !d->has_ext                   ? "the device has no VK_EXT_graphics_pipeline_library"
                      : !d->library                   ? "the device has VK_EXT_graphics_pipeline_library without "
                                                        "graphicsPipelineLibrary"
                      : !d->fast_link                 ? "the device links pipeline libraries without fast linking"
                      : !d->independent_interpolation ? "the device has no independent interpolation decorations for "
                                                        "pipeline libraries"
                                                      : NULL;
    *dyn_pr = 0;
    if (off) {
        snprintf(line, n, "vulkan: graphics pipeline libraries off (%s): each pipeline compiles its shaders whole", off);
        return 0;
    }
    if (d->polygon) *dyn_pr |= VK_DYN_PR_POLYGON;
    if (d->depth_clamp) *dyn_pr |= VK_DYN_PR_DEPTH_CLAMP;
    if (d->provoking && d->has_provoking_last) *dyn_pr |= VK_DYN_PR_PROVOKING;
    if (d->conservative_mode && d->conservative_size && d->has_conservative) *dyn_pr |= VK_DYN_PR_CONSERVATIVE;
    if (d->patch) *dyn_pr |= VK_DYN_PR_PATCH;
    static const char* const names[5] = {"polygon mode", "depth clamp", "provoking vertex",
                                         "conservative rasterization", "patch control points"};
    char dyn[160];
    size_t at = 0;
    dyn[0] = 0;
    for (int i = 0; i < 5; i++)
        if ((*dyn_pr >> i & 1) && at < sizeof dyn)
            at += (size_t)snprintf(dyn + at, sizeof dyn - at, "%s%s", at ? ", " : "", names[i]);
    snprintf(line, n,
             "vulkan: graphics pipeline libraries on: a new pipeline links the parts of its shaders, each compiled "
             "once, and is optimized in the background; dynamic pre-rasterization state: %s",
             at ? dyn : "none");
    return 1;
}

/* every dynamic state a draw pipeline can have, in the order pipe_create always listed them (a pipeline cache entry
   depends on it), with the part that holds it (the specification's "Graphics Pipeline Library" says which state each
   part holds) and when it is there */
enum { DYN_ALWAYS, DYN_FEEDBACK, DYN_STRIDE, DYN_VERTEX_INPUT, DYN_PR, DYN_PR_TESS };
static const struct {
    VkDynamicState state;
    u32 part; /* VK_PART_* */
    u32 when; /* DYN_*: for DYN_PR and DYN_PR_TESS also the VK_DYN_PR_* bit, from bit 8 */
} g_dyn_states[] = {
    {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_CULL_MODE, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_FRONT_FACE, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_PART_VI, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_STENCIL_OP, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_PART_VI, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_LINE_WIDTH, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_BIAS, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_PART_FO, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_BOUNDS, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT, VK_PART_FS | VK_PART_FO, DYN_FEEDBACK},
    {VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_PART_VI, DYN_STRIDE},
    {VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_PART_VI, DYN_VERTEX_INPUT},
    {VK_DYNAMIC_STATE_POLYGON_MODE_EXT, VK_PART_PR, DYN_PR | VK_DYN_PR_POLYGON << 8},
    {VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT, VK_PART_PR, DYN_PR | VK_DYN_PR_DEPTH_CLAMP << 8},
    {VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT, VK_PART_PR, DYN_PR | VK_DYN_PR_PROVOKING << 8},
    {VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT, VK_PART_PR, DYN_PR | VK_DYN_PR_CONSERVATIVE << 8},
    {VK_DYNAMIC_STATE_EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT, VK_PART_PR, DYN_PR | VK_DYN_PR_CONSERVATIVE << 8},
    {VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT, VK_PART_PR, DYN_PR_TESS | VK_DYN_PR_PATCH << 8},
};
typedef char dyn_states_fit[sizeof g_dyn_states / sizeof *g_dyn_states <= VK_PIPE_DYN_MAX ? 1 : -1];

u32 vk_pipe_dynamic(u32 parts, const PipeDynIn* in, VkDynamicState* out) {
    u32 n = 0;
    for (u32 i = 0; i < sizeof g_dyn_states / sizeof *g_dyn_states; i++) {
        u32 when = g_dyn_states[i].when & 0xFF, bit = g_dyn_states[i].when >> 8;
        int on = when == DYN_ALWAYS         ? 1
                 : when == DYN_FEEDBACK     ? in->feedback_dynamic
                 : when == DYN_STRIDE       ? in->stride
                 : when == DYN_VERTEX_INPUT ? in->vertex_input
                 : when == DYN_PR           ? (in->dyn_pr & bit) != 0
                                            : (in->dyn_pr & bit) && in->tess;
        if (on && (g_dyn_states[i].part & parts)) out[n++] = g_dyn_states[i].state;
    }
    return n;
}
