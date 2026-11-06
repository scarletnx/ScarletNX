/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Graphics pipeline libraries (VK_EXT_graphics_pipeline_library, docs/PIPELINE_LIBRARIES.md): whether the renderer
   makes its pipelines from them, and which pre-rasterization state its draws then set as dynamic state. Pure
   functions of what the device has, with no GPU, so a test checks each case */
#ifndef SNX_VK_PIPELINE_PARTS_H
#define SNX_VK_PIPELINE_PARTS_H
#include <stddef.h>
#include "common/cpu/aarch64/cpu.h"
#include "volk.h"
#include "nx/gpu/vk/vk.h"

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

/* the parts of a pipeline made of libraries, a bit each (docs/PIPELINE_LIBRARIES.md, "The four parts") */
#define VK_PART_VI  1u  /* vertex input interface */
#define VK_PART_PR  2u  /* pre-rasterization shaders */
#define VK_PART_FS  4u  /* fragment shader */
#define VK_PART_FO  8u  /* fragment output interface */
#define VK_PART_ALL 15u /* a whole pipeline */
#define VK_PIPE_DYN_MAX 32 /* the most dynamic states a draw pipeline has */
/* what decides a draw pipeline's dynamic states: dyn_pr (VK_DYN_PR_*), the feedback loop's dynamic state, a binding
   stride (a vertex layout with bindings and has_dynamic_stride), dynamic vertex input and tessellation */
typedef struct {
    u32 dyn_pr;
    int feedback_dynamic, stride, vertex_input, tess;
} PipeDynIn;
/* the dynamic states of the parts in parts (VK_PART_*) into out, in the order a whole pipeline lists them: how many */
u32 vk_pipe_dynamic(u32 parts, const PipeDynIn* in, VkDynamicState* out);
/* ---- the four parts of a pipeline (docs/PIPELINE_LIBRARIES.md, "The four parts") */
/* the bits of PipeKey.arrays (a bit per set, shader_vk_set) of the fragment stage and of the stages before it */
#define VK_ARRAYS_FS 2u
#define VK_ARRAYS_PR 29u
/* each part's key, made by vk_pipe_split from a whole pipeline's: zeroed first and compared with memcmp, so no padding
   (u32 and handle members only) */
typedef struct {
    u32 topo_class;
    VertexLayout vi;
} PipeViKey;
typedef struct {
    VkShaderModule vs, tcs, tes, gs;
    u32 arrays; /* PipeKey.arrays' bits of the vertex, tessellation and geometry sets */
    /* 0 where dyn_pr makes them dynamic */
    u32 patch, polygon, depth_clamp, provoking_last, conservative;
} PipePrKey;
typedef struct {
    VkShaderModule fs; /* the fragment program, or the empty one */
    u32 arrays;        /* PipeKey.arrays' bit of the fragment set */
    u32 feedback;      /* 0 with the feedback loop's dynamic state */
    /* with sample shading, the multisample state the fragment output part has too (they must be the same) */
    u32 sample_shading, samples, sample_off, alpha_to_coverage, alpha_to_one;
    VkCoverage coverage;
} PipeFsKey;
typedef struct {
    u32 ncolor, color[8], depth, stencil;
    struct {
        u32 en, sc, dc, cop, sa, da, aop, mask;
    } blend[8]; /* the write masks 0 without a fragment program, as its outputs are undefined */
    u32 logic_op, samples, sample_off, alpha_to_coverage, alpha_to_one, sample_shading, feedback;
    VkCoverage coverage;
} PipeFoKey;
/* the device state the split reads, g_vk.dyn_pr and has_feedback_dynamic, as arguments so a test gives its own */
typedef struct {
    u32 dyn_pr;
    int feedback_dynamic;
} PipeSplitDevice;
/* k's parts: fs is the fragment program the pipeline gets (k->fs, or the empty one for none) */
void vk_pipe_split(const PipeKey* k, VkShaderModule fs, const PipeSplitDevice* d, PipeViKey* vi, PipePrKey* pr,
                   PipeFsKey* f, PipeFoKey* fo);
#endif
