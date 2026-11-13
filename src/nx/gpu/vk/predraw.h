/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The images of the pre-draw capture (ShaderInfo.predraw_slots), in predraw.c */
#ifndef SNX_VK_PREDRAW_H
#define SNX_VK_PREDRAW_H
#include "nx/gpu/vk/vk.h"

/* Images of at least sw x sh for a draw that captures sides slots, outside any pass when they are made again (their
   stamps cleared). 0 when they can't be made: the draw's slots read a copy */
int vk_predraw_reserve(u32 sw, u32 sh, u32 sides);
/* the next capturing draw's number, which no stamp holds yet */
u32 vk_predraw_next(void);
/* the stamp image's view (0) or side image k - 1's, in GENERAL */
VkImageView vk_predraw_view(u32 k);
/* a capturing draw was recorded: its stores reach the next pass's draws through vk_predraw_pass_begin */
void vk_predraw_drawn(void);
/* before a rendering pass begins on cb: makes the stores of the passes before visible to its draws */
void vk_predraw_pass_begin(VkCommandBuffer cb);
#endif
