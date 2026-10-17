/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vulkan renderer: the renderer interface (vk_renderer, the vkr_ functions it points to) and presenting.

   Render targets, clears, draws (gpu/vk/render.c), compute (gpu/vk/compute.c), write-backs to and reloads from
   guest memory, and presenting the game's framebuffer from its render target (or from memory, converted on the CPU,
   when no render target holds it). Everything that records runs under vk_lock(), which is reentrant: submissions and
   presents on the GPU thread (gpu/gpu.c), the menu's refresh on the main thread (vkr_refresh), screenshots on the
   thread asking, and shader results landing on whichever thread reads their memory first. Acquiring a swapchain image
   never waits for the display: with every image still on it or queued for it, that frame isn't shown (counted). */
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
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/renderer.h"

static VkSemaphore g_acquired[VK_FRAMES];
/* the CPU fallback's image, and the last presented frame for screenshots */
static Surf* g_cpu_img;
static Surf* g_last;
static u32 g_last_x, g_last_y, g_last_w, g_last_h;
static u32 g_last_transform;
static void vkr_before_write(u64 gva, u64 size) {
    if (size) vk_release_need(gva, size);
    vk_buf_before_write(gva, size);
    if (size)
        vk_gfx_image_mem_need(gva, size);
    else
        vk_gfx_image_land();
    vk_compute_before_write(gva, size);
    vk_rt_before_write(gva, size);
}
static void vkr_gfx_mem_need(u64 gva, u64 size) {
    vk_buf_mem_need(gva, size);
    vk_gfx_image_mem_need(gva, size);
}
static void vkr_mapping_begin(u64 gva, u64 size, int full) {
    vk_lock();
    vk_release_drain();
    if (full) {
        vk_buf_land_map();
        vk_gfx_image_land();
        vk_compute_land();
    } else {
        vk_buf_mem_need_map(gva, size);
        vk_gfx_image_mem_need(gva, size);
        vk_compute_mem_need(gva, size);
    }
}
static void vkr_mapping_end(void) { vk_unlock(); }

/* 0 when Vulkan can't run here */
static int vkr_start(void) {
    if (!vk_init()) return 0;
    g_vk_scale = g_opt_resf;       /* the .ini keeps resolution within 0.25 to 4 (options.c) */
    g_vk_anisotropy = g_opt_aniso; /* the sampler table keeps what it makes: anisotropy applies at start */
    if (g_vk_anisotropy > 1 && !g_vk.feat.samplerAnisotropy)
        LOG("vulkan: this GPU has no anisotropic filtering, so anisotropy=%d does nothing\n", g_vk_anisotropy);
    if (g_vk_scale != 1.0f) LOG("vulkan: render targets at %.2fx\n", g_vk_scale);
    vk_cmd_init();
    vk_pipeline_init();
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
static void vkr_mem_may_have_changed(void) { __atomic_add_fetch(&g_vk_epoch, 1, __ATOMIC_RELAXED); }
static void vkr_draw_texture(GpuChannel* ch) {
    vk_lock();
    vk_render_draw_texture(ch);
    vk_unlock();
}
static void vkr_clear(GpuChannel* ch, u32 v) {
    vk_lock();
    vk_render_clear(ch, v);
    vk_unlock();
}
static void vkr_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 bv, u32 bi, u32 instances) {
    g_draws++;
    vk_lock();
    vk_render_draw(ch, indexed, topology, first, count, bv, bi, instances);
    vk_unlock();
}
static void vkr_method_barrier(int kind) {
    vk_lock();
    if (kind == 3) {
        /* The TIC/TSC words are read from guest memory on each bind. Expire cached texture backing as well. */
        vk_tex_invalidate_all();
        __atomic_add_fetch(&g_vk_epoch, 1, __ATOMIC_RELAXED);
    } else {
        vk_render_end();
        /* The GPU queue orders work across this barrier, including WAIT_FOR_IDLE. CPU-visible releases
           are gated on GPU completion by the release path instead of stalling this command stream. */
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(vk_cmd(), &di);
        if (kind == 2) {
            /* Texture invalidation orders later reads after earlier GPU writes. The release path handles
               CPU-visible results; waiting for every invalidation stalls games that issue them per draw. */
            vk_tex_invalidate_all();
            __atomic_add_fetch(&g_vk_epoch, 1, __ATOMIC_RELAXED);
        }
    }
    vk_unlock();
}
static int vkr_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                    double sdy) {
    vk_lock();
    int r = vk_blit2d(s, d, dx, dy, dw, dh, sx, sy, sdx, sdy);
    vk_unlock();
    return r; /* 0: the front end copies through memory, with the renderer's rt_flush and rt_reload around it */
}
static int vkr_vic_compose(const VicJob* j) {
    static int off = -1;
    if (off < 0) off = getenv("SNX_NO_VIC_GPU") != NULL;
    if (off) return 0;
    return vk_vic_compose(j); /* 0: the caller composes in guest memory. It takes vk_lock where it needs it */
}
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
static void vkr_dispatch(GpuChannel* ch, const u32* q, u64 code) {
    static int off = -1;
    if (off < 0) off = getenv("SNX_NO_COMPUTE") != NULL;
    if (off) return;
    r3d_pending_flush(); /* a merged draw the front end is holding comes first */
    vk_lock();
    vk_dispatch(ch, q, code);
    vk_unlock();
}
/* Pending shader results land before the stream signals or waits. */
static void vkr_before_syncwait(void) {
    vk_buf_land_syncwait();
    vk_gfx_image_land();
    vk_compute_land();
}
static void vkr_stream_incr(u32 id) { vk_release_incr(id); }

