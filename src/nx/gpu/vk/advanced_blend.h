/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Native advanced equations include their own factors and apply the same operation to RGB and alpha. A device
   without coherent operations needs explicit primitive ordering, not merely one barrier between draws. */
#ifndef SNX_VK_ADVANCED_BLEND_H
#define SNX_VK_ADVANCED_BLEND_H
#include "nx/gpu/nvn/nvn_obj.h"
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>
#include <string.h>

typedef struct {
    u32 operation, src_premultiplied, overlap;
} VkAdvancedBlend;
typedef struct {
    u32 available, coherent, max_attachments, nonpremultiplied_source, correlated_overlap, all_operations;
} VkAdvancedBlendCaps;

static inline u32 vk_advanced_blend_operation(u32 mode) {
    /* NVN groups the extra arithmetic operations before MULTIPLY, and HSL after the channel selectors. */
    static const u32 operations[] = {VK_BLEND_OP_ZERO_EXT,
                                     VK_BLEND_OP_SRC_EXT,
                                     VK_BLEND_OP_DST_EXT,
                                     VK_BLEND_OP_SRC_OVER_EXT,
                                     VK_BLEND_OP_DST_OVER_EXT,
                                     VK_BLEND_OP_SRC_IN_EXT,
                                     VK_BLEND_OP_DST_IN_EXT,
                                     VK_BLEND_OP_SRC_OUT_EXT,
                                     VK_BLEND_OP_DST_OUT_EXT,
                                     VK_BLEND_OP_SRC_ATOP_EXT,
                                     VK_BLEND_OP_DST_ATOP_EXT,
                                     VK_BLEND_OP_XOR_EXT,
                                     VK_BLEND_OP_PLUS_EXT,
                                     VK_BLEND_OP_PLUS_CLAMPED_EXT,
                                     VK_BLEND_OP_PLUS_CLAMPED_ALPHA_EXT,
                                     VK_BLEND_OP_PLUS_DARKER_EXT,
                                     VK_BLEND_OP_MULTIPLY_EXT,
                                     VK_BLEND_OP_SCREEN_EXT,
                                     VK_BLEND_OP_OVERLAY_EXT,
                                     VK_BLEND_OP_DARKEN_EXT,
                                     VK_BLEND_OP_LIGHTEN_EXT,
                                     VK_BLEND_OP_COLORDODGE_EXT,
                                     VK_BLEND_OP_COLORBURN_EXT,
                                     VK_BLEND_OP_HARDLIGHT_EXT,
                                     VK_BLEND_OP_SOFTLIGHT_EXT,
                                     VK_BLEND_OP_DIFFERENCE_EXT,
                                     VK_BLEND_OP_MINUS_EXT,
                                     VK_BLEND_OP_MINUS_CLAMPED_EXT,
                                     VK_BLEND_OP_EXCLUSION_EXT,
                                     VK_BLEND_OP_CONTRAST_EXT,
                                     VK_BLEND_OP_INVERT_EXT,
                                     VK_BLEND_OP_INVERT_RGB_EXT,
                                     VK_BLEND_OP_INVERT_OVG_EXT,
                                     VK_BLEND_OP_LINEARDODGE_EXT,
                                     VK_BLEND_OP_LINEARBURN_EXT,
                                     VK_BLEND_OP_VIVIDLIGHT_EXT,
                                     VK_BLEND_OP_LINEARLIGHT_EXT,
                                     VK_BLEND_OP_PINLIGHT_EXT,
                                     VK_BLEND_OP_HARDMIX_EXT,
                                     VK_BLEND_OP_RED_EXT,
                                     VK_BLEND_OP_GREEN_EXT,
                                     VK_BLEND_OP_BLUE_EXT,
                                     VK_BLEND_OP_HSL_HUE_EXT,
                                     VK_BLEND_OP_HSL_SATURATION_EXT,
                                     VK_BLEND_OP_HSL_COLOR_EXT,
                                     VK_BLEND_OP_HSL_LUMINOSITY_EXT};
    return mode >= NVN_BLEND_ADVANCED_MODE_ZERO && mode <= NVN_BLEND_ADVANCED_MODE_HSL_LUMINOSITY
               ? operations[mode - NVN_BLEND_ADVANCED_MODE_ZERO]
               : 0;
}
static inline int vk_advanced_blend_required_operation(u32 mode) {
    /* These artistic and HSL operations are mandatory even when advancedBlendAllOperations is false. */
    return (mode >= NVN_BLEND_ADVANCED_MODE_MULTIPLY && mode <= NVN_BLEND_ADVANCED_MODE_DIFFERENCE) ||
           mode == NVN_BLEND_ADVANCED_MODE_EXCLUSION ||
           (mode >= NVN_BLEND_ADVANCED_MODE_HSL_HUE && mode <= NVN_BLEND_ADVANCED_MODE_HSL_LUMINOSITY);
}
static inline int vk_advanced_blend_key(VkAdvancedBlend* key, u64 packed, const VkAdvancedBlendCaps* caps,
                                        u32 attachments, int primitive_ordered) {
    memset(key, 0, sizeof *key); /* every byte participates in the pipeline key */
    u32 mode = (u32)nvn_field(packed, NVN_BLEND_ADV);
    if (!mode) return 1;
    u32 operation = vk_advanced_blend_operation(mode);
    u32 overlap = (u32)nvn_field(packed, NVN_BLEND_ADV_OVL);
    u32 premultiplied = (u32)nvn_field(packed, NVN_BLEND_ADV_PMS);
    if (!operation || !caps->available || (!caps->coherent && !primitive_ordered) || !attachments ||
        attachments > caps->max_attachments || overlap > NVN_BLEND_ADVANCED_OVERLAP_CONJOINT ||
        (!premultiplied && !caps->nonpremultiplied_source) || (overlap && !caps->correlated_overlap) ||
        (!caps->all_operations && !vk_advanced_blend_required_operation(mode)))
        return 0;
    /* NormalizedDst promises that attachment input/output values are clamped. It does not describe destination
       premultiplication. Vulkan's destination remains premultiplied for both values of that hint. */
    key->operation = operation;
    key->src_premultiplied = premultiplied;
    key->overlap = overlap;
    return 1;
}
#endif
