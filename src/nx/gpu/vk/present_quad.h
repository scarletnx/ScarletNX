/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_PRESENT_QUAD_H
#define SNX_VK_PRESENT_QUAD_H
#include "nx/gpu/vk/vk.h"
/* Draw a crop into an already cleared color target. Android transforms apply flips before a clockwise quarter turn.
   The GPU samples and converts the image. No host pixel conversion or image readback occurs here. */
int vk_present_quad(Surf* source, VkImage destination, VkFormat format, VkExtent2D extent,
                    VkRect2D source_rect, VkRect2D destination_rect, u32 transform, int linear);
#endif
