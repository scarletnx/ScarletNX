/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vulkan renderer: the renderer interface (vk_renderer, the vkr_ functions it points to) and presenting.

   Render targets, clears, draws (gpu/vk/render.c), compute (gpu/vk/compute.c), write-backs to and reloads from
   guest memory, and presenting the game's framebuffer from its render target (or from memory, converted on the CPU,
   when no render target holds it). Everything that records runs under vk_lock(), which is reentrant: submissions and
   presents on the GPU thread (gpu/gpu.c), the menu's refresh on the main thread (vkr_refresh), screenshots on the
   thread asking, and shader results landing on whichever thread reads their memory first. Acquiring a swapchain image
   never waits for the display: with every image still on it or queued for it, that frame isn't shown (counted). */
#include "common/memory/jemalloc.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/guest_stats.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/hdr_swapchain.h"
#include "nx/gpu/vk/resource_stats.h"
#include "nx/gpu/vk/tex_report.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/gpu/vk/present_quad.h"
#include "nx/gpu/renderer.h"

static VkSemaphore g_acquired[VK_FRAMES];
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
/* pending compute and graphics results, storage images and queued releases all count their pages in g_pend_any
   (pending_pages.h) */
static int vkr_mem_pending(u64 gva, u64 size) {
    int hit;
    /* the tables' lock, not vk_lock: the game's threads ask this of each read they make through the GPU's address
       space (gmem_read), and waited here for vk_lock while the GPU thread held it across its waits for the GPU */
    pend_lock();
    hit = pend_hit(&g_pend_any, gva, size);
    pend_unlock();
    return hit;
}
static void vkr_gfx_mem_need(u64 gva, u64 size) {
    vk_mirror_mem_need(gva, size); /* the GPU mirror's programs' writes there landed */
    vk_buf_mem_need(gva, size);
    vk_uma_mem_need(gva, size); /* the in-place writes there done */
    vk_gfx_image_mem_need(gva, size);
}
/* the GPU range a mapping change is changing, from its begin to its end (size 0 for a change of guest memory) */
static u64 g_mapping_gva, g_mapping_size;
/* whether the renderer holds anything over guest memory [a, a + size), with pend_lock held: pending results or
   GPU-owned memory on its host pages, which land there, or a GPU mapping of it, through which the renderer reads and
   writes it (gmmu_run, then the bytes, with vk_lock held). Storage images and render targets write back through their
   GPU mapping too */
static void alias_seen(void* ctx, u64 gva, u64 n) {
    (void)gva;
    (void)n;
    *(int*)ctx = 1;
}
static int guest_held(u64 a, u64 size) {
    u64 page = plat_vm_page(), lo = a & ~(page - 1), hi = (a + size + page - 1) & ~(page - 1);
    int mapped = 0;
    if (pend_hit_guest(&g_pend_any, a, size) || own_trapped(lo, hi - lo)) return 1;
    gmmu_aliases(a, size, alias_seen, &mapped);
    return mapped;
}
/* whether it may: unified memory reads guest memory in place, and its imports aren't looked at here */
static int guest_covered(u64 a, u64 size) {
    if (vk_uma() || size == ~0ull || a + size < a) return 1;
    return guest_held(a, size);
}
/* whether a change that takes vk_lock waits for results of the GPU, in its begin or in mem.c's change, with pend_lock
   held. A change of guest pages alone (GPU_MAPPING_GUEST_ONLY): when the renderer holds something over them. Another
   change of guest memory lands what the GPU holds of other pages too (a view's source), so: when anything pends or is
   owned at all. A change of the GPU's address space has its callers off their cores already */
static int mapping_lands(u64 gva, u64 size, int kind) {
    if (kind == GPU_MAPPING_GMMU) return 0;
    if (kind == GPU_MAPPING_GUEST || size == ~0ull || gva + size < gva)
        return g_pend_any.total || own_any() || g_gfx_img_any;
    return guest_held(gva, size);
}
/* The brackets of this thread that skipped vk_lock, a bit each by depth: mem.c's brackets don't nest, the GMMU's
   may run inside one of the renderer's own (vk_lock held there already) */
static _Thread_local u32 t_mapping_depth, t_mapping_quick;
/* Locks, in order: vk_lock, then pend_lock (pending_pages.h), then mem.c's (memory/own.c has the rest). Every change
   holds pend_lock while it changes memory, from the end of its begin to its end, so nothing the renderer holds over
   guest memory comes meanwhile: pending results (pend_add), GPU-owned memory (owned.c) and GPU mappings (this bracket,
   for a GMMU change). A change of guest memory alone (GPU_MAPPING_GUEST_ONLY) that nothing covers takes no more. In one
   game's menus its memory thread's MapPhysicalMemory and UnmapPhysicalMemory waited 59 s of 130 for vk_lock, up to 7.9
   s at a time, while a landing held it waiting for a pipeline */
static void vkr_mapping_begin(u64 gva, u64 size, int kind) {
    u32 bit = 1u << (t_mapping_depth++ & 31);
    pend_lock();
    if (kind == GPU_MAPPING_GUEST_ONLY && !guest_covered(gva, size)) {
        t_mapping_quick |= bit;
        return;
    }
    t_mapping_quick &= ~bit;
    int full = kind != GPU_MAPPING_GMMU;
    int lands = mapping_lands(gva, size, kind);
    /* taken again after vk_lock, in order, and once what lands here has landed: a landing may wait for the GPU, and
       held pend_lock meanwhile, which the quick changes of other guest memory and the game's reads wait for. Each
       change of the tables takes it on its own until then */
    pend_unlock();
    /* the kernel hears of a wait before it starts (g_gpu_mapping_waits), and of none where the lock is free and
       nothing lands: a thread in a memory SVC then keeps its core to the SVC's end, as on the console */
    if (lands || !vk_trylock()) {
        if (g_gpu_mapping_waits) g_gpu_mapping_waits();
        vk_lock();
    }
    g_mapping_gva = full ? 0 : gva;
    g_mapping_size = full ? 0 : size;
    if (full) {
        /* A change of guest memory changes only its own guest pages, and a result pending elsewhere lands there as
           before. So the queued releases (their payloads count in g_pend_any), the buffer and compute results land,
           all of them as before, only when one covers a page of the change, and storage images when their bytes are
           there. svcMapPhysicalMemory maps fresh memory, where none is pending: landing every result made each map
           wait for all of the GPU's work, and one game's memory thread waited so with its core held */
        int hit = pend_hit_guest(&g_pend_any, gva, size);
        if (hit) vk_release_drain();
        if (hit) vk_buf_land_map();
        if (hit) vk_mirror_mem_need(0, ~0ull); /* the GPU mirror's programs' writes, all of them */
        vk_gfx_image_guest_need(gva, size);
        if (hit) vk_compute_land();
        /* last, as the landings may submit: mem.c's change of guest memory [gva, gva + size), whose in-place readers
           run and reach the driver first */
        vk_uma_mapping_begin(gva, size);
    } else {
        vk_release_drain(); /* before any translation changes: a payload loses the pages it gained */
        vk_buf_mem_need_map(gva, size);
        vk_mirror_mem_need(gva, size);
        vk_gfx_image_mem_need(gva, size);
        vk_compute_mem_need_map(gva, size);
        /* Images still describe the old backing until the page table changes. */
        vk_rt_before_write(gva, size);
    }
    pend_lock();
}
/* the pages changed count what is pending at the guest pages they show now. A change of guest memory (full) leaves
   the GMMU as it was: pending pages are compared by address, and freed memory is landed first */
