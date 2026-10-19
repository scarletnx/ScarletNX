/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Render targets by guest GPU address, in a table textures share (gpu/vk/texture.c). A render target lives on the
   GPU; vk_rt_flush writes it back to guest memory when the CPU side needs it (DMA and 2D copies), vk_rt_reload reads
   it again after the CPU side wrote that memory. Games reuse one address for render targets of different sizes and
   formats within a frame: a target whose address is taken by another is parked and reused when its size and format
   come back. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"

#define SURFH 4096
static Surf* g_surfh[SURFH];
static Surf* g_rt_first;
static _Thread_local int g_rt_flushing;
static u32 hkey(u64 a) { return (u32)((a >> 8) ^ (a >> 20)) & (SURFH - 1); }

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
            vk_barrier(cb, s->img, vk_aspect(&s->f), s->stage ? s->stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       layout, stage, access, layout);
        }
        s->stage |= stage;
        s->access |= access;
        return;
    }
    vk_render_end(); /* no barriers on images inside a rendering pass */
    vk_barrier(cb, s->img, vk_aspect(&s->f), s->stage ? s->stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, s->access,
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

static Surf* surf_create(u64 gva, u32 w, u32 h, VkFmt f, u32 fkey, u32 bl, u32 bh, u32 pitch, int is_rt, u32 layers) {
    Surf* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->gva = gva;
    s->w = w;
    s->h = h;
    s->sw = is_rt ? vk_sc(w) : w;
    s->sh = is_rt ? vk_sc(h) : h;
    s->layers = layers;
    s->levels = 1;
    s->fkey = fkey;
    s->f = f;
    s->bl = bl;
    s->bh = bh;
    s->pitch = pitch;
    s->is_rt = is_rt;
    if (is_rt) s->layer_size = bl ? bl_size(w * f.bpp, h, bh) : (u64)pitch * h;
    s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (is_rt) usage |= f.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkFormatProperties format_props;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f.vk, &format_props);
    if (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        s->storage_capable = 1;
    }
    s->type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = f.vk,
                            .extent = {s->sw, s->sh, 1},
                            .mipLevels = 1,
                            .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkViewFormats vf; /* sRGB and linear views */
    vk_view_formats(&ii, &vf);
    /* a scaled target can pass the allocation cap (4x of 4K is 506 MiB in RGBA8) */
    if (!vk_image_create(&ii, &s->img, &s->mem)) {
        free(s);
        return NULL;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = s->img,
                                .viewType = s->type,
                                .format = f.vk,
                                .subresourceRange = {vk_aspect(&f), 0, 1, 0, layers}};
    VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &s->att));
    u32 k = hkey(gva);
    s->next = g_surfh[k];
    g_surfh[k] = s;
    if (is_rt) {
        s->rt_next = g_rt_first;
        g_rt_first = s;
    }
    return s;
}

/* SET_SRGB_WRITE selects the attachment view, leaving the image's stored bytes and sampling format intact. */
VkImageView vk_rt_attachment(Surf* s, int srgb_write) {
    VkFormat linear = vk_srgb_as(s->f.vk, 0);
    if (srgb_write || linear == s->f.vk) return s->att;
    if (!s->att_linear) {
        VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .pNext = &usage,
                                    .image = s->img,
                                    .viewType = s->type,
                                    .format = linear,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, s->layers}};
        VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &s->att_linear));
    }
    return s->att_linear;
}

Surf* vk_surf_first(u64 gva) { return g_surfh[hkey(gva)]; }
void vk_surf_add(Surf* s) {
    u32 k = hkey(s->gva);
    s->next = g_surfh[k];
    g_surfh[k] = s;
}

Surf* vk_rt_find(u64 gva) {
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == gva && s->is_rt) return s;
    return NULL;
}

VkImage vk_unscaled_image(const Surf* s, u32 layers, VmaAllocation* mem) {
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
    if (s->sw == s->w && s->sh == s->h) return 1;
    if (blit_ok(s->f.vk, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) return 1;
    LOG_LIMIT(1, "vulkan: format %d can't be scaled: render targets in it don't move to or from guest memory\n",
              (int)s->f.vk);
    return 0;
}
/* src (TRANSFER_SRC) scaled into dst (TRANSFER_DST), whole images */
static void scale_blit(VkCommandBuffer cb, VkImage src, u32 sw, u32 sh, u32 src_layer, VkImage dst, u32 dw, u32 dh,
                       u32 dst_layer, int is_int) {
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, src_layer, 1},
                     .srcOffsets = {{0, 0, 0}, {(i32)sw, (i32)sh, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, dst_layer, 1},
                     .dstOffsets = {{0, 0, 0}, {(i32)dw, (i32)dh, 1}}};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   is_int ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
}

