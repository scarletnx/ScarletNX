/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An NVDEC picture laid out in its surfaces on the GPU, and kept there (docs/GPU_OWNERSHIP.md). FFmpeg decodes on the
   CPU, and its planes' rows are staged as they are; or, with the vulkan_video setting (experimental, untested), FFmpeg
   decodes H.264 with Vulkan Video on this device, and its picture's planes are copied out of its image on the GPU.
   The rest is the GPU's: each surface (its current contents: the guest's bytes, or what the GPU holds of them) is
   deswizzled into rows, the picture's rows are copied over them (exactly its width: the bytes after it in a row's last
   16 keep their values, as the hardware leaves them), a CPU picture's U and V interleaved into NV12's chroma first
   (texconv.comp), and the rows swizzled back (blocklinear.comp) into a buffer that is published over the surface. VIC
   then reads it on the GPU. Under vk_lock, in the frame's command buffer */
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/memory/mem.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>

static u64 up(u64 v, u64 a) { return (v + a - 1) / a * a; }

/* a surface's current contents into a new buffer of its extent: staged, what the GPU holds copied over */
static VkOwnRes* surface_load(VkCommandBuffer cb, u64 cpu, u64 ext) {
    VkOwnRes* r = vk_own_res_new(ext);
    VkBuffer staged;
    u64 at;
    u8* p = r ? vk_stage(ext, vk_stage_align(), &staged, &at) : NULL;
    if (!p) {
        if (r) vk_own_res_unref(r);
        return NULL;
    }
    vk_cpu_read(cpu, ext, p, staged, at);
    VkBufferCopy c = {at, 0, ext};
    rec_CmdCopyBuffer(cb, staged, r->buf, 1, &c);
    return r;
}

/* ---- a picture FFmpeg decoded with Vulkan Video: its planes copied into the scratch on the GPU. FFmpeg locks the
   picture (lock_frame) before it takes a queue (queue_lock below, vk_lock), so the picture is locked before vk_lock
   here too */
typedef struct {
    AVFrame* f; /* a reference, held until the frame's work is done */
    AVHWFramesContext* fc;
    AVVkFrame* vkf;
} HwPic;
/* the picture locked and referenced: 1, or 0 when it can't be written */
static int hw_lock(const NvdecPic* p, HwPic* h) {
    const AVFrame* src = p->hw;
    AVVulkanFramesContext* vfc;
    if (!src->hw_frames_ctx) return 0;
    h->fc = (AVHWFramesContext*)src->hw_frames_ctx->data;
    if (h->fc->sw_format != AV_PIX_FMT_NV12) {
        WARN_LIMIT(4, "video: a Vulkan Video picture of format %d isn't written (8 bit NV12 only)\n",
                   h->fc->sw_format);
        return 0;
    }
    h->vkf = (AVVkFrame*)src->data[0];
    if (h->vkf->img[1]) { /* FFmpeg's decoder makes one two-plane image */
        WARN_LIMIT(4, "video: a Vulkan Video picture in an image per plane isn't written\n");
        return 0;
    }
    if (!(h->f = av_frame_clone(src))) return 0;
    vfc = h->fc->hwctx;
    vfc->lock_frame(h->fc, h->vkf);
    return 1;
}
static void hw_unlock(HwPic* h) {
    AVVulkanFramesContext* vfc = h->fc->hwctx;
    vfc->unlock_frame(h->fc, h->vkf);
}
static void frame_free(void* f) {
    AVFrame* p = f;
    av_frame_free(&p);
}
/* the luma rows (w bytes apart) at at_y and the chroma rows (2 * ((w + 1) / 2) bytes apart) at at_c of the scratch:
   the image's planes in TRANSFER_SRC_OPTIMAL, after FFmpeg's decode. The work recorded before goes first, so it doesn't
   wait for the decode; the next submission waits for its semaphore, at the value its decode signals, and steps it on
   one, FFmpeg's next use waiting for that (as its own transfers do) */
