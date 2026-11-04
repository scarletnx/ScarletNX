/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vulkan renderer: the renderer interface (vk_renderer, the vkr_ functions it points to) and presenting.

   Render targets, clears, draws (gpu/vk/render.c), compute (gpu/vk/compute.c), write-backs to and reloads from
   guest memory, and presenting the game's framebuffer from its render target (or from memory, converted on the CPU,
   when no render target holds it). Everything that records runs under vk_lock(), which is reentrant: submissions and
   presents on the GPU thread (gpu/gpu.c), the menu's refresh on the main thread (vkr_refresh), screenshots on the
   thread asking, and shader results landing on whichever thread reads their memory first. Acquiring a swapchain image
   never waits for the display: with every image still on it or queued for it, that frame isn't shown (counted). */
#include "common/gpu/vk/interp_auto.h"
#include "common/utilities/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/guest_stats.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "common/gpu/vk/present_fx.h"
#include "nx/gpu/vk/hdr.h"
#include "nx/gpu/vk/upscale.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/renderer.h"

static VkSemaphore g_acquired[VK_FRAMES];
static VkSemaphore g_acquired2[VK_FRAMES]; /* the second image of an interpolated frame */
static SnxFx* g_present_fx;
static SnxInterpAuto g_interp_auto; /* frame interpolation's Auto gate */
static void present_fx_log(const char* text) { LOG("%s\n", text); }
/* the CPU fallback's image, and the last presented frame for screenshots */
static Surf* g_cpu_img;
static Surf* g_last;
static u32 g_last_x, g_last_y, g_last_w, g_last_h;
static u32 g_last_transform;
/* vk_lock once around the hooks: each takes it again only as a depth count. Each took and dropped it on its own, and
   every drop let the lock go (vk_let_go) on the GPU thread's writes, the constant buffer uploads before most draws */
static void vkr_before_write(u64 gva, u64 size) {
    vk_lock();
    if (size) vk_release_need(gva, size);
    vk_buf_before_write(gva, size);
    if (size) vk_uma_before_write(gva, size); /* what reads it in place runs first */
    if (size)
        vk_gfx_image_mem_need(gva, size);
    else
        vk_gfx_image_land();
    vk_compute_before_write(gva, size);
    vk_rt_before_write(gva, size);
    vk_unlock();
}
/* pending compute and graphics results and queued releases all count their pages in g_pend_any (pending_pages.h);
   storage images don't, so any of them pending answers yes */
static int vkr_mem_pending(u64 gva, u64 size) {
    int hit;
    if (g_gfx_img_any) return 1;
    vk_lock();
    hit = pend_hit(&g_pend_any, gva, size);
    vk_unlock();
    return hit;
}
static void vkr_gfx_mem_need(u64 gva, u64 size) {
    vk_buf_mem_need(gva, size);
    vk_uma_mem_need(gva, size); /* the in-place writes there done */
    vk_gfx_image_mem_need(gva, size);
}
/* the GPU range a mapping change is changing, from its begin to its end (size 0 for a change of guest memory) */
static u64 g_mapping_gva, g_mapping_size;
static void vkr_mapping_begin(u64 gva, u64 size, int full) {
    vk_lock();
    g_mapping_gva = full ? 0 : gva;
    g_mapping_size = full ? 0 : size;
    vk_release_drain();
    if (full) {
        vk_buf_land_map();
        vk_gfx_image_land();
        vk_compute_land();
        /* last, as the landings may submit: mem.c's change of guest memory [gva, gva + size), whose in-place readers
           run and reach the driver first */
        vk_uma_mapping_begin(gva, size);
    } else {
        vk_buf_mem_need_map(gva, size);
        vk_gfx_image_mem_need(gva, size);
        vk_compute_mem_need_map(gva, size);
        /* Images still describe the old backing until the page table changes. */
        vk_rt_before_write(gva, size);
    }
}
/* the pages changed count what is pending at the guest pages they show now. A change of guest memory (full) leaves
   the GMMU as it was: pending pages are compared by address, and freed memory is landed first */
static void vkr_mapping_end(void) {
    vk_uma_mapping_end(); /* first: what follows may submit */
    vk_buf_mapping_changed();
    pend_remap(g_mapping_gva, g_mapping_size);
    g_mapping_size = 0;
    vk_unlock();
}

/* 0 when Vulkan can't run here */
static int vkr_start(void) {
    if (!vk_init()) return 0;
    g_vk_scale = g_settings.resolution_scale;       /* the .ini keeps resolution within 0.25 to 16 (options.c) */
    g_vk_anisotropy = g_settings.anisotropic_filtering; /* the sampler table keeps what it makes: anisotropy applies at start */
    vk_upscale_configure(snx_upscale_parse(g_settings.upscaler), g_settings.sharpness);
    {
        /* the test pattern is drawn in the HDR passes: it takes them even where hdr would be off */
        int mode = snx_hdr_parse(g_settings.hdr);
        if (g_settings.hdr_test_pattern && mode == SNX_HDR_OFF) mode = SNX_HDR_ON;
        vk_hdr_configure(mode, snx_hdr_source_parse(g_settings.hdr_source), g_settings.hdr_paper_white,
                         g_settings.hdr_peak, g_settings.hdr_expansion, g_settings.hdr_test_pattern);
    }
    g_present_fx = snx_fx_create(g_vk.phys, g_vk.dev, g_vk.gfx, g_vk.gfx_family, vk_pipeline_cache(), VK_FRAMES,
                                 vk_frame_slot, vk_render_forget_state, present_fx_log);
    if (g_present_fx) {
        SnxFxConfig cfg = {SNX_FX_UPSCALE_OFF, 0, snx_fx_aa_parse(g_settings.anti_aliasing), SNX_FX_HDR_OFF,
                           SNX_FX_CS_AUTO, SNX_FX_SRC_AUTO, g_settings.hdr_paper_white, g_settings.hdr_peak,
                           g_settings.hdr_expansion, 0};
        snx_fx_configure(g_present_fx, &cfg);
        snx_fx_set_fp16(g_present_fx, g_shader_float16);
    }
    if (g_vk_anisotropy > 1 && !g_vk.feat.samplerAnisotropy)
        LOG("vulkan: this GPU has no anisotropic filtering, so anisotropy=%d does nothing\n", g_vk_anisotropy);
    if (g_vk_scale != 1.0f) LOG("vulkan: render targets at %.2fx\n", g_vk_scale);
    vk_cmd_init();
    vk_pipeline_init();
    vk_own_init(); /* GPU engines' results stay on the GPU where guest memory can be trapped (memory/own.c) */
    g_gpu_before_write = vkr_before_write;
    g_gpu_mapping_begin = vkr_mapping_begin;
    g_gpu_mapping_end = vkr_mapping_end;
    g_gpu_mem_changed = vk_mem_changed;
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!g_opt_headless)
        for (int i = 0; i < VK_FRAMES; i++)
            if (!VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_acquired[i])) ||
                !VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_acquired2[i])))
                return 0;
    g_renderer_up = 1; /* a renderer is active */
    return 1;
}