/* the guest bytes of a render target: block linear, or pitch linear (its last row ends after the row's bytes, not
   after a whole pitch) */
static u64 rt_layer_bytes(const Surf* s) {
    u64 row = (u64)s->w * s->f.bpp;
    return s->bl ? bl_size((u32)row, s->h, s->bh) : (u64)s->pitch * (s->h - 1) + row;
}
static u64 rt_bytes(const Surf* s) {
    if (s->volume && s->bl) {
        u64 slice = rt_layer_bytes(s);
        u32 depth = (s->layers + (1u << s->bd) - 1) & ~((1u << s->bd) - 1);
        return slice * depth;
    }
    return s->layer_size * (s->layers - 1) + rt_layer_bytes(s);
}
u64 vk_surf_guest_size(const Surf* s) { return s ? rt_bytes(s) : 0; }
int vk_surf_can_writeback(const Surf* s) { return s && s->f.raw && !s->f.depth && moves_scaled(s); }

void vk_rt_reload(u64 gva) {
    Surf* s = vk_rt_find(gva);
    if (!s) return;
    s->wgen++; /* the copies of it (crops) are out of date */
    if (s->f.depth || !s->f.raw || !moves_scaled(s)) {
        /* new memory over a target we can't load as is: memory is the truth. Its contents are undefined until it is
           drawn to again (textures over it are loaded from memory meanwhile, and it isn't presented); the stage and
           access stay for the next barrier to wait on */
        vk_render_end();
        s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        s->draw_wseen = 0;
        s->gw = gpu_mem_generation();
        return;
    }
    u64 sz = rt_bytes(s), row = (u64)s->w * s->f.bpp;
    u8* tmp = NULL;
    const u8* src = gmem_ptr(gva, sz);
    if (!src) {
        tmp = malloc(sz);
        if (!tmp) return;
        gmem_read(gva, tmp, sz);
        src = tmp;
    }
    vk_render_end();
    VkBuffer buf;
    u64 off;
    u64 slice = row * s->h;
    u8* p = vk_stage(slice * s->layers, s->f.bpp, &buf, &off);
    if (p) {
        for (u32 layer = 0; layer < s->layers; layer++) {
            const u8* layer_src = s->volume && s->bl ? src : src + (u64)layer * s->layer_size;
            u8* layer_dst = p + (u64)layer * slice;
            if (s->volume && s->bl) {
                u32 bh = bl_height(s->h, s->bh);
                for (u32 y = 0; y < s->h; y++)
                    for (u32 x = 0; x < row; x += 16) {
                        u32 n = row - x < 16 ? (u32)(row - x) : 16;
                        memcpy(layer_dst + y * row + x, src + bl_offset(x, y, layer, (u32)row, s->h, bh, s->bd), n);
                    }
            } else if (s->bl)
                deswizzle(layer_dst, (u32)row, layer_src, (u32)row, s->h, bl_height(s->h, s->bh));
            else
                for (u32 y = 0; y < s->h; y++) memcpy(layer_dst + y * row, layer_src + (u64)y * s->pitch, row);
        }
        VkCommandBuffer cb = vk_cmd();
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        for (u32 layer = 0; layer < s->layers; layer++) {
            VkBufferImageCopy c = {.bufferOffset = off + (u64)layer * slice,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
                                   .imageExtent = {s->w, s->h, 1}};
            VmaAllocation tm;
            VkImage t = s->sw == s->w && s->sh == s->h ? VK_NULL_HANDLE : vk_unscaled_image(s, 1, &tm);
            if (!t)
                vkCmdCopyBufferToImage(cb, buf, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            else {
                c.imageSubresource.baseArrayLayer = 0;
                vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdCopyBufferToImage(cb, buf, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                scale_blit(cb, t, s->w, s->h, 0, s->img, s->sw, s->sh, layer, s->f.is_int);
                vk_defer_image(t, tm, VK_NULL_HANDLE, VK_NULL_HANDLE);
            }
        }
        s->mem_wgen = s->wgen; /* memory and image hold the same now: nothing to write back */
        s->draw_wseen = 0;     /* a successful full load starts a new source epoch for later draws */
        s->gw = gpu_mem_generation();
    }
    free(tmp);
}

/* a depth target whose Vulkan format holds the guest's bytes (Z32F as D32_SFLOAT, Z16 as D16_UNORM): its depth
   aspect copies out in the guest's own layout. The packed ones (the Z24S8 kinds, Z32F_X24S8) would need converting,
   and a scaled one a depth blit; those stay on the GPU */
static int depth_copies_raw(const Surf* s) {
    return (s->f.vk == VK_FORMAT_D32_SFLOAT || s->f.vk == VK_FORMAT_D16_UNORM) && s->sw == s->w && s->sh == s->h;
}
/* the bytes s's pixels take read back, or 0 when they don't go to memory: a format guest memory can't hold as it is, no
   pixels, or not drawn to since it was last written back or loaded (memory has it already, and a CPU write since then
   is newer than the target) */
static u64 flush_bytes(const Surf* s) {
    if (!s || (s->f.depth ? !depth_copies_raw(s) : !s->f.raw) || s->layout == VK_IMAGE_LAYOUT_UNDEFINED) return 0;
    if (s->mem_wgen == s->wgen || !moves_scaled(s)) return 0;
    return (u64)s->w * s->f.bpp * s->h * s->layers;
}
/* s's pixels copied into buf at at, a layer after another, at the guest's size */
static void flush_record(VkCommandBuffer cb, Surf* s, VkBuffer buf, u64 at) {
    u64 slice = (u64)s->w * s->f.bpp * s->h;
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    for (u32 layer = 0; layer < s->layers; layer++) {
        VkBufferImageCopy c = {
            .bufferOffset = at + (u64)layer * slice,
            .imageSubresource = {s->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
            .imageExtent = {s->w, s->h, 1}};
        VmaAllocation tm;
        VkImage t = s->sw == s->w && s->sh == s->h ? VK_NULL_HANDLE : vk_unscaled_image(s, 1, &tm);
        if (!t)
            vkCmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        else {
            c.imageSubresource.baseArrayLayer = 0;
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            scale_blit(cb, s->img, s->sw, s->sh, layer, t, s->w, s->h, 0, s->f.is_int);
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vkCmdCopyImageToBuffer(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
            vk_defer_image(t, tm, VK_NULL_HANDLE, VK_NULL_HANDLE);
        }
    }
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
    vkCmdPipelineBarrier2(cb, &di);
    vk_finish();
    vk_readback_ready();
}
/* s's pixels, read back to p, into guest memory in its layout (or over original, a storage image's merge) */
static void flush_land(Surf* s, const u8* p, const u8* original) {
    u64 gva = s->gva, row = (u64)s->w * s->f.bpp, slice = row * s->h;
    u64 sz = rt_bytes(s);
    u8* dst = original ? NULL : gmem_ptr(gva, sz);
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
    for (u32 layer = 0; layer < s->layers; layer++) {
        u8* layer_dst = s->volume && s->bl ? dst : dst + (u64)layer * s->layer_size;
        const u8* layer_src = p + (u64)layer * slice;
        if (s->volume && s->bl) {
            u32 bh = bl_height(s->h, s->bh);
            for (u32 y = 0; y < s->h; y++)
                for (u32 x = 0; x < row; x += 16) {
                    u32 n = row - x < 16 ? (u32)(row - x) : 16;
                    memcpy(layer_dst + bl_offset(x, y, layer, (u32)row, s->h, bh, s->bd), layer_src + y * row + x, n);
                }
        } else if (s->bl)
            swizzle(layer_dst, layer_src, (u32)row, (u32)row, s->h, bl_height(s->h, s->bh));
        else
            for (u32 y = 0; y < s->h; y++) memcpy(layer_dst + (u64)y * s->pitch, layer_src + y * row, row);
    }
    int guest_changed = 0;
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
        s->mem_wgen = s->wgen; /* avoid flushing this target again when writeback checks overlapping targets */
        vk_write_changed(gva, dst, original, (u32)sz);
        free(tmp);
    } else if (tmp) {
        gmem_write(gva, tmp, sz);
        free(tmp);
    } else
        gpu_mem_written(gva, sz); /* caches built from this memory check it again */
    s->mem_wgen = s->wgen;
    s->gw = gpu_mem_generation();
    if (!s->is_rt) vk_tex_note_writeback(s);
    if (guest_changed) {
        /* The image still holds the shader result where the CPU won the merge. Force the next bind to upload
           the merged guest bytes, even though note_writeback just hashed those same bytes. */
        s->hash ^= 1;
        s->gw = 0;
    }
}
static void surf_flush(Surf* s, const u8* original) {
    u64 size = flush_bytes(s);
    VkBuffer buf;
    if (!size) return;
    vk_render_end();
    const u8* p = vk_readback_buffer(size, &buf);
    if (!p) return;
    VkCommandBuffer cb = vk_cmd();
    flush_record(cb, s, buf, 0);
    flush_wait(cb);
    flush_land(s, p, original);
}
void vk_surf_flush(Surf* s, const u8* original) {
    vk_result_begin();
    surf_flush(s, original);
    vk_result_end();
}
void vk_rt_flush(u64 gva) { vk_surf_flush(vk_rt_find(gva), NULL); }

void vk_rt_before_write(u64 gva, u64 size) {
    if (!size || g_rt_flushing) return;
    vk_lock();
    g_rt_flushing = 1;
    for (Surf* s = g_rt_first; s; s = s->rt_next)
        if (!(s->gva >> 63) && gva < s->gva + rt_bytes(s) && s->gva < gva + size)
            vk_surf_flush(s, NULL);
    g_rt_flushing = 0;
    vk_unlock();
}

static Surf* rt_create(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth, u32 pitch, u32 layers, u64 layer_size,
                       int volume) {
    if (!w || !h) return NULL;
    u32 fkey = fmt | (depth ? 0x10000 : 0);
    /* the layout is part of what a target is: block linear with its block height, or pitch linear with its pitch */
    u32 bl = !((tile >> 12) & 1), bh = bl ? (tile >> 4) & 15 : 0, bd = bl && volume ? (tile >> 8) & 15 : 0;
    while (bd && layers <= (1u << (bd - 1))) bd--;
    if (bl)
        pitch = 0;
    else if (!pitch)
        pitch = w * (depth ? vk_zt_format(fmt) : vk_rt_format(fmt)).bpp;
    VkFmt layout_fmt = depth ? vk_zt_format(fmt) : vk_rt_format(fmt);
    u64 min_layer_size = bl ? bl_size(w * layout_fmt.bpp, h, bh) : (u64)pitch * h;
    if (layer_size < min_layer_size) layer_size = min_layer_size;
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == gva && s->is_rt && s->w == w && s->h == h && s->fkey == fkey && s->bl == bl && s->bh == bh &&
            s->pitch == pitch && s->layers == layers && s->layer_size == layer_size && s->bd == bd &&
            s->volume == volume) {
            s->last_used = g_vk_present;
            vk_rt_note_read(s);
            return s;
        }
    /* another size, format or layout at this address: park the current one, reuse a parked one that fits */
    Surf* parked = NULL;
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == (gva | 1ull << 63) && s->is_rt && s->w == w && s->h == h && s->fkey == fkey && s->bl == bl &&
            s->bh == bh && s->pitch == pitch && s->layers == layers && s->layer_size == layer_size && s->bd == bd &&
            s->volume == volume && !parked)
            parked = s;
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == gva && s->is_rt) s->gva = gva | 1ull << 63;
    if (parked) {
        parked->gva = gva;
        parked->last_used = g_vk_present;
        vk_rt_note_read(parked);
        return parked;
    }
    VkFmt f = depth ? vk_zt_format(fmt) : vk_rt_format(fmt);
    Surf* s = surf_create(gva, w, h, f, fkey, bl, bh, pitch, 1, layers);
    if (!s) return NULL;
    s->layer_size = layer_size;
    s->bd = bd;
    s->volume = volume;
    s->last_used = g_vk_present;
    s->gw = gpu_mem_generation();
    /* the CPU may have written the memory already (a cleared or decoded frame): start from it */
    if (!depth) vk_rt_reload(gva);
    return s;
}

