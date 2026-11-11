/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/hdr_swapchain.h"
#include "common/gpu/vk/swapchain.h"
#include <math.h>

int vk_hdr_surface_format(u32 space, const VkSurfaceFormatKHR* formats, u32 n, VkSurfaceFormatKHR* out) {
    static const VkFormat pq[] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32,
                                  VK_FORMAT_R16G16B16A16_SFLOAT};
    static const VkFormat scrgb[] = {VK_FORMAT_R16G16B16A16_SFLOAT};
    switch (space) {
        case SNX_HDR_SPACE_PQ:
            /* in this order, whatever the surface's: snx_vk_pick_format takes the surface's first of several */
            for (u32 i = 0; i < 3; i++)
                if (snx_vk_pick_format(formats, n, &pq[i], 1, VK_COLOR_SPACE_HDR10_ST2084_EXT, out)) return 1;
            return 0;
        case SNX_HDR_SPACE_SCRGB:
            return snx_vk_pick_format(formats, n, scrgb, 1, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, out);
        default: return 0;
    }
}

int vk_hdr_surface_takes(const VkSurfaceFormatKHR* formats, u32 n) {
    VkSurfaceFormatKHR found;
    return vk_hdr_surface_format(SNX_HDR_SPACE_PQ, formats, n, &found) &&
           vk_hdr_surface_format(SNX_HDR_SPACE_SCRGB, formats, n, &found);
}

int vk_hdr_display(int setting, int has_colorspace, int has_metadata, const VkSurfaceFormatKHR* formats, u32 n,
                   const char** why) {
    VkSurfaceFormatKHR found;
    *why = !setting          ? "hdr_game=0"
           : !has_colorspace ? "the Vulkan instance has no VK_EXT_swapchain_colorspace"
           : !has_metadata   ? "the device has no VK_EXT_hdr_metadata"
           : !vk_hdr_surface_format(SNX_HDR_SPACE_PQ, formats, n, &found)
               ? "the window's surface lists no HDR10 (ST 2084) format of 10 bits or half floats"
           : !vk_hdr_surface_format(SNX_HDR_SPACE_SCRGB, formats, n, &found)
               ? "the window's surface lists no scRGB (extended linear sRGB) half float format"
               : NULL;
    return !*why;
}

int vk_hdr_metadata(const SnxHdrFrame* frame, VkHdrMetadataEXT* out) {
    const f32* m = frame->metadata;
    for (u32 i = 0; i < 12; i++)
        if (!isfinite(m[i]) || m[i] < 0 || (i < 8 && m[i] > 1)) return 0;
    if (m[9] <= m[8]) return 0;
    *out = (VkHdrMetadataEXT){.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
                              .displayPrimaryRed = {m[0], m[1]},
                              .displayPrimaryGreen = {m[2], m[3]},
                              .displayPrimaryBlue = {m[4], m[5]},
                              .whitePoint = {m[6], m[7]},
                              .maxLuminance = m[9],
                              .minLuminance = m[8],
                              .maxContentLightLevel = m[10],
                              .maxFrameAverageLightLevel = m[11]};
    return 1;
}

int vk_hdr_metadata_set(VkDevice device, VkSwapchainKHR swapchain, const SnxHdrFrame* frame) {
    VkHdrMetadataEXT metadata;
    if (!vk_hdr_metadata(frame, &metadata)) return 0;
    vkSetHdrMetadataEXT(device, 1, &swapchain, &metadata);
    return 1;
}
