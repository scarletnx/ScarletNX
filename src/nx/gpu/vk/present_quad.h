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
   converts nothing); then its crop drawn into the pass open on cb, whose attachment has format, its edges smoothed
   with antialias (FXAA, common/gpu/vk/fxaa.glsl: a source made ready with linear only). 0 when it can't be */
typedef struct {
    Surf* surf;
    VkDescriptorSet set;
    int light; /* sampled through an sRGB view: the shader reads linear light */
} SnxPresentSource;
int vk_present_quad_source(VkCommandBuffer cb, Surf* source, int linear, int raw, SnxPresentSource* out);
int vk_present_quad_draw(VkCommandBuffer cb, const SnxPresentSource* s, VkFormat format, VkRect2D source_rect,
                         VkRect2D destination_rect, u32 transform, int antialias);
#endif