Surf* vk_rt(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth, u32 pitch) {
    return rt_create(gva, w, h, fmt, tile, depth, pitch, 1, 0, 0);
}

Surf* vk_rt_array(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, u32 pitch, u32 layers, u64 layer_size, int volume) {
    return rt_create(gva, w, h, fmt, tile, 0, pitch, layers, layer_size, volume);
}

Surf* vk_zt_array(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, u32 layers, u64 layer_size) {
    return rt_create(gva, w, h, fmt, tile, 1, w * vk_zt_format(fmt).bpp, layers, layer_size, 0);
}

void vk_rt_note_read(Surf* s) {
    s->last_used = g_vk_present;
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

Surf* vk_rt_for_cpu(u64 cpu_addr, u32 w, u32 h) {
    /* the one of the frame's size, else any; the most recently used of those */
    Surf* best = NULL;
    int best_exact = 0;
    for (int k = 0; k < SURFH; k++)
        for (Surf* s = g_surfh[k]; s; s = s->next) {
            if (!s->is_rt || s->f.depth || (s->gva >> 63) || s->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
                gmmu_translate(s->gva) != cpu_addr)
                continue;
            int exact = s->w == w && s->h == h;
            if (!best || exact > best_exact || (exact == best_exact && s->last_used > best->last_used)) {
                best = s;
                best_exact = exact;
            }
        }
    if (best) vk_rt_note_read(best);
    return best && best->layout != VK_IMAGE_LAYOUT_UNDEFINED ? best : NULL;
}

/* render targets drawn this frame or the last, for a texture made of them (gpu/vk/texture.c) */
static Surf** g_drawn;
static int g_ndrawn, g_capdrawn;
static u32 g_drawn_seq;
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
        h = (h ^ (cpu ? vk_hash(GP(cpu), n) : n)) * 1099511628211ull; /* an unmapped page counts as its length */
        o += n;
    }
    return h;
}
void vk_rt_drawn(Surf* s) {
    /* A memory write after the first draw can leave untouched pixels stale even when later draws touch the target.
       Keep that first baseline until the next present instead of scanning the whole target on every draw. */
    if (!s->draw_wseen || s->drawn_at != g_vk_present) {
        u64 size = rt_bytes(s);
        /* without write tracking, a hash of the whole target: only for one a texture takes pixels from (feeds_tex),
           where it is the one answer vk_rt_drawn_fresh has. Hashing every target on its first draw of each present
           took most of the GPU thread (FreeBSD, Linux before 6.7) */
        s->draw_has_hash = vk_written_since(gmem_ptr_untracked(s->gva, size), size, 0) < 0 && s->feeds_tex;
        if (s->draw_has_hash) s->draw_hash = rt_mem_hash(s);
        s->draw_wseen = vk_ww_now();
        s->draw_gw = gpu_mem_generation();
    }
    s->drawn_at = g_vk_present;
    s->drawn_seq = ++g_drawn_seq;
    for (int i = 0; i < g_ndrawn; i++)
        if (g_drawn[i] == s) return;
    if (g_ndrawn == g_capdrawn) {
        int cap = g_capdrawn ? g_capdrawn * 2 : 64;
        Surf** n = realloc(g_drawn, (size_t)cap * sizeof *n);
        if (!n) return;
        g_drawn = n;
        g_capdrawn = cap;
    }
    g_drawn[g_ndrawn++] = s;
}
void vk_rt_drawn_full(Surf* s) {
    s->draw_wseen = 0; /* every pixel was replaced, so earlier memory writes no longer matter */
    vk_rt_drawn(s);
}
int vk_rt_drawn_fresh(Surf* s) {
    u64 size = rt_bytes(s);
    if (!s->draw_wseen || s->layout == VK_IMAGE_LAYOUT_UNDEFINED || gpu_mem_dirty_since(s->draw_gw, s->gva, size))
        return 0;
    if (s->draw_has_hash) return rt_mem_hash(s) == s->draw_hash;
    int written = vk_written_since(gmem_ptr_untracked(s->gva, size), size, s->draw_wseen);
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
    for (int i = 0; i < g_ndrawn; i++) {
        Surf* s = g_drawn[i];
        /* Generated textures may first be sampled several frames after their draw.
           Age does not make guest memory authoritative: tex_take_drawn checks
           vk_rt_drawn_fresh before copying, and surf_free removes evicted targets. */
        if (s->gva >= lo && s->gva < hi) {
            if (n < max) out[n] = s; /* a parked target's address is out of every range */
            n++;
        }
    }
    return n;
}

