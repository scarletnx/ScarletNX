/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A raster copy supports quarter-turn presentation, which VkImageBlit cannot express. Each call uses a fresh
   descriptor set from the frame pool. Image views retire with that frame, so threaded recording retains them. */
#include "nx/gpu/vk/descriptor_objects.h"
#include "nx/gpu/vk/present_quad.h"
#include "nx/gpu/vk/present_quad_spv.h"
#include <stdlib.h>

static struct {
    VkDescriptorSetLayout descriptors;
    VkPipelineLayout layout;
    VkShaderModule vertex, fragment;
    VkSampler sampler[2];
    struct { VkFormat format; VkPipeline pipeline; }* pipelines;
    u32 count, capacity;
} g_present_quad;

static VkShaderModule module(const u32* code, size_t size) {
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule result = VK_NULL_HANDLE;
    return VK_OK(vkCreateShaderModule(g_vk.dev, &info, NULL, &result)) ? result : VK_NULL_HANDLE;
}
static int initialize(void) {
    if (g_present_quad.layout) return 1;
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo descriptors = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &descriptors, NULL, &g_present_quad.descriptors))) return 0;
    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount = 1, .pSetLayouts = &g_present_quad.descriptors,
                                          .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &layout, NULL, &g_present_quad.layout))) return 0;
    g_present_quad.vertex = module(present_quad_vert_spv, present_quad_vert_spv_size);
    g_present_quad.fragment = module(present_quad_frag_spv, present_quad_frag_spv_size);
    for (u32 i = 0; i < 2; i++) {
        VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                        .magFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                        .minFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        if (!VK_OK(vk_sampler_create(g_vk.dev, &sampler, NULL, &g_present_quad.sampler[i]))) return 0;
    }
    return g_present_quad.vertex && g_present_quad.fragment;
}
static VkPipeline pipeline(VkFormat format) {
    for (u32 i = 0; i < g_present_quad.count; i++)
        if (g_present_quad.pipelines[i].format == format) return g_present_quad.pipelines[i].pipeline;
    if (!initialize()) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = g_present_quad.vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = g_present_quad.fragment, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                       .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                    .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                    .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo samples = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                 .attachmentCount = 1, .pAttachments = &attachment};
    VkDynamicState states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                 .dynamicStateCount = 2, .pDynamicStates = states};
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                 .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
                                          .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
                                          .pInputAssemblyState = &assembly, .pViewportState = &viewport,
                                          .pRasterizationState = &raster, .pMultisampleState = &samples,
                                          .pColorBlendState = &blend, .pDynamicState = &dynamic,
                                          .layout = g_present_quad.layout};
    VkPipeline result;
    if (!VK_OK(vkCreateGraphicsPipelines(g_vk.dev, vk_pipeline_cache(), 1, &info, NULL, &result))) return VK_NULL_HANDLE;
    if (g_present_quad.count == g_present_quad.capacity) {
        u32 capacity = g_present_quad.capacity ? g_present_quad.capacity * 2 : 4;
        void* grown = capacity > UINT32_MAX / sizeof *g_present_quad.pipelines ? NULL
                      : realloc(g_present_quad.pipelines, capacity * sizeof *g_present_quad.pipelines);
        if (!grown) { vkDestroyPipeline(g_vk.dev, result, NULL); return VK_NULL_HANDLE; }
        g_present_quad.pipelines = grown;
        g_present_quad.capacity = capacity;
    }
    g_present_quad.pipelines[g_present_quad.count].format = format;
    g_present_quad.pipelines[g_present_quad.count++].pipeline = result;
    return result;
}
static VkImageView view(VkImage image, VkFormat format, VkImageUsageFlags usage) {
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = usage};
    VkImageViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &use, .image = image,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView result = VK_NULL_HANDLE;
    return VK_OK(vk_image_view_create(g_vk.dev, &info, NULL, &result)) ? result : VK_NULL_HANDLE;
}
int vk_present_quad(Surf* source, VkImage destination, VkFormat format, VkExtent2D extent,
                    VkRect2D source_rect, VkRect2D destination_rect, u32 transform, int linear) {
    if (!source || !source_rect.extent.width || !source_rect.extent.height ||
        !destination_rect.extent.width || !destination_rect.extent.height) return 0;
    VkPipeline p = pipeline(format);
    if (!p) return 0;
    VkDescriptorSet set = vk_desc_alloc(g_present_quad.descriptors);
    if (!set) return 0;
    VkImageView from = view(source->img, source->f.vk, VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImageView to = view(destination, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!from || !to) {
        if (from) vk_image_view_destroy(g_vk.dev, from, NULL);
        if (to) vk_image_view_destroy(g_vk.dev, to, NULL);
        return 0;
    }
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vk_barrier(cb, destination, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkDescriptorImageInfo image = {g_present_quad.sampler[!!linear], from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
                                   .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &image};
    vkUpdateDescriptorSets(g_vk.dev, 1, &write, 0, NULL);
    VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = to,
                                            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, extent},
                                  .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment};
    VkViewport viewport = {(float)destination_rect.offset.x, (float)destination_rect.offset.y,
                            (float)destination_rect.extent.width, (float)destination_rect.extent.height, 0, 1};
    struct { float crop[4]; u32 flags[4]; } push = {
        {(float)source_rect.offset.x / source->sw, (float)source_rect.offset.y / source->sh,
         (float)source_rect.extent.width / source->sw, (float)source_rect.extent.height / source->sh},
        {transform & 7, 0, 0, 0}};
    rec_CmdBeginRendering(cb, &rendering);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rec_CmdSetViewport(cb, 0, 1, &viewport);
    rec_CmdSetScissor(cb, 0, 1, &destination_rect);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_present_quad.layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_present_quad.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_render_forget_state();
    vk_barrier(cb, destination, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vk_defer_image(VK_NULL_HANDLE, NULL, from, to);
    return 1;
}
