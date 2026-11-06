/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_DRAW_TEXTURE_SHADER_H
#define SNX_VK_DRAW_TEXTURE_SHADER_H
#include "nx/gpu/vk/vk.h"
/* The vertex shader reads rectangle and normalized texture corners at push offsets80 and96. */
VkSh* vk_draw_texture_vertex(void);
VkSh* vk_draw_texture_fragment(u32 texture_int, u32 texture_signed, u32 target_int, u32 target_signed,
                                u64 advanced_blend, u32 advanced_mask);
#endif
