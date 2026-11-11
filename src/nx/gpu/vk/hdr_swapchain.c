/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/hdr_swapchain.h"
#include "common/gpu/vk/swapchain.h"

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
