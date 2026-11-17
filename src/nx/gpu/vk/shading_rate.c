/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Ounce's shading rate image (GpuShadingRate, NVN's BindShadingRateImage). The game's image has a texel for each
   16x16 pixels, and each texel is an index into the palette of the viewport that a primitive goes to. A palette entry
   is a rate as the hardware numbers it: one fragment shader invocation for 1x1 to 4x4 pixels, 2 to 16 invocations
   for each pixel, or none. Each pixel shaded gives the image of the full rate, and the GPU does more work than the
   console asks for.

   The game's image becomes an image of the renderer's on the GPU (shading_rate.comp), at each bind and after a
   palette changes:
   - VK_NV_shading_rate_image has the console's model: a texel size of 16x16 on NVIDIA's GPUs, and a palette for each
     viewport, with the console's numbers. The image holds the game's indices, and the draws set the game's palettes
     (vkCmdSetViewportShadingRatePaletteNV). A pipeline reads the image with shadingRateImageEnable.
   - VK_KHR_fragment_shading_rate reads a rate from each texel of an attachment of the pass, with no palette. The
     image holds the rates of viewport 0's palette, encoded as the extension does (log2 of the width in bits 2-3,
     of the height in bits 0-1), at a texel size that the device takes. It can't give 2 to 16 invocations for each
     pixel (those shade each pixel) or none (those shade 4x4 pixels once). A pipeline combines the attachment's rate
     with REPLACE, and has the flag of a pass with that attachment.
   - Without either, each pixel is shaded, as before.

   The draws of the id that the last conversion gave read the image (DrawCall.shading_rate). A draw of an older id
   shades each pixel: its texels are gone. Each conversion ends the open pass, as a dispatch does */
#include <stdio.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/shading_rate.h"
#include "nx/gpu/vk/shading_rate_spv.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* the renderer's palette for VK_NV_shading_rate_image is the game's palette of the viewport */
#define PALETTES 16u

static struct {
    VkDescriptorSetLayout ds;
    VkPipelineLayout layout;
    VkPipeline pipe;
    int failed;
    VkImage img;
    DevMem mem;
    VkImageView view;
    u32 w, h;             /* the image's texels */
    int fresh;            /* its layout is still undefined */
    u32 id;               /* the last conversion's id, 0 for none yet */
    int valid;            /* the last conversion made the image: draws of id read it */
    u64 palette[PALETTES]; /* the palettes of id */
    /* what the frame's command buffer has bound for the draws: the command buffer (vk_cb_begun), the view, and
       the palettes of that id (VK_VRS_NV) */
    u32 cb, bound_id;
    VkImageView bound;
    int bound_set;
} g_rate;

static int rate_init(void) {
    if (g_rate.pipe) return 1;
    if (g_rate.failed) return 0;
    g_rate.failed = 1; /* until everything below is made */
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 2,
                                          .pBindings = b};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_rate.ds))) return 0;
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkVrsPush)};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_rate.ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pc};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &g_rate.layout))) return 0;
    u64 started = plat_ns();
    if (!VK_OK(snx_vk_compute_pipeline(g_vk.dev, vk_pipeline_cache(), g_rate.layout, shading_rate_spv,
                                       shading_rate_spv_size, &g_rate.pipe))) {
        g_rate.pipe = VK_NULL_HANDLE;
        return 0;
    }
    vk_texture_pipeline_made(plat_ns() - started);
    g_rate.failed = 0;
    return 1;
}

/* the renderer's image of w x h texels at least: the one it has, or a larger one */
static int rate_image(u32 w, u32 h) {
    if (g_rate.img && g_rate.w >= w && g_rate.h >= h) return 1;
    if (g_rate.img) {
        vk_defer_image(g_rate.img, g_rate.mem, g_rate.view, VK_NULL_HANDLE);
        if (w < g_rate.w) w = g_rate.w;
        if (h < g_rate.h) h = g_rate.h;
        g_rate.img = VK_NULL_HANDLE;
        g_rate.view = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8_UINT,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | (g_vk.vrs == VK_VRS_NV
                                                                       ? VK_IMAGE_USAGE_SHADING_RATE_IMAGE_BIT_NV
                                                                       : VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR),
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!vk_image_create(&ii, &g_rate.img, &g_rate.mem)) {
        g_rate.img = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = g_rate.img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8_UINT,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_rate.view))) {
        vk_defer_image(g_rate.img, g_rate.mem, VK_NULL_HANDLE, VK_NULL_HANDLE);
        g_rate.img = VK_NULL_HANDLE;
        return 0;
    }
    g_rate.w = w, g_rate.h = h;
    g_rate.fresh = 1;
    return 1;
}

