/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A layer's rectangles (common/utilities/ui_rects.h: the on-screen controls) over a window's image on a Vulkan device
   with dynamic rendering: an instance each, from a buffer the CPU maps, in the rendering that drew the picture or in
   one of their own after the transfers that drew it */
#ifndef SNX_COMMON_GPU_VK_RECTS_H
#define SNX_COMMON_GPU_VK_RECTS_H
#include "common/utilities/ui_rects.h"
#include <volk.h>

typedef struct SnxVkRects SnxVkRects;
/* one per window, with slots buffers: one a recording slot, its own until that slot's submission is done (the ring
   waits for it before the slot records again). NULL when out of memory */
SnxVkRects* snx_vk_rects_create(VkPhysicalDevice phys, VkDevice dev, uint32_t slots);
void snx_vk_rects_destroy(SnxVkRects* vr);
/* rects drawn into the rendering open on cb, after the picture's draws there: its one colour attachment is the window's
   image, width by height, of format B8G8R8A8 or R8G8B8A8 UNORM (another is said through log once and draws nothing). It
   binds a pipeline and a vertex buffer of its own. 1 when drawn; 0 when there is none (NULL), or its buffer or pipeline
   could not be made (said through log) */
int snx_vk_rects_draw(SnxVkRects* vr, VkCommandBuffer cb, uint32_t slot, VkFormat format, uint32_t width,
                      uint32_t height, const SnxUiRects* rects, void (*log)(const char* text));
/* the same over image (view its view), in a rendering of their own on cb after the transfers written to it: image is in
   TRANSFER_DST_OPTIMAL and stays so */
int snx_vk_rects_record(SnxVkRects* vr, VkCommandBuffer cb, uint32_t slot, VkImage image, VkImageView view,
                        VkFormat format, uint32_t width, uint32_t height, const SnxUiRects* rects,
                        void (*log)(const char* text));
#endif
