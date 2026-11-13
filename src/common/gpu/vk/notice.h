/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A notice (common/utilities/notice.h) over a window's image on a Vulkan device, for the 3DS's and the Vita's windows:
   its band's pixels written to a buffer the CPU maps and copied over the picture, after the copies that drew it */
#ifndef SNX_COMMON_GPU_VK_NOTICE_H
#define SNX_COMMON_GPU_VK_NOTICE_H
#include "common/utilities/notice.h"
#include <volk.h>

typedef struct SnxVkNotice SnxVkNotice;
/* one per window, with slots buffers: one a recording slot, its own until that slot's submission is done (the ring
   waits for it before the slot records again). NULL when out of memory */
SnxVkNotice* snx_vk_notice_create(VkPhysicalDevice phys, VkDevice dev, uint32_t slots);
void snx_vk_notice_destroy(SnxVkNotice* vn);
/* n, when it shows at now_ns, over image (a picture width by height whose display is at percent of its pixels,
   snx_notice_scale's), recorded on cb in slot after the transfers written to image before it: image is in
   TRANSFER_DST_OPTIMAL and stays so. format is the window's, B8G8R8A8 or R8G8B8A8, UNORM or SRGB, whose bytes the band
   is written in; another is said through log once and draws nothing. 1 when it was drawn; 0 when none shows, the band
   is off the picture or its buffer could not be made (said through log) */
int snx_vk_notice_record(SnxVkNotice* vn, VkCommandBuffer cb, uint32_t slot, VkImage image, VkFormat format,
                         uint32_t width, uint32_t height, int percent, const SnxNotice* n, uint64_t now_ns,
                         void (*log)(const char* text));
#endif