static void hw_begin(VkCommandBuffer* cb, const NvdecPic* p, HwPic* h, VkBuffer scratch, u64 at_y, u64 at_c) {
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    vk_submit_after(h->vkf->sem[0], h->vkf->sem_value[0], h->vkf->sem_value[0] + 1);
    h->vkf->sem_value[0]++;
    *cb = vk_cmd();
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_NONE, /* the semaphore made the decode visible */
                               .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                               .oldLayout = h->vkf->layout[0],
                               .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, /* concurrent images */
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = h->vkf->img[0],
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(*cb, &d);
    h->vkf->layout[0] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    h->vkf->access[0] = VK_ACCESS_2_TRANSFER_READ_BIT;
    /* the coded picture from its top left, as the CPU's planes are (the buffer the decoder started is uncropped) */
    VkBufferImageCopy c[2] = {{.bufferOffset = at_y,
                               .imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1},
                               .imageExtent = {p->w, p->h, 1}},
                              {.bufferOffset = at_c,
                               .imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1},
                               .imageExtent = {(p->w + 1) / 2, (p->h + 1) / 2, 1}}};
    rec_CmdCopyImageToBuffer(*cb, h->vkf->img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scratch, 2, c);
}
/* the commands submitted now, so FFmpeg's next use of the picture isn't kept waiting for the frame's end; the
   reference let go once they are done */
static void hw_end(HwPic* h) {
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    vk_defer_call(frame_free, h->f);
}

static int nvdec_write(const NvdecPic* p, HwPic* hp) {
    u32 w = p->w, h = p->h, pairs = (w + 1) / 2, cw = 2 * pairs, ch = (h + 1) / 2;
    u32 ly = (u32)up(p->pitch_y, 16), lc = (u32)up(p->pitch_c, 16); /* the deswizzled rows' pitches */
    u32 up4 = (u32)up(pairs, 4), uv_pitch = up4 * 2;                /* staged U/V rows, interleaved rows */
    u64 lin_y = (u64)ly * h, lin_c = (u64)lc * ch, cap;
    u64 at_c = up(lin_y, 16), at_uv = at_c + up(lin_c, 16);
    /* a Vulkan Video picture's rows go after the surfaces' rows: luma w apart, then chroma cw apart */
    u64 at_hy = at_uv, at_hc = up(at_hy + (u64)w * h, 16);
    u64 used = p->hw ? at_hc + (u64)cw * ch : at_uv + (u64)uv_pitch * ch;
    VkBlMove my = {p->pitch_y, p->surf_h, p->bh, 0, 0, h, 0, 1, ly};
    VkBlMove mc = {p->pitch_c, (p->surf_h + 1) / 2, p->bh, 0, 0, ch, 0, 1, lc};
    VkBuffer rows, scratch;
    u64 at_rows, at_crows;
    u32 crow_pitch;
    vk_render_end(); /* copies and compute happen outside rendering passes */
    VkCommandBuffer cb = vk_cmd();
    if (!(scratch = vk_bl_scratch(used, &cap))) {
        if (p->hw) hw_end(hp); /* the picture's semaphores step on all the same */
        return 0;
    }
    if (p->hw) {
        hw_begin(&cb, p, hp, scratch, at_hy, at_hc);
        rows = scratch;
        at_rows = at_hy;
        at_crows = at_hc;
        crow_pitch = cw;
    } else {
        /* the picture's rows, then U's and V's rows padded to words */
        u64 sz_y = up((u64)w * h, 16), sz_uv = (u64)up4 * ch; /* U and V start on words, which the interleave reads */
        u8* s = vk_stage(sz_y + 2 * sz_uv, vk_stage_align(), &rows, &at_rows);
        if (!s) return 0;
        for (u32 y = 0; y < h; y++) memcpy(s + (u64)y * w, p->plane[0] + (size_t)y * (size_t)p->stride[0], w);
        for (u32 y = 0; y < ch; y++) {
            memcpy(s + sz_y + (u64)y * up4, p->plane[1] + (size_t)y * (size_t)p->stride[1], pairs);
            memcpy(s + sz_y + sz_uv + (u64)y * up4, p->plane[2] + (size_t)y * (size_t)p->stride[2], pairs);
        }
        at_crows = at_rows + sz_y; /* until interleaved */
        crow_pitch = uv_pitch;
    }
    VkOwnRes* ry = surface_load(cb, p->luma, p->ext_y);
    VkOwnRes* rc = ry ? surface_load(cb, p->chroma, p->ext_c) : NULL;
    int ok = rc != NULL;
    if (ok) {
        vk_all_barrier(cb);
        /* the surfaces as rows, and a CPU picture's NV12 chroma from U and V */
        ok = vk_bl_move(cb, &my, 0, ry->buf, 0, scratch, 0) && vk_bl_move(cb, &mc, 0, rc->buf, 0, scratch, at_c);
        if (ok && !p->hw) {
            ok = vk_tc_nv12_uv(cb, pairs, ch, rows, at_crows, at_crows + (u64)up4 * ch, up4, scratch, at_uv, uv_pitch);
            at_crows = at_uv;
        }
    }
    if (ok) {
        vk_all_barrier(cb);
        /* the picture's rows over the surfaces' */
        vk_copy_rows(cb, rows, at_rows, w, scratch, 0, ly, w, h);
        vk_copy_rows(cb, scratch, at_crows, crow_pitch, scratch, at_c, lc, cw, ch);
        vk_all_barrier(cb);
        ok = vk_bl_move(cb, &my, 1, ry->buf, 0, scratch, 0) && vk_bl_move(cb, &mc, 1, rc->buf, 0, scratch, at_c);
        vk_all_barrier(cb);
    }
    if (ok) {
        vk_own_publish(p->luma, p->ext_y, ry, 0);
        vk_own_publish(p->chroma, p->ext_c, rc, 0);
    } else
        LOG_LIMIT(4, "video: no GPU memory for an NVDEC picture, or it exceeds the device's limits: dropped\n");
    if (p->hw) hw_end(hp); /* submitted either way: the picture's semaphores step on */
    if (ry) vk_own_res_unref(ry);
    if (rc) vk_own_res_unref(rc);
    return ok;
}
int vk_nvdec_write(const NvdecPic* p) {
    HwPic hp;
    int r;
    if (!p->w || !p->h || (p->hw && !hw_lock(p, &hp))) return 0;
    vk_lock();
    r = nvdec_write(p, &hp);
    vk_unlock();
    if (p->hw) hw_unlock(&hp);
    return r;
}

