/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The VIC composition as a compute shader (gpu/video/vic.comp). gpu/video/vic.c hands over the job's planes and its
   slots' parameters; the planes travel into a host-visible buffer byte for byte as they lie in guest memory (the
   shader's address math matches the CPU's, bl_offset included), the shader composes the picture, and the output
   planes travel back the same way. The call is synchronous: when it returns the picture is in guest memory, which
   is what the CPU path in gpu/video/vic.c gives the callers too.

   The shader works on a device-local copy of that buffer, made and read back with transfer copies in the job's
   command buffer. Over the host buffer itself every output word was a read and a write across the bus in block
   linear order: a 1280x720 picture took 33 ms on an RTX 2080, with the submitting game thread waiting in its ioctl,
   against well under 1 ms of copies. Where the host buffer is device-local already (a UMA device), the shader
   uses it directly.

   The output planes are copied in as well as out: the shader writes whole words, one owning invocation each, and
   the bytes it does not own (outside the slot rectangles, past the width) keep their guest values.

   A job holds vk_lock only to make the pipeline and the buffer and to submit to the queue it shares with the
   renderer: its buffer, descriptor set, command buffer and fence are its own, under g_vic_lock. The planes are
   copied in and out of guest memory, and the fence waited for, without vk_lock, as the CPU composition writes them:
   written under it, the renderer did not take the whole picture (parts of a 1920x1080 block linear one stayed
   black) and stood still for the whole job. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/video/vic_spv.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* the shader's structs (gpu/video/vic.comp): only 32 bit members, so std430 packs them exactly as C does */
typedef struct {
    u32 offset, pitch, height, lim, blk, bh;
} GpuPlane;
typedef struct {
    u32 on;
    u32 sx0, sy0, dx0, dy0, w, h;
    int mat[12]; /* the color matrix rows: c00 c01 c02 c03 c10 ... c23 */
    u32 mat_shift, mat_enable;
    u32 alpha, clamp_lo, clamp_hi;
    GpuPlane iy, ic;
} GpuSlot;
typedef struct {
    u32 fmt, sw, sh, nslots;
    GpuPlane ol, oc;
    GpuSlot slots[8];
} GpuParams;
_Static_assert(sizeof(GpuPlane) == 24, "GpuPlane disagrees with the shader");
_Static_assert(sizeof(GpuSlot) == 144, "GpuSlot disagrees with the shader");
_Static_assert(sizeof(GpuParams) == 64 + 8 * 144, "GpuParams disagrees with the shader");

static PMutex g_vic_lock = PMUTEX_INIT; /* a job at a time: the buffer and the objects below are the job's */
static VkDescriptorSetLayout g_vic_ds;
static VkDescriptorPool g_vic_pool;
static VkDescriptorSet g_vic_set;
static VkPipelineLayout g_vic_layout;
static VkPipeline g_vic_pipe, g_vic_pipe_unaligned;
static VkBuffer g_vic_buf;
static VmaAllocation g_vic_mem;
static u8* g_vic_ptr;
static u64 g_vic_cap;
static VkBuffer g_vic_dbuf; /* the shader's device-local copy, VK_NULL_HANDLE when g_vic_buf is device-local */
static VmaAllocation g_vic_dmem;
static VkCommandPool g_vic_cpool;
static VkCommandBuffer g_vic_cb;
static VkFence g_vic_fence;

static void vic_init(void) {
    if (g_vic_pipe) return;
    VkDescriptorSetLayoutBinding bindings[4];
    for (u32 i = 0; i < 4; i++)
        bindings[i] =
            (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = bindings};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_vic_ds)))
        fatal("vulkan: cannot create the VIC descriptor layout");
    VkPipelineLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_vic_ds};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_vic_layout)))
        fatal("vulkan: cannot create the VIC pipeline layout");
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vic_spv, .pCode = vic_spv};
    VkShaderModuleCreateInfo mi_unaligned = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                             .codeSize = sizeof vic_unaligned_spv,
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
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g_vk.compute_family};
    if (!VK_OK(vkCreateCommandPool(g_vk.dev, &pci, NULL, &g_vic_cpool)))
        fatal("vulkan: cannot create the VIC command pool");
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = g_vic_cpool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    if (!VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &g_vic_cb)))
        fatal("vulkan: cannot allocate the VIC command buffer");
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!VK_OK(vkCreateFence(g_vk.dev, &fci, NULL, &g_vic_fence))) fatal("vulkan: cannot create the VIC fence");
    /* the job's own set, not one of the frame's: those pools are reset when a frame starts, which a job no longer
       holds back */
    VkDescriptorPoolSize psz = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &psz};
    if (!VK_OK(vkCreateDescriptorPool(g_vk.dev, &pi, NULL, &g_vic_pool)))
        fatal("vulkan: cannot create the VIC descriptor pool");
    VkDescriptorSetAllocateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = g_vic_pool,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &g_vic_ds};
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &si, &g_vic_set))) fatal("vulkan: cannot allocate the VIC set");
}

