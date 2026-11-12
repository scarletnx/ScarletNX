/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Render targets by guest GPU address, in a table textures share (gpu/vk/texture.c). A render target lives on the
   GPU; vk_rt_flush writes it back to guest memory when the CPU side needs it (DMA and 2D copies), vk_rt_reload reads
   it again after the CPU side wrote that memory. Games reuse one address for render targets of different sizes and
   formats within a frame: a target whose address is taken by another is parked and reused when its size and format
   come back. Textures and render targets are each in a list from the least recently used to the most, which the
   caches' eviction takes from (vk_surf_reclaim): idle ones, and more when device memory nears its budget
   (vk_mem_trim, with the budget from gpu/vk/memory.c). */
#include "nx/gpu/vk/descriptor_objects.h"
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/resource_stats.h"
#include "nx/gpu/vk/tex_report.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/gpu/texture/astc_bc.h"
#include "nx/gpu/texture/blocklinear.h"
#include "nx/memory/own.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/runtime/state_io.h"

#define SURFH 4096
static Surf* g_surfh[SURFH];
/* The recency lists, from the least recently used to the most: textures (VK_LRU_TEX) and render targets (VK_LRU_RT,
   parked ones too), each with a second list (LRU_AGED + its own) of the old ones whose pixels would be lost if they
   went (lossless below), moved there as an eviction that takes only lossless ones passes them. Each stays in the
   order of last use: what is moved comes from the old end of the first list, older than all left there. A surface
   used again goes back to the recent end of the first. Both render target lists together are every real target, for
   overlapping guest-memory writes (vk_rt_before_write) */
#define LRU_AGED 2
static struct {
    Surf *head, *tail;
} g_lru[5];
static struct {
    u32 n;
    u64 bytes;
} g_held[3];         /* by VK_LRU_TEX or VK_LRU_RT: the surfaces and their memory */
static u32 g_parked; /* render targets parked */
static int lru_kind(const Surf* s) { return s->lru > LRU_AGED ? s->lru - LRU_AGED : s->lru; }
static void lru_link(Surf* s, int list) {
    s->lru = list;
    s->lru_next = NULL;
    s->lru_prev = g_lru[list].tail;
    if (s->lru_prev)
        s->lru_prev->lru_next = s;
    else
        g_lru[list].head = s;
    g_lru[list].tail = s;
}
static void lru_unlink(Surf* s) {
    int list = s->lru;
    if (s->lru_prev)
        s->lru_prev->lru_next = s->lru_next;
    else
        g_lru[list].head = s->lru_next;
    if (s->lru_next)
        s->lru_next->lru_prev = s->lru_prev;
    else
        g_lru[list].tail = s->lru_prev;
    s->lru = VK_LRU_NONE;
}
static u64 g_touched;
void vk_surf_touch(Surf* s) {
    if (s->last_used == g_vk_present) return;
    s->last_used = g_vk_present;
    if (!s->lru) return;
    s->touched = ++g_touched;
    int kind = lru_kind(s);
    lru_unlink(s);
    lru_link(s, kind);
}
static int lossless(const Surf* s);
/* s's pixels can be had again now (written back, loaded, or left undefined) and are taken out of the aged list, else
   a lossless eviction wouldn't find it and the idle one would keep it for five minutes. Into its first list in order
   of last use: near the old end, as everything there was used after everything aged, past only the others relisted */
void vk_surf_relist(Surf* s) {
    if (s->lru <= LRU_AGED || !lossless(s)) return;
    int kind = lru_kind(s);
    Surf* at = g_lru[kind].head; /* the first used after s */
    while (at && (i32)((u32)at->last_used - (u32)s->last_used) <= 0) at = at->lru_next;
    lru_unlink(s);
    s->lru = kind;
    s->touched = ++g_touched; /* past the others of its last_used */
    s->lru_next = at;
    s->lru_prev = at ? at->lru_prev : g_lru[kind].tail;
    if (s->lru_prev)
        s->lru_prev->lru_next = s;
    else
        g_lru[kind].head = s;
    if (at)
        at->lru_prev = s;
    else
        g_lru[kind].tail = s;
}
static void lru_add(Surf* s, int kind) {
    s->last_used = g_vk_present;
    s->touched = ++g_touched;
    lru_link(s, kind);
    g_held[kind].n++;
    g_held[kind].bytes += s->bytes;
}
void vk_surf_stats(int kind, u32* count, u64* bytes, u32* parked) {
    *count = g_held[kind].n;
    *bytes = g_held[kind].bytes;
    *parked = kind == VK_LRU_RT ? g_parked : 0;
}
void vk_surf_each(int kind, void (*fn)(void* ctx, const Surf* s), void* ctx) {
    for (int list = kind; list <= kind + LRU_AGED; list += LRU_AGED)
        for (const Surf* s = g_lru[list].head; s; s = s->lru_next) fn(ctx, s);
}
static _Thread_local int g_rt_flushing;
static u32 hkey(u64 a) { return (u32)((a >> 8) ^ (a >> 20)) & (SURFH - 1); }
u32 g_vk_surf_epoch;
static u32 g_surf_bucket_epoch[SURFH];
u32 vk_surf_epoch_at(u64 gva) { return g_surf_bucket_epoch[hkey(gva)]; }
/* a change of the surface table at gva: of its own bucket (a parked target's high bit leaves it in the same one) */
static void surf_changed(u64 gva) {
    g_vk_surf_epoch++;
    g_surf_bucket_epoch[hkey(gva)]++;
}

/* bytes of a block-linear surface of w_bytes x h, block height 2^bh GOBs (shrunk for small surfaces, as the
   hardware does) */
static u64 bl_size(u32 w_bytes, u32 h, u32 bh) {
    u32 gx = (w_bytes + 63) / 64, gy = (h + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    u64 gys = ((u64)(gy + (1u << bh) - 1) >> bh) << bh;
    return (u64)gx * gys * 512;
}
static u32 bl_height(u32 h, u32 bh) {
    u32 gy = (h + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    return bh;
}
VkImageAspectFlags vk_aspect(const VkFmt* f) {
    if (!f->depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (f->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

static void surf_barrier(VkCommandBuffer cb, const Surf* s, VkPipelineStageFlags2 source_stage,
                          VkAccessFlags2 source_access, VkImageLayout old_layout, VkPipelineStageFlags2 stage,
                          VkAccessFlags2 access, VkImageLayout layout) {
    VkSampleLocationEXT positions[16];
    VkSampleLocationsInfoEXT locations;
    VkImageMemoryBarrier2 image = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                    .pNext = s->sample_locations_compatible && old_layout != VK_IMAGE_LAYOUT_UNDEFINED
                                                 ? vk_sample_locations_barrier(&s->sample_locations,
                                                                               vk_surf_samples(s), &locations,
                                                                               positions)
                                                 : NULL,
                                    .srcStageMask = source_stage, .srcAccessMask = source_access,
                                    .dstStageMask = stage, .dstAccessMask = access,
                                    .oldLayout = old_layout, .newLayout = layout,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = s->img,
                                    .subresourceRange = {vk_aspect(&s->f), 0, VK_REMAINING_MIP_LEVELS, 0,
                                                         VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                    .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &image};
    rec_CmdPipelineBarrier2(cb, &dependency);
}
void vk_surf_use(VkCommandBuffer cb, Surf* s, VkImageLayout layout, VkPipelineStageFlags2 stage,
                 VkAccessFlags2 access) {
    int writes = (access & (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) != 0;
    int wrote = (s->access & (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) != 0;
    if (s->layout == layout && !writes && !wrote) {
        /* read after read in the same layout needs no barrier, unless the reader is in a stage the barrier that made
           the last write visible didn't name (a copy after a blit read it): a barrier from the earlier readers
           extends that visibility to it */
        if (stage & ~s->stage) {
            vk_render_end();
            surf_barrier(cb, s, s->stage ? s->stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       layout, stage, access, layout);
        }
        s->stage |= stage;
        s->access |= access;
        return;
    }
    vk_render_end(); /* no barriers on images inside a rendering pass */
    surf_barrier(cb, s, s->stage ? s->stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, s->access,
               s->layout, stage, access, layout);
    s->layout = layout;
    s->stage = stage;
    s->access = access;
}

float g_vk_scale = 1.0f;
u32 vk_sc(u32 v) {
    u32 r = (u32)((float)v * g_vk_scale + 0.5f);
    return r ? r : 1;
}
float vk_scf(float v) { return v * g_vk_scale; }

static u64 g_rt_made; /* for the stats line */
u64 vk_rt_made(void) { return g_rt_made; }
/* the guest's width and height of s: its pixels', or a multisampled one's samples' (DS_MSAA_) */
static u32 guest_w(const Surf* s) { return s->w << ds_msaa_log2_x(s->msaa); }
static u32 guest_h(const Surf* s) { return s->h << ds_msaa_log2_y(s->msaa); }
/* whether a w x h image of s's texel size laid out as given (block linear with blocks 2^bh GOBs tall, or pitch linear
   rows pitch bytes apart) has each of its pixels at the guest bytes where single-sampled s has it: no larger than s,
   its rows as many GOBs wide (or as far apart) and its blocks as tall. Dynamic resolution writes and samples a target
   through such a smaller header */
int vk_rt_same_layout(const Surf* s, u32 w, u32 h, int bl, u32 bh, u32 pitch) {
    if (s->msaa || w > s->w || h > s->h || !bl != !s->bl) return 0;
    if (!bl) return pitch == s->pitch;
    return (w * s->f.bpp + 63) / 64 == (s->w * s->f.bpp + 63) / 64 && bl_height(h, bh) == bl_height(s->h, s->bh);
}
/* a surface of w by h pixels with the samples of the sample layout msaa (render targets only: textures have one) */
static Surf* surf_create_(u64 gva, u32 w, u32 h, VkFmt f, u32 fkey, u32 bl, u32 bh, u32 pitch, int is_rt, u32 layers,
                         u32 msaa) {
    Surf* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->gva = gva;
    s->w = w;
    s->h = h;
    s->sw = is_rt ? vk_sc(w) : w;
    s->sh = is_rt ? vk_sc(h) : h;
    s->layers = layers;
    s->levels = 1;
    s->depth3d = 1; /* one slice deep, which a storage image over the target compares (tex_from_tic) */
    s->fkey = fkey;
    s->f = f;
    s->bl = bl;
    s->bh = bh;
    s->pitch = pitch;
    s->is_rt = is_rt;
    s->msaa = (u8)msaa;
    if (is_rt) g_rt_made++;
    if (is_rt) s->layer_size = bl ? bl_size(guest_w(s) * f.bpp, guest_h(s), bh) : (u64)pitch * guest_h(s);
    s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (is_rt) usage |= f.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    /* the draws rendering to a color target can sample it, in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL (gpu/vk/render.c) */
    if (is_rt && !f.depth && g_vk.has_feedback) usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    /* sRGB has no storage-image format: the ordered blend shader writes the same bytes through a view of the linear
       twin, which EXTENDED_USAGE allows on an image that stays sRGB. The image's own format must stay the target's:
       vkCmdBlitImage converts by it, so a linear image blitted to an sRGB one (the presentation's swapchain) is
       encoded twice and comes out washed out */
    VkFormat storage_format = vk_srgb_as(f.vk, 0);
    VkImageCreateFlags flags = 0;
    VkFormatProperties format_props;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, storage_format, &format_props);
    if ((!msaa || g_vk.feat.shaderStorageImageMultisample) &&
        (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        VkImageCreateFlags storage_flags =
            storage_format != f.vk ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0;
        VkImageFormatProperties image_props;
        if (vkGetPhysicalDeviceImageFormatProperties(g_vk.phys, f.vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                     usage | VK_IMAGE_USAGE_STORAGE_BIT, storage_flags,
                                                     &image_props) == VK_SUCCESS) {
            usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            flags |= storage_flags;
            s->storage_capable = 1;
        }
    }
    /* a multisampled color image's samples move to and from guest memory through views of their raw bytes
       (gpu/vk/msaa.c), when the device renders and samples those with its samples */
    VkFormat raw = VK_FORMAT_UNDEFINED;
    if (msaa && !f.depth) {
        raw = vk_raw_format(f.bpp);
        VkImageUsageFlags moves = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (raw && !vk_ms_supported(raw, moves, 0, msaa)) raw = VK_FORMAT_UNDEFINED;
        if (raw && raw != f.vk) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }
    if (f.depth && g_vk.sample_locations.available && g_vk.props.limits.standardSampleLocations &&
        ds_msaa_samples(msaa) <= 16)
        flags |= VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT;
    if (msaa && s->storage_capable && !vk_ms_supported(f.vk, usage, flags, msaa)) {
        /* Optional storage support must not remove an otherwise supported ordinary render target. */
        usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
        flags &= ~VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        s->storage_capable = 0;
    }
    if (msaa && !vk_ms_supported(f.vk, usage, flags, msaa)) {
        LOG_LIMIT(4, "vulkan: the GPU makes no %u-sample images of format %d: the target isn't drawn\n",
                  ds_msaa_samples(msaa), (int)f.vk);
        free(s);
        return NULL;
    }
    /* depth moves only as the formats that copy out as they are (depth_copies_raw) */
    s->ms_raw = msaa && (raw || f.vk == VK_FORMAT_D32_SFLOAT || f.vk == VK_FORMAT_D16_UNORM);
    s->type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .flags = flags,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = f.vk,
                            .extent = {s->sw, s->sh, 1},
                            .mipLevels = 1,
                            .arrayLayers = layers,
                            .samples = vk_surf_samples(s),
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkViewFormats vf; /* sRGB and linear views, and the raw one */
    s->sample_locations_compatible = (ii.flags & VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT) != 0;
    if (s->sample_locations_compatible) s->sample_locations = vk_sample_locations_standard(vk_surf_samples(s));
    vk_view_formats(&ii, &vf, raw);
    /* a scaled target can pass the allocation cap (4x of 4K is 506 MiB in RGBA8). Room is made for it under the
       budget: the caller holds only surfaces it used this present, which stay */
    if (!vk_cache_image_create(&ii, &s->img, &s->mem, VK_OWN_TARGET, &s->bytes)) {
        free(s);
        return NULL;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = s->img,
                                .viewType = s->type,
                                .format = f.vk,
                                .subresourceRange = {vk_aspect(&f), 0, 1, 0, layers}};
    VkImageViewUsageCreateInfo view_usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                            .usage = usage & ~VK_IMAGE_USAGE_STORAGE_BIT};
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && storage_format != f.vk) vi.pNext = &view_usage;
    VK_OK(vk_image_view_create(g_vk.dev, &vi, NULL, &s->att));
    u32 k = hkey(gva);
    s->next = g_surfh[k];
    g_surfh[k] = s;
    surf_changed(gva);
    if (is_rt) lru_add(s, VK_LRU_RT);
    return s;
}
static Surf* surf_create(u64 gva, u32 w, u32 h, VkFmt f, u32 fkey, u32 bl, u32 bh, u32 pitch, int is_rt, u32 layers,
                         u32 msaa) {
    VkResourceScope scope = vk_resource_begin();
    Surf* s = surf_create_(gva, w, h, f, fkey, bl, bh, pitch, is_rt, layers, msaa);
    vk_resource_end(VK_RESOURCE_SURFACE_CREATE, scope);
    return s;
}

/* SET_SRGB_WRITE selects the attachment view, leaving the image's stored bytes and sampling format intact. */
VkImageView vk_rt_attachment(Surf* s, int srgb_write) {
    VkFormat linear = vk_srgb_as(s->f.vk, 0);
    if (srgb_write || linear == s->f.vk) return s->att;
    if (!s->att_linear) {
        /* an attachment in the feedback-loop layout needs the loop's usage on its view, and sampling
           (VUID-VkRenderingAttachmentInfo-imageView-10780) */
        VkImageViewUsageCreateInfo usage = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
            .usage =
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                (g_vk.has_feedback ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0)};
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .pNext = &usage,
                                    .image = s->img,
                                    .viewType = s->type,
                                    .format = linear,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, s->layers}};
        VK_OK(vk_image_view_create(g_vk.dev, &vi, NULL, &s->att_linear));
    }
    return s->att_linear;
}

Surf* vk_surf_first(u64 gva) { return g_surfh[hkey(gva)]; }
void vk_surf_add(Surf* s) {
    u32 k = hkey(s->gva);
    s->next = g_surfh[k];
    g_surfh[k] = s;
    surf_changed(s->gva);
    lru_add(s, VK_LRU_TEX);
}

/* The live render targets (parked ones aren't) by address, open addressed. vk_rt_find is asked about 4,000 times a
   frame in one game, mostly by texture lookups, and walking the hash chain read two or three Surfs scattered in
   memory each time: a twentieth of its GPU thread. An address has at most one live target (rt_create parks the one
   there before it makes or takes another), and a few hundred live at once, so the table stays in the cache. Without
   it (an allocation failed) the chain is walked */
typedef struct {
    u64 gva;
    Surf* s; /* NULL: a free slot */
} RtSlot;
static RtSlot* g_rtx;
static u32 g_rtx_mask, g_rtx_n;
static int g_rtx_failed;
static u32 rtx_home(u64 gva) { return (u32)((gva * 0x9E3779B97F4A7C15ull) >> 40) & g_rtx_mask; }
static void rtx_put(RtSlot* t, u32 mask, u64 gva, Surf* s) {
    u32 i = (u32)((gva * 0x9E3779B97F4A7C15ull) >> 40) & mask;
    while (t[i].s) i = (i + 1) & mask;
    t[i].gva = gva;
    t[i].s = s;
}
/* The same live targets in order of address, for the targets over a range (rt_pending_over), with the largest span
   any of them had: one game reports a samples count dozens of times a frame, and each release walked every target it
   kept, live or parked, twice (a twentieth of its GPU thread). Without it (an allocation failed) the lists are
   walked */
static u64 rt_bytes(const Surf* s);
static Surf** g_rts;
static u32 g_nrts, g_caprts;
static u64 g_rts_span;
static int g_rts_failed;
/* the first live target at or past address a */
static u32 rts_lower(u64 a) {
    u32 lo = 0, hi = g_nrts;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_rts[mid]->gva < a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
static void rts_add(Surf* s) {
    if (g_rts_failed) return;
    if (g_nrts == g_caprts) {
        u32 cap = g_caprts ? g_caprts * 2 : 256;
        Surf** n = realloc(g_rts, (size_t)cap * sizeof *n);
        if (!n) {
            g_rts_failed = 1;
            return;
        }
        g_rts = n;
        g_caprts = cap;
    }
    u32 at = rts_lower(s->gva);
    memmove(&g_rts[at + 1], &g_rts[at], (size_t)(g_nrts - at) * sizeof *g_rts);
    g_rts[at] = s;
    g_nrts++;
    if (rt_bytes(s) > g_rts_span) g_rts_span = rt_bytes(s);
}
static void rts_del(u64 gva) {
    if (g_rts_failed) return;
    u32 at = rts_lower(gva);
    if (at < g_nrts && g_rts[at]->gva == gva) {
        memmove(&g_rts[at], &g_rts[at + 1], (size_t)(g_nrts - at - 1) * sizeof *g_rts);
        g_nrts--;
    }
}
static void rtx_add(Surf* s) {
    rts_add(s);
    if (g_rtx_failed) return;
    if ((g_rtx_n + 1) * 2 > (g_rtx ? g_rtx_mask + 1 : 0)) {
        u32 cap = g_rtx ? (g_rtx_mask + 1) * 2 : 1024;
        RtSlot* t = calloc(cap, sizeof *t);
        if (!t) {
            g_rtx_failed = 1;
            free(g_rtx);
            g_rtx = NULL;
            return;
        }
        for (u32 i = 0; g_rtx && i <= g_rtx_mask; i++)
            if (g_rtx[i].s) rtx_put(t, cap - 1, g_rtx[i].gva, g_rtx[i].s);
        free(g_rtx);
        g_rtx = t;
        g_rtx_mask = cap - 1;
    }
    rtx_put(g_rtx, g_rtx_mask, s->gva, s);
    g_rtx_n++;
}
/* the live target at gva out of the table, the slots after it shifted back so that every probe still finds its own */
static void rtx_del(u64 gva) {
    u32 i, j;
    rts_del(gva);
    if (!g_rtx) return;
    for (i = rtx_home(gva); g_rtx[i].s && g_rtx[i].gva != gva; i = (i + 1) & g_rtx_mask) {}
    if (!g_rtx[i].s) return;
    g_rtx_n--;
    for (j = (i + 1) & g_rtx_mask; g_rtx[j].s; j = (j + 1) & g_rtx_mask) {
        u32 h = rtx_home(g_rtx[j].gva);
        /* j's entry stays when its home lies cyclically in (i, j] */
        if (i <= j ? (i < h && h <= j) : (i < h || h <= j)) continue;
        g_rtx[i] = g_rtx[j];
        i = j;
    }
    g_rtx[i].s = NULL;
}
Surf* vk_rt_find(u64 gva) {
    if (g_rtx_failed) {
        for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
            if (s->gva == gva && s->is_rt) return s;
        return NULL;
    }
    if (!g_rtx) return NULL;
    for (u32 i = rtx_home(gva); g_rtx[i].s; i = (i + 1) & g_rtx_mask)
        if (g_rtx[i].gva == gva) return g_rtx[i].s;
    return NULL;
}

VkImage vk_unscaled_image(const Surf* s, u32 layers, DevMem* mem) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = s->f.vk,
                            .extent = {s->w, s->h, 1},
                            .mipLevels = 1,
                            .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage img = VK_NULL_HANDLE;
    if (!vk_image_create(&ii, &img, mem)) return VK_NULL_HANDLE;
    return img;
}
static int blit_ok(VkFormat f, VkFormatFeatureFlags need) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & need) == need;
}
/* a scaled target's contents move between its image and guest memory by blits: 0 when its format can't be blitted
   (said once) */
