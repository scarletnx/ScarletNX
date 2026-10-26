/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The spatial upscaler of the presented frame. FSR 1, NIS and SGSR take the color image's shown crop and write one
   image at the screen rectangle's size, which the presenter blits 1:1; None and Bilinear leave the frame's blit as it
   was. Auto picks by the device's vendor and runs only above 1.3x (1.25x to stop, 1.35x to start again), so a game at
   1:1 pays nothing. Under vk_lock, in the frame being recorded. */
#ifndef SNX_GPU_VK_UPSCALE_H
#define SNX_GPU_VK_UPSCALE_H
#include "nx/gpu/vk/upscale_select.h"
#include "nx/gpu/vk/vk.h"

/* the setting and its sharpness, read once at start */
void vk_upscale_configure(int filter, int sharpness);
/* the filter the frame runs: None or Bilinear when it runs none, Auto resolved; -1 for a frame with no source */
int vk_upscale_pick(u32 src_w, u32 src_h, u32 dst_w, u32 dst_h, u32 transform);
/* the filter's shown source to the output extent: 1 with out and out_view set, 0 when it could not run. The source
   is in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; the output is left in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL */
int vk_upscale_process(VkCommandBuffer cb, VkImageView src, u32 image_w, u32 image_h, u32 x, u32 y, u32 w, u32 h,
                       u32 transform, u32 dst_w, u32 dst_h, int filter, int sharpness, VkImage* out,
                       VkImageView* out_view);
/* the image format vk_upscale_process writes (RGBA16F, or RGBA8 where the device has no 16-bit one) */
VkFormat vk_upscale_format(void);
/* whether a filter's sharpness applies, for the menu's greyed row */
int vk_upscale_filter_uses_sharpness(int filter);
/* the live cycle (Ctrl+U): FSR 1, Bilinear, NVIDIA NIS, Qualcomm SGSR, None, and round. Auto enters at FSR 1 */
int vk_upscale_cycle(void);
#endif
