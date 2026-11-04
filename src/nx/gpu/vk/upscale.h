/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The spatial upscaler of the presented frame. FSR 1 is the default. FSR 1, NIS and SGSR take the color image's shown
   crop and write one image at the target, the screen rectangle times upscaler_mult, which the presenter blits into the
   rectangle (1:1 at 1x, downsampled above it). None and Bilinear leave the frame's blit as it was. Under vk_lock, in
   the frame being recorded. */
#ifndef SNX_GPU_VK_UPSCALE_H
#define SNX_GPU_VK_UPSCALE_H
#include "nx/gpu/vk/upscale_select.h"
#include "nx/gpu/vk/vk.h"

/* the setting and its sharpness, read once at start */
void vk_upscale_configure(int filter, int sharpness);
/* the window's surface went: its images go too. The device must be idle and vk_lock held */
void vk_upscale_release(void);
/* whether the device can run a filter: its output format has the features the filter's pipeline and copy need */
int vk_upscale_available(int filter);
/* the filter the frame runs: an unavailable filter is replaced by FSR 1, then Bilinear; -1 for a frame with no
   source */
int vk_upscale_pick(u32 src_w, u32 src_h, u32 dst_w, u32 dst_h, u32 transform);
/* the upscaler's target from the screen rectangle and upscaler_mult, at most max_dim each axis: *out_w and *out_h,
   and the multiplier it used (a smaller one when the largest image cannot hold it) */
u32 vk_upscale_target(u32 w, u32 h, u32 mult, u32 max_dim, u32* out_w, u32* out_h);
/* the filter's shown source to the output extent: 1 with out and out_view set, 0 when it could not run. The source
   is in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; the output is left in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL */
int vk_upscale_process(VkCommandBuffer cb, VkImageView src, u32 image_w, u32 image_h, u32 x, u32 y, u32 w, u32 h,
                       u32 transform, u32 dst_w, u32 dst_h, int filter, int sharpness, VkImage* out,
                       VkImageView* out_view);
/* the image format vk_upscale_process writes (RGBA16F, or RGBA8 where the device has no 16-bit one) */
VkFormat vk_upscale_format(void);
/* whether a filter's sharpness applies, for the menu's greyed row */
int vk_upscale_filter_uses_sharpness(int filter);
/* the live cycle (upscaler_key): FSR 1, Bilinear, NVIDIA NIS, Qualcomm SGSR, None, and round. The window thread
   changes the filter, the GPU thread reads it at the next present */
int vk_upscale_cycle(void);
#endif
