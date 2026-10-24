/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The VIC composition as a compute shader (gpu/video/vic.comp), and its picture kept on the GPU (docs/GPU_OWNERSHIP.md).
   gpu/video/vic.c hands over the job's planes and its slots' parameters. A job's buffer is device-local and its own:
   the parameters (gpu/video/vic.c's vic_gpu_params) as a uniform buffer at its start, the slots' input planes, then
   the output planes, each byte for byte as it lies in guest memory (the shader's address math matches the CPU's,
   bl_offset included). What the CPU holds of the planes comes through staging, read without touching what the GPU
   holds (own_stage); what the GPU holds (NVDEC's pictures, the last job's output) is copied in on the GPU. The output
   planes come in too: the shader writes whole words, one owning invocation each, and the bytes it doesn't own
   (outside the target rectangle, past the width) keep their values.

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
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)vic_spv_size, .pCode = vic_spv};
    VkShaderModuleCreateInfo mi_unaligned = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                             .codeSize = (size_t)vic_unaligned_spv_size,
                                             .pCode = vic_unaligned_spv};
    /* two builds of the shader: the aligned one is the happy path, the unaligned one handles planes whose rows
       are not word aligned (gpu/video/vic.comp) */
    VkPipeline* pipes[2] = {&g_vic_pipe, &g_vic_pipe_unaligned};
    const VkShaderModuleCreateInfo* mis[2] = {&mi, &mi_unaligned};
    for (int i = 0; i < 2; i++) {
        VkShaderModule module;
        if (!VK_OK(vkCreateShaderModule(g_vk.dev, mis[i], NULL, &module))) fatal("vulkan: cannot load the VIC shader");
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                    .module = module,
                                                    .pName = "main"},
                                          .layout = g_vic_layout};
        int ok = VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, pipes[i]));
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

/* plane pl's bytes into the staging at host (its place at `at` in the job's buffer): the CPU's from guest memory, the
   GPU's as copies into the job's buffer */
static void plane_stage(const VicPlane* pl, u8* host, u64 at, Copies* cp) {
    OwnRange o[16];
    u32 n, i;
    VkBuffer ib;
    u64 io, cpu = (u64)(pl->p - g_mem);
    if (!pl->p || !pl->lim) return;
    /* written in place on the GPU (an NVDEC picture under unified memory), not done yet: copied from guest memory on
       the GPU after the write */
    if (vk_uma_pending_cpu(cpu, pl->lim) && cp->n < sizeof cp->c / sizeof *cp->c && vk_uma_get(cpu, pl->lim, 1, &ib, &io)) {
        cp->src[cp->n] = ib;
        cp->c[cp->n] = (VkBufferCopy){io, at, pl->lim};
        cp->n++;
        return;
    }
    vk_uma_mem_need_cpu(cpu, pl->lim); /* or waited for */
    n = own_stage((u64)(pl->p - g_mem), pl->lim, host, o, sizeof o / sizeof *o);
    if (n > sizeof o / sizeof *o || cp->n + n > sizeof cp->c / sizeof *cp->c) {
        /* more owners than noted: what the GPU holds of the plane lands, and it is read as it is then */
        own_acquire((u64)(pl->p - g_mem), pl->lim);
        memcpy(host, pl->p, pl->lim);
        return;
    }
    for (i = 0; i < n; i++) {
        const VkOwnRes* r = (const VkOwnRes*)o[i].res;
        cp->src[cp->n] = r->buf;
        cp->c[cp->n].srcOffset = o[i].res_off;
        cp->c[cp->n].dstOffset = at + (o[i].addr - (u64)(pl->p - g_mem));
        cp->c[cp->n].size = o[i].size;
        cp->n++;
    }
}

