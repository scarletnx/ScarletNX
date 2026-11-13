/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The VIC composition as a compute shader (gpu/video/vic.comp), and its picture kept on the GPU (docs/layers/gpu/ownership.md).
   gpu/video/vic.c hands over the job's planes and its slots' parameters. A job's buffer is device-local and its own:
   the parameters (gpu/video/vic.c's vic_gpu_params) as a uniform buffer at its start, the slots' input planes, then
   the output planes, each the rows the job reads or writes byte for byte as they lie in guest memory (the shader's
   address math matches the CPU's, bl_offset included). What the CPU holds of the planes comes through staging, read
   without touching what the GPU holds (own_stage); what the GPU holds (NVDEC's pictures, the last job's output) is
   copied in on the GPU. The output planes come in too: the shader writes whole words, one owning invocation each, and
   the bytes it doesn't own (outside the target rectangle, past the width) keep their values. A job bigger than one
   buffer goes in bands of the target's rows, each a job of its own (compose_bands).

   The job is recorded into the frame's command buffer under vk_lock, in order with the draws and copies that read
   its picture, and nothing waits for it: the output planes are published over the guest's (vk_own_publish), so a
   texture or the display copies them on the GPU, and the CPU sees them only when it touches them (the trap lands
   them then). */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/video/vic.h"
#include "nx/gpu/video/vic_spv.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/runtime/platform/log.h"

static VkDescriptorSetLayout g_vic_ds;
static VkPipelineLayout g_vic_layout;
static VkPipeline g_vic_pipe, g_vic_pipe_unaligned;

static void vic_init(void) {
    if (g_vic_pipe) return;
    /* the parameters, the input planes, the output planes */
    VkDescriptorSetLayoutBinding bindings[3];
    for (u32 i = 0; i < 3; i++)
        bindings[i] =
            (VkDescriptorSetLayoutBinding){i, i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bindings};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_vic_ds)))
        fatal("vulkan: cannot create the VIC descriptor layout");
    VkPipelineLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_vic_ds};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_vic_layout)))
        fatal("vulkan: cannot create the VIC pipeline layout");
    /* two builds of the shader: the aligned one is the happy path, the unaligned one handles planes whose rows
       are not word aligned (gpu/video/vic.comp) */
    VkPipeline* pipes[2] = {&g_vic_pipe, &g_vic_pipe_unaligned};
    const u32* words[2] = {vic_spv, vic_unaligned_spv};
    const size_t sizes[2] = {(size_t)vic_spv_size, (size_t)vic_unaligned_spv_size};
    for (int i = 0; i < 2; i++) {
        VkShaderModule module;
        if (!VK_OK(snx_vk_shader_module(g_vk.dev, words[i], sizes[i], &module)))
            fatal("vulkan: cannot load the VIC shader");
        int ok = VK_OK(snx_vk_module_pipeline(g_vk.dev, vk_pipeline_cache(), g_vic_layout, module, NULL, pipes[i]));
        vkDestroyShaderModule(g_vk.dev, module, NULL);
        if (!ok) fatal("vulkan: cannot create the VIC compute pipeline");
    }
}

/* a barrier in the frame's command buffer */
static void job_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 src_access,
                        VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = src,
                           .srcAccessMask = src_access,
                           .dstStageMask = dst,
                           .dstAccessMask = dst_access};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
}

/* the job's GPU copies: owned parts of its planes, from their resources into the job's buffer */
typedef struct {
    VkBufferCopy c[64];
    VkBuffer src[64];
    u32 n;
} Copies;

/* plane pl's bytes into the staging at host + at (its place in the job's buffer): the CPU's from guest memory, the
   GPU's as copies into the job's buffer. The bytes from done (where those written before it end) to at are cleared
   first. Where the plane's bytes end */
