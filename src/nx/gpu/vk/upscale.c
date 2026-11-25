/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The renderer's upscale (renderer.h's upscale), which a Switch 2 game's DLSS evaluations (gpu/nvn/nvn_copy.c's
   OP_DLSS) and the upscales hook modules record (its OP_UPSCALE) go through: the guest's surfaces as images of the
   upscalers' interface (gpu/vk/upscaler/upscaler.h), which picks the upscaler from the render_upscaler setting among
   those the caller takes. FSR 2.2.1 (gpu/vk/fsr2, MIT, Copyright (c) 2022-2023 Advanced Micro Devices, Inc.) is the
   one a DLSS evaluation gets by default. The game's color, depth and motion vector targets are sampled where they are.
   The upscaler writes an image of ours, as its output is a storage image and a render target need not be one, and
   that is copied onto the game's output target. The upscalers record with plain vkCmd* calls, so the evaluation runs
   on the recording thread, in order with the frame's other commands (vk_rec_call). 0 for anything none can take: the
   front end's filtered copy stands in */
#include <string.h>
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/upscaler/reproject.h"
#include "nx/gpu/vk/upscaler/upscaler.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

static struct {
    SnxUpscaler* up;
    SnxUpscalerId id;
    u32 render[2], out[2], flags; /* what up was made for: render is the largest render size it takes */
    VkFormat format;              /* and our output image's format */
    VkImage image;
    DevMem memory;
    VkImageView view;
    int fresh; /* the image has no layout yet, and the history nothing */
    u64 last;  /* plat_ns at the last evaluation */
    /* the last setup that failed: not tried again (each evaluation, usually one a frame) until one of these changes */
    SnxUpscalerId failed_id;
    u32 failed_render[2], failed_out[2], failed_flags;
    VkFormat failed_format;
    int failed;
    /* motion vectors made from depth for a hook module that gives a reprojection matrix (reproject.h), at the largest
       render size asked so far */
    SnxReproject* reproject;
} g_upscale;

struct VkJitter g_vk_jitter;
/* the jitter's sequence: Halton bases 2 and 3, offsets in render pixels from -0.5 to 0.5, 8 of them */
static u32 g_jitter_index;
static float halton(u32 i, u32 base) {
    float f = 1, r = 0;
    for (; i; i /= base) f /= (float)base, r += f * (float)(i % base);
    return r;
}

/* the FSR input conventions setting's bits (settings.txt) */
enum { FLIP_JITTER_X = 1, FLIP_JITTER_Y = 2, FLIP_MOTION = 4, FLIP_INVERTED = 8, FLIP_JITTERED = 16, AUTO_EXPOSURE = 32, FLIP_HDR = 64 };

typedef struct {
    SnxUpscaler* up;
    SnxUpscaleEval e;
    /* the motion vectors made first from the depth, with the matrix, when reproject is set */
    SnxReproject* reproject;
    float reprojection[16];
} Run;
static void run(const void* arg) {
    Run r;
    const char* why;
    memcpy(&r, arg, sizeof r);
    if (r.reproject) snx_reproject_record(r.reproject, r.e.cb, &r.e.depth, r.e.render, r.reprojection, &r.e.motion);
    if (!snx_upscaler_evaluate(r.up, &r.e, &why)) LOG_LIMIT(4, "vulkan: an upscale failed: %s\n", why);
}

/* the motion vectors pass for render, made (again, larger) when the one there is smaller: NULL when the device
   refuses it, said once */
