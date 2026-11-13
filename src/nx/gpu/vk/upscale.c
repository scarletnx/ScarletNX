/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The renderer's temporal upscaler (renderer.h's upscale), which a Switch 2 game's DLSS evaluations go through
   (gpu/nvn/nvn_copy.c's OP_DLSS): AMD FidelityFX Super Resolution 2.2.1 (gpu/vk/fsr2, MIT, Copyright (c) 2022-2023
   Advanced Micro Devices, Inc.). The game's color, depth and motion vector targets are sampled where they are; FSR
   writes an image of ours, as its output is a storage image and a render target need not be one, and that is copied
   onto the game's output target. FSR's backend records with plain vkCmd* calls, so its dispatch runs on the recording
   thread, in order with the frame's other commands (vk_rec_call). 0 for anything it can't take: the front end's
   filtered copy stands in */
#include <string.h>
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/fsr2/fsr2.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

static struct {
    SnxFsr2* fsr;
    u32 render[2], out[2], flags; /* what fsr was made for */
    VkFormat format;              /* and our output image's format */
    VkImage image;
    DevMem memory;
    VkImageView view;
    int fresh; /* the image has no layout yet, and the history nothing */
    u64 last;  /* plat_ns at the last dispatch */
} g_upscale;

/* the FSR input conventions setting's bits (settings.txt) */
enum { FLIP_JITTER_X = 1, FLIP_JITTER_Y = 2, FLIP_MOTION = 4, FLIP_INVERTED = 8, FLIP_JITTERED = 16, AUTO_EXPOSURE = 32, FLIP_HDR = 64 };

typedef struct {
    SnxFsr2* fsr;
    SnxFsr2Dispatch d;
} Run;
static void run(const void* arg) {
    Run r;
    memcpy(&r, arg, sizeof r);
    if (!snx_fsr2_dispatch(r.fsr, &r.d))
        LOG_LIMIT(4, "vulkan: FSR 2's dispatch failed (FSR error %#x)\n", (u32)snx_fsr2_error());
}

static void release(void) {
    if (!g_upscale.fsr) return;
    /* nothing recorded or running may still use the context's images and sets */
    vk_rec_sync();
    vk_queue_lock(g_vk.gfx);
    if (g_vk.compute != g_vk.gfx) vk_queue_lock(g_vk.compute);
    vkDeviceWaitIdle(g_vk.dev);
    if (g_vk.compute != g_vk.gfx) vk_queue_unlock(g_vk.compute);
    vk_queue_unlock(g_vk.gfx);
    snx_fsr2_destroy(g_upscale.fsr);
    g_upscale.fsr = NULL;
    vk_defer_image(g_upscale.image, g_upscale.memory, g_upscale.view, VK_NULL_HANDLE);
    g_upscale.image = VK_NULL_HANDLE;
    g_upscale.view = VK_NULL_HANDLE;
}

/* the context and our output image for these sizes, flags and format: 1 when there */
static int ready(const u32 render[2], const u32 out[2], u32 flags, VkFormat format) {
    if (g_upscale.fsr && !memcmp(g_upscale.render, render, sizeof g_upscale.render) &&
        !memcmp(g_upscale.out, out, sizeof g_upscale.out) && g_upscale.flags == flags && g_upscale.format == format)
        return 1;
    release();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {out[0], out[1], 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (!vk_image_create(&ii, &g_upscale.image, &g_upscale.memory)) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = g_upscale.image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_upscale.view))) {
        vk_image_destroy(g_upscale.image, g_upscale.memory);
        g_upscale.image = VK_NULL_HANDLE;
        return 0;
    }
    SnxFsr2Info info = {g_vk.phys, g_vk.dev, {render[0], render[1]}, {out[0], out[1]}, flags};
    u64 started = plat_ns();
    g_upscale.fsr = snx_fsr2_create(&info);
    if (!g_upscale.fsr) {
        LOG("vulkan: FSR 2 can't be set up for %ux%u to %ux%u (FSR error %#x): DLSS evaluations are a filtered copy\n",
            render[0], render[1], out[0], out[1], (u32)snx_fsr2_error());
        vkDestroyImageView(g_vk.dev, g_upscale.view, NULL);
        vk_image_destroy(g_upscale.image, g_upscale.memory);
        g_upscale.image = VK_NULL_HANDLE;
        g_upscale.view = VK_NULL_HANDLE;
        return 0;
    }
    memcpy(g_upscale.render, render, sizeof g_upscale.render);
    memcpy(g_upscale.out, out, sizeof g_upscale.out);
    g_upscale.flags = flags;
    g_upscale.format = format;
    g_upscale.fresh = 1;
    LOG("vulkan: FSR 2.2.1 upscales %ux%u to %ux%u for the game's DLSS evaluations (flags %#x: %s color, motion "
        "vectors at the %s size%s, %s depth, %s exposure; output format %d; set up in %.1f ms)\n",
        render[0], render[1], out[0], out[1], flags, flags & SNX_FSR2_HDR ? "HDR" : "SDR",
        flags & SNX_FSR2_DISPLAY_MOTION ? "output's" : "render", flags & SNX_FSR2_JITTERED_MOTION ? " with jitter" : "",
        flags & SNX_FSR2_INVERTED_DEPTH ? "inverted" : "forward", flags & SNX_FSR2_AUTO_EXPOSURE ? "FSR's own" : "the game's",
        (int)format, (double)(plat_ns() - started) / 1e6);
    return 1;
}

