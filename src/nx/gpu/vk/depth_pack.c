/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Packed depth and stencil targets (Z32F_X24S8 and the Z24S8 kinds) written back in the guest's layout. Their Vulkan
   formats have no layout a buffer copy can give: the depth and stencil aspects are copied out apart, then packed on the
   GPU (depth_pack.comp) as the target's format lays a pixel in memory, the reverse of texconv.comp's unpacking on
   upload. Without it they never reached memory, and a game copying one through memory (the DMA engine) read zeros */
#include <stdlib.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/depth_pack_spv.h"
#include "nx/runtime/platform/log.h"

static struct {
    VkDescriptorSetLayout ds;
    VkPipelineLayout layout;
    VkPipeline pipe;
    int failed;
} g_depth_pack;

/* depth_pack.comp's modes, by the guest's zeta format (Eden's PixelFormatFromDepthFormat names the same layouts) */
enum { DP_Z32F_S8, DP_S8_Z24, DP_Z24_S8, DP_Z24_X8 };
int vk_depth_pack_mode(u32 zeta) {
    switch (zeta) {
        case 0x19: return DP_Z32F_S8; /* Z32_FLOAT_X24S8_UINT */
        case 0x14: return DP_S8_Z24;  /* Z24_UNORM_S8_UINT: the stencil in the low byte */
        case 0x16: return DP_Z24_S8;  /* S8Z24_UNORM: the stencil in the high byte */
        case 0x15: return DP_Z24_X8;  /* X8Z24_UNORM */
    }
    return -1;
}

static int pack_init(void) {
    if (g_depth_pack.pipe) return 1;
    if (g_depth_pack.failed) return 0;
    g_depth_pack.failed = 1; /* until everything below is made */
    VkDescriptorSetLayoutBinding b[3];
    for (u32 i = 0; i < 3; i++)
        b[i] =
            (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_depth_pack.ds))) return 0;
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(u32)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_depth_pack.ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_depth_pack.layout))) return 0;
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = depth_pack_spv_size, .pCode = depth_pack_spv};
    VkShaderModule m;
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m))) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m,
                                                .pName = "main"},
                                      .layout = g_depth_pack.layout};
    int ok = VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &g_depth_pack.pipe));
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (!ok) {
        g_depth_pack.pipe = VK_NULL_HANDLE;
        return 0;
    }
    g_depth_pack.failed = 0;
    return 1;
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 from, VkAccessFlags2 wrote, VkPipelineStageFlags2 to,
                    VkAccessFlags2 reads) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = from,
                           .srcAccessMask = wrote,
                           .dstStageMask = to,
                           .dstAccessMask = reads};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &d);
}

int vk_depth_pack(VkCommandBuffer cb, const Surf* s, u32 layer, VkBuffer buf, u64 at, u32 pitch) {
    int mode = vk_depth_pack_mode(s->fkey & 0xFFFF);
    u32 w = s->w, h = s->h, bpp = mode == DP_Z32F_S8 ? 8 : 4, row = w * bpp;
    u64 n = (u64)w * h, dat, sat, pat;
    VkBuffer db, sb, pb;
    if (mode < 0 || !pack_init()) return 0;
    if (!vk_stage_device(n * 4, vk_stage_align(), &db, &dat) ||
        !vk_stage_device((n + 3) / 4 * 4, vk_stage_align(), &sb, &sat) ||
        !vk_stage_device(n * bpp, vk_stage_align(), &pb, &pat))
        return 0;
    VkDescriptorSet set = vk_desc_alloc(g_depth_pack.ds);
    if (!set) return 0;
    /* the aspects apart, tightly packed: a word of depth a pixel (a 24-bit one in the low bits), a byte of stencil */
    VkBufferImageCopy c[2] = {
        {.bufferOffset = dat, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 1}, .imageExtent = {w, h, 1}},
        {.bufferOffset = sat,
         .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, layer, 1},
         .imageExtent = {w, h, 1}}};
    rec_CmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, db, 1, &c[0]);
    rec_CmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sb, 1, &c[1]);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    VkDescriptorBufferInfo bufs[3] = {{db, dat, n * 4}, {sb, sat, (n + 3) / 4 * 4}, {pb, pat, n * bpp}};
    VkWriteDescriptorSet wr[3];
    for (u32 i = 0; i < 3; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = i,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &bufs[i]};
    vkUpdateDescriptorSets(g_vk.dev, 3, wr, 0, NULL);
    u32 push[4] = {w, h, (u32)mode, s->f.vk == VK_FORMAT_D32_SFLOAT_S8_UINT && mode != DP_Z32F_S8};
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_depth_pack.pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_depth_pack.layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_depth_pack.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    rec_CmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    /* the packed rows to where the caller wants them, at any offset and pitch */
    if (pitch == row) {
        VkBufferCopy all = {pat, at, n * bpp};
        rec_CmdCopyBuffer(cb, pb, buf, 1, &all);
        return 1;
    }
    VkBufferCopy rows[64];
    for (u32 y = 0; y < h;) {
        u32 k = 0;
        for (; k < 64 && y < h; k++, y++) rows[k] = (VkBufferCopy){pat + (u64)y * row, at + (u64)y * pitch, row};
        rec_CmdCopyBuffer(cb, pb, buf, k, rows);
    }
    return 1;
}