static SnxReproject* reprojection(const u32 render[2]) {
    u32 have[2] = {0, 0};
    if (g_upscale.reproject) snx_reproject_size(g_upscale.reproject, have);
    if (g_upscale.reproject && have[0] >= render[0] && have[1] >= render[1]) return g_upscale.reproject;
    if (g_upscale.reproject) {
        vk_finish(); /* nothing recorded may still use its image or sets */
        snx_reproject_destroy(g_upscale.reproject);
    }
    const char* why = NULL;
    u32 size[2] = {have[0] > render[0] ? have[0] : render[0], have[1] > render[1] ? have[1] : render[1]};
    g_upscale.reproject = snx_reproject_create(g_vk.dev, g_vk.phys, size, &why);
    if (!g_upscale.reproject)
        LOG_LIMIT(1, "vulkan: no motion vectors from depth for hook modules: %s\n", why ? why : "refused");
    else
        LOG_LIMIT(1, "vulkan: motion vectors from depth for a hook module, up to %ux%u\n", size[0], size[1]);
    return g_upscale.reproject;
}

static void release(void) {
    if (!g_upscale.up) return;
    /* nothing recorded or running may still use the context's pipelines, sets and images: an evaluation of this frame
       may be in the open command buffer (a second evaluation of another size in the frame), which a device wait
       doesn't cover. vk_finish submits it and waits */
    vk_finish();
    snx_upscaler_destroy(g_upscale.up);
    g_upscale.up = NULL;
    vk_defer_image(g_upscale.image, g_upscale.memory, g_upscale.view, VK_NULL_HANDLE);
    g_upscale.image = VK_NULL_HANDLE;
    g_upscale.view = VK_NULL_HANDLE;
}

void vk_upscale_forget(void) {
    g_upscale.fresh = 1; /* the next evaluation resets the history */
    g_upscale.last = 0;
}

/* a setup for these that failed, kept so it isn't tried again: 0 */
static int setup_failed(SnxUpscalerId id, const u32 render[2], const u32 out[2], u32 flags, VkFormat format) {
    g_upscale.failed_id = id;
    memcpy(g_upscale.failed_render, render, sizeof g_upscale.failed_render);
    memcpy(g_upscale.failed_out, out, sizeof g_upscale.failed_out);
    g_upscale.failed_flags = flags;
    g_upscale.failed_format = format;
    g_upscale.failed = 1;
    return 0;
}

/* the context of upscaler id and our output image for render sizes up to render, and these output size, flags and
   format: 1 when there. color is the color's format when it is made, which the choice checked and no backend keeps,
   so another one doesn't make another context. A context made for a larger render size takes this one (a dynamic
   resolution): a game's dynamic resolution under DLSS changes the render size between frames, and a new context is a
   device wait and a history reset. who: what the log says it is for, fallback: what stands in when it can't be made */
