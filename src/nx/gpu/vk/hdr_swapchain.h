/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The window's image for a game's HDR frames (gpu/hdr_output.h): which of a surface's formats takes them as they are.
   Nothing here asks the device, so tests/hdr_output.c holds each choice to a list of its own */
#ifndef SNX_VK_HDR_SWAPCHAIN_H
#define SNX_VK_HDR_SWAPCHAIN_H
#include <volk.h>
#include "nx/gpu/hdr_output.h"

/* The swapchain image for frames of space, from the formats a surface lists: HDR10's is 10 bits a channel, else half
   floats, in VK_COLOR_SPACE_HDR10_ST2084_EXT; scRGB's is half floats in VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, whose
   1.0 is 80 nits as the console's is. The present then writes the frame's values to it unchanged. 1 and *out it; 0
   with *out untouched where the surface lists none of them, and for SNX_HDR_SPACE_SRGB, whose image is the renderer's
   own choice */
int vk_hdr_surface_format(u32 space, const VkSurfaceFormatKHR* formats, u32 n, VkSurfaceFormatKHR* out);
/* 1 where the surface takes both HDR spaces: a game is told the display is HDR before it says which one it draws */
int vk_hdr_surface_takes(const VkSurfaceFormatKHR* formats, u32 n);
#endif