/* one buffer for the job, grown as needed: every job waits for its fence before it returns, so the old buffer is
   idle when the next job replaces it */
static int buf_fit(u64 need) {
    if (g_vic_buf && need <= g_vic_cap) return 1;
    if (g_vic_dbuf) {
        vmaDestroyBuffer(g_vk.vma, g_vic_dbuf, g_vic_dmem);
        g_vic_dbuf = VK_NULL_HANDLE;
    }
    if (g_vic_buf) {
        vmaDestroyBuffer(g_vk.vma, g_vic_buf, g_vic_mem);
        g_vic_buf = VK_NULL_HANDLE;
        g_vic_cap = 0;
    }
    u64 cap = 8ull << 20;
    while (cap < need) cap *= 2;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = cap,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.flags =
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo info;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_vic_buf, &g_vic_mem, &info))) {
        g_vic_buf = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryPropertyFlags props;
    vmaGetAllocationMemoryProperties(g_vk.vma, g_vic_mem, &props);
    if (!(props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        VmaAllocationCreateInfo di = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &di, &g_vic_dbuf, &g_vic_dmem, NULL))) {
            g_vic_dbuf = VK_NULL_HANDLE;
            vmaDestroyBuffer(g_vk.vma, g_vic_buf, g_vic_mem);
            g_vic_buf = VK_NULL_HANDLE;
            return 0;
        }
    }
    g_vic_ptr = info.pMappedData;
    g_vic_cap = cap;
    return 1;
}
/* a barrier between two stages of the job's command buffer */
static void job_barrier(VkPipelineStageFlags2 src, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst,
                        VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = src,
                           .srcAccessMask = src_access,
                           .dstStageMask = dst,
                           .dstAccessMask = dst_access};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(g_vic_cb, &di);
}

static GpuPlane plane_desc(const VicPlane* p, u32 offset) {
    /* an unmapped plane composes nothing: its limits are zero in the shader */
    if (!p->p) return (GpuPlane){offset, 0, 0, 0, 0, 0};
    return (GpuPlane){offset, p->pitch, p->h, (u32)p->lim, p->blk, p->bh};
}

/* the aligned shader build needs every plane's rows to keep the word aligned columns word aligned: block linear
   rows do (bl_offset keeps the column's alignment), pitch linear rows only when the pitch is a multiple of 4 */
static int job_aligned(const VicJob* j) {
    for (int s = 0; s < 8; s++) {
        const VicSlot* sl = &j->slots[s];
        if (!sl->on) continue;
        if (sl->iy.p && !sl->iy.blk && (sl->iy.pitch & 3)) return 0;
        if (sl->ic.p && !sl->ic.blk && (sl->ic.pitch & 3)) return 0;
    }
    if (j->oy.p && !j->oy.blk && (j->oy.pitch & 3)) return 0;
    if (j->oc.p && !j->oc.blk && (j->oc.pitch & 3)) return 0;
    return 1;
}