static int ready(SnxUpscalerId id, const u32 render[2], const u32 out[2], u32 flags, VkFormat color, VkFormat format,
                 const char* who, const char* fallback) {
    const char* why;
    const SnxUpscalerCaps* caps = snx_upscaler_caps(id);
    if (g_upscale.up && g_upscale.id == id && g_upscale.render[0] >= render[0] && g_upscale.render[1] >= render[1] &&
        !memcmp(g_upscale.out, out, sizeof g_upscale.out) && g_upscale.flags == flags && g_upscale.format == format)
        return 1;
    if (g_upscale.failed && g_upscale.failed_id == id &&
        !memcmp(g_upscale.failed_render, render, sizeof g_upscale.failed_render) &&
        !memcmp(g_upscale.failed_out, out, sizeof g_upscale.failed_out) && g_upscale.failed_flags == flags &&
        g_upscale.failed_format == format)
        return 0;
    release();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {out[0], out[1], 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            /* DLSS (Streamline) clears it before it writes it */
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (!vk_image_create(&ii, &g_upscale.image, &g_upscale.memory)) return setup_failed(id, render, out, flags, format);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = g_upscale.image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_upscale.view))) {
        vk_image_destroy(g_upscale.image, g_upscale.memory);
        g_upscale.image = VK_NULL_HANDLE;
        return setup_failed(id, render, out, flags, format);
    }
    SnxUpscalerInfo info = {g_vk.phys, g_vk.dev, {render[0], render[1]}, {out[0], out[1]}, color, format, flags};
    u64 started = plat_ns();
    g_upscale.up = snx_upscaler_create(id, &info, &why);
    if (!g_upscale.up) {
        LOG_LIMIT(4, "vulkan: %s can't be set up for %ux%u to %ux%u (%s): %s stand in\n", caps->name, render[0],
                  render[1], out[0], out[1], why, fallback);
        vkDestroyImageView(g_vk.dev, g_upscale.view, NULL);
        vk_image_destroy(g_upscale.image, g_upscale.memory);
        g_upscale.image = VK_NULL_HANDLE;
        g_upscale.view = VK_NULL_HANDLE;
        return setup_failed(id, render, out, flags, format);
    }
    g_upscale.failed = 0;
    g_upscale.id = id;
    memcpy(g_upscale.render, render, sizeof g_upscale.render);
    memcpy(g_upscale.out, out, sizeof g_upscale.out);
    g_upscale.flags = flags;
    g_upscale.format = format;
    g_upscale.fresh = 1;
    if (caps->kind == SNX_UP_TEMPORAL)
        LOG("vulkan: %s upscales up to %ux%u to %ux%u for %s (flags %#x: %s color, motion vectors at the %s size%s, %s "
            "depth, %s exposure, output format %d, set up in %.1f ms)\n",
            caps->name, render[0], render[1], out[0], out[1], who, flags, flags & SNX_UP_FLAG_HDR ? "HDR" : "SDR",
            flags & SNX_UP_FLAG_DISPLAY_MOTION ? "output's" : "render",
            flags & SNX_UP_FLAG_JITTERED_MOTION ? " with jitter" : "",
            flags & SNX_UP_FLAG_INVERTED_DEPTH ? "inverted" : "forward",
            flags & SNX_UP_FLAG_AUTO_EXPOSURE ? "its own" : "the game's", (int)format,
            (double)(plat_ns() - started) / 1e6);
    else
        LOG("vulkan: %s upscales up to %ux%u to %ux%u for %s (output format %d, set up in %.1f ms)\n", caps->name,
            render[0], render[1], out[0], out[1], who, (int)format, (double)(plat_ns() - started) / 1e6);
    return 1;
}

/* a render target the renderer holds with pixels, as a sampled image of the upscaler's. Its read check comes first: a
   write to its memory since it was drawn reloads it (vk_rt_reload), which can land results and submit the frame's
   commands, or leave it without pixels. The frame's command buffer is taken after every input's check */
static int input(const Surf2D* s, int depth, const u32 least[2], Surf** surf, SnxUpscaleImage* image) {
    Surf* t = vk_rt_find(s->addr);
    if (t) vk_rt_note_read(t);
    /* a hosted target, read as level 0 of its own image here: given one first (docs/dev/one-image.md) */
    if (t && t->host && !vk_rt_own(t)) return 0;
    if (!t || t->layout == VK_IMAGE_LAYOUT_UNDEFINED || t->msaa || !t->f.depth != !depth || t->sw < least[0] ||
        t->sh < least[1]) {
        LOG_LIMIT(6, "vulkan: the upscaler can't sample the surface at %llx (format %#x): %s\n",
                  (unsigned long long)s->addr, s->format,
                  !t                                       ? "no render target there"
                  : t->layout == VK_IMAGE_LAYOUT_UNDEFINED ? "nothing drawn to it yet"
                  : t->msaa                                ? "multisampled"
                  : !t->f.depth != !depth                  ? "depth where color goes, or the reverse"
                                                           : "smaller than the render size");
        return 0;
    }
    const VkComponentMapping identity = {0};
    VkImageView view = vk_tex_view(t, VK_IMAGE_VIEW_TYPE_2D, 0, &identity, 0);
    if (!view) return 0;
    *surf = t;
    *image = (SnxUpscaleImage){t->img, view, t->f.vk, t->sw, t->sh};
    return 1;
}

