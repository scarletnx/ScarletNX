/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ASTC LDR uploads on the graphics queue. CPU work stops at deswizzling the
   compressed blocks. The shader writes packed RGBA8 to device memory, then a
   transfer copies those bytes to the image (including sRGB images, without an
   unwanted gamma conversion). Chunking bounds scratch memory and SSBO ranges. */
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/texture/astc_spv.h"
#include "nx/runtime/platform/log.h"

static VkDescriptorSetLayout g_astc_ds;
static VkPipelineLayout g_astc_layout;
static VkPipeline g_astc_pipe;
static VkBuffer g_astc_scratch;
static VmaAllocation g_astc_mem;
static u64 g_astc_cap;

/* astc.comp's push constants: the strip's size in texels, the block footprint, and whether to use the
   specification's sRGB decode mode, whose R, G and B bytes differ from the UNORM mode's. An SRGB image (the
   texture header's sRGB bit, vk_tic_format) gets the sRGB mode, so the bytes are those an sRGB ASTC texture
   decodes to on the console. */
typedef struct {
    u32 w, h, bw, bh, srgb;
} AstcParams;

static void astc_init(void) {
    if (g_astc_pipe) return;
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_astc_ds)))
        fatal("vulkan: cannot create the ASTC descriptor layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcParams)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_astc_ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_astc_layout)))
        fatal("vulkan: cannot create the ASTC pipeline layout");
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof astc_spv, .pCode = astc_spv};
    VkShaderModule module;
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &module))) fatal("vulkan: cannot load the ASTC shader");
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = module,
                                                .pName = "main"},
                                      .layout = g_astc_layout};
    int ok = VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &g_astc_pipe));
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    if (!ok) fatal("vulkan: cannot create the ASTC compute pipeline");
    /* Kept for the device lifetime, like the other internal pipelines. All uses
       are on g_vk.gfx, under vk_lock(), even when uploads originate in compute. */
    g_astc_cap = 8ull << 20;
    if (g_astc_cap > g_vk.props.limits.maxStorageBufferRange) g_astc_cap = g_vk.props.limits.maxStorageBufferRange;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = g_astc_cap,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_astc_scratch, &g_astc_mem, NULL)))
        fatal("vulkan: cannot allocate ASTC scratch memory");
}

static void scratch_barrier(VkCommandBuffer cb, int before_decode) {
    VkBufferMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = before_decode ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = before_decode ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = before_decode ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = before_decode ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = g_astc_scratch,
        .size = VK_WHOLE_SIZE};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

void vk_astc_upload(VkCommandBuffer cb, Surf* t, const u8* blocks, u32 w, u32 h, u32 depth, u32 level, u32 layer) {
    astc_init();
    u32 bw = t->f.bw, bh = t->f.bh;
    u32 nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh;
    u64 row_in = (u64)nx * 16, row_out = (u64)w * bh * 4;
    u32 rows = (u32)(g_astc_cap / row_out);
    u32 in_rows = (u32)(g_vk.props.limits.maxStorageBufferRange / row_in);
    if (rows > in_rows) rows = in_rows;
    if (rows > g_vk.props.limits.maxComputeWorkGroupCount[1]) rows = g_vk.props.limits.maxComputeWorkGroupCount[1];
    if (!rows || (nx + 31) / 32 > g_vk.props.limits.maxComputeWorkGroupCount[0])
        fatal("vulkan: ASTC upload exceeds device limits");
    for (u32 z = 0; z < depth; z++) {
        for (u32 y = 0; y < ny; y += rows) {
            u32 n = ny - y < rows ? ny - y : rows;
            u32 height = h - y * bh < n * bh ? h - y * bh : n * bh;
            u64 size = row_in * n, offset;
            VkBuffer input;
            u8* mapped = vk_stage(size, vk_stage_align(), &input, &offset);
            if (!mapped) fatal("vulkan: cannot stage ASTC blocks");
            memcpy(mapped, blocks + ((u64)z * ny + y) * row_in, size);
            VkDescriptorSet set = vk_desc_alloc(g_astc_ds);
            if (!set) fatal("vulkan: cannot allocate ASTC descriptors");
            VkDescriptorBufferInfo buffers[2] = {{input, offset, size}, {g_astc_scratch, 0, (u64)w * height * 4}};
            VkWriteDescriptorSet writes[2];
            for (u32 i = 0; i < 2; i++)
                writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                   .dstSet = set,
                                                   .dstBinding = i,
                                                   .descriptorCount = 1,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   .pBufferInfo = &buffers[i]};
            vkUpdateDescriptorSets(g_vk.dev, 2, writes, 0, NULL);
            /* Submission makes the flushed host staging writes visible. Scratch
               reuse also needs a WAR dependency on the preceding image copy,
               including when that copy belongs to an earlier frame. */
            scratch_barrier(cb, 1);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc_pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc_layout, 0, 1, &set, 0, NULL);
            AstcParams p = {w, height, bw, bh, t->f.vk == VK_FORMAT_R8G8B8A8_SRGB};
            vkCmdPushConstants(cb, g_astc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            vkCmdDispatch(cb, (nx + 31) / 32, n, 1);
            scratch_barrier(cb, 0);
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
                                      .imageOffset = {0, (i32)(y * bh), (i32)z},
                                      .imageExtent = {w, height, 1}};
            vkCmdCopyBufferToImage(cb, g_astc_scratch, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
    }
}