/* ------------------------------------------------------------ the front end's calls */
/* a new epoch: the caches that trust what they checked in this one (buffers, constant buffers, shaders) check again */
static void vkr_mem_may_have_changed(void) { vk_ww_new_epoch(); }
static void vkr_draw_texture(const DsState* ds, const DrawTexture* t) {
    vk_lock();
    vk_render_draw_texture(ds, t);
    vk_unlock();
}
static void vkr_discard(const DsState* ds, u32 targets) {
    vk_lock();
    vk_render_discard(ds, targets);
    vk_unlock();
}
static void vkr_clear(const DsState* ds, const DrawClear* c, const DrawOwner* o) {
    vk_lock();
    vk_render_clear(ds, c, o);
    vk_unlock();
}
static void vkr_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    /* a draw predicated on the GPU (gpu.h render_pred) counts too: whether it runs isn't known here without the wait
       the predicate is there to avoid, and the counters made up from this (maxwell/engines.c counter_report) only
       stand in for what the console counts, growing with the work given */
    g_draws++;
    vk_lock();
    vk_render_draw(ds, dc, o);
    vk_unlock();
}
static void vkr_samples_reset(u32 owner) {
    vk_lock();
    vk_query_reset(owner);
    vk_unlock();
}
static void vkr_samples_enable(u32 owner, int on) {
    vk_lock();
    vk_query_enable(owner, on);
    vk_unlock();
}
static int vkr_samples_report(u32 owner, u64 gva, u32 len, u64 ticks) {
    vk_lock();
    int r = vk_query_report(owner, gva, len, ticks);
    vk_unlock();
    return r;
}
static void vkr_method_barrier(int kind)
{
	VkMemoryBarrier2 mb = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT |
				 VK_ACCESS_2_MEMORY_WRITE_BIT,
	};
	VkDependencyInfo di = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.memoryBarrierCount = 1,
		.pMemoryBarriers = &mb,
	};

	/* Texture descriptors are read at every bind. Surface transitions and
	 * memory write tracking supply their ordering without expiring caches.
	 * A fragment barrier (NVN's ORDER_FRAGMENTS and INVALIDATE_TEXTURE)
	 * mostly orders what the renderer orders itself: a target sampled after
	 * it was drawn gets its barrier when it is bound (vk_surf_use), one
	 * sampled while it is drawn gets one in the pass (feedback_order), and
	 * draws that read results of earlier ones get one there (buffer.c
	 * gfx_gpu_src). Ended here, the pass ended about 120 times a frame in one
	 * game's open world. Writes that none of those track (stores into guest
	 * memory read in place) are not covered yet, so only the opt-in
	 * vk_descriptors setting skips it.
	 */
	if (kind == 1 || kind == 3 || (kind == 0 && g_settings.descriptor_buffers))
		return;

	vk_lock();
	if ((kind == 6 || kind == 7) && g_settings.nvn_storage_ordering == 2) {
		vk_buf_order_request(kind == 7);
		vk_unlock();
		return;
	}
	/* Surface and copy consumers retain their own dependencies. NVN's points need an additional buffer
	 * barrier only after a shader write or a pending read that a later in-place writer could overwrite. */
	if ((kind == 6 || kind == 7) && !vk_buf_order_pending(kind == 7)) {
		vk_unlock();
		return;
	}
	if (kind == 5) {
		/* Content caches keep their modules. Only address shortcuts expire. */
		g_vk.shader_generation++;
		vk_unlock();
		return;
	}
	vk_render_end();
	/* CPU-visible releases wait for completion in the release path. */
	rec_CmdPipelineBarrier2(vk_cmd(), &di);
	vk_buf_ordered();
	vk_unlock();
}
static int vkr_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                    double sdy) {
    vk_lock();
    int r = vk_blit2d(s, d, dx, dy, dw, dh, sx, sy, sdx, sdy);
    vk_unlock();
    return r; /* 0: the front end copies through memory, with the renderer's rt_flush and rt_reload around it */
}
static int vkr_vic_compose(const VicJob* j) { return vk_vic_compose(j); } /* it takes vk_lock */
static int vkr_nvdec_write(const NvdecPic* p) { return vk_nvdec_write(p); }   /* it takes vk_lock */
static void* vkr_nvdec_hw_device(void) { return vk_nvdec_hw_device(); }
static int vkr_dma_copy(const GpuDmaOp* o) { return vk_dma_copy(o); }         /* it takes vk_lock */
static int vkr_blit_mem(const GpuBlit* b) { return vk_blit_mem(b); }          /* it takes vk_lock */
static void vkr_rt_flush(u64 gva) {
    vk_lock();
    vk_gfx_image_mem_need(gva, 1);
    vk_rt_flush(gva);
    vk_unlock();
}
static void vkr_rt_reload(u64 gva) {
    vk_lock();
    vk_rt_reload(gva);
    vk_unlock();
}
static void vkr_dispatch(const DispatchState* d) {
    if (!g_settings.compute_dispatches) return;
    r3d_pending_flush(); /* a merged draw the front end is holding comes first */
    vk_lock();
    vk_dispatch(d);
    vk_unlock();
}
/* Pending shader results land before the stream signals or waits. */
static void vkr_before_syncwait(void) {
    SNX_ZONE(z, "vk before syncwait");
    vk_buf_land_syncwait();
    vk_gfx_image_land();
    vk_compute_land();
}
static void vkr_stream_incr(u32 id) { vk_release_incr(id); }

/* ------------------------------------------------------------ presenting */
/* the frame from memory when no render target holds it (the game drew it on the CPU, or a video frame): staged as it
   is, deswizzled and converted on the GPU (vk/blocklinear.c, vk/texconv.c) */
static Surf* cpu_frame(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt) {
    if (g_cpu_img && (g_cpu_img->w != w || g_cpu_img->h != h)) {
        vk_defer_image(g_cpu_img->img, g_cpu_img->mem, VK_NULL_HANDLE, g_cpu_img->att);
        if (g_last == g_cpu_img) g_last = NULL;
        free(g_cpu_img);
        g_cpu_img = NULL;
    }
    if (!g_cpu_img) {
        g_cpu_img = calloc(1, sizeof *g_cpu_img);
        if (!g_cpu_img) return NULL;
        g_cpu_img->w = g_cpu_img->sw = w;
        g_cpu_img->h = g_cpu_img->sh = h;
        g_cpu_img->levels = 1;
        g_cpu_img->type = VK_IMAGE_VIEW_TYPE_2D;
        g_cpu_img->f = vk_rt_format(0xD5); /* RGBA8 */
        g_cpu_img->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .extent = {w, h, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &g_cpu_img->img, &g_cpu_img->mem, NULL))) {
            free(g_cpu_img);
            g_cpu_img = NULL;
            return NULL;
        }
        vk_mem_track(g_cpu_img->mem);
    }
    /* its bytes, staged as they are: a block linear one's whole rows of blocks, a pitch linear one's rows (and
       16 bytes, as a conversion reads whole words and a row of 2 byte pixels may end inside one) */
    u32 bpp = fmt == 4 || fmt == 6 || fmt == 7 ? 2 : 4, row_bytes = w * bpp;
    u32 stride = pitch > row_bytes ? pitch : row_bytes, bl = layout == 3; /* libnx's NvLayout: 3 block linear */
    VkBlMove m = {stride, h, bhl, 0, 0, h, 0, 1, (stride + 15) & ~15u};
    u64 lo = 0, hi = (u64)stride * h, at, cap;
    if (bl) vk_bl_range(&m, &lo, &hi);
    if (cpu_addr >= AS_SIZE || hi > AS_SIZE - cpu_addr) {
        WARN_LIMIT(5, "vi: buffer at %llx runs outside guest memory, not shown\n", (unsigned long long)cpu_addr);
        return NULL;
    }
    /* a pitch linear one whose rows don't start on words is staged a row at a time into rows that do */
    u32 in_pitch = bl ? m.lin_pitch : stride % 4 ? (row_bytes + 3) & ~3u : stride;
    u64 staged_size = bl || in_pitch == stride ? hi : (u64)in_pitch * h;
    VkBuffer staged;
    u8* p = vk_stage(staged_size + 16, vk_stage_align(), &staged, &at);
    if (!p) return NULL;
    /* a CPU address (nvmap's), in the guest's address space: what the GPU holds of it (VIC's picture) is copied on
       the GPU */
    if (bl || in_pitch == stride)
        vk_cpu_read(cpu_addr, hi, p, staged, at);
    else
        for (u32 y = 0; y < h; y++)
            vk_cpu_read(cpu_addr + (u64)y * stride, row_bytes, p + (u64)y * in_pitch, staged, at + (u64)y * in_pitch);
    memset(p + staged_size, 0, 16);
    /* deswizzled and converted to RGBA8 on the GPU, in the scratch buffer: the linear rows, then the converted ones */
    int conv = vk_tc_vi_converts(fmt);
    VkTcShape tc = conv ? vk_tc_vi_shape(fmt, w) : (VkTcShape){.out_texels = in_pitch / 4};
    u64 lin = bl ? (u64)m.lin_pitch * h : 0, out = (lin + 15) & ~15ull;
    u64 need = out + (conv ? (u64)tc.out_texels * 4 * h : 0);
    VkBuffer scratch = need ? vk_bl_scratch(need, &cap) : VK_NULL_HANDLE;
    if (need && !scratch) return NULL;
    VkCommandBuffer cb = vk_cmd();
    VkBuffer from = staged;
    u64 from_at = at;
    if (bl || conv) vk_bl_barrier(cb, 1);
    if (bl) {
        if (!vk_bl_move(cb, &m, 0, staged, at, scratch, 0)) return NULL;
        vk_bl_barrier(cb, 0);
        from = scratch;
        from_at = 0;
    }
    if (conv) {
        if (!vk_tc_convert_vi(cb, fmt, w, h, from, from_at, in_pitch, scratch, out)) return NULL;
        vk_bl_barrier(cb, 0);
        from = scratch;
        from_at = out;
    }
    vk_surf_use(cb, g_cpu_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy c = {.bufferOffset = from_at,
                           .bufferRowLength = tc.out_texels,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {w, h, 1}};
    rec_CmdCopyBufferToImage(cb, from, g_cpu_img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    return g_cpu_img;
}

static int is_srgb(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

static int g_sc_mode = VK_SC_UNORM; /* the swapchain's kind the last create asked for (a surface may refuse HDR10) */
/* the window's surface: lost while the app is in the background (Android), made anew when it comes back */
static u32 g_generation;
static int g_lost;
static u64 g_interp_pairs, g_interp_plain; /* frames shown as a blend and a frame, and those that could not */
static u64 g_shown_ns; /* when the game last gave the window a frame (atomic: vkr_refresh reads it unlocked) */
/* a blit's source flipped as the frame's Android transform says (FlipH 1, FlipV 2): the endpoints swapped */
static void flip_src(VkImageBlit* b, u32 transform) {
    if (transform & 1) {
        i32 x = b->srcOffsets[0].x;
        b->srcOffsets[0].x = b->srcOffsets[1].x;
        b->srcOffsets[1].x = x;
    }
    if (transform & 2) {
        i32 y = b->srcOffsets[0].y;
        b->srcOffsets[0].y = b->srcOffsets[1].y;
        b->srcOffsets[1].y = y;
    }
}
/* a source image's crop to the window's rectangle, in guest order flipped as the frame's transform says */
static void present_blit(VkCommandBuffer cb, VkImage img, u32 x, u32 y, u32 w, u32 h, VkImage sc, i32 dx, i32 dy,
                         u32 dw, u32 dh, u32 transform, VkFilter filter) {
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{(i32)x, (i32)y, 0}, {(i32)(x + w), (i32)(y + h), 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{dx, dy, 0}, {dx + (i32)dw, dy + (i32)dh, 1}}};
    flip_src(&blit, transform);
    rec_CmdBlitImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                     filter);
}