/* the conversion's result where the draws read it, or its write after their reads */
static void rate_barrier(VkCommandBuffer cb, int to_draws) {
    const VkPipelineStageFlags2 draws = VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = to_draws ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : draws,
                               .srcAccessMask = to_draws ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0,
                               .dstStageMask = to_draws ? draws : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               .dstAccessMask = to_draws ? VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR
                                                         : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                               .oldLayout = !to_draws && g_rate.fresh ? VK_IMAGE_LAYOUT_UNDEFINED
                                                                      : VK_IMAGE_LAYOUT_GENERAL,
                               .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = g_rate.img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
    g_rate.fresh = 0;
}

static u64 g_converted, g_failed, g_draws; /* g_draws: the draws that read an image */
static int g_said;                        /* the first image's line was logged */

u32 vk_shading_rate(const GpuShadingRate* r) {
    if (g_vk.vrs == VK_VRS_NONE) {
        LOG_LIMIT(1, "vulkan: a shading rate image is shaded at the full rate: the device has no way to read it\n");
        return 0;
    }
    int reuse = vk_vrs_reuse(g_vk.vrs, r->new_image, g_rate.valid, g_rate.palette[0], r->palette[0]);
    if (reuse != VK_VRS_CONVERT) {
        memcpy(g_rate.palette, r->palette, sizeof g_rate.palette);
        return reuse == VK_VRS_NEW_ID ? ++g_rate.id : g_rate.id;
    }
    /* the game's texels as a compute program reads a texture (the guest's size where a scaled target has a copy of
       it): the last texels the GPU or the CPU wrote, from the texture header, as a draw takes it */
    Surf* game = r->texel ? vk_tex_from_tic_guest(r->tic, 0) : NULL;
    if (!game || game->msaa || game->f.depth || game->f.vk != VK_FORMAT_R8_UINT || !game->sw || !game->sh ||
        !rate_init()) {
        g_failed++;
        LOG_LIMIT(4, "vulkan: a shading rate image at %llx is shaded at the full rate: %s\n", (unsigned long long)r->addr,
                  !game                              ? "no texture there"
                  : game->f.vk != VK_FORMAT_R8_UINT ? "its format is not 8-bit unsigned integer"
                                                     : "the conversion can't be made");
        g_rate.id++; /* the draws of the image before it no longer read the renderer's */
        g_rate.valid = 0;
        return 0;
    }
    VkVrsPush push;
    vk_vrs_push(g_vk.vrs, g_vk.vrs_texel, game->sw, game->sh, vk_vrs_game_texel(r->texel, g_vk_scale, game->w, game->sw),
                r->palette[0], &push);
    VkComponentMapping identity = {0};
    VkImageView in = vk_tex_view(game, VK_IMAGE_VIEW_TYPE_2D, 0, &identity, 0);
    VkDescriptorSet set = in && rate_image(push.size_x, push.size_y) ? vk_desc_alloc(g_rate.ds) : VK_NULL_HANDLE;
    if (!set) {
        g_failed++;
        g_rate.id++;
        g_rate.valid = 0;
        return 0;
    }
    /* the whole image, which an earlier, larger game image may have grown: its texels past this one's take entry 0 */
    push.size_x = g_rate.w, push.size_y = g_rate.h;
    VkCommandBuffer cb = vk_cmd(); /* after the texture lookup, which can submit the frame's commands */
    vk_render_end();                /* a dispatch outside the pass */
    vk_surf_use(cb, game, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    rate_barrier(cb, 0);
    VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {VK_NULL_HANDLE, g_rate.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = set,
                                   .dstBinding = 0,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                   .pImageInfo = &ii[0]},
                                  {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = set,
                                   .dstBinding = 1,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   .pImageInfo = &ii[1]}};
    vkUpdateDescriptorSets(g_vk.dev, 2, wr, 0, NULL);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rate.pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rate.layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_rate.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vk_gpuprof_tag(VK_PROF_SHADING_RATE);
    rec_CmdDispatch(cb, (push.size_x + 7) / 8, (push.size_y + 7) / 8, 1);
    rate_barrier(cb, 1);
    vk_surf_touch(game);
    memcpy(g_rate.palette, r->palette, sizeof g_rate.palette);
    g_converted++;
    if (!g_said++)
        LOG("vulkan: the first shading rate image: %ux%u texels of the game's (format R8_UINT) as %ux%u of %ux%u "
            "pixels, viewport 0's palette %016llx\n",
            game->sw, game->sh, push.size_x, push.size_y, push.texel_w, push.texel_h, (unsigned long long)r->palette[0]);
    g_rate.valid = 1;
    return ++g_rate.id;
}