/* a surface's image and views go when the frame being recorded is done (descriptors in it may name them) */
static void surf_free(Surf* s) {
    if (s->is_rt) {
        for (Surf** pp = &g_rt_first; *pp; pp = &(*pp)->rt_next)
            if (*pp == s) {
                *pp = s->rt_next;
                break;
            }
    }
    for (int i = 0; i < g_ndrawn; i++)
        if (g_drawn[i] == s) {
            g_drawn[i] = g_drawn[--g_ndrawn];
            break;
        }
    for (int i = 0; i < s->nsv; i++) vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, s->sv[i].v, VK_NULL_HANDLE);
    vk_defer_image(s->img, s->mem, s->att, s->att_linear);
    free(s);
}
/* a before b in when they were last drawn to */
static int drawn_before(const void* a, const void* b) {
    i32 d = (i32)((*(Surf* const*)a)->drawn_seq - (*(Surf* const*)b)->drawn_seq);
    return d < 0 ? -1 : d > 0;
}
/* the bytes of s's pixels that go to guest memory as it is evicted, or 0 */
static u64 evict_bytes(Surf* s) {
    u64 b = s->is_rt && !(s->gva >> 63) ? flush_bytes(s) : 0;
    return b && vk_rt_drawn_fresh(s) ? b : 0;
}
/* s's pixels to guest memory, as vk_surf_evict sends them: 1 when they went */
static int evict_flush(Surf* s) {
    if (!evict_bytes(s)) return 0;
    vk_surf_flush(s, NULL);
    return s->mem_wgen == s->wgen;
}
/* the pixels of those of the n targets in s that go to memory, read back behind one wait for the GPU, not one each
   (Mario Kart 8 Deluxe lets 400 go at its title), and landed in s's order; how many went, their bytes added to
   *bytes */