static void vkr_mapping_end(void) {
    u32 bit = 1u << (--t_mapping_depth & 31);
    /* no GPU mapping shows the pages: no pending page count changes, and nothing the buffers keep describes them
       (vk_buf_mapping_changed) */
    if (t_mapping_quick & bit) {
        pend_unlock();
        return;
    }
    vk_uma_mapping_end(); /* first: what follows may submit */
    vk_buf_mapping_changed();
    if (g_mapping_size) vk_surf_mapping_changed(g_mapping_gva, g_mapping_size);
    pend_remap(g_mapping_gva, g_mapping_size);
    g_mapping_size = 0;
    pend_unlock();
    vk_unlock();
}

/* 0 when Vulkan can't run here */
static int vkr_start(void) {
    if (!vk_init()) return 0;
    g_vk_scale = g_settings.resolution_scale;       /* the .ini keeps resolution within 0.25 to 16 (options.c) */
    g_vk_anisotropy = g_settings.anisotropic_filtering; /* the sampler table keeps what it makes: anisotropy applies at start */
    if (g_vk_anisotropy > 1 && !g_vk.feat.samplerAnisotropy)
        LOG("vulkan: this GPU has no anisotropic filtering, so anisotropy=%d does nothing\n", g_vk_anisotropy);
    if (g_vk_scale != 1.0f) LOG("vulkan: render targets at %.2fx\n", g_vk_scale);
    vk_cmd_init();
    vk_render_upload_cache_reset();
    vk_pipeline_init();
    vk_own_init(); /* GPU engines' results stay on the GPU where guest memory can be trapped (memory/own.c) */
    g_gpu_before_write = vkr_before_write;
    g_gpu_mapping_begin = vkr_mapping_begin;
    g_gpu_mapping_end = vkr_mapping_end;
    g_gpu_mem_changed = vk_mem_changed;
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!g_opt_headless)
        for (int i = 0; i < VK_FRAMES; i++)
            if (!VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_acquired[i]))) return 0;
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
static int vkr_xfb_control(u32 owner_id, u64 control, u32 what, u32 stride, DrawOwner* owner) {
    vk_lock();
    int result = vk_render_xfb_control(owner_id, control, what, stride, owner);
    vk_unlock();
    return result;
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
	/* a fragment barrier, and the 3D engine's texture data cache invalidation (kind 2), make earlier draws' writes
	 * visible to texture reads (vk_rt_snapshot) */
	if (kind == 0 || kind == 2) __atomic_add_fetch(&g_vk_texture_epoch, 1, __ATOMIC_RELAXED);
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
	 * sampled while it is drawn gets one in the pass after this barrier
	 * (vk_render_fragment_barrier, feedback_order), and
	 * draws that read results of earlier ones get one there (buffer.c
	 * gfx_gpu_src). Ended here, the pass ended about 120 times a frame in one
	 * game's open world. Writes that none of those track (stores into guest
	 * memory read in place) are not covered yet, so it is skipped only where
	 * the device runs graphics descriptor buffers or heaps, which the
	 * vk_descriptors_mode setting asks for: a device that has neither draws with
	 * descriptor sets and keeps every fragment barrier.
	 */
	if (kind == 0 && (g_vk.has_descriptor_buffers || g_vk.has_descriptor_heap_support)) {
		vk_lock();
		vk_render_fragment_barrier();
		vk_unlock();
		return;
	}
	if (kind == 1 || kind == 3)
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
static int vkr_upscale(const GpuUpscale* u) {
    vk_lock();
    int result = vk_upscale(u);
    vk_unlock();
    return result;
}
static int vkr_blit_filter(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy,
                           double sdx, double sdy, int linear) {
    vk_lock();
    int result = vk_blit2d_filter(s, d, dx, dy, dw, dh, sx, sy, sdx, sdy, linear);
    vk_unlock();
    return result;
}
static int vkr_vic_compose(const VicJob* j) { return vk_vic_compose(j); } /* it takes vk_lock */
static int vkr_nvdec_write(const NvdecPic* p) { return vk_nvdec_write(p); }   /* it takes vk_lock */
static void* vkr_nvdec_hw_device(u32 codec) { return vk_nvdec_hw_device(codec); }
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
        DevMemWant aci = {.at = DM_DEVICE};
        if (!VK_OK(vk_devmem_image(&ii, &aci, &g_cpu_img->img, &g_cpu_img->mem))) {
            free(g_cpu_img);
            g_cpu_img = NULL;
            return NULL;
        }
        vk_mem_track(g_cpu_img->mem, VK_OWN_UI);
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

