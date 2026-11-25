/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Motion vectors made from depth (up_reproject.comp), for a caller that has a camera and no motion vectors of its own:
   a matrix takes a render pixel from this frame to where it was in the frame before (reproject.c's comment), and the
   motion vectors image holds the difference at the render size, in picture coordinates (0 to 1), as the upscalers
   take them with a motion vector scale of the render size. The camera's motion alone */
#ifndef SNX_NX_GPU_VK_UPSCALER_REPROJECT_H
#define SNX_NX_GPU_VK_UPSCALER_REPROJECT_H
#include "nx/gpu/vk/upscaler/upscaler.h"

typedef struct SnxReproject SnxReproject;
/* an image of up to render_max[0] x render_max[1]: NULL and why when the device refuses something */
SnxReproject* snx_reproject_create(VkDevice device, VkPhysicalDevice physical, const uint32_t render_max[2],
                                   const char** why);
/* the largest render size it takes */
void snx_reproject_size(const SnxReproject* r, uint32_t out[2]);
/* Records the pass on cb: depth sampled (in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), reproject the row-major matrix
   from (x, y, depth, 1) of this frame to the frame before, with x and y the picture's coordinates as -1 to 1, y down.
   The motion vectors image goes in *motion, in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL once the pass is done */
void snx_reproject_record(SnxReproject* r, VkCommandBuffer cb, const SnxUpscaleImage* depth, const uint32_t render[2],
                          const float reproject[16], SnxUpscaleImage* motion);
void snx_reproject_destroy(SnxReproject* r);
#endif
