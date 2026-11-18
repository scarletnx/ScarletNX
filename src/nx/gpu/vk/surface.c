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
#include "nx/gpu/vk/rt_bands.h"
#include "nx/gpu/vk/rt_cover.h"
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "nx/gpu/gpu.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/guest_textures.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/resource_stats.h"
#include "nx/gpu/vk/tex_report.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/gpu/texture/astc_bc.h"
#include "nx/gpu/texture/blocklinear.h"
#include "nx/memory/own.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/runtime/state_io.h"

/* The table of surfaces, one row each, as columns indexed by row (a struct of arrays). Row 0 is none: its columns stay
   zero, so a surface without a row (a copy, the CPU frame) is in no list. A surface has a row from the time it enters
   the table (row_take) until it is freed (surf_free). The hash chains, the recency lists and the list of ended
   textures link rows by u32 index, so a walk of one reads these dense columns, and a Surf only when it needs that
   surface's own state. A free row's hnext is the next free row.

   The recency lists, from the least recently used to the most: textures (VK_LRU_TEX) and render targets (VK_LRU_RT,
   parked ones too), each with a second list (LRU_AGED + its own) of the old ones whose pixels would be lost if they
   went (lossless below), moved there as an eviction that takes only lossless ones passes them. Each stays in the
   order of last use: what is moved comes from the old end of the first list, older than all left there. A surface
   used again goes back to the recent end of the first. Both render target lists together are every real target, for
   overlapping guest-memory writes (vk_rt_before_write).

   The list of ended textures, newest first: those whose last texture object the game ended (ended_collect), or, in a
   game with no texture objects, whose first page it unmapped (remapped_collect). Nothing draws with one again unless
   the game makes an object at its address (maps it again), and its pixels are in memory still or they are not
   (lossless): a trim takes these before any texture the game still has, whatever their age. Linked both ways:
   with memory to spare nothing trims it, and one game's list held 22,000 textures. The idle eviction (vk_tex_frame)
   takes the oldest, at its far end, and a walk to each from the head cost 100 to 190 ms in a present that evicted a
   few thousand */
#define SURFH    4096
#define LRU_AGED 2
#define NLISTS   5
typedef struct {
    u32 prev, next;
} RowLink;
typedef struct {
    u32 head, tail;
} RowList;
static struct {
    /* the columns, in one block that rows_grow carves */
    Surf** surf;    /* the surface of the row */
    u64* touched;   /* when it last went to the recent end of its recency list: ties of last_used in the lists' order */
    RowLink* lru;   /* its links in its recency list */
    RowLink* ended; /* its links in the list of ended textures */
    u32* hnext;     /* the next row of its hash chain, or of the free rows */
    u32* ended_at;  /* the present the cache learned that its texture object ended */
    u8* list;       /* its recency list: VK_LRU_TEX, VK_LRU_RT, either + LRU_AGED, or VK_LRU_NONE */
    /* 0, or why it is in the list of ended textures: 1, the game ended its last texture object and made none there
       since; 2, in a game with no texture objects, its first page was unmapped and nothing was mapped there since */
    u8* ended_by;
    u32 cap;        /* rows in the columns */
    u32 top;        /* the highest row ever taken */
    u32 free;       /* the first free row below top */
    u32 hash[SURFH];
    RowList lists[NLISTS];
    RowList ended_list;
    u64 clock; /* the last touched value given */
} g_rows;
/* the columns with room for twice the rows, all in one block */
static void rows_grow(void) {
    u32 cap = g_rows.cap ? g_rows.cap * 2 : 1024;
    const size_t row_bytes = sizeof(Surf*) + sizeof(u64) + 2 * sizeof(RowLink) + 2 * sizeof(u32) + 2 * sizeof(u8);
    if (cap <= g_rows.cap || cap > (size_t)-1 / row_bytes) fatal("vulkan: too many surfaces for the table");
    char* block = (char*)calloc(cap, row_bytes);
    if (!block) fatal("vulkan: out of memory for the table of surfaces");
    /* each column at a multiple of its type's size: the 8-byte ones first, then the 4-byte ones, then the bytes */
    Surf** surf = (Surf**)block;
    u64* touched = (u64*)(surf + cap);
    RowLink* lru = (RowLink*)(touched + cap);
    RowLink* ended = lru + cap;
    u32* hnext = (u32*)(ended + cap);
    u32* ended_at = hnext + cap;
    u8* list = (u8*)(ended_at + cap);
    u8* ended_by = list + cap;
    u32 n = g_rows.cap;
    if (n) {
        memcpy(surf, g_rows.surf, n * sizeof *surf);
        memcpy(touched, g_rows.touched, n * sizeof *touched);
        memcpy(lru, g_rows.lru, n * sizeof *lru);
        memcpy(ended, g_rows.ended, n * sizeof *ended);
        memcpy(hnext, g_rows.hnext, n * sizeof *hnext);
        memcpy(ended_at, g_rows.ended_at, n * sizeof *ended_at);
        memcpy(list, g_rows.list, n);
        memcpy(ended_by, g_rows.ended_by, n);
    }
    free(g_rows.surf); /* the old block: its first column starts it */
    g_rows.surf = surf;
    g_rows.touched = touched;
    g_rows.lru = lru;
    g_rows.ended = ended;
    g_rows.hnext = hnext;
    g_rows.ended_at = ended_at;
    g_rows.list = list;
    g_rows.ended_by = ended_by;
    g_rows.cap = cap;
}
/* r at the end of q, the first of q, or before at (0: at the end) */
static void link_before(RowLink* l, RowList* q, u32 r, u32 at) {
    l[r].next = at;
    l[r].prev = at ? l[at].prev : q->tail;
    if (l[r].prev)
        l[l[r].prev].next = r;
    else
        q->head = r;
    if (at)
        l[at].prev = r;
    else
        q->tail = r;
}
static void link_out(RowLink* l, RowList* q, u32 r) {
    if (l[r].prev)
        l[l[r].prev].next = l[r].next;
    else
        q->head = l[r].next;
    if (l[r].next)
        l[l[r].next].prev = l[r].prev;
    else
        q->tail = l[r].prev;
    l[r] = (RowLink){0};
}
static u32 hkey(u64 a) { return (u32)((a >> 8) ^ (a >> 20)) & (SURFH - 1); }
/* s into the table: a row, at the head of its address's hash chain */
static void row_take(Surf* s) {
    u32 r = g_rows.free;
    if (r)
        g_rows.free = g_rows.hnext[r];
    else {
        if (g_rows.top + 1 >= g_rows.cap) rows_grow();
        r = ++g_rows.top;
    }
    g_rows.surf[r] = s;
    s->row = r;
    u32 k = hkey(s->gva);
    g_rows.hnext[r] = g_rows.hash[k];
    g_rows.hash[k] = r;
}
/* s out of its hash chain: in no lookup, its row still held */
static void row_unhash(const Surf* s) {
    for (u32* at = &g_rows.hash[hkey(s->gva)]; *at; at = &g_rows.hnext[*at])
        if (*at == s->row) {
            *at = g_rows.hnext[s->row];
            break;
        }
}
/* s's row free again, all zero (s is out of every list and chain) */
static void row_drop(Surf* s) {
    u32 r = s->row;
    g_rows.surf[r] = NULL;
    g_rows.touched[r] = 0;
    g_rows.ended_at[r] = 0;
    g_rows.list[r] = g_rows.ended_by[r] = 0;
    g_rows.hnext[r] = g_rows.free;
    g_rows.free = r;
    s->row = 0;
}
static struct {
    u32 n;
    u64 bytes;
} g_held[3];         /* by VK_LRU_TEX or VK_LRU_RT: the surfaces and their memory */
static u32 g_parked; /* render targets parked */
static int lru_kind(u32 r) { return g_rows.list[r] > LRU_AGED ? g_rows.list[r] - LRU_AGED : g_rows.list[r]; }
static void lru_link(u32 r, int list) {
    g_rows.list[r] = (u8)list;
    link_before(g_rows.lru, &g_rows.lists[list], r, 0);
}
static void lru_unlink(u32 r) {
    link_out(g_rows.lru, &g_rows.lists[g_rows.list[r]], r);
    g_rows.list[r] = VK_LRU_NONE;
}
static u64 g_ended_freed; /* bytes of ended textures a trim took, for its log */
static void ended_unlink(u32 r) {
    if (!g_rows.ended_by[r]) return;
    link_out(g_rows.ended, &g_rows.ended_list, r);
    g_rows.ended_by[r] = 0;
}
void vk_surf_touch(Surf* s) {
    u32 r = s->row;
    if (r) ended_unlink(r); /* used: the game has an object there again */
    if (s->last_used == g_vk_present) return;
    s->last_used = g_vk_present;
    if (!r || !g_rows.list[r]) return;
    g_rows.touched[r] = ++g_rows.clock;
    int kind = lru_kind(r);
    lru_unlink(r);
    lru_link(r, kind);
}
static int lossless(const Surf* s);
/* s's pixels can be had again now (written back, loaded, or left undefined) and are taken out of the aged list, else
   a lossless eviction wouldn't find it and the idle one would keep it for five minutes. Into its first list in order
   of last use: near the old end, as everything there was used after everything aged, past only the others relisted */
void vk_surf_relist(Surf* s) {
    u32 r = s->row;
    if (!r || g_rows.list[r] <= LRU_AGED || !lossless(s)) return;
    int kind = lru_kind(r);
    u32 at = g_rows.lists[kind].head; /* the first used after s */
    while (at && (i32)((u32)g_rows.surf[at]->last_used - (u32)s->last_used) <= 0) at = g_rows.lru[at].next;
    lru_unlink(r);
    g_rows.list[r] = (u8)kind;
    g_rows.touched[r] = ++g_rows.clock; /* past the others of its last_used */
    link_before(g_rows.lru, &g_rows.lists[kind], r, at);
}
static void lru_add(Surf* s, int kind) {
    s->last_used = g_vk_present;
    g_rows.touched[s->row] = ++g_rows.clock;
    lru_link(s->row, kind);
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
        for (u32 r = g_rows.lists[list].head; r; r = g_rows.lru[r].next) fn(ctx, g_rows.surf[r]);
}
void vk_surf_textures(void (*fn)(Surf* t, void* ctx), void* ctx) {
    for (int list = VK_LRU_TEX; list <= VK_LRU_TEX + LRU_AGED; list += LRU_AGED)
        for (u32 r = g_rows.lists[list].head; r; r = g_rows.lru[r].next) fn(g_rows.surf[r], ctx);
}
static _Thread_local int g_rt_flushing;
_Atomic u32 g_vk_surf_epoch;
static u32 g_surf_bucket_epoch[SURFH];
u32 vk_surf_epoch_at(u64 gva) { return g_surf_bucket_epoch[hkey(gva)]; }
/* a change of the surface table at gva: of its own bucket (a parked target's high bit leaves it in the same one) */
static void surf_changed(u64 gva) {
    atomic_fetch_add_explicit(&g_vk_surf_epoch, 1, memory_order_relaxed);
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

/* the image barriers that vk_surf_use makes between vk_surf_batch_begin and vk_surf_batch_end, recorded in one call
   when the batch ends: barriers on different images in one call are the same as one call each, in their order. A
   pass's start puts a barrier on each of its targets, and a copy on its two surfaces */
#define SURF_BATCH 16
static _Thread_local struct {
    VkImageMemoryBarrier2 b[SURF_BATCH];
    const Surf* s[SURF_BATCH];
    VkCommandBuffer cb;
    u32 n;
    int on;
} g_surf_batch;
static void surf_batch_flush(void) {
    if (!g_surf_batch.n) return;
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                    .imageMemoryBarrierCount = g_surf_batch.n,
                                    .pImageMemoryBarriers = g_surf_batch.b};
    rec_CmdPipelineBarrier2(g_surf_batch.cb, &dependency);
    g_surf_batch.n = 0;
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
    /* a barrier with sample locations stays on its own: a chained one makes the recorder sync (rec_CmdPipelineBarrier2).
       A second barrier on the same image waits for the first one's call: in one call they would not be ordered */
    if (g_surf_batch.on && !image.pNext) {
        int again = g_surf_batch.n == SURF_BATCH || (g_surf_batch.n && g_surf_batch.cb != cb);
        for (u32 i = 0; i < g_surf_batch.n && !again; i++) again = g_surf_batch.s[i] == s;
        if (again) surf_batch_flush();
        g_surf_batch.cb = cb;
        g_surf_batch.s[g_surf_batch.n] = s;
        g_surf_batch.b[g_surf_batch.n++] = image;
        return;
    }
    surf_batch_flush(); /* the barriers collected before this one go first */
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                    .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &image};
    rec_CmdPipelineBarrier2(cb, &dependency);
}
void vk_surf_batch_begin(void) { g_surf_batch.on = 1; }
void vk_surf_batch_end(void) {
    surf_batch_flush();
    g_surf_batch.on = 0;
}
void vk_surf_use(VkCommandBuffer cb, Surf* s, VkImageLayout layout, VkPipelineStageFlags2 stage,
                 VkAccessFlags2 access) {
    int writes = (access & (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) != 0;
    /* With unified image layouts each use of the image is in GENERAL. A read then makes the last write visible to
       every later read, in each stage. Later reads of another kind (a sample after a copy read, a copy after a
       sample) then take no barrier, and they do not end the open rendering pass */
    if (g_vk.has_unified_layouts && layout == VK_IMAGE_LAYOUT_GENERAL && !writes) {
        stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        access = VK_ACCESS_2_MEMORY_READ_BIT;
    }
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
/* s's image (a render target, or a surface of the caches): its create info (vf has to live until it is made), the
   usage, and the format its storage view takes. It sets what s keeps of them (storage_capable, ms_raw, type, the
   sample locations). 0 when the GPU makes no such image. surf_create_ makes it, and vk_rt_move makes it again */
static int surf_image_info(Surf* s, VkImageCreateInfo* ii, VkViewFormats* vf, VkImageUsageFlags* usage_out,
                           VkFormat* storage_out) {
    const VkFmt f = s->f;
    const int is_rt = s->is_rt;
    const u32 msaa = s->msaa, layers = s->layers;
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
        return 0;
    }
    /* depth moves only as the formats that copy out as they are (depth_copies_raw) */
    s->ms_raw = msaa && (raw || f.vk == VK_FORMAT_D32_SFLOAT || f.vk == VK_FORMAT_D16_UNORM);
    s->type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    *ii = (VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
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
    s->sample_locations_compatible = (ii->flags & VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT) != 0;
    if (s->sample_locations_compatible) s->sample_locations = vk_sample_locations_standard(vk_surf_samples(s));
    vk_view_formats(ii, vf, raw);
    *usage_out = usage;
    *storage_out = storage_format;
    return 1;
}
/* s's attachment view of its image (a usage without storage, where its storage view takes another format) */
static void surf_att_create(Surf* s, VkImageUsageFlags usage, VkFormat storage_format) {
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = s->img,
                                .viewType = s->type,
                                .format = s->f.vk,
                                .subresourceRange = {vk_aspect(&s->f), 0, 1, 0, s->layers}};
    VkImageViewUsageCreateInfo view_usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                            .usage = usage & ~VK_IMAGE_USAGE_STORAGE_BIT};
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && storage_format != s->f.vk) vi.pNext = &view_usage;
    VK_OK(vk_image_view_create(g_vk.dev, &vi, NULL, &s->att));
}
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
    VkImageCreateInfo ii;
    VkViewFormats vf; /* sRGB and linear views, and the raw one */
    VkImageUsageFlags usage;
    VkFormat storage_format;
    if (!surf_image_info(s, &ii, &vf, &usage, &storage_format)) {
        free(s);
        return NULL;
    }
    /* a scaled target can pass the allocation cap (4x of 4K is 506 MiB in RGBA8). Room is made for it under the
       budget: the caller holds only surfaces it used this present, which stay */
    if (!vk_cache_image_create(&ii, &s->img, &s->mem, VK_OWN_TARGET | (is_rt ? VK_OWN_MOVES : 0), &s->bytes)) {
        free(s);
        return NULL;
    }
    surf_att_create(s, usage, storage_format);
    /* a render target can move to another block when its own is emptied (vk_rt_move) */
    if (is_rt && s->mem) {
        s->movable = 1;
        vk_devmem_owner(s->mem, s);
    }
    row_take(s);
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