static int g_sc_kind; /* the VK_SC_ kind the swapchain was asked for (the surface may not have it) */
/* what the next present's frame holds (vkr_present_hdr), and what the last frame shown does, for its refreshes */
static SnxHdrFrame g_next_hdr, g_last_hdr;
/* the swapchain for a frame: an HDR frame's own kind, else the 8-bit one every other frame's bytes go to as they are */
static int frame_kind(const SnxHdrFrame* hdr) {
    return hdr->space == SNX_HDR_SPACE_PQ ? VK_SC_PQ : hdr->space == SNX_HDR_SPACE_SCRGB ? VK_SC_SCRGB : VK_SC_UNORM;
}
/* the window's surface: lost while the app is in the background (Android), made anew when it comes back */
static u32 g_generation;
static int g_lost;
static u64 g_shown_ns; /* when the game last gave the window a frame (atomic: vkr_refresh reads it unlocked) */
/* The HDR metadata of the game's window, given to the swapchain made for its frames: once the swapchain is made,
   and again whenever the game's window names other values */
static void hdr_metadata(const SnxHdrFrame* hdr) {
    static u32 told; /* the swapchain (g_vk.sc_made) that has sent */
    static f32 sent[12];
    const f32* m = hdr->metadata;
    int given;
    if (g_vk.sc_space == SNX_HDR_SPACE_SRGB || hdr->space != g_vk.sc_space) return;
    if (told == g_vk.sc_made && !memcmp(sent, m, sizeof sent)) return;
    told = g_vk.sc_made;
    memcpy(sent, m, sizeof sent);
    pm_lock(&g_vk.sc_lock);
    given = vk_hdr_metadata_set(g_vk.dev, g_vk.swapchain, hdr);
    pm_unlock(&g_vk.sc_lock);
    LOG_LIMIT(16,
              "vulkan: the window's HDR metadata %s: red %g,%g green %g,%g blue %g,%g white %g,%g, %g to %g nits, "
              "content up to %g nits and %g a frame on average\n",
              given ? "given to the swapchain" : "names no display, and the swapchain is given none", m[0], m[1], m[2],
              m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
}
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

/* a frame image's size: small enough that a trace holds a frame's pictures, large enough to read a scene */
#define FRAME_IMAGE_W 320
#define FRAME_IMAGE_H 180
static void frame_image(void); /* after surf_read, whose readback it asks for */
#if defined(SNX_TRACY)
/* The heap the caches manage, as vk_mem_use reads it for their own trimming, in bytes (the plot is a Memory one). */
static void gpu_memory_plots(void) {
    u64 used, budget;
    vk_mem_use(&used, &budget);
    SNX_PLOT("gpu memory", used);
    SNX_PLOT("gpu memory budget", budget);
}
/* the host's heap, its footprint and the memory the system can still give: once a second, as the footprint reads a
   file on Linux and the available memory a whole /proc/meminfo. What a leak or a GC shows is here, not in the GPU's */
static void host_memory_plots(void) {
    static u64 last;
    u64 now = plat_ns(), free;
    if (last && now - last < 1000000000ull) return;
    last = now;
    SNX_PLOT("host heap", snx_heap_used());
    SNX_PLOT("host rss", snx_footprint());
    if (snx_memory_available(&free)) SNX_PLOT("host free", free);
}
#endif

/* The window's image: src's g_last_ rectangle (a frame of fw x fh, flipped by transform) letterboxed on black, or
   black alone, with the on-screen controls and the game's menu over it; then the frame's commands are submitted and
   the image presented. A game's frame (game, which the game released at released_ns, 0 unknown, at its pace of a
   frame every period_ns, 0 none) also moves the per-present bookkeeping, and without src shows nothing; its
   shown_syncpt (0 none) goes up once it is displayed, or known not to be (renderer.h). hdr says what src holds
   (g_last_hdr). Under vk_lock */
static void show(Surf* src, const SnxHdrFrame* hdr, u32 fw, u32 fh, u32 transform, int game, u64 released_ns,
                 u64 period_ns, u32 shown_syncpt) {
    SNX_ZONE(z_show, "vk show");
    if (transform & 4) { u32 w = fw; fw = fh; fh = w; }
    /* the size the swapchain was made for, not the extent the surface clamped it to: that can stay different */
    int ww, wh, kind = src ? frame_kind(hdr) : g_sc_kind, shown = backend_window_shown();
    backend_window_size(&ww, &wh);
    if (shown && (g_lost || backend_window_generation() != g_generation)) {
        vk_finish();
        g_lost = !vk_surface_recreate();
        g_generation = backend_window_generation();
    }
    shown = shown && !g_lost;
    int tried = 0; /* a create already ran this frame: one that failed waits for the next frame */
    /* a new vsync in the .ini takes a new swapchain, as a new size does, and a frame of another kind: a game's HDR
       frames after its SDR ones, or back */
    if (shown && ww > 0 && wh > 0 &&
        (ww != g_vk.sc_win_w || wh != g_vk.sc_win_h || kind != g_sc_kind ||
         __atomic_load_n(&g_settings.vsync, __ATOMIC_RELAXED) != g_vk.sc_vsync)) {
        vk_finish();
        g_sc_kind = kind;
        vk_swapchain_create(kind);
        tried = 1;
    }
    VkSemaphore wait = VK_NULL_HANDLE, signal = VK_NULL_HANDLE;
    u32 idx = 0;
    int recreate = 0;
    if (shown && (src || !game) && (g_vk.swapchain || (!tried && vk_swapchain_create(kind)))) {
        if (src) hdr_metadata(hdr);
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
            /* letterbox: the largest rectangle of the frame's aspect that fits the window's part clear of the system's
               bars, centered there; the clear part scales with the extent the surface may have clamped */
            u32 sw = g_vk.sc_extent.width, sh = g_vk.sc_extent.height, cx = 0, cy = 0, cw = sw, ch = sh, dw, dh;
            int sx, sy, sww, swh;
            VkRect2D crop = {0}, to = {0};
            Surf* framed = NULL; /* src, where it has a frame to draw */
            backend_window_safe(&sx, &sy, &sww, &swh);
            if (g_vk.sc_win_w > 0 && g_vk.sc_win_h > 0 && sww > 0 && swh > 0 && sx + sww <= g_vk.sc_win_w &&
                sy + swh <= g_vk.sc_win_h) {
                cx = (u32)((u64)sw * (u32)sx / (u32)g_vk.sc_win_w);
                cy = (u32)((u64)sh * (u32)sy / (u32)g_vk.sc_win_h);
                cw = (u32)((u64)sw * (u32)sww / (u32)g_vk.sc_win_w);
                ch = (u32)((u64)sh * (u32)swh / (u32)g_vk.sc_win_h);
            }
            if (src && fw && fh) {
                dw = cw;
                dh = (u32)((u64)cw * fh / fw);
                if (dh > ch) {
                    dh = ch;
                    dw = (u32)((u64)ch * fw / fh);
                }
                i32 dx = (i32)(cx + (cw - dw) / 2), dy = (i32)(cy + (ch - dh) / 2);
                crop = (VkRect2D){{(i32)g_last_x, (i32)g_last_y}, {g_last_w, g_last_h}};
                to = (VkRect2D){{dx, dy}, {dw, dh}};
                framed = src;
            }
            { /* one pass: black, the frame, the on-screen layer */
                int safe[4] = {(int)cx, (int)cy, (int)cw, (int)ch};
                vk_ui_present(cb, sc, framed, crop, to, transform, safe);
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
        static u64 was[15];
        static VkResourceStats resource_was;
        VkResourceStats resource_is = g_vk_resource_stats;
        u64 now_ns = plat_ns(), is[15] = {0};
        vk_pipeline_stats(&is[0], &is[1], &is[2], &is[3], &is[4], &is[5]);
        vk_tex_stats(&is[6], &is[7]);
        is[8] = vk_rt_made();
        is[9] = g_vk_finish_ns / 1000000;
        vk_texture_pipeline_stats(&is[10], &is[11]);
        for (u32 k = 0; k < VK_WAIT_N; k++) is[12 + k] = g_vk_wait_ns[k];
        if (last_present_ns) {
            u64 d = now_ns - last_present_ns;
            SNX_PLOT("frame ms", (double)d / 1e6);
#if defined(SNX_TRACY)
            gpu_memory_plots();
            host_memory_plots();
            frame_image();
            /* what the CPU has put in the queues and the GPU hasn't got through: the CPU running ahead of the GPU
               (a submit backlog), and the compute batches waiting to land */
            {
                u64 sub, done, csub, cdone;
                vk_submit_progress(&sub, &done);
                SNX_PLOT("submits pending", (double)(sub - done));
                vk_compute_progress(&csub, &cdone);
                SNX_PLOT("compute pending", (double)(csub - cdone));
                static u64 was_dispatches, was_reads;
                SNX_PLOT("dispatches", (double)(g_vk_dispatches - was_dispatches));
                was_dispatches = g_vk_dispatches;
                SNX_PLOT("compute cpu reads", (double)(g_vk_cs_cpu_reads - was_reads));
                was_reads = g_vk_cs_cpu_reads;
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
                          "for), %llu render targets made, %llu texture uploads (%llu KB), %llu ms in finishes; "
                          "%llu internal texture pipelines (%llu ms); GPU waits finish/recycle/submit %llu/%llu/%llu ms\n",
                          (unsigned long long)(d / 1000000), (unsigned long long)(is[4] - was[4]),
                          (unsigned long long)(is[5] - was[5]), (unsigned long long)(is[0] - was[0]),
                          (unsigned long long)(is[3] - was[3]), (unsigned long long)(is[8] - was[8]),
                          (unsigned long long)(is[6] - was[6]), (unsigned long long)((is[7] - was[7]) >> 10),
                          (unsigned long long)(is[9] - was[9]),
                          (unsigned long long)(is[10] - was[10]),
                          (unsigned long long)((is[11] - was[11]) / 1000000),
                          (unsigned long long)((is[12 + VK_WAIT_FINISH] - was[12 + VK_WAIT_FINISH]) / 1000000),
                          (unsigned long long)((is[12 + VK_WAIT_RECYCLE] - was[12 + VK_WAIT_RECYCLE]) / 1000000),
                          (unsigned long long)((is[12 + VK_WAIT_SUBMIT] - was[12 + VK_WAIT_SUBMIT]) / 1000000));
                /* These CPU categories exclude nested resource categories. Pipeline and wait counters above can
                   overlap them, so the two groups must not be added as independent parts of a frame. */
                for (u32 k = 0; k < VK_RESOURCE_N; k++) {
                    static const char* const names[VK_RESOURCE_N] = {
                        [VK_RESOURCE_IMAGE_QUERY] = "image requirements",
                        [VK_RESOURCE_IMAGE_CREATE] = "image allocation",
                        [VK_RESOURCE_IMAGE_DESTROY] = "image destruction",
                        [VK_RESOURCE_SURFACE_CREATE] = "target setup",
                        [VK_RESOURCE_RT_RELOAD] = "target reload",
                        [VK_RESOURCE_SURFACE_RECLAIM] = "surface reclaim",
                        [VK_RESOURCE_BUFFER_MAINTENANCE] = "buffer maintenance",
                        [VK_RESOURCE_BUFFER_CACHE_BUILD] = "buffer cache build"};
                    u64 ns = resource_is.ns[k] - resource_was.ns[k];
                    if (ns >= 1000000)
                        LOG_LIMIT(400, "vulkan: frame resource CPU: %s %llu ms (%llu calls)\n", names[k],
                                  (unsigned long long)(ns / 1000000),
                                  (unsigned long long)(resource_is.calls[k] - resource_was.calls[k]));
                }
            }
            if (d > slowest_ns) slowest_ns = d;
        }
        memcpy(was, is, sizeof was);
        resource_was = resource_is;
        last_present_ns = now_ns;
        g_vk_present++;
        __atomic_add_fetch(&g_vk_texture_epoch, 1, __ATOMIC_RELAXED); /* a frame's snapshots never outlive it */
        {
            SnxStatsGpu gpu = {0};
            if (__atomic_load_n(&g_settings.stats, __ATOMIC_RELAXED)) { /* read only while the box shows */
                gpu.busy_ns = vk_gpuprof_busy_ns();
                gpu.timed = vk_gpuprof_timed();
                vk_mem_driver_use(&gpu.vram, &gpu.vram_budget, &gpu.vram_ours);
            }
            stats_frame(now_ns, &gpu);
        }
        if (g_vk_present == 1 || g_vk_present % 600 == 0) {
            guest_stats_log(now_ns, g_vk_present);
            ipc_stats_log(now_ns);
        }
        vk_mem_present();
        if (g_last) vk_surf_touch(g_last); /* what screenshots read stays */
        vk_tex_frame();
        vk_buf_frame();
        vk_own_trim(); /* before the caches' trim: what it lands is room the trim then need not make */
        vk_mem_trim(0, 0);
        vk_astc_auto_present(); /* after the trim: Exact again for astc_mode's Auto where the room is back */
        if (g_vk_present % 600 == 0) {
            char cs_why[256];
            u64 cs_skipped = vk_cs_skipped(cs_why, sizeof cs_why);
            LOG("vulkan: %llu draws, %llu skipped (%llu mesh draws, %llu skipped); %llu dispatches in %llu batches "
                "(%llu ms waiting), %llu skipped%s; "
                "%llu finishes (%llu ms); %llu reads past queued releases (%llu samples counts written on the GPU); "
                "%llu descriptor sets written, %llu reused (%u descriptor pools, %llu sets with texture arrays taken "
                "from them); "
                "%llu draws kept the last one's shaders and pipeline (%llu checked), %llu its targets\n",
                (unsigned long long)g_vk_drawn, (unsigned long long)g_vk_skipped, (unsigned long long)g_vk_mesh_draws,
                (unsigned long long)(g_vk_mesh_draws - g_vk_mesh_drawn), (unsigned long long)g_vk_dispatches,
                (unsigned long long)g_vk_cs_batches,
                (unsigned long long)(__atomic_load_n(&g_vk_cs_wait_ns, __ATOMIC_RELAXED) / 1000000),
                (unsigned long long)cs_skipped, cs_why, (unsigned long long)g_vk_finishes,
                (unsigned long long)(g_vk_finish_ns / 1000000),
                (unsigned long long)__atomic_load_n(&g_vk_release_forwards, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&g_vk_counts_on_gpu, __ATOMIC_RELAXED),
                (unsigned long long)g_vk_dset_new, (unsigned long long)g_vk_dset_reused, vk_desc_pools(),
                (unsigned long long)vk_desc_array_sets(), (unsigned long long)g_vk_draw_kept,
                (unsigned long long)g_vk_draw_checked, (unsigned long long)g_vk_targets_kept);
            {
                static u64 synced_was;
                if (g_rec_barriers_synced != synced_was)
                    LOG("vulkan: %llu barriers of the last 600 frames waited for the recording thread to finish its "
                        "ring, for a structure chained to them\n",
                        (unsigned long long)(g_rec_barriers_synced - synced_was));
                synced_was = g_rec_barriers_synced;
            }
            {
                u64 pipes, hits, make_ms, pipe_ms, shaders, shader_ms, uploads, upload_bytes, partial, partial_bytes;
                vk_pipeline_stats(&pipes, &hits, &make_ms, &pipe_ms, &shaders, &shader_ms);
                vk_tex_stats(&uploads, &upload_bytes);
                vk_tex_partial_stats(&partial, &partial_bytes);
                LOG("vulkan: %llu shaders translated (%llu ms), %llu pipelines made (%llu from the cache, %llu ms "
                    "making, %llu ms waited for), %llu render targets made, %llu texture uploads (%llu MB), %llu of "
                    "them some blocks alone (%llu MB); %llu frames over 50 ms in the last 600, the slowest %llu ms\n",
                    (unsigned long long)shaders, (unsigned long long)shader_ms, (unsigned long long)pipes,
                    (unsigned long long)hits, (unsigned long long)make_ms, (unsigned long long)pipe_ms,
                    (unsigned long long)vk_rt_made(), (unsigned long long)uploads,
                    (unsigned long long)(upload_bytes >> 20), (unsigned long long)partial,
                    (unsigned long long)(partial_bytes >> 20), (unsigned long long)slow_frames,
                    (unsigned long long)(slowest_ns / 1000000));
                if (vk_shader_folds())
                    LOG("vulkan: %llu of those shaders translated again, to hold more of their draws' constant buffer "
                        "words as constants\n",
                        (unsigned long long)vk_shader_folds());
                slow_frames = slowest_ns = 0;
                if (g_vk.has_gpl) {
                    u64 made[4], hits[4], make_ms[4], links, link_ms, opts, opt_ms;
                    vk_pipeline_lib_stats(made, hits, make_ms, &links, &link_ms, &opts, &opt_ms);
                    LOG("vulkan: pipeline libraries: %llu pre-rasterization and %llu fragment shader parts made (%llu "
                        "and %llu from the cache, %llu and %llu ms), %llu vertex input and %llu fragment output parts "
                        "(%llu ms); %llu links (%llu ms); %llu optimized pipelines made in the background (%llu ms)\n",
                        (unsigned long long)made[1], (unsigned long long)made[2], (unsigned long long)hits[1],
                        (unsigned long long)hits[2], (unsigned long long)make_ms[1], (unsigned long long)make_ms[2],
                        (unsigned long long)made[0], (unsigned long long)made[3],
                        (unsigned long long)(make_ms[0] + make_ms[3]), (unsigned long long)links,
                        (unsigned long long)link_ms, (unsigned long long)opts, (unsigned long long)opt_ms);
                    u64 warm_made, warm_hit, warm_ms;
                    vk_pipeline_lib_warm_stats(&warm_made, &warm_hit, &warm_ms);
                    if (warm_made)
                        LOG("vulkan: %llu shipped programs' parts warmed ahead of their draws (%llu from the cache, "
                            "%llu ms)\n",
                            (unsigned long long)warm_made, (unsigned long long)warm_hit,
                            (unsigned long long)warm_ms);
                    u64 waits, wait_ms, behind_ms, max_ms;
                    int opt_workers;
                    vk_pipeline_lib_waits(&waits, &wait_ms, &behind_ms, &max_ms, &opt_workers);
                    LOG("vulkan: %llu new pipelines of draws waited %llu ms for a worker (%llu ms of it with every "
                        "worker busy and one or more on an optimized pipeline), the longest %llu ms; %d worker(s) at "
                        "most on optimized pipelines\n",
                        (unsigned long long)waits, (unsigned long long)wait_ms, (unsigned long long)behind_ms,
                        (unsigned long long)max_ms, opt_workers);
                }
                u64 bc7_levels, bc7_hits, bc7_bytes, bc7_rgba8, cache_reads, cache_stored, cache_restarts, cache_file;
                vk_astc_stats(&bc7_levels, &bc7_hits, &bc7_bytes, &bc7_rgba8);
                vk_bc7_cache_stats(&cache_reads, &cache_stored, &cache_restarts, &cache_file);
                if (bc7_levels)
                    LOG("vulkan: %llu ASTC texture levels or regions uploaded straight to BC7, %llu MB where RGBA8 "
                        "takes %llu MB; %llu encoder dispatches in all\n",
                        (unsigned long long)bc7_levels, (unsigned long long)(bc7_bytes >> 20),
                        (unsigned long long)(bc7_rgba8 >> 20), (unsigned long long)vk_astc_dispatches());
                if (cache_file)
                    LOG("vulkan: the BC7 cache gave %llu levels and took %llu new; its file holds %llu MB (started over "
                        "%llu times at %llu MB)\n",
                        (unsigned long long)bc7_hits, (unsigned long long)cache_stored,
                        (unsigned long long)(cache_file >> 20), (unsigned long long)cache_restarts,
                        (unsigned long long)(VK_BC7_CACHE_MAX >> 20));
                VkLaterStats later;
                vk_tex_later_stats(&later);
                if (later.made || later.waiting || later.restarted || later.went_hot)
                    LOG("vulkan: %llu ASTC textures made BC7 between frames (%llu MB of RGBA8 freed, %llu blocks "
                        "encoded, %llu encodes started over for a write); %u waiting, %llu MB as RGBA8; %u held "
                        "Exact for their writes (%llu went so, %llu made BC7 again)\n",
                        (unsigned long long)later.made, (unsigned long long)(later.freed >> 20),
                        (unsigned long long)later.blocks, (unsigned long long)later.restarted, later.waiting,
                        (unsigned long long)(later.waiting_bytes >> 20), later.hot, (unsigned long long)later.went_hot,
                        (unsigned long long)later.cooled);
                VkAstcBackground bg;
                vk_astc_background_stats(&bg);
                if (bg.quick || bg.hits)
                    LOG("vulkan: %llu very large ASTC levels encoded quickly, %llu given their fast build in the "
                        "background (%llu done, %llu going: %llu pieces stored, %llu pieces on the GPU, %llu loaded "
                        "from the cache), %llu whose fast build came from the cache\n",
                        (unsigned long long)bg.quick, (unsigned long long)bg.made, (unsigned long long)bg.finished,
                        (unsigned long long)bg.pending, (unsigned long long)bg.stored,
                        (unsigned long long)bg.gpu_pieces, (unsigned long long)bg.loaded, (unsigned long long)bg.hits);
                vk_astc_auto_say();
                vk_astc_kind_say();
                vk_astc_quick_say();
                u64 sparse_binds, sparse_ns;
                vk_sparse_stats(&sparse_binds, &sparse_ns);
                if (sparse_binds)
                    LOG("vulkan: %llu sparse binds of the textures mapped in part (%llu ms waited for)\n",
                        (unsigned long long)sparse_binds, (unsigned long long)(sparse_ns / 1000000));
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
                   it (devmem.c's blocks): past the budget the driver moves memory, and the heap the CPU maps (the BAR window)
                   is the one to watch. NVIDIA counts the whole system's use of the window as ours, and budgets what
                   is left of it (cmd.c bar_room) */
                DevMemBudget b[VK_MAX_MEMORY_HEAPS];
                const VkPhysicalDeviceMemoryProperties* mp;
                char line[256];
                size_t at = 0;
                vk_devmem_budgets(b);
                mp = vk_devmem_props();
                for (u32 q = 0; q < mp->memoryHeapCount && at < sizeof line; q++)
                    at += (size_t)snprintf(
                        line + at, sizeof line - at, "%s heap %u %llu of %llu MB (%llu ours, %llu of it allocated)",
                        q ? "," : "", q, (unsigned long long)(b[q].usage >> 20),
                        (unsigned long long)(b[q].budget >> 20), (unsigned long long)(b[q].block_bytes >> 20),
                        (unsigned long long)(b[q].allocated_bytes >> 20));
                LOG("vulkan: memory used and budgeted:%s\n", line);
            }
            vk_ledger_log();
            vk_own_log();
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
                    "MB); %llu of %llu MB in use as they count it, %llu MB freed for the budget so far; %llu MB of "
                    "textures freed so far because the game ended them\n",
                    nrt, (unsigned long long)(brt >> 20), parked, ntex, (unsigned long long)(btex >> 20), nbuf,
                    (unsigned long long)(bbuf >> 20), (unsigned long long)(usage >> 20),
                    (unsigned long long)(budget >> 20), (unsigned long long)(trimmed >> 20),
                    (unsigned long long)(vk_surf_ended_freed() >> 20));
            }
            vk_tex_report_log();
            LOG("vulkan: %llu draws sampled a target they render to in place (%llu barriers in passes, %llu passes "
                "begun again, %llu drawn two times, %llu feedback draws kept in place), %llu through a copy; %llu "
                "copies of targets written\n",
                (unsigned long long)g_vk_fb_draws, (unsigned long long)g_vk_fb_barriers,
                (unsigned long long)g_vk_fb_restarts, (unsigned long long)g_vk_predraws,
                (unsigned long long)g_vk_predraw_refused,
                (unsigned long long)g_vk_rt_copies, (unsigned long long)g_vk_rt_copy_writes);
            vk_predraw_report();
            if (g_vk_capture_draws || g_vk_capture_capped)
                LOG("vulkan: %llu draws captured a primitive at a time (%llu primitives), %llu skipped past %u "
                    "primitives\n",
                    (unsigned long long)g_vk_capture_draws, (unsigned long long)g_vk_capture_primitives,
                    (unsigned long long)g_vk_capture_capped, VK_CAPTURE_PRIMITIVES_MAX);
            LOG("vulkan: %llu rendering passes, %llu clear commands, %llu attachments cleared and %llu left undefined "
                "by their load op; %llu buffers filled ahead of the passes\n",
                (unsigned long long)g_vk_passes, (unsigned long long)g_vk_clear_cmds,
                (unsigned long long)g_vk_clear_loads, (unsigned long long)g_vk_dontcare_loads,
                (unsigned long long)vk_buf_ahead());
            vk_pass_ends_log();
            vk_finishes_log();
            vk_ww_stats(600);
            vk_descriptors_stats();
            /* what the GPU engines keep on the GPU (docs/layers/gpu/ownership.md), and what the CPU had to wait for */
            u64 by[OWN_BY_N];
            own_landings_by(by);
            LOG("vulkan: GPU-owned guest memory landed for the CPU %llu times (%llu for the runtime's own access, "
                "%llu for a read of the game's code, %llu for a write of it), render targets written back for an "
                "engine %llu times; block linear targets watched %llu times, published for a CPU access %llu times "
                "and at a release %llu times\n",
                (unsigned long long)own_landings(), (unsigned long long)by[OWN_BY_RUNTIME],
                (unsigned long long)by[OWN_BY_READ], (unsigned long long)by[OWN_BY_WRITE],
                (unsigned long long)g_vk_rt_readbacks,
                (unsigned long long)g_vk_rt_watch.arms, (unsigned long long)g_vk_rt_watch.hits,
                (unsigned long long)g_vk_rt_watch.publishes);
        }
    }
    /* Offscreen frames still submit and advance the frame ring. g_last remains available for readback. */
    vk_submit(wait, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, signal); /* the window's pass, vk_ui_present */
    if (game) vk_renderdoc_present(g_vk_present);
    /* after the frame's submission, so that the GPU's piece of a background encode never holds up the present */
    vk_astc_background();
    vk_frame_next(); /* every show used this frame's slot and acquire semaphore, the menu's own too */
    if (signal) {
        /* after the frame's submission, whose semaphore it waits on, without waiting for its recording */
        /* display.c watches for it before it is made, which says so (vk_display_made) */
        u64 id = vk_display_id(), target = game && id ? vk_display_target(period_ns) : 0;
        vk_display_presented(g_vk.swapchain, id, game, released_ns, shown_syncpt);
        VkResult r = vk_rec_present(g_vk.swapchain, idx, signal, id, target);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !g_vk.sc_turned))
            recreate = 1;
        else if (r == VK_ERROR_SURFACE_LOST_KHR)
            g_lost = 1;
        else
            VK_OK(r);
    } else /* not presented: displayed never, once the presents before it are done with */
        vk_display_presented(VK_NULL_HANDLE, 0, game, released_ns, shown_syncpt);
    /* the BC7 encoder's share of the frame after its present, which it never holds back */
    vk_tex_later_frame();
    if (recreate && !g_lost && !g_vk.sc_lost) { /* not when this frame's create found the surface lost */
        vk_finish();
        vk_swapchain_create(kind);
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
        if (src && !src->f.is_int) {
        } else if (fmt == RENDERER_FORMAT_RGBA_1010102 || fmt == RENDERER_FORMAT_RGBA_FP16) {
            /* cpu_frame converts vi's 8-bit and 16-bit formats to RGBA8, which holds neither of these */
            LOG_LIMIT(4, "vulkan: a %ux%u frame of format %#x with no render target over its buffer is shown black\n",
                      w, h, fmt);
            src = NULL;
        } else
            src = cpu_frame(cpu_addr, w, h, pitch, layout, bhl, fmt);
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
    SnxHdrFrame hdr = g_next_hdr; /* this present's alone: the next one's is an SDR frame's until it is said */
    g_next_hdr = (SnxHdrFrame){0};
    if (src) { /* in its pixels: targets are scaled */
        g_last = src;
        g_last_hdr = hdr;
        g_last_transform = transform;
        g_last_x = (u32)((u64)src->sw * x0 / src->w);
        g_last_y = (u32)((u64)src->sh * y0 / src->h);
        g_last_w = (u32)((u64)src->sw * x1 / src->w) - g_last_x;
        g_last_h = (u32)((u64)src->sh * y1 / src->h) - g_last_y;
    }
    if (prepare_screen_shown()) { /* a program switch's screen gives way before the game's frame takes its layer */
        ui_lock();
        prepare_screen_game_drew();
        ui_unlock();
    }
    show(src, &hdr, x1 - x0, y1 - y0, transform, 1, released_ns, period_ns, shown_syncpt);
    __atomic_store_n(&g_shown_ns, plat_ns(), __ATOMIC_RELAXED);
    vk_unlock();
}
/* the presenting thread's, before its present: nothing else reads g_next_hdr */
static void vkr_present_hdr(const SnxHdrFrame* frame) { g_next_hdr = *frame; }
/* what the display session tells a game (vi2's GetHdrStatus and GetHdrPaperWhite), any thread */
static int vkr_hdr_display(u32* peak, u32* paper_white) {
    if (!__atomic_load_n(&g_vk.hdr_display, __ATOMIC_RELAXED)) return 0;
    *peak = (u32)g_settings.hdr_peak;
    *paper_white = snx_hdr_paper_white(g_settings.hdr_paper_white);
    return 1;
}

