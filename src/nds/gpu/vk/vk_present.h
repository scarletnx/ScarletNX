/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NDS_VK_PRESENT_H
#define SNX_NDS_VK_PRESENT_H
#include <stdbool.h>
/* a point of the window, in its pixels, as the frame's pixel (256 by 384, the lower screen from row 192): false
   outside the frame */
bool nds_vk_present_point(float x, float y, int* frame_x, int* frame_y);
#endif
