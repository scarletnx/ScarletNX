/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_PRESENT_QUAD_H
#define SNX_VK_PRESENT_QUAD_H
#include "nx/gpu/vk/vk.h"
/* Draw a crop into an already cleared color target. Android transforms apply flips before a clockwise quarter turn.
   The GPU samples and converts the image. No host pixel conversion or image readback occurs here. */
int vk_present_quad(Surf* source, VkImage destination, VkFormat format, VkExtent2D extent,
                    VkRect2D source_rect, VkRect2D destination_rect, u32 transform, int linear);
/* vk_present_quad in two, for a pass that draws more (the window's, with the on-screen layer: gpu/vk/ui.c): source made
   ready to sample, outside a rendering pass (raw: through the view of its bytes, its sRGB twin's UNORM, which
   converts nothing); then its crop drawn into the pass open on cb, whose attachment has format. 0 when it can't be */
typedef struct {
    VkDescriptorSet set;
    VkImageView view;  /* what set samples */
    int light;         /* view is an sRGB view: what it samples is linear light */
    u32 width, height; /* its image's size, which source_rect is a part of */
} SnxPresentSource;
int vk_present_quad_source(VkCommandBuffer cb, Surf* source, int linear, int raw, SnxPresentSource* out);
int vk_present_quad_draw(VkCommandBuffer cb, const SnxPresentSource* s, VkFormat format, VkRect2D source_rect,
                         VkRect2D destination_rect, u32 transform);
/* The upscaler and the anti aliasing of the window's frame (common/gpu/vk/present_fx.h), the settings' at start
   (upscaler, upscaler_sharpness, anti_aliasing), said in the log; a shortcut's step for the run since */
void vk_present_quad_effects_init(void);
/* the upscaler's (which 0) or the anti aliasing's (1) next step for this run, and its label (any thread) */
const char* vk_present_quad_effects_step(int which);
/* s's crop (an SDR frame: the swapchain's g_vk.sc_space SRGB) through them, into an image of the size to has on
   screen (its sides swapped by transform's quarter turn), which s and crop then name, drawn with the same transform:
   1; 0 with neither changed where both are Off or nothing ran. Between the window's commands, before its pass. Under
   vk_lock */
int vk_present_quad_effects(VkCommandBuffer cb, SnxPresentSource* s, VkRect2D* crop, VkRect2D to, u32 transform);
#endif