static u32 evict_flush_all(Surf** s, u32 n, u64* bytes) {
    static u64* at;
    static u32 cap;
    u64 total = 0;
    u32 kept = 0, i;
    int batched = 1;
    VkBuffer buf = VK_NULL_HANDLE;
    const u8* p = NULL;
    u8* copy = NULL;
    if (n > cap) {
        u64* a = realloc(at, (size_t)n * sizeof *a);
        if (a) {
            at = a;
            cap = n;
        } else
            batched = 0;
    }
    for (i = 0; batched && i < n; i++) {
        u64 b = evict_bytes(s[i]);
        at[i] = b ? total : ~0ull;
        total += (b + 15) & ~15ull; /* each copy's offset a multiple of its texel */
    }
    if (batched && total) {
        vk_render_end();
        p = vk_readback_buffer(total, &buf);
        copy = p ? malloc(total) : NULL;
        batched = copy != NULL;
    }
    if (!batched) { /* no room for them all: one at a time */
        for (i = 0; i < n; i++)
            if (evict_flush(s[i])) kept++, *bytes += rt_bytes(s[i]);
        return kept;
    }
    if (!total) return 0;
    VkCommandBuffer cb = vk_cmd();
    for (i = 0; i < n; i++)
        if (at[i] != ~0ull) flush_record(cb, s[i], buf, at[i]);
    flush_wait(cb);
    memcpy(copy, p, total); /* landing one can read another back through the same buffer (vk_rt_before_write) */
    vk_result_begin();
    for (i = 0; i < n; i++)
        if (at[i] != ~0ull) {
            flush_land(s[i], copy + at[i], NULL);
            kept++;
            *bytes += rt_bytes(s[i]);
        }
    vk_result_end();
    free(copy);
    return kept;
}
/* a target whose pixels memory can't take (a depth one, or a format or scale that doesn't copy out as the guest's
   bytes) and that holds a draw: it stays while its address is its own, as freeing it would lose what it drew (Mario
   Kart 8 Deluxe draws depth targets as a course loads and reads them through the race). Depth draws aren't tracked:
   a depth target holds one once anything left its layout defined */
