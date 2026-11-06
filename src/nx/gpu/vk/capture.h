/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_CAPTURE_H
#define SNX_VK_CAPTURE_H
#include "nx/gpu/vk/vk.h"

/* Host-only scratch attachments. They must not enter the guest surface cache or vk_rt_drawn/writeback lists.
   Capture the original shader once per primitive, then resolve before starting the next primitive. */
typedef struct {
    Surf source, valid;
    Surf* target;
    u32 flags;
    VkBuffer tile_buffer;
    VmaAllocation tile_memory;
    u64 tile_bytes;
    u32 tile_count;
    int tile_dirty; /* a capture not followed by a recorded resolve requires a full flag clear */
} VkCapture;

/* Called on the GPU thread under vk_lock. A different shape retires the earlier images with the frame. */
VkCapture* vk_capture_get(Surf* target);
int vk_capture_tiles_supported(void);
/* Optional GPU-only adaptive header pass; ordinary tiled resolves remain the default. */
int vk_capture_finalize_tiles(VkCapture* capture);
int vk_capture_clear(VkCapture* capture);
/* coverage0: advanced equation; coverage1: integer post-test coverage mask, with target signedness from its format. */
int vk_capture_resolve(VkCapture* capture, u64 blend, u32 flags, int coverage);
/* After submitted work is idle, before the Vulkan device and command pools are destroyed. */
void vk_capture_destroy(void);
#endif
