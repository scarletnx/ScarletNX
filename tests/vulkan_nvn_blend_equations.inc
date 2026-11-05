/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the same scalar equation emitter used by programmable blending, without raster/attachment rounding.
   Endpoint alpha and RGB values test safe division, dodge/burn endpoints and achromatic HSL. */
#include "support/advanced_blend_spirv.h"
#include "support/nvn_advanced_blend_ref.h"

static int check_nvn_blend_equations(void) {
    VkDescriptorSetLayoutBinding bindings[2];
    for (u32 i = 0; i < 2; i++)
        bindings[i] =
            (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo descriptor_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout descriptors;
    VkPipelineLayout layout;
    CHECK(VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &descriptor_info, NULL, &descriptors)));
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &descriptors};
    CHECK(VK_OK(vkCreatePipelineLayout(g_vk.dev, &layout_info, NULL, &layout)));
    u32 samples = 289, checks = 0;
    u64 alignment = g_vk.props.limits.minStorageBufferOffsetAlignment;
    if (alignment < 16) alignment = 16;
    vk_lock();
    for (u32 mode = 5; mode <= 50; mode++)
        for (u32 pm = 0; pm < 2; pm++)
            for (u32 overlap = 0; overlap < 3; overlap++) {
                float input[289][8];
                for (u32 i = 0; i < samples; i++) {
                    float a = (float)(i / 17) / 16, b = (float)(i % 17) / 16;
                    const float s[3] = {(float)(i % 5) / 4, (float)(i % 3) / 2, (float)(i % 9) / 8};
                    const float d[3] = {(float)((i + 2) % 5) / 4, (float)((i + 1) % 3) / 2, (float)((i + 4) % 9) / 8};
                    for (u32 c = 0; c < 3; c++) {
                        input[i][c] = pm ? s[c] * a : s[c];
                        input[i][4 + c] = d[c] * b;
                    }
                    input[i][3] = a;
                    input[i][7] = b;
                }
                size_t words = 0;
                u32* code = nvn_blend_compute_spirv(mode, (int)pm, overlap, &words);
                CHECK(code);
                VkShaderModuleCreateInfo module_info = {
                    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = code};
                VkShaderModule module;
                CHECK(VK_OK(vkCreateShaderModule(g_vk.dev, &module_info, NULL, &module)));
                free(code);
                VkComputePipelineCreateInfo pipeline_info = {
                    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                    .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                              .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                              .module = module,
                              .pName = "main"},
                    .layout = layout};
                VkPipeline pipeline;
                CHECK(VK_OK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline)));
                vkDestroyShaderModule(g_vk.dev, module, NULL);
                VkBuffer source, destination, readback;
                u64 source_offset, destination_offset;
                u8* stage = vk_stage(sizeof input, alignment, &source, &source_offset);
                CHECK(stage);
                memcpy(stage, input, sizeof input);
                CHECK(vk_stage_output(samples * 16, alignment, &destination, &destination_offset, NULL));
                u8* actual = vk_readback_buffer(samples * 16, &readback);
                CHECK(actual);
                VkDescriptorSet set = vk_desc_alloc(descriptors);
                CHECK(set);
                VkDescriptorBufferInfo buffers[2] = {{source, source_offset, sizeof input},
                                                     {destination, destination_offset, samples * 16}};
                VkWriteDescriptorSet writes[2];
                for (u32 i = 0; i < 2; i++)
                    writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                       .dstSet = set,
                                                       .dstBinding = i,
                                                       .descriptorCount = 1,
                                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                       .pBufferInfo = &buffers[i]};
                vkUpdateDescriptorSets(g_vk.dev, 2, writes, 0, NULL);
                vk_render_end();
                VkCommandBuffer cb = vk_cmd();
                rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
                rec_CmdDispatch(cb, samples, 1, 1);
                VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
                VkDependencyInfo dependency = {
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
                rec_CmdPipelineBarrier2(cb, &dependency);
                VkBufferCopy copy = {destination_offset, 0, samples * 16};
                rec_CmdCopyBuffer(cb, destination, readback, 1, &copy);
                vk_finish();
                vk_readback_ready();
                for (u32 i = 0; i < samples; i++) {
                    float expected[4], got[4];
                    memcpy(got, actual + i * 16, 16);
                    nvn_advanced_blend_reference(mode, (int)pm, overlap, input[i], input[i] + 4, expected);
                    for (u32 c = 0; c < 4; c++) {
                        if (!(fabsf(got[c] - expected[c]) < 0.00005f))
                            fprintf(stderr,
                                    "blend equation mode%u pm%u overlap%u sample%u component%u: %g expected %g\n", mode,
                                    pm, overlap, i, c, got[c], expected[c]);
                        CHECK(fabsf(got[c] - expected[c]) < 0.00005f);
                    }
                    checks++;
                }
                vkDestroyPipeline(g_vk.dev, pipeline, NULL);
                vk_render_forget_state();
            }
    vk_unlock();
    vkDestroyPipelineLayout(g_vk.dev, layout, NULL);
    vkDestroyDescriptorSetLayout(g_vk.dev, descriptors, NULL);
    printf(
        "PASS NVN programmable blend equations: all46 modes, premultiplication, overlaps and %u endpoint colors checked=1\n",
        checks);
    return 0;
}