static int moves_scaled(const Surf* s) {
    if (s->msaa) return vk_ms_moves(s); /* a multisampled one's samples move by draws, unscaled (gpu/vk/msaa.c) */
    if (s->sw == s->w && s->sh == s->h) return 1;
    if (blit_ok(s->f.vk, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) return 1;
    LOG_LIMIT(1, "vulkan: format %d can't be scaled: render targets in it don't move to or from guest memory\n",
              (int)s->f.vk);
    return 0;
}
/* src (TRANSFER_SRC) scaled into dst (TRANSFER_DST), whole images */
static void scale_blit(VkCommandBuffer cb, VkImage src, u32 sw, u32 sh, u32 src_layer, VkImage dst, u32 dw, u32 dh,
                       u32 dst_layer, int nearest) {
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, src_layer, 1},
                     .srcOffsets = {{0, 0, 0}, {(i32)sw, (i32)sh, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, dst_layer, 1},
                     .dstOffsets = {{0, 0, 0}, {(i32)dw, (i32)dh, 1}}};
    rec_CmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                     nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
}

/* the guest bytes of a render target: block linear, or pitch linear (its last row ends after the row's bytes, not
   after a whole pitch) */
static u64 rt_layer_bytes(const Surf* s) {
    u64 row = (u64)guest_w(s) * s->f.bpp;
    return s->bl ? bl_size((u32)row, guest_h(s), s->bh) : (u64)s->pitch * (guest_h(s) - 1) + row;
}
/* whether s's slices are one 3D volume in guest memory (a 3D render target's layers, a 3D texture's depth, written
   to as a storage image), not layers a layer_size apart */
static int surf_volume(const Surf* s) { return s->volume || s->type == VK_IMAGE_VIEW_TYPE_3D; }
static u32 surf_slices(const Surf* s) { return s->type == VK_IMAGE_VIEW_TYPE_3D ? s->depth3d : s->layers; }
/* a target's guest bytes, all layers: kept in rt_size once rt_create has made it, as its shape doesn't change */
static u64 rt_bytes(const Surf* s) {
    if (s->rt_size) return s->rt_size;
    if (surf_volume(s) && s->bl) {
        u64 slice = rt_layer_bytes(s);
        u32 depth = (surf_slices(s) + (1u << s->bd) - 1) & ~((1u << s->bd) - 1);
        return slice * depth;
    }
    return s->layer_size * (s->layers - 1) + rt_layer_bytes(s);
}
u64 vk_surf_guest_size(const Surf* s) { return s ? rt_bytes(s) : 0; }
static u64 g_wseq;
/* every image's write generations, its uploads' and its writes' alike, from one count: a surface counting its own
   writes ran ahead of the uploads' count, and an upload then handed it a generation its image would reach again with
   guest memory still at it, so its next write looked written back already and never landed */
static u32 g_surf_gen;
u32 vk_surf_gen(void) { return ++g_surf_gen; }
/* What a texture takes from the drawn targets over it (texture.c tex_take_drawn) changes only with the targets in its
   range and their pixels, so a texture that had nothing left to take is passed over while this count hasn't moved. It
   moves when a target enters the list of drawn targets or leaves it, is parked or taken out of parking, and when one
   vk_rt_drawn_in returned since its last write is written or drawn again (take_seen). A texture searched each time
   it was bound, and found the targets it had copied already: most of its searches in one game */
static u64 g_take_gen = 1;
u64 vk_rt_take_gen(void) { return g_take_gen; }
static void take_changed(Surf* s) {
    if (!s->take_seen) return;
    s->take_seen = 0;
    g_take_gen++;
}
void vk_surf_wrote(Surf* s) {
    take_changed(s);
    s->wgen = vk_surf_gen();
    s->wseq = ++g_wseq;
}
/* the GPU pages the render targets not parked cover, so a write that meets none (almost all) skips the walk over every
   target in vk_rt_before_write: a target adds its pages when it is made or taken out of parking, and removes them when it
   is parked or freed */
static PendPages g_rt_pages;
static void rt_pages(const Surf* s, int delta) {
    if (!(s->gva >> 63)) pend_add_va(&g_rt_pages, s->gva, s->rt_size, delta);
}
int vk_surf_can_writeback(const Surf* s) { return s && s->f.raw && !s->f.depth && moves_scaled(s); }

/* s's layers, in rows of pitch bytes from off in buf, a layer slice bytes after the last, copied into its image
   (scaled when it is): 0 when that can't be done */