#if defined(SNX_TRACY)
/* The heap the caches manage, as vk_mem_use reads it for their own trimming, in bytes (the plot is a Memory one). */
static void gpu_memory_plots(void) {
    u64 used, budget;
    vk_mem_use(&used, &budget);
    SNX_PLOT("gpu memory", used);
    SNX_PLOT("gpu memory budget", budget);
}
#endif

/* One image of the window filled with a frame: the frame's crop letterboxed on black (the upscaler's or the
   effects' output when they ran), through the HDR composition and the encode when hdr, else a plain blit, with the
   menu and the controls over it. The image comes back in PRESENT_SRC_KHR. */
typedef struct {
    VkImage sc;
    VkImageView view; /* the frame the HDR passes compose, or VK_NULL_HANDLE */
    u32 vw, vh;       /* the view's pixels */
    u32 vx, vy, vw_crop, vh_crop;
    u32 transform;
    int source;       /* SNX_HDR_SRC_* */
    VkImage blit;     /* the plain blit's source, when HDR is off or cannot run */
    u32 bx, by, bw, bh;
    u32 btransform;
    VkFilter bfilter;
    i32 dx, dy;
    u32 dw, dh;
    int enc;          /* VK_HDR_ENC_* */
    const int* safe;
} PresentedPic;

