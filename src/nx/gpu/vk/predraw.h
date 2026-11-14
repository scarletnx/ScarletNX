/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The images of the pre-draw capture (ShaderInfo.predraw_slots), in predraw.c */
#ifndef SNX_VK_PREDRAW_H
#define SNX_VK_PREDRAW_H
#include "nx/gpu/vk/vk.h"

/* Images of at least sw x sh for a draw that captures sides slots, outside any pass when they are made again (their
   stamps cleared). 0 when they can't be made: the draw's slots read a copy */
int vk_predraw_reserve(u32 sw, u32 sh, u32 sides);
/* a capturing draw's number: the one before while no texture barrier came between (g_vk_texture_epoch) and the draw
   captures the same targets the same way, by side image (targets[k], n of them, each read as views[k] says: its
   header's swizzle and its sRGB reading, render.c), else one no stamp holds yet */
u32 vk_predraw_next(const Surf* const* targets, const u32* views, u32 n);
/* the stamp image's view (0) or side image k - 1's, in GENERAL */
VkImageView vk_predraw_view(u32 k);
/* a capturing draw was recorded: its stores reach the next pass's draws through vk_predraw_pass_begin */
void vk_predraw_drawn(void);
/* before a rendering pass begins on cb: makes the stores of the passes before visible to its draws */
void vk_predraw_pass_begin(VkCommandBuffer cb);
#endif
