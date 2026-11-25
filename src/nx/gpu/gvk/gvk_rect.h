/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's rectangles at the resolution's scale (gvk_api.c). Not in gvk.h: the kernel and the runtime include that one,
   and their tests are built without Vulkan's headers */
#ifndef SNX_GVK_RECT_H
#define SNX_GVK_RECT_H
#include "common/cpu/aarch64/cpu.h"
#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* r at f times its size: an extent may reach INT32_MAX from its offset (a game's scissor of everything), and stays
   there */
static inline VkRect2D gvk_rect_at(VkRect2D r, u32 f) {
    int64_t x = (int64_t)r.offset.x * f, y = (int64_t)r.offset.y * f;
    int64_t w = (int64_t)r.extent.width * f, h = (int64_t)r.extent.height * f;
    x = x < INT32_MIN ? INT32_MIN : x > INT32_MAX ? INT32_MAX : x;
    y = y < INT32_MIN ? INT32_MIN : y > INT32_MAX ? INT32_MAX : y;
    int64_t wmax = INT32_MAX - (x > 0 ? x : 0), hmax = INT32_MAX - (y > 0 ? y : 0);
    return (VkRect2D){{(i32)x, (i32)y}, {(u32)(w < wmax ? w : wmax), (u32)(h < hmax ? h : hmax)}};
}

/* whether any of the n regions of a copy between a buffer and an image moves its depth */
static inline int gvk_copies_depth(const VkBufferImageCopy* r, u32 n) {
    for (u32 i = 0; r && i < n; i++)
        if (r[i].imageSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) return 1;
    return 0;
}

#endif
