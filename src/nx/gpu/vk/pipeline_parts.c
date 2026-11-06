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