/* s's image made again where want places it and copied there on cb, as vk_rt_move says; the attachment view made
   again, and the owner set where the memory is the GPU's own (DM_DEVICE), which blocks are emptied of */
static int rt_move(VkCommandBuffer cb, Surf* s, const DevMemWant* want, u64* bytes) {
    if (vk_gfx_image_holds(s)) return -1; /* a storage result of it still lands in memory from its image */
    VkImageCreateInfo ii;
    VkViewFormats vf;
    VkImageUsageFlags usage;
    VkFormat storage_format;
    /* surf_image_info gives a depth target the standard sample locations, as a new one has: this one keeps those of
       its last draw (render.c), which its depth was written with and which its barriers name */
    const VkSampleLocations locations = s->sample_locations;
    const int info_ok = surf_image_info(s, &ii, &vf, &usage, &storage_format);
    s->sample_locations = locations;
    if (!info_ok) return -1;
    DevMemInfo was_info, info;
    vk_devmem_info(s->mem, &was_info);
    VkImage img;
    DevMem mem;
    if (vk_devmem_image(&ii, want, &img, &mem) != VK_SUCCESS) return 0;
    vk_devmem_info(mem, &info);
    vk_mem_track(mem, was_info.tag ? was_info.tag - 1 : VK_OWN_TARGET);
    *bytes = info.size;
    const VkImageLayout was = s->layout;
    const VkPipelineStageFlags2 stage = s->stage;
    const VkAccessFlags2 access = s->access;
    const int copied = was != VK_IMAGE_LAYOUT_UNDEFINED; /* nothing to copy out of one in UNDEFINED */
    if (copied) {
        const VkImageAspectFlags aspect = vk_aspect(&s->f);
        vk_surf_use(cb, s, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        vk_barrier(cb, img, aspect, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_LAYOUT(TRANSFER_DST_OPTIMAL));
        const VkImageSubresourceLayers sub = {.aspectMask = aspect, .mipLevel = 0, .layerCount = s->layers};
        const VkImageCopy rg = {.srcSubresource = sub, .dstSubresource = sub, .extent = {s->sw, s->sh, 1}};
        rec_CmdCopyImage(cb, s->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), img, VK_LAYOUT(TRANSFER_DST_OPTIMAL), 1,
                         &rg);
    }
    /* the old image and every view of it go once the frames that use them are done; the views are made again of
       the new one, the attachment now and the others when they are next asked for */
    for (int i = 0; i < s->nsv; i++) vk_defer_image(VK_NULL_HANDLE, 0, s->sv[i].v, VK_NULL_HANDLE);
    s->nsv = 0;
    if (s->present) vk_defer_image(VK_NULL_HANDLE, 0, s->present, VK_NULL_HANDLE);
    s->present = VK_NULL_HANDLE;
    vk_defer_image(s->img, s->mem, s->att, s->att_linear);
    vk_defer_bytes(s->bytes);
    if (vk_devmem_emptying()) vk_defer_moved(s->bytes); /* released into a block closed to requests */
    s->att = s->att_linear = VK_NULL_HANDLE;
    s->img = img;
    s->mem = mem;
    vk_surf_bytes(s, info.size);
    s->layout = copied ? VK_LAYOUT(TRANSFER_DST_OPTIMAL) : VK_IMAGE_LAYOUT_UNDEFINED;
    s->stage = copied ? VK_PIPELINE_STAGE_2_COPY_BIT : 0;
    s->access = copied ? VK_ACCESS_2_TRANSFER_WRITE_BIT : 0;
    surf_att_create(s, usage, storage_format);
    if (want->at == DM_DEVICE) vk_devmem_owner(mem, s);
    if (copied && was != VK_LAYOUT(TRANSFER_DST_OPTIMAL)) vk_surf_use(cb, s, was, stage, access);
    return 1;
}
int vk_rt_move(VkCommandBuffer cb, Surf* s, u64* bytes) {
    const DevMemWant want = {.at = DM_DEVICE, .image = 1, .existing = 1};
    return s->demoted ? -1 : rt_move(cb, s, &want, bytes);
}

/* Render targets the caches would let go with pixels memory lacks. Such a target lost them as it went: memory has older
   ones, and writing them there as it goes put older pixels over newer ones (surf_reclaim says why). It stays instead
   (rt_give_way). Where the GPU has memory of its own, its image moves into system memory, copied bit for bit, as a
   texture's does (texture.c's second tier): the GPU still draws to it and samples it there, more slowly, and it goes
   back into the GPU's memory once it is used again and there is room (vk_rt_sys_frame). Where the GPU shares the
   system's memory a move frees nothing: the target stays where it is until the budget's pressure takes it, never for
   its age. Either way it stays a live target that whatever asks for its address finds, and memory the guest wrote
   while it was set aside is newer than its pixels: its next use loads that, as a target made after an eviction would
   (rt_aside) */
static struct {
    u64 out, back;        /* targets moved into system memory, and back, so far */
    u64 n, bytes;         /* those there now, and their bytes */
    u64 lost, lost_bytes; /* targets that went with pixels memory lacks, so far */
    u64 kept, reloads;    /* targets set aside so far (rt_aside), and those loaded again at their next use */
    u32 heap;             /* the heap they went to, plus one: 0 before the first */
} g_rt_sys;
void vk_rt_sys_stats(VkRtSysStats* st) {
    *st = (VkRtSysStats){.out = g_rt_sys.out,
                         .back = g_rt_sys.back,
                         .n = g_rt_sys.n,
                         .bytes = g_rt_sys.bytes,
                         .lost = g_rt_sys.lost,
                         .lost_bytes = g_rt_sys.lost_bytes,
                         .kept = g_rt_sys.kept,
                         .reloads = g_rt_sys.reloads};
}
/* s's image moved into system memory (the frame's command buffer: a rendering pass open ends): the bytes of the GPU's
   memory it lets go, or 0, with why in *no */