static int rt_load_copy(VkCommandBuffer cb, Surf* s, VkBuffer buf, u64 off, u32 pitch, u64 slice) {
    if (s->msaa) { /* the samples drawn from the guest's layout of them, a layer at a time */
        for (u32 layer = 0; layer < s->layers; layer++)
            if (!vk_ms_pack(cb, s, layer, buf, off + (u64)layer * slice, pitch)) return 0;
        return 1;
    }
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (u32 layer = 0; layer < s->layers; layer++) {
        VkBufferImageCopy c = {.bufferOffset = off + (u64)layer * slice,
                               .bufferRowLength = pitch / s->f.bpp,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
                               .imageExtent = {s->w, s->h, 1}};
        DevMem tm;
        VkImage t = s->sw == s->w && s->sh == s->h ? VK_NULL_HANDLE : vk_unscaled_image(s, 1, &tm);
        if (!t)
            rec_CmdCopyBufferToImage(cb, buf, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        else {
            c.imageSubresource.baseArrayLayer = 0;
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            rec_CmdCopyBufferToImage(cb, buf, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            /* a bigger image takes each of the guest's texels whole, in all its pixels: blended with its neighbours,
               a table's entry (a row of parameters, a list a compute program stored) has a part of the next one */
            scale_blit(cb, t, s->w, s->h, 0, s->img, s->sw, s->sh, layer, s->f.is_int || s->sw > s->w || s->sh > s->h);
            vk_defer_image(t, tm, VK_NULL_HANDLE, VK_NULL_HANDLE);
        }
    }
    return 1;
}
/* fn(user, at, host, n) for each run of guest memory behind the GPU range [gva, gva + size): n bytes at host, at
   byte offset at of the range. A target's pages need not be contiguous in guest memory (a virtual pool maps them
   page by page). Unmapped pages are skipped */
static void span_runs(u64 gva, u64 size, void (*fn)(void*, u64, u8*, u64), void* user) {
    for (u64 off = 0; off < size;) {
        u64 cpu = 0, n = gmmu_run(gva + off, size - off, &cpu);
        if (!n) {
            n = 0x1000 - ((gva + off) & 0xfff);
            if (n > size - off) n = size - off;
        } else
            fn(user, off, GP(cpu), n);
        off += n;
    }
}
/* the writes on a run so far taken into write tracking: the ones after count as later (*user: 1 when tracked) */
static void run_take(void* user, u64 at, u8* host, u64 n) {
    (void)at;
    if (vk_written_since(host, n, 0) >= 0) *(int*)user = 1;
}
/* a run's hot pages protected again, once its writes were taken in (vk_ww_cool): from the baseline after, a page
   reported written was written */
static void run_cool(void* user, u64 at, u8* host, u64 n) {
    (void)user;
    (void)at;
    vk_ww_cool(host, n);
}
/* s's image and guest memory agree as of now, or the image replaced all of memory: the CPU's writes from now on are
   newer than its pixels (mem_wseen). 0 without write tracking. The writes before now are taken in first, so they
   don't count as later ones */
static void mem_baseline(Surf* s) {
    u64 size = rt_bytes(s);
    int tracked = 0;
    if (!(s->gva >> 63) && size) span_runs(s->gva, size, run_take, &tracked);
    if (tracked) span_runs(s->gva, size, run_cool, NULL);
    s->mem_wseen = tracked ? vk_ww_now() : 0;
}
static void rt_loaded(Surf* s) {
    s->mem_wgen = s->wgen; /* memory and image hold the same now: nothing to write back */
    s->draw_wseen = 0;     /* a successful full load starts a new source epoch for later draws */
    mem_baseline(s);
    s->gw = gpu_mem_generation();
    vk_surf_relist(s);
}
/* the moves of a block linear target's layers: one of every slice of a volume, one a layer of an array (each layer
   at layer_size from the last, where the layout starts again) */
static VkBlMove rt_bl_move(const Surf* s, u32 pitch) {
    u32 h = guest_h(s);
    VkBlMove m = {guest_w(s) * s->f.bpp, h, bl_height(h, s->bh), 0, 0, h, 0, 1, pitch};
    if (surf_volume(s)) {
        m.bd = s->bd;
        m.slices = surf_slices(s);
    }
    return m;
}
/* a block linear target's load that found no buffer (what) of bytes for its rows from y of a layer: 0 */
static int load_fail(const Surf* s, u32 layer, u32 y, const char* what, u64 bytes) {
    LOG_LIMIT(4,
              "vulkan: a target at %llx found no %s of %llu bytes for its rows from %u of layer %u: memory's pixels "
              "there don't reach its image\n",
              (unsigned long long)s->gva, what, (unsigned long long)bytes, y, layer);
    return 0;
}
/* rows [y, y + n) of a layer of an unscaled single-sampled target, in rows of pitch bytes from the start of buf,
   copied into its image */
static void rt_load_rows(VkCommandBuffer cb, Surf* s, VkBuffer buf, u32 pitch, u32 layer, u32 y, u32 n) {
    VkBufferImageCopy c = {.bufferRowLength = pitch / s->f.bpp,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
                           .imageOffset = {0, (i32)y, 0},
                           .imageExtent = {s->w, n, 1}};
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    rec_CmdCopyBufferToImage(cb, buf, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}
/* s's layers a band of rows at a time, as flush_parts reads them back: whole rows of blocks, VK_PART_SHARE of the cap
   at most (a row of blocks at least), as the scratch buffer stays as big as the most asked of it. Each band is staged
   from where it lies and deswizzled into the start of the scratch buffer: the barrier before its move orders the copy
   of the band before out of there */
static int load_bands(Surf* s, u32 pitch) {
    u32 h = guest_h(s), band = 8u << bl_height(h, s->bh);
    u32 rows = (u32)(g_vk.alloc_cap / VK_PART_SHARE / pitch) / band * band;
    if (!rows) rows = band;
    for (u32 l = 0; l < s->layers; l++)
        for (u32 y = 0; y < h; y += rows) {
            VkBlMove m = rt_bl_move(s, pitch);
            VkBuffer staged, scratch;
            u64 lo, hi, off, cap;
            m.y0 = y;
            m.rows = h - y < rows ? h - y : rows;
            vk_bl_range(&m, &lo, &hi);
            u64 from = s->gva + (u64)l * s->layer_size + lo;
            u8* p = vk_stage_guest_input(from, hi - lo, hi - lo, &staged, &off);
            if (!p) return load_fail(s, l, y, "staging", hi - lo);
            vk_guest_read(from, hi - lo, p, staged, off);
            /* after the read, which may land GPU results and submit: the command buffer, and the scratch buffer if
               they grew it, are the ones from then */
            VkCommandBuffer cb = vk_cmd();
            if (!(scratch = vk_bl_scratch((u64)m.rows * pitch, &cap)))
                return load_fail(s, l, y, "scratch buffer", (u64)m.rows * pitch);
            vk_bl_barrier(cb, 1);
            if (!vk_bl_move(cb, &m, 0, staged, off, scratch, 0)) return 0; /* said there */
            vk_bl_barrier(cb, 0);
            rt_load_rows(cb, s, scratch, pitch, l, y, m.rows);
        }
    return 1;
}
/* a block linear target loaded: its guest bytes staged as they are, from where they are, and deswizzled on the GPU.
   One whose rows need more than VK_PART_SHARE of the cap goes in bands (load_bands); a multisampled one goes whole,
   its layers drawn from the guest's layout of them (vk_ms_pack), and so do a scaled one, blitted from an unscaled
   image, and a volume, its slices one move */
static int surf_load_bl(Surf* s) {
    u64 sz = rt_bytes(s);
    u32 pitch = (guest_w(s) * s->f.bpp + 15) & ~15u, moves = s->volume ? 1 : s->layers, h = guest_h(s);
    VkBlMove m = rt_bl_move(s, pitch);
    u64 lin = (u64)pitch * h * s->layers, cap;
    VkBuffer staged, scratch;
    u64 off;
    vk_render_end();
    if (lin > g_vk.alloc_cap / VK_PART_SHARE && !s->msaa && s->sw == s->w && s->sh == s->h && !surf_volume(s))
        return load_bands(s, pitch);
    u8* p = vk_stage_guest_input(s->gva, sz, sz, &staged, &off);
    if (!p) return load_fail(s, 0, 0, "staging", sz);
    if (!(scratch = vk_bl_scratch(lin, &cap))) return load_fail(s, 0, 0, "scratch buffer", lin);
    vk_guest_read(s->gva, sz, p, staged, off); /* what the GPU holds of it copied on the GPU */
    VkCommandBuffer cb = vk_cmd();
    vk_bl_barrier(cb, 1);
    for (u32 i = 0; i < moves; i++)
        if (!vk_bl_move(cb, &m, 0, staged, off + (u64)i * s->layer_size, scratch, (u64)i * pitch * h)) return 0;
    vk_bl_barrier(cb, 0);
    return rt_load_copy(cb, s, scratch, 0, pitch, (u64)pitch * h);
}
/* a pitch linear one: its guest bytes staged as they lie, the copy taking the pitch, when it is whole texels (else a
   row at a time into tight rows); what the GPU holds of them copied on the GPU */
static int surf_load_pitch(Surf* s) {
    u32 gh = guest_h(s);
    u64 sz = rt_bytes(s), row = (u64)guest_w(s) * s->f.bpp;
    int as_is = s->pitch % s->f.bpp == 0;
    VkBuffer buf;
    u64 off;
    vk_render_end();
    u8* p = vk_stage_guest_input(s->gva, sz, as_is ? sz : row * gh * s->layers, &buf, &off);
    if (!p) return 0;
    if (as_is)
        vk_guest_read(s->gva, sz, p, buf, off);
    else
        for (u32 layer = 0; layer < s->layers; layer++)
            for (u32 y = 0; y < gh; y++) {
                u64 to = ((u64)layer * gh + y) * row;
                vk_guest_read(s->gva + (u64)layer * s->layer_size + (u64)y * s->pitch, row, p + to, buf, off + to);
            }
    if (as_is) return rt_load_copy(vk_cmd(), s, buf, off, s->pitch, s->layer_size);
    return rt_load_copy(vk_cmd(), s, buf, off, (u32)row, row * gh);
}
int vk_ms_load(Surf* s) { return vk_ms_moves(s) && (s->bl ? surf_load_bl(s) : surf_load_pitch(s)); }

static void rt_reload(u64 gva) {
    Surf* s = vk_rt_find(gva);
    if (!s) return;
    vk_surf_wrote(s); /* the copies of it (crops) are out of date */
    if (s->f.depth || !s->f.raw || !moves_scaled(s)) {
        /* new memory over a target we can't load as is: memory is the truth. Its contents are undefined until it is
           drawn to again (textures over it are loaded from memory meanwhile, and it isn't presented); the stage and
           access stay for the next barrier to wait on */
        vk_render_end();
        s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        s->draw_wseen = 0;
        s->gw = gpu_mem_generation();
        vk_surf_relist(s);
        return;
    }
    if (s->bl ? surf_load_bl(s) : surf_load_pitch(s)) {
        rt_loaded(s);
        return;
    }
    /* a load that failed left the image behind memory (its old pixels, or some bands of the new ones): undefined
       until it is drawn to again, as above, so it is never written back over memory */
    s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s->draw_wseen = 0;
    vk_surf_relist(s);
}
void vk_rt_reload(u64 gva) {
    VkResourceScope scope = vk_resource_begin();
    rt_reload(gva);
    vk_resource_end(VK_RESOURCE_RT_RELOAD, scope);
}

/* S8's target, the one depth format of a byte a texel: its stencil aspect is the guest's bytes */
static int stencil_alone(const Surf* s) { return s->f.depth && s->f.bpp == 1; }
/* a depth target whose Vulkan format holds the guest's bytes (Z32F as D32_SFLOAT, Z16 as D16_UNORM, S8's stencil): its
   depth (or stencil) aspect copies out in the guest's own layout. The packed ones (the Z24S8 kinds, Z32F_X24S8) are
   packed on the GPU (vk_depth_pack); a scaled or multisampled packed one would need a depth blit first and stays on
   the GPU, as a multisampled S8 one, whose samples vk_ms_unpack reads from the depth */
static int depth_copies_raw(const Surf* s) {
    return (s->f.vk == VK_FORMAT_D32_SFLOAT || s->f.vk == VK_FORMAT_D16_UNORM || stencil_alone(s)) && s->sw == s->w &&
           s->sh == s->h && (!s->msaa || (vk_ms_moves(s) && !stencil_alone(s)));
}
static VkImageAspectFlags flush_aspect(const Surf* s) {
    return !s->f.depth        ? VK_IMAGE_ASPECT_COLOR_BIT
           : stencil_alone(s) ? VK_IMAGE_ASPECT_STENCIL_BIT
                              : VK_IMAGE_ASPECT_DEPTH_BIT;
}
static int depth_packs(const Surf* s) {
    return !depth_copies_raw(s) && vk_depth_pack_mode(s->fkey & 0xFFFF) >= 0 && s->sw == s->w && s->sh == s->h &&
           !s->msaa;
}
/* the bytes s's pixels take read back, or 0 when they don't go to memory: a format guest memory can't hold as it is, no
   pixels, or not drawn to since it was last written back or loaded (memory has it already, and a CPU write since then
   is newer than the target). A block linear target comes back in its guest layout, swizzled on the GPU */
static u64 flush_bytes(const Surf* s) {
    if (!s || (s->f.depth ? !depth_copies_raw(s) && !depth_packs(s) : !s->f.raw) ||
        s->layout == VK_IMAGE_LAYOUT_UNDEFINED)
        return 0;
    if (s->mem_wgen == s->wgen || !moves_scaled(s)) return 0;
    return s->bl ? rt_bytes(s) : (u64)guest_w(s) * s->f.bpp * guest_h(s) * s->layers;
}
/* What a readback copies: rows [y, y + rows) of layers [layer, layer + layers), at the guest's size. A target over
   one allocation goes in parts (flush_parts), else all of it in one */
typedef struct {
    u32 layer, layers, y, rows;
} FlushPart;
static FlushPart flush_whole(const Surf* s) {
    FlushPart w = {0, s->layers, 0, guest_h(s)};
    return w;
}
/* part's pixels of s copied into buf at at, in rows of pitch bytes, a layer (or a 3D image's slice) after another. A
   packed depth layer goes whole (flush_parts gives it whole rows) */
static void flush_copy(VkCommandBuffer cb, Surf* s, VkBuffer buf, u64 at, u32 pitch, const FlushPart* part) {
    u64 slice = (u64)pitch * part->rows;
    u32 depth = s->type == VK_IMAGE_VIEW_TYPE_3D ? s->depth3d : 1; /* a 3D image's slices in one copy */
    if (s->msaa) { /* the part's samples drawn into the guest's layout of them a layer at a time, copied from there */
        for (u32 layer = part->layer; layer < part->layer + part->layers; layer++) {
            VkImage grid = vk_ms_unpack(cb, s, layer, part->y, part->rows);
            if (!grid) continue;
            VkBufferImageCopy c = {
                .bufferOffset = at + (u64)(layer - part->layer) * slice,
                .bufferRowLength = pitch / s->f.bpp,
                .imageSubresource = {s->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageExtent = {guest_w(s), part->rows, 1}};
            rec_CmdCopyImageToBuffer(cb, grid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        }
        return;
    }
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    for (u32 layer = part->layer; layer < part->layer + part->layers; layer++) {
        if (depth_packs(s)) { /* flush_bytes took it: it packs */
            vk_depth_pack(cb, s, layer, buf, at + (u64)(layer - part->layer) * slice, pitch);
            continue;
        }
        VkBufferImageCopy c = {.bufferOffset = at + (u64)(layer - part->layer) * slice,
                               .bufferRowLength = pitch / s->f.bpp,
                               .imageSubresource = {flush_aspect(s), 0, layer, 1},
                               .imageOffset = {0, (i32)part->y, 0},
                               .imageExtent = {s->w, part->rows, depth}};
        DevMem tm;
        VkImage t = s->sw == s->w && s->sh == s->h ? VK_NULL_HANDLE : vk_unscaled_image(s, 1, &tm);
        if (!t)
            rec_CmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        else {
            c.imageSubresource.baseArrayLayer = 0;
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            scale_blit(cb, s->img, s->sw, s->sh, layer, t, s->w, s->h, 0, s->f.is_int);
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            rec_CmdCopyImageToBuffer(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
            vk_defer_image(t, tm, VK_NULL_HANDLE, VK_NULL_HANDLE);
        }
    }
}
/* part of a block linear target's pixels into buf at at in the guest's layout: the image copied into the scratch
   buffer and swizzled from there into buf, a layer at layer_size from the last (a band of rows: its rows of blocks,
   from the first, vk_bl_range). 0 when that can't be done (said) */
static int flush_swizzle(VkCommandBuffer cb, Surf* s, VkBuffer buf, u64 at, const FlushPart* part) {
    u32 pitch = (guest_w(s) * s->f.bpp + 15) & ~15u, moves = surf_volume(s) ? 1 : part->layers;
    u64 cap, need = (u64)pitch * part->rows * (surf_volume(s) ? surf_slices(s) : part->layers);
    VkBlMove m = rt_bl_move(s, pitch);
    m.y0 = part->y;
    m.rows = part->rows;
    VkBuffer scratch = vk_bl_scratch(need, &cap);
    if (!scratch) {
        LOG_LIMIT(4, "vulkan: a target at %llx found no scratch buffer of %llu bytes for its readback: nothing lands\n",
                  (unsigned long long)s->gva, (unsigned long long)need);
        return 0;
    }
    flush_copy(cb, s, scratch, 0, pitch, part);
    vk_bl_barrier(cb, 1);
    for (u32 i = 0; i < moves; i++)
        if (!vk_bl_move(cb, &m, 1, buf, at + (u64)i * s->layer_size, scratch, (u64)i * pitch * part->rows)) return 0;
    vk_bl_barrier(cb, 0);
    return 1;
}
/* part of s's pixels into buf at at as flush_bytes says: tightly packed rows, or the guest's block linear layout
   (flush_swizzle). Rows that need more than VK_PART_SHARE of the cap in scratch go a layer's band at a time, as the
   scratch buffer stays as big as the most asked of it: whole rows of blocks, each band's bytes where the part's layout
   has them, the next band's copy into the scratch buffer after the moves out of it (vk_bl_barrier). A volume and a
   packed depth layer go whole, as flush_parts takes them, and a scaled target too: each band would blit its layer */
static int flush_record(VkCommandBuffer cb, Surf* s, VkBuffer buf, u64 at, const FlushPart* part) {
    u32 row = guest_w(s) * s->f.bpp, pitch = (row + 15) & ~15u;
    u64 limit = g_vk.alloc_cap / VK_PART_SHARE, first, lo, hi;
    if (!s->bl) {
        flush_copy(cb, s, buf, at, row, part);
        return 1;
    }
    if ((u64)pitch * part->rows * part->layers <= limit || surf_volume(s) || depth_packs(s) ||
        (!s->msaa && (s->sw != s->w || s->sh != s->h)))
        return flush_swizzle(cb, s, buf, at, part);
    u32 band = 8u << bl_height(guest_h(s), s->bh), rows = (u32)(limit / pitch) / band * band,
        end = part->y + part->rows;
    VkBlMove m = rt_bl_move(s, pitch);
    if (!rows) rows = band;
    m.y0 = part->y;
    m.rows = part->rows;
    vk_bl_range(&m, &first, &hi); /* the part's bytes in buf start at its first row of blocks */
    for (u32 i = 0; i < part->layers; i++)
        for (u32 y = part->y; y < end; y += rows) {
            FlushPart b = {part->layer + i, 1, y, end - y < rows ? end - y : rows};
            m.y0 = y;
            m.rows = b.rows;
            vk_bl_range(&m, &lo, &hi);
            if (!flush_swizzle(cb, s, buf, at + (u64)i * s->layer_size + (lo - first), &b)) return 0;
        }
    return 1;
}
/* the copies recorded so far done, and their bytes readable by the host */
static void flush_wait(VkCommandBuffer cb) {
    /* the copy's writes made visible to the host reads below (the fence wait alone doesn't) */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    vk_finish();
    vk_readback_ready();
}
/* A write-back keeps what the CPU wrote over a target after its draws. On the console the GPU's writes reached memory
   as it drew, and a game reuses that memory once its sync wait says the GPU is done: the CPU's bytes there are newer
   than the pixels. The renderer holds the pixels until something needs them, by then over the CPU's bytes. One game
   records command lists where a target it no longer draws was: a GPU write beside the target wrote it back over a
   queued job's records. Write tracking tells the pages written since the target's draw baseline (draw_wseen, taken
   at its first draw in a present). A page the CPU wrote between two draws keeps the CPU's bytes too, the later
   draw's pixels on it lost: what the page rule costs */
#define CPU_RUNS 64
typedef struct {
    u64 run[CPU_RUNS][2]; /* [from, to) of the span, in order */
    u32 n;
    u32 since; /* the clock the runs are written after */
    u64 base;  /* the span's offset of the guest run being looked at */
} CpuRuns;
static void cpu_run(void* user, u64 at, u64 n) {
    CpuRuns* r = (CpuRuns*)user;
    at += r->base;
    if (r->n == CPU_RUNS) { /* more: the last run reaches to this one's end, the pages between keeping memory too */
        r->run[CPU_RUNS - 1][1] = at + n;
        return;
    }
    r->run[r->n][0] = at;
    r->run[r->n][1] = at + n;
    r->n++;
}
/* the runs of s's span [s->gva, s->gva + size) the CPU wrote since s's image last matched memory (mem_wseen), else
   since its draw baseline: 0 when none, or when write tracking can't tell (the old write-back of every pixel then).
   The draw baseline alone missed a target that a copy changed after a load, with no draw: it has none */
static void run_newer(void* user, u64 at, u8* host, u64 n) {
    CpuRuns* r = (CpuRuns*)user;
    r->base = at;
    if (vk_written_since(host, n, r->since) > 0) vk_written_runs(host, n, r->since, cpu_run, r);
}
static u32 cpu_newer(const Surf* s, u64 size, CpuRuns* r) {
    r->n = 0;
    r->since = s->mem_wseen ? s->mem_wseen : s->draw_wseen;
    if (r->since) span_runs(s->gva, size, run_newer, r);
    return r->n;
}
/* after a write-back of s: a new draw baseline, so what the CPU writes from now on is newer, and the write-back's own
   writes aren't. With pages kept (kept), s's image isn't memory there: no baseline, and its generation from before
   (gw0), with the span marked written, so its next use loads it from memory again (vk_rt_note_read) */
static void flush_rebase(Surf* s, u64 size, int kept, u64 gw0) {
    int tracked = 0;
    if (kept) {
        gpu_mem_written(s->gva, size);
        s->gw = gw0;
        s->draw_wseen = 0;
        s->mem_wseen = 0;
        return;
    }
    s->gw = gpu_mem_generation();
    span_runs(s->gva, size, run_take, &tracked);
    if (tracked) span_runs(s->gva, size, run_cool, NULL);
    if (s->draw_wseen && tracked) s->draw_wseen = vk_ww_now();
    s->mem_wseen = tracked ? vk_ww_now() : 0;
}
/* the guest pages [gva + from, gva + to) a write-back wrote, for write tracking: they hold the GPU's bytes, which a
   later scan doesn't count as the CPU's while they stay as written (vk_ww_landed). The scan reads them through the
   alias pool's mirror, as own.c's landings give them: a page the GPU holds by then is trapped where the guest sees
   it. A page outside the pool isn't noted, and its write counts as the CPU's */
static void run_landed(void* user, u64 at, u8* host, u64 n) {
    u8* mirror = (u8*)user;
    u64 off;
    (void)at;
    for (u64 cpu = (u64)(host - g_mem), end = cpu + n; cpu < end; cpu += 0x1000)
        if (!mem_pool_offset(cpu, &off)) vk_ww_landed(cpu, 0x1000, mirror + off);
}
static void flush_note_landed(u64 gva, u64 from, u64 to) {
    u64 lo = (gva + from) & ~0xfffull, hi = (gva + to + 0xfff) & ~0xfffull;
    u8* mirror = plat_alias_mirror();
    if (to > from && mirror) span_runs(lo, hi - lo, run_landed, mirror);
}
static u32 rt_pending_over(u64 gva, u64 size, Surf** local, u32 nlocal, Surf*** out);
typedef struct {
    Surf** surfaces;
    u32 count, capacity;
} RtPendingList;

/* Collect under the GMMU alias lock, then flush after it is released. A flush
   writes guest memory and asks the alias index about those writes itself. */
static void rt_pending_alias(void* ctx, u64 gva, u64 size) {
    RtPendingList* pending = ctx;
    Surf *local[16], **list;
    u32 count;
    if (!pend_hit_va(&g_rt_pages, gva, size)) return;
    count = rt_pending_over(gva, size, local, 16, &list);
    for (u32 i = 0; i < count; i++) {
        Surf* s = list[i];
        u32 at = 0;
        while (at < pending->count && pending->surfaces[at]->wseq < s->wseq) at++;
        if (at < pending->count && pending->surfaces[at] == s) continue;
        if (pending->count == pending->capacity) {
            u32 capacity = pending->capacity * 2;
            if (capacity < pending->capacity || (u64)capacity > SIZE_MAX / sizeof *pending->surfaces)
                fatal("Too many render target aliases");
            Surf** grown = malloc((size_t)capacity * sizeof *grown);
            if (!grown) fatal("Out of memory collecting render target aliases");
            memcpy(grown, pending->surfaces, (size_t)pending->count * sizeof *grown);
            if (pending->capacity > 16) free(pending->surfaces);
            pending->surfaces = grown;
            pending->capacity = capacity;
        }
        memmove(pending->surfaces + at + 1, pending->surfaces + at,
                (size_t)(pending->count - at) * sizeof *pending->surfaces);
        pending->surfaces[at] = s;
        pending->count++;
    }
    if (list != local) free(list);
}
/* The targets over s's bytes whose images are newer than s's and newer than memory, with nothing written under them
   since they last matched it: s's landing goes under them, as rt_flush_older leaves them pending on top, so it is no
   write of theirs: loading memory there would drop their pixels for bytes that never held them. Those at other GPU
   addresses showing the same guest memory too: one game maps one physical pool through several virtual pools, and
   its cutscenes' post-processing draws its targets through one of them over the bytes of the shadow map drawn
   earlier through another. Landing that shadow map late (under a compute program's result published over the same
   bytes) left the newer target looking written, and its next use loaded the old bytes over its pixels: a cutscene's
   whole frame showed pieces of old frames. In local (nlocal of 16) while it holds them, else in a list the caller
   frees */
static u32 rt_newer_over(const Surf* s, u64 size, Surf** local, u32 nlocal, Surf*** out) {
    RtPendingList pending = {local, 0, nlocal};
    u32 k = 0;
    *out = local;
    if ((s->gva >> 63) || !size) return 0;
    for (u64 off = 0; off < size;) { /* each run of guest memory s's addresses show, and every address showing it */
        u64 cpu = 0, n = gmmu_run(s->gva + off, size - off, &cpu);
        if (n)
            gmmu_aliases(cpu, n, rt_pending_alias, &pending);
        else if ((n = 0x1000 - ((s->gva + off) & 0xfff)) > size - off)
            n = size - off;
        off += n;
    }
    for (u32 i = 0; i < pending.count; i++) {
        Surf* t = pending.surfaces[i];
        if (t != s && t->wseq > s->wseq && !gpu_mem_dirty_since(t->gw, t->gva, rt_bytes(t))) pending.surfaces[k++] = t;
    }
    *out = pending.surfaces;
    return k;
}
/* s's pixels, read back to p as of its write generation wgen, into guest memory in its layout (or over original, a
   storage image's merge), but for the pages the CPU wrote since s's draws (cpu_newer) */
static void flush_land(Surf* s, const u8* p, const u8* original, u64 wgen, u8** keep, u64* keep_size) {
    u32 gh = guest_h(s);
    u64 gva = s->gva, row = (u64)guest_w(s) * s->f.bpp, slice = row * gh;
    u64 sz = rt_bytes(s), gw0 = gpu_mem_generation();
    CpuRuns newer;
    u32 kept = original ? 0 : cpu_newer(s, sz, &newer);
    u8* dst = original || kept ? NULL : gmem_ptr(gva, sz);
    u8* tmp = NULL;
    if (!dst) {
        tmp = malloc(sz);
        if (!tmp) return;
        if (original)
            memcpy(tmp, original, sz);
        else
            gmem_read(gva, tmp, sz);
        dst = tmp;
    }
    if (s->bl && surf_volume(s))
        bl_land(dst, p, (u32)row, gh, surf_slices(s), bl_height(gh, s->bh), s->bd);
    else
        for (u32 layer = 0; layer < s->layers; layer++) {
            u8* layer_dst = dst + (u64)layer * s->layer_size;
            if (s->bl)
                bl_land(layer_dst, p + (u64)layer * s->layer_size, (u32)row, gh, 1, bl_height(gh, s->bh), 0);
            else
                for (u32 y = 0; y < gh; y++)
                    memcpy(layer_dst + (u64)y * s->pitch, p + (u64)layer * slice + y * row, row);
        }
    Surf *above_local[16], **above;
    u32 nabove = rt_newer_over(s, sz, above_local, 16, &above);
    int guest_changed = 0, flushing = g_rt_flushing;
    g_rt_flushing = 1; /* s's own write: the targets newer than s over its bytes stay pending (vk_rt_before_write) */
    if (original) {
        const u8* current = gmem_ptr(gva, sz);
        u8* current_copy = NULL;
        if (!current) {
            current_copy = malloc(sz);
            if (!current_copy) fatal("Out of memory comparing storage image writeback");
            gmem_read(gva, current_copy, sz);
            current = current_copy;
        }
        guest_changed = memcmp(current, original, sz) != 0;
        free(current_copy);
        s->mem_wgen = wgen; /* avoid flushing this target again when writeback checks overlapping targets */
        vk_write_changed(gva, dst, original, (u32)sz);
        if (keep) { /* the image's bytes as this landing leaves them, for a later draw's write-back */
            *keep = tmp;
            *keep_size = sz;
        } else
            free(tmp);
    } else if (kept) { /* the runs between the CPU's */
        u64 at = 0;
        for (u32 i = 0; i <= kept; i++) {
            u64 to = i < kept ? newer.run[i][0] : sz;
            if (to > at) {
                gmem_write(gva + at, tmp + at, to - at);
                flush_note_landed(gva, at, to);
            }
            if (i < kept) at = newer.run[i][1];
        }
        free(tmp);
    } else if (tmp) {
        gmem_write(gva, tmp, sz);
        flush_note_landed(gva, 0, sz);
        free(tmp);
    } else {
        gpu_mem_written(gva, sz); /* caches built from this memory check it again */
        flush_note_landed(gva, 0, sz);
    }
    g_rt_flushing = flushing;
    s->mem_wgen = wgen;
    if (original)
        s->gw = gpu_mem_generation();
    else
        flush_rebase(s, sz, kept != 0, gw0);
    for (u32 i = 0; i < nabove; i++) above[i]->gw = gpu_mem_generation(); /* their images are still the newest */
    if (above != above_local) free(above);
    vk_surf_relist(s);
    if (!s->is_rt) vk_tex_note_writeback(s);
    if (guest_changed) {
        /* The image still holds the shader result where the CPU won the merge. Force the next bind to upload
           the merged guest bytes, even though note_writeback just hashed those same bytes. */
        s->hash ^= 1;
        s->gw = 0;
    }
}
/* The targets over [gva, gva + size) holding pixels guest memory hasn't got, oldest image first: memory freed and
   given to a new target still has the old target's pixels pending, and where two cover the same bytes the newer one's
   go on top. In local while it holds nlocal, else in a list the caller frees */
static u32 rt_pending_over(u64 gva, u64 size, Surf** local, u32 nlocal, Surf*** out) {
    Surf** list = local;
    u32 n = 0, cap = nlocal;
    /* the live targets that can reach gva, from the index, or every target the lists keep */
    u32 at = g_rts_failed ? 0 : rts_lower(gva > g_rts_span ? gva - g_rts_span : 0);
    Surf* walk = g_rts_failed ? g_lru[VK_LRU_RT].head : NULL;
    int lru = VK_LRU_RT;
    for (;;) {
        Surf* s;
        if (!g_rts_failed) {
            if (at >= g_nrts || g_rts[at]->gva >= gva + size) break;
            s = g_rts[at++];
        } else {
            while (!walk && lru < VK_LRU_RT + LRU_AGED) walk = g_lru[lru += LRU_AGED].head;
            if (!walk) break;
            s = walk;
            walk = walk->lru_next;
        }
        if ((s->gva >> 63) || s->gva >= gva + size || s->gva + rt_bytes(s) <= gva || !flush_bytes(s)) continue;
        if (n == cap) {
            Surf** grown = (Surf**)malloc((size_t)cap * 2 * sizeof *grown);
            if (!grown) fatal("Out of memory listing the render targets over %llx", (unsigned long long)gva);
            memcpy(grown, list, (size_t)n * sizeof *grown);
            if (list != local) free(list);
            list = grown;
            cap *= 2;
        }
        u32 i = n++;
        for (; i && list[i - 1]->wseq > s->wseq; i--) list[i] = list[i - 1];
        list[i] = s;
    }
    *out = list;
    return n;
}
static void surf_flush(Surf* s, const u8* original);
/* the targets over s's bytes whose images are older than s's and not written back yet go to guest memory first,
   oldest first, so s's bytes land over theirs as the console's GPU wrote them; newer ones stay pending, on top */
static void rt_flush_older(const Surf* s) {
    Surf *local[16], **list;
    u64 size = rt_bytes(s);
    u32 n, i;
    if ((s->gva >> 63) || !size || !pend_hit_va(&g_rt_pages, s->gva, size)) return;
    n = rt_pending_over(s->gva, size, local, 16, &list);
    for (i = 0; i < n && list[i]->wseq < s->wseq; i++) surf_flush(list[i], NULL);
    if (list != local) free(list);
}
/* part of s's pixels read back, bytes of them, to to */
static int flush_part(Surf* s, const FlushPart* part, u8* to, u64 bytes) {
    VkBuffer buf;
    const u8* p = vk_readback_buffer(bytes, &buf);
    if (!p) {
        LOG_LIMIT(4, "vulkan: a target at %llx found no readback buffer of %llu bytes: nothing lands\n",
                  (unsigned long long)s->gva, (unsigned long long)bytes);
        return 0;
    }
    VkCommandBuffer cb = vk_cmd();
    if (!flush_record(cb, s, buf, 0, part)) return 0;
    flush_wait(cb);
    memcpy(to, p, bytes);
    return 1;
}
/* s's pixels, size bytes of them, when they are more than one allocation may be: read back a part at a time into one
   host buffer, which lands as one. A part is whole layers, or a layer's band of rows (block linear: whole rows of
   blocks) when one is more than VK_PART_SHARE of the cap. A 3D image goes whole, and a packed depth layer too */
static void flush_parts(Surf* s, u64 size, const u8* original) {
    u64 limit = g_vk.alloc_cap / VK_PART_SHARE;
    u32 h = guest_h(s), row = guest_w(s) * s->f.bpp, pitch = (row + 15) & ~15u;
    /* a layer's bytes as flush_land takes them: block linear layers layer_size apart, pitch linear rows packed */
    u64 layer = !s->bl ? (u64)row * h : s->layers > 1 ? s->layer_size : rt_layer_bytes(s);
    u64 per = layer <= limit ? limit / layer : 0;
    u32 band = s->bl ? 8u << bl_height(h, s->bh) : 1, rows = (u32)(limit / (s->bl ? pitch : row)) / band * band;
    if (!rows && (u64)band * pitch <= g_vk.alloc_cap) rows = band; /* a row of blocks is the least a band takes */
    if (surf_volume(s) || (!per && (depth_packs(s) || !rows))) {
        LOG_LIMIT(4,
                  "vulkan: a target at %llx of %llu bytes, %llu a layer, is more than an allocation may be (%llu) and "
                  "does not go in parts: nothing lands\n",
                  (unsigned long long)s->gva, (unsigned long long)size, (unsigned long long)layer,
                  (unsigned long long)g_vk.alloc_cap);
        return;
    }
    u8* all = (u8*)malloc(size);
    if (!all) {
        LOG_LIMIT(4, "vulkan: a target at %llx found no %llu bytes of host memory for its readback: nothing lands\n",
                  (unsigned long long)s->gva, (unsigned long long)size);
        return;
    }
    int ok = 1;
    for (u32 l = 0; ok && l < s->layers;) {
        if (per) { /* whole layers */
            FlushPart part = {l, s->layers - l < per ? s->layers - l : (u32)per, 0, h};
            u64 at = (u64)l * layer, bytes = (u64)part.layers * layer;
            ok = flush_part(s, &part, all + at, bytes < size - at ? bytes : size - at);
            l += part.layers;
            continue;
        }
        for (u32 y = 0; ok && y < h; y += rows) { /* a layer's bands of rows */
            FlushPart part = {l, 1, y, h - y < rows ? h - y : rows};
            u64 at = (u64)y * row, bytes = (u64)part.rows * row;
            if (s->bl) { /* its rows of blocks, from the first */
                VkBlMove m = rt_bl_move(s, pitch);
                m.y0 = y;
                m.rows = part.rows;
                vk_bl_range(&m, &at, &bytes);
                bytes -= at;
            }
            ok = flush_part(s, &part, all + (u64)l * layer + at, bytes);
        }
        l++;
    }
    if (ok) flush_land(s, all, original, s->wgen, NULL, NULL);
    free(all);
}
static void surf_flush(Surf* s, const u8* original) {
    u64 size = flush_bytes(s);
    VkBuffer buf;
    if (!size) return;
    rt_flush_older(s);
    vk_render_end();
    if (size > g_vk.alloc_cap) {
        flush_parts(s, size, original);
        return;
    }
    const u8* p = vk_readback_buffer(size, &buf);
    if (!p) {
        LOG_LIMIT(4, "vulkan: a target at %llx found no readback buffer of %llu bytes: nothing lands\n",
                  (unsigned long long)s->gva, (unsigned long long)size);
        return;
    }
    VkCommandBuffer cb = vk_cmd();
    FlushPart whole = flush_whole(s);
    if (!flush_record(cb, s, buf, 0, &whole)) return;
    flush_wait(cb);
    flush_land(s, p, original, s->wgen, NULL, NULL);
}
u64 vk_surf_flush_size(const Surf* s) { return flush_bytes(s); }
void vk_surf_flush_record(Surf* s, VkBuffer buf, u64 at) {
    VkCommandBuffer cb;
    rt_flush_older(s);
    vk_render_end();
    FlushPart whole = flush_whole(s);
    cb = vk_cmd();
    flush_record(cb, s, buf, at, &whole);
    /* the copy's writes made visible to the host reads that land it, once its submit is done */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
}
void vk_surf_flush_land(Surf* s, const u8* p, const u8* original, u64 wgen, u8** keep, u64* keep_size) {
    if (keep) *keep = NULL;
    vk_result_begin();
    flush_land(s, p, original, wgen, original ? keep : NULL, keep_size);
    vk_result_end();
}
void vk_surf_flush(Surf* s, const u8* original) {
    vk_result_begin();
    surf_flush(s, original);
    vk_result_end();
}
void vk_rt_flush(u64 gva) { vk_surf_flush(vk_rt_find(gva), NULL); }
/* The pending targets wholly in [gva, gva + size), whose pixels vk_rt_overlay copied into a buffer about to be
   published over those bytes: guest memory (its owner) has them from then on, so they are pending no more. Left
   pending, publishing the buffer flushed each again (vk_rt_before_write_cpu), a wait for the GPU and its readback, and
   the landing of the bytes' last owner under it: one game draws four 32x32 targets of 256 KB and writes them with a
   compute program every frame, four waits and four landings a frame. Their images keep their pixels, older than
   memory's (the publish stamps the bytes written), so they load memory's before they are used again, as after a
   flush */
static void publish_span(const Surf* s, VkOwnRes* r, u64 at, u64 n);
/* With r (s's publish), r is published over each one's bytes once all are retired: publishing one flushes the
   pending targets over it (vk_rt_before_write_cpu) */
static void rt_retire_over(u64 gva, u64 size, const Surf* s, VkOwnRes* r) {
    Surf *local[16], **list;
    u32 count;
    if (!pend_hit_va(&g_rt_pages, gva, size)) return;
    count = rt_pending_over(gva, size, local, 16, &list);
    for (u32 pass = 0; pass < (r ? 2u : 1u); pass++)
        for (u32 i = 0; i < count; i++) {
            Surf* t = list[i];
            if (t->gva < gva || t->gva + rt_bytes(t) > gva + size) continue; /* vk_rt_overlay flushed those */
            if (pass) {
                publish_span(s, r, t->gva - s->gva, rt_bytes(t));
                continue;
            }
            t->mem_wgen = t->wgen;
            vk_surf_relist(t);
        }
    if (list != local) free(list);
}
/* r's bytes [at, at + n) of s's span published over the guest memory there, a run of it at a time (an unmapped page
   takes no write), and stamped written */
static void publish_span(const Surf* s, VkOwnRes* r, u64 at, u64 n) {
    for (u64 off = at, end = at + n; off < end;) {
        u64 cpu = 0, k = gmmu_run(s->gva + off, end - off, &cpu);
        if (!k) {
            k = 0x1000 - ((s->gva + off) & 0xFFF);
            if (k > end - off) k = end - off;
        } else
            vk_own_publish(cpu, k, r, off);
        off += k;
    }
    gpu_mem_written(s->gva + at, n);
}
/* r published over s's own bytes: each layer's from where it starts (a volume's span whole). The bytes between its
   layers aren't s's, and keep their owners: published as the GPU saw them, they changed nothing, but they were stamped
   written all the same, and all that is built from them was loaded again. One game writes each level of its reflection
   cubes through a view of that level, whose layers are a whole mip chain apart, so each view's span holds the other
   views' levels: every view and every cube loaded all its memory again each frame, 30 uploads and 12 MB */
static void publish_own(const Surf* s, VkOwnRes* r, u64 size) {
    u64 own = rt_layer_bytes(s);
    if (surf_volume(s) || s->layers < 2 || own >= s->layer_size) {
        publish_span(s, r, 0, size);
        return;
    }
    for (u32 i = 0; i < s->layers; i++) publish_span(s, r, (u64)i * s->layer_size, own);
}
/* A storage image's results kept on the GPU (docs/layers/gpu/ownership.md), as the DMA engine's destination is: its span
   staged as the GPU sees it into a buffer of its own (the bytes between its texels keep theirs), its texels laid out
   over that in the guest's layout, and the buffer published over its texels and the targets taken in (publish_own,
   rt_retire_over) run by run of guest memory. Nothing waits:
   the bytes land when the CPU touches them. A 3D volume a game writes twice a frame and only samples took a wait
   each, a fifth of the run. A pitch-linear one's rows go at the guest's pitch, over the bytes between them as the
   guest has them. 0 when it can't be (pitch-linear rows of part texels, or layers not a pitch's rows apart, no room):
   the caller flushes at once */
static int surf_publishable(const Surf* s) {
    return s->bl || (!(s->pitch % s->f.bpp) && s->type != VK_IMAGE_VIEW_TYPE_3D &&
                     (s->layers == 1 || s->layer_size == (u64)s->pitch * guest_h(s)));
}
int vk_surf_publish(Surf* s) {
    u64 size = flush_bytes(s);
    u32 mem_wgen = s ? s->mem_wgen : 0;
    VkOwnRes* r;
    VkCommandBuffer cb;
    if (!size || (s->gva >> 63)) return 0;
    if (!s->bl) {
        if (!surf_publishable(s)) return 0;
        size = rt_bytes(s);
    }
    vk_render_end();
    CpuRuns newer;
    u32 kept = cpu_newer(s, size, &newer);
    u64 gw0 = gpu_mem_generation(), kat = 0, kbytes = 0;
    VkBuffer kbuf = VK_NULL_HANDLE;
    r = vk_own_res_new(size, VK_OWN_BY_TARGET);
    if (!r) return 0;
    /* straight into the buffer that becomes the span's owner, as the copy engine's destinations go (dma.c): the bytes
       the GPU owns there are copied on the GPU, and only the CPU's cross the bus, once. Staged in host memory, the span
       went out and came back several times, 12 to 14 MB a frame in one game */
    if (!vk_guest_read_device(s->gva, size, r->buf, 0)) {
        vk_own_res_unref(r);
        return 0;
    }
    for (u32 i = 0; i < kept; i++) kbytes += newer.run[i][1] - newer.run[i][0];
    if (kept) { /* the CPU's runs as guest memory has them, before other targets go over the span */
        u64 k = 0;
        if (!vk_stage_device(kbytes, vk_stage_align(), &kbuf, &kat)) {
            vk_own_res_unref(r);
            return 0;
        }
        cb = vk_cmd();
        for (u32 i = 0; i < kept; i++) {
            VkBufferCopy kc = {newer.run[i][0], kat + k, newer.run[i][1] - newer.run[i][0]};
            rec_CmdCopyBuffer(cb, r->buf, kbuf, 1, &kc);
            k += kc.size;
        }
        vk_all_barrier(cb);
    }
    /* the other targets over the span: s's own pixels go over them below, in the guest's layout, so a target leaves
       itself out (its tight rows would be a pitch-linear one's padding) */
    s->mem_wgen = s->wgen;
    vk_rt_overlay(s->gva, size, r->buf, 0);
    s->mem_wgen = mem_wgen;
    cb = vk_cmd();
    vk_all_barrier(cb);
    FlushPart whole = flush_whole(s);
    if (!s->bl)
        flush_copy(cb, s, r->buf, 0, s->pitch, &whole);
    else if (!flush_record(cb, s, r->buf, 0, &whole)) {
        vk_own_res_unref(r);
        return 0;
    }
    vk_all_barrier(cb);
    for (u64 i = 0, k = 0; i < kept; i++) { /* the CPU's runs over the image (cpu_newer) */
        VkBufferCopy kc = {kat + k, newer.run[i][0], newer.run[i][1] - newer.run[i][0]};
        rec_CmdCopyBuffer(cb, kbuf, r->buf, 1, &kc);
        k += kc.size;
    }
    if (kept) vk_all_barrier(cb);
    /* The buffer now holds this image, and the targets under it: publishing it must not flush them again */
    s->mem_wgen = s->wgen;
    rt_retire_over(s->gva, size, s, r);
    publish_own(s, r, size);
    s->pub_serial = __atomic_load_n(&r->serial, __ATOMIC_ACQUIRE);
    vk_own_res_unref(r);
    /* guest memory (its owner) has the image's pixels now: the texture cache finds them current, by their owner */
    flush_rebase(s, size, kept != 0, gw0);
    vk_surf_relist(s);
    if (!s->is_rt) vk_tex_note_writeback(s);
    return 1;
}
/* A draw's storage image kept on the GPU until the CPU touches its bytes, where the write-back read all of them on the
   CPU before the draw, and copied the image back after it, to merge the two with what the CPU wrote between. Here
   the span is GPU-owned from the bind on: by the owner a release published there before, or by one made now from
   memory, which holds the image as the texture check before the bind left it. That owner is the span's bytes from
   before the draw, and the host's trap tells whether the CPU touched them: at the release, a span it still owns whole
   gets the image published over it, exactly what the merge would give, and one the CPU touched gets the merge, with
   the owner's bytes read back as the ones from before the draw. It takes the host's traps, a span of whole pages that
   is one run of guest memory (a page shared with other bytes would trap at each of their writes), and a span the CPU
   hasn't touched lately: one it reads or writes every frame would trap and wait for the GPU each time, which the
   write-back's copies never do */
#define KEEP_CPU_PRESENTS 1200 /* as long as compute.c's storage buffers stay off the GPU once the CPU read them */
/* the one owner of all of [cpu, cpu + size), the span at *off of it, or NULL (vk_lock) */
static VkOwnRes* span_owner(u64 cpu, u64 size, u64* off) {
    OwnRange r[2];
    if (own_lookup(cpu, size, r, 2) != 1 || r[0].addr != cpu || r[0].size != size) return NULL;
    *off = r[0].res_off;
    return (VkOwnRes*)r[0].res;
}
int vk_surf_keep_begin(Surf* s, VkImageKeep* k) {
    u64 size = rt_bytes(s), cpu = 0;
    VkOwnRes* r;
    *k = (VkImageKeep){0};
    if (!own_traps() || !size || (s->gva >> 63) || ((s->gva | size) & 0xFFF) || !surf_publishable(s) ||
        gmmu_run(s->gva, size, &cpu) != size || vk_own_cpu_read(cpu, size, KEEP_CPU_PRESENTS))
        return 0;
    if ((r = span_owner(cpu, size, &k->off)))
        vk_own_res_ref(r);
    else { /* memory's bytes, with the targets pending over them, as a release's publish stages them */
        u32 mem_wgen = s->mem_wgen;
        if (!(r = vk_own_res_new(size, VK_OWN_BY_TARGET))) return 0;
        if (!vk_guest_read_device(s->gva, size, r->buf, 0)) {
            vk_own_res_unref(r);
            return 0;
        }
        s->mem_wgen = s->wgen; /* a target goes over nothing of its own */
        if (vk_rt_overlay(s->gva, size, r->buf, 0)) vk_all_barrier(vk_cmd());
        s->mem_wgen = mem_wgen;
        rt_retire_over(s->gva, size, NULL, NULL);
        vk_own_publish(cpu, size, r, 0);
        if (span_owner(cpu, size, &k->off) != r) { /* landed instead (unified memory over it) */
            vk_own_res_unref(r);
            return 0;
        }
        k->made = 1;
        if (!s->is_rt) vk_tex_note_writeback(s); /* its image is memory's bytes, by their owner */
        s->gw = gpu_mem_generation();
    }
    k->held = r;
    mem_baseline(s); /* a write-back's merge (cpu_newer) counts the CPU's writes from the bind on */
    return 1;
}
/* held's bytes from off on, size of them, read back (a wait for the GPU), malloc'd: NULL when they can't be */
static u8* keep_original(VkOwnRes* held, u64 off, u64 size) {
    VkBuffer buf;
    const u8* p = size <= g_vk.alloc_cap ? vk_readback_buffer(size, &buf) : NULL;
    u8* original = p ? (u8*)malloc(size) : NULL;
    if (!original) {
        LOG_LIMIT(4,
                  "vulkan: a storage image's %llu bytes from before its draw found no room to be read back: the CPU's "
                  "pages written since keep their bytes over the image's there\n",
                  (unsigned long long)size);
        return NULL;
    }
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_all_barrier(cb);
    VkBufferCopy c = {off, 0, size};
    rec_CmdCopyBuffer(cb, held->buf, buf, 1, &c);
    flush_wait(cb);
    memcpy(original, p, size);
    return original;
}
int vk_surf_keep_end(Surf* s, const VkImageKeep* k, u8** original) {
    u64 size = rt_bytes(s), cpu = 0, off = 0;
    int whole = gmmu_run(s->gva, size, &cpu) == size;
    *original = NULL;
    if (whole && span_owner(cpu, size, &off) == k->held && off == k->off && vk_surf_publish(s)) {
        vk_own_res_unref(k->held);
        return 1;
    }
    if (whole) vk_own_cpu_touched(cpu, size); /* its next draws take the write-back */
    *original = keep_original(k->held, k->off, size);
    vk_own_res_unref(k->held);
    return 0;
}

u64 g_vk_rt_readbacks; /* render targets written back to guest memory because the GPU's copy couldn't take them */
int vk_rt_overlay(u64 gva, u64 size, VkBuffer buf, u64 at) {
    Surf *local[16], **list;
    int n = 0;
    u32 count, i;
    if (!size || !pend_hit_va(&g_rt_pages, gva, size)) return 0;
    count = rt_pending_over(gva, size, local, 16, &list);
    for (i = 0; i < count; i++) {
        Surf* s = list[i];
        if (s->gva >= gva && s->gva + rt_bytes(s) <= gva + size) {
            FlushPart whole = flush_whole(s);
            vk_render_end();
            if (flush_record(vk_cmd(), s, buf, at + (s->gva - gva), &whole)) {
                n++;
                continue;
            }
        }
        /* reaching past the range, or its copy refused: written back to guest memory, the copy keeping the bytes
           the caller read. A target past the range is mostly one whose memory a smaller surface took over since, and
           the program's writes there are newer than its pixels */
        g_vk_rt_readbacks++;
        vk_surf_flush(s, NULL);
    }
    if (list != local) free(list);
    return n;
}

int vk_rt_over(u64 gva, u64 size) {
    Surf *local[16], **list;
    u32 count;
    if (!size || !pend_hit_va(&g_rt_pages, gva, size)) return 0;
    count = rt_pending_over(gva, size, local, 16, &list);
    if (list != local) free(list);
    return count != 0;
}

void vk_rt_before_write(u64 gva, u64 size) {
    Surf *local[16], **list;
    u32 count, i;
    if (!size || g_rt_flushing) return;
    vk_lock();
    g_rt_flushing = 1;
    if (pend_hit_va(&g_rt_pages, gva, size)) {
        count = rt_pending_over(gva, size, local, 16, &list);
        for (i = 0; i < count; i++) vk_surf_flush(list[i], NULL);
        if (list != local) free(list);
    }
    g_rt_flushing = 0;
    vk_unlock();
}

/* A producer can replace image memory through another GPU address. Land every
   older image over that backing before publishing the new owner, so a later
   partial image read cannot write its old pixels over the producer's result. */
void vk_rt_before_write_cpu(u64 cpu, u64 size) {
    Surf* local[16];
    RtPendingList pending = {local, 0, 16};
    if (!size || g_rt_flushing) return;
    vk_lock();
    g_rt_flushing = 1;
    gmmu_aliases(cpu, size, rt_pending_alias, &pending);
    for (u32 i = 0; i < pending.count; i++) vk_surf_flush(pending.surfaces[i], NULL);
    if (pending.surfaces != local) free(pending.surfaces);
    g_rt_flushing = 0;
    vk_unlock();
}

static void zt_restore(Surf* s);
/* Another format over the same bytes: one game draws its scene to an sRGB target and the overlay over it to a UNORM
   one at the same address, and presents that. The target taking the address starts from the pixels of the one it
   parks (fresh: newer than memory) when they are newer than its own and both images hold the guest's bytes as they
   are, in the same shape. Memory, which neither target's draws wrote, doesn't have them. 1 when it took them */
static int rt_take_pixels(Surf* to, Surf* from, int fresh) {
    if (!from || from == to || !fresh || from->layout == VK_IMAGE_LAYOUT_UNDEFINED || from->wseq <= to->wseq ||
        from->f.depth || to->f.depth || !from->f.raw || !to->f.raw || from->f.bpp != to->f.bpp || from->w != to->w ||
        from->h != to->h || from->sw != to->sw || from->sh != to->sh || from->bl != to->bl || from->bh != to->bh ||
        from->bd != to->bd || from->pitch != to->pitch || from->layers != to->layers ||
        from->layer_size != to->layer_size || from->volume || to->volume || from->msaa != to->msaa)
        return 0;
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, to->layers},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, to->layers},
                     .extent = {to->sw, to->sh, 1}};
    rec_CmdCopyImage(cb, from->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &c);
    LOG_LIMIT(10, "vulkan: the %ux%u render target at %llx in format %x takes the pixels of the one in format %x\n",
              to->w, to->h, (unsigned long long)to->gva, to->fkey, from->fkey);
    vk_surf_wrote(to);
    if (from->mem_wgen == from->wgen) to->mem_wgen = to->wgen;
    /* drawn as from was: fresh as long as memory isn't written after from's baseline */
    vk_rt_drawn(to);
    to->draw_wseen = from->draw_wseen;
    to->draw_gw = from->draw_gw;
    to->draw_has_hash = from->draw_has_hash;
    to->draw_hash = from->draw_hash;
    return 1;
}
static Surf* rt_create_(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth, u32 pitch, u32 layers, u64 layer_size,
                        int volume, u32 msaa);
/* a render target's lookup or creation as a profiler zone (SNX_TRACY builds) */
static Surf* rt_create(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth, u32 pitch, u32 layers, u64 layer_size,
                       int volume, u32 msaa) {
    SNX_ZONE(z, "render target");
    Surf* s = rt_create_(gva, w, h, fmt, tile, depth, pitch, layers, layer_size, volume, msaa);
    SNX_ZONE_END(z);
    return s;
}
/* w by h pixels of the sample layout msaa (DS_MSAA_, as a volume never is), which guest memory holds as samples */
static Surf* rt_create_(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth, u32 pitch, u32 layers, u64 layer_size,
                        int volume, u32 msaa) {
    if (!w || !h) return NULL;
    msaa = volume ? DS_MSAA_1X1 : ds_msaa_canonical(msaa);
    u32 fkey = fmt | (depth ? 0x10000 : 0);
    u32 gw = w << ds_msaa_log2_x(msaa), gh = h << ds_msaa_log2_y(msaa);
    /* the layout is part of what a target is: block linear with its block height, or pitch linear with its pitch */
    u32 bl = !((tile >> 12) & 1), bh = bl ? (tile >> 4) & 15 : 0, bd = bl && volume ? (tile >> 8) & 15 : 0;
    while (bd && layers <= (1u << (bd - 1))) bd--;
    if (bl)
        pitch = 0;
    else if (!pitch)
        pitch = gw * (depth ? vk_zt_format(fmt) : vk_rt_format(fmt)).bpp;
    VkFmt layout_fmt = depth ? vk_zt_format(fmt) : vk_rt_format(fmt);
    u64 min_layer_size = bl ? bl_size(gw * layout_fmt.bpp, gh, bh) : (u64)pitch * gh;
    if (layer_size < min_layer_size) layer_size = min_layer_size;
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == gva && s->is_rt && s->w == w && s->h == h && s->fkey == fkey && s->bl == bl && s->bh == bh &&
            s->pitch == pitch && s->layers == layers && s->layer_size == layer_size && s->bd == bd &&
            s->volume == volume && s->msaa == msaa) {
            vk_rt_note_read(s);
            return s;
        }
    /* another size, format or layout at this address: park the current one, reuse a parked one that fits */
    Surf* parked = NULL;
    /* the one parked here, whose freshness is asked while it holds the address (not when only the size changes, as
       dynamic resolution does every frame in some games) */
    Surf* cur = depth ? NULL : vk_rt_find(gva);
    int cur_fresh = cur && cur->w == w && cur->h == h && cur->layers == layers && !volume && vk_rt_drawn_fresh(cur);
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == (gva | 1ull << 63) && s->is_rt && s->w == w && s->h == h && s->fkey == fkey && s->bl == bl &&
            s->bh == bh && s->pitch == pitch && s->layers == layers && s->layer_size == layer_size && s->bd == bd &&
            s->volume == volume && s->msaa == msaa && !parked)
            parked = s;
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == gva && s->is_rt) {
            rt_pages(s, -1);
            rtx_del(gva);
            s->gva = gva | 1ull << 63;
            g_take_gen++;
            g_parked++;
            surf_changed(gva);
        }
    if (parked) {
        parked->gva = gva;
        g_take_gen++;
        rtx_add(parked);
        g_parked--;
        surf_changed(gva);
        rt_pages(parked, 1);
        vk_rt_note_read(parked);
        rt_take_pixels(parked, cur, cur_fresh);
        return parked;
    }
    VkFmt f = depth ? vk_zt_format(fmt) : vk_rt_format(fmt);
    Surf* s = surf_create(gva, w, h, f, fkey, bl, bh, pitch, 1, layers, msaa);
    if (!s) return NULL;
    rtx_add(s);
    s->layer_size = layer_size;
    s->bd = bd;
    s->volume = volume;
    s->rt_size = rt_bytes(s);
    rt_pages(s, 1);
    s->gw = gpu_mem_generation();
    /* the CPU may have written the memory already (a cleared or decoded frame): start from it. A depth target never
       comes from memory, but a loaded save state may hold its pixels */
    if (depth)
        zt_restore(s);
    else if (!rt_take_pixels(s, cur, cur_fresh))
        vk_rt_reload(gva);
    return s;
}

