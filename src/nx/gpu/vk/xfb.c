/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Transform-feedback control blocks and draw arguments stay on the GPU. The NVN control format is one stride
   word followed by four byte-count words. Begin writes only the stride to guest memory; End/Pause publish the
   counters, and Resume imports them. A compute invocation turns the first byte count into draw arguments. */
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/xfb_indirect_spv.h"
#include "nx/runtime/platform/log.h"
#include <string.h>

static struct {
    VkDescriptorSetLayout descriptors;
    VkPipelineLayout layout;
    VkPipeline pipeline;
} g_xfb_indirect;

static void xfb_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 source, VkAccessFlags2 source_access,
                        VkPipelineStageFlags2 destination, VkAccessFlags2 destination_access) {
    VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = source,
                                .srcAccessMask = source_access,
                                .dstStageMask = destination,
                                .dstAccessMask = destination_access};
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    rec_CmdPipelineBarrier2(cb, &dependency);
}

int vk_xfb_control_buffer(VkBuffer counters, u64 counter_offset, u64 control, u32 what, u32 stride) {
    if (what > 3 || (control & 3) || (!control && what == 0)) return 0;
    if (!control) return 1; /* temporary pause/resume keeps the current counters */
    vk_render_end();
    VkBuffer buffer, source = VK_NULL_HANDLE;
    u64 offset, source_offset = 0;
    if (what == 0 || what == 1 || what == 2) {
        u64 address = what ? control + 4 : control;
        if (!vk_buf_copy_writable(address, what ? 16 : 4, &buffer, &offset, 0)) return 0;
        if (!what) {
            u8* staged = vk_stage(20, 4, &source, &source_offset);
            if (!staged) {
                vk_buf_writable_abort();
                return 0;
            }
            snx_st_le32(staged, stride);
            memset(staged + 4, 0, 16); /* the bytes copied to the GPU's four counters */
        }
    } else if (!vk_buf_get(control + 4, 16, &buffer, &offset))
        return 0;
    VkCommandBuffer cb = vk_cmd();
    /* Private counters can be read by a capture and written by its end; copies must wait for both uses. */
    xfb_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!what) {
        VkBufferCopy header = {source_offset, offset, 4}, reset = {source_offset + 4, counter_offset, 16};
        rec_CmdCopyBuffer(cb, source, buffer, 1, &header);
        rec_CmdCopyBuffer(cb, source, counters, 1, &reset);
    } else {
        VkBufferCopy copy = {what == 3 ? offset : counter_offset, what == 3 ? counter_offset : offset, 16};
        rec_CmdCopyBuffer(cb, what == 3 ? buffer : counters, what == 3 ? counters : buffer, 1, &copy);
    }
    xfb_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    if (what != 3) vk_buf_writable_commit();
    return 1;
}

static int xfb_indirect_init(void) {
    if (g_xfb_indirect.pipeline) return 1;
    if (!g_xfb_indirect.descriptors) {
        VkDescriptorSetLayoutBinding bindings[2];
        for (u32 i = 0; i < 2; i++)
            bindings[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                         VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
        if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &info, NULL, &g_xfb_indirect.descriptors))) return 0;
    }
    if (!g_xfb_indirect.layout) {
        VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                           .setLayoutCount = 1,
                                           .pSetLayouts = &g_xfb_indirect.descriptors,
                                           .pushConstantRangeCount = 1,
                                           .pPushConstantRanges = &push};
        if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &info, NULL, &g_xfb_indirect.layout))) return 0;
    }
    return VK_OK(snx_vk_compute_pipeline(g_vk.dev, vk_pipeline_cache(), g_xfb_indirect.layout, xfb_indirect_spv,
                                         xfb_indirect_spv_size, &g_xfb_indirect.pipeline));
}

int vk_xfb_indirect(u64 control, VkBuffer* arguments, u64* offset) {
    if ((control & 3) || !control || !xfb_indirect_init()) return 0;
    VkBuffer input;
    u64 input_offset, alignment = g_vk.props.limits.minStorageBufferOffsetAlignment;
    if (!alignment) alignment = 1;
    if (!vk_buf_get(control, 8, &input, &input_offset)) return 0;
    if (!vk_stage_output(16, alignment < 4 ? 4 : alignment, arguments, offset, NULL)) return 0;
    VkDescriptorSet set = vk_desc_alloc(g_xfb_indirect.descriptors);
    if (!set) return 0;
    u64 base = input_offset / alignment * alignment;
    VkDescriptorBufferInfo buffers[2] = {{input, base, input_offset - base + 8}, {*arguments, *offset, 16}};
    snx_vk_storage_buffers(g_vk.dev, set, buffers, 2);
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    xfb_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    u32 first = (u32)((input_offset - base) / 4);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_xfb_indirect.pipeline);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_xfb_indirect.layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_xfb_indirect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof first, &first);
    vk_gpuprof_tag(VK_PROF_XFB_INDIRECT);
    rec_CmdDispatch(cb, 1, 1, 1);
    xfb_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    return 1;
}
