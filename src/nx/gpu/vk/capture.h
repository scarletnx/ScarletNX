/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_CAPTURE_H
#define SNX_VK_CAPTURE_H
#include "nx/gpu/vk/vk.h"

/* Host-only scratch attachments. They must not enter the guest surface cache or vk_rt_drawn/writeback lists.
   Capture the original shader once per primitive, then resolve before starting the next primitive. */
typedef struct {
    Surf source;
    Surf* target;
    VkDescriptorSet mixed_set;
    u64 mixed_frame;
} VkCaptureExtra;
typedef struct {
    Surf source, valid;
    Surf* target;
    u32 flags;
    VkImageUsageFlags usage; /* sampled mixed scratch and storage advanced scratch must never alias their cache */
    VkDescriptorSet mixed_set; /* immutable source/valid views, never updated after their first use */
    u64 mixed_frame; /* pool lifetime: compare only after vk_cmd has started the current frame */
    u32 mixed_count; /* source0 keeps the fields above; source i>0 is extra[i-1] */
    VkCaptureExtra extra[6]; /* one fixed allocation owned by the capture cache, with one shared validity image */
    VkBuffer tile_buffer;
    DevMem tile_memory;
    u64 tile_bytes;
    u32 tile_count;
    int tile_dirty; /* a capture not followed by a recorded resolve requires a full flag clear */
} VkCapture;

/* Properties must describe COLOR_ATTACHMENT|SAMPLED|TRANSFER_DST. Only red records validity; retain RGBA8 when
   R8 cannot represent this sample count, extent or layer count. This choice does not estimate allocation size. */
static inline VkFormat vk_capture_validity_format(const VkImageFormatProperties* r8, u32 samples, u32 width,
                                                   u32 height, u32 layers) {
    return r8 && samples && !(samples & (samples - 1)) && width && height && layers && r8->maxMipLevels &&
           (r8->sampleCounts & samples) && width <= r8->maxExtent.width && height <= r8->maxExtent.height &&
           layers <= r8->maxArrayLayers ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
}

/* Called on the GPU thread under vk_lock. A different shape retires the earlier images with the frame. */
VkCapture* vk_capture_get(Surf* target);
/* Color-only portable mixed coverage: one float/normalized 2D target, one stored sample, N raster samples.
   The caller must prove the guest shader's per-pixel source invariant across host sample invocations. */
VkCapture* vk_capture_get_mixed(Surf* target, u32 raster_samples);
/* Contiguous C1 targets, equal extents and one layer, with distinct image/storage ranges. The validity output
   occupies attachment count, so at most min(7,maxColorAttachments-1) sources fit. */
VkCapture* vk_capture_get_mixed_targets(Surf* const* targets, u32 count, u32 raster_samples);
Surf* vk_capture_mixed_source(VkCapture* capture, u32 index);
Surf* vk_capture_mixed_target(VkCapture* capture, u32 index);
/* An absent output leaves its destination untouched. Active partial declarations need a separate proof of
   source channels consumed by blending; do not resolve an undefined scratch component in this initial path. */
static inline int vk_capture_mixed_outputs_supported(u32 outputs, u32 count, const DsBlend* blend) {
    if (!blend || !count || count > 7 || (outputs >> (count * 4))) return 0;
    for (u32 i = 0; i < count; i++) {
        u32 declared = outputs >> (i * 4) & 15;
        if ((declared & blend->mask[i]) && declared != 15) return 0;
    }
    return 1;
}
/* Native one-sample final pass. Guest tests/sample masks already produced source/validity at N samples. Preserve
   ordinary blending, logic, component masks and sRGB conversion; reject dual-source state. Ends its own pass and
   invalidates the renderer's cached dynamic state. Call once per primitive before capturing the next one. */
int vk_capture_mixed_resolve(VkCapture* capture, const VkCoverage* coverage, const DsState* state, u32 outputs);
int vk_capture_tiles_supported(void);
/* Optional GPU-only adaptive header pass; ordinary tiled resolves remain the default. */
int vk_capture_finalize_tiles(VkCapture* capture);
int vk_capture_clear(VkCapture* capture);
/* coverage0: advanced equation; coverage1: integer post-test coverage mask, with target signedness from its format. */
int vk_capture_resolve(VkCapture* capture, u64 blend, u32 flags, int coverage);
/* After submitted work is idle, before the Vulkan device and command pools are destroyed. */
void vk_capture_destroy(void);
#endif