/* ---- FFmpeg's Vulkan device: this one. Its submissions to the graphics and compute queues (frame setup) take
   vk_lock, after the recording thread's, and the queue's lock (vk_queue_lock), as the renderer's own do; its decode
   queue is its alone */
static void queue_lock(AVHWDeviceContext* c, uint32_t family, uint32_t index) {
    (void)c;
    vk_lock();
    if (family == g_vk.video_family) return;
    vk_rec_sync();
    VkQueue q;
    vkGetDeviceQueue(g_vk.dev, family, index, &q);
    vk_queue_lock(q);
}
static void queue_unlock(AVHWDeviceContext* c, uint32_t family, uint32_t index) {
    (void)c;
    if (family != g_vk.video_family) {
        VkQueue q;
        vkGetDeviceQueue(g_vk.dev, family, index, &q);
        vk_queue_unlock(q);
    }
    vk_unlock();
}
void* vk_nvdec_hw_device(void) {
    AVBufferRef* ref;
    AVHWDeviceContext* dc;
    AVVulkanDeviceContext* v;
    int n = 0;
    if (g_vk.video_family == ~0u || !(ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN))) return NULL;
    dc = (AVHWDeviceContext*)ref->data;
    v = dc->hwctx;
    v->get_proc_addr = vkGetInstanceProcAddr;
    v->inst = g_vk.instance;
    v->phys_dev = g_vk.phys;
    v->act_dev = g_vk.dev;
    v->device_features = g_vk.feat2;
    v->enabled_dev_extensions = g_vk.dev_ext;
    v->nb_enabled_dev_extensions = (int)g_vk.dev_ext_n;
    v->qf[n++] = (AVVulkanDeviceQueueFamily){
        .idx = (int)g_vk.gfx_family,
        .num = g_vk.compute_family == g_vk.gfx_family ? (int)g_vk.gfx_queues : 1,
        .flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT};
    if (g_vk.compute_family != g_vk.gfx_family)
        v->qf[n++] = (AVVulkanDeviceQueueFamily){
            .idx = (int)g_vk.compute_family, .num = 1, .flags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT};
    v->qf[n++] = (AVVulkanDeviceQueueFamily){.idx = (int)g_vk.video_family,
                                             .num = 1,
                                             .flags = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
                                             .video_caps = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR};
    v->nb_qf = n;
#if FF_API_VULKAN_SYNC_QUEUES
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    v->lock_queue = queue_lock;
    v->unlock_queue = queue_unlock;
#pragma clang diagnostic pop
#else
#error "FFmpeg no longer locks queues through the device: the graphics and compute queues need another way"
#endif
    if (av_hwdevice_ctx_init(ref) < 0) {
        WARN_LIMIT(1, "video: FFmpeg could not take the Vulkan device for Vulkan Video: FFmpeg decodes on the CPU\n");
        av_buffer_unref(&ref);
        return NULL;
    }
    return ref;
}