static int compose(const VicJob* j) {
    u64 al = vk_stage_align();
    /* the buffer's layout: the parameters, every slot's input planes, then the two output planes */
    u64 at = sizeof(VicGpuParams);
    u32 in_offs[16] = {0}; /* slot s's luma plane at 2s, its chroma at 2s + 1 */
    for (int s = 0; s < 8; s++) {
        const VicSlot* sl = &j->slots[s];
        if (!sl->on) continue;
        u64 lims[2] = {sl->iy.lim, sl->ic.lim};
        for (int k = 0; k < 2; k++) {
            at = (at + al - 1) & ~(al - 1);
            in_offs[2 * s + k] = (u32)at;
            at += lims[k];
        }
    }
    at = ((at + 3) & ~3ull) + 4; /* a word past the last plane: the unaligned build's funnel loads it */
    u64 ol_off = (at + al - 1) & ~(al - 1), ol_size = (j->oy.lim + 3) & ~3ull;
    /* every binding offset, the chroma's too, is storage aligned */
    u64 oc_off = (ol_off + ol_size + al - 1) & ~(al - 1), oc_size = (j->oc.lim + 3) & ~3ull;
    if (oc_size < 4) oc_size = 4;
    u64 total = oc_off + oc_size;
    u64 range_max = g_vk.props.limits.maxStorageBufferRange;
    if (total - ol_off > range_max || at > range_max || total > g_vk.alloc_cap) {
        LOG_LIMIT(4, "video: a VIC job of %llu bytes exceeds the device's limits, skipped\n", (unsigned long long)total);
        return 0;
    }
    VicGpuParams p;
    vic_gpu_params(j, &p, in_offs, (u32)(oc_off - ol_off));

    vic_init();
    vk_render_end(); /* compute and copies happen outside rendering passes */
    VkOwnRes* w = vk_own_res_new(total);
    VkBuffer staged;
    u64 soff;
    u8* host = w ? vk_stage(total, al, &staged, &soff) : NULL;
    VkDescriptorSet set = host ? vk_desc_alloc(g_vic_ds) : VK_NULL_HANDLE;
    if (!set) {
        LOG_LIMIT(4, "video: no GPU memory for a VIC job, skipped\n");
        if (w) vk_own_res_unref(w);
        return 0;
    }
    Copies cp;
    cp.n = 0;
    memset(host, 0, total); /* the padding between planes, which the shader may load and never uses */
    memcpy(host, &p, sizeof p);
    snx_le32_array(host, sizeof p / 4); /* host numbers, all 32 bit words (vic_params.glsl), read little-endian */
    for (int s = 0; s < 8; s++) {
        const VicSlot* sl = &j->slots[s];
        if (!sl->on) continue;
        plane_stage(&sl->iy, host + in_offs[2 * s], in_offs[2 * s], &cp);
        plane_stage(&sl->ic, host + in_offs[2 * s + 1], in_offs[2 * s + 1], &cp);
    }
    /* the output planes come in: the shader owns whole words and leaves the other bytes as they are */
    plane_stage(&j->oy, host + ol_off, ol_off, &cp);
    plane_stage(&j->oc, host + oc_off, oc_off, &cp);

    VkDescriptorBufferInfo buffers[3] = {{w->buf, 0, sizeof(VicGpuParams)},
                                         /* from 0: in_offs count from the start of the buffer */
                                         {w->buf, 0, at},
                                         /* both output planes: the chroma's base is oc_off - ol_off */
                                         {w->buf, ol_off, total - ol_off}};
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
    rec_CmdDispatch(cb, (p.job.tx1 - p.job.gx0 + VIC_TILE_W - 1) / VIC_TILE_W,
                    (p.job.ty0 + p.job.th - p.job.gy0 + VIC_TILE_H - 1) / VIC_TILE_H, 1);
    /* what reads the picture next (a copy into a texture or the display, a landing) sees the shader's writes */
    job_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    /* the picture is the guest's output planes from now on */
    vk_own_publish((u64)(j->oy.p - g_mem), j->oy.lim, w, ol_off);
    if (j->oc.p && j->oc.lim) vk_own_publish((u64)(j->oc.p - g_mem), j->oc.lim, w, oc_off);
    vk_own_res_unref(w); /* the owned ranges hold it */
    return 1;
}
int vk_vic_compose(const VicJob* j) {
    vk_lock();
    int r = compose(j);
    vk_unlock();
    return r;
}