static int keeps_unsaved(Surf* s) {
    if (!s->is_rt || (s->gva >> 63) || vk_surf_can_writeback(s)) return 0;
    return s->f.depth ? s->layout != VK_IMAGE_LAYOUT_UNDEFINED : vk_rt_drawn_fresh(s);
}
void vk_surf_evict(u32 before) {
    static Surf** gone;
    static u32 cap;
    u32 n = 0, kept = 0;
    u64 kept_bytes = 0, t0 = plat_ns();
    /* A storage image keeps a Surf pointer until its result reaches guest memory. */
    vk_gfx_image_land();
    /* render targets too, parked or not: one not drawn, sampled or presented for as long goes. What it drew since
       memory last had it goes to memory first, as the console's GPU draws into memory: a texture made there later
       reads it (Mario Kart 8 Deluxe draws its Mii icons at boot and shows them a minute later). Not a parked one,
       whose address is another target's now, nor one whose memory was written after its draw. Oldest draw first:
       where two overlap, the later one's pixels are what memory keeps */
    for (int k = 0; k < SURFH; k++)
        for (Surf** pp = &g_surfh[k]; *pp;) {
            Surf* s = *pp;
            if ((i32)((u32)s->last_used - before) >= 0 || keeps_unsaved(s)) {
                pp = &s->next;
                continue;
            }
            *pp = s->next;
            if (n == cap) {
                u32 c = cap ? cap * 2 : 256;
                Surf** g = realloc(gone, (size_t)c * sizeof *g);
                if (!g) { /* no room to order them: this one goes to memory now */
                    if (evict_flush(s)) kept++, kept_bytes += rt_bytes(s);
                    for (Surf *c2 = s->crop, *x; c2; c2 = x) {
                        x = c2->next;
                        surf_free(c2);
                    }
                    surf_free(s);
                    continue;
                }
                gone = g;
                cap = c;
            }
            gone[n++] = s;
        }
    if (n > 1) qsort(gone, n, sizeof *gone, drawn_before);
    kept += evict_flush_all(gone, n, &kept_bytes);
    for (u32 i = 0; i < n; i++) {
        for (Surf *c = gone[i]->crop, *x; c; c = x) { /* its copies (gpu/vk/texture.c rt_crop) */
            x = c->next;
            surf_free(c);
        }
        surf_free(gone[i]);
    }
    if (kept)
        LOG_LIMIT(20,
                  "vulkan: %u render target%s unused for ten seconds went to guest memory before they went (%llu KB, "
                  "%llu ms)\n",
                  kept, kept == 1 ? "" : "s", (unsigned long long)(kept_bytes >> 10),
                  (unsigned long long)((plat_ns() - t0) / 1000000));
}