static void draw_presented(VkCommandBuffer cb, const PresentedPic* p, int hdr, VkPipelineStageFlags2* end_stage,
                           VkAccessFlags2* end_access, VkImageLayout* end_layout) {
    u32 sw = g_vk.sc_extent.width, sh = g_vk.sc_extent.height;
    if (hdr && p->view) {
        VkImage comp_img = VK_NULL_HANDLE;
        VkImageView comp_view = VK_NULL_HANDLE;
        if (vk_hdr_compose(cb, p->view, p->source, p->vw, p->vh, p->vx, p->vy, p->vw_crop, p->vh_crop, p->transform,
                           sw, sh, p->dx, p->dy, p->dw, p->dh, &comp_img, &comp_view)) {
            VkUiTarget t = {.img = comp_img,
                            .view = comp_view,
                            .fmt = vk_hdr_compose_format(),
                            .extent = {sw, sh},
                            .mode = VK_UI_HDR_NITS,
                            .gain = (float)vk_hdr_paper_nits(),
                            .from_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            .from_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            .from_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            vk_ui_draw(cb, &t, p->safe);
            if (vk_hdr_encode(cb, comp_img, comp_view, sw, sh, p->sc, vk_ui_image_view(p->sc, g_vk.sc_format),
                              g_vk.sc_format, p->enc)) {
                *end_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                *end_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                *end_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                return;
            }
        }
    }
    present_blit(cb, p->blit, p->bx, p->by, p->bw, p->bh, p->sc, p->dx, p->dy, p->dw, p->dh, p->btransform,
                 p->bfilter);
    {
        VkUiTarget t = {.img = p->sc,
                        .view = VK_NULL_HANDLE,
                        .fmt = g_vk.sc_format,
                        .extent = g_vk.sc_extent,
                        .mode = g_vk.sc_srgb ? VK_UI_SRGB : VK_UI_ENCODED,
                        .gain = 1.0f,
                        .from_stage = VK_PIPELINE_STAGE_2_BLIT_BIT,
                        .from_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        .from_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
        if (vk_ui_draw(cb, &t, p->safe)) {
            *end_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            *end_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            *end_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }
}

/* the display's refresh in Hz, 0 when the backend does not know it */
static int display_hz(void) {
    u64 refresh = backend_display_refresh_ns();
    return refresh ? (int)(1000000000ull / refresh) : 0;
}

/* The window's image: src's g_last_ rectangle (a frame of fw x fh, flipped by transform) letterboxed on black, or
   black alone, with the on-screen controls and the game's menu over it; then the frame's commands are submitted and
   the image presented. A game's frame (game, which the game released at released_ns, 0 unknown, at its pace of a
   frame every period_ns, 0 none) also moves the per-present bookkeeping, and without src shows nothing; its
   shown_syncpt (0 none) goes up once it is displayed, or known not to be (renderer.h). Under vk_lock */
static void show(Surf* src, u32 fw, u32 fh, u32 transform, int game, u64 released_ns, u64 period_ns,
                 u32 shown_syncpt) {
    SNX_ZONE(z_show, "vk show");
    /* the size the swapchain was made for, not the extent the surface clamped it to: that can stay different */
    int ww, wh, shown = backend_window_shown();
    /* the image the window is given: HDR10's for the hdr setting, else the encoding of the frame presented */
    int want_mode = vk_hdr_active() ? VK_SC_HDR
                    : !src                     ? g_sc_mode
                    : is_srgb(src->f.vk)        ? VK_SC_SRGB
                                                : VK_SC_UNORM;
    backend_window_size(&ww, &wh);
    if (shown && (g_lost || backend_window_generation() != g_generation)) {
        vk_finish();
        g_lost = !vk_surface_recreate();
        g_generation = backend_window_generation();
    }
    shown = shown && !g_lost;
    int tried = 0; /* a create already ran this frame: one that failed waits for the next frame */
    /* a new vsync in the .ini takes a new swapchain, as a new size does */
    if (shown && ww > 0 && wh > 0 &&
        (ww != g_vk.sc_win_w || wh != g_vk.sc_win_h || want_mode != g_sc_mode ||
         __atomic_load_n(&g_settings.vsync, __ATOMIC_RELAXED) != g_vk.sc_vsync)) {
        vk_finish();
        g_sc_mode = want_mode;
        vk_swapchain_create(want_mode);
        tried = 1;
    }
    VkSemaphore wait = VK_NULL_HANDLE, signal = VK_NULL_HANDLE;
    VkSemaphore acq2 = VK_NULL_HANDLE, signal2 = VK_NULL_HANDLE; /* the second image of an interpolated frame */
    u32 idx = 0, idx2 = 0;
    int show2 = 0, blend_shown = 0;
    int recreate = 0;
    if (shown && (src || !game) && (g_vk.swapchain || (!tried && vk_swapchain_create(want_mode)))) {
        VkCommandBuffer cb = vk_cmd(); /* waits for this frame slot, so its acquire semaphore is free */
        VkSemaphore acq = g_acquired[vk_frame_id() % VK_FRAMES];
        /* no wait: vk_lock is held (guest threads mapping memory take it too), and the game's pace is present_pace's.
           Every image still on the display or queued for it (FIFO behind) leaves this frame unshown */
        pm_lock(&g_vk.sc_lock);
        VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, 0, acq, VK_NULL_HANDLE, &idx);
        pm_unlock(&g_vk.sc_lock);
        if (r == VK_NOT_READY || r == VK_TIMEOUT) {
            /* the game faster than the display (vsync=1 with fps_limit over its refresh, or a 59.94 Hz one) */
            static u64 dropped;
            if (game) vk_display_dropped();
            if (game && ++dropped % 600 == 1)
                LOG("vulkan: %llu frames not shown, the display had every swapchain image\n",
                    (unsigned long long)dropped);
        } else if (r == VK_ERROR_OUT_OF_DATE_KHR)
            recreate = 1;
        else if (r == VK_ERROR_SURFACE_LOST_KHR)
            g_lost = 1;
        else if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            wait = acq;
            signal = g_vk.sc_done[idx];
            VkImage sc = g_vk.sc_images[idx];
            /* Frame interpolation shows each game frame in two window images: the blend of the frame before it and
               itself, then itself. The second image is taken without waiting, so a display still holding every image
               makes this frame a plain one instead of stalling the guest */
            acq2 = g_acquired2[vk_frame_id() % VK_FRAMES];
            if (game && src && g_present_fx) {
                int mode = snx_fx_interp_parse(g_settings.frame_interpolation);
                int dhz = display_hz();
                if (mode == SNX_FX_INTERP_BLEND ||
                    (mode == SNX_FX_INTERP_AUTO &&
                     snx_interp_auto_frame(&g_interp_auto, released_ns ? released_ns : plat_ns(), dhz))) {
                    VkResult r2;
                    pm_lock(&g_vk.sc_lock);
                    r2 = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, 0, acq2, VK_NULL_HANDLE, &idx2);
                    pm_unlock(&g_vk.sc_lock);
                    if (r2 == VK_SUCCESS || r2 == VK_SUBOPTIMAL_KHR) {
                        show2 = 1;
                        signal2 = g_vk.sc_done[idx2];
                        if (!g_interp_pairs)
                            LOG("vulkan: frame interpolation %s: a blend and the frame itself for each game frame "
                                "(display %d Hz)\n",
                                snx_fx_interp_label(mode), dhz);
                        g_interp_pairs++;
                    } else
                        g_interp_plain++;
                }
            }
            /* the submit waits for the acquire at the clear, the image's first use */
            vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue black = {{0, 0, 0, 1}};
            VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            rec_CmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
            vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            /* letterbox: the largest rectangle of the frame's aspect that fits the window's part clear of the system's
               bars, centered there; the clear part scales with the extent the surface may have clamped */
            u32 sw = g_vk.sc_extent.width, sh = g_vk.sc_extent.height, cx = 0, cy = 0, cw = sw, ch = sh, dw, dh;
            int sx, sy, sww, swh;
            backend_window_safe(&sx, &sy, &sww, &swh);
            if (g_vk.sc_win_w > 0 && g_vk.sc_win_h > 0 && sww > 0 && swh > 0 && sx + sww <= g_vk.sc_win_w &&
                sy + swh <= g_vk.sc_win_h) {
                cx = (u32)((u64)sw * (u32)sx / (u32)g_vk.sc_win_w);
                cy = (u32)((u64)sh * (u32)sy / (u32)g_vk.sc_win_h);
                cw = (u32)((u64)sw * (u32)sww / (u32)g_vk.sc_win_w);
                ch = (u32)((u64)sh * (u32)swh / (u32)g_vk.sc_win_h);
            }
            /* what an HDR image here holds: scRGB where the window's surface took it, else PQ */
            int enc = g_vk.sc_p3      ? VK_HDR_ENC_P3
                      : g_vk.sc_scrgb ? VK_HDR_ENC_SCRGB
                      : g_vk.sc_hdr   ? VK_HDR_ENC_PQ
                                      : VK_HDR_ENC_SDR;
            int safe[4] = {(int)cx, (int)cy, (int)cw, (int)ch};
            /* the state of each window's image after the frame's drawing, for its present */
            int use_hdr = 0;
            PresentedPic pic[2];
            u32 npic = 0;
            if (src && fw && fh) {
                dw = cw;
                dh = (u32)((u64)cw * fh / fw);
                if (dh > ch) {
                    dh = ch;
                    dw = (u32)((u64)ch * fw / fh);
                }
                i32 dx = (i32)(cx + (cw - dw) / 2), dy = (i32)(cy + (ch - dh) / 2);
                /* the upscaler writes the screen rectangle times upscaler_mult, at most the device's largest image */
                u32 uw, uh;
                u32 mult = vk_upscale_target(dw, dh, (u32)g_settings.upscaling_target_mult,
                                             g_vk.props.limits.maxImageDimension2D, &uw, &uh);
                int filter = vk_upscale_pick(g_last_w, g_last_h, uw, uh, transform);
                int source = vk_hdr_source_for(src->f.vk);
                /* HDR of the display, or of a frame that already holds HDR data: the frame's crop into the
                   composition (linear light in nits), the menu over it, and the encode into the window's image.
                   The upscaler, when it runs, offers its output as that crop */
                VkImageView frame_view = VK_NULL_HANDLE;
                u32 fw2 = src->sw, fh2 = src->sh, fx = g_last_x, fy = g_last_y, fwid = g_last_w, fhei = g_last_h,
                    ftransform = transform;
                VkImage up_img = VK_NULL_HANDLE, aa_img = VK_NULL_HANDLE;
                VkImageView up_view = VK_NULL_HANDLE, aa_view = VK_NULL_HANDLE;
                u32 aa_w = 0, aa_h = 0;
                int upscaled = 0;
                use_hdr = vk_hdr_available() && g_vk.sc_hdr && vk_hdr_active();
                if (!use_hdr && source != SNX_HDR_SRC_SDR) use_hdr = vk_hdr_available();
                if (filter > SNX_UPSCALE_BILINEAR) {
                    VkComponentMapping identity = {0};
                    /* the filter works in the space the final blit leaves: sRGB frames are sampled through their
                       sRGB view (linear), and the blit to an sRGB swapchain encodes once; an sRGB frame shown on a
                       non-sRGB swapchain keeps its encoded bytes, and so does a CPU frame */
                    VkImageView src_view =
                        vk_tex_view(src, VK_IMAGE_VIEW_TYPE_2D, is_srgb(src->f.vk) && g_vk.sc_srgb, &identity, 0);
                    vk_surf_use(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                    if (src_view)
                        upscaled = vk_upscale_process(cb, src_view, src->sw, src->sh, g_last_x, g_last_y, g_last_w,
                                                      g_last_h, transform, uw, uh, filter, g_settings.sharpness,
                                                      &up_img, &up_view);
                    if (upscaled) {
                        frame_view = up_view;
                        fw2 = uw;
                        fh2 = uh;
                        fx = fy = 0;
                        fwid = uw;
                        fhei = uh;
                        ftransform = 0;
                    }
                }
                if (g_present_fx) {
                    int aa = snx_fx_pick_aa(g_present_fx, snx_fx_aa_parse(g_settings.anti_aliasing), g_last_w, g_last_h,
                                            dw, dh, g_vk.props.limits.maxImageDimension2D);
                    snx_fx_set_aa(g_present_fx, aa);
                    if (aa != SNX_FX_AA_OFF) {
                        VkImageView aa_src = frame_view;
                        u32 aw = fw2, ah = fh2, ax = fx, ay = fy, awid = fwid, ahei = fhei, atransform = ftransform;
                        /* the recording thread has begun this frame's command buffer once its ring drained */
                        vk_rec_sync();
                        if (!aa_src) {
                            VkComponentMapping identity = {0};
                            aa_src = vk_tex_view(src, VK_IMAGE_VIEW_TYPE_2D, 0, &identity, 0);
                            vk_surf_use(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                            aw = src->sw;
                            ah = src->sh;
                            ax = g_last_x;
                            ay = g_last_y;
                            awid = g_last_w;
                            ahei = g_last_h;
                            atransform = transform;
                        } else if (upscaled) {
                            vk_barrier(cb, up_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        }
                        {
                            SnxFxImage fxout = {0};
                            if (aa_src &&
                                snx_fx_render(g_present_fx, cb, aa_src, aw, ah, ax, ay, awid, ahei, atransform, awid,
                                              ahei, SNX_FX_UPSCALE_OFF, aa, &fxout)) {
                                aa_img = fxout.image;
                                aa_view = fxout.view;
                                aa_w = fxout.width;
                                aa_h = fxout.height;
                                frame_view = aa_view;
                                fw2 = aa_w;
                                fh2 = aa_h;
                                fx = fy = 0;
                                fwid = aa_w;
                                fhei = aa_h;
                                ftransform = 0;
                            }
                        }
                    }
                }
                /* the frame the blend and the keep copy read, and the layout it is in: the effects leave their
                   output in TRANSFER_SRC, the frame itself is sampled */
                VkImage frame_img = src->img;
                VkImageLayout frame_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if (aa_img) {
                    frame_img = aa_img;
                    frame_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                } else if (upscaled) {
                    frame_img = up_img;
                    frame_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                }
                /* The blend of the frame before and this one, and this one in the keep image for the next blend.
                   The passes write the descriptor sets they bind, so the recording thread must have taken the
                   frame's commands before they change */
                SnxFxImage blend = {0};
                int have_blend = 0;
                if (show2 && frame_view) {
                    vk_rec_sync();
                    if (frame_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                        vk_barrier(cb, frame_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT, frame_layout, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    {
                        SnxFxImage prev = {0};
                        if (snx_fx_keep_previous(g_present_fx, &prev) && prev.width == fw2 && prev.height == fh2) {
                            vk_barrier(cb, prev.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                            have_blend =
                                snx_fx_blend(g_present_fx, cb, prev.view, frame_view, fw2, fh2, 0.5f, &blend);
                            vk_barrier(cb, prev.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        }
                    }
                    {
                        SnxFxImage kept = {0};
                        snx_fx_keep(g_present_fx, cb, frame_view, fw2, fh2, &kept);
                    }
                    if (frame_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                        vk_barrier(cb, frame_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                   frame_layout);
                }
                if (use_hdr && !frame_view) {
                    VkComponentMapping identity = {0};
                    frame_view = vk_tex_view(src, VK_IMAGE_VIEW_TYPE_2D, 0, &identity, 0);
                    vk_surf_use(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                }
                if (use_hdr) {
                    if (upscaled)
                        vk_barrier(cb, up_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    else if (aa_img)
                        vk_barrier(cb, aa_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    if (have_blend)
                        vk_barrier(cb, blend.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                /* the frame itself */
                pic[npic++] = (PresentedPic){
                    .view = frame_view,
                    .vw = fw2,
                    .vh = fh2,
                    .vx = fx,
                    .vy = fy,
                    .vw_crop = fwid,
                    .vh_crop = fhei,
                    .transform = ftransform,
                    .source = source,
                    .blit = aa_img ? aa_img : upscaled ? up_img : src->img,
                    .bx = aa_img || upscaled ? 0 : g_last_x,
                    .by = aa_img || upscaled ? 0 : g_last_y,
                    .bw = aa_img ? aa_w : upscaled ? uw : g_last_w,
                    .bh = aa_img ? aa_h : upscaled ? uh : g_last_h,
                    .btransform = aa_img || upscaled ? 0 : transform,
                    .bfilter = aa_img || upscaled ? (mult > 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST) : VK_FILTER_LINEAR,
                    .dx = dx,
                    .dy = dy,
                    .dw = dw,
                    .dh = dh,
                    .enc = enc,
                    .safe = safe};
                /* the blend, before the frame it comes from */
                if (show2) {
                    if (have_blend)
                        pic[npic++] = (PresentedPic){.view = blend.view,
                                                     .vw = blend.width,
                                                     .vh = blend.height,
                                                     .vw_crop = blend.width,
                                                     .vh_crop = blend.height,
                                                     .source = source,
                                                     .blit = blend.image,
                                                     .bw = blend.width,
                                                     .bh = blend.height,
                                                     .bfilter = VK_FILTER_NEAREST,
                                                     .dx = dx,
                                                     .dy = dy,
                                                     .dw = dw,
                                                     .dh = dh,
                                                     .enc = enc,
                                                     .safe = safe};
                    /* the second image is acquired for a pair: with no blend it still shows this frame */
                    if (npic == 1) pic[npic] = pic[0];
                    npic = 2;
                    blend_shown = have_blend;
                }
            }
            if (npic) {
                for (u32 i = 0; i < npic; ++i) {
                    VkImage target = i ? g_vk.sc_images[idx2] : sc;
                    VkPipelineStageFlags2 end_stage = VK_PIPELINE_STAGE_2_BLIT_BIT;
                    VkAccessFlags2 end_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    VkImageLayout end_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    pic[i].sc = target;
                    if (i) { /* the second image waits for its acquire at its clear */
                        vk_barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, 0,
                                   VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        rec_CmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
                        vk_barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    }
                    draw_presented(cb, &pic[i], use_hdr, &end_stage, &end_access, &end_layout);
                    vk_barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT, end_stage, end_access, end_layout,
                               VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                }
            } else {
                if (vk_hdr_active() && g_vk.sc_hdr && vk_hdr_available()) {
                    /* the menu's screen or the shader loading one, with no frame under it: its colours still go
                       through the composition, so the PQ image holds them in its encoding, not as the codes of
                       another one */
                    VkImage comp = VK_NULL_HANDLE;
                    VkImageView comp_view = VK_NULL_HANDLE;
                    if (vk_hdr_compose(cb, VK_NULL_HANDLE, SNX_HDR_SRC_SDR, 0, 0, 0, 0, 0, 0, 0, sw, sh, 0, 0, sw,
                                       sh, &comp, &comp_view)) {
                        VkUiTarget t = {.img = comp,
                                        .view = comp_view,
                                        .fmt = vk_hdr_compose_format(),
                                        .extent = {sw, sh},
                                        .mode = VK_UI_HDR_NITS,
                                        .gain = (float)vk_hdr_paper_nits(),
                                        .from_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        .from_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                        .from_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                        vk_ui_draw(cb, &t, safe);
                        if (vk_hdr_encode(cb, comp, comp_view, sw, sh, sc, vk_ui_image_view(sc, g_vk.sc_format),
                                          g_vk.sc_format, enc)) {
                            vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT,
                                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                            use_hdr = 1;
                        }
                    }
                }
                if (!use_hdr) { /* the menu and the controls over the window's image, as the blit leaves it */
                    VkUiTarget t = {.img = sc,
                                    .view = VK_NULL_HANDLE,
                                    .fmt = g_vk.sc_format,
                                    .extent = g_vk.sc_extent,
                                    .mode = g_vk.sc_srgb ? VK_UI_SRGB : VK_UI_ENCODED,
                                    .gain = 1.0f,
                                    .from_stage = VK_PIPELINE_STAGE_2_BLIT_BIT,
                                    .from_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .from_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
                    VkPipelineStageFlags2 st = VK_PIPELINE_STAGE_2_BLIT_BIT;
                    VkAccessFlags2 ac = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    VkImageLayout ly = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    if (vk_ui_draw(cb, &t, safe)) {
                        st = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                        ac = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                        ly = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    }
                    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, st, ac, ly,
                               VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                }
            }
        } else
            VK_OK(r);
    }
    if (game) {
        /* the time from the last present, for the stats line: frames slower than 50 ms (a hitch at any pace a game
           runs at) and the slowest, since the last line */
        static u64 last_present_ns, slow_frames, slowest_ns;
        [[maybe_unused]] static u64 was_drawn; /* the draws plot's base; only the profiler reads it */
        /* the counters as of the last present: a slow frame says what it did, which names what a hitch waited for */
        static u64 was[12];
        u64 now_ns = plat_ns(), is[11];
        vk_pipeline_stats(&is[0], &is[1], &is[2], &is[3], &is[4], &is[5]);
        vk_tex_stats(&is[6], &is[7]);
        is[8] = vk_rt_made();
        is[9] = g_vk_finish_ns / 1000000;
        if (last_present_ns) {
            u64 d = now_ns - last_present_ns;
            SNX_PLOT("frame ms", (double)d / 1e6);
#if defined(SNX_TRACY)
            gpu_memory_plots();
            /* what the CPU has put in the queues and the GPU hasn't got through: the CPU running ahead of the GPU
               (a submit backlog), and the compute batches waiting to land */
            {
                u64 sub, done, csub, cdone;
                vk_submit_progress(&sub, &done);
                SNX_PLOT("submits pending", (double)(sub - done));
                vk_compute_progress(&csub, &cdone);
                SNX_PLOT("compute pending", (double)(csub - cdone));
                SNX_PLOT("dispatches", (double)(g_vk_dispatches - was[10]));
                was[10] = g_vk_dispatches;
                SNX_PLOT("compute cpu reads", (double)(g_vk_cs_cpu_reads - was[11]));
                was[11] = g_vk_cs_cpu_reads;
            }
#endif
            SNX_PLOT("draws", (double)(g_vk_drawn - was_drawn));
            SNX_PLOT("shaders", (double)(is[4] - was[4]));
            SNX_PLOT("pipelines", (double)(is[0] - was[0]));
            SNX_PLOT("pipeline cache hits", (double)(is[1] - was[1]));
            SNX_PLOT("pipeline make ms", (double)(is[2] - was[2]));
            SNX_PLOT("pipeline wait ms", (double)(is[3] - was[3]));
            SNX_PLOT("texture uploads", is[7] - was[7]);
            was_drawn = g_vk_drawn;
            if (d > 50000000) {
                slow_frames++;
                LOG_LIMIT(200,
                          "vulkan: a %llu ms frame: %llu shaders translated (%llu ms), %llu pipelines made (%llu ms waited "
                          "for), %llu render targets made, %llu texture uploads (%llu KB), %llu ms in finishes\n",
                          (unsigned long long)(d / 1000000), (unsigned long long)(is[4] - was[4]),
                          (unsigned long long)(is[5] - was[5]), (unsigned long long)(is[0] - was[0]),
                          (unsigned long long)(is[3] - was[3]), (unsigned long long)(is[8] - was[8]),
                          (unsigned long long)(is[6] - was[6]), (unsigned long long)((is[7] - was[7]) >> 10),
                          (unsigned long long)(is[9] - was[9]));
            }
            if (d > slowest_ns) slowest_ns = d;
        }
        memcpy(was, is, sizeof was);
        last_present_ns = now_ns;
        g_vk_present++;
        if (blend_shown) stats_generated(); /* before the frame, so the half second's rates count it */
        stats_frame(now_ns);
        if (g_vk_present == 1 || g_vk_present % 600 == 0) guest_stats_log(now_ns, g_vk_present);
        vk_mem_present();
        if (g_last) vk_surf_touch(g_last); /* what screenshots read stays */
        vk_tex_frame();
        vk_buf_frame();
        vk_mem_trim(0, 0);
        if (g_vk_present % 600 == 0) {
            char cs_why[256];
            u64 cs_skipped = vk_cs_skipped(cs_why, sizeof cs_why);
            LOG("vulkan: %llu draws, %llu skipped; %llu dispatches in %llu batches (%llu ms waiting), %llu skipped%s; "
                "%llu finishes (%llu ms); %llu reads past queued releases (%llu samples counts written on the GPU); "
                "%llu descriptor sets written, %llu reused; "
                "%llu draws kept the last one's shaders and pipeline (%llu checked), %llu its targets\n",
                (unsigned long long)g_vk_drawn, (unsigned long long)g_vk_skipped, (unsigned long long)g_vk_dispatches,
                (unsigned long long)g_vk_cs_batches,
                (unsigned long long)(__atomic_load_n(&g_vk_cs_wait_ns, __ATOMIC_RELAXED) / 1000000),
                (unsigned long long)cs_skipped, cs_why, (unsigned long long)g_vk_finishes,
                (unsigned long long)(g_vk_finish_ns / 1000000),
                (unsigned long long)__atomic_load_n(&g_vk_release_forwards, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&g_vk_counts_on_gpu, __ATOMIC_RELAXED),
                (unsigned long long)g_vk_dset_new, (unsigned long long)g_vk_dset_reused,
                (unsigned long long)g_vk_draw_kept, (unsigned long long)g_vk_draw_checked,
                (unsigned long long)g_vk_targets_kept);
            if (g_interp_pairs || g_interp_plain)
                LOG("vulkan: %llu game frames shown as a blend and a frame, %llu as one image (the display held "
                    "every image)\n",
                    (unsigned long long)g_interp_pairs, (unsigned long long)g_interp_plain);
            {
                u64 pipes, hits, make_ms, pipe_ms, shaders, shader_ms, uploads, upload_bytes;
                vk_pipeline_stats(&pipes, &hits, &make_ms, &pipe_ms, &shaders, &shader_ms);
                vk_tex_stats(&uploads, &upload_bytes);
                LOG("vulkan: %llu shaders translated (%llu ms), %llu pipelines made (%llu from the cache, %llu ms "
                    "making, %llu ms waited for), %llu render targets made, %llu texture uploads (%llu MB); %llu "
                    "frames over 50 ms in the last 600, the slowest %llu ms\n",
                    (unsigned long long)shaders, (unsigned long long)shader_ms, (unsigned long long)pipes,
                    (unsigned long long)hits, (unsigned long long)make_ms, (unsigned long long)pipe_ms,
                    (unsigned long long)vk_rt_made(), (unsigned long long)uploads,
                    (unsigned long long)(upload_bytes >> 20), (unsigned long long)slow_frames,
                    (unsigned long long)(slowest_ns / 1000000));
                slow_frames = slowest_ns = 0;
                u64 bc7_levels, bc7_bytes, bc7_rgba8;
                vk_astc_stats(&bc7_levels, &bc7_bytes, &bc7_rgba8);
                if (bc7_levels)
                    LOG("vulkan: %llu ASTC texture levels uploaded as BC7, %llu MB where RGBA8 takes %llu MB\n",
                        (unsigned long long)bc7_levels, (unsigned long long)(bc7_bytes >> 20),
                        (unsigned long long)(bc7_rgba8 >> 20));
                u64 early_made, early_hits, early_left;
                vk_pipeline_prewarm_stats(&early_made, &early_hits, &early_left);
                if (early_made || early_left)
                    LOG("vulkan: %llu pipelines of earlier runs made in the background (%llu from the cache), %llu "
                        "still to make\n",
                        (unsigned long long)early_made, (unsigned long long)early_hits,
                        (unsigned long long)early_left);
            }
            {
                /* each memory heap's use and the driver's budget for it (VK_EXT_memory_budget), and our own memory in
                   it (VMA's blocks): past the budget the driver moves memory, and the heap the CPU maps (the BAR window)
                   is the one to watch. NVIDIA counts the whole system's use of the window as ours, and budgets what
                   is left of it (cmd.c bar_room) */
                VmaBudget b[VK_MAX_MEMORY_HEAPS];
                const VkPhysicalDeviceMemoryProperties* mp;
                char line[256];
                size_t at = 0;
                vmaGetHeapBudgets(g_vk.vma, b);
                vmaGetMemoryProperties(g_vk.vma, &mp);
                for (u32 q = 0; q < mp->memoryHeapCount && at < sizeof line; q++)
                    at += (size_t)snprintf(line + at, sizeof line - at, "%s heap %u %llu of %llu MB (%llu ours)",
                                           q ? "," : "", q, (unsigned long long)(b[q].usage >> 20),
                                           (unsigned long long)(b[q].budget >> 20),
                                           (unsigned long long)(b[q].statistics.blockBytes >> 20));
                LOG("vulkan: memory used and budgeted:%s\n", line);
            }
            {
                /* what the caches hold, which vk_mem_trim keeps within the budget */
                u32 nrt, ntex, nbuf, parked, none;
                u64 brt, btex, bbuf, usage, budget, trimmed;
                vk_surf_stats(VK_LRU_RT, &nrt, &brt, &parked);
                vk_surf_stats(VK_LRU_TEX, &ntex, &btex, &none);
                vk_buf_stats(&nbuf, &bbuf);
                vk_mem_use(&usage, &budget);
                trimmed = vk_mem_trimmed();
                LOG("vulkan: caches: %u render targets (%llu MB, %u parked), %u textures (%llu MB), %u buffers (%llu "
                    "MB); %llu of %llu MB in use as they count it, %llu MB freed for the budget so far\n",
                    nrt, (unsigned long long)(brt >> 20), parked, ntex, (unsigned long long)(btex >> 20), nbuf,
                    (unsigned long long)(bbuf >> 20), (unsigned long long)(usage >> 20),
                    (unsigned long long)(budget >> 20), (unsigned long long)(trimmed >> 20));
            }
            LOG("vulkan: %llu draws sampled a target they render to in place (%llu barriers in passes, %llu passes "
                "begun again), %llu through a copy\n",
                (unsigned long long)g_vk_fb_draws, (unsigned long long)g_vk_fb_barriers,
                (unsigned long long)g_vk_fb_restarts, (unsigned long long)g_vk_rt_copies);
            LOG("vulkan: %llu rendering passes, %llu clear commands, %llu attachments cleared and %llu left undefined "
                "by their load op\n",
                (unsigned long long)g_vk_passes, (unsigned long long)g_vk_clear_cmds,
                (unsigned long long)g_vk_clear_loads, (unsigned long long)g_vk_dontcare_loads);
            vk_pass_ends_log();
            vk_descriptors_stats();
            /* what the GPU engines keep on the GPU (docs/GPU_OWNERSHIP.md), and what the CPU had to wait for */
            LOG("vulkan: GPU-owned guest memory landed for the CPU %llu times, render targets written back for an "
                "engine %llu times\n",
                (unsigned long long)own_landings(), (unsigned long long)g_vk_rt_readbacks);
        }
    }
    /* Offscreen frames still submit and advance the frame ring. g_last remains available for readback. */
    if (signal2)
        vk_submit_present2(wait, VK_PIPELINE_STAGE_2_CLEAR_BIT, acq2, signal, signal2);
    else
        vk_submit(wait, VK_PIPELINE_STAGE_2_CLEAR_BIT, signal);
    if (game) vk_renderdoc_present(g_vk_present);
    vk_frame_next(); /* every show used this frame's slot and acquire semaphore, the menu's own too */
    if (signal) {
        /* after the frame's submission, whose semaphore it waits on, without waiting for its recording */
        /* display.c watches for it before it is made, which says so (vk_display_made) */
        u64 id = vk_display_id(), target = game && id ? vk_display_target(period_ns) : 0;
        vk_display_presented(g_vk.swapchain, id, game, released_ns, shown_syncpt);
        VkResult r;
        if (signal2) {
            /* the blend at the frame's refresh, the frame itself at the one after: FIFO shows them in this order */
            u64 refresh = backend_display_refresh_ns();
            r = vk_rec_present(g_vk.swapchain, idx2, signal2, id, target);
            VkResult r2 = vk_rec_present(g_vk.swapchain, idx, signal, 0, target && refresh ? target + refresh : 0);
            if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) r = r2;
        } else
            r = vk_rec_present(g_vk.swapchain, idx, signal, id, target);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !g_vk.sc_turned))
            recreate = 1;
        else if (r == VK_ERROR_SURFACE_LOST_KHR)
            g_lost = 1;
        else
            VK_OK(r);
    } else /* not presented: displayed never, once the presents before it are done with */
        vk_display_presented(VK_NULL_HANDLE, 0, game, released_ns, shown_syncpt);
    if (recreate && !g_lost && !g_vk.sc_lost) { /* not when this frame's create found the surface lost */
        vk_finish();
        vk_swapchain_create(g_sc_mode);
    }
    if (g_vk.sc_lost) { /* a create found the surface lost: the next frame makes it anew */
        g_vk.sc_lost = 0;
        g_lost = 1;
    }
}

/* the game queued a buffer for display: show it, letterboxed to the window. A frame that can't be shown (minimized,
   no image to draw to) is still submitted, with the per-present bookkeeping, so frames don't pile up */
static void vkr_present_buffer(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                               u32 transform, u64 released_ns, u64 period_ns, u32 shown_syncpt) {
    SNX_ZONE(z_present, "present");
    vk_lock();
    vk_buf_writable_abort(); /* a draw abandoned during setup has no shader writes to land */
    vk_render_end();
    Surf* src = NULL;
    if (w && h && w <= g_vk.props.limits.maxImageDimension2D && h <= g_vk.props.limits.maxImageDimension2D) {
        src = vk_rt_for_cpu(cpu_addr, w, h);
        if (!src || src->f.is_int) src = cpu_frame(cpu_addr, w, h, pitch, layout, bhl, fmt);
    }
    /* the part of the buffer shown: vi's crop, or all of it */
    u32 x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (crop[2] > crop[0] && crop[3] > crop[1] && crop[0] >= 0 && crop[1] >= 0) {
        x0 = (u32)crop[0] < w ? (u32)crop[0] : 0;
        y0 = (u32)crop[1] < h ? (u32)crop[1] : 0;
        x1 = (u32)crop[2] < w ? (u32)crop[2] : w;
        y1 = (u32)crop[3] < h ? (u32)crop[3] : h;
    }
    if (src) { /* the render target found for the buffer can be smaller than it: a crop outside it shows all of it */
        if (x1 > src->w) x1 = src->w;
        if (y1 > src->h) y1 = src->h;
        if (x1 <= x0 || y1 <= y0) {
            x0 = y0 = 0;
            x1 = w < src->w ? w : src->w;
            y1 = h < src->h ? h : src->h;
        }
    }
    if (src) { /* in its pixels: targets are scaled */
        g_last = src;
        g_last_transform = transform;
        g_last_x = (u32)((u64)src->sw * x0 / src->w);
        g_last_y = (u32)((u64)src->sh * y0 / src->h);
        g_last_w = (u32)((u64)src->sw * x1 / src->w) - g_last_x;
        g_last_h = (u32)((u64)src->sh * y1 / src->h) - g_last_y;
    }
    show(src, x1 - x0, y1 - y0, transform, 1, released_ns, period_ns, shown_syncpt);
    __atomic_store_n(&g_shown_ns, plat_ns(), __ATOMIC_RELAXED);
    vk_unlock();
}

/* The last frame again, under the controls and the menu, while the game shows none: a game loading, stuck, or drawing
   nothing still has them. Not before a quarter second without a frame (a slow game's own come between), at 30 a second
   at most, and only when the GPU thread holds no Vulkan work: then the next call tries again */
static void vkr_refresh(void) {
    static u64 last;
    u64 now = plat_ns();
    if (!backend_window_shown() || now - __atomic_load_n(&g_shown_ns, __ATOMIC_RELAXED) < 250000000ull ||
        now - last < 33000000ull)
        return;
    if (!vk_trylock()) return;
    last = now;
    vk_render_end();
    show(g_last, g_last_w, g_last_h, g_last_transform, 0, 0, 0, 0);
    vk_unlock();
}

/* the upscaler_key hotkey: the live cycle through the filters */
static const char* vkr_cycle_upscale(void) { return snx_upscale_label(vk_upscale_cycle()); }

static const char* vkr_cycle_aa(void) {
    static int aa = -1;
    if (aa < 0) aa = snx_fx_aa_parse(g_settings.anti_aliasing);
    aa = (aa + 1) % SNX_FX_AA_COUNT;
    snprintf(g_settings.anti_aliasing, sizeof g_settings.anti_aliasing, "%s", snx_fx_aa_key(aa));
    if (g_present_fx) snx_fx_set_aa(g_present_fx, aa);
    return snx_fx_aa_label(aa);
}

/* the hdr_key hotkey: HDR off and on. The window's image follows at the next present (show's want_mode) */
static const char* vkr_toggle_hdr(void) {
    static char text[192], note[96];
    int mode = vk_hdr_toggle();
    int display = vk_hdr_display_kind();
    vk_hdr_describe(display, text, sizeof text);
    if (mode == SNX_HDR_ON) {
        if (!vk_hdr_available())
            snprintf(note, sizeof note, " (this device cannot run the passes)");
        else if (display == SNX_HDR_DISPLAY_NONE)
            snprintf(note, sizeof note, " (this display takes no HDR image: tone mapped)");
        else
            note[0] = 0;
    } else
        note[0] = 0;
    if (note[0]) {
        size_t at = strlen(text);
        snprintf(text + at, sizeof text - at, "%s", note);
    }
    return text;
}

/* a part of a surface (x, y, w x h), at dw x dh, scaled on the GPU, and read back RGBA8: the pixels, good until the
   next readback, or NULL. A frame that holds HDR data goes through the HDR passes into the SDR range: a screenshot is
   the tone mapped picture, not the bytes above white cut off */
static const u8* surf_read(Surf* s, u32 x, u32 y, u32 w, u32 h, u32 transform, u32 dw, u32 dh) {
    /* blit to an RGBA8 image, whatever the target's format, then read that back. sRGB to sRGB, so the bytes stay
       encoded as the display shows them */
    int tonemap = vk_hdr_available() && vk_hdr_frame_hdr(s->f.vk);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = tonemap ? VK_FORMAT_R8G8B8A8_UNORM
                                    : is_srgb(s->f.vk) ? VK_FORMAT_R8G8B8A8_SRGB
                                                       : VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {dw, dh, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     (tonemap ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage tmp;
    VmaAllocation tmem;
    VkImageView tmp_view = VK_NULL_HANDLE;
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &tmp, &tmem, NULL))) return NULL;
    vk_mem_track(tmem);
    VkBuffer buf;
    u8* p = vk_readback_buffer((u64)dw * dh * 4, &buf);
    if (!p) {
        vk_mem_untrack(tmem);
        vmaDestroyImage(g_vk.vma, tmp, tmem);
        return NULL;
    }
    vk_render_end(); /* the blit, the copy and their barriers can't be recorded inside a rendering pass */
    VkCommandBuffer cb = vk_cmd();
    if (tonemap) {
        VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = tmp,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = ii.format,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkComponentMapping identity = {0};
        VkImage comp;
        VkImageView comp_view, src_view;
        if (!VK_OK(vkCreateImageView(g_vk.dev, &vci, NULL, &tmp_view))) {
            vk_mem_untrack(tmem);
            vmaDestroyImage(g_vk.vma, tmp, tmem);
            return NULL;
        }
        src_view = vk_tex_view(s, VK_IMAGE_VIEW_TYPE_2D, 0, &identity, 0);
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        if (src_view && vk_hdr_compose(cb, src_view, vk_hdr_source_for(s->f.vk), s->sw, s->sh, x, y, w, h, transform,
                                       dw, dh, 0, 0, dw, dh, &comp, &comp_view))
            vk_hdr_encode(cb, comp, comp_view, dw, dh, tmp, tmp_view, ii.format, VK_HDR_ENC_SDR);
        vk_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    } else {
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        vk_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .srcOffsets = {{(i32)x, (i32)y, 0}, {(i32)(x + w), (i32)(y + h), 1}},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .dstOffsets = {{0, 0, 0}, {(i32)dw, (i32)dh, 1}}};
        flip_src(&blit, transform); /* screenshots show the window's transform, offscreen and headless presents too */
        rec_CmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &blit, dw == w && dh == h ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        vk_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {dw, dh, 1}};
    rec_CmdCopyImageToBuffer(cb, tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    vk_finish();
    vk_readback_ready();
    if (tmp_view) vkDestroyImageView(g_vk.dev, tmp_view, NULL);
    vk_mem_untrack(tmem);
    vmaDestroyImage(g_vk.vma, tmp, tmem);
    return p;
}
/* a part of a surface, blitted to RGBA8 and read back: its hash, and a .ppm at path if given */
static u64 surf_save(Surf* s, u32 x, u32 y, u32 w, u32 h, u32 transform, const char* path) {
    u64 hash = 1469598103934665603ull;
    const u8* px = surf_read(s, x, y, w, h, transform, w, h);
    if (!px) return hash;
    /* the pixels as the device wrote them, little-endian words: the hash is the same on every host */
    for (u64 i = 0; i < (u64)w * h; i++) hash = snx_fnv_step(hash, snx_ld_le32(px + i * 4));
    if (path) {
        char fn[1100];
        snprintf(fn, sizeof fn, "%s.ppm", path);
        FILE* f = fopen(fn, "wb");
        int ok = f != NULL;
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (u64 i = 0; i < (u64)w * h && ok; i++) {
                u8 rgb[3] = {px[i * 4], px[i * 4 + 1], px[i * 4 + 2]};
                ok = fwrite(rgb, 1, 3, f) == 3;
            }
            ok = fclose(f) == 0 && ok;
        }
        if (!ok) WARN_LIMIT(1, "vulkan: the screenshot %s couldn't be written\n", fn);
    }
    return hash;
}
/* the last presented frame as PPM (path.ppm), and a hash of its pixels */
static u64 vkr_screenshot(const char* path) {
    vk_lock();
    u64 hash = g_last ? surf_save(g_last, g_last_x, g_last_y, g_last_w, g_last_h, g_last_transform, path)
                      : 1469598103934665603ull;
    vk_unlock();
    return hash;
}

/* the last presented frame scaled to w x h, RGBA8 as the display shows it, into rgba: 0, or -1 with none shown */
static int vkr_thumbnail(u8* rgba, u32 w, u32 h) {
    const u8* px = NULL;
    vk_lock();
    /* a quarter turn shows the frame on its side: the thumbnail is of the picture as the player sees it */
    if (g_last) px = surf_read(g_last, g_last_x, g_last_y, g_last_w, g_last_h, g_last_transform, w, h);
    if (px) memcpy(rgba, px, (size_t)w * h * 4);
    vk_unlock();
    return px ? 0 : -1;
}

/* the stall watchdog: the releases queued and how far the submissions they wait for have got */
static void vk_report_stall(void) {
    u64 n, gate, cgate, gsub, gdone, csub, cdone;
    vk_release_queued(&n, &gate, &cgate);
    vk_submit_progress(&gsub, &gdone);
    vk_compute_progress(&csub, &cdone);
    WARN("vulkan: %llu releases queued (the oldest waits for graphics submission %llu and compute batch %llu), "
         "graphics submissions done %llu of %llu, compute batches %llu of %llu, %llu bytes of commands not recorded\n",
         (unsigned long long)n, (unsigned long long)gate, (unsigned long long)cgate, (unsigned long long)gdone,
         (unsigned long long)gsub, (unsigned long long)cdone, (unsigned long long)csub,
         (unsigned long long)vk_rec_behind());
}
static void vkr_idle(void) {
    vk_lock();
    vk_buf_land_syncwait(); /* results recorded since the last release, which a finish alone leaves pending */
    vk_gfx_image_land();
    vk_compute_land();
    vk_finish();
    vk_release_drain();
    vk_unlock();
}
static void vkr_flush_targets(void) {
    u32 n;
    vk_lock();
    n = vk_rt_flush_all();
    vk_finish();
    vk_unlock();
    LOG("vulkan: %u render targets written back for a save state\n", n);
}
static int vkr_save_targets(struct StateIo* io) {
    int r;
    vk_lock();
    r = vk_zt_save(io);
    vk_unlock();
    return r;
}
static int vkr_load_targets(struct StateIo* io) {
    int r;
    vk_lock();
    r = vk_zt_load(io);
    vk_unlock();
    return r;
}
static void vkr_reset(void) {
    vk_lock();
    vk_render_end();
    vk_render_forget_state();
    vk_zt_forget();
    vk_surf_forget_all();
    vk_buf_forget_all();
    vk_query_forget_all();
    if (g_cpu_img) {
        vk_defer_image(g_cpu_img->img, g_cpu_img->mem, VK_NULL_HANDLE, g_cpu_img->att);
        free(g_cpu_img);
        g_cpu_img = NULL;
    }
    g_last = NULL;
    g_vk.shader_generation++; /* the shaders found at guest addresses: the code there is the state's */
    vk_unlock();
}
const Renderer vk_renderer = {
    .name = "Vulkan",
    .start = vkr_start,
    .start_failure = vk_start_failure,
    .mem_may_have_changed = vkr_mem_may_have_changed,
    .stream_run = vk_ww_stream,
    .clear = vkr_clear,
    .draw_texture = vkr_draw_texture,
    .discard = vkr_discard,
    .draw = vkr_draw,
    .method_barrier = vkr_method_barrier,
    .blit = vkr_blit,
    .vic_compose = vkr_vic_compose,
    .nvdec_write = vkr_nvdec_write,
    .nvdec_hw_device = vkr_nvdec_hw_device,
    .dma_copy = vkr_dma_copy,
    .snap_alloc = vk_snap_alloc,
    .snap_free = vk_snap_free,
    .blit_mem = vkr_blit_mem,
    .rt_flush = vkr_rt_flush,
    .rt_reload = vkr_rt_reload,
    .dispatch = vkr_dispatch,
    .cs_mem_need = vk_compute_mem_need,
    .mem_pending = vkr_mem_pending,
    .gfx_mem_need = vkr_gfx_mem_need,
    .before_syncwait = vkr_before_syncwait,
    .stream_incr = vkr_stream_incr,
    .release_write = vk_release_write,
    .release_need = vk_release_need,
    .release_read = vk_release_read,
    .release_value = vk_release_value,
    .render_predicate = vk_render_predicate,
    .render_predicate_drop = vk_render_predicate_drop,
    .release_retire = vk_release_retire,
    .report_stall = vk_report_stall,
    .idle = vkr_idle,
    .reset = vkr_reset,
    .flush_targets = vkr_flush_targets,
    .save_targets = vkr_save_targets,
    .load_targets = vkr_load_targets,
    .samples_reset = vkr_samples_reset,
    .samples_enable = vkr_samples_enable,
    .samples_report = vkr_samples_report,
    .stream_end = vk_release_submit,
    .present_buffer = vkr_present_buffer,
    .display_wait = vk_display_wait,
    .screenshot = vkr_screenshot,
    .thumbnail = vkr_thumbnail,
    .refresh = vkr_refresh,
    .save = vk_pipeline_save,
    .release_window = vk_window_release,
    .prepare = vk_pipeline_prepare,
    .cycle_upscale = vkr_cycle_upscale,
    .toggle_hdr = vkr_toggle_hdr,
    .cycle_aa = vkr_cycle_aa,
};
const Renderer* g_renderer = &vk_renderer;