static u64 plane_stage(const VicPlane* pl, u8* host, u64 at, u64 done, Copies* cp) {
    OwnRange o[16];
    u32 n, i;
    VkBuffer ib;
    u64 io;
    if (!pl->p || !pl->lim) return done; /* nothing here: its space, if it has any, is cleared with what follows */
    memset(host + done, 0, at - done);
    /* written in place on the GPU (uma.c: an NVDEC picture, the last job's output), not done yet: copied from guest
       memory on the GPU, after the write */
    if (cp->n < sizeof cp->c / sizeof *cp->c && vk_uma_copy_pending((u64)(pl->p - g_mem), pl->lim, &ib, &io)) {
        cp->src[cp->n] = ib;
        cp->c[cp->n] = (VkBufferCopy){io, at, pl->lim};
        cp->n++;
        return at + pl->lim;
    }
    vk_uma_mem_need_cpu((u64)(pl->p - g_mem), pl->lim); /* or waited for */
    host += at;
    n = own_stage((u64)(pl->p - g_mem), pl->lim, host, o, sizeof o / sizeof *o);
    if (n > sizeof o / sizeof *o || cp->n + n > sizeof cp->c / sizeof *cp->c) {
        /* more owners than noted: what the GPU holds of the plane lands, and it is read as it is then */
        own_acquire((u64)(pl->p - g_mem), pl->lim);
        memcpy(host, pl->p, pl->lim);
        return at + pl->lim;
    }
    for (i = 0; i < n; i++) {
        const VkOwnRes* r = (const VkOwnRes*)o[i].res;
        cp->src[cp->n] = r->buf;
        cp->c[cp->n].srcOffset = o[i].res_off;
        cp->c[cp->n].dstOffset = at + (o[i].addr - (u64)(pl->p - g_mem));
        cp->c[cp->n].size = o[i].size;
        cp->n++;
    }
    return at + pl->lim;
}

static u64 up(u64 v, u64 a) { return (v + a - 1) / a * a; }

/* the bytes [*lo, *hi) of plane pl that its rows [a, b) lie in: a pitch linear plane's rows, a block linear one's
   rows of blocks, within the bytes it may touch */
static void plane_rows(const VicPlane* pl, i64 a, i64 b, u64* lo, u64* hi) {
    *lo = *hi = 0;
    if (!pl->p || !pl->lim) return;
    if (a < 0) a = 0;
    if (b <= a) return;
    if (pl->blk) {
        u64 stride = (u64)((pl->pitch + 63) / 64) << (9 + pl->bh);
        u32 sh = 3 + pl->bh;
        *lo = ((u64)a >> sh) * stride;
        *hi = (((u64)(b - 1) >> sh) + 1) * stride;
    } else {
        *lo = (u64)a * pl->pitch;
        *hi = (u64)b * pl->pitch;
    }
    if (*hi > pl->lim) *hi = pl->lim;
    if (*lo > *hi) *lo = *hi;
}

/* What the band of the target's rows [r0, r1) reads and writes: the bytes [lo, hi) of each plane (16 slot planes, 2s
   the luma and 2s + 1 the chroma of slot s, then the output's luma and chroma at 16 and 17) and where they lie in the
   job's buffer. The buffer: the parameters, the slots' planes, a word past them (the unaligned build's funnel loads
   it, in_end), the output's. A slot's rows are taken from the even row its first block of 4x2 pixels starts on to the
   row after its last, which a block reads and leaves out */
typedef struct {
    u64 lo[18], hi[18], at[18];
    u64 in_end, total;
} VicBand;
static void band_layout(const VicJob* j, u32 r0, u32 r1, VicBand* v) {
    u64 al = vk_stage_align(), at = sizeof(VicGpuParams);
    memset(v, 0, sizeof *v);
    for (u32 s = 0; s < 8; s++) {
        const VicSlot* sl = &j->slots[s];
        if (!sl->on) continue;
        i64 a = r0 > sl->dy0 ? r0 : sl->dy0, b = r1 < sl->dy0 + sl->h ? r1 : sl->dy0 + sl->h;
        if (b > a) {
            i64 sy = (a - (a & 1)) + (i64)sl->sy0 - (i64)sl->dy0, ey = b + 1 + (i64)sl->sy0 - (i64)sl->dy0;
            if (sy < 0) sy = 0;
            plane_rows(&sl->iy, sy, ey, &v->lo[2 * s], &v->hi[2 * s]);
            plane_rows(&sl->ic, sy / 2, ey / 2 + 1, &v->lo[2 * s + 1], &v->hi[2 * s + 1]);
        }
        for (u32 k = 2 * s; k < 2 * s + 2; k++) {
            at = up(at, al);
            v->at[k] = at;
            at += v->hi[k] - v->lo[k];
        }
    }
    v->in_end = up(at, 4) + 4;
    plane_rows(&j->oy, r0 & ~1u, up(r1, 2), &v->lo[16], &v->hi[16]);
    plane_rows(&j->oc, (r0 & ~1u) / 2, up(r1, 2) / 2, &v->lo[17], &v->hi[17]);
    /* every binding offset, the chroma's too, is storage aligned; the shader writes whole words */
    v->at[16] = up(v->in_end, al);
    v->at[17] = up(v->at[16] + up(v->hi[16] - v->lo[16], 4), al);
    v->total = v->at[17] + (v->hi[17] > v->lo[17] ? up(v->hi[17] - v->lo[17], 4) : 4);
}