VkImageView vk_vrs_attachment(u32 id, VkExtent2D area) {
    if (g_vk.vrs != VK_VRS_KHR || !id || id != g_rate.id || !g_rate.view) return VK_NULL_HANDLE;
    /* the attachment covers the render area (VUID-VkRenderingInfo-imageView-06119): a larger pass shades each pixel */
    if ((u64)g_rate.w * g_vk.vrs_texel.width < area.width || (u64)g_rate.h * g_vk.vrs_texel.height < area.height)
        return VK_NULL_HANDLE;
    return g_rate.view;
}

typedef struct {
    VkCommandBuffer cb;
    VkImageView view;
    u32 palettes;
    VkShadingRatePaletteEntryNV e[PALETTES][16];
} NvBind;
static void nv_bind_run(const void* p) {
    const NvBind* x = p;
    vkCmdBindShadingRateImageNV(x->cb, x->view, VK_IMAGE_LAYOUT_GENERAL);
    VkShadingRatePaletteNV pal[PALETTES];
    for (u32 i = 0; i < x->palettes; i++) pal[i] = (VkShadingRatePaletteNV){16, x->e[i]};
    vkCmdSetViewportShadingRatePaletteNV(x->cb, 0, x->palettes, pal);
}

void vk_vrs_draw(VkCommandBuffer cb, u32 id) {
    if (id != g_rate.id || !g_rate.valid) id = 0; /* its texels are gone: each pixel shaded */
    g_draws += id != 0;
    if (g_vk.vrs != VK_VRS_NV) return;
    u32 serial = vk_cb_begun();
    if (g_rate.bound_set && g_rate.cb == serial && g_rate.bound_id == id) return;
    NvBind x = {cb, id ? g_rate.view : VK_NULL_HANDLE, PALETTES};
    /* without an image every texel reads as index 0: entry 0 of each palette shades each pixel then */
    for (u32 v = 0; v < PALETTES; v++)
        for (u32 i = 0; i < 16; i++)
            x.e[v][i] = id ? (VkShadingRatePaletteEntryNV)((g_rate.palette[v] >> (4 * i)) & 15u)
                           : VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_PIXEL_NV;
    vk_rec_call(nv_bind_run, &x, sizeof x);
    g_rate.cb = serial;
    g_rate.bound_id = id;
    g_rate.bound = x.view;
    g_rate.bound_set = 1;
}

void vk_vrs_keep(VkCommandBuffer cb) {
    if (g_vk.vrs != VK_VRS_NV || (g_rate.bound_set && g_rate.cb == vk_cb_begun())) return;
    vk_vrs_draw(cb, 0);
}

void vk_vrs_pipeline(VkVrsPipe* o, VkPipelineViewportStateCreateInfo* vp, VkPipelineCreationFeedbackCreateInfo* tail) {
    *o = (VkVrsPipe){0};
    if (g_vk.vrs == VK_VRS_NV) {
        /* the viewport state's count (0 with VIEWPORT_WITH_COUNT): the palettes are dynamic, and vk_vrs_draw sets one
           for each viewport that a draw may set (VUID-vkCmdDraw-shadingRateImage-09234) */
        o->nv = (VkPipelineViewportShadingRateImageStateCreateInfoNV){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SHADING_RATE_IMAGE_STATE_CREATE_INFO_NV,
            .pNext = vp->pNext,
            .shadingRateImageEnable = VK_TRUE,
            .viewportCount = vp->viewportCount};
        vp->pNext = &o->nv;
    } else if (g_vk.vrs == VK_VRS_KHR) {
        /* the pipeline's rate 1x1, the primitive's kept, the attachment's taken (1x1 in a pass without one) */
        o->khr = (VkPipelineFragmentShadingRateStateCreateInfoKHR){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
            .pNext = tail->pNext,
            .fragmentSize = {1, 1},
            .combinerOps = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
                            VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR}};
        tail->pNext = &o->khr;
    }
}

VkPipelineCreateFlags vk_vrs_pipeline_flags(void) {
    return g_vk.vrs == VK_VRS_KHR ? VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR : 0;
}

void vk_vrs_log(void) {
    if (!g_converted && !g_failed) return;
    LOG("vulkan: shading rate images: %llu made for the device, %llu shaded at the full rate; %llu draws read one\n",
        (unsigned long long)g_converted, (unsigned long long)g_failed, (unsigned long long)g_draws);
    g_converted = g_failed = g_draws = 0;
}
