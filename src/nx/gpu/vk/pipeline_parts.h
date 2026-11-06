/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Graphics pipeline libraries (VK_EXT_graphics_pipeline_library, docs/PIPELINE_LIBRARIES.md): whether the renderer
   makes its pipelines from them, and which pre-rasterization state its draws then set as dynamic state. Pure
   functions of what the device has, with no GPU, so a test checks each case */
#ifndef SNX_VK_PIPELINE_PARTS_H
#define SNX_VK_PIPELINE_PARTS_H
#include <stddef.h>
#include "common/cpu/aarch64/cpu.h"

/* the pre-rasterization state a pipeline made of libraries takes as dynamic state (g_vk.dyn_pr), so that one
   pre-rasterization part serves every value of it: a bit each */
#define VK_DYN_PR_POLYGON      1u  /* polygon mode: extendedDynamicState3PolygonMode */
#define VK_DYN_PR_DEPTH_CLAMP  2u  /* depth clamp: extendedDynamicState3DepthClampEnable */
#define VK_DYN_PR_PROVOKING    4u  /* provoking vertex: extendedDynamicState3ProvokingVertexMode */
#define VK_DYN_PR_CONSERVATIVE 8u  /* conservative rasterization and its dilation: the two EDS3 members */
#define VK_DYN_PR_PATCH        16u /* patch control points: extendedDynamicState2PatchControlPoints */

/* what a device has, as the choice reads it */
typedef struct {
    int setting;   /* pipeline_libraries */
    int has_ext;   /* VK_EXT_graphics_pipeline_library and VK_KHR_pipeline_library */
    int library;   /* graphicsPipelineLibrary */
    int fast_link; /* graphicsPipelineLibraryFastLinking */
    int independent_interpolation; /* graphicsPipelineLibraryIndependentInterpolationDecoration */
    /* the members of VK_EXT_extended_dynamic_state3 and 2 the parts can take (0 without the extension) */
    int polygon, depth_clamp, provoking, conservative_mode, conservative_size, patch;
    /* what the renderer uses at all: a dynamic state of a feature it doesn't use is left out */
    int has_provoking_last, has_conservative;
} VkGplDevice;

/* whether pipelines are made from libraries on d: 1 or 0, the dynamic pre-rasterization state (VK_DYN_PR_*) in
   *dyn_pr (0 when they aren't), and the log line that says so into line */
int vk_gpl_choose(const VkGplDevice* d, u32* dyn_pr, char* line, size_t n);
#endif
