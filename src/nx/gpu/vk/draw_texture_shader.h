/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_DRAW_TEXTURE_SHADER_H
#define SNX_VK_DRAW_TEXTURE_SHADER_H
#include "nx/gpu/vk/vk.h"
/* The vertex shader reads rectangle and normalized texture corners at push offsets80 and96. */
VkSh* vk_draw_texture_vertex(void);
VkSh* vk_draw_texture_fragment(u32 texture_int, u32 texture_signed, u32 target_int, u32 target_signed,
                                u64 advanced_blend, u32 advanced_mask);
/* The first draw of a feedback draw drawn two times (render.c predraw_run): for each of count textures, TLD.LZ at
   the fragment's own pixel, out to the k-th color slot of slots from the lowest. The texture slot of k has its
   handle at word SNX_PREDRAW_HANDLE + k, which the renderer replaces by the target. Bit k of ints and sints:
   integer textures and targets */
#define SNX_PREDRAW_HANDLE 0x10
VkSh* vk_predraw_fragment(u32 count, u32 slots, u32 ints, u32 sints);
#endif
