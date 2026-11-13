/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How many elements each texture array (SNX_VKB_TEXARRAY and SNX_VKB_TEXDEPTH) takes on a device, and the lines that
   say it. A pipeline layout has five stage sets, each with `slots` combined image samplers of its own and the two
   arrays; each array takes `most` elements (SNX_TEX_ARRAY) where the device's limits bind them, else as many as the
   least of those limits leaves after the slots, so a device that binds fewer samplers a stage (PanVK on Bifrost: 128)
   still picks textures from a table at run time, as long as a draw's tables name no more than that */
#ifndef SNX_VK_TEXTURE_ARRAYS_H
#define SNX_VK_TEXTURE_ARRAYS_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* the device's limits a stage's combined image samplers count against, as VkPhysicalDeviceLimits names them */
typedef struct {
    uint32_t stage_samplers; /* maxPerStageDescriptorSamplers */
    uint32_t stage_images;   /* maxPerStageDescriptorSampledImages */
    uint32_t set_samplers;   /* maxDescriptorSetSamplers: every set of a pipeline layout, five of them */
    uint32_t set_images;     /* maxDescriptorSetSampledImages */
} SnxVkTextureLimits;

/* the combined image samplers a stage may hold: the least of the limits, the layout's two counted for one stage */
static inline uint32_t snx_vk_texture_stage_room(const SnxVkTextureLimits* l) {
    uint32_t least = l->stage_samplers < l->stage_images ? l->stage_samplers : l->stage_images;
    if (l->set_samplers / 5 < least) least = l->set_samplers / 5;
    if (l->set_images / 5 < least) least = l->set_images / 5;
    return least;
}

/* the elements each array takes: most, or what the stage's room leaves after its slots, split between the two
   arrays; 0 when not one fits */
static inline uint32_t snx_vk_texture_array_fit(const SnxVkTextureLimits* l, uint32_t slots, uint32_t most) {
    uint32_t room = snx_vk_texture_stage_room(l);
    if (room <= slots) return 0;
    return (room - slots) / 2 < most ? (room - slots) / 2 : most;
}

/* The elements each array takes, and in out the line that says why where that is fewer than most: the device's values
   and what the arrays of most would need. out is empty where each array takes most (the descriptor pools' line says
   the arrays then). nonuniform: the device has shaderSampledImageArrayNonUniformIndexing, without which an element
   can't be picked per invocation */
static inline uint32_t snx_vk_texture_array_say(char* out, size_t n, int nonuniform, const SnxVkTextureLimits* l,
                                                uint32_t slots, uint32_t most) {
    uint32_t fit = nonuniform ? snx_vk_texture_array_fit(l, slots, most) : 0;
    if (!n) return fit;
    out[0] = 0;
    if (!nonuniform)
        snprintf(out, n,
                 "vulkan: texture arrays off: the GPU has no shaderSampledImageArrayNonUniformIndexing, which picks an "
                 "array's element per invocation: a texture a program picks from a table of handles at run time can't "
                 "be bound");
    else if (!fit)
        snprintf(out, n,
                 "vulkan: texture arrays off: the GPU binds %u samplers and %u sampled images a stage, and %u and %u "
                 "in the five stage sets, where one element of each of the two arrays beside the %u textures of a "
                 "stage needs %u: a texture a program picks from a table of handles at run time can't be bound",
                 (unsigned)l->stage_samplers, (unsigned)l->stage_images, (unsigned)l->set_samplers,
                 (unsigned)l->set_images, (unsigned)slots, (unsigned)(slots + 2));
    else if (fit < most)
        snprintf(out, n,
                 "vulkan: texture arrays of %u elements, not %u: the GPU binds %u samplers and %u sampled images a "
                 "stage, and %u and %u in the five stage sets, where arrays of %u need %u a stage (%u textures and two "
                 "arrays): a draw whose tables of handles name more than %u textures is not drawn",
                 (unsigned)fit, (unsigned)most, (unsigned)l->stage_samplers, (unsigned)l->stage_images,
                 (unsigned)l->set_samplers, (unsigned)l->set_images, (unsigned)most, (unsigned)(slots + 2 * most),
                 (unsigned)slots, (unsigned)fit);
    return fit;
}

/* How the descriptor pools' line says their arrays: "64 of them with texture arrays of 256 elements", or that their
   sets hold none (array_sets 0: the compute pools) or that the device takes none (elements 0) */
static inline void snx_vk_pool_arrays_text(char* out, size_t n, uint32_t array_sets, uint32_t elements) {
    if (!elements)
        snprintf(out, n, "none with texture arrays, which this GPU takes none of");
    else if (!array_sets)
        snprintf(out, n, "none with texture arrays");
    else
        snprintf(out, n, "%u of them with texture arrays of %u elements", (unsigned)array_sets, (unsigned)elements);
}
#endif