Surf* vk_rt(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth, u32 pitch) {
    return rt_create(gva, w, h, fmt, tile, depth, pitch, 1, 0, 0, DS_MSAA_1X1);
}

Surf* vk_rt_array(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, u32 pitch, u32 layers, u64 layer_size, int volume,
                  u32 msaa) {
    return rt_create(gva, w, h, fmt, tile, 0, pitch, layers, layer_size, volume, msaa);
}

Surf* vk_zt_array(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, u32 layers, u64 layer_size, u32 msaa) {
    return rt_create(gva, w, h, fmt, tile, 1, (w << ds_msaa_log2_x(msaa)) * vk_zt_format(fmt).bpp, layers, layer_size,
                     0, msaa);
}

void vk_rt_note_read(Surf* s) {
    vk_surf_touch(s);
    if (!s->is_rt) return;
    /* writes the 3D engine's draws don't see: I2M uploads, constant updates and compute results landing there go
       to memory, which DMA and 2D copies would load (vk_rt_reload) but these don't */
    if (s->gw == gpu_mem_generation()) return;
    if (!gpu_mem_dirty_since(s->gw, s->gva, rt_bytes(s))) {
        s->gw = gpu_mem_generation(); /* still unwritten as of now: the next check asks only about later writes */
        return;
    }
    vk_rt_reload(s->gva);
    if (s->gw != gpu_mem_generation())
        LOG_LIMIT(1, "vulkan: could not reload render target at %llx after its guest memory changed\n",
                  (unsigned long long)s->gva);
}