/* a render target the renderer holds with pixels, as a sampled image of FSR's. Its read check comes first: a write to
   its memory since it was drawn reloads it (vk_rt_reload), which can land results and submit the frame's commands, or
   leave it without pixels. The frame's command buffer is taken after every input's check */
static int input(const Surf2D* s, int depth, const u32 least[2], Surf** surf, SnxFsr2Image* image) {
    Surf* t = vk_rt_find(s->addr);
    if (t) vk_rt_note_read(t);
    if (!t || t->layout == VK_IMAGE_LAYOUT_UNDEFINED || t->msaa || !t->f.depth != !depth || t->sw < least[0] ||
        t->sh < least[1]) {
        LOG_LIMIT(6, "vulkan: FSR 2 can't sample the surface at %llx (format %#x): %s\n", (unsigned long long)s->addr,
                  s->format,
                  !t ? "no render target there" : t->layout == VK_IMAGE_LAYOUT_UNDEFINED ? "nothing drawn to it yet"
                  : t->msaa ? "multisampled" : !t->f.depth != !depth ? "depth where color goes, or the reverse"
                  : "smaller than the render size");
        return 0;
    }
    const VkComponentMapping identity = {0};
    VkImageView view = vk_tex_view(t, VK_IMAGE_VIEW_TYPE_2D, 0, &identity, 0);
    if (!view) return 0;
    *surf = t;
    *image = (SnxFsr2Image){t->img, view, t->f.vk, t->sw, t->sh};
    return 1;
}