/* whether the window needs its picture drawn again though the game gave no frame: the on-screen layer is another
   (vk_ui_changed), or the window is: its surface, its size or the .ini's vsync (what show makes a swapchain for), or an
   event of its own came (uncovered, moved, back from behind) */
static int refresh_due(void) {
    static u32 seen = ~0u; /* the window's events at the last refresh drawn */
    u32 events = backend_window_changes();
    int ww, wh, due;
    backend_window_size(&ww, &wh);
    due = g_lost || backend_window_generation() != g_generation || ww != g_vk.sc_win_w || wh != g_vk.sc_win_h ||
          __atomic_load_n(&g_settings.vsync, __ATOMIC_RELAXED) != g_vk.sc_vsync || events != seen;
    if (vk_ui_changed()) due = 1; /* asked whatever the window says: it makes the layer's parts, as a frame does */
    if (due) seen = events;
    return due;
}
static const char* vkr_device_name(void) { return g_vk.props.deviceName; }
/* The last frame again, under the controls and the menu, while the game shows none: a game loading, stuck, or drawing
   nothing still has them. Not before a quarter second without a frame (a slow game's own come between), at 30 a second
   at most, only when the GPU thread holds no Vulkan work (then the next call tries again), and only when the layer or
   the window changed: a still layer over a still frame is on the window already */