static u64 rt_demote(Surf* s, const char** no) {
    if (g_vk.mem_model == VK_MEM_UMA) {
        *no = "the GPU shares the system's memory, so a move frees none of it";
        return 0;
    }
    if (!s->mem) {
        *no = "its image is in pieces";
        return 0;
    }
    const DevMemWant want = {.at = DM_SYSTEM, .image = 1};
    const u64 was = s->bytes;
    u64 bytes;
    vk_render_end();
    const int moved = rt_move(vk_cmd(), s, &want, &bytes);
    if (moved <= 0) {
        *no = moved < 0 ? "a storage result of it still lands" : "no system memory took its image";
        return 0;
    }
    DevMemInfo info;
    vk_devmem_info(s->mem, &info);
    g_rt_sys.heap = vk_devmem_props()->memoryTypes[info.type].heapIndex + 1;
    vk_surf_bytes(s, 0); /* none of the GPU's memory: the budget's bytes are those */
    s->demoted = 1;
    s->sys_bytes = bytes;
    g_rt_sys.out++;
    g_rt_sys.n++;
    g_rt_sys.bytes += bytes;
    return was;
}
/* s out of system memory: moved back, or freed */
static void rt_sys_left(Surf* s) {
    if (!s->is_rt || !s->demoted) return;
    s->demoted = 0;
    g_rt_sys.n--;
    g_rt_sys.bytes -= s->sys_bytes;
    s->sys_bytes = 0;
}
/* s's image back in the GPU's memory: 1 when it went */
static int rt_promote(Surf* s) {
    const DevMemWant want = {
        .at = DM_DEVICE, .image = 1, .moves = 1, .low = vk_mem_owner_prio(VK_OWN_TARGET) == DM_PRIO_LOW};
    u64 bytes;
    vk_render_end();
    if (rt_move(vk_cmd(), s, &want, &bytes) <= 0) return 0;
    rt_sys_left(s);
    g_rt_sys.back++;
    return 1;
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

Surf* vk_surf_first(u64 gva) { return g_rows.surf ? g_rows.surf[g_rows.hash[hkey(gva)]] : NULL; }
Surf* vk_surf_next(const Surf* s) { return g_rows.surf[g_rows.hnext[s->row]]; }
void vk_surf_add(Surf* s) {
    row_take(s);
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
        for (Surf* s = vk_surf_first(gva); s; s = vk_surf_next(s))
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
    rec_CmdBlitImage(cb, src, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), dst, VK_LAYOUT(TRANSFER_DST_OPTIMAL), 1, &b,
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
    vk_surf_use(cb, s, VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (u32 layer = 0; layer < s->layers; layer++) {
        VkBufferImageCopy c = {.bufferOffset = off + (u64)layer * slice,
                               .bufferRowLength = pitch / s->f.bpp,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
                               .imageExtent = {s->w, s->h, 1}};
        DevMem tm;
        const int scaled = s->sw != s->w || s->sh != s->h;
        VkImage t = scaled ? vk_unscaled_image(s, 1, &tm) : VK_NULL_HANDLE;
        /* no memory for the image of the guest's size: never a copy of that size into the scaled one, which went
           past its edge and faulted the GPU's copy engine (Xid 39) in a racing game at resolution=0.5 */
        if (scaled && !t) return 0;
        if (!t)
            rec_CmdCopyBufferToImage(cb, buf, s->img, VK_LAYOUT(TRANSFER_DST_OPTIMAL), 1, &c);
        else {
            c.imageSubresource.baseArrayLayer = 0;
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_LAYOUT(TRANSFER_DST_OPTIMAL));
            rec_CmdCopyBufferToImage(cb, buf, t, VK_LAYOUT(TRANSFER_DST_OPTIMAL), 1, &c);
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_LAYOUT(TRANSFER_SRC_OPTIMAL));
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
    vk_surf_use(cb, s, VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    rec_CmdCopyBufferToImage(cb, buf, s->img, VK_LAYOUT(TRANSFER_DST_OPTIMAL), 1, &c);
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
            if (!vk_guest_in_place(from, hi - lo, 16, &staged, &off)) { /* else read in a region */
                u8* p = vk_stage_guest_input(from, hi - lo, hi - lo, &staged, &off);
                if (!p) return load_fail(s, l, y, "staging", hi - lo);
                vk_guest_read(from, hi - lo, p, staged, off);
            }
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
/* a block linear target loaded: its guest bytes staged as they are, from where they are, and deswizzled on the GPU,
   or read in the region that holds them (vk_guest_in_place). One whose rows need more than VK_PART_SHARE of the cap goes
   in bands (load_bands); a multisampled one goes whole, its layers drawn from the guest's layout of them (vk_ms_pack),
   and so do a scaled one, blitted from an unscaled image, and a volume, its slices one move */
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
    const int in_place = vk_guest_in_place(s->gva, sz, 16, &staged, &off);
    u8* p = in_place ? NULL : vk_stage_guest_input(s->gva, sz, sz, &staged, &off);
    if (!in_place && !p) return load_fail(s, 0, 0, "staging", sz);
    if (!(scratch = vk_bl_scratch(lin, &cap))) return load_fail(s, 0, 0, "scratch buffer", lin);
    if (!in_place) vk_guest_read(s->gva, sz, p, staged, off); /* what the GPU holds of it copied on the GPU */
    VkCommandBuffer cb = vk_cmd();
    vk_bl_barrier(cb, 1);
    for (u32 i = 0; i < moves; i++)
        if (!vk_bl_move(cb, &m, 0, staged, off + (u64)i * s->layer_size, scratch, (u64)i * pitch * h)) return 0;
    vk_bl_barrier(cb, 0);
    return rt_load_copy(cb, s, scratch, 0, pitch, (u64)pitch * h);
}
/* a pitch linear one: its guest bytes staged as they lie, the copy taking the pitch, when it is whole texels (else a
   row at a time into tight rows); what the GPU holds of them copied on the GPU. Whole texels that a region holds are
   read there (vk_guest_in_place), at a multiple of 4 and of a texel's bytes */
static int surf_load_pitch(Surf* s) {
    u32 gh = guest_h(s);
    u64 sz = rt_bytes(s), row = (u64)guest_w(s) * s->f.bpp, align = 4;
    int as_is = s->pitch % s->f.bpp == 0;
    VkBuffer buf;
    u64 off;
    vk_render_end();
    while (s->f.bpp && align % s->f.bpp) align += 4;
    if (as_is && vk_guest_in_place(s->gva, sz, align, &buf, &off))
        return rt_load_copy(vk_cmd(), s, buf, off, s->pitch, s->layer_size);
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
    /* the console's GPU reads the target's memory as it is: the load is the runtime's work, which a game that times
       its frames must not count (stamp.c) */
    vk_stamp_runtime_begin();
    rt_reload(gva);
    vk_stamp_runtime_end();
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
   packed depth layer goes whole (flush_parts gives it whole rows). 0 when a scaled one found no memory for the image
   of the guest's size it goes through, -1 when a multisampled layer's samples did not unpack (vk_ms_unpack): its
   bytes in buf stay as they were */
static int flush_copy(VkCommandBuffer cb, Surf* s, VkBuffer buf, u64 at, u32 pitch, const FlushPart* part) {
    u64 slice = (u64)pitch * part->rows;
    u32 depth = s->type == VK_IMAGE_VIEW_TYPE_3D ? s->depth3d : 1; /* a 3D image's slices in one copy */
    if (s->msaa) { /* the part's samples drawn into the guest's layout of them a layer at a time, copied from there */
        int left = 0;
        for (u32 layer = part->layer; layer < part->layer + part->layers; layer++) {
            VkImage grid = vk_ms_unpack(cb, s, layer, part->y, part->rows);
            if (!grid) {
                left = 1;
                continue;
            }
            VkBufferImageCopy c = {
                .bufferOffset = at + (u64)(layer - part->layer) * slice,
                .bufferRowLength = pitch / s->f.bpp,
                .imageSubresource = {s->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageExtent = {guest_w(s), part->rows, 1}};
            rec_CmdCopyImageToBuffer(cb, grid, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), buf, 1, &c);
        }
        return left ? -1 : 1;
    }
    vk_surf_use(cb, s, VK_LAYOUT(TRANSFER_SRC_OPTIMAL),
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
        const int scaled = s->sw != s->w || s->sh != s->h;
        VkImage t = scaled ? vk_unscaled_image(s, 1, &tm) : VK_NULL_HANDLE;
        if (scaled && !t) return 0; /* as rt_load_copy: never a copy of the guest's size out of the scaled image */
        if (!t)
            rec_CmdCopyImageToBuffer(cb, s->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), buf, 1, &c);
        else {
            c.imageSubresource.baseArrayLayer = 0;
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_LAYOUT(TRANSFER_DST_OPTIMAL));
            scale_blit(cb, s->img, s->sw, s->sh, layer, t, s->w, s->h, 0, s->f.is_int);
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_LAYOUT(TRANSFER_SRC_OPTIMAL));
            rec_CmdCopyImageToBuffer(cb, t, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), buf, 1, &c);
            vk_defer_image(t, tm, VK_NULL_HANDLE, VK_NULL_HANDLE);
        }
    }
    return 1;
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
    if (s->msaa && !surf_volume(s)) {
        /* a layer at a time through the scratch buffer: one whose samples did not unpack is not moved, and buf keeps
           its bytes there, as the copy of a pitch linear one does. Moved, it would swizzle the scratch buffer's old
           bytes over them */
        for (u32 i = 0; i < moves; i++) {
            FlushPart one = *part;
            one.layer = part->layer + i;
            one.layers = 1;
            int copied = flush_copy(cb, s, scratch, 0, pitch, &one);
            if (!copied) return 0;
            if (copied < 0) continue;
            vk_bl_barrier(cb, 1);
            if (!vk_bl_move(cb, &m, 1, buf, at + (u64)i * s->layer_size, scratch, 0)) return 0;
            vk_bl_barrier(cb, 0);
        }
        return 1;
    }
    if (!flush_copy(cb, s, scratch, 0, pitch, part)) return 0;
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
    if (!s->bl) return flush_copy(cb, s, buf, at, row, part) != 0;
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
    vk_finish_at(g_vk_finish_who ? g_vk_finish_who : __FILE__, g_vk_finish_who ? 0 : __LINE__);
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
        gpu_mem_written_by(s->gva, size, GPU_WRITE_TARGET, (u32)s->f.vk, s->gva);
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
    /* a parked target (another size or format took its address, rt_create_) owns none of memory's bytes now: its pixels,
       a storage image's result among them, land nowhere and are settled as if they had. Its address with the parked
       bit is no GPU address: a write stamped there reads as one over every page (write_pages_stamp), which loaded
       every target again from memory and left the depth targets undefined */
    if (s->gva >> 63) {
        s->mem_wgen = wgen;
        return;
    }
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
                gmem_write_by(gva + at, tmp + at, to - at, GPU_WRITE_TARGET, (u32)s->f.vk, s->gva);
                flush_note_landed(gva, at, to);
            }
            if (i < kept) at = newer.run[i][1];
        }
        free(tmp);
    } else if (tmp) {
        gmem_write_by(gva, tmp, sz, GPU_WRITE_TARGET, (u32)s->f.vk, s->gva);
        flush_note_landed(gva, 0, sz);
        free(tmp);
    } else {
        /* caches built from this memory check it again */
        gpu_mem_written_by(gva, sz, GPU_WRITE_TARGET, (u32)s->f.vk, s->gva);
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
static int rt_superseded(const Surf* s);
/* The targets over [gva, gva + size) holding pixels guest memory hasn't got, oldest image first: memory freed and
   given to a new target still has the old target's pixels pending, and where two cover the same bytes the newer one's
   go on top. In local while it holds nlocal, else in a list the caller frees */
static u32 rt_pending_over(u64 gva, u64 size, Surf** local, u32 nlocal, Surf*** out) {
    Surf** list = local;
    u32 n = 0, cap = nlocal;
    /* the live targets that can reach gva, from the index, or every target the lists keep */
    u32 at = g_rts_failed ? 0 : rts_lower(gva > g_rts_span ? gva - g_rts_span : 0);
    u32 walk = g_rts_failed ? g_rows.lists[VK_LRU_RT].head : 0;
    int lru = VK_LRU_RT;
    for (;;) {
        Surf* s;
        if (!g_rts_failed) {
            if (at >= g_nrts || g_rts[at]->gva >= gva + size) break;
            s = g_rts[at++];
        } else {
            while (!walk && lru < VK_LRU_RT + LRU_AGED) walk = g_rows.lists[lru += LRU_AGED].head;
            if (!walk) break;
            s = g_rows.surf[walk];
            walk = g_rows.lru[walk].next;
        }
        if ((s->gva >> 63) || s->gva >= gva + size || s->gva + rt_bytes(s) <= gva || !flush_bytes(s)) continue;
        /* later targets drew all over it since: memory would hold their pixels there, so none of its own go to memory
           before another owner or a write takes bytes under it (one game's three shadow layers of 18 MB, drawn a frame
           128 KiB further on in its pool, were published and written back when a 2 KB image went over the old ones) */
        if (rt_superseded(s)) continue;
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
    u64 lines = limit / row; /* pitch linear: rows of row bytes, one at least when it is within the cap */
    u32 rows = s->bl   ? vk_rt_band_rows(g_vk.alloc_cap, VK_PART_SHARE, row, h, bl_height(h, s->bh))
               : lines ? (u32)(lines < h ? lines : h)
                       : row <= g_vk.alloc_cap;
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
static void surf_flush_body(Surf* s, const u8* original);
/* A flush waits for the GPU to reach the copies, so each is a round trip the guest's threads sit in: logged with the
   target and the reason a caller of vk_rt_before_write named (g_vk_finish_who) when it costs a millisecond, since
   which of the paths asks for it decides whether the wait is needed then at all */
static void surf_flush(Surf* s, const u8* original) {
    u64 t0 = plat_ns(), dt;
    u64 size = flush_bytes(s);
    surf_flush_body(s, original);
    dt = plat_ns() - t0;
    if (size && dt > 1000000)
        LOG_LIMIT(24, "vulkan: a flush of %llu KB took %llu us, for %s\n", (unsigned long long)(size >> 10),
                  (unsigned long long)(dt / 1000), g_vk_finish_who ? g_vk_finish_who : "an unnamed caller");
}
static void surf_flush_body(Surf* s, const u8* original) {
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
   pending, publishing the buffer took each again (vk_rt_before_publish_cpu, a write-back then: a wait for the GPU and
   its readback), and the landing of the bytes' last owner under it: one game draws four 32x32 targets of 256 KB and
   writes them with a compute program every frame, four waits and four landings a frame. Their images keep their
   pixels, older than memory's (the publish stamps the bytes written), so they load memory's before they are used
   again, as after a flush */
static void publish_span(const Surf* s, VkOwnRes* r, u64 base, u64 at, u64 n);
/* With r (s's publish, its byte 0 s's span's byte base), r is published over each one's bytes once all are retired:
   publishing one publishes the pending targets over it first (vk_rt_before_publish_cpu). How many it retired */
static u32 rt_retire_over(u64 gva, u64 size, const Surf* s, VkOwnRes* r, u64 base) {
    Surf *local[16], **list;
    u32 count, retired = 0;
    if (!pend_hit_va(&g_rt_pages, gva, size)) return 0;
    count = rt_pending_over(gva, size, local, 16, &list);
    for (u32 pass = 0; pass < (r ? 2u : 1u); pass++)
        for (u32 i = 0; i < count; i++) {
            Surf* t = list[i];
            if (t->gva < gva || t->gva + rt_bytes(t) > gva + size) continue; /* vk_rt_overlay flushed those */
            if (pass) {
                publish_span(s, r, base, t->gva - s->gva, rt_bytes(t));
                continue;
            }
            t->mem_wgen = t->wgen;
            vk_surf_relist(t);
            retired++;
        }
    if (list != local) free(list);
    return retired;
}
/* bytes [at, at + n) of s's span published over the guest memory there from r, whose byte 0 is the span's byte base
   (0, or a band's first: publish_bands), a run of it at a time (an unmapped page takes no write), and stamped
   written */
static void publish_span(const Surf* s, VkOwnRes* r, u64 base, u64 at, u64 n) {
    for (u64 off = at, end = at + n; off < end;) {
        u64 cpu = 0, k = gmmu_run(s->gva + off, end - off, &cpu);
        if (!k) {
            k = 0x1000 - ((s->gva + off) & 0xFFF);
            if (k > end - off) k = end - off;
        } else
            vk_own_publish(cpu, k, r, off - base);
        off += k;
    }
    gpu_mem_written_by(s->gva + at, n, GPU_WRITE_IMAGE, (u32)s->f.vk, s->gva);
}
#define RT_OWN_RUNS 4
static u32 rt_own_runs(const Surf* s, u64 size, u64 run[][2], u32 max);
/* r published over s's own bytes: each layer's from where it starts (a volume's span whole), where no target drawn
   later is over them (rt_own_runs). The bytes between its layers aren't s's, and keep their owners: published as the GPU saw them, they changed nothing, but they were stamped
   written all the same, and all that is built from them was loaded again. One game writes each level of its reflection
   cubes through a view of that level, whose layers are a whole mip chain apart, so each view's span holds the other
   views' levels: every view and every cube loaded all its memory again each frame, 30 uploads and 12 MB */
/* The record of GPU-side writes is by 4 KiB page, and a mip tail's levels share a page: the publish of one level's
   storage surface stamped the page, and every target or stored surface of another level in it read as written after
   its draw (vk_rt_drawn_fresh), though no byte of it changed. One game's cubes kept four levels of each face so, and
   loaded them from memory in each present. The drawn surfaces over [lo, hi)'s pages that are fresh
   before the publish and share no byte with s's span keep their baseline past its stamp (keep_fresh_after) */
#define KEEP_FRESH 64
static u64 g_drawn_max; /* defined with the drawn list below */
typedef struct {
    Surf* s[KEEP_FRESH];
    int n;
} KeepFresh;
static void keep_fresh_before(const Surf* s, u64 lo, u64 hi, KeepFresh* k) {
    Surf* d[KEEP_FRESH];
    k->n = 0;
    if (hi <= lo) return;
    const u64 plo = lo & ~0xFFFull, phi = (hi + 0xFFF) & ~0xFFFull;
    int n = vk_rt_drawn_in(plo > g_drawn_max ? plo - g_drawn_max : 0, phi, d, KEEP_FRESH);
    if (n > KEEP_FRESH) n = KEEP_FRESH;
    for (int i = 0; i < n; i++) {
        Surf* o = d[i];
        const u64 olo = o->gva, ohi = o->gva + rt_bytes(o);
        if (o == s || ohi <= plo || olo >= phi || (olo < hi && lo < ohi) || !o->draw_wseen ||
            gpu_mem_dirty_since(o->draw_gw, olo, ohi - olo))
            continue;
        k->s[k->n++] = o;
    }
}
static void keep_fresh_after(const KeepFresh* k) {
    for (int i = 0; i < k->n; i++) k->s[i]->draw_gw = gpu_mem_generation();
}
static void publish_own(const Surf* s, VkOwnRes* r, u64 size) {
    const u64 own = rt_layer_bytes(s);
    const u32 layers = surf_volume(s) || s->layers < 2 || own >= s->layer_size ? 1 : s->layers;
    u64 run[RT_OWN_RUNS][2];
    const u32 runs = rt_own_runs(s, size, run, RT_OWN_RUNS);
    for (u32 i = 0; i < layers; i++) {
        const u64 at = layers == 1 ? 0 : (u64)i * s->layer_size, end = layers == 1 ? size : at + own;
        for (u32 k = 0; k < runs; k++) {
            const u64 from = run[k][0] > at ? run[k][0] : at, to = run[k][1] < end ? run[k][1] : end;
            if (from < to) publish_span(s, r, 0, from, to - from);
        }
    }
}
/* A storage image's results kept on the GPU (docs/layers/gpu/ownership.md), as the DMA engine's destination is: its span
   staged as the GPU sees it into a buffer of its own (the bytes between its texels keep theirs), its texels laid out
   over that in the guest's layout, and the buffer published over its texels and the targets taken in (publish_own,
   rt_retire_over) run by run of guest memory. Nothing waits:
   the bytes land when the CPU touches them. A 3D volume a game writes twice a frame and only samples took a wait
   each, a fifth of the run. A pitch-linear one's rows go at the guest's pitch, over the bytes between them as the
   guest has them. 0 when it can't be (pitch-linear rows of part texels, or layers not a pitch's rows apart, no room):
   the caller flushes at once */
static int rt_overlay(u64 gva, u64 size, VkBuffer buf, u64 at, const Surf* of, int newer);
static void rt_publish_newer(const Surf* s, u64 size, int whole);
/* the target whose owner a publish is putting over its span (publish_own, rt_retire_over): the pending ones newer
   than it are not published before that owner (rt_before_cpu). The ones reaching past its span are after it
   (rt_publish_newer); one at another GPU address of the memory stays pending, on top, as a write-back leaves it */
static const Surf* g_rt_publishing;
static int surf_publishable(const Surf* s) {
    return s->bl || (!(s->pitch % s->f.bpp) && s->type != VK_IMAGE_VIEW_TYPE_3D &&
                     (s->layers == 1 || s->layer_size == (u64)s->pitch * guest_h(s)));
}
/* A block linear target of one layer over what one owner may hold (g_vk.alloc_cap), published a band of rows of blocks
   at a time (flush_parts' bands), each band an owner of its own over its bytes: the CPU's access lands the band it
   touches, and no page of the span is let go before its bytes are there. Written back in parts, a 512 MiB target's
   span was let go when the write-back took it (its watch found by own_acquire) and written over a second after, and
   a read on another thread meanwhile took the bytes from before (#2094). 0 when a band can't be, with the bands
   before it published: the caller writes the target back, over them */
static int publish_bands(Surf* s, u64 size, u32 mem_wgen, u64 gw0) {
    u32 h = guest_h(s), pitch = (guest_w(s) * s->f.bpp + 15) & ~15u;
    u32 rows = vk_rt_band_rows(g_vk.alloc_cap, VK_PART_SHARE, guest_w(s) * s->f.bpp, h, bl_height(h, s->bh));
    if (!rows) return 0;
    for (u32 y = 0; y < h; y += rows) {
        FlushPart part = {0, 1, y, h - y < rows ? h - y : rows};
        VkBlMove m = rt_bl_move(s, pitch);
        u64 at, hi;
        m.y0 = y;
        m.rows = part.rows;
        vk_bl_range(&m, &at, &hi); /* the band's rows of blocks: at in the span, hi - at bytes */
        if (hi > size) hi = size;
        VkOwnRes* r = vk_own_res_new(hi - at, VK_OWN_BY_TARGET);
        if (!r) return 0;
        if (!vk_guest_read_device(s->gva + at, hi - at, r->buf, 0)) {
            vk_own_res_unref(r);
            return 0;
        }
        s->mem_wgen = s->wgen; /* the older targets over the band, under s's pixels */
        rt_overlay(s->gva + at, hi - at, r->buf, 0, s, 0);
        s->mem_wgen = mem_wgen;
        VkCommandBuffer cb = vk_cmd();
        vk_all_barrier_by(VK_BAR_SURFACE, cb);
        if (!flush_record(cb, s, r->buf, 0, &part)) {
            vk_own_res_unref(r);
            return 0;
        }
        vk_all_barrier_by(VK_BAR_SURFACE, cb);
        s->mem_wgen = s->wgen; /* the newer ones wholly in the band, over them */
        if (rt_overlay(s->gva + at, hi - at, r->buf, 0, s, 1)) vk_all_barrier_by(VK_BAR_SURFACE, vk_cmd());
        const Surf* publishing = g_rt_publishing;
        g_rt_publishing = s;
        rt_retire_over(s->gva + at, hi - at, s, r, at); /* r holds the band: its byte 0 is the span's byte at */
        publish_span(s, r, at, at, hi - at);
        g_rt_publishing = publishing;
        s->mem_wgen = mem_wgen;
        vk_own_res_unref(r);
    }
    s->mem_wgen = s->wgen;
    s->pub_serial = 0; /* no one owner holds it */
    flush_rebase(s, size, 0, gw0);
    vk_surf_relist(s);
    if (!s->is_rt) vk_tex_note_writeback(s);
    rt_publish_newer(s, size, 1);
    return 1;
}
/* the parts of a publish's span [0, size) that its flush leaves as guest memory has them, as ranges of the span in
   r[n][2], sorted and apart: 0 when they are more than max, and the caller reads the span whole. Each 2D layer of a
   block linear target writes every GOB whose 64 bytes and 8 rows its image holds whole: the GOBs left are those at the
   image's right and bottom edges, which hold padding, and the span past the layout's size. The CPU's newer runs are
   read too, as the caller copies them out of the read (cpu_newer). Reading the span whole read every result over it,
   about 35 copies a publish in one game's race, and the flush wrote over nearly all of them */
static u32 publish_gaps(const Surf* s, u64 size, const CpuRuns* newer, u64 (*r)[2], u32 max) {
    u32 n = 0;
    u64 row = (u64)guest_w(s) * s->f.bpp, h = guest_h(s), base = 0;
    u32 gx = (u32)((row + 63) / 64), gy = (u32)((h + 7) / 8), full_x = (u32)(row / 64), full_y = (u32)(h / 8);
    u32 bh = bl_height((u32)h, s->bh), block = 1u << bh;
    u32 blocks_y = (gy + block - 1) >> bh;
    u64 gob = 512, bsize = gob << bh, layout = (u64)gx * blocks_y * bsize;
#define GAP(lo_, hi_)                                                                                                  \
    do {                                                                                                               \
        u64 lo = (lo_), hi = (hi_) < size ? (hi_) : size;                                                              \
        if (lo >= hi) break;                                                                                           \
        if (n && r[n - 1][1] >= lo) {                                                                                  \
            if (hi > r[n - 1][1]) r[n - 1][1] = hi;                                                                    \
        } else {                                                                                                       \
            if (n == max) return 0;                                                                                    \
            r[n][0] = lo;                                                                                              \
            r[n++][1] = hi;                                                                                            \
        }                                                                                                              \
    } while (0)
    /* each layer, layer_size apart: blocks in layout order, where a block of a column the image spans whole is
       left only below its rows, and a column it spans in part, or not at all, is left whole; then the bytes up to the
       next layer */
    for (u32 l = 0; l < s->layers; l++, base += s->layer_size) {
        for (u32 by = 0; by < blocks_y; by++)
            for (u32 bx = 0; bx < gx; bx++) {
                u64 at = base + ((u64)by * gx + bx) * bsize;
                u32 first = by << bh; /* the block's first GOB row */
                if (bx >= full_x)
                    GAP(at, at + bsize);
                else if (first + block > full_y)
                    GAP(at + (u64)(full_y > first ? full_y - first : 0) * gob, at + bsize);
            }
        GAP(base + layout, l + 1 < s->layers ? base + s->layer_size : size);
    }
#undef GAP
    /* the CPU's runs, merged in order */
    for (u32 i = 0; i < newer->n; i++) {
        u64 lo = newer->run[i][0], hi = newer->run[i][1];
        u32 k = 0;
        while (k < n && r[k][1] < lo) k++;
        if (k < n && r[k][0] <= hi) { /* overlaps or touches r[k] */
            if (lo < r[k][0]) r[k][0] = lo;
            if (hi > r[k][1]) r[k][1] = hi;
            while (k + 1 < n && r[k + 1][0] <= r[k][1]) { /* and the ones after it it reaches */
                if (r[k + 1][1] > r[k][1]) r[k][1] = r[k + 1][1];
                for (u32 j = k + 1; j + 1 < n; j++) r[j][0] = r[j + 1][0], r[j][1] = r[j + 1][1];
                n--;
            }
        } else {
            if (n == max) return 0;
            for (u32 j = n; j > k; j--) r[j][0] = r[j - 1][0], r[j][1] = r[j - 1][1];
            r[k][0] = lo;
            r[k][1] = hi;
            n++;
        }
    }
    return n ? n : (u32)-1; /* -1: nothing to read, the flush writes it all */
}
u64 g_vk_publish_reads_cut, g_vk_publish_bytes_skipped; /* publishes that read only their gaps, and the bytes left */
int vk_surf_publish(Surf* s) {
    SNX_ZONE(z_publish, "surface publish");
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
    /* over one owner: in bands when nothing the CPU wrote since the draws has to stay over the pixels (cpu_newer), a
       single 2D layer that packs no depth; else the caller writes it back */
    if (size > g_vk.alloc_cap)
        return s->bl && !kept && s->layers == 1 && !surf_volume(s) && !depth_packs(s) &&
               publish_bands(s, size, mem_wgen, gw0);
    r = vk_own_res_new(size, VK_OWN_BY_TARGET);
    if (!r) return 0;
    /* straight into the buffer that becomes the span's owner, as the copy engine's destinations go (dma.c): the bytes
       the GPU owns there are copied on the GPU, and only the CPU's cross the bus, once. Staged in host memory, the span
       went out and came back several times, 12 to 14 MB a frame in one game. The flush below writes the image's own
       bytes into the span, so the copy is thrown away where the span is the image whole (no bytes past a row, no rows
       past the image, one layer) and the CPU wrote none of it: the picture before the picture. A layout with padding
       keeps its bytes, and the copy is what brings them; so does a layer's span with room after it, which the copy
       leaves as it found it (publish_own writes each layer's own bytes only) */
    u64 row = (u64)guest_w(s) * s->f.bpp;
    int covers_all = s->bl && !s->msaa && s->layers == 1 && !surf_volume(s) && kept == 0 &&
                     bl_size((u32)row, guest_h(s), s->bh) == row * guest_h(s);
    /* one list for every publish, as threads publish one at a time (vk_lock). The read below can publish again on the
       same thread (gmem_need lands a kept storage image, image_flush_at, vk_surf_keep_end), and that publish would
       write over the list the read walks: it reads its span whole instead */
    static u64 gaps[4096][2];
    static int gaps_busy;
    /* a multisampled target reads its span whole: a layer whose samples fail to unpack (vk_ms_unpack) keeps the bytes
       guest memory has */
    u32 ngaps = !gaps_busy && !covers_all && s->bl && !s->msaa && !surf_volume(s) && !depth_packs(s) &&
                        (s->layers == 1 || s->layer_size >= rt_layer_bytes(s))
                    ? publish_gaps(s, size, &newer, gaps, sizeof gaps / sizeof *gaps)
                    : 0;
    if (ngaps) { /* the parts the flush leaves alone */
        u64 read = 0;
        if (ngaps != (u32)-1) {
            for (u32 i = 0; i < ngaps; i++) read += gaps[i][1] - gaps[i][0];
            gaps_busy = 1;
            int ok = vk_guest_read_owned_runs(s->gva, (const u64(*)[2])gaps, ngaps, r);
            gaps_busy = 0;
            if (!ok) {
                vk_own_res_unref(r);
                return 0;
            }
        }
        g_vk_publish_reads_cut++;
        g_vk_publish_bytes_skipped += size - read;
    } else if (!covers_all && !vk_guest_read_owned(s->gva, size, r, 0)) {
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
        vk_all_barrier_by(VK_BAR_SURFACE, cb);
    }
    /* the older targets over the span: s's own pixels go over them below, in the guest's layout, so a target leaves
       itself out (its tight rows would be a pitch-linear one's padding) */
    s->mem_wgen = s->wgen;
    rt_overlay(s->gva, size, r->buf, 0, s, 0);
    s->mem_wgen = mem_wgen;
    cb = vk_cmd();
    vk_all_barrier_by(VK_BAR_SURFACE, cb);
    FlushPart whole = flush_whole(s);
    if (!(s->bl ? flush_record(cb, s, r->buf, 0, &whole) : flush_copy(cb, s, r->buf, 0, s->pitch, &whole))) {
        vk_own_res_unref(r);
        return 0;
    }
    vk_all_barrier_by(VK_BAR_SURFACE, cb);
    s->mem_wgen = s->wgen; /* the newer targets wholly in the span, over s's pixels */
    if (rt_overlay(s->gva, size, r->buf, 0, s, 1)) vk_all_barrier_by(VK_BAR_SURFACE, vk_cmd());
    s->mem_wgen = mem_wgen;
    cb = vk_cmd();
    for (u64 i = 0, k = 0; i < kept; i++) { /* the CPU's runs over the image (cpu_newer) */
        VkBufferCopy kc = {kat + k, newer.run[i][0], newer.run[i][1] - newer.run[i][0]};
        rec_CmdCopyBuffer(cb, kbuf, r->buf, 1, &kc);
        k += kc.size;
    }
    if (kept) vk_all_barrier_by(VK_BAR_SURFACE, cb);
    /* The buffer now holds this image, and the targets under it: publishing it must not flush them again */
    s->mem_wgen = s->wgen;
    const Surf* publishing = g_rt_publishing;
    g_rt_publishing = s;
    const u32 retired = rt_retire_over(s->gva, size, s, r, 0);
    KeepFresh keep;
    keep_fresh_before(s, s->gva, s->gva + size, &keep);
    publish_own(s, r, size);
    keep_fresh_after(&keep);
    g_rt_publishing = publishing;
    s->pub_serial = atomic_load_explicit(&r->serial, memory_order_acquire);
    vk_own_res_unref(r);
    /* guest memory (its owner) has the image's pixels now: the texture cache finds them current, by their owner */
    flush_rebase(s, size, kept != 0, gw0);
    vk_surf_relist(s);
    if (!s->is_rt) vk_tex_note_writeback(s);
    rt_publish_newer(s, size, retired != 0);
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
/* the one owner of all of [cpu, cpu + size), the span at *off of it, or NULL (vk_lock). own.c holds an owner as one
   entry for each run of the pool's pages under it (own_begin), so one owner can come back in pieces: they are one
   owner when they are the same resource, touch, and go on in it where the one before ended. Counted as no owner, a
   storage image published over two runs was taken as landed, read back through the CPU, and written back at every
   draw from then on: one game ran a fifth slower in the runs where the pool gave its image two runs */
static VkOwnRes* span_owner(u64 cpu, u64 size, u64* off) {
    OwnRange r[16]; /* own_begin's most */
    u32 n = own_lookup(cpu, size, r, 16);
    u64 at = cpu;
    if (!n || n > 16) return NULL;
    for (u32 i = 0; i < n; i++) {
        if (r[i].addr != at || r[i].res != r[0].res || r[i].res_off != r[0].res_off + (at - cpu)) return NULL;
        at += r[i].size;
    }
    if (at != cpu + size) return NULL;
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
        if (vk_rt_overlay(s->gva, size, r->buf, 0)) vk_all_barrier_by(VK_BAR_SURFACE, vk_cmd());
        s->mem_wgen = mem_wgen;
        rt_retire_over(s->gva, size, NULL, NULL, 0);
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
    vk_all_barrier_by(VK_BAR_SURFACE, cb);
    VkBufferCopy c = {off, 0, size};
    rec_CmdCopyBuffer(cb, held->buf, buf, 1, &c);
    flush_wait(cb);
    memcpy(original, p, size);
    return original;
}
int vk_surf_keep_whole(const Surf* s, const VkImageKeep* k) {
    u64 size = rt_bytes(s), cpu = 0, off = 0;
    return gmmu_run(s->gva, size, &cpu) == size && span_owner(cpu, size, &off) == k->held && off == k->off;
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
u64 g_vk_rt_dropped;   /* render targets dropped instead of written back: the guest wrote over all of their bytes */
/* a publish made for another one's sake (rt_overlay, rt_publish_newer) is under way: one met inside it is written back */
static int g_rt_nested;
/* vk_rt_overlay's work for every pending target over the range (of NULL), or for a publish of the target of: the
   ones older than it (newer 0), whose pixels go under its own, or the ones newer (newer 1), which go over them. A
   newer one that reaches past the range is left to rt_publish_newer, once of's owner is over the span: published
   here, before it, of's older pixels went over the newer one's where the two meet (one game's shadow layers, drawn a
   frame 128 KiB further on in its pool: the layer of the frame before went over the new one in guest memory, and the
   texture over them loaded the old pixels) */
static int rt_overlay(u64 gva, u64 size, VkBuffer buf, u64 at, const Surf* of, int newer) {
    Surf *local[16], **list;
    int n = 0;
    u32 count, i;
    if (!size || !pend_hit_va(&g_rt_pages, gva, size)) return 0;
    count = rt_pending_over(gva, size, local, 16, &list);
    for (i = 0; i < count; i++) {
        Surf* s = list[i];
        const int inside = s->gva >= gva && s->gva + rt_bytes(s) <= gva + size;
        if (of && (newer ? s->wseq < of->wseq || !inside : s->wseq > of->wseq)) continue;
        if (inside) {
            FlushPart whole = flush_whole(s);
            vk_render_end();
            if (flush_record(vk_cmd(), s, buf, at + (s->gva - gva), &whole)) {
                n++;
                continue;
            }
        }
        /* reaching past the range, or its copy refused: published (a copy into an owner on the GPU, no wait), as a
           release does and as rt_before_cpu does before a producer's publish, or written back where it can't be, the
           copy keeping the bytes the caller read either way. What the caller publishes over the overlap goes over that
           owner after it. Not nested: an overlay inside that publish writes such a target back. Written back always,
           it was a readback, a wait for the GPU and a landing, once a frame in one game's compute pass. A target past
           the range is mostly one whose memory a smaller surface took over since, and the program's writes there are
           newer than its pixels */
        if (!g_rt_nested) {
            g_rt_nested = 1;
            const int published = vk_surf_publish(s);
            g_rt_nested = 0;
            if (published) continue;
        }
        g_vk_rt_readbacks++;
        vk_surf_flush(s, NULL);
    }
    if (list != local) free(list);
    return n;
}
int vk_rt_overlay(u64 gva, u64 size, VkBuffer buf, u64 at) { return rt_overlay(gva, size, buf, at, NULL, 0); }
/* s's owner is over its span's size bytes: the pending targets newer than s that reach past the span (rt_overlay left
   them) are published now, so their owners go over s's where they meet; written back where one can't be published, or
   inside a publish made for another's sake. One that only meets bytes s's owner didn't take (publish_own leaves the
   ones a later target drew over) stays pending, unless the span went whole (a band, or targets retired in it) */
static void rt_publish_newer(const Surf* s, u64 size, int whole) {
    Surf *local[16], **list = local;
    u64 run[RT_OWN_RUNS][2] = {{0, size}};
    if ((s->gva >> 63) || !pend_hit_va(&g_rt_pages, s->gva, size)) return;
    const u32 runs = whole ? 1 : rt_own_runs(s, size, run, RT_OWN_RUNS);
    const u32 count = runs ? rt_pending_over(s->gva, size, local, 16, &list) : 0;
    for (u32 i = 0; i < count; i++) {
        Surf* t = list[i];
        u32 met = 0;
        if (t == s || t->wseq < s->wseq || (t->gva >= s->gva && t->gva + rt_bytes(t) <= s->gva + size)) continue;
        for (u32 k = 0; k < runs && !met; k++)
            met = t->gva < s->gva + run[k][1] && t->gva + rt_bytes(t) > s->gva + run[k][0];
        if (!met) continue;
        if (!g_rt_nested) {
            g_rt_nested = 1;
            const int published = vk_surf_publish(t);
            g_rt_nested = 0;
            if (published) continue;
        }
        g_vk_rt_readbacks++;
        vk_surf_flush(t, NULL);
    }
    if (list != local) free(list);
}

/* whether a render target not parked covers a GPU page of [gva, gva + size), with pend_lock held (or vk_lock) */
int vk_rt_pages_hit(u64 gva, u64 size) { return size && pend_hit_va(&g_rt_pages, gva, size); }
int vk_rt_over(u64 gva, u64 size) {
    Surf *local[16], **list;
    u32 count;
    if (!size || !pend_hit_va(&g_rt_pages, gva, size)) return 0;
    count = rt_pending_over(gva, size, local, 16, &list);
    if (list != local) free(list);
    return count != 0;
}

void vk_rt_before_write(u64 gva, u64 size, int whole) {
    Surf *local[16], **list;
    u32 count, i;
    if (!size || g_rt_flushing) return;
    vk_lock();
    g_rt_flushing = 1;
    if (pend_hit_va(&g_rt_pages, gva, size)) {
        count = rt_pending_over(gva, size, local, 16, &list);
        for (i = 0; i < count; i++) {
            Surf* s = list[i];
            u64 bytes = rt_bytes(s);
            /* The caller writes every byte of the target: the guest's bytes replace every pixel, so memory is the
               truth and the image has nothing of its own to give back. Flushing it would be a wait for the GPU to
               draw pixels the write is about to drop, a round trip each of about 0.3 ms for a 1 MiB target
               (tests/vulkan_headless.c, check_rt_drop). The image is dropped instead, as a reload whose memory
               changed leaves it (rt_reload): its layout goes undefined, so the next draw starts from none of it (a
               full overwrite), and a read of it builds a texture from memory, which the write left as the truth. A
               write that covers part of a target, or one whole only in part of its range, still flushes it */
            if (whole && vk_rt_write_covers(s->gva, bytes, gva, size)) {
                vk_surf_wrote(s); /* the copies of it (crops) are out of date */
                vk_render_end();
                s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
                s->draw_wseen = 0;
                s->gw = gpu_mem_generation();
                /* Nothing of the image goes to memory, as it holds none of the target's bytes now. The layout says so
                   only while nothing binds the image: a refresh of the last frame, a screenshot or a present reads it
                   and leaves it in another layout (vk_surf_use), which a flush meets. Its next draw loads the bytes
                   the program wrote (or leaves it undefined, if that load fails), and its own write makes the image
                   flushable again */
                s->mem_wgen = s->wgen;
                vk_surf_relist(s);
                g_vk_rt_dropped++;
                continue;
            }
            vk_surf_flush(s, NULL);
        }
        if (list != local) free(list);
    }
    g_rt_flushing = 0;
    vk_unlock();
}

/* A producer can replace image memory through another GPU address. Every older image over that backing goes to
   guest memory before the new owner does, so a later partial image read cannot write its old pixels over the
   producer's result: published (publish) or written back */
static void rt_before_cpu(u64 cpu, u64 size, int publish) {
    Surf* local[16];
    RtPendingList pending = {local, 0, 16};
    if (!size || g_rt_flushing) return;
    vk_lock();
    g_rt_flushing = 1;
    gmmu_aliases(cpu, size, rt_pending_alias, &pending);
    for (u32 i = 0; i < pending.count; i++) {
        /* newer than the target whose owner comes: over it, published after it (rt_publish_newer) or still pending */
        if (publish && g_rt_publishing && pending.surfaces[i]->wseq > g_rt_publishing->wseq) continue;
        if (!publish || !vk_surf_publish(pending.surfaces[i])) vk_surf_flush(pending.surfaces[i], NULL);
    }
    if (pending.surfaces != local) free(pending.surfaces);
    g_rt_flushing = 0;
    vk_unlock();
}
/* Before a writer of guest memory on the CPU (the mirror's land_entry): written back, as its write follows at once and
   a publish would trap the pages again between its own_acquire and its write */
void vk_rt_before_write_cpu(u64 cpu, u64 size) { rt_before_cpu(cpu, size, 0); }
static void rt_pending_any(void* ctx, u64 gva, u64 size) {
    int* any = ctx;
    if (!*any && pend_hit_va(&g_rt_pages, gva, size)) *any = 1;
}
int vk_rt_pending_cpu(u64 cpu, u64 size) {
    int any = 0;
    if (!size) return 0;
    vk_lock();
    gmmu_aliases(cpu, size, rt_pending_any, &any);
    vk_unlock();
    return any;
}
/* Before a producer's owner goes over [cpu, cpu + size) (own_publish): the older targets there are published, as a
   release's are (rt_publish_now), their spans copied into owners on the GPU that the producer's owner then replaces
   where they meet, with no wait. Written back, each was a readback, a wait for the GPU and the landing of the owners
   under it: one game publishes targets over memory that other targets alias, 7 such write-backs a frame */
void vk_rt_before_publish_cpu(u64 cpu, u64 size) { rt_before_cpu(cpu, size, 1); }

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
    const int open = vk_render_begun();
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, from, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, to, VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, to->layers},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, to->layers},
                     .extent = {to->sw, to->sh, 1}};
    rec_CmdCopyImage(cb, from->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), to->img, VK_LAYOUT(TRANSFER_DST_OPTIMAL),
                     1, &c);
    vk_rt_copy_count(VK_RT_COPY_TAKE, open, (u64)to->sw * to->sh * to->layers * to->f.bpp * (u64)vk_surf_samples(to),
                     from->f.vk, to->f.vk);
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
    for (Surf* s = vk_surf_first(gva); s; s = vk_surf_next(s))
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
    for (Surf* s = vk_surf_first(gva); s; s = vk_surf_next(s))
        if (s->gva == (gva | 1ull << 63) && s->is_rt && s->w == w && s->h == h && s->fkey == fkey && s->bl == bl &&
            s->bh == bh && s->pitch == pitch && s->layers == layers && s->layer_size == layer_size && s->bd == bd &&
            s->volume == volume && s->msaa == msaa && !parked)
            parked = s;
    for (Surf* s = vk_surf_first(gva); s; s = vk_surf_next(s))
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

static void rt_aside_check(Surf* s);
void vk_rt_note_read(Surf* s) {
    vk_surf_touch(s);
    if (!s->is_rt) return;
    if (s->aside) rt_aside_check(s);
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
    return d > 0 || (d == 0 && g_rows.touched[s->row] > g_rows.touched[best->row]);
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
        for (u32 r = g_rows.lists[list].head; r; r = g_rows.lru[r].next) {
            Surf* s = g_rows.surf[r];
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
static u64 mem_hash(u64 gva, u64 size) {
    u64 h = 1469598103934665603ull;
    for (u64 o = 0; o < size;) {
        u64 a = gva + o, cpu = gmmu_translate(a), n = 0x1000 - (a & 0xFFF);
        if (n > size - o) n = size - o;
        while (cpu && o + n < size && gmmu_translate(a + n) == cpu + n) /* the next page follows in host memory */
            n += size - o - n < 0x1000 ? size - o - n : 0x1000;
        /* an unmapped page counts as its length, pages the GPU holds as their owners (they are trapped) */
        if (!cpu || !own_hash(cpu, n, &h)) h = snx_fnv_step(h, cpu ? vk_hash(GP(cpu), n) : n);
        o += n;
    }
    return h;
}
static u64 rt_mem_hash(const Surf* s) { return mem_hash(s->gva, rt_bytes(s)); }
/* Pitch-linear color targets drawn since the last release. A target's pixels reach guest memory only when something
   asks for them, and the CPU can't ask. Reading back on the CPU what the GPU drew is what pitch-linear targets are
   mostly for: one game draws its ambient light's spherical harmonics into seven 8x8 RGBA32F targets and reads them
   after a fence, and lit its scenes with the old bytes there, values up to 1e31. A release watches them as it watches
   block linear ones (below): the CPU's first read publishes them, and one the CPU read lately is published at the
   release, GPU-owned (vk_surf_publish), so that its reads after find the pixels with no fault; one it never reads is
   copied back never. Without traps a release publishes them, as no watch can be had.

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
        } else if (!own_covered(cpu, n)) {
            /* Armed only where no owner or watch is yet: an owner's trap is a watch too (OwnHooks' refresh), and over
               one own_watch arms nothing. Asked of watches alone, a run under an owner was armed again at each
               release: one game's 864x480 depth target, drawn between its occlusion reports, 1,700 times a present, about
               10 ms of each (31 fps with this, 25 without) */
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
        if (!(s->gva >> 63) && flush_bytes(s)) {
            if (own_traps())
                rt_watch(s);
            else
                rt_publish_now(s);
        }
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
void vk_rt_watch_owed(void) {
    u64 cpu, size;
    /* watched again, as a release watches them: no copy and no GPU work, and the CPU's next access publishes them.
       Publishing them here copied each target into a buffer of its own that stayed until the CPU touched it, which it
       may never do. As many spans as own.c holds now: a span that these watches owe again waits for the next
       vk_unlock. A watch takes mem.c's lock (mem_own_prepare), so a hold let go under it leaves them to the next */
    if (mem_lock_held()) return;
    for (u32 i = 0; i < OWN_OWED_MAX && own_owed_take(&cpu, &size); i++) {
        Surf* local[16];
        RtPendingList pending = {local, 0, 16};
        gmmu_aliases(cpu, size, rt_pending_alias, &pending);
        for (u32 k = 0; k < pending.count; k++) rt_watch(pending.surfaces[k]);
        if (pending.surfaces != local) free(pending.surfaces);
    }
}
static u64 g_drawn_max; /* the biggest span a listed target has: how far before an address one over it can start */
/* Whether every byte from lo to lo + span was drawn over by targets other than self, each over its own span, drawn
   after the write count since (wseq). The drawn list is in order of address, so the targets over the span are a sweep
   from its start */
static int span_drawn_over(const Surf* self, u64 lo, u64 span, u64 since) {
    u64 hi = lo + span, reach = lo;
    for (int i = drawn_lower(lo > g_drawn_max ? lo - g_drawn_max : 0); i < g_ndrawn && g_drawn[i]->drawn_from <= reach;
         i++) {
        const Surf* o = g_drawn[i];
        if (o == self || o->draw_wseq <= since || o->drawn_from + o->rt_size <= reach) continue;
        reach = o->drawn_from + o->rt_size;
        if (reach >= hi) return 1;
    }
    return 0;
}
/* Whether every byte of s's span was drawn over after s last changed, by targets drawn later, each over its own span:
   memory holds their pixels now (or would on the console), so s's are no one's to read and it goes without loss. A
   game rendering at a dynamic resolution makes its scene's targets again at another size each frame over the same
   memory, and each old one held a draw memory never got: kept as precious, a few thousand took the budget from the
   targets that are (one game's ink maps, drawn once at a level's start, went under that pressure with the budget
   lowered, and its walls took no ink) */
static int rt_superseded(const Surf* s) {
    if (!s->is_rt || !s->rt_size) return 0;
    return span_drawn_over(s, s->gva & ~(1ull << 63), s->rt_size, s->wseq);
}
/* The same for a texture that holds pixels it took from targets (tex_take_drawn), which memory may lack: every byte
   of its guest memory drawn over by targets drawn after its last take (vk_tex_took). It holds no one's pixels then,
   and one made there again takes the newer targets' (rt_superseded's reasoning). A game whose frame places its
   transient targets in one pool, each frame at offsets that move with its dynamic resolution, samples its two-layer
   2048x2048 shadow map through a texture header at the new offset each frame: each was a copy of 32 MB that only the
   budget's lossy steps freed, as nothing else could tell its pixels were no one's. The textures over that pool held
   7.2 GB of images over 602 MB of guest memory, 2.2 GB of them such copies, and a 14.6 GB budget ran out in seconds */
static int tex_superseded(const Surf* s) {
    return !s->is_rt && s->took_drawn && s->size && span_drawn_over(s, s->gva, s->size, s->took_wseq);
}
void vk_tex_took(Surf* t) { t->took_wseq = g_wseq; }
void vk_rt_kinds(VkRtKinds* k) {
    *k = (VkRtKinds){0};
    for (int lru = VK_LRU_RT; lru <= VK_LRU_RT + LRU_AGED; lru += LRU_AGED)
        for (u32 r = g_rows.lists[lru].head; r; r = g_rows.lru[r].next) {
            const Surf* s = g_rows.surf[r];
            const int kind = s->layout != VK_IMAGE_LAYOUT_UNDEFINED && rt_superseded(s) ? 0 : lossless(s) ? 1 : 2;
            k->n[kind]++;
            k->bytes[kind] += s->bytes;
            k->parked[kind] += (s->gva >> 63) != 0;
            for (const Surf* c = s->crop; c; c = c->next) {
                k->copies++;
                k->copy_bytes += c->bytes;
            }
        }
}
/* The runs of s's span's size bytes that no target drawn after s last changed is over, as [from, to) offsets into the
   span in address order: the bytes whose newest pixels are s's. The rest are a later target's, as all of a target
   covered whole are (rt_superseded, whose sweep this is, kept going past a gap): s's owner there would put its pixels
   over newer ones in memory. How many; the last of max takes the rest of the span as it is. One game's shadow layer
   of 18 MB, drawn for a frame 128 KiB further on, was published whole for an image of 2 KB in the 128 KiB of the old
   one the new one left, and the new one after it: 36 MB, and the 54 MB texture over them loaded again */
static u32 rt_own_runs(const Surf* s, u64 size, u64 run[][2], u32 max) {
    const u64 lo = s->gva, hi = lo + size;
    u64 reach = lo;
    u32 n = 0;
    if (s->is_rt && !(lo >> 63))
        for (int i = drawn_lower(lo > g_drawn_max ? lo - g_drawn_max : 0); i < g_ndrawn && g_drawn[i]->drawn_from < hi;
             i++) {
            const Surf* o = g_drawn[i];
            if (o == s || o->draw_wseq <= s->wseq || o->drawn_from + o->rt_size <= reach) continue;
            if (o->drawn_from > reach) {
                if (n + 1 == max) break;
                run[n][0] = reach - lo, run[n++][1] = o->drawn_from - lo;
            }
            reach = o->drawn_from + o->rt_size;
            if (reach >= hi) return n;
        }
    run[n][0] = reach - lo, run[n++][1] = size;
    return n;
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
int vk_rt_drawn_fresh(Surf* s) { return vk_rt_drawn_fresh_in(s, 0, rt_bytes(s)); }
void vk_surf_stored(Surf* s) {
    if (s->gva >> 63) return;
    vk_rt_drawn_full(s); /* the publish wrote every byte: the baseline is from now, after its stamp */
}
int vk_rt_drawn_fresh_in(Surf* s, u64 at, u64 size) {
    const u64 all = rt_bytes(s);
    if (at >= all) return 0;
    if (size > all - at) size = all - at;
    if (!s->draw_wseen || s->layout == VK_IMAGE_LAYOUT_UNDEFINED || gpu_mem_dirty_since(s->draw_gw, s->gva + at, size))
        return 0;
    if (s->draw_has_hash) return rt_mem_hash(s) == s->draw_hash;
    int written = vk_written_since(gmem_host(s->gva + at, size), size, s->draw_wseen);
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
    const u32 r = s->row;
    vk_tex_later_forget(s);
    surf_changed(s->gva);
    vk_astc_forget(s, ~0u); /* the background encodes of its levels stop writing into it */
    if (r) {
        ended_unlink(r);
        if (g_rows.list[r]) {
            g_held[lru_kind(r)].n--;
            g_held[lru_kind(r)].bytes -= s->bytes;
            lru_unlink(r);
        }
        row_drop(s);
    }
    if (s->is_rt) rt_pages(s, -1);
    rt_sys_left(s);
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
   render targets since, or all of whose memory later targets drew over (tex_superseded). A render target: without
   defined contents, or with memory holding what it holds (written back
   or loaded since its last draw, in a format that moves between the two; rt_create loads it again), or with every
   byte drawn over by later targets (rt_superseded). A parked one too only then: taken out of parking it gives back
   its own pixels, which memory, written by neither target's draws, doesn't have */
static int lossless(const Surf* s) {
    if (!s->is_rt) return !s->took_drawn || tex_superseded(s);
    if (s->layout == VK_IMAGE_LAYOUT_UNDEFINED) return 1;
    return (!s->f.depth && s->f.raw && s->mem_wgen == s->wgen && moves_scaled(s)) || rt_superseded(s);
}
/* s out of the table and freed, with its copies (gpu/vk/texture.c rt_crop): the device memory that releases */
/* a texture where an eviction would take it (want: bytes of the GPU's memory are asked for): moved into system memory
   (vk_tex_demote), else evicted. One in system memory already frees none of the GPU's memory: it stays for such a
   reclaim, and an idle one goes as any texture does when nothing is asked for (want ~0, vk_tex_frame) */
static u64 surf_evict(Surf* s);
static u64 tex_give_way(Surf* s, u64 want) {
    if (s->demoted) return want == ~0ull ? surf_evict(s) : 0;
    const u64 moved = want != ~0ull ? vk_tex_demote(s) : 0;
    return moved ? moved : surf_evict(s);
}
/* the CPU's writes to runs of guest memory since a write-tracking clock: written 1 at the first (or where the run's
   writes can't be told) */
typedef struct {
    u32 since;
    int written;
} RunsWritten;
static void run_written(void* user, u64 at, u8* host, u64 n) {
    RunsWritten* r = user;
    (void)at;
    if (!r->written && vk_written_since(host, n, r->since) != 0) r->written = 1;
}
/* s kept where the caches would have let it go, with pixels memory lacks (rt_give_way): the guest's memory as of now,
   which its next use asks about (rt_aside_check). Taken once until that use */
static void rt_aside(Surf* s) {
    if (s->aside) return;
    const u64 at = s->gva & ~(1ull << 63), size = rt_bytes(s);
    int tracked = 0;
    span_runs(at, size, run_take, &tracked);
    if (tracked) span_runs(at, size, run_cool, NULL);
    s->aside_wseen = tracked ? vk_ww_now() : 0;
    s->aside_hash = tracked ? 0 : mem_hash(at, size);
    s->aside = 1;
    g_rt_sys.kept++;
}
/* s's first use since it was set aside: memory the guest wrote since then is newer than its pixels, which load from
   it again (vk_rt_reload), as a target made after an eviction does. A parked one is asked once it holds its address
   again. The GPU's writes are vk_rt_note_read's own question */
static void rt_aside_check(Surf* s) {
    if (s->gva >> 63) return;
    s->aside = 0;
    int written;
    if (s->aside_wseen) {
        RunsWritten r = {s->aside_wseen, 0};
        span_runs(s->gva, rt_bytes(s), run_written, &r);
        written = r.written;
    } else
        written = rt_mem_hash(s) != s->aside_hash;
    if (!written) return;
    g_rt_sys.reloads++;
    vk_rt_reload(s->gva);
}
/* a render target where an eviction would take it: under the budget's pressure (lossy VK_RECLAIM_ANY), or unused for
   minutes (VK_RECLAIM_IDLE). One memory can give again goes. One whose pixels exist only here moves into system
   memory (rt_demote) and stays there, where it frees none of the GPU's memory. One that can't move stays for its age,
   and goes under the pressure with its pixels, said */
static u64 rt_give_way(Surf* s, int lossy) {
    if (lossless(s)) return surf_evict(s);
    if (s->demoted) return 0;
    const char* no = "";
    const u64 moved = rt_demote(s, &no);
    if (moved || lossy != VK_RECLAIM_ANY) {
        rt_aside(s);
        return moved;
    }
    g_rt_sys.lost++;
    g_rt_sys.lost_bytes += s->bytes;
    LOG_LIMIT(
        8,
        "vulkan: the %ux%u render target at %llx (format %x, %llu KB) goes under the budget's pressure with pixels "
        "memory lacks, as it can't move into system memory: %s\n",
        s->w, s->h, (unsigned long long)(s->gva & ~(1ull << 63)), s->fkey, (unsigned long long)(s->bytes >> 10), no);
    return surf_evict(s);
}
static u64 surf_evict(Surf* s) {
    if (s->row && lru_kind(s->row) == VK_LRU_TEX) vk_tex_evicted(s); /* statistics: one made again later counts */
    if (s->is_rt && !(s->gva >> 63)) rtx_del(s->gva);
    if (s->row) row_unhash(s);
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
/* the bytes of the entries a trim for headroom left for their size (surf_reclaim's most), and of the textures it
   left as the game came back for them (came_back), since its last line */
static u64 g_left_big, g_left_back;
/* keep_back: textures that came back stay where they are */
static u64 surf_reclaim(int kind, u32 before, u64 want, int lossy, u64 most, int keep_back) {
    /* A render target that goes takes its contents with it: guest memory has them as of its last write-back. They
       aren't written to memory as they go: memory is shared by targets the GPU drew over one another, parked and freed
       ones among them, and by textures and buffers that copied from them on the GPU, and the renderer doesn't know them
       all. Writing an evicted target's pixels, as a game's Mii icons wanted (drawn at boot, shown a minute later), put
       older pixels over newer ones in another: a black block over the player character, and red and green blocks over
       its textures. So a target memory doesn't hold is let go only under the budget's pressure once nothing lossless
       is left (vk_mem_trim), or after minutes unused (vk_tex_frame), and then not one holding a draw a texture made at
       its address later would take (keeps_drawn), which only the budget's pressure takes. Where the GPU has memory of
       its own it moves into system memory then, with its pixels (rt_give_way); elsewhere its age leaves it, and the
       pressure takes it.

       From the least recently used on, up to the first used since `before`: with lossy, the aged list first, as it
       holds the oldest of those. A storage image's result still to land holds its surface: those are passed over,
       a few hundred at most a call. So is an entry of more than `most` bytes, which keeps its place: a trim for
       headroom does not take one far bigger than it wants (vk_mem_trim) */
    u64 freed = 0;
    u32 passed = 0;
    if (lossy)
        for (u32 r = g_rows.lists[kind + LRU_AGED].head, n; r && freed < want && passed < 512; r = n) {
            n = g_rows.lru[r].next;
            Surf* s = g_rows.surf[r];
            if ((i32)((u32)s->last_used - before) >= 0) break;
            if (lossy == VK_RECLAIM_IDLE && keeps_drawn(s)) continue;
            if (s->bytes > most) {
                g_left_big += s->bytes;
                continue;
            }
            if (keep_back && s->came_back) {
                g_left_back += s->bytes;
                continue;
            }
            if (vk_gfx_image_holds(s))
                passed++;
            else
                freed += kind == VK_LRU_TEX ? tex_give_way(s, want) : rt_give_way(s, lossy);
        }
    for (u32 r = g_rows.lists[kind].head, n; r && freed < want && passed < 512; r = n) {
        n = g_rows.lru[r].next;
        Surf* s = g_rows.surf[r];
        if ((i32)((u32)s->last_used - before) >= 0) break;
        if (!lossy && !lossless(s)) { /* out of the way of the next lossless eviction, held or not, in order */
            lru_unlink(r);
            lru_link(r, kind + LRU_AGED);
        } else if (lossy == VK_RECLAIM_IDLE && keeps_drawn(s))
            continue;
        else if (s->bytes > most)
            g_left_big += s->bytes;
        else if (keep_back && s->came_back)
            g_left_back += s->bytes;
        else if (vk_gfx_image_holds(s))
            passed++;
        else
            freed += kind == VK_LRU_TEX ? tex_give_way(s, want) : rt_give_way(s, lossy);
    }
    return freed;
}
u64 vk_surf_reclaim(int kind, u32 before, u64 want, int lossy) {
    VkResourceScope scope = vk_resource_begin();
    u64 freed = surf_reclaim(kind, before, want, lossy, ~0ull, 0);
    vk_resource_end(VK_RESOURCE_SURFACE_RECLAIM, scope);
    return freed;
}
/* the render targets last used before `before` that later ones drew all over (rt_superseded), from either list: the
   aged one holds those that went there with pixels memory lacked, which draws since may have made lossless */
static u64 rt_reclaim_superseded(u32 before, u64 want) {
    u64 freed = 0;
    for (int lru = VK_LRU_RT + LRU_AGED; lru >= VK_LRU_RT && freed < want; lru -= LRU_AGED)
        for (u32 r = g_rows.lists[lru].head, n; r && freed < want; r = n) {
            n = g_rows.lru[r].next;
            Surf* s = g_rows.surf[r];
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
/* the textures last used before `before` whose memory later targets drew all over since they took pixels from
   targets (tex_superseded), from either list: the aged one holds those that took pixels, which draws since may have
   made lossless */
u64 vk_tex_reclaim_superseded(u32 before, u64 want) {
    VkResourceScope scope = vk_resource_begin();
    u64 freed = 0;
    for (int lru = VK_LRU_TEX + LRU_AGED; lru >= VK_LRU_TEX && freed < want; lru -= LRU_AGED)
        for (u32 r = g_rows.lists[lru].head, n; r && freed < want; r = n) {
            n = g_rows.lru[r].next;
            Surf* s = g_rows.surf[r];
            if ((i32)((u32)s->last_used - before) >= 0) break;
            if (tex_superseded(s) && !vk_gfx_image_holds(s)) freed += surf_evict(s);
        }
    vk_resource_end(VK_RESOURCE_SURFACE_RECLAIM, scope);
    return freed;
}

/* ------------------------------------------------------------ the budget */
/* A trim that found the images short of memory, where ASTC textures are held as RGBA8 by astc_mode=1 (Exact; Auto makes
   them BC7 by itself, vk_astc_auto_trimmed) on a GPU that samples BCn but not ASTC: the log suggests astc_mode=2 (BC7)
   once a run, with the numbers (snx_astc_bc_suggest).
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
    if (!snx_astc_bc_suggest(vk_astc_bc7() || g_settings.astc_mode == 0, g_vk.astc_bc7_could, 0, astc, why)) return;
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
/* the present memory last ran short (+1: 0 is none): an image found none, a trim found too little to free, or the
   caches stayed past their budget after one */
static u32 g_short_at;
#define PRESSED_PRESENTS 300 /* how long a shortage counts as pressure (vk_mem_pressed) */
int vk_mem_pressed(void) {
    u64 usage, budget;
    if (g_short_at && g_vk_present + 1 < g_short_at + PRESSED_PRESENTS) return 1;
    vk_mem_use(&usage, &budget);
    return usage >= budget / 10 * 9;
}
/* The GPU ranges whose mapping the game changed since ended_collect last looked, in a game whose front end has no
   texture objects (one that writes its own command buffers): the unmap of a texture's memory is the one sign such a
   game gives that it ended the texture. More ranges than the list holds count as all of the address space */
static struct {
    u64 gva[16], size[16];
    u32 n;
    int all;
} g_remapped;
void vk_surf_mapping_changed(u64 gva, u64 size) {
    if (gpu_textures_added()) return; /* the front end tells of each texture's end */
    if (g_remapped.n == sizeof g_remapped.gva / sizeof *g_remapped.gva)
        g_remapped.all = 1;
    else {
        g_remapped.gva[g_remapped.n] = gva;
        g_remapped.size[g_remapped.n++] = size;
    }
}
static int ended_live(u32 r) {
    const u64 gva = g_rows.surf[r]->gva;
    return g_rows.ended_by[r] == 2 ? gmmu_translate(gva) != 0 : gpu_texture_live(gva);
}
/* row r at the head of the list of ended textures, ended as `by` says */
static void ended_link(u32 r, u8 by) {
    g_rows.ended_by[r] = by;
    g_rows.ended_at[r] = g_vk_present;
    link_before(g_rows.ended, &g_rows.ended_list, r, g_rows.ended_list.head);
}
/* the textures in those ranges whose first page is mapped no more go on the list of the ended */
static void remapped_collect(void) {
    if (!g_remapped.n && !g_remapped.all) return;
    for (int list = VK_LRU_TEX; list <= VK_LRU_TEX + LRU_AGED; list += LRU_AGED)
        for (u32 r = g_rows.lists[list].head; r; r = g_rows.lru[r].next) {
            const Surf* s = g_rows.surf[r];
            int in = g_remapped.all;
            for (u32 i = 0; i < g_remapped.n && !in; i++)
                in = s->gva < g_remapped.gva[i] + g_remapped.size[i] &&
                     g_remapped.gva[i] < s->gva + (s->size ? s->size : 1);
            if (!in || s->is_rt || g_rows.ended_by[r] || gmmu_translate(s->gva)) continue;
            ended_link(r, 2);
        }
    g_remapped.n = 0;
    g_remapped.all = 0;
}
/* the textures at the addresses whose last object ended since the last call go on the list of those */
static void ended_collect(void) {
    u64 gva[256];
    remapped_collect();
    for (u32 n; (n = gpu_textures_ended(gva, 256));) {
        for (u32 i = 0; i < n; i++)
            for (u32 r = g_rows.hash[hkey(gva[i])]; r; r = g_rows.hnext[r]) {
                const Surf* s = g_rows.surf[r];
                if (s->gva == gva[i] && !s->is_rt && !g_rows.ended_by[r] && g_rows.list[r]) ended_link(r, 1);
            }
        if (n < 256) break;
    }
}
/* up to want bytes of them freed: those memory can give again, ended and unused since before `before`. One at whose
   address the game made an object since is a texture as any other */
static u64 ended_reclaim(u64 want, u32 before) {
    u64 freed = 0;
    for (u32 r = g_rows.ended_list.head, n; r && freed < want; r = n) {
        n = g_rows.ended[r].next;
        Surf* s = g_rows.surf[r];
        if (ended_live(r))
            ended_unlink(r);
        else if ((i32)((u32)s->last_used - before) < 0 && (i32)(g_rows.ended_at[r] - before) < 0 && lossless(s) &&
                 !vk_gfx_image_holds(s))
            freed += surf_evict(s);
    }
    g_ended_freed += freed;
    return freed;
}
u64 vk_surf_ended_freed(void) { return g_ended_freed; }
/* Two seconds and 30 presents after its object ended, while four fifths of the budget are in use: before the trim
   starts, as the other pools give back what is idle then (owned.c, writelog.c, mirror.c). With memory to spare they
   stay, as any texture does, until they age out (vk_tex_frame): one game ends textures and makes them again at
   their addresses some seconds later, and with 15% of its budget in use it loaded 480 textures again in 200 s
   where it loads 49 */
void vk_surf_ended_frame(void) {
    ended_collect();
    if (!g_rows.ended_list.head) return;
    u64 usage, budget;
    vk_mem_use(&usage, &budget);
    if (usage >= budget / 5 * 4) ended_reclaim(~0ull, vk_present_before(2000000000ull, 30));
}
/* What a trim leaves free of the budget: a sixteenth of it, 128 MiB at least and 256 MiB at most, and never more
   than a quarter. The trim started at 90% of the budget and freed what had gone unused for a second down to 80%: a
   tenth of the GPU's memory emptied each time, of textures most of which a game's race asked for again within 300
   presents, 3.6 to 6.1 GB of images made again in 300 s. What the next frames allocate is a few hundred megabytes
   at most, and the allocator stops at the budget itself (devmem.c), where a failed request frees what it needs */
static u64 trim_head(u64 budget) {
    u64 head = budget / 16;
    if (head < (128ull << 20)) head = 128ull << 20;
    if (head > (256ull << 20)) head = 256ull << 20;
    return head > budget / 4 ? budget / 4 : head;
}
static u32 g_forced;    /* the present a failed allocation last forced a trim (+1) */
static u32 g_heap_swept; /* the present the heap's own use last freed the superseded targets (+1) */
static u32 g_oom;       /* the present an image allocation last found no memory (+1) */
static u64 g_oom_bytes; /* the smallest size that found none in that present */
static u32 g_oom_target; /* the same of render targets */
static u64 g_oom_target_bytes;
u64 vk_mem_trimmed(void) { return g_trimmed; }
u64 vk_mem_trim_start(u64 budget) { return budget - trim_head(budget); }
/* The caches' trim takes only what the last presents didn't use, and on a GPU of 2 GB a racing game's working set
   didn't fit: its depth targets found no memory, and most of its draws were skipped. Its terrain textures, bound by
   tiles, are the memory that can give way without a hole: a level coarser, a quarter of the bytes */
u64 vk_tex_shed(u64 need) {
    static u32 at;
    if (at == g_vk_present + 1) return 0;
    at = g_vk_present + 1;
    u64 freed = 0;
    while (freed < need) {
        Surf* big = NULL;
        for (int l = VK_LRU_TEX; l <= VK_LRU_TEX + LRU_AGED; l += LRU_AGED)
            for (u32 r = g_rows.lists[l].head; r; r = g_rows.lru[r].next) {
                Surf* t = g_rows.surf[r];
                if (vk_tex_can_coarsen(t) && (!big || t->bytes > big->bytes)) big = t;
            }
        u64 got = big ? vk_tex_coarser(big) : 0;
        if (!got) break;
        freed += got;
    }
    return freed;
}
/* Textures sparse for their levels (texture.c's shed) a level coarser, the least recently used first, a level each,
   until want bytes go: their finest level's memory, three quarters of what they hold, where an eviction took all of
   it and the next use loaded it all again. Those the present being recorded uses too, but for one a draw recorded and
   not yet submitted holds: the bind goes before the next submission, which a recorded draw would be in, sampling a
   level with no memory. A submitted draw is safe: the bind waits for the work submitted before it (memory.c). The
   working set itself has to give way: on a GPU of 2 GB a racing game's frames used more than the budget, and a
   trim that took only what two presents hadn't used freed nothing. The bytes released */
#define SHED_PER_TRIM 64 /* the textures a trim makes coarser at most: each is a sparse bind */
static u32 g_shed_at; /* the present a level last gave way (+1) */
static u64 tex_shed_lru(u32 before, u64 want) {
    u64 freed = 0;
    u32 n_shed = 0;
    for (int l = VK_LRU_TEX; l <= VK_LRU_TEX + LRU_AGED && freed < want && n_shed < SHED_PER_TRIM; l += LRU_AGED)
        for (u32 r = g_rows.lists[l].head, n; r && freed < want && n_shed < SHED_PER_TRIM; r = n) {
            n = g_rows.lru[r].next;
            Surf* t = g_rows.surf[r];
            if (before && (i32)((u32)t->last_used - before) >= 0) break;
            if (t->shed && vk_tex_can_coarsen(t) && !vk_gfx_image_holds(t)) freed += vk_tex_coarser(t), n_shed++;
        }
    if (n_shed) g_shed_at = g_vk_present + 1;
    return freed;
}
/* Once a present, the headroom of textures sparse for their levels (texture.c's shed). Memory let go comes back only
   when the frames that could read it are done, and a request in the meantime finds none: on a GPU of 2 GB a racing
   game's frames used more than the budget, the trim ran when an allocation was already short, and the sparse blocks
   of the textures it then loaded found no memory frame after frame. So while the heap's use, less what waits to be
   freed, is past the budget less two of the trim's heads, those textures give up their finest level ahead of the
   requests, the least recently used first. Under a head more below that, and once 300 presents have gone with no level
   given way, the ones the last 120 presents used take a level back, a few at a time (vk_tex_finer) */
void vk_tex_refine_frame(void) {
    u64 usage, budget;
    vk_mem_use(&usage, &budget);
    const u64 head = trim_head(budget), pending = vk_deferred_bytes();
    const u64 high = budget > 2 * head ? budget - 2 * head : 0, low = high > head ? high - head : 0;
    const u64 use = usage > pending ? usage - pending : 0;
    if (use > high) {
        tex_shed_lru(0, use - high);
        return;
    }
    if (use >= low || (g_shed_at && g_vk_present + 1 - g_shed_at < 300)) return;
    const u32 since = vk_present_before(0, 120);
    u32 took = 0, looked = 0;
    for (u32 r = g_rows.lists[VK_LRU_TEX].tail, p; r && took < 4 && looked < 256; r = p, looked++) {
        p = g_rows.lru[r].prev;
        Surf* t = g_rows.surf[r];
        if ((i32)((u32)t->last_used - since) < 0) break;
        if (t->shed && (t->lod_floor || t->sys_levels)) took += (u32)vk_tex_finer(t);
    }
}
/* Once a present: the demoted textures (vk_tex_demote) the last two presents used, the most recently used first, moved
   back into the GPU's memory, 16 MB a present at most; where the GPU's memory has no room for one, textures no present
   used in the last 30 go into system memory in its place, the least recently used first. Waiting for room alone, the
   GPU's memory never had any in a racing game's race, and the textures its frames used stayed in system memory,
   sampled over the bus. The use counted is less what waits to be freed: what a swap lets go is the GPU's again when
   the frame is done, and the move back that finds no memory now is tried again at the next present */
/* room within high for bytes more of the GPU's memory, its use *use: textures no present used since cold moved into
   system memory, the least recently used first, from row *coldr on (0: none). 1 when it fits */
static int room_for(u64* use, u64 bytes, u64 high, u32* coldr, u32 cold) {
    while (*use + bytes > high && *coldr) {
        Surf* c = g_rows.surf[*coldr];
        *coldr = g_rows.lru[*coldr].next;
        if ((i32)((u32)c->last_used - cold) >= 0) {
            *coldr = 0; /* the rest were used lately */
            break;
        }
        if (c->demoted || vk_gfx_image_holds(c)) continue;
        const u64 got = vk_tex_demote(c);
        *use = *use > got ? *use - got : 0;
    }
    return *use + bytes <= high;
}
void vk_tex_promote_frame(void) {
    if (!g_settings.texture_system_memory) return;
    u64 usage, budget;
    vk_mem_use(&usage, &budget);
    const u64 head = trim_head(budget), high = budget > head ? budget - head : 0, pending = vk_deferred_bytes();
    u64 use = usage > pending ? usage - pending : 0, moved = 0;
    const u32 hot = vk_present_before(0, 2), cold = vk_present_before(0, 30);
    u32 looked = 0, coldr = g_rows.lists[VK_LRU_TEX].head;
    for (u32 r = g_rows.lists[VK_LRU_TEX].tail, p; r && looked < 256 && moved < (16ull << 20); r = p, looked++) {
        p = g_rows.lru[r].prev;
        Surf* t = g_rows.surf[r];
        if ((i32)((u32)t->last_used - hot) < 0) break;
        if (!t->demoted) continue;
        if (!room_for(&use, t->sys_bytes, high, &coldr, cold)) break;
        const u64 b = t->sys_bytes;
        if (vk_tex_promote(t)) moved += b, use += b;
    }
}
/* Once a present, the render targets in system memory (rt_demote): those the last two presents used go back into the
   GPU's memory as the demoted textures do (vk_tex_promote_frame), the most recently used first, while it has room.
   Every 30 presents, while the heap they went to is past its own budget less a head, the least recently used of the
   rest go, their pixels with them: that memory is the system's, which the texture tier and the mirror's granules
   take too, and the targets would otherwise keep all of it */
void vk_rt_sys_frame(void) {
    if (!g_rt_sys.n) return;
    u64 usage, budget;
    vk_mem_use(&usage, &budget);
    const u64 head = trim_head(budget), high = budget > head ? budget - head : 0, pending = vk_deferred_bytes();
    u64 use = usage > pending ? usage - pending : 0, moved = 0;
    const u32 hot = vk_present_before(0, 2), cold = vk_present_before(0, 30);
    u32 looked = 0, coldr = g_settings.texture_system_memory ? g_rows.lists[VK_LRU_TEX].head : 0;
    for (u32 r = g_rows.lists[VK_LRU_RT].tail, p; r && looked < 256 && moved < (16ull << 20); r = p, looked++) {
        p = g_rows.lru[r].prev;
        Surf* s = g_rows.surf[r];
        if ((i32)((u32)s->last_used - hot) < 0) break;
        if (!s->demoted || vk_gfx_image_holds(s)) continue;
        if (!room_for(&use, s->sys_bytes, high, &coldr, cold)) break;
        const u64 b = s->sys_bytes;
        if (rt_promote(s)) moved += b, use += b;
    }
    if (g_vk_present % 30 || !g_rt_sys.heap) return;
    DevMemBudget heaps[VK_MAX_MEMORY_HEAPS];
    vk_devmem_budgets(heaps);
    const DevMemBudget* sys = &heaps[g_rt_sys.heap - 1];
    const u64 sys_high = sys->budget - trim_head(sys->budget);
    if (sys->usage <= sys_high) return;
    const u64 over = sys->usage - sys_high;
    u64 freed = 0;
    u32 went = 0;
    for (int lru = VK_LRU_RT + LRU_AGED; lru >= VK_LRU_RT && freed < over; lru -= LRU_AGED)
        for (u32 r = g_rows.lists[lru].head, n; r && freed < over; r = n) {
            n = g_rows.lru[r].next;
            Surf* s = g_rows.surf[r];
            if (!s->demoted || (i32)((u32)s->last_used - hot) >= 0 || vk_gfx_image_holds(s)) continue;
            freed += s->sys_bytes;
            went++;
            g_rt_sys.lost++;
            g_rt_sys.lost_bytes += s->sys_bytes;
            surf_evict(s);
        }
    if (went)
        LOG_LIMIT(8,
                  "vulkan: system memory heap %u has %llu MB in use of a budget of %llu MB, past it less a head: %u "
                  "render targets moved there went, %llu MB, with pixels memory lacks\n",
                  g_rt_sys.heap - 1, (unsigned long long)(sys->usage >> 20), (unsigned long long)(sys->budget >> 20),
                  went, (unsigned long long)(freed >> 20));
}
/* vk_surf_reclaim with the most bytes an entry may have */
static u64 trim_reclaim(int kind, u32 before, u64 want, int lossy, u64 most, int keep_back) {
    VkResourceScope scope = vk_resource_begin();
    u64 freed = surf_reclaim(kind, before, want, lossy, most, keep_back);
    vk_resource_end(VK_RESOURCE_SURFACE_RECLAIM, scope);
    return freed;
}
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
    const u64 head = trim_head(budget);
    if (force) { /* the allocation's size, less what evictions before it are releasing already: not what moves
                    release, which goes back to a block no request takes while it is emptied */
        u64 gone = vk_deferred_bytes() - vk_deferred_moved();
        need = extra > gone ? extra - gone : 0;
    } else if (usage <= vk_mem_trim_start(budget)) {
        /* The caches count the free space inside the blocks as room, but a request takes only a run of its own kind
           and size, and the driver's heap can be at its budget while that space is in pieces no request takes. Then
           the render targets later ones drew all over go after two presents, once a present, though the hold below
           would keep them for a second: they are lossless. A game at a dynamic resolution that places its frame's
           targets in one pool, at offsets that move each frame, made about 30 targets a frame at 30 frames a
           second. A second of them held 5 to 8 GB of a 14.6 GB budget while the caches counted 3 GB of room in the
           blocks, so no trim ran until an image found no memory, every second and a half: the forced trim then freed
           5 to 8 GB of targets, too late for that image, and its draws went without it. The textures that took
           targets' pixels where later targets drew all over go with them (tex_superseded) */
        if (g_heap_swept != g_vk_present + 1 && vk_mem_heap_use() + extra > vk_mem_trim_start(budget)) {
            g_heap_swept = g_vk_present + 1;
            g_trimmed += vk_rt_reclaim_superseded(vk_present_before(0, 2), ~0ull);
            g_trimmed += vk_tex_reclaim_superseded(vk_present_before(0, 2), ~0ull);
        }
        return;
    }
    SNX_ZONE_V(z, "vk mem trim", force);
    /* What goes first is what can be had again without loss: textures (loaded again from memory), own buffers (copied
       again), then render targets memory holds. Those unused for a second go down to two heads under the budget
       (trim_head), room for what comes next. The working set itself (unused for eight presents) goes only to stay
       half a head under it: freeing it further when it alone is bigger made one game load hundreds of textures and
       targets again every 600 presents under a lowered budget, where staying near the budget costs nothing. Two
       presents took textures a game uses every few frames: of 3,000 it evicted in one game's menus, 2,358 had gone
       unused for three presents, and each came back at once, a BC7 encode each (astc.c). Render targets whose pixels
       exist only here (memory has older ones: draws aren't written back, see vk_surf_reclaim) and textures that took
       such pixels lose them when they go, so they go only when the lossless steps leave the heap within a head of
       the budget: those unused for five seconds, then, within half a head, those unused for a second and eight
       presents, the youngest a lossy entry can be. Every step frees the least recently used first. Before them all
       go the render targets later ones drew all over, unused for two presents (for a second and eight where the
       driver moves memory, below), and the textures that took their pixels from targets and whose memory later ones
       drew all over: what they hold is no one's (rt_superseded, tex_superseded), and the textures whose texture object
       the game ended (ended_reclaim) */
    /* Where the driver moves memory by priority (g_vk.has_pageable), a trim for headroom leaves those targets as long
       as the sweep every 30 presents does, a second and eight presents: they have the highest priority, so where the
       GPU's memory runs short the driver moves textures to system memory first. Freed after two presents, they were
       made again: a game at a dynamic resolution draws its scene to targets of a new size each frame, and under a
       2 GB budget, where a trim ran each present, 60 and 85% of the targets made in its two busiest spans of 600
       presents (21% in a quieter one) had the shape of one freed in the 120 presents before. Each was a new image
       and, for the large ones, memory of its own: a call to the driver of 0.64 ms where it moves memory, 0.29 ms
       where it doesn't. A trim for an allocation (force) still takes them after two presents */
    const u32 superseded_before = g_vk.has_pageable && !force ? vk_present_before(1000000000ull, 8)
                                                               : vk_present_before(0, 2);
    u64 superseded = vk_rt_reclaim_superseded(superseded_before, ~0ull);
    const u64 tex_superseded_bytes = vk_tex_reclaim_superseded(superseded_before, ~0ull);
    static const struct {
        u32 ms, presents;
        int lossy;
        u32 mark; /* the halves of trim_head below the budget that the step frees down to */
    } steps[] = {{1000, 8, 0, 4}, {0, 8, 0, 1}, {5000, 30, 1, 2}, {1000, 8, 1, 1}};
    u64 freed = superseded + tex_superseded_bytes, by[3] = {tex_superseded_bytes, 0, superseded}, working = 0,
        step_big = 0, step_back = 0;
    int short_of = 0;
    /* first the textures the game has ended, down to the lowest mark: no later frame asks for them */
    ended_collect();
    {
        const u64 low = budget - 2 * head, left = usage > freed ? usage - freed : 0;
        u64 want = left > low ? left - low : 0;
        if (want < need) want = need;
        /* under the budget's pressure, after two presents: a draw recorded before the object ended may hold one */
        const u64 got = want ? ended_reclaim(want, vk_present_before(0, 2)) : 0;
        by[0] += got;
        freed += got;
    }
    /* then the textures sparse for their levels, a level each, down to the first step's mark: what the steps would
       evict, smaller and still there */
    {
        const u64 low = budget - steps[0].mark * (head / 2), left = usage > freed ? usage - freed : 0;
        u64 want = left > low ? left - low : 0;
        if (want < need) want = need;
        const u64 got = want ? tex_shed_lru(0, want) : 0;
        by[0] += got;
        freed += got;
    }
    for (u32 i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        u64 mark = budget - steps[i].mark * (head / 2), left = usage > freed ? usage - freed : 0;
        u64 want = left > mark ? left - mark : 0;
        if (freed < need && want < need - freed) want = need - freed;
        if (!want) continue;
        u32 before = vk_present_before((u64)steps[i].ms * 1000000ull, steps[i].presents);
        int level = steps[i].lossy ? VK_RECLAIM_ANY : VK_RECLAIM_LOSSLESS;
        /* A step for headroom takes no entry of more than twice what it wants: it would free far past its mark, and
           what goes is what the game still uses now and then. In a game under a 2 GB budget, a step that wanted 161 MB
           took its 650 MiB texture array, which the game asked for again 0.4 s later. Making it again took a trim of
           735 MB, and 4,100 textures were loaded again in the 30 s after it, where a run without that eviction loaded
           160 to 270. What an allocation needs (force) takes any entry */
        const u64 most = freed < need ? ~0ull : 2 * want;
        /* The first step, which frees idle entries down to two heads under the budget for headroom, leaves the
           textures the game came back for after an eviction: it asks for them again, and each costs a load (and a
           BC7 encode for an ASTC one). The working-set and lossy steps, which run only while the heap stays past
           their marks, take them, as does a trim for an allocation (force) */
        const int keep_back = i == 0 && freed >= need;
        /* each step walks the lists from their start again, passing over the same entries: the line tells the most
           one step passed over */
        const u64 big_before = g_left_big, back_before = g_left_back;
        u64 got = trim_reclaim(VK_LRU_TEX, before, want, level, most, keep_back);
        by[0] += got;
        if (got < want && !steps[i].lossy) {
            u64 n = vk_buf_reclaim(before, want - got);
            by[1] += n;
            got += n;
        }
        if (got < want) {
            u64 n = trim_reclaim(VK_LRU_RT, before, want - got, level, most, 0);
            by[2] += n;
            got += n;
        }
        freed += got;
        if (g_left_big - big_before > step_big) step_big = g_left_big - big_before;
        if (g_left_back - back_before > step_back) step_back = g_left_back - back_before;
        g_left_big = big_before;
        g_left_back = back_before;
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
    vk_astc_auto_trimmed(usage, snx_astc_trim_left(usage, extra, freed, force), budget, force);
    if (short_of) g_trim_dry = g_vk_present + 1;
    if (force || short_of || (usage > freed ? usage - freed : 0) > budget) g_short_at = g_vk_present + 1;
    g_left_big = step_big;
    g_left_back = step_back;
    char left_big[160] = "";
    if (g_left_big || g_left_back)
        snprintf(left_big, sizeof left_big,
                 "; left %llu MB of entries over twice what their step wanted and %llu MB of textures the game came "
                 "back for",
                 (unsigned long long)(g_left_big >> 20), (unsigned long long)(g_left_back >> 20));
    if (freed || short_of)
        LOG_LIMIT(20,
                  "vulkan: %llu of %llu MB budgeted in use%s: freed %llu MB of textures, %llu of buffers, %llu of "
                  "render targets%s%s\n",
                  (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20),
                  force ? " (an allocation found no memory)" : "", (unsigned long long)(by[0] >> 20),
                  (unsigned long long)(by[1] >> 20), (unsigned long long)(by[2] >> 20),
                  short_of ? ", all that could go" : "", left_big);
    g_left_big = g_left_back = 0;
}

/* the system's word (vk_mem_release_ask): asked, its words, and once run what went and the blocks before, for the line
   four presents later */
static struct {
    _Atomic int asked;
    PMutex lock; /* why, between the thread that asks and the renderer's */
    char why[160], said[160];
    u64 by[3], blocks; /* what went (textures, buffers, render targets), and the blocks before */
    u64 bc7;           /* the BC7 cache's blocks that went, the system's memory */
    u32 present;       /* when it ran (+1), 0 none to tell */
    u32 runs;
} g_release = {.lock = PMUTEX_INIT};
u32 vk_mem_release_stats(u64 by[3]) {
    for (int k = 0; k < 3; k++) by[k] = g_release.by[k];
    return g_release.runs;
}
void vk_mem_release_ask(const char* why) {
    pm_lock(&g_release.lock);
    snprintf(g_release.why, sizeof g_release.why, "%s", why);
    pm_unlock(&g_release.lock);
    atomic_store_explicit(&g_release.asked, 1, memory_order_release);
}
void vk_mem_release_run(void) {
    if (g_release.present && g_vk_present - (g_release.present - 1) >= 4) {
        LOG_LIMIT(20,
                  "vulkan: %s: %llu MB of textures, %llu of buffers and %llu of render targets that the two presents "
                  "before didn't use went, and the renderer's memory blocks went from %llu to %llu MB; the BC7 cache "
                  "let %llu MB of the system's memory go\n",
                  g_release.said, (unsigned long long)(g_release.by[0] >> 20),
                  (unsigned long long)(g_release.by[1] >> 20), (unsigned long long)(g_release.by[2] >> 20),
                  (unsigned long long)(g_release.blocks >> 20), (unsigned long long)(vk_memory_blocks() >> 20),
                  (unsigned long long)(g_release.bc7 >> 20));
        g_release.present = 0;
    }
    if (!atomic_exchange_explicit(&g_release.asked, 0, memory_order_acq_rel)) return;
    pm_lock(&g_release.lock);
    snprintf(g_release.said, sizeof g_release.said, "%s", g_release.why);
    pm_unlock(&g_release.lock);
    /* what can be had again without loss, as vk_mem_trim's lossless steps take it, whatever the budget: render
       targets and textures later targets drew over, then textures, own buffers and render targets unused for two
       presents */
    u32 before = vk_present_before(0, 2);
    g_release.blocks = vk_memory_blocks();
    g_release.by[2] = vk_rt_reclaim_superseded(before, ~0ull);
    g_release.by[0] = vk_tex_reclaim_superseded(before, ~0ull);
    g_release.by[0] += vk_surf_reclaim(VK_LRU_TEX, before, ~0ull, VK_RECLAIM_LOSSLESS);
    g_release.by[1] = vk_buf_reclaim(before, ~0ull);
    g_release.by[2] += vk_surf_reclaim(VK_LRU_RT, before, ~0ull, VK_RECLAIM_LOSSLESS);
    g_release.bc7 = vk_bc7_cache_release(before); /* the system's memory, not the renderer's: not in g_trimmed */
    g_trimmed += g_release.by[0] + g_release.by[1] + g_release.by[2];
    g_release.present = g_vk_present + 1;
    g_release.runs++;
}

void vk_surf_bytes(Surf* s, u64 bytes) {
    if (s->row && g_rows.list[s->row]) {
        const int kind = lru_kind(s->row);
        g_held[kind].bytes = g_held[kind].bytes - s->bytes + bytes;
    }
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
    /* one as big of its kind found no memory this present: neither will this one (what was freed meanwhile is released
       when the frame is done). Its draws go without it until the next present. Render targets are counted apart: in
       a racing game on a GPU of 2 GB, textures of 66 KB to 1.6 MB found none in each present, and its depth targets
       of 2 to 8 MB, refused for them, skipped 500,000 draws in a minute and a half where 220 found no memory. So are
       the images its compute programs store to (VK_OWN_WRITTEN), whose loss is a dispatch's results: refused about
       20 times for each that found no memory, and each refusal skipped the dispatch */
    const int target = (owner & ~(VK_OWN_WRITTEN | VK_OWN_MOVES)) == VK_OWN_TARGET || (owner & VK_OWN_WRITTEN);
    owner &= ~VK_OWN_WRITTEN; /* VK_OWN_MOVES goes on to vk_image_create_r */
    u32* oom = target ? &g_oom_target : &g_oom;
    u64* oom_bytes = target ? &g_oom_target_bytes : &g_oom_bytes;
    if (*oom == g_vk_present + 1 && *bytes >= *oom_bytes) return 0;
    vk_mem_trim(*bytes, 0);
    if (vk_image_create_r(ii, img, mem, owner, &res)) return 1;
    if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY && target) { /* the memory held back for it (vk_devmem_reserve) */
        vk_devmem_reserve_use(1);
        int ok = vk_image_create_r(ii, img, mem, owner, &res);
        vk_devmem_reserve_use(0);
        if (ok) return 1;
    }
    if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        if (*oom != g_vk_present + 1 || *bytes < *oom_bytes) *oom_bytes = *bytes;
        *oom = g_vk_present + 1;
        vk_tex_shed(*bytes);
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
    vk_surf_use(cb, source, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, destination, VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
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
        rec_CmdCopyImageToBuffer(cb, source->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), scratch, 1, &read);
        VkBufferCopy copy = {table_offset, table_at, table_size};
        rec_CmdCopyBuffer(cb, table_buffer, scratch, 1, &copy);
        vk_all_barrier_by(VK_BAR_SURFACE, cb);
        if (!vk_tc_blit(cb, bytes, 0, width * bytes, height, scratch, used, 0, input_pitch, table_at,
                        table_at + 4ull * width, scratch, output_at, output_pitch))
            return 0;
        vk_all_barrier_by(VK_BAR_SURFACE, cb);
        VkBufferImageCopy write = {.bufferOffset = output_at,
                                   .bufferRowLength = output_pitch / bytes,
                                   .imageSubresource = {bit, 0, 0, 1},
                                   .imageOffset = {dx, dy, 0},
                                   .imageExtent = {width, height, 1}};
        rec_CmdCopyBufferToImage(cb, scratch, destination->img, VK_LAYOUT(TRANSFER_DST_OPTIMAL), 1, &write);
        vk_all_barrier_by(VK_BAR_SURFACE, cb);
    }
    return 1;
}

/* A 2D blit whose source is a depth target: the game copies the depth buffer's texels as a color surface of their
   size (one game blits its D24S8 depth as BGRA8 into another depth buffer, which its lighting tests
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
    vk_surf_use(cb, st, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, dt, VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (same) { /* the region both sides hold */
        u32 w = (u32)(x1 - x0 < X1 - dx ? x1 - x0 : X1 - dx), h = (u32)(y1 - y0 < Y1 - dy ? y1 - y0 : Y1 - dy);
        VkImageCopy c = {
            .srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT | (st->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), 0, 0, 1},
            .srcOffset = {x0, y0, 0},
            .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT | (st->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), 0, 0, 1},
            .dstOffset = {dx, dy, 0},
            .extent = {w, h, 1}};
        rec_CmdCopyImage(cb, st->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), dt->img,
                         VK_LAYOUT(TRANSFER_DST_OPTIMAL), 1, &c);
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
        rec_CmdBlitImage(cb, st->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), dt->img,
                         VK_LAYOUT(TRANSFER_DST_OPTIMAL), st->f.stencil ? 2 : 1, b, VK_FILTER_NEAREST);
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
    vk_surf_use(cb, st, VK_LAYOUT(DEPTH_STENCIL_ATTACHMENT_OPTIMAL), rs,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    vk_barrier(cb, tmp, aspect, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, rs,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_LAYOUT(DEPTH_STENCIL_ATTACHMENT_OPTIMAL));
    VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                     .imageView = sv,
                                     .imageLayout = VK_LAYOUT(DEPTH_STENCIL_ATTACHMENT_OPTIMAL),
                                     .resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
                                     .resolveImageView = tv,
                                     .resolveImageLayout = VK_LAYOUT(DEPTH_STENCIL_ATTACHMENT_OPTIMAL),
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
               VK_LAYOUT(DEPTH_STENCIL_ATTACHMENT_OPTIMAL), VK_PIPELINE_STAGE_2_BLIT_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT, VK_LAYOUT(TRANSFER_SRC_OPTIMAL));
    vk_surf_use(cb, dt, VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    /* one aspect a region; nearest, as filtering depth and stencil would only mix them */
    VkImageBlit b[2] = {{.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                         .srcOffsets = {{x0, y0, 0}, {x1, y1, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                         .dstOffsets = {{dx, dy, 0}, {X1, Y1, 1}}}};
    b[1] = b[0];
    b[1].srcSubresource.aspectMask = b[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    rec_CmdBlitImage(cb, tmp, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), dt->img, VK_LAYOUT(TRANSFER_DST_OPTIMAL),
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
    vk_surf_use(cb, st, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), reads, VK_ACCESS_2_TRANSFER_READ_BIT);
    if (st->msaa && !scaled && x1 > x0 && y1 > y0 && dt->f.vk == st->f.vk) {
        vk_surf_use(cb, dt, VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_RESOLVE_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageResolve r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .srcOffset = {x0, y0, 0},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .dstOffset = {dx, dy, 0},
                            .extent = {(u32)(x1 - x0), (u32)(y1 - y0), 1}};
        rec_CmdResolveImage(cb, st->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), dt->img,
                            VK_LAYOUT(TRANSFER_DST_OPTIMAL), 1, &r);
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
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_LAYOUT(TRANSFER_DST_OPTIMAL));
            if (st->msaa) {
                i32 left = x0 < x1 ? x0 : x1, top = y0 < y1 ? y0 : y1;
                VkImageResolve r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .srcOffset = {left, top, 0},
                                    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .dstOffset = {left, top, 0},
                                    .extent = {(u32)abs(x1 - x0), (u32)abs(y1 - y0), 1}};
                rec_CmdResolveImage(cb, st->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), src,
                                    VK_LAYOUT(TRANSFER_DST_OPTIMAL), 1, &r);
            } else {
                VkImageCopy r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .extent = {source_w, source_h, 1}};
                rec_CmdCopyImage(cb, st->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), src,
                                 VK_LAYOUT(TRANSFER_DST_OPTIMAL), 1, &r);
            }
            vk_barrier(cb, src, VK_IMAGE_ASPECT_COLOR_BIT, reads, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_LAYOUT(TRANSFER_SRC_OPTIMAL));
        }
        vk_surf_use(cb, dt, VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .srcOffsets = {{x0, y0, 0}, {x1, y1, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .dstOffsets = {{dx, dy, 0}, {X1, Y1, 1}}};
        int linear = (filter < 0 ? scaled : filter != 0) && !st->f.is_int &&
                     blit_ok(st->f.vk, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        rec_CmdBlitImage(cb, src, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), dt->img, VK_LAYOUT(TRANSFER_DST_OPTIMAL),
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
        while (g_rows.hash[k]) surf_evict(g_rows.surf[g_rows.hash[k]]);
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
        for (u32 r = g_rows.lists[list].head; r; r = g_rows.lru[r].next) n += flush_wanted(g_rows.surf[r]);
    if (!n || !(all = (Surf**)malloc(sizeof *all * n))) return 0;
    n = 0;
    for (list = VK_LRU_RT; list <= VK_LRU_RT + LRU_AGED; list += LRU_AGED)
        for (u32 r = g_rows.lists[list].head; r; r = g_rows.lru[r].next)
            if (flush_wanted(g_rows.surf[r])) all[n++] = g_rows.surf[r];
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
/* The bytes a layer of an aspect of s takes in the copies' buffer, rounded up to 4: a depth or stencil copy's buffer
   offset is a multiple of 4 (VUID-vkCmdCopyImageToBuffer-srcImage-07978, VUID-vkCmdCopyBufferToImage-dstImage-07978),
   and a layer of the stencil's byte a texel, or of D16's two, ends off it where the texels are odd. The state holds
   each layer's bytes alone, one after another (zt_put, zt_restore) */
static u64 zt_layer(const Surf* s, u32 aspect) { return (u64)s->sw * s->sh * (aspect ? 1u : zt_depth_bytes(s->f.vk)); }
static u64 zt_layer_padded(const Surf* s, u32 aspect) { return (zt_layer(s, aspect) + 3) & ~3ull; }
/* the copies' bytes of every aspect and layer of s */
static u64 zt_padded(const Surf* s) {
    return s->layers * (zt_layer_padded(s, 0) + (zt_stencil(s->f.vk) ? zt_layer_padded(s, 1) : 0));
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
/* each aspect of every layer of s copied between its image and buf at at (a multiple of 4), the depth's then the
   stencil's, each layer zt_layer_padded bytes: to the buffer (out) or from it */
static void zt_copy(VkCommandBuffer cb, Surf* s, VkBuffer buf, u64 at, int out) {
    u32 layer, a, aspects = zt_stencil(s->f.vk) ? 2 : 1;
    for (a = 0; a < aspects; a++)
        for (layer = 0; layer < s->layers; layer++) {
            VkBufferImageCopy c = {
                .bufferOffset = at,
                .imageSubresource = {a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 1},
                .imageExtent = {s->sw, s->sh, 1}};
            if (out)
                rec_CmdCopyImageToBuffer(cb, s->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), buf, 1, &c);
            else
                rec_CmdCopyBufferToImage(cb, buf, s->img, VK_LAYOUT(TRANSFER_DST_OPTIMAL), 1, &c);
            at += zt_layer_padded(s, a);
        }
}
/* the layers of the copies' bytes at p, each aspect's in turn, into the state: the bytes of each layer alone */
static int zt_put(StateIo* io, const Surf* s, const u8* p) {
    u32 aspects = zt_stencil(s->f.vk) ? 2 : 1;
    for (u32 a = 0; a < aspects; a++)
        for (u32 layer = 0; layer < s->layers; layer++) {
            if (io_put(io, p, zt_layer(s, a))) return -1;
            p += zt_layer_padded(s, a);
        }
    return 0;
}
int vk_zt_save(StateIo* io) {
    u64 total = 0, at = 0;
    u32 n = 0, i;
    int list, r = 0;
    Surf** all;
    VkBuffer buf;
    u8* p;
    for (list = VK_LRU_RT; list <= VK_LRU_RT + LRU_AGED; list += LRU_AGED)
        for (u32 row = g_rows.lists[list].head; row; row = g_rows.lru[row].next) {
            const Surf* s = g_rows.surf[row];
            if (zt_saved(s)) {
                n++;
                total += zt_padded(s);
            }
        }
    if (!n) return io_put32(io, 0);
    if (!(all = (Surf**)malloc(sizeof *all * n))) return -1;
    n = 0;
    for (list = VK_LRU_RT; list <= VK_LRU_RT + LRU_AGED; list += LRU_AGED)
        for (u32 row = g_rows.lists[list].head; row; row = g_rows.lru[row].next)
            if (zt_saved(g_rows.surf[row])) all[n++] = g_rows.surf[row];
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
            vk_surf_use(cb, s, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
            zt_copy(cb, s, buf, at, 1);
            at += zt_padded(s);
        }
        rec_CmdPipelineBarrier2(cb, &di);
    }
    vk_finish();
    vk_readback_ready();
    r |= io_put32(io, n);
    for (i = 0, at = 0; i < n && !r; i++) {
        Surf* s = all[i];
        u64 dsize = zt_layer(s, 0) * s->layers, ssize = zt_stencil(s->f.vk) ? zt_layer(s, 1) * s->layers : 0;
        r |= io_put64(io, s->gva) | io_put32(io, s->w) | io_put32(io, s->h) | io_put32(io, s->sw) | io_put32(io, s->sh);
        r |= io_put32(io, s->fkey) | io_put32(io, (u32)s->f.vk) | io_put32(io, s->layers) | io_put64(io, dsize) |
             io_put64(io, ssize) | zt_put(io, s, p + at);
        at += zt_padded(s);
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
            z->dsize != zt_layer(s, 0) * s->layers ||
            z->ssize != (zt_stencil(s->f.vk) ? zt_layer(s, 1) * s->layers : 0))
            continue;
        if ((to = vk_stage(zt_padded(s), 4, &buf, &off)) != NULL) {
            /* each layer's bytes where zt_copy reads it */
            const u8* from = z->bytes;
            for (u32 a = 0; a < (zt_stencil(s->f.vk) ? 2u : 1u); a++)
                for (u32 layer = 0; layer < s->layers; layer++) {
                    memcpy(to, from, (size_t)zt_layer(s, a));
                    from += zt_layer(s, a);
                    to += zt_layer_padded(s, a);
                }
            vk_render_end();
            cb = vk_cmd();
            vk_surf_use(cb, s, VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
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