int vk_upscale(const GpuUpscale* u) {
    static const u32 one[2] = {1, 1};
    Surf *color, *depth, *motion, *exposure = NULL, *reactive = NULL;
    Run r = {0};
    int conventions = g_settings.fsr_input_conventions;
    if (g_settings.dlss_upscaler != 1) return 0;
    if (!g_vk.feat.shaderStorageImageReadWithoutFormat || !g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        LOG_LIMIT(1, "vulkan: FSR 2 needs storage images read and written without a format, which this GPU lacks: "
                     "DLSS evaluations are a filtered copy\n");
        return 0;
    }
    if (g_vk_scale != 1.0f) {
        LOG_LIMIT(1, "vulkan: FSR 2 isn't run with a resolution scale other than 1: DLSS evaluations are a filtered "
                     "copy\n");
        return 0;
    }
    if (!input(&u->color, 0, u->render, &color, &r.d.color) || !input(&u->depth, 1, u->render, &depth, &r.d.depth) ||
        !input(&u->motion, 0, one, &motion, &r.d.motion)) {
        LOG_LIMIT(4, "vulkan: a DLSS evaluation's color (format %#x), depth (%#x) or motion vectors (%#x) aren't "
                     "render targets FSR 2 can sample: a filtered copy this time\n",
                  u->color.format, u->depth.format, u->motion.format);
        return 0;
    }
    if (u->has_exposure && !input(&u->exposure, 0, one, &exposure, &r.d.exposure)) exposure = NULL;
    if (u->has_reactive && !input(&u->reactive, 0, u->render, &reactive, &r.d.reactive)) reactive = NULL;
    vk_surf_touch(color), vk_surf_touch(depth), vk_surf_touch(motion);
    if (exposure) vk_surf_touch(exposure);
    if (reactive) vk_surf_touch(reactive);
    u32 tile = (u->output.linear ? 0x1000u : 0) | (u->output.bh << 4);
    Surf* out = vk_rt(u->output.addr, u->output.width, u->output.height, u->output.format, tile, 0, u->output.pitch);
    if (!out || out->f.depth || out->f.is_int || out->msaa || out == color) return 0;
    /* our image: the output's format without its sRGB transfer (the copy moves the texels as they are), which the
       device must take as a storage image. FSR's own table of formats lacks the depth ones and some color ones, and
       only describes the images it is given: it is not asked */
    VkFormat format = vk_srgb_as(out->f.vk, 0);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        LOG_LIMIT(1, "vulkan: FSR 2 can't write the DLSS output's format (%d): a filtered copy stands in\n",
                  (int)format);
        return 0;
    }
    if (u->out_base[0] < 0 || u->out_base[1] < 0 || (u32)u->out_base[0] + u->out[0] > out->sw ||
        (u32)u->out_base[1] + u->out[1] > out->sh)
        return 0;
    VkFormat cf = color->f.vk;
    int hdr = cf == VK_FORMAT_R16G16B16A16_SFLOAT || cf == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
              cf == VK_FORMAT_R32G32B32A32_SFLOAT;
    u32 flags = (!hdr != !(conventions & FLIP_HDR) ? SNX_FSR2_HDR : 0) |
                (motion->sw >= u->out[0] && motion->sh >= u->out[1] && u->out[0] > u->render[0] ? SNX_FSR2_DISPLAY_MOTION
                                                                                              : 0) |
                (!(u->flags & 4) != !(conventions & FLIP_JITTERED) ? SNX_FSR2_JITTERED_MOTION : 0) |
                (!(u->flags & 8) != !(conventions & FLIP_INVERTED) ? SNX_FSR2_INVERTED_DEPTH : 0) |
                (!exposure || (u->flags & 0x40) || (conventions & AUTO_EXPOSURE) ? SNX_FSR2_AUTO_EXPOSURE : 0);
    if (flags & SNX_FSR2_AUTO_EXPOSURE) r.d.exposure = (SnxFsr2Image){0};
    if (!ready(u->render, u->out, flags, format)) return 0;

    VkCommandBuffer cb = vk_cmd(); /* after the inputs' read checks (input) and the context's setup */
    Surf* reads[5] = {color, depth, motion, exposure, reactive};
    for (u32 i = 0; i < 5; i++) {
        if (!reads[i] || (i == 3 && (flags & SNX_FSR2_AUTO_EXPOSURE))) continue;
        vk_surf_use(cb, reads[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    vk_render_end(); /* dispatches outside a rendering pass */
    if (g_upscale.fresh)
        vk_barrier(cb, g_upscale.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    u64 now = plat_ns();
    r.fsr = g_upscale.fsr;
    r.d.cb = cb;
    r.d.output = (SnxFsr2Image){g_upscale.image, g_upscale.view, format, u->out[0], u->out[1]};
    r.d.render[0] = u->render[0], r.d.render[1] = u->render[1];
    r.d.jitter[0] = conventions & FLIP_JITTER_X ? -u->jitter[0] : u->jitter[0];
    r.d.jitter[1] = conventions & FLIP_JITTER_Y ? -u->jitter[1] : u->jitter[1];
    float sign = conventions & FLIP_MOTION ? -1.0f : 1.0f;
    r.d.motion_scale[0] = u->motion_scale[0] * sign;
    r.d.motion_scale[1] = u->motion_scale[1] * sign;
    r.d.pre_exposure = u->pre_exposure;
    r.d.frame_ms = g_upscale.last ? (float)((double)(now - g_upscale.last) / 1e6) : 0;
    /* DLSS takes no camera: planes and a field of view FSR's depth tests are tolerant of (they scale its depth
       separation, not the picture) */
    r.d.near_plane = 0.1f;
    r.d.far_plane = 1000.0f;
    r.d.fov_y = 1.0f;
    r.d.reset = u->reset || g_upscale.fresh;
    r.d.sharpness = g_settings.fsr_sharpness;
    g_upscale.last = now;
    g_upscale.fresh = 0;
    LOG_LIMIT(2, "vulkan: FSR 2 dispatch: color %d %ux%u, depth %d %ux%u, motion %d %ux%u scale %g %g, jitter %g %g, "
                 "pre-exposure %g, exposure %s, reactive %s, reset %d, network flags %#x\n",
              (int)color->f.vk, color->sw, color->sh, (int)depth->f.vk, depth->sw, depth->sh, (int)motion->f.vk,
              motion->sw, motion->sh, (double)u->motion_scale[0], (double)u->motion_scale[1], (double)u->jitter[0],
              (double)u->jitter[1], (double)u->pre_exposure, exposure ? "given" : "none", reactive ? "given" : "none",
              r.d.reset, u->flags);
    vk_rec_call(run, &r, sizeof r);

    /* our image onto the game's output */
    vk_barrier(cb, g_upscale.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_surf_use(cb, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffset = {u->out_base[0], u->out_base[1], 0},
                        .extent = {u->out[0], u->out[1], 1}};
    rec_CmdCopyImage(cb, g_upscale.image, VK_IMAGE_LAYOUT_GENERAL, out->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                     &copy);
    vk_barrier(cb, g_upscale.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_surf_wrote(out);
    if (!u->out_base[0] && !u->out_base[1] && u->out[0] == out->sw && u->out[1] == out->sh)
        vk_rt_drawn_full(out);
    else
        vk_rt_drawn(out);
    vk_surf_touch(out);
    return 1;
}
