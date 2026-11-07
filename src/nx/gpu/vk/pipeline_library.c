/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Pipelines linked from parts: the device's choice and the dynamic states' parts (pipeline_library.h) */
#include "nx/gpu/vk/pipeline_library.h"
#include <stdio.h>
#include "nx/gpu/vk/extension_bans.h"
#include "volk.h"

int vk_gpl_choose(const VkGplDevice* d, char* line, size_t n) {
    /* Without fast linking a link is a compile of the whole pipeline again, which takes what the parts saved. A
       fragment program's interpolation is the translation's own, which the stage before it doesn't repeat: a device
       that needs them to match can't take a part made apart from the other */
    const char* lacks =
        d->has == 0        ? "has no VK_EXT_graphics_pipeline_library"
        : d->has == 1      ? "has VK_EXT_graphics_pipeline_library without its graphicsPipelineLibrary feature"
        : !d->fast_linking ? "links pipeline libraries slowly (no graphicsPipelineLibraryFastLinking)"
        : !d->independent_interpolation ? "needs a fragment program's interpolation to match the stage before it (no "
                                          "graphicsPipelineLibraryIndependentInterpolationDecoration)"
                                        : NULL;
    char version[32];
    vk_driver_version_text(d->driver_id, d->driver_version, version, sizeof version);
    if (d->ban) { /* the table took the extension out of the device's list: has is 0, and the ban is why */
        snprintf(line, n,
                 "vulkan: new pipelines are made whole, a draw's first waiting for all of it: "
                 "VK_EXT_graphics_pipeline_library is banned on %s (%s, %s, driver version %s): %s",
                 d->device, d->driver_name, d->driver_info, version, d->ban);
        return 0;
    }
    if (!lacks) {
        snprintf(line, n,
                 "vulkan: new pipelines are linked from parts (VK_EXT_graphics_pipeline_library) on %s (%s, %s, driver "
                 "version %s): a draw's first waits for its programs' parts alone, made once for every pipeline "
                 "that shares them, %s",
                 d->device, d->driver_name, d->driver_info, version,
                 d->cpu ? "and the link stays, as fast to draw with on a CPU device as the pipeline made whole"
                        : "and the pipeline made whole replaces it in the background");
        return 1;
    }
    snprintf(
        line, n,
        "vulkan: new pipelines are made whole, a draw's first waiting for all of it: %s (%s, %s, driver version %s) "
        "%s",
        d->device, d->driver_name, d->driver_info, version, lacks);
    return 0;
}

u32 vk_gpl_dynamic_part(u32 state) {
    switch (state) {
        case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT:
        case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE:
        case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:
        case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
            return VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
        case VK_DYNAMIC_STATE_VIEWPORT:
        case VK_DYNAMIC_STATE_SCISSOR:
        case VK_DYNAMIC_STATE_LINE_WIDTH:
        case VK_DYNAMIC_STATE_DEPTH_BIAS:
        case VK_DYNAMIC_STATE_CULL_MODE:
        case VK_DYNAMIC_STATE_FRONT_FACE:
        case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT:
        case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT:
        case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE:
        case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:
        case VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT:
        case VK_DYNAMIC_STATE_LINE_STIPPLE:
        case VK_DYNAMIC_STATE_TESSELLATION_DOMAIN_ORIGIN_EXT:
        case VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT:
        case VK_DYNAMIC_STATE_POLYGON_MODE_EXT:
        case VK_DYNAMIC_STATE_RASTERIZATION_STREAM_EXT:
        case VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT:
        case VK_DYNAMIC_STATE_EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT:
        case VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT:
        case VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT:
        case VK_DYNAMIC_STATE_LINE_RASTERIZATION_MODE_EXT:
        case VK_DYNAMIC_STATE_LINE_STIPPLE_ENABLE_EXT:
        case VK_DYNAMIC_STATE_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT:
            return VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
        case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
        case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
        case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
        case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
        case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:
        case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
        case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP:
        case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE:
        case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:
        case VK_DYNAMIC_STATE_STENCIL_OP: return VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
        case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
        case VK_DYNAMIC_STATE_LOGIC_OP_EXT:
        case VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT:
        case VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT:
        case VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT:
        case VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT:
        case VK_DYNAMIC_STATE_SAMPLE_MASK_EXT:
        case VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT:
        case VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT:
        case VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT:
        case VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT:
        case VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT:
        case VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT:
        case VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_ENABLE_EXT:
        case VK_DYNAMIC_STATE_COLOR_BLEND_ADVANCED_EXT:
        case VK_DYNAMIC_STATE_COVERAGE_TO_COLOR_ENABLE_NV:
        case VK_DYNAMIC_STATE_COVERAGE_TO_COLOR_LOCATION_NV:
        case VK_DYNAMIC_STATE_COVERAGE_MODULATION_MODE_NV:
        case VK_DYNAMIC_STATE_COVERAGE_MODULATION_TABLE_ENABLE_NV:
        case VK_DYNAMIC_STATE_COVERAGE_MODULATION_TABLE_NV:
        case VK_DYNAMIC_STATE_COVERAGE_REDUCTION_MODE_NV:
            return VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
        default: return 0;
    }
}

const u32* vk_pipe_dynamic_states(u32* n) {
    static const u32 states[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
                                 VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
                                 VK_DYNAMIC_STATE_CULL_MODE,
                                 VK_DYNAMIC_STATE_FRONT_FACE,
                                 VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                                 VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                 VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                 VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                 VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
                                 VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                                 VK_DYNAMIC_STATE_STENCIL_OP,
                                 VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
                                 VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
                                 VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
                                 VK_DYNAMIC_STATE_LINE_WIDTH,
                                 VK_DYNAMIC_STATE_DEPTH_BIAS,
                                 VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                 VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                 VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                 VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                 VK_DYNAMIC_STATE_DEPTH_BOUNDS};
    *n = (u32)(sizeof states / sizeof *states);
    return states;
}