/* ------------------------------------------------------------ presenting */
/* the frame from memory when no render target holds it (the game drew it on the CPU, or a video frame) */
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
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &g_cpu_img->img, &g_cpu_img->mem, NULL))) {
            free(g_cpu_img);
            g_cpu_img = NULL;
            return NULL;
        }
    }
    VkBuffer buf;
    u64 off;
    u32* px = (u32*)vk_stage((u64)w * h * 4, 4, &buf, &off);
    if (!px) return NULL;
    vi_convert_cpu(cpu_addr, w, h, pitch, layout, bhl, fmt, px);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, g_cpu_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy c = {
        .bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(cb, buf, g_cpu_img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    return g_cpu_img;
}

static int is_srgb(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

static int g_sc_srgb; /* what the swapchain was asked for (the surface may not have it) */
/* the window's surface: lost while the app is in the background (Android), made anew when it comes back */
static u32 g_generation;
static int g_lost;
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

/* The window's image: src's g_last_ rectangle (a frame of fw x fh, flipped by transform) letterboxed on black, or
   black alone, with the on-screen controls and the game's menu over it; then the frame's commands are submitted and
   the image presented. A game's frame (game) also moves the per-present bookkeeping, and without src shows nothing.
   Under vk_lock */
static void show(Surf* src, u32 fw, u32 fh, u32 transform, int game) {
    /* the size the swapchain was made for, not the extent the surface clamped it to: that can stay different */
    int ww, wh, srgb = src ? is_srgb(src->f.vk) : g_sc_srgb, shown = backend_window_shown();
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
        (ww != g_vk.sc_win_w || wh != g_vk.sc_win_h || srgb != g_sc_srgb ||
         __atomic_load_n(&g_opt_vsync, __ATOMIC_RELAXED) != g_vk.sc_vsync)) {
        vk_finish();
        g_sc_srgb = srgb;
        vk_swapchain_create(srgb);
        tried = 1;
    }
    VkSemaphore wait = VK_NULL_HANDLE, signal = VK_NULL_HANDLE;
    u32 idx = 0;
    int recreate = 0;
    if (shown && (src || !game) && (g_vk.swapchain || (!tried && vk_swapchain_create(srgb)))) {
        VkCommandBuffer cb = vk_cmd(); /* waits for this frame slot, so its acquire semaphore is free */
        VkSemaphore acq = g_acquired[vk_frame_id() % VK_FRAMES];
        /* no wait: vk_lock is held (guest threads mapping memory take it too), and the game's pace is present_pace's.
           Every image still on the display or queued for it (FIFO behind) leaves this frame unshown */
        VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, 0, acq, VK_NULL_HANDLE, &idx);
        if (r == VK_NOT_READY || r == VK_TIMEOUT) {
            /* the game faster than the display (vsync=1 with fps_limit over its refresh, or a 59.94 Hz one) */
            static u64 dropped;
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
            if (src)
                vk_surf_use(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT);
            /* the submit waits for the acquire at the clear, the image's first use */
            vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue black = {{0, 0, 0, 1}};
            VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
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
            if (src && fw && fh) {
                dw = cw;
                dh = (u32)((u64)cw * fh / fw);
                if (dh > ch) {
                    dh = ch;
                    dw = (u32)((u64)ch * fw / fh);
                }
                i32 dx = (i32)(cx + (cw - dw) / 2), dy = (i32)(cy + (ch - dh) / 2);
                VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .srcOffsets = {{(i32)g_last_x, (i32)g_last_y, 0},
                                                   {(i32)(g_last_x + g_last_w), (i32)(g_last_y + g_last_h), 1}},
                                    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .dstOffsets = {{dx, dy, 0}, {dx + (i32)dw, dy + (i32)dh, 1}}};
                flip_src(&blit, transform); /* render targets stay in guest order */
                vkCmdBlitImage(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            }
            {
                int safe[4] = {(int)cx, (int)cy, (int)cw, (int)ch};
                vk_ui_draw(cb, sc, safe);
            }
            vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        } else
            VK_OK(r);
    }
    if (game) {
        g_vk_present++;
        if (g_last) g_last->last_used = g_vk_present; /* what screenshots read stays */
        vk_tex_frame();
        vk_buf_frame();
        if (g_vk_present % 600 == 0) {
            LOG("vulkan: %llu draws, %llu skipped; %llu dispatches in %llu batches (%llu ms waiting), %llu skipped; "
                "%llu finishes (%llu ms)\n",
                (unsigned long long)g_vk_drawn, (unsigned long long)g_vk_skipped, (unsigned long long)g_vk_dispatches,
                (unsigned long long)g_vk_cs_batches,
                (unsigned long long)(__atomic_load_n(&g_vk_cs_wait_ns, __ATOMIC_RELAXED) / 1000000),
                (unsigned long long)g_vk_cs_skipped, (unsigned long long)g_vk_finishes,
                (unsigned long long)(g_vk_finish_ns / 1000000));
        }
    }
    /* Offscreen frames still submit and advance the frame ring. g_last remains available for readback. */
    vk_submit(wait, VK_PIPELINE_STAGE_2_CLEAR_BIT, signal);
    vk_frame_next(); /* every show used this frame's slot and acquire semaphore, the menu's own too */
    if (signal) {
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                               .waitSemaphoreCount = 1,
                               .pWaitSemaphores = &signal,
                               .swapchainCount = 1,
                               .pSwapchains = &g_vk.swapchain,
                               .pImageIndices = &idx};
        VkResult r = vkQueuePresentKHR(g_vk.gfx, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !g_vk.sc_turned))
            recreate = 1;
        else if (r == VK_ERROR_SURFACE_LOST_KHR)
            g_lost = 1;
        else
            VK_OK(r);
    }
    if (recreate && !g_lost && !g_vk.sc_lost) { /* not when this frame's create found the surface lost */
        vk_finish();
        vk_swapchain_create(srgb);
    }
    if (g_vk.sc_lost) { /* a create found the surface lost: the next frame makes it anew */
        g_vk.sc_lost = 0;
        g_lost = 1;
    }
}

