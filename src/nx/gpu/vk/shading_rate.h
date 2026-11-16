/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Ounce's shading rate image on the device (shading_rate.c) */
#ifndef SNX_VK_SHADING_RATE_H
#define SNX_VK_SHADING_RATE_H
#include <stddef.h>
#include "nx/gpu/vk/vk.h"

/* what the device has of the two extensions, as device.c reads it: a feature that is on, and their properties */
typedef struct {
    int nv;                /* VK_NV_shading_rate_image's shadingRateImage */
    VkExtent2D nv_texel;   /* shadingRateTexelSize */
    u32 nv_palette;        /* shadingRatePaletteSize */
    int khr;               /* VK_KHR_fragment_shading_rate's attachment and pipeline rates */
    VkExtent2D khr_min, khr_max; /* the attachment's texel sizes */
    u32 khr_aspect;        /* maxFragmentShadingRateAttachmentTexelSizeAspectRatio */
} VkVrsDevice;
/* VK_VRS_* for g_vk.vrs, and the texel size of the device's image in *texel. line: the log line that says which */
int vk_vrs_choose(const VkVrsDevice* d, VkExtent2D* texel, char* line, size_t n);
/* Renderer.shading_rate: the game's image converted for the draws (an id for DrawCall.shading_rate, 0 for none) */
u32 vk_shading_rate(const GpuShadingRate* r);
/* VK_VRS_NV: the image and the palettes of a draw of id bound in cb, when the command buffer has not got them. A
   draw pipeline (vk_pipeline_get) needs them in each command buffer: its palettes are dynamic state */
void vk_vrs_draw(VkCommandBuffer cb, u32 id);
/* the same for a draw of the renderer's own (a clear): what the command buffer has bound, else no image */
void vk_vrs_keep(VkCommandBuffer cb);
/* VK_VRS_KHR: the attachment of a pass of area for the draws of id, VK_NULL_HANDLE for none */
VkImageView vk_vrs_attachment(u32 id, VkExtent2D area);
/* a draw pipeline's state for the image, which vk_vrs_pipeline chains into the viewport state (VK_VRS_NV) or after
   the tail of the create info (VK_VRS_KHR), and the flags that it adds */
typedef struct {
    VkPipelineViewportShadingRateImageStateCreateInfoNV nv;
    VkPipelineFragmentShadingRateStateCreateInfoKHR khr;
} VkVrsPipe;
void vk_vrs_pipeline(VkVrsPipe* o, VkPipelineViewportStateCreateInfo* vp, VkPipelineCreationFeedbackCreateInfo* tail);
VkPipelineCreateFlags vk_vrs_pipeline_flags(void);
/* the conversions since the last call, for the stats lines */
void vk_vrs_log(void);
#endif