static void vkr_refresh(void) {
    static u64 last;
    u64 now = plat_ns();
    if (!backend_window_shown() || now - __atomic_load_n(&g_shown_ns, __ATOMIC_RELAXED) < 250000000ull ||
        now - last < 33000000ull)
        return;
    if (!vk_trylock()) return;
    last = now;
    if (refresh_due()) {
        vk_render_end();
        show(g_last, &g_last_hdr, g_last_w, g_last_h, g_last_transform, 0, 0, 0, 0);
    }
    vk_unlock();
}

/* a part of a surface (x, y, w x h), blitted to RGBA8 at dw x dh, scaled on the GPU, and read back: the pixels, good
   until the next readback, or NULL */
static const u8* surf_read(Surf* s, u32 x, u32 y, u32 w, u32 h, u32 transform, u32 dw, u32 dh) {
    /* blit to an RGBA8 image, whatever the target's format, then read that back. sRGB to sRGB, so the bytes stay
       encoded as the display shows them */
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = is_srgb(s->f.vk) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {dw, dh, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant aci = {.at = DM_DEVICE};
    VkImage tmp;
    DevMem tmem;
    if (!VK_OK(vk_devmem_image(&ii, &aci, &tmp, &tmem))) return NULL;
    vk_mem_track(tmem, VK_OWN_IMAGE_OTHER);
    VkBuffer buf;
    u8* p = vk_readback_buffer((u64)dw * dh * 4, &buf);
    if (!p) {
        vk_mem_untrack(tmem);
        vk_devmem_image_destroy(tmp, tmem);
        return NULL;
    }
    vk_render_end(); /* the blit, the copy and their barriers can't be recorded inside a rendering pass */
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{(i32)x, (i32)y, 0}, {(i32)(x + w), (i32)(y + h), 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {(i32)dw, (i32)dh, 1}}};
    if (transform & 4) {
        VkRect2D crop = {{(i32)x, (i32)y}, {w, h}}, destination = {{0, 0}, {dw, dh}};
        /* The raster copy covers the whole temporary image. Its LOAD still requires defined contents. */
        VkClearColorValue black = {{0, 0, 0, 1}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        rec_CmdClearColorImage(cb, tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        if (!vk_present_quad(s, tmp, ii.format, (VkExtent2D){dw, dh}, crop, destination, transform,
                             dw != h || dh != w)) {
            vk_defer_image(tmp, tmem, VK_NULL_HANDLE, VK_NULL_HANDLE);
            return NULL;
        }
    } else {
        flip_src(&blit, transform);
        rec_CmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                        &blit, dw == w && dh == h ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
    vk_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
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
    vk_mem_untrack(tmem);
    vk_devmem_image_destroy(tmp, tmem);
    return p;
}
/* the frame's picture on the trace's timeline (tracy_frame_images' seconds): a readback of what was just
   presented, which stops the frame it lands in, so a default run pays none of it */
static void frame_image(void) {
    static u64 last;
    u32 secs = g_settings.tracy_frame_images;
    const u8* px;
    u64 now;
    if (!secs || !g_last) return;
    now = plat_ns();
    if (last && now - last < (u64)secs * 1000000000ull) return;
    last = now;
    px = surf_read(g_last, g_last_x, g_last_y, g_last_w, g_last_h, g_last_transform, FRAME_IMAGE_W, FRAME_IMAGE_H);
    if (px) snx_tracy_frame_image(px, FRAME_IMAGE_W, FRAME_IMAGE_H);
}
/* a part of a surface, blitted to RGBA8 and read back: its hash, and a .ppm at path if given */
static u64 surf_save(Surf* s, u32 x, u32 y, u32 w, u32 h, u32 transform, const char* path) {
    u64 hash = 1469598103934665603ull;
    u32 dw = transform & 4 ? h : w, dh = transform & 4 ? w : h;
    const u8* px = surf_read(s, x, y, w, h, transform, dw, dh);
    if (!px) return hash;
    /* the pixels as the device wrote them, little-endian words: the hash is the same on every host */
    for (u64 i = 0; i < (u64)w * h; i++) hash = snx_fnv_step(hash, snx_ld_le32(px + i * 4));
    if (path) {
        char fn[1100];
        snprintf(fn, sizeof fn, "%s.ppm", path);
        FILE* f = fopen(fn, "wb");
        int ok = f != NULL;
        if (f) {
            ok = !snx_ppm_write(f, dw, dh, px, 4, (ptrdiff_t)dw * 4, SNX_PPM_RGBA);
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
    static u64 last_gdone, last_cdone;
    static int have;
    u64 n, gate, cgate, gsub, gdone, csub, cdone;
    vk_release_queued(&n, &gate, &cgate);
    vk_submit_progress(&gsub, &gdone);
    vk_compute_progress(&csub, &cdone);
    WARN("vulkan: %llu releases queued (the oldest waits for graphics submission %llu and compute batch %llu), "
         "graphics submissions done %llu of %llu, compute batches %llu of %llu, %llu bytes of commands not recorded\n",
         (unsigned long long)n, (unsigned long long)gate, (unsigned long long)cgate, (unsigned long long)gdone,
         (unsigned long long)gsub, (unsigned long long)cdone, (unsigned long long)csub,
         (unsigned long long)vk_rec_behind());
    /* a wedge is a GPU whose done count stopped moving, not merely one that is behind: name its last draw then */
    if (have && ((gdone < gsub && gdone == last_gdone) || (cdone < csub && cdone == last_cdone)))
        vk_checkpoint_dump("stall");
    last_gdone = gdone;
    last_cdone = cdone;
    have = 1;
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
    vk_upscale_forget();
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
    .device_name = vkr_device_name,
    .mem_may_have_changed = vkr_mem_may_have_changed,
    .stream_run = vk_ww_stream,
    .mem_seen = vk_ww_seen,
    .ready = vk_frame_ready,
    .clear = vkr_clear,
    .draw_texture = vkr_draw_texture,
    .discard = vkr_discard,
    .draw = vkr_draw,
    .xfb_control = vkr_xfb_control,
    .method_barrier = vkr_method_barrier,
    .blit = vkr_blit,
    .blit_filter = vkr_blit_filter,
    .upscale = vkr_upscale,
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
    .memory_text = vk_memory_text,
    .memory_blocks = vk_memory_blocks,
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
    .present_hdr = vkr_present_hdr,
    .hdr_display = vkr_hdr_display,
    .display_wait = vk_display_wait,
    .screenshot = vkr_screenshot,
    .capture = vk_renderdoc_capture,
    .thumbnail = vkr_thumbnail,
    .refresh = vkr_refresh,
    .save = vk_pipeline_save,
    .release_window = vk_window_release,
    .prepare = vk_pipeline_prepare,
};
const Renderer* g_renderer = &vk_renderer;