/* the game queued a buffer for display: show it, letterboxed to the window. A frame that can't be shown (minimized,
   no image to draw to) is still submitted, with the per-present bookkeeping, so frames don't pile up */
static void vkr_present_buffer(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                               u32 transform) {
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
    show(src, x1 - x0, y1 - y0, transform, 1);
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
    show(g_last, g_last_w, g_last_h, g_last_transform, 0);
    vk_unlock();
}

/* a part of a surface, blitted to RGBA8 and read back: its hash, and a .ppm at path if given */
static u64 surf_save(Surf* s, u32 x, u32 y, u32 w, u32 h, u32 transform, const char* path) {
    u64 hash = 1469598103934665603ull;
    /* blit to an RGBA8 image, whatever the target's format, then read that back. sRGB to sRGB, so the bytes stay
       encoded as the display shows them */
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = is_srgb(s->f.vk) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage tmp;
    VmaAllocation tmem;
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &tmp, &tmem, NULL))) return hash;
    VkBuffer buf;
    u8* p = vk_readback_buffer((u64)w * h * 4, &buf);
    if (!p) {
        vmaDestroyImage(g_vk.vma, tmp, tmem);
        return hash;
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
                        .dstOffsets = {{0, 0, 0}, {(i32)w, (i32)h, 1}}};
    flip_src(&blit, transform); /* screenshots show the window's transform, offscreen and headless presents too */
    vkCmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_NEAREST);
    vk_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    vk_finish();
    vk_readback_ready();
    vmaDestroyImage(g_vk.vma, tmp, tmem);
    const u32* px = (const u32*)p;
    for (u64 i = 0; i < (u64)w * h; i++) hash = (hash ^ px[i]) * 1099511628211ull;
    if (path) {
        char fn[1100];
        snprintf(fn, sizeof fn, "%s.ppm", path);
        FILE* f = fopen(fn, "wb");
        int ok = f != NULL;
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (u64 i = 0; i < (u64)w * h && ok; i++) {
                u8 rgb[3] = {(u8)px[i], (u8)(px[i] >> 8), (u8)(px[i] >> 16)};
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

const Renderer vk_renderer = {
    .name = "Vulkan",
    .start = vkr_start,
    .mem_may_have_changed = vkr_mem_may_have_changed,
    .clear = vkr_clear,
    .draw_texture = vkr_draw_texture,
    .draw = vkr_draw,
    .method_barrier = vkr_method_barrier,
    .blit = vkr_blit,
    .vic_compose = vkr_vic_compose,
    .rt_flush = vkr_rt_flush,
    .rt_reload = vkr_rt_reload,
    .dispatch = vkr_dispatch,
    .cs_mem_need = vk_compute_mem_need,
    .gfx_mem_need = vkr_gfx_mem_need,
    .before_syncwait = vkr_before_syncwait,
    .stream_incr = vkr_stream_incr,
    .release_write = vk_release_write,
    .release_need = vk_release_need,
    .release_value = vk_release_value,
    .release_retire = vk_release_retire,
    .present_buffer = vkr_present_buffer,
    .screenshot = vkr_screenshot,
    .refresh = vkr_refresh,
    .save = vk_pipeline_save,
    .release_window = vk_window_release,
};
const Renderer* g_renderer = &vk_renderer;
