/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Ounce's shading rate image on the device (shading_rate.c) */
#ifndef SNX_VK_SHADING_RATE_H
#define SNX_VK_SHADING_RATE_H
#include <stddef.h>
#include "nx/gpu/vk/vk.h"

/* what the device has of the two extensions, as device.c reads it: a feature that is on, and their properties */
typedef struct {
    /* the game is an Ounce program (g_ounce): no other has a shading rate image, so the others keep their pipelines as
       they were, with neither extension on */
    int ounce;
    int nv;                /* VK_NV_shading_rate_image's shadingRateImage */
    VkExtent2D nv_texel;   /* shadingRateTexelSize */
    u32 nv_palette;        /* shadingRatePaletteSize */
    int khr;               /* VK_KHR_fragment_shading_rate's attachment and pipeline rates */
    VkExtent2D khr_min, khr_max; /* the attachment's texel sizes */
    u32 khr_aspect;        /* maxFragmentShadingRateAttachmentTexelSizeAspectRatio */
} VkVrsDevice;
/* VK_VRS_* for g_vk.vrs, and the texel size of the device's image in *texel. line: the log line that says which
   (shading_rate_table.c) */
int vk_vrs_choose(const VkVrsDevice* d, VkExtent2D* texel, char* line, size_t n);
/* a palette entry (VkShadingRatePaletteEntryNV's numbers) as VK_KHR_fragment_shading_rate encodes it: log2 of the
   width in bits 2-3, of the height in bits 0-1. 1x1 for 2 to 16 invocations for each pixel, 4x4 for none */
u32 vk_vrs_khr_rate(u32 e);
/* the conversion's push constants (shading_rate.comp's Params, in its order): the device's image of texel pixels a
   texel for the game's game_w x game_h texels of game_texel pixels, and each palette entry's value for vrs
   (VK_VRS_NV: the index, VK_VRS_KHR: viewport 0's rate from palette0). finest: VK_VRS_KHR, where a texel of the
   device's image takes the finest rate of the game's texels under it, else the one under its center */
typedef struct {
    u32 size_x, size_y, game_w, game_h, texel_w, texel_h, game_texel, finest;
    u32 table[4];
} VkVrsPush;
void vk_vrs_push(int vrs, VkExtent2D texel, u32 game_w, u32 game_h, u32 game_texel, u64 palette0, VkVrsPush* o);
/* the device's pixels that a texel of the game's image covers: the game's texel (texel pixels) at the resolution
   scale, over the texels of the image the renderer holds (sw of them for the guest's w: a scaled image where no copy
   of the guest's size could be made). 1 at least */
u32 vk_vrs_game_texel(u32 texel, float scale, u32 w, u32 sw);
/* what a new palette or image (vk_shading_rate) needs: VK_VRS_CONVERT, a conversion of the image; VK_VRS_NEW_ID, the
   image as it is with a new id, whose draws set the new palettes (VK_VRS_NV); VK_VRS_SAME_ID, nothing, as viewport 0's
   palette, the only one the image holds, is the same (VK_VRS_KHR). valid: the last conversion made the image.
   palette0_was and palette0: viewport 0's palette that the image holds and the new one */
enum { VK_VRS_CONVERT, VK_VRS_NEW_ID, VK_VRS_SAME_ID };
int vk_vrs_reuse(int vrs, int new_image, int valid, u64 palette0_was, u64 palette0);
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