/* the band of rows that could not be composed, and the bytes it lacked them for */
static int band_fail(const VicJob* j, u32 r0, u32 r1, const char* what, u64 bytes) {
    LOG_LIMIT(4, "video: a VIC job of %ux%u pixels, its rows %u to %u, %s %llu bytes: those rows are not composed\n",
              j->tw, j->th, r0, r1 - 1, what, (unsigned long long)bytes);
    return 0;
}

/* the target's rows [r0, r1) as v lays them out */
static int compose(const VicJob* j, u32 r0, u32 r1, const VicBand* v) {
    u64 al = vk_stage_align(), total = v->total;
    u32 in_offs[16] = {0};
    VicGpuParams p;
    /* a plane's base is where its byte 0 would be: its window's first byte lo at its place, modulo 2^32 as the shader
       adds them. Its lim is the window's end, so no load or store leaves it */
    for (u32 k = 0; k < 16; k++) in_offs[k] = (u32)(v->at[k] - v->lo[k]);
    vic_gpu_params(j, &p, in_offs, (u32)(v->at[17] - v->at[16] - v->lo[17]));
    for (u32 s = 0, n = 0; s < 8; s++)
        if (j->slots[s].on) {
            if (p.slots[n].iy.lim) p.slots[n].iy.lim = (u32)v->hi[2 * s];
            if (p.slots[n].ic.lim) p.slots[n].ic.lim = (u32)v->hi[2 * s + 1];
            n++;
        }
    p.job.ol.base = (u32)(0 - v->lo[16]);
    if (p.job.ol.lim) p.job.ol.lim = (u32)v->hi[16];
    if (p.job.oc.lim) p.job.oc.lim = (u32)v->hi[17];
    /* the band's rows of the target, from the even row its blocks of 4x2 pixels start on */
    p.job.ty0 = r0;
    p.job.th = r1 - r0;
    p.job.gy0 = r0 & ~1u;

    vic_init();
    vk_render_end(); /* compute and copies happen outside rendering passes */
    VkOwnRes* w = vk_own_res_new(total, VK_OWN_BY_VIDEO);
    VkBuffer staged;
    u64 soff;
    u8* host = w ? vk_stage(total, al, &staged, &soff) : NULL;
    VkDescriptorSet set = host ? vk_desc_alloc(g_vic_ds) : VK_NULL_HANDLE;
    if (!set) {
        if (w) vk_own_res_unref(w);
        return band_fail(j, r0, r1, "found no GPU memory for its", total);
    }
    Copies cp;
    cp.n = 0;
    memcpy(host, &p, sizeof p);
    snx_le32_array(host, sizeof p / 4); /* host numbers, all 32 bit words (vic_params.glsl), read little-endian */
    /* every window is staged whole, so only the padding between them is cleared: the shader may load it and never
       uses it (a whole frame cleared and then copied over was most of a video's CPU time) */
    u64 done = sizeof p;
    for (u32 s = 0; s < 8; s++) {
        const VicSlot* sl = &j->slots[s];
        if (!sl->on) continue;
        VicPlane y = sl->iy, c = sl->ic;
        y.p += v->lo[2 * s], y.lim = v->hi[2 * s] - v->lo[2 * s];
        c.p += v->lo[2 * s + 1], c.lim = v->hi[2 * s + 1] - v->lo[2 * s + 1];
        done = plane_stage(&y, host, v->at[2 * s], done, &cp);
        done = plane_stage(&c, host, v->at[2 * s + 1], done, &cp);
    }
    /* the output's windows come in: the shader owns whole words and leaves the other bytes as they are */
    VicPlane oy = j->oy, oc = j->oc;
    oy.p += v->lo[16], oy.lim = v->hi[16] - v->lo[16];
    if (oc.p) oc.p += v->lo[17], oc.lim = v->hi[17] - v->lo[17];
    done = plane_stage(&oy, host, v->at[16], done, &cp);
    done = plane_stage(&oc, host, v->at[17], done, &cp);
    memset(host + done, 0, total - done);

    VkDescriptorBufferInfo buffers[3] = {{w->buf, 0, sizeof(VicGpuParams)},
                                         /* from 0: in_offs count from the start of the buffer */
                                         {w->buf, 0, v->in_end},
                                         /* both output planes: the chroma's base counts from the luma's place */
                                         {w->buf, v->at[16], total - v->at[16]}};
    VkWriteDescriptorSet writes[3];
    for (u32 i = 0; i < 3; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                               : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(g_vk.dev, 3, writes, 0, NULL);

    VkCommandBuffer cb = vk_cmd();
    /* the staged bytes, then what the GPU holds over them: the GPU's writes to those resources first done */
    VkBufferCopy in = {soff, 0, total};
    job_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    rec_CmdCopyBuffer(cb, staged, w->buf, 1, &in);
    if (cp.n) {
        job_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        for (u32 i = 0; i < cp.n; i++) rec_CmdCopyBuffer(cb, cp.src[i], w->buf, 1, &cp.c[i]);
    }
    job_barrier(
        cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, vic_gpu_aligned(j) ? g_vic_pipe : g_vic_pipe_unaligned);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_vic_layout, 0, 1, &set, 0, NULL);
    /* a workgroup composes a tile of output pixels, over the target rectangle only: nothing else is written */
    vk_gpuprof_tag(VK_PROF_VIC);
    rec_CmdDispatch(cb, (p.job.tx1 - p.job.gx0 + VIC_TILE_W - 1) / VIC_TILE_W,
                    (p.job.ty0 + p.job.th - p.job.gy0 + VIC_TILE_H - 1) / VIC_TILE_H, 1);
    /* what reads the picture next (a copy into a texture or the display, a landing) sees the shader's writes */
    job_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    /* the windows are the guest's output planes from now on */
    if (oy.lim) vk_own_publish((u64)(oy.p - g_mem), oy.lim, w, v->at[16]);
    if (oc.p && oc.lim) vk_own_publish((u64)(oc.p - g_mem), oc.lim, w, v->at[17]);
    vk_own_res_unref(w); /* the owned ranges hold it */
    return 1;
}