/* the GPU addresses showing a guest address (gmmu_aliases), up to RT_CPU_MAX: one more says there were more */
#define RT_CPU_MAX 32
typedef struct {
    u64 gva[RT_CPU_MAX];
    u32 n;
} RtCpuAt;
static void rt_cpu_at(void* ctx, u64 gva, u64 n) {
    RtCpuAt* q = ctx;
    (void)n;
    if (q->n < RT_CPU_MAX)
        q->gva[q->n++] = gva;
    else
        q->n = RT_CPU_MAX + 1;
}
/* whether s, a target showing the frame, is a better one than best: of the frame's size, else any; the most recently
   used of those, and of two used in the same present the one first used later, as the recency lists order them */
static int rt_cpu_better(const Surf* s, int exact, const Surf* best, int best_exact) {
    if (!best || exact != best_exact) return !best || exact > best_exact;
    i32 d = (i32)((u32)s->last_used - (u32)best->last_used);
    return d > 0 || (d == 0 && s->touched > best->touched);
}
Surf* vk_rt_for_cpu(u64 cpu_addr, u32 w, u32 h) {
    Surf* best = NULL;
    int best_exact = 0;
    RtCpuAt q;
    /* Each present asked this of every render target, translating its address: 3,600 of them in one game, 1.5% of
       its GPU thread. The GPU addresses showing the frame, from the guest side, find the live targets there instead */
    q.n = 0;
    gmmu_aliases(cpu_addr, 1, rt_cpu_at, &q);
    if (q.n <= RT_CPU_MAX) {
        for (u32 i = 0; i < q.n; i++) {
            Surf* s = vk_rt_find(q.gva[i]);
            /* a multisampled one isn't a picture: its resolve is */
            if (!s || s->f.depth || s->msaa || s->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
                gmmu_translate(s->gva) != cpu_addr)
                continue;
            int exact = s->w == w && s->h == h;
            if (rt_cpu_better(s, exact, best, best_exact)) {
                best = s;
                best_exact = exact;
            }
        }
        if (best) vk_rt_note_read(best);
        return best && best->layout != VK_IMAGE_LAYOUT_UNDEFINED ? best : NULL;
    }
    for (int list = VK_LRU_RT + LRU_AGED; list >= VK_LRU_RT; list -= LRU_AGED)
        for (Surf* s = g_lru[list].head; s; s = s->lru_next) {
            if (s->f.depth || s->msaa || (s->gva >> 63) || s->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
                gmmu_translate(s->gva) != cpu_addr)
                continue;
            int exact = s->w == w && s->h == h;
            /* the lists run to the most recently used: of two used in the same present, the later is the one first
               used later */
            if (!best || exact > best_exact ||
                (exact == best_exact && (i32)((u32)s->last_used - (u32)best->last_used) >= 0)) {
                best = s;
                best_exact = exact;
            }
        }
    if (best) vk_rt_note_read(best);
    return best && best->layout != VK_IMAGE_LAYOUT_UNDEFINED ? best : NULL;
}

/* render targets drawn since they were made, for a texture made of them (gpu/vk/texture.c), in order of the address
   they were drawn at (drawn_from). Every texture bind looks up a range in it: a game keeping 6,500 targets made a walk
   of the whole list three quarters of the GPU thread, where a binary search finds the range's first */