int vk_upscale(const GpuUpscale* u) {
    static const u32 one[2] = {1, 1};
    Surf *color, *depth = NULL, *motion = NULL, *exposure = NULL, *reactive = NULL;
    Run r = {0};
    int conventions = g_settings.fsr_input_conventions;
    int module = (u->flags & GPU_UPSCALE_MODULE) != 0;
    const char* who = module ? "a hook module" : "the game's DLSS evaluations";
    const char* why;
    /* None: a DLSS evaluation is its front end's filtered copy (nvn_copy.c), 1:1 at the output's size. With SMAA or a
       steady picture asked for, the upscalers' copy runs it instead, after those */
    int none = !module && g_settings.render_upscaler == SNX_UPSCALER_COPY + 1;
    int before = g_settings.render_anti_aliasing || g_settings.jitter;
    if (none && !before) return 0;
    if (!g_vk.feat.shaderStorageImageReadWithoutFormat || !g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        LOG_LIMIT(1,
                  "vulkan: the upscalers need storage images read and written without a format, which this GPU "
                  "lacks: %s run their fallback\n",
                  module ? "hook modules' upscales are a filtered copy, and"
                         : "DLSS evaluations are a filtered copy, and");
        return 0;
    }
    /* At a resolution multiplier other than 1 the render targets are the guest's sizes times it: so are the call's
       sizes and corner, and its jitter and motion vector scale, which count render pixels */
    GpuUpscale scaled;
    if (g_vk_scale != 1.0f) {
        scaled = *u;
        for (u32 a = 0; a < 2; a++) {
            scaled.render[a] = (u32)((float)u->render[a] * g_vk_scale + 0.5f);
            scaled.out[a] = (u32)((float)u->out[a] * g_vk_scale + 0.5f);
            scaled.render_max[a] = (u32)((float)u->render_max[a] * g_vk_scale + 0.5f);
            scaled.out_base[a] = (i32)((float)u->out_base[a] * g_vk_scale + 0.5f);
            scaled.jitter[a] = u->jitter[a] * g_vk_scale;
            scaled.motion_scale[a] = u->motion_scale[a] * g_vk_scale;
        }
        u = &scaled;
    }
    /* in this order, each after the one before was taken (a DLSS evaluation needs all three): each input's read check
       may submit the frame's commands */
    int has_color = input(&u->color, 0, u->render, &color, &r.e.color);
    int has_depth = u->has_depth && has_color && input(&u->depth, 1, u->render, &depth, &r.e.depth);
    int has_motion =
        u->has_motion && has_color && (module || has_depth) && input(&u->motion, 0, one, &motion, &r.e.motion);
    /* a hook module without motion vectors of its own, with depth and a reprojection matrix: they are made from the
       depth on the recording thread, just before the upscaler runs */
    if (module && !has_motion && has_depth && (u->flags & GPU_UPSCALE_REPROJECTED_MOTION)) {
        r.reproject = reprojection(u->render);
        if (r.reproject) memcpy(r.reprojection, u->reprojection, sizeof r.reprojection);
    }
    if (!has_color || (!module && (!has_depth || !has_motion))) {
        LOG_LIMIT(4,
                  "vulkan: %s color (format %#x), depth (%#x) or motion vectors (%#x) aren't render targets the "
                  "upscaler can sample: %s this time\n",
                  module ? "a hook module's" : "a DLSS evaluation's", u->color.format, u->depth.format,
                  u->motion.format, "a filtered copy");
        return 0;
    }
    if (u->has_exposure && !input(&u->exposure, 0, one, &exposure, &r.e.exposure)) exposure = NULL;
    if (u->has_reactive && !input(&u->reactive, 0, u->render, &reactive, &r.e.reactive)) reactive = NULL;
    vk_surf_touch(color);
    if (depth) vk_surf_touch(depth);
    if (motion) vk_surf_touch(motion);
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
        LOG_LIMIT(1, "vulkan: the upscaler can't write the output's format (%d): %s\n", (int)format,
                  "a filtered copy stands in");
        return 0;
    }
    if (u->out_base[0] < 0 || u->out_base[1] < 0 || (u32)u->out_base[0] + u->out[0] > out->sw ||
        (u32)u->out_base[1] + u->out[1] > out->sh)
        return 0;
    /* the context's render size: the network's largest, which the render size stays within with a dynamic
       resolution, or this one when larger (or the largest isn't known) */
    u32 render_max[2];
    for (u32 a = 0; a < 2; a++) render_max[a] = u->render_max[a] > u->render[a] ? u->render_max[a] : u->render[a];
    /* motion vectors at the output's size: a target that large while the render size is smaller. At the output's
       size the two readings are the same, and the context's own stands, so a dynamic resolution reaching the output's
       size doesn't make another context */
    int display_motion = motion && motion->sw >= u->out[0] && motion->sh >= u->out[1] &&
                         (u->out[0] > u->render[0] || u->out[1] > u->render[1]);
    if (u->render[0] == u->out[0] && u->render[1] == u->out[1] && g_upscale.up)
        display_motion = (g_upscale.flags & SNX_UP_FLAG_DISPLAY_MOTION) != 0;
    VkFormat cf = color->f.vk;
    int hdr = u->flags & GPU_UPSCALE_RANGE_GIVEN
                  ? (u->flags & GPU_UPSCALE_HDR) != 0
                  : cf == VK_FORMAT_R16G16B16A16_SFLOAT || cf == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
                        cf == VK_FORMAT_R32G32B32A32_SFLOAT;
    hdr = !hdr != !(conventions & FLIP_HDR);
    u32 flags =
        (hdr ? SNX_UP_FLAG_HDR : 0) | (display_motion ? SNX_UP_FLAG_DISPLAY_MOTION : 0) |
        (!(u->flags & GPU_UPSCALE_JITTERED_MOTION) != !(conventions & FLIP_JITTERED) ? SNX_UP_FLAG_JITTERED_MOTION
                                                                                     : 0) |
        (!(u->flags & GPU_UPSCALE_INVERTED_DEPTH) != !(conventions & FLIP_INVERTED) ? SNX_UP_FLAG_INVERTED_DEPTH : 0) |
        (!exposure || (u->flags & GPU_UPSCALE_AUTO_EXPOSURE) || (conventions & AUTO_EXPOSURE)
             ? SNX_UP_FLAG_AUTO_EXPOSURE
             : 0);
    if (flags & SNX_UP_FLAG_AUTO_EXPOSURE) r.e.exposure = (SnxUpscaleImage){0};
    /* the upscaler: the setting's (0 Best, then upscaler.h's order), among the kinds the caller takes. A DLSS
       evaluation takes a temporal one only: its front end's copy is its fallback */
    SnxUpscaleNeeds needs = {(depth ? SNX_UP_DEPTH : 0) | (motion || r.reproject ? SNX_UP_MOTION : 0) | SNX_UP_JITTER |
                                 (exposure ? SNX_UP_EXPOSURE : 0) | (reactive ? SNX_UP_REACTIVE : 0),
                             none ? SNX_UP_COPY
                                  : SNX_UP_TEMPORAL | (u->flags & GPU_UPSCALE_SPATIAL_OK ? SNX_UP_SPATIAL : 0) |
                                        (u->flags & GPU_UPSCALE_COPY_OK ? SNX_UP_COPY : 0),
                             hdr ? SNX_UP_HDR : SNX_UP_LDR,
                             {u->render[0], u->render[1]},
                             {u->out[0], u->out[1]},
                             cf,
                             format};
    SnxUpscalerId id = snx_upscaler_choose(none ? SNX_UPSCALER_COPY : (SnxUpscalerId)(g_settings.render_upscaler - 1),
                                           g_vk.phys, &needs, &why);
    if (id == SNX_UPSCALERS) {
        LOG_LIMIT(4, "vulkan: no upscaler takes %s's %ux%u to %ux%u (%s): %s\n",
                  module ? "a hook module" : "a DLSS evaluation", u->render[0], u->render[1], u->out[0], u->out[1], why,
                  "a filtered copy stands in");
        return 0;
    }
    if (why)
        LOG_LIMIT(2, "vulkan: %s stands in for the upscaler the settings ask for: %s\n", snx_upscaler_caps(id)->name,
                  why);
    if (!ready(id, render_max, u->out, flags, cf, format, who, "filtered copies")) return 0;

    VkCommandBuffer cb = vk_cmd(); /* after the inputs' read checks (input) and the context's setup */
    Surf* reads[5] = {color, depth, motion, exposure, reactive};
    for (u32 i = 0; i < 5; i++) {
        if (!reads[i] || (i == 3 && (flags & SNX_UP_FLAG_AUTO_EXPOSURE))) continue;
        /* the upscalers write their descriptors themselves, with this layout (upscaler.h's evaluation, FSR 2's fsr2.h),
           also with unified image layouts */
        vk_surf_use(cb, reads[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    vk_render_end(); /* dispatches outside a rendering pass */
    if (g_upscale.fresh)
        vk_barrier(cb, g_upscale.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    u64 now = plat_ns();
    r.up = g_upscale.up;
    r.e.cb = cb;
    r.e.output = (SnxUpscaleImage){g_upscale.image, g_upscale.view, format, u->out[0], u->out[1]};
    r.e.render[0] = u->render[0], r.e.render[1] = u->render[1];
    float jitter[2] = {u->jitter[0], u->jitter[1]};
    /* the scene's jitter of ours (GPU_UPSCALE_RUNTIME_JITTER), for a temporal upscaler only (a spatial one would show
       it): this frame's draws into the depth took the offset set after the last evaluation (none at the first), and
       the next frame's is set now. In our pixels, the render pixels the upscaler sees */
    if (module && (u->flags & GPU_UPSCALE_RUNTIME_JITTER) && depth && snx_upscaler_caps(id)->kind == SNX_UP_TEMPORAL) {
        int took = g_vk_jitter.on && g_vk_jitter.depth == depth->img;
        jitter[0] = took ? g_vk_jitter.px[0] : 0, jitter[1] = took ? g_vk_jitter.px[1] : 0;
        g_jitter_index = g_jitter_index % 8 + 1;
        g_vk_jitter.depth = depth->img;
        g_vk_jitter.size[0] = u->render[0], g_vk_jitter.size[1] = u->render[1];
        g_vk_jitter.dsize[0] = depth->sw, g_vk_jitter.dsize[1] = depth->sh;
        g_vk_jitter.px[0] = halton(g_jitter_index, 2) - 0.5f, g_vk_jitter.px[1] = halton(g_jitter_index, 3) - 0.5f;
        g_vk_jitter.on = 1;
        LOG_LIMIT(1, "vulkan: the runtime jitters the scene's draws for a hook module (Halton 2, 3, 8 offsets)\n");
    } else if (module)
        g_vk_jitter.on = 0;
    r.e.jitter[0] = conventions & FLIP_JITTER_X ? -jitter[0] : jitter[0];
    r.e.jitter[1] = conventions & FLIP_JITTER_Y ? -jitter[1] : jitter[1];
    float sign = conventions & FLIP_MOTION ? -1.0f : 1.0f;
    r.e.motion_scale[0] = u->motion_scale[0] * sign;
    r.e.motion_scale[1] = u->motion_scale[1] * sign;
    /* the made motion vectors are in picture coordinates (0 to 1) of the render size */
    if (r.reproject) r.e.motion_scale[0] = (float)u->render[0] * sign, r.e.motion_scale[1] = (float)u->render[1] * sign;
    r.e.pre_exposure = u->pre_exposure;
    r.e.frame_ms = g_upscale.last ? (float)((double)(now - g_upscale.last) / 1e6) : 0;
    /* DLSS takes no camera: planes and a field of view the depth tests are tolerant of (they scale the depth
       separation, not the picture), unless the caller knows them */
    r.e.near_plane = u->near_plane > 0 ? u->near_plane : 0.1f;
    r.e.far_plane = u->far_plane > 0 ? u->far_plane : 1000.0f;
    r.e.fov_y = u->fov_y > 0 ? u->fov_y : 1.0f;
    r.e.reset = u->reset || g_upscale.fresh;
    r.e.sharpness = g_settings.fsr_sharpness;
    /* where no temporal upscaler runs, the anti-aliasing and the steady picture the settings ask for: the picture
       taken back by the jitter the caller gives, the game's own */
    if (snx_upscaler_caps(id)->kind != SNX_UP_TEMPORAL) {
        r.e.smaa = g_settings.render_anti_aliasing;
        r.e.smaa_quality = g_settings.smaa_quality;
        r.e.smaa_edges = g_settings.smaa_edges;
        if (g_settings.jitter) r.e.shift[0] = u->jitter[0], r.e.shift[1] = u->jitter[1];
        /* T2x resolves each frame with the one before, reprojected by the motion vectors: without them it is 1x.
           Both frames are taken back by their jitter first, so the resolve is steady whatever the jitter setting */
        if (r.e.smaa == 2 && !motion && !r.reproject) {
            LOG_LIMIT(1, "vulkan: SMAA T2x runs as 1x for %s: no motion vectors are given\n", who);
            r.e.smaa = 1;
        }
        if (r.e.smaa == 2) r.e.shift[0] = u->jitter[0], r.e.shift[1] = u->jitter[1];
        /* depth edges need a depth image the size of our images */
        if (r.e.smaa && r.e.smaa_edges == 2 && (!depth || depth->sw != render_max[0] || depth->sh != render_max[1])) {
            LOG_LIMIT(1, "vulkan: SMAA finds the edges by luma for %s: %s\n", who,
                      !depth ? "no depth is given" : "its depth isn't the render size");
            r.e.smaa_edges = 0;
        }
    }
    g_upscale.last = now;
    g_upscale.fresh = 0;
    LOG_LIMIT(2,
              "vulkan: %s evaluation: color %d %ux%u, depth %d %ux%u, motion %d %ux%u scale %g %g, jitter %g %g, "
              "pre-exposure %g, exposure %s, reactive %s, reset %d, flags %#x, render %ux%u to %ux%u, SMAA %d, "
              "shifted back %g %g\n",
              snx_upscaler_caps(id)->name, (int)color->f.vk, color->sw, color->sh, depth ? (int)depth->f.vk : 0,
              depth ? depth->sw : 0, depth ? depth->sh : 0,
              motion        ? (int)motion->f.vk
              : r.reproject ? -1
                            : 0,
              motion ? motion->sw : 0, motion ? motion->sh : 0, (double)r.e.motion_scale[0],
              (double)r.e.motion_scale[1], (double)r.e.jitter[0], (double)r.e.jitter[1], (double)u->pre_exposure,
              exposure ? "given" : "none", reactive ? "given" : "none", r.e.reset, u->flags, u->render[0], u->render[1],
              u->out[0], u->out[1], r.e.smaa, (double)r.e.shift[0], (double)r.e.shift[1]);
    vk_rec_call(run, &r, sizeof r);

    /* our image onto the game's output */
    vk_barrier(cb, g_upscale.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_surf_use(cb, out, VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffset = {u->out_base[0], u->out_base[1], 0},
                        .extent = {u->out[0], u->out[1], 1}};
    rec_CmdCopyImage(cb, g_upscale.image, VK_IMAGE_LAYOUT_GENERAL, out->img, VK_LAYOUT(TRANSFER_DST_OPTIMAL), 1,
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
    if (u->ran) *u->ran = snx_upscaler_caps(id)->name; /* the caps' name: a constant string */
    return 1;
}