/* A job too big for one buffer (one allocation, or a storage binding: maxStorageBufferRange) goes in bands of the
   target's rows (vk_band), each a job of its own that stages the rows of each plane its rows read. A band ends on
   the output planes' rows of blocks, so each block is read and written back by one band */
static u64 band_need(const void* user, u32 y, u32 n) {
    const VicJob* j = (const VicJob*)user;
    VicBand v;
    band_layout(j, j->ty0 + y, j->ty0 + y + n, &v);
    return v.total;
}
static int compose_bands(const VicJob* j) {
    u64 range = g_vk.props.limits.maxStorageBufferRange;
    u32 unit = 2;
    if (j->oy.blk && 8u << j->oy.bh > unit) unit = 8u << j->oy.bh;
    if (j->oc.p && j->oc.blk && 16u << j->oc.bh > unit) unit = 16u << j->oc.bh; /* a chroma row is two of luma */
    VkBands bands = {range < g_vk.alloc_cap ? range : g_vk.alloc_cap, unit, j->ty0, band_need, j};
    for (u32 y = 0, n; y < j->th; y += n) {
        u64 most = 0;
        n = vk_band(&bands, y, j->th - y, &most);
        if (most > bands.cap) return band_fail(j, j->ty0 + y, j->ty0 + y + n, "needs more than a buffer may be:", most);
        VicBand v;
        band_layout(j, j->ty0 + y, j->ty0 + y + n, &v);
        if (!compose(j, j->ty0 + y, j->ty0 + y + n, &v)) return 0;
    }
    return 1;
}
int vk_vic_compose(const VicJob* j) {
    vk_lock();
    int r = compose_bands(j);
    vk_unlock();
    /* with unified memory the planes are copied in on the GPU and no trap lands them: host1x increments the job's
       syncpoint once this returns, and the guest reads them then */
    if (r) vk_uma_mem_need_cpu((u64)(j->oy.p - g_mem), j->oy.lim);
    if (r && j->oc.p && j->oc.lim) vk_uma_mem_need_cpu((u64)(j->oc.p - g_mem), j->oc.lim);
    return r;
}