static Surf** g_drawn;
static int g_ndrawn, g_capdrawn;
/* the first drawn target at or past address a */
static int drawn_lower(u64 a) {
    int lo = 0, hi = g_ndrawn;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (g_drawn[mid]->drawn_from < a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
/* the GPU pages they started at when listed, so a texture over none of them (most) skips the list: one game looked at
   each of about 30 for each of 13,000 textures a frame. A target parked since still counts its old page, which only
   lets the walk run */
static PendPages g_drawn_pages;
/* Read backing memory directly: asking gmem_read to land pending output here could submit a draw mid-setup. A run of
   pages contiguous in host memory is hashed at once, eight bytes at a time (vk_hash) */
static u64 rt_mem_hash(const Surf* s) {
    u64 h = 1469598103934665603ull;
    u64 size = rt_bytes(s);
    for (u64 o = 0; o < size;) {
        u64 a = s->gva + o, cpu = gmmu_translate(a), n = 0x1000 - (a & 0xFFF);
        if (n > size - o) n = size - o;
        while (cpu && o + n < size && gmmu_translate(a + n) == cpu + n) /* the next page follows in host memory */
            n += size - o - n < 0x1000 ? size - o - n : 0x1000;
        /* an unmapped page counts as its length, pages the GPU holds as their owners (they are trapped) */
        if (!cpu || !own_hash(cpu, n, &h)) h = snx_fnv_step(h, cpu ? vk_hash(GP(cpu), n) : n);
        o += n;
    }
    return h;
}
/* Pitch-linear color targets drawn since the last release. A target's pixels reach guest memory only when something
   asks for them, and the CPU can't ask. Reading back on the CPU what the GPU drew is what pitch-linear targets are
   mostly for, and they are few: one game draws its ambient light's spherical harmonics into seven 8x8 RGBA32F targets
   and reads them after a fence, and lit its scenes with the old bytes there, values up to 1e31. A release publishes
   them, GPU-owned (vk_surf_publish): the GPU copies them over their guest bytes, and they land only if the CPU
   touches those.

   Block linear color targets drawn since the last release are the second list. Most are never read on the CPU, and
   to publish each one at each release would copy tens of megabytes a frame. But a game also reads one now and then:
   one game draws its save's thumbnail into a 320x180 target, and reads its texels from memory with no call to the GPU.
   A release watches their bytes (own_watch): the pages are trapped with no copy of the GPU's, and the CPU's first
   access publishes the targets pending there (vk_rt_watch_hit) and lands them. A target that the CPU read lately
   (vk_own_cpu_read) is published at the release, as a pitch-linear one. Where an owner already covers it, the
   owner's trap is its watch.

   Depth targets go in the same lists, in their formats' layouts (flush_bytes): a program that clears one with the 3D
   engine, waits for the GPU and reads it from memory read the bytes from before the clear */
typedef struct {
    Surf** s;
    u32 n, cap;
} DrawnList;
static DrawnList g_linear, g_bl;
VkRtWatchStats g_vk_rt_watch;
static void drawn_push(DrawnList* l, Surf* s, int* in) {
    if (*in) return;
    if (l->n == l->cap) {
        u32 cap = l->cap ? l->cap * 2 : 16;
        Surf** n = realloc(l->s, (size_t)cap * sizeof *n);
        if (!n) return;
        l->s = n;
        l->cap = cap;
    }
    l->s[l->n++] = s;
    *in = 1;
}
static void drawn_pull(DrawnList* l, const Surf* s, int* in) {
    u32 i = 0;
    if (!*in) return;
    while (i < l->n && l->s[i] != s) i++;
    if (i < l->n) l->s[i] = l->s[--l->n];
    *in = 0;
}
static void linear_note(Surf* s) {
    if (s->bl)
        drawn_push(&g_bl, s, &s->in_bl);
    else
        drawn_push(&g_linear, s, &s->in_linear);
}
static void linear_drop(Surf* s) {
    drawn_pull(&g_linear, s, &s->in_linear);
    drawn_pull(&g_bl, s, &s->in_bl);
}
static void rt_publish_now(Surf* s) {
    if (!vk_surf_publish(s)) vk_surf_flush(s, NULL);
}
/* a block linear target s drawn since the last release, its pixels newer than memory: watched, or published */
static void rt_watch(Surf* s) {
    u64 size = rt_bytes(s);
    int publish = 0, armed = 0;
    if (!own_traps()) return; /* no traps: left as before, as a publish would land at once, a readback a release */
    /* Read lately: published now, so that the CPU's next read finds it with no fault. That is a CPU access that had
       this target published (cpu_asked), not any read near it: by the 64 KiB area (vk_own_cpu_read), a 2560x1440
       target beside a 64x1 one that a game reads each frame was published at each release with it. Not for an owner
       over it either (one a publish made before, or another engine's), whose bytes are older than its pixels: the
       owner's pages are trapped as a watch's are, and the CPU's access to them publishes the targets pending there
       before the owner lands (OwnHooks' refresh, which is vk_rt_watch_hit too). Published at each release while its
       owner stayed, one game's targets were copied 24,000 to 43,000 times in 600 presents, its 2560x1440 depth
       targets among them, and each wait for the GPU took 63 ms where it takes 4 */
    publish = s->cpu_asked && g_vk_present + 1 - s->cpu_asked <= KEEP_CPU_PRESENTS;
    if (publish) {
        if (g_vk_rt_watch.publishes++ < 4)
            LOG("vulkan: the %ux%u render target at %llx (%llu bytes) published at a release: the CPU read it lately\n",
                s->w, s->h, (unsigned long long)s->gva, (unsigned long long)size);
        rt_publish_now(s);
        return;
    }
    for (u64 off = 0; off < size;) {
        u64 cpu = 0, n = gmmu_run(s->gva + off, size - off, &cpu);
        if (!n) {
            n = 0x1000 - ((s->gva + off) & 0xFFF);
            if (n > size - off) n = size - off;
        } else if (!own_watched(cpu, n)) {
            /* memory an import holds (unified memory): the publish copies in place, with no trap */
            if (vk_own_watch(cpu, n) && vk_uma_held(cpu, n)) {
                rt_publish_now(s);
                return;
            }
            armed = 1;
        }
        off += n;
    }
    if (armed) g_vk_rt_watch.arms++;
}
void vk_rt_publish_drawn(void) {
    /* publishing can submit and land, which leaves the lists alone: each is taken whole first */
    while (g_linear.n) {
        Surf* s = g_linear.s[--g_linear.n];
        s->in_linear = 0;
        if (!(s->gva >> 63) && flush_bytes(s)) rt_publish_now(s);
    }
    while (g_bl.n) {
        Surf* s = g_bl.s[--g_bl.n];
        s->in_bl = 0;
        if (!(s->gva >> 63) && flush_bytes(s)) rt_watch(s); /* pending: drawn since memory last had it */
    }
}
void vk_rt_watch_hit(u64 cpu, u64 size) {
    Surf* local[16];
    RtPendingList pending = {local, 0, 16};
    vk_lock();
    gmmu_aliases(cpu, size, rt_pending_alias, &pending);
    /* each published over its whole span: its watch goes with the owner that replaces it. One that can't be is
       written back, and its watch goes as the write-back's own_acquire finds it */
    for (u32 i = 0; i < pending.count; i++) {
        Surf* s = pending.surfaces[i];
        if (g_vk_rt_watch.hits++ < 4)
            LOG("vulkan: the CPU touched guest memory %llx, a page of the %ux%u render target at %llx (%llu bytes), "
                "watched: published\n",
                (unsigned long long)cpu, s->w, s->h, (unsigned long long)s->gva, (unsigned long long)rt_bytes(s));
        s->cpu_asked = g_vk_present + 1;
        rt_publish_now(s);
    }
    if (pending.surfaces != local) free(pending.surfaces);
    vk_unlock();
}
static u64 g_drawn_max; /* the biggest span a listed target has: how far before an address one over it can start */
/* Whether every byte of s's span was drawn over after s last changed, by targets drawn later, each over its own span:
   memory holds their pixels now (or would on the console), so s's are no one's to read and it goes without loss. A
   game rendering at a dynamic resolution makes its scene's targets again at another size each frame over the same
   memory, and each old one held a draw memory never got: kept as precious, a few thousand took the budget from the
   targets that are (one game's ink maps, drawn once at a level's start, went under that pressure with the budget
   lowered, and its walls took no ink). The drawn list is in order of address, so the targets over s are a sweep from its start */
static int rt_superseded(const Surf* s) {
    u64 lo = s->gva & ~(1ull << 63), hi = lo + s->rt_size, reach = lo;
    if (!s->is_rt || !s->rt_size) return 0;
    for (int i = drawn_lower(lo > g_drawn_max ? lo - g_drawn_max : 0); i < g_ndrawn && g_drawn[i]->drawn_from <= reach;
         i++) {
        const Surf* o = g_drawn[i];
        if (o == s || o->draw_wseq <= s->wseq || o->drawn_from + o->rt_size <= reach) continue;
        reach = o->drawn_from + o->rt_size;
        if (reach >= hi) return 1;
    }
    return 0;
}
void vk_rt_drawn(Surf* s) {
    take_changed(s);
    linear_note(s);
    s->draw_wseq = s->wseq;
    if (s->rt_size > g_drawn_max) g_drawn_max = s->rt_size;
    /* A memory write after the first draw can leave untouched pixels stale even when later draws touch the target.
       Keep that first baseline until the next present instead of scanning the whole target on every draw. */
    if (!s->draw_wseen || s->drawn_at != g_vk_present) {
        u64 size = rt_bytes(s);
        /* without write tracking, a hash of the whole target: only for one a texture takes pixels from (feeds_tex),
           where it is the one answer vk_rt_drawn_fresh has. Hashing every target on its first draw of each present
           took most of the GPU thread (FreeBSD, Linux before 6.7) */
        /* Dirty metadata needs the mapping's address, without acquiring pixels
           that can still be GPU-owned. The hash fallback acquires its own data. */
        s->draw_has_hash = vk_written_since(gmem_host(s->gva, size), size, 0) < 0 && s->feeds_tex;
        if (s->draw_has_hash) s->draw_hash = rt_mem_hash(s);
        if (!(s->gva >> 63) && size) span_runs(s->gva, size, run_cool, NULL);
        s->draw_wseen = vk_ww_now();
        s->draw_gw = gpu_mem_generation();
    }
    s->drawn_at = g_vk_present;
    if (s->in_drawn) return;
    if (g_ndrawn == g_capdrawn) {
        int cap = g_capdrawn ? g_capdrawn * 2 : 64;
        Surf** n = realloc(g_drawn, (size_t)cap * sizeof *n);
        if (!n) return;
        g_drawn = n;
        g_capdrawn = cap;
    }
    s->drawn_from = s->gva;
    int at = drawn_lower(s->drawn_from);
    memmove(&g_drawn[at + 1], &g_drawn[at], (size_t)(g_ndrawn - at) * sizeof *g_drawn);
    g_drawn[at] = s;
    g_ndrawn++;
    s->in_drawn = 1;
    g_take_gen++;
    pend_add_va(&g_drawn_pages, s->drawn_from, 1, 1);
}
void vk_rt_drawn_full(Surf* s) {
    s->draw_wseen = 0; /* every pixel was replaced, so earlier memory writes no longer matter */
    vk_rt_drawn(s);
    mem_baseline(s);
}
int vk_rt_drawn_fresh(Surf* s) {
    u64 size = rt_bytes(s);
    if (!s->draw_wseen || s->layout == VK_IMAGE_LAYOUT_UNDEFINED || gpu_mem_dirty_since(s->draw_gw, s->gva, size))
        return 0;
    if (s->draw_has_hash) return rt_mem_hash(s) == s->draw_hash;
    int written = vk_written_since(gmem_host(s->gva, size), size, s->draw_wseen);
    if (written < 0) {
        /* no write tracking, and no baseline yet: the target's pixels are taken this once, as the GPU's writes to its
           memory are known and the game's own seldom land in a target it draws to. Its next baselines are hashed */
        s->feeds_tex = 1;
        return 1;
    }
    return written == 0;
}
int vk_rt_drawn_in(u64 lo, u64 hi, Surf** out, int max) {
    int n = 0;
    if (hi <= lo || !pend_hit_va(&g_drawn_pages, lo, hi - lo)) return 0;
    for (int i = drawn_lower(lo); i < g_ndrawn && g_drawn[i]->drawn_from < hi; i++) {
        Surf* s = g_drawn[i];
        /* Generated textures may first be sampled several frames after their draw.
           Age does not make guest memory authoritative: tex_take_drawn checks
           vk_rt_drawn_fresh before copying, and surf_free removes evicted targets. A parked target's address (its
           drawn_from with the top bit set) is out of every range */
        if (s->gva == s->drawn_from) {
            if (n < max) out[n] = s;
            n++;
            s->take_seen = 1;
        }
    }
    return n;
}

/* a surface's image and views go when the frame being recorded is done (descriptors in it may name them), the BC7
   image being made for it too */
static void surf_free(Surf* s) {
    vk_tex_later_forget(s);
    surf_changed(s->gva);
    vk_astc_forget(s, ~0u); /* the background encodes of its levels stop writing into it */
    if (s->lru) {
        g_held[lru_kind(s)].n--;
        g_held[lru_kind(s)].bytes -= s->bytes;
        lru_unlink(s);
    }
    if (s->is_rt) rt_pages(s, -1);
    if (s->gva >> 63) g_parked--;
    linear_drop(s);
    if (s->in_drawn) {
        g_take_gen++;
        int i = drawn_lower(s->drawn_from);
        while (i < g_ndrawn && g_drawn[i] != s) i++;
        if (i < g_ndrawn) {
            memmove(&g_drawn[i], &g_drawn[i + 1], (size_t)(g_ndrawn - i - 1) * sizeof *g_drawn);
            g_ndrawn--;
        }
        pend_add_va(&g_drawn_pages, s->drawn_from, 1, -1);
    }
    for (int i = 0; i < s->nsv; i++) vk_defer_image(VK_NULL_HANDLE, 0, s->sv[i].v, VK_NULL_HANDLE);
    if (s->present) vk_defer_image(VK_NULL_HANDLE, 0, s->present, VK_NULL_HANDLE);
    vk_defer_image(s->img, s->mem, s->att, s->att_linear);
    free(s);
}
/* a target holding a draw that memory doesn't have: it stays while its address is its own and memory wasn't written
   over it since, as a texture made there later takes its pixels (tex_take_drawn) and freeing it would lose them. Depth
   draws aren't tracked: a depth target holds one once anything left its layout defined */
static int keeps_drawn(Surf* s) {
    if (!s->is_rt || (s->gva >> 63)) return 0;
    return s->f.depth ? s->layout != VK_IMAGE_LAYOUT_UNDEFINED : vk_rt_drawn_fresh(s);
}
/* a surface whose pixels can be had again once it goes. A texture: loaded from memory, unless it took pixels from
   render targets since. A render target: without defined contents, or with memory holding what it holds (written back
   or loaded since its last draw, in a format that moves between the two; rt_create loads it again), or with every
   byte drawn over by later targets (rt_superseded). A parked one too only then: taken out of parking it gives back
   its own pixels, which memory, written by neither target's draws, doesn't have */
static int lossless(const Surf* s) {
    if (!s->is_rt) return !s->took_drawn;
    if (s->layout == VK_IMAGE_LAYOUT_UNDEFINED) return 1;
    return (!s->f.depth && s->f.raw && s->mem_wgen == s->wgen && moves_scaled(s)) || rt_superseded(s);
}
/* s out of the table and freed, with its copies (gpu/vk/texture.c rt_crop): the device memory that releases */
static u64 surf_evict(Surf* s) {
    if (s->lru && lru_kind(s) == VK_LRU_TEX) vk_tex_evicted(s); /* statistics: one made again later is counted */
    if (s->is_rt && !(s->gva >> 63)) rtx_del(s->gva);
    for (Surf** pp = &g_surfh[hkey(s->gva)]; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    u64 bytes = s->bytes;
    for (Surf *c = s->crop, *n; c; c = n) {
        n = c->next;
        bytes += c->bytes;
        surf_free(c);
    }
    surf_free(s);
    vk_defer_bytes(bytes);
    return bytes;
}
static u64 surf_reclaim(int kind, u32 before, u64 want, int lossy) {
    /* A render target goes with its contents: guest memory has them as of its last write-back. They aren't written to
       memory as they go: memory is shared by targets the GPU drew over one another, parked and freed ones among them,
       and by textures and buffers that copied from them on the GPU, and the renderer doesn't know them all. Writing an
       evicted target's pixels, as a game's Mii icons wanted (drawn at boot, shown a minute later), put older pixels
       over newer ones in another: a black block over the player character, and red and green blocks over its
       textures. So a target memory doesn't hold goes only under the budget's pressure once nothing lossless is left
       (vk_mem_trim), or after minutes unused (vk_tex_frame), and then not one holding a draw a texture made at its
       address later would take (keeps_drawn), which only the budget's pressure takes.

       From the least recently used on, up to the first used since `before`: with lossy, the aged list first, as it
       holds the oldest of those. A storage image's result still to land holds its surface: those are passed over,
       a few hundred at most a call */
    u64 freed = 0;
    u32 passed = 0;
    if (lossy)
        for (Surf *s = g_lru[kind + LRU_AGED].head, *n; s && freed < want && passed < 512; s = n) {
            n = s->lru_next;
            if ((i32)((u32)s->last_used - before) >= 0) break;
            if (lossy == VK_RECLAIM_IDLE && keeps_drawn(s)) continue;
            if (vk_gfx_image_holds(s))
                passed++;
            else
                freed += surf_evict(s);
        }
    for (Surf *s = g_lru[kind].head, *n; s && freed < want && passed < 512; s = n) {
        n = s->lru_next;
        if ((i32)((u32)s->last_used - before) >= 0) break;
        if (!lossy && !lossless(s)) { /* out of the way of the next lossless eviction, held or not, in order */
            lru_unlink(s);
            lru_link(s, kind + LRU_AGED);
        } else if (lossy == VK_RECLAIM_IDLE && keeps_drawn(s))
            continue;
        else if (vk_gfx_image_holds(s))
            passed++;
        else
            freed += surf_evict(s);
    }
    return freed;
}
u64 vk_surf_reclaim(int kind, u32 before, u64 want, int lossy) {
    VkResourceScope scope = vk_resource_begin();
    u64 freed = surf_reclaim(kind, before, want, lossy);
    vk_resource_end(VK_RESOURCE_SURFACE_RECLAIM, scope);
    return freed;
}
/* the render targets last used before `before` that later ones drew all over (rt_superseded), from either list: the
   aged one holds those that went there with pixels memory lacked, which draws since may have made lossless */
static u64 rt_reclaim_superseded(u32 before, u64 want) {
    u64 freed = 0;
    for (int lru = VK_LRU_RT + LRU_AGED; lru >= VK_LRU_RT && freed < want; lru -= LRU_AGED)
        for (Surf *s = g_lru[lru].head, *n; s && freed < want; s = n) {
            n = s->lru_next;
            if ((i32)((u32)s->last_used - before) >= 0) break;
            if (s->layout != VK_IMAGE_LAYOUT_UNDEFINED && rt_superseded(s) && !vk_gfx_image_holds(s))
                freed += surf_evict(s);
        }
    return freed;
}
u64 vk_rt_reclaim_superseded(u32 before, u64 want) {
    VkResourceScope scope = vk_resource_begin();
    u64 freed = rt_reclaim_superseded(before, want);
    vk_resource_end(VK_RESOURCE_SURFACE_RECLAIM, scope);
    return freed;
}

/* ------------------------------------------------------------ the budget */
/* A trim that found the images short of memory, where ASTC textures are held as RGBA8 (astc_mode=1, Exact, or Auto with
   the budget for it at start) on a GPU that samples BCn but not ASTC: the log suggests astc_mode=2 (BC7) once a run,
   with the numbers (snx_astc_bc_suggest).
   Short: an image allocation found no memory (force), the caches left past their budget once all that could go went,
   or the working set evicted (what the last eight presents used, the trim's second step), which the next frames load
   again: a game held slow by eviction */
static void astc_bc_suggest(int force, u64 extra, u64 usage, u64 budget, u64 freed, u64 working) {
    u64 left = usage > freed ? usage - freed : 0, astc = vk_astc_rgba8_bytes();
    int why = force           ? SNX_ASTC_SHORT_NO_MEMORY
              : left > budget ? SNX_ASTC_SHORT_PAST_BUDGET
              : working       ? SNX_ASTC_SHORT_WORKING_SET
                              : SNX_ASTC_SHORT_NONE;
    char said[128];
    if (!snx_astc_bc_suggest(vk_astc_bc7(), g_vk.astc_bc7_could, 0, astc, why)) return;
    if (why == SNX_ASTC_SHORT_NO_MEMORY)
        snprintf(said, sizeof said, "an image of %llu MB found no memory", (unsigned long long)(extra >> 20));
    else if (why == SNX_ASTC_SHORT_PAST_BUDGET)
        snprintf(said, sizeof said, "the caches stay %llu MB past their budget after a trim",
                 (unsigned long long)((left - budget) >> 20));
    else
        snprintf(said, sizeof said, "a trim took %llu MB of textures and targets the last eight presents used",
                 (unsigned long long)(working >> 20));
    LOG_LIMIT(
        1,
        "vulkan: %s, %llu of %llu MB budgeted in use: astc_mode=2 (BC7, Settings > Graphics) would hold the %llu MB "
        "of ASTC textures decoded to RGBA8 in a quarter, encoded again as BC7 between frames\n",
        said, (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20), (unsigned long long)(astc >> 20));
}
static u64 g_trimmed;   /* statistics: bytes freed for the budget */
static u32 g_trim_dry;  /* the present a trim last found too little to free (+1: 0 is none) */
static u32 g_forced;    /* the present a failed allocation last forced a trim (+1) */
static u32 g_oom;       /* the present an image allocation last found no memory (+1) */
static u64 g_oom_bytes; /* the smallest size that found none in that present */
u64 vk_mem_trimmed(void) { return g_trimmed; }
void vk_mem_trim(u64 extra, int force) {
    u64 usage, budget, need = 0;
    if (force) {
        /* once a present: what it frees is released when the frame is done, so a second one this present would
           find the same shortfall and take more */
        if (g_forced == g_vk_present + 1) return;
        g_forced = g_vk_present + 1;
    } else if (g_trim_dry == g_vk_present + 1)
        return; /* nothing more to free until the next present */
    vk_mem_use(&usage, &budget);
    usage += extra;
    if (force) { /* the allocation's size, less what evictions before it are releasing already */
        u64 gone = vk_deferred_bytes();
        need = extra > gone ? extra - gone : 0;
    } else if (usage <= budget / 100 * 90)
        return;
    SNX_ZONE_V(z, "vk mem trim", force);
    /* What goes first is what can be had again without loss: textures (loaded again from memory), own buffers (copied
       again), then render targets memory holds. Those unused for a second go down to 80% of the budget, room for what
       comes next. The working set itself (unused for eight presents) goes only to stay under 95%: freeing it to 80%
       when it alone is bigger made one game load hundreds of textures and targets again every 600 presents under a
       lowered budget, where staying near the budget costs nothing. Two presents took textures a game uses every few
       frames: of 3,000 it evicted in one game's menus, 2,358 had gone unused for three presents, and each came back
       at once, a BC7 encode each (astc.c). Render targets whose pixels exist only here
       (memory has older ones: draws aren't written back, see vk_surf_reclaim) and textures that took such pixels lose
       them when they go, so they go only when the lossless steps leave the heap past 90%: those unused for five
       seconds, then, past 95%, those unused for a second and eight presents, the youngest a lossy entry can be. Every
       step frees the least recently used first. Before them all go the render targets later ones drew all over, unused
       for two presents: what they hold is no one's (rt_superseded) */
    u64 superseded = vk_rt_reclaim_superseded(vk_present_before(0, 2), ~0ull);
    static const struct {
        u32 ms, presents;
        int lossy;
        u32 mark; /* percent of the budget the step frees down to */
    } steps[] = {{1000, 8, 0, 80}, {0, 8, 0, 95}, {5000, 30, 1, 90}, {1000, 8, 1, 95}};
    u64 freed = superseded, by[3] = {0, 0, superseded}, working = 0;
    int short_of = 0;
    for (u32 i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        u64 mark = budget / 100 * steps[i].mark, left = usage > freed ? usage - freed : 0;
        u64 want = left > mark ? left - mark : 0;
        if (freed < need && want < need - freed) want = need - freed;
        if (!want) continue;
        u32 before = vk_present_before((u64)steps[i].ms * 1000000ull, steps[i].presents);
        int level = steps[i].lossy ? VK_RECLAIM_ANY : VK_RECLAIM_LOSSLESS;
        u64 got = vk_surf_reclaim(VK_LRU_TEX, before, want, level);
        by[0] += got;
        if (got < want && !steps[i].lossy) {
            u64 n = vk_buf_reclaim(before, want - got);
            by[1] += n;
            got += n;
        }
        if (got < want) {
            u64 n = vk_surf_reclaim(VK_LRU_RT, before, want - got, level);
            by[2] += n;
            got += n;
        }
        freed += got;
        if (i == 1) working = got;
        short_of = got < want;
    }
    g_trimmed += freed;
    astc_bc_suggest(force, extra, usage, budget, freed, working);
    /* short of memory, as astc_bc_suggest counts it: BC1 held as ASTC 4x4 goes back to ETC2, half the bytes */
    if (force)
        vk_bc1_tight("an image found no memory");
    else if ((usage > freed ? usage - freed : 0) > budget)
        vk_bc1_tight("the caches stay past their budget after a trim");
    else if (working)
        vk_bc1_tight("a trim took textures and targets the last eight presents used");
    if (short_of) g_trim_dry = g_vk_present + 1;
    if (freed || short_of)
        LOG_LIMIT(20,
                  "vulkan: %llu of %llu MB budgeted in use%s: freed %llu MB of textures, %llu of buffers, %llu of "
                  "render targets%s\n",
                  (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20),
                  force ? " (an allocation found no memory)" : "", (unsigned long long)(by[0] >> 20),
                  (unsigned long long)(by[1] >> 20), (unsigned long long)(by[2] >> 20),
                  short_of ? ", all that could go" : "");
}

void vk_surf_bytes(Surf* s, u64 bytes) {
    if (s->lru) g_held[lru_kind(s)].bytes = g_held[lru_kind(s)].bytes - s->bytes + bytes;
    s->bytes = bytes;
}

int vk_cache_image_create(const VkImageCreateInfo* ii, VkImage* img, DevMem* mem, u32 owner, u64* bytes) {
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                         .pCreateInfo = ii};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    VkResult res;
    SNX_ZONE(z, "vk image create");
    VkResourceScope query = vk_resource_begin();
    vkGetDeviceImageMemoryRequirements(g_vk.dev, &q, &req);
    vk_resource_end(VK_RESOURCE_IMAGE_QUERY, query);
    *bytes = req.memoryRequirements.size;
    SNX_ZONE_VALUE(z, *bytes);
    *img = VK_NULL_HANDLE;
    *mem = 0;
    /* more than the whole heap: never made, and the caches aren't emptied for it (a header's garbage size) */
    const VkPhysicalDeviceMemoryProperties* mp;
    mp = vk_devmem_props();
    u64 heap = mp->memoryHeaps[vk_mem_heap()].size;
    if (*bytes > heap) {
        LOG_LIMIT(4, "vulkan: a %ux%ux%u image of %u layers takes %llu MB, more than the GPU's %llu MB (not made)\n",
                  ii->extent.width, ii->extent.height, ii->extent.depth, ii->arrayLayers,
                  (unsigned long long)(*bytes >> 20), (unsigned long long)(heap >> 20));
        return 0;
    }
    /* one as big found no memory this present: neither will this one (what was freed meanwhile is released when the
       frame is done). Its draws go without it until the next present */
    if (g_oom == g_vk_present + 1 && *bytes >= g_oom_bytes) return 0;
    vk_mem_trim(*bytes, 0);
    if (vk_image_create_r(ii, img, mem, owner, &res)) return 1;
    if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        if (g_oom != g_vk_present + 1 || *bytes < g_oom_bytes) g_oom_bytes = *bytes;
        g_oom = g_vk_present + 1;
        vk_mem_trim(*bytes, 1); /* room for it by the time it is asked for again, a present or two later */
    }
    return 0;
}

/* A nearest depth/stencil transform when the format has no image blit support. Copy each aspect into device rows,
   gather through one column/row table, then copy the transformed aspect back. This retains D32FS8 values and stencil
   bits without depending on color reinterpretation, host readback or one operation per pixel. */
static int blit_depth_gather(Surf* source, Surf* destination, i32 x0, i32 y0, i32 x1, i32 y1, i32 dx, i32 dy, u32 width,
                             u32 height) {
    u64 table_size = 4ull * (width + height), cap;
    VkBuffer table_buffer;
    u64 table_offset;
    u32* tables = (u32*)(void*)vk_stage(table_size, vk_stage_align(), &table_buffer, &table_offset);
    if (!tables) return 0;
    for (u32 i = 0; i < width; i++) {
        double x = x0 + ((double)i + 0.5) * ((double)x1 - x0) / width;
        tables[i] = x <= 0 ? 0 : x >= source->sw ? source->sw - 1 : (u32)x;
    }
    for (u32 i = 0; i < height; i++) {
        double y = y0 + ((double)i + 0.5) * ((double)y1 - y0) / height;
        tables[width + i] = y <= 0 ? 0 : y >= source->sh ? source->sh - 1 : (u32)y;
    }
    snx_le32_array(tables, (size_t)width + height);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (u32 aspect = 0; aspect < (source->f.stencil ? 2u : 1u); aspect++) {
        u32 bytes = aspect ? 1 : source->f.vk == VK_FORMAT_D16_UNORM ? 2 : 4;
        u32 input_pitch = (source->sw * bytes + 3) & ~3u, output_pitch = (width * bytes + 3) & ~3u;
        u64 table_at = ((u64)input_pitch * source->sh + 15) & ~15ull;
        u64 output_at = (table_at + table_size + 15) & ~15ull;
        u64 used = output_at + (u64)output_pitch * height;
        VkBuffer scratch = vk_bl_scratch(used, &cap);
        if (!scratch) return 0;
        VkImageAspectFlags bit = aspect ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        VkBufferImageCopy read = {.bufferRowLength = input_pitch / bytes,
                                  .imageSubresource = {bit, 0, 0, 1},
                                  .imageExtent = {source->sw, source->sh, 1}};
        rec_CmdCopyImageToBuffer(cb, source->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scratch, 1, &read);
        VkBufferCopy copy = {table_offset, table_at, table_size};
        rec_CmdCopyBuffer(cb, table_buffer, scratch, 1, &copy);
        vk_all_barrier(cb);
        if (!vk_tc_blit(cb, bytes, 0, width * bytes, height, scratch, used, 0, input_pitch, table_at,
                        table_at + 4ull * width, scratch, output_at, output_pitch))
            return 0;
        vk_all_barrier(cb);
        VkBufferImageCopy write = {.bufferOffset = output_at,
                                   .bufferRowLength = output_pitch / bytes,
                                   .imageSubresource = {bit, 0, 0, 1},
                                   .imageOffset = {dx, dy, 0},
                                   .imageExtent = {width, height, 1}};
        rec_CmdCopyBufferToImage(cb, scratch, destination->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &write);
        vk_all_barrier(cb);
    }
    return 1;
}

/* A 2D blit whose source is a depth target: the game copies the depth buffer's texels as a color surface of their
   size (Pokemon Legends Z-A blits its D24S8 depth as BGRA8 into another depth buffer, which its lighting tests
   against, and halves it for half-size passes). The texels are copied whole on the console, so here the depth and
   stencil go into a depth target of the source's format at the destination, which the draw that binds it finds.
   Depth targets don't reach guest memory, so a copy through memory would leave it undefined: black lighting. 0 when
   the surfaces don't describe that (another texel size, or a source of another size than the target) */
static int blit_depth(Surf* st, const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy,
                      double sdx, double sdy) {
    u32 fmt = st->fkey & 0xFFFF, tile = (d->linear ? 0x1000u : 0) | (d->bh << 4);
    Surf* dt;
    /* one to one it is a copy, which every depth format takes: RADV blits no D32_SFLOAT_S8_UINT */
    int same = sdx == 1.0 && sdy == 1.0;
    if (vk_rt_format(s->format).bpp != vk_zt_format(fmt).bpp || s->format != d->format || s->height < st->h ||
        !vk_rt_same_layout(st, s->width, st->h, !s->linear, s->bh, s->pitch))
        return 0;
    vk_surf_touch(st); /* used now: making the destination may evict what wasn't */
    /* NVN copy descriptors keep logical widths. Block-linear attachment widths cover whole GOB rows, exactly as
       nvn_draw_surface describes the same texture on its later bind. Keep its extra columns out of the copy. */
    u32 target_width = d->linear ? d->width : ((d->width * st->f.bpp + 63) / 64 * 64) / st->f.bpp;
    dt = vk_zt_array(d->addr, target_width, d->height, fmt, tile, 1, 0, DS_MSAA_1X1);
    if (!dt || dt == st || dt->f.vk != st->f.vk) return 0;
    float S = g_vk_scale;
    i32 x0 = (i32)(sx * S), y0 = (i32)(sy * S), x1 = (i32)((sx + sdx * dw) * S + 0.5),
        y1 = (i32)((sy + sdy * dh) * S + 0.5);
    i32 X1 = (i32)((dx + dw) * S + 0.5f), Y1 = (i32)((dy + dh) * S + 0.5f);
    dx = (i32)(dx * S);
    dy = (i32)(dy * S);
    if (x1 > (i32)st->sw) x1 = (i32)st->sw;
    if (y1 > (i32)st->sh) y1 = (i32)st->sh;
    if (X1 > (i32)dt->sw) X1 = (i32)dt->sw;
    if (Y1 > (i32)dt->sh) Y1 = (i32)dt->sh;
    if (x1 == x0 || y1 == y0 || X1 <= dx || Y1 <= dy || x0 < 0 || y0 < 0 || x1 < 0 || y1 < 0 || x0 > (i32)st->sw ||
        y0 > (i32)st->sh || dx < 0 || dy < 0)
        return 0;
    vk_rt_note_read(st);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, st, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, dt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (same) { /* the region both sides hold */
        u32 w = (u32)(x1 - x0 < X1 - dx ? x1 - x0 : X1 - dx), h = (u32)(y1 - y0 < Y1 - dy ? y1 - y0 : Y1 - dy);
        VkImageCopy c = {
            .srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT | (st->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), 0, 0, 1},
            .srcOffset = {x0, y0, 0},
            .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT | (st->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), 0, 0, 1},
            .dstOffset = {dx, dy, 0},
            .extent = {w, h, 1}};
        rec_CmdCopyImage(cb, st->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dt->img,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        X1 = dx + (i32)w;
        Y1 = dy + (i32)h;
    } else if (!blit_ok(st->f.vk, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        if (!blit_depth_gather(st, dt, x0, y0, x1, y1, dx, dy, (u32)(X1 - dx), (u32)(Y1 - dy))) return 0;
    } else {
        /* one aspect a region; nearest, as filtering packed depth and stencil bits as color would only mix them */
        VkImageBlit b[2] = {{.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                             .srcOffsets = {{x0, y0, 0}, {x1, y1, 1}},
                             .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                             .dstOffsets = {{dx, dy, 0}, {X1, Y1, 1}}}};
        b[1] = b[0];
        b[1].srcSubresource.aspectMask = b[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        rec_CmdBlitImage(cb, st->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dt->img,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, st->f.stencil ? 2 : 1, b, VK_FILTER_NEAREST);
    }
    vk_surf_wrote(dt);
    if (dx == 0 && dy == 0 && X1 == (i32)dt->sw && Y1 == (i32)dt->sh)
        vk_rt_drawn_full(dt);
    else
        vk_rt_drawn(dt);
    vk_surf_touch(dt);
    return 1;
}

/* A multisampled depth target's resolve (NVN's Downsample of a depth texture): its depth and stencil at sample 0, the
   resolve every Vulkan device has for depth (SAMPLE_ZERO: AVERAGE isn't one for depth), made by a dynamic rendering
   pass with no draws over the rectangle into an image of the source's pixels, which a blit then copies into the
   destination, a depth target of the source's format. sx, sy, sdx and sdy are in the source's pixels. 0 when the
   destination's 2D format has other bytes a pixel than the source's depth format: then nothing takes the resolve */
static int resolve_depth(Surf* st, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                         double sdy) {
    u32 fmt = st->fkey & 0xFFFF, tile = (d->linear ? 0x1000u : 0) | (d->bh << 4);
    /* the 2D engine's color format of the depth format's bytes, as blit_depth takes it */
    if (vk_rt_format(d->format).bpp != vk_zt_format(fmt).bpp ||
        !blit_ok(st->f.vk, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return 0;
    vk_surf_touch(st); /* used now: making the destination may evict what wasn't */
    Surf* dt = vk_zt_array(d->addr, d->width, d->height, fmt, tile, 1, 0, DS_MSAA_1X1);
    if (!dt || dt == st || dt->f.vk != st->f.vk) return 0;
    float S = g_vk_scale;
    i32 x0 = (i32)(sx * S), y0 = (i32)(sy * S), x1 = (i32)((sx + sdx * dw) * S + 0.5),
        y1 = (i32)((sy + sdy * dh) * S + 0.5);
    i32 X1 = (i32)((dx + dw) * S + 0.5f), Y1 = (i32)((dy + dh) * S + 0.5f);
    dx = (i32)(dx * S);
    dy = (i32)(dy * S);
    if (x1 > (i32)st->sw) x1 = (i32)st->sw;
    if (y1 > (i32)st->sh) y1 = (i32)st->sh;
    if (X1 > (i32)dt->sw) X1 = (i32)dt->sw;
    if (Y1 > (i32)dt->sh) Y1 = (i32)dt->sh;
    if (x1 <= x0 || y1 <= y0 || X1 <= dx || Y1 <= dy || x0 < 0 || y0 < 0 || dx < 0 || dy < 0) return 0;
    vk_rt_note_read(st);
    if (st->layout == VK_IMAGE_LAYOUT_UNDEFINED) return 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (st->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    DevMem tm;
    VkImage tmp;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = st->f.vk,
                            .extent = {st->sw, st->sh, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (!vk_image_create(&ii, &tmp, &tm)) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = st->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = st->f.vk,
                                .subresourceRange = {aspect, 0, 1, 0, 1}};
    VkImageView sv = VK_NULL_HANDLE, tv = VK_NULL_HANDLE;
    int ok = VK_OK(vk_image_view_create(g_vk.dev, &vi, NULL, &sv));
    vi.image = tmp;
    ok = ok && VK_OK(vk_image_view_create(g_vk.dev, &vi, NULL, &tv));
    vk_defer_image(tmp, tm, sv, tv); /* once the frame's commands that use them are done */
    if (!ok) return 0;
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    /* the resolve reads the samples and writes the image in the color attachment output stage, as every resolve does;
       the pass loads the samples (depth and stencil tests' accesses) and stores them unchanged */
    const VkPipelineStageFlags2 rs = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    vk_surf_use(cb, st, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, rs,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    vk_barrier(cb, tmp, aspect, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, rs,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                     .imageView = sv,
                                     .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                     .resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
                                     .resolveImageView = tv,
                                     .resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}},
                          .layerCount = 1,
                          .pDepthAttachment = &att,
                          .pStencilAttachment = st->f.stencil ? &att : NULL};
    rec_CmdBeginRendering(cb, &ri);
    rec_CmdEndRendering(cb);
    vk_barrier(cb, tmp, aspect, rs,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_surf_use(cb, dt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    /* one aspect a region; nearest, as filtering depth and stencil would only mix them */
    VkImageBlit b[2] = {{.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                         .srcOffsets = {{x0, y0, 0}, {x1, y1, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                         .dstOffsets = {{dx, dy, 0}, {X1, Y1, 1}}}};
    b[1] = b[0];
    b[1].srcSubresource.aspectMask = b[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    rec_CmdBlitImage(cb, tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     st->f.stencil ? 2 : 1, b, VK_FILTER_NEAREST);
    vk_surf_wrote(dt);
    if (dx == 0 && dy == 0 && X1 == (i32)dt->sw && Y1 == (i32)dt->sh)
        vk_rt_drawn_full(dt);
    else
        vk_rt_drawn(dt);
    vk_surf_touch(dt);
    return 1;
}

/* the 2D engine's blit between render targets, on the GPU. The surfaces are gpu/gpu.h's
   Surf2D. 0 when it can't be done there: the front end then copies through guest memory.
   A multisampled source is the resolve (NVN's Downsample, a game's blit of its multisampled target): the 2D engine
   takes its samples as texels of the guest's layout of them (gpu/vk/msaa.c), so the blit's source rectangle covers
   whole blocks of a pixel's samples, and its pixels are the samples' resolve, vkCmdResolveImage's: their average, or
   one sample's value for an integer format, as Eden resolves such a blit. A resolve of the destination's format and
   size goes straight into it; else into an image of the source's format, which the blit then reads */
static int blit2d(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                  double sdy, int filter) {
    Surf* st = vk_rt_find(s->addr);
    /* Explicit NVN copies can sample ordinary guest memory as well as a prior render target. Load it through the
       surface cache, whose GPU upload performs the layout and format conversion once, and reuse it thereafter. */
    if (!st && filter >= 0) {
        u32 tile = (s->linear ? 0x1000u : 0) | (s->bh << 4);
        st = vk_rt(s->addr, s->width, s->height, s->format, tile, 0, s->pitch);
    }
    if (st && st->msaa) { /* in the source's pixels */
        double bx = (double)(1u << ds_msaa_log2_x(st->msaa)), by = (double)(1u << ds_msaa_log2_y(st->msaa));
        sx /= bx;
        sdx /= bx;
        sy /= by;
        sdy /= by;
        if (st->f.depth) {
            if (st->layout == VK_IMAGE_LAYOUT_UNDEFINED || dw <= 0 || dh <= 0) return 0;
            if (resolve_depth(st, d, dx, dy, dw, dh, sx, sy, sdx, sdy)) return 1;
            LOG_LIMIT(1, "vulkan: a multisampled depth target's resolve into format %#x isn't done\n", d->format);
            return 0;
        }
    }
    if (st && st->f.depth && st->layout != VK_IMAGE_LAYOUT_UNDEFINED && dw > 0 && dh > 0)
        return blit_depth(st, s, d, dx, dy, dw, dh, sx, sy, sdx, sdy);
    if (!st || st->f.depth || st->layout == VK_IMAGE_LAYOUT_UNDEFINED || dw <= 0 || dh <= 0) return 0;
    vk_surf_touch(st); /* used now: making the destination may evict what wasn't */
    u32 tile = (d->linear ? 0x1000u : 0) | (d->bh << 4);
    Surf* dt = vk_rt(d->addr, d->width, d->height, d->format, tile, 0, d->pitch);
    if (!dt || dt == st || dt->f.depth || st->f.is_int != dt->f.is_int) return 0;
    if (!blit_ok(st->f.vk, VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !blit_ok(dt->f.vk, VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return 0;
    if (filter > 0 && !st->f.is_int && !blit_ok(st->f.vk, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) return 0;
    /* in the images' pixels: targets are scaled */
    float S = g_vk_scale;
    i32 x0 = (i32)(sx * S), y0 = (i32)(sy * S), x1 = (i32)((sx + sdx * dw) * S + 0.5),
        y1 = (i32)((sy + sdy * dh) * S + 0.5);
    i32 X1 = (i32)((dx + dw) * S + 0.5f), Y1 = (i32)((dy + dh) * S + 0.5f);
    dx = (i32)(dx * S);
    dy = (i32)(dy * S);
    if (x1 > (i32)st->sw) x1 = (i32)st->sw;
    if (y1 > (i32)st->sh) y1 = (i32)st->sh;
    if (X1 > (i32)dt->sw) X1 = (i32)dt->sw;
    if (Y1 > (i32)dt->sh) Y1 = (i32)dt->sh;
    /* The explicit NVN path permits reversed source endpoints. Maxwell retains its earlier clipping contract. */
    if (x1 == x0 || y1 == y0 || (filter < 0 && (x1 < x0 || y1 < y0)) || X1 <= dx || Y1 <= dy || x0 < 0 || y0 < 0 ||
        x1 < 0 || y1 < 0 || x0 > (i32)st->sw || y0 > (i32)st->sh || dx < 0 || dy < 0)
        return 0;
    int scaled = abs(x1 - x0) != X1 - dx || abs(y1 - y0) != Y1 - dy;
    u32 source_w = vk_sc(s->width >> ds_msaa_log2_x(st->msaa));
    u32 source_h = vk_sc(s->height >> ds_msaa_log2_y(st->msaa));
    int crop = filter > 0 && (source_w < st->sw || source_h < st->sh);
    if (source_w > st->sw) source_w = st->sw;
    if (source_h > st->sh) source_h = st->sh;
    vk_rt_note_read(st);
    if (st->layout == VK_IMAGE_LAYOUT_UNDEFINED) return 0;
    VkCommandBuffer cb = vk_cmd();
    VkPipelineStageFlags2 reads = st->msaa ? VK_PIPELINE_STAGE_2_RESOLVE_BIT
                                  : crop   ? VK_PIPELINE_STAGE_2_COPY_BIT
                                           : VK_PIPELINE_STAGE_2_BLIT_BIT;
    vk_surf_use(cb, st, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, reads, VK_ACCESS_2_TRANSFER_READ_BIT);
    if (st->msaa && !scaled && x1 > x0 && y1 > y0 && dt->f.vk == st->f.vk) {
        vk_surf_use(cb, dt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_RESOLVE_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageResolve r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .srcOffset = {x0, y0, 0},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .dstOffset = {dx, dy, 0},
                            .extent = {(u32)(x1 - x0), (u32)(y1 - y0), 1}};
        rec_CmdResolveImage(cb, st->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dt->img,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    } else {
        VkImage src = st->img;
        if (st->msaa || crop) { /* Resolve samples, or isolate logical texels from render-target row padding. */
            DevMem tm;
            VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = st->f.vk,
                                    .extent = {crop ? source_w : st->sw, crop ? source_h : st->sh, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
            if (!vk_image_create(&ii, &src, &tm)) return 0;
            vk_defer_image(src, tm, VK_NULL_HANDLE, VK_NULL_HANDLE);
            vk_barrier(cb, src, VK_IMAGE_ASPECT_COLOR_BIT, reads, 0, VK_IMAGE_LAYOUT_UNDEFINED, reads,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            if (st->msaa) {
                i32 left = x0 < x1 ? x0 : x1, top = y0 < y1 ? y0 : y1;
                VkImageResolve r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .srcOffset = {left, top, 0},
                                    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .dstOffset = {left, top, 0},
                                    .extent = {(u32)abs(x1 - x0), (u32)abs(y1 - y0), 1}};
                rec_CmdResolveImage(cb, st->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            } else {
                VkImageCopy r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .extent = {source_w, source_h, 1}};
                rec_CmdCopyImage(cb, st->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            }
            vk_barrier(cb, src, VK_IMAGE_ASPECT_COLOR_BIT, reads, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        vk_surf_use(cb, dt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .srcOffsets = {{x0, y0, 0}, {x1, y1, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .dstOffsets = {{dx, dy, 0}, {X1, Y1, 1}}};
        int linear = (filter < 0 ? scaled : filter != 0) && !st->f.is_int &&
                     blit_ok(st->f.vk, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        rec_CmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    vk_surf_wrote(dt);
    if (dx == 0 && dy == 0 && X1 == (i32)dt->sw && Y1 == (i32)dt->sh)
        vk_rt_drawn_full(dt);
    else
        vk_rt_drawn(dt);
    vk_surf_touch(dt);
    return 1;
}

int vk_blit2d(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
              double sdy) {
    return blit2d(s, d, dx, dy, dw, dh, sx, sy, sdx, sdy, -1);
}
int vk_blit2d_filter(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                     double sdy, int linear) {
    return blit2d(s, d, dx, dy, dw, dh, sx, sy, sdx, sdy, linear != 0);
}

/* a save state's load: every texture and render target goes, as guest memory under them is the state's now */
void vk_surf_forget_all(void) {
    u32 k;
    for (k = 0; k < SURFH; k++)
        while (g_surfh[k]) surf_evict(g_surfh[k]);
}

/* a save state's save: every render target whose pixels memory doesn't hold yet writes them back, so the state's guest
   memory has them (one a format the CPU can't take, a depth target or a scaled one, can't: it has none to write) */
static int flush_wanted(const Surf* s) {
    return s->is_rt && !(s->gva >> 63) && s->layout != VK_IMAGE_LAYOUT_UNDEFINED && s->mem_wgen != s->wgen &&
           flush_bytes(s);
}
u32 vk_rt_flush_all(void) {
    u32 n = 0, i;
    int list;
    Surf** all;
    /* gathered first: a write-back moves its target in the lists */
    for (list = VK_LRU_RT; list <= VK_LRU_RT + LRU_AGED; list += LRU_AGED)
        for (Surf* s = g_lru[list].head; s; s = s->lru_next) n += flush_wanted(s);
    if (!n || !(all = (Surf**)malloc(sizeof *all * n))) return 0;
    n = 0;
    for (list = VK_LRU_RT; list <= VK_LRU_RT + LRU_AGED; list += LRU_AGED)
        for (Surf* s = g_lru[list].head; s; s = s->lru_next)
            if (flush_wanted(s)) all[n++] = s;
    for (i = 0; i < n; i++) vk_surf_flush(all[i], NULL);
    free(all);
    return n;
}

/* ---- a save state's depth targets. Their pixels never go to guest memory and never come from it (rt_create), so a
   state holds them itself: each one live at the save, its depth and its stencil read back once (the one readback a
   save makes), and put into the target of the same place, format and size the loaded game makes next ---- */
typedef struct {
    u64 gva;
    u32 w, h, sw, sh, fkey, vk, layers;
    u64 dsize, ssize; /* the depth aspect's bytes and the stencil's (0 for none), all layers */
    u8* bytes;        /* the depth's, then the stencil's */
} ZtSaved;
static ZtSaved* g_zt;
static u32 g_nzt;
/* the bytes a texel of each aspect takes in a buffer copy, and whether it has stencil */
static u32 zt_depth_bytes(VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D16_UNORM_S8_UINT ? 2 : 4; }
static int zt_stencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}
/* a multisampled one's samples aren't saved: copies don't reach them */
static int zt_saved(const Surf* s) {
    return s->is_rt && s->f.depth && !(s->gva >> 63) && s->layout != VK_IMAGE_LAYOUT_UNDEFINED && !s->volume &&
           s->type != VK_IMAGE_VIEW_TYPE_3D && !s->msaa;
}
static void zt_drop(void) {
    u32 i;
    for (i = 0; i < g_nzt; i++) free(g_zt[i].bytes);
    free(g_zt);
    g_zt = NULL;
    g_nzt = 0;
}
/* each aspect of every layer of s copied between its image and buf at at, the depth's then the stencil's: to the
   buffer (out) or from it */
static void zt_copy(VkCommandBuffer cb, Surf* s, VkBuffer buf, u64 at, int out) {
    u64 dsize = (u64)s->sw * s->sh * zt_depth_bytes(s->f.vk);
    u32 layer, a, aspects = zt_stencil(s->f.vk) ? 2 : 1;
    for (a = 0; a < aspects; a++)
        for (layer = 0; layer < s->layers; layer++) {
            VkBufferImageCopy c = {
                .bufferOffset = at,
                .imageSubresource = {a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 1},
                .imageExtent = {s->sw, s->sh, 1}};
            if (out)
                rec_CmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
            else
                rec_CmdCopyBufferToImage(cb, buf, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            at += a ? (u64)s->sw * s->sh : dsize;
        }
}
int vk_zt_save(StateIo* io) {
    u64 total = 0, at = 0;
    u32 n = 0, i;
    int list, r = 0;
    Surf** all;
    VkBuffer buf;
    u8* p;
    for (list = VK_LRU_RT; list <= VK_LRU_RT + LRU_AGED; list += LRU_AGED)
        for (Surf* s = g_lru[list].head; s; s = s->lru_next)
            if (zt_saved(s)) {
                n++;
                total += (u64)s->sw * s->sh * s->layers * (zt_depth_bytes(s->f.vk) + (zt_stencil(s->f.vk) ? 1u : 0u));
            }
    if (!n) return io_put32(io, 0);
    if (!(all = (Surf**)malloc(sizeof *all * n))) return -1;
    n = 0;
    for (list = VK_LRU_RT; list <= VK_LRU_RT + LRU_AGED; list += LRU_AGED)
        for (Surf* s = g_lru[list].head; s; s = s->lru_next)
            if (zt_saved(s)) all[n++] = s;
    if (!(p = vk_readback_buffer(total, &buf))) {
        free(all);
        return -1;
    }
    vk_render_end(); /* the copies can't be recorded inside a rendering pass */
    {
        VkCommandBuffer cb = vk_cmd();
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                               .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        for (i = 0; i < n; i++) {
            Surf* s = all[i];
            vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
            zt_copy(cb, s, buf, at, 1);
            at += (u64)s->sw * s->sh * s->layers * (zt_depth_bytes(s->f.vk) + (zt_stencil(s->f.vk) ? 1u : 0u));
        }
        rec_CmdPipelineBarrier2(cb, &di);
    }
    vk_finish();
    vk_readback_ready();
    r |= io_put32(io, n);
    for (i = 0, at = 0; i < n && !r; i++) {
        Surf* s = all[i];
        u64 dsize = (u64)s->sw * s->sh * s->layers * zt_depth_bytes(s->f.vk),
            ssize = zt_stencil(s->f.vk) ? (u64)s->sw * s->sh * s->layers : 0;
        r |= io_put64(io, s->gva) | io_put32(io, s->w) | io_put32(io, s->h) | io_put32(io, s->sw) | io_put32(io, s->sh);
        r |= io_put32(io, s->fkey) | io_put32(io, (u32)s->f.vk) | io_put32(io, s->layers) | io_put64(io, dsize) |
             io_put64(io, ssize) | io_put(io, p + at, dsize + ssize);
        at += dsize + ssize;
    }
    free(all);
    LOG("vulkan: %u depth targets saved (%llu KB)\n", n, (unsigned long long)(total >> 10));
    return r ? -1 : 0;
}
int vk_zt_load(StateIo* io) {
    u32 n, i;
    zt_drop();
    if (io_get32(io, &n) || n > 4096) return -1;
    if (!n) return 0;
    if (!(g_zt = (ZtSaved*)calloc(n, sizeof *g_zt))) return -1;
    for (i = 0; i < n; i++, g_nzt++) {
        ZtSaved* z = &g_zt[i];
        if (io_get64(io, &z->gva) || io_get32(io, &z->w) || io_get32(io, &z->h) || io_get32(io, &z->sw) ||
            io_get32(io, &z->sh) || io_get32(io, &z->fkey) || io_get32(io, &z->vk) || io_get32(io, &z->layers) ||
            io_get64(io, &z->dsize) || io_get64(io, &z->ssize) || z->dsize + z->ssize > (1ull << 32) ||
            !(z->bytes = (u8*)malloc((size_t)(z->dsize + z->ssize + 1))) || io_get(io, z->bytes, z->dsize + z->ssize)) {
            zt_drop();
            return -1;
        }
    }
    return 0;
}
/* s, a depth target just made: the pixels a loaded state holds for one of its place, format and size go into it, once */
static void zt_restore(Surf* s) {
    u32 i;
    if (s->msaa) return; /* none was saved (zt_saved) */
    for (i = 0; i < g_nzt; i++) {
        ZtSaved* z = &g_zt[i];
        VkCommandBuffer cb;
        VkBuffer buf;
        u64 off;
        u8* to;
        if (z->gva != s->gva || z->w != s->w || z->h != s->h || z->sw != s->sw || z->sh != s->sh ||
            z->fkey != s->fkey || z->vk != (u32)s->f.vk || z->layers != s->layers ||
            z->dsize != (u64)s->sw * s->sh * s->layers * zt_depth_bytes(s->f.vk))
            continue;
        if ((to = vk_stage(z->dsize + z->ssize, 4, &buf, &off)) != NULL) {
            memcpy(to, z->bytes, (size_t)(z->dsize + z->ssize));
            vk_render_end();
            cb = vk_cmd();
            vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
            zt_copy(cb, s, buf, off, 0);
            LOG("vulkan: the depth target at %llx (%ux%u) has its pixels from the save state\n",
                (unsigned long long)s->gva, s->w, s->h);
        }
        free(z->bytes);
        *z = g_zt[--g_nzt];
        return;
    }
}
/* a load's reset: what an earlier state left unmatched goes */
void vk_zt_forget(void) { zt_drop(); }