/* the 2D engine's blit between render targets, on the GPU. The surfaces are gpu/gpu.h's
   Surf2D. 0 when it can't be done there: the front end then copies through guest memory */
int vk_blit2d(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
              double sdy) {
    Surf* st = vk_rt_find(s->addr);
    if (!st || st->f.depth || st->layout == VK_IMAGE_LAYOUT_UNDEFINED || dw <= 0 || dh <= 0) return 0;
    u32 tile = (d->linear ? 0x1000u : 0) | (d->bh << 4);
    Surf* dt = vk_rt(d->addr, d->width, d->height, d->format, tile, 0, d->pitch);
    if (!dt || dt == st || dt->f.depth || st->f.is_int != dt->f.is_int) return 0;
    if (!blit_ok(st->f.vk, VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !blit_ok(dt->f.vk, VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return 0;
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
    /* a region starting outside either image (the source's corner can be negative): the front end copies it */
    if (x1 <= x0 || y1 <= y0 || X1 <= dx || Y1 <= dy || x0 < 0 || y0 < 0 || dx < 0 || dy < 0) return 0;
    int scaled = x1 - x0 != X1 - dx || y1 - y0 != Y1 - dy;
    vk_rt_note_read(st);
    if (st->layout == VK_IMAGE_LAYOUT_UNDEFINED) return 0;
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, st, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, dt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{x0, y0, 0}, {x1, y1, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstOffsets = {{dx, dy, 0}, {X1, Y1, 1}}};
    int linear = scaled && !st->f.is_int && blit_ok(st->f.vk, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    vkCmdBlitImage(cb, st->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    dt->wgen++;
    if (dx == 0 && dy == 0 && X1 == (i32)dt->sw && Y1 == (i32)dt->sh)
        vk_rt_drawn_full(dt);
    else
        vk_rt_drawn(dt);
    dt->last_used = g_vk_present;
    return 1;
}