static int compose(const VicJob* j) {
    vk_lock();
    vic_init();
    vk_unlock();
    u64 al = vk_stage_align();
    /* the buffer's layout: the parameters, every slot's input planes, then the two output planes */
    u64 params_off = 0, in_off = params_off + sizeof(GpuParams), at = in_off;
    u32 in_offs[8][2] = {{0}};
    for (int s = 0; s < 8; s++) {
        const VicSlot* sl = &j->slots[s];
        if (!sl->on) continue;
        u64 lims[2] = {sl->iy.lim, sl->ic.lim};
        for (int k = 0; k < 2; k++) {
            at = (at + al - 1) & ~(al - 1);
            in_offs[s][k] = (u32)at;
            at += lims[k];
        }
    }
    u64 ol_off = (at + al - 1) & ~(al - 1), ol_size = (j->oy.lim + 3) & ~3ull;
    /* every binding offset, the chroma's too, is storage aligned */
    u64 oc_off = (ol_off + ol_size + al - 1) & ~(al - 1), oc_size = (j->oc.lim + 3) & ~3ull;
    if (oc_size < 4) oc_size = 4;
    u64 total = oc_off + oc_size;
    u64 range_max = g_vk.props.limits.maxStorageBufferRange;
    if (total > range_max || ol_size > range_max || oc_size > range_max || at > range_max) {
        LOG_LIMIT(4, "video: VIC job too large for the GPU (%llu bytes), composed in guest memory\n",
                  (unsigned long long)total);
        return 0;
    }
    vk_lock();
    int fits = buf_fit(total);
    vk_unlock();
    if (!fits) {
        LOG_LIMIT(4, "video: no GPU memory for a VIC job, composed in guest memory\n");
        return 0;
    }

    GpuParams p = {0};
    p.fmt = j->fmt;
    p.sw = j->sw;
    p.sh = j->sh;
    p.nslots = 8;
    p.ol = plane_desc(&j->oy, 0);
    p.oc = plane_desc(&j->oc, 0);
    for (int s = 0; s < 8; s++) {
        const VicSlot* sl = &j->slots[s];
        GpuSlot* g = &p.slots[s];
        g->on = (u32)sl->on;
        g->sx0 = sl->sx0;
        g->sy0 = sl->sy0;
        g->dx0 = sl->dx0;
        g->dy0 = sl->dy0;
        g->w = sl->w;
        g->h = sl->h;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) g->mat[r * 4 + c] = sl->mat[r][c];
        g->mat_shift = (u32)sl->mat_shift;
        g->mat_enable = (u32)sl->mat_enable;
        g->alpha = sl->alpha;
        g->clamp_lo = sl->clamp_lo;
        g->clamp_hi = sl->clamp_hi;
        if (!sl->on) continue;
        g->iy = plane_desc(&sl->iy, in_offs[s][0]);
        g->ic = plane_desc(&sl->ic, in_offs[s][1]);
        memcpy(g_vic_ptr + in_offs[s][0], sl->iy.p, sl->iy.lim);
        memcpy(g_vic_ptr + in_offs[s][1], sl->ic.p, sl->ic.lim);
    }
    memcpy(g_vic_ptr + params_off, &p, sizeof p);
    /* the output planes round trip: the shader owns whole words and leaves the other bytes alone */
    memcpy(g_vic_ptr + ol_off, j->oy.p, j->oy.lim);
    if (j->oc.p) memcpy(g_vic_ptr + oc_off, j->oc.p, j->oc.lim);
    vmaFlushAllocation(g_vk.vma, g_vic_mem, 0, VK_WHOLE_SIZE);

    VkDescriptorSet set = g_vic_set;
    VkBuffer work = g_vic_dbuf ? g_vic_dbuf : g_vic_buf; /* the buffer the shader composes in */
    VkDescriptorBufferInfo buffers[4] = {{work, params_off, sizeof(GpuParams)},
                                         /* from 0: in_offs count from the start of the buffer */
                                         {work, 0, at},
                                         {work, ol_off, ol_size},
                                         {work, oc_off, oc_size}};
    VkWriteDescriptorSet writes[4];
    for (u32 i = 0; i < 4; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(g_vk.dev, 4, writes, 0, NULL);

    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_OK(vkBeginCommandBuffer(g_vic_cb, &bi));
    if (g_vic_dbuf) {
        VkBufferCopy in = {0, 0, total};
        vkCmdCopyBuffer(g_vic_cb, g_vic_buf, g_vic_dbuf, 1, &in);
        job_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    vkCmdBindPipeline(g_vic_cb, VK_PIPELINE_BIND_POINT_COMPUTE, job_aligned(j) ? g_vic_pipe : g_vic_pipe_unaligned);
    vkCmdBindDescriptorSets(g_vic_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_vic_layout, 0, 1, &set, 0, NULL);
    /* a workgroup composes a 32x8 tile of output pixels (gpu/video/vic.comp) */
    vkCmdDispatch(g_vic_cb, (j->sw + 31) / 32, (j->sh + 7) / 8, 1);
    /* the fence alone does not make the writes visible to the host */
    if (g_vic_dbuf) {
        VkBufferCopy out = {ol_off, ol_off, total - ol_off}; /* the output planes, which end the buffer */
        job_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        vkCmdCopyBuffer(g_vic_cb, g_vic_dbuf, g_vic_buf, 1, &out);
        job_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                    VK_ACCESS_2_HOST_READ_BIT);
    } else
        job_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    VK_OK(vkEndCommandBuffer(g_vic_cb));
    VkCommandBufferSubmitInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = g_vic_cb};
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &c};
    vk_lock(); /* the queue is the renderer's compute queue too */
    VkResult r = vkQueueSubmit2(g_vk.compute, 1, &si, g_vic_fence);
    vk_unlock();
    if (r == VK_ERROR_DEVICE_LOST) fatal("vulkan: the GPU device was lost (driver reset or crash)");
    if (r != VK_SUCCESS) return 0;
    VK_OK(vkWaitForFences(g_vk.dev, 1, &g_vic_fence, VK_TRUE, UINT64_MAX));
    VK_OK(vkResetFences(g_vk.dev, 1, &g_vic_fence));
    vmaInvalidateAllocation(g_vk.vma, g_vic_mem, 0, VK_WHOLE_SIZE);
    memcpy(j->oy.p, g_vic_ptr + ol_off, j->oy.lim);
    if (j->oc.p) memcpy(j->oc.p, g_vic_ptr + oc_off, j->oc.lim);
    return 1;
}
int vk_vic_compose(const VicJob* j) {
    pm_lock(&g_vic_lock);
    int r = compose(j);
    pm_unlock(&g_vic_lock);
    return r;
}
