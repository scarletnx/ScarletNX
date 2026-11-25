/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/rects.h"
#include "common/gpu/vk/rects_spv.h"
#include "common/gpu/vk/vk_common.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char* mapped;
    size_t room; /* rectangles it takes */
    const SnxUiRects* of;
    uint32_t version; /* the ones it holds, 0 for none */
} RectsBuffer;

#define RECTS_FORMATS 2 /* the windows' 8-bit UNORM formats: B8G8R8A8 and R8G8B8A8 */

struct SnxVkRects {
    VkDevice dev;
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t slots;
    int format_said; /* a window format it does not write was said */
    int failed;      /* the pipeline could not be made: said */
    VkPipelineLayout layout;
    VkPipeline pipelines[RECTS_FORMATS];
    RectsBuffer buffers[];
};

SnxVkRects* snx_vk_rects_create(VkPhysicalDevice phys, VkDevice dev, uint32_t slots) {
    SnxVkRects* vr = calloc(1, sizeof *vr + slots * sizeof(RectsBuffer));
    if (!vr) return NULL;
    vr->dev = dev;
    vr->slots = slots;
    vkGetPhysicalDeviceMemoryProperties(phys, &vr->memory);
    return vr;
}

static void buffer_free(SnxVkRects* vr, RectsBuffer* b) {
    if (b->memory) vkFreeMemory(vr->dev, b->memory, NULL); /* unmapped with it */
    if (b->buffer) vkDestroyBuffer(vr->dev, b->buffer, NULL);
    *b = (RectsBuffer){0};
}

void snx_vk_rects_destroy(SnxVkRects* vr) {
    if (!vr) return;
    for (uint32_t i = 0; i < vr->slots; ++i) buffer_free(vr, &vr->buffers[i]);
    for (uint32_t i = 0; i < RECTS_FORMATS; ++i)
        if (vr->pipelines[i]) vkDestroyPipeline(vr->dev, vr->pipelines[i], NULL);
    if (vr->layout) vkDestroyPipelineLayout(vr->dev, vr->layout, NULL);
    free(vr);
}

/* slot's buffer with rects in it, made again where it takes fewer, which its slot no longer reads */
static RectsBuffer* buffer_for(SnxVkRects* vr, uint32_t slot, const SnxUiRects* rects, void (*log)(const char* text)) {
    RectsBuffer* b = &vr->buffers[slot];
    if (b->room < rects->count) {
        size_t room = 512;
        while (room < rects->count) room *= 2;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = (VkDeviceSize)room * sizeof(SnxUiRect),
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkMemoryRequirements req;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        void* mapped = NULL;
        buffer_free(vr, b);
        VkResult r = vkCreateBuffer(vr->dev, &bi, NULL, &b->buffer);
        if (r == VK_SUCCESS) {
            vkGetBufferMemoryRequirements(vr->dev, b->buffer, &req);
            ai.allocationSize = req.size;
            ai.memoryTypeIndex =
                snx_vk_memory_type(&vr->memory, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            r = ai.memoryTypeIndex == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT
                                                 : vkAllocateMemory(vr->dev, &ai, NULL, &b->memory);
        }
        if (r == VK_SUCCESS) r = vkBindBufferMemory(vr->dev, b->buffer, b->memory, 0);
        if (r == VK_SUCCESS) r = vkMapMemory(vr->dev, b->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (r != VK_SUCCESS) {
            char text[160];
            snprintf(text, sizeof text,
                     "the on-screen controls' buffer of %llu bytes could not be made (%s): they are not drawn",
                     (unsigned long long)bi.size, snx_vk_result_str(r));
            if (log) log(text);
            buffer_free(vr, b);
            return NULL;
        }
        b->mapped = mapped;
        b->room = room;
    }
    if (b->of != rects || b->version != rects->version) {
        memcpy(b->mapped, rects->rect, rects->count * sizeof *rects->rect);
        b->of = rects;
        b->version = rects->version;
    }
    return b;
}

/* the pipeline for format (RECTS_FORMATS' at index), made at the first draw into a window of it: VK_NULL_HANDLE where
   it can't be, said once */
static VkPipeline pipeline_for(SnxVkRects* vr, uint32_t index, VkFormat format, void (*log)(const char* text)) {
    if (vr->pipelines[index] || vr->failed) return vr->pipelines[index];
    VkResult r = VK_SUCCESS;
    if (!vr->layout) {
        VkPushConstantRange window = {VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float)};
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &window};
        r = vkCreatePipelineLayout(vr->dev, &pl, NULL, &vr->layout);
    }
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = (size_t)rects_vert_spv_size,
                                   .pCode = rects_vert_spv};
    VkShaderModuleCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = (size_t)rects_frag_spv_size,
                                   .pCode = rects_frag_spv};
    if (r == VK_SUCCESS) r = vkCreateShaderModule(vr->dev, &vi, NULL, &vs);
    if (r == VK_SUCCESS) r = vkCreateShaderModule(vr->dev, &fi, NULL, &fs);
    if (r == VK_SUCCESS) {
        VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                  .module = vs,
                                                  .pName = "main"},
                                                 {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                  .module = fs,
                                                  .pName = "main"}};
        VkVertexInputBindingDescription vb = {0, sizeof(SnxUiRect), VK_VERTEX_INPUT_RATE_INSTANCE};
        VkVertexInputAttributeDescription va[3] = {{0, 0, VK_FORMAT_R16G16_SINT, offsetof(SnxUiRect, x)},
                                                   {1, 0, VK_FORMAT_R16G16_UINT, offsetof(SnxUiRect, w)},
                                                   {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(SnxUiRect, rgba)}};
        VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                    .vertexBindingDescriptionCount = 1,
                                                    .pVertexBindingDescriptions = &vb,
                                                    .vertexAttributeDescriptionCount = 3,
                                                    .pVertexAttributeDescriptions = va};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType =
                                                         VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                     .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo vp = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs = {.sType =
                                                         VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL,
                                                     .cullMode = VK_CULL_MODE_NONE,
                                                     .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
        VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                   .attachmentCount = 1,
                                                   .pAttachments = &ba};
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                               .dynamicStateCount = 2,
                                               .pDynamicStates = dyn};
        VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachmentFormats = &format};
        VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                           .pNext = &ri,
                                           .stageCount = 2,
                                           .pStages = st,
                                           .pVertexInputState = &vin,
                                           .pInputAssemblyState = &ia,
                                           .pViewportState = &vp,
                                           .pRasterizationState = &rs,
                                           .pMultisampleState = &ms,
                                           .pColorBlendState = &cbs,
                                           .pDynamicState = &ds,
                                           .layout = vr->layout};
        r = vkCreateGraphicsPipelines(vr->dev, VK_NULL_HANDLE, 1, &ci, NULL, &vr->pipelines[index]);
        if (r != VK_SUCCESS) vr->pipelines[index] = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(vr->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(vr->dev, fs, NULL);
    if (r != VK_SUCCESS) {
        char text[160];
        vr->failed = 1;
        snprintf(text, sizeof text,
                 "the on-screen controls' draw for a window of format %d could not be made (%s): they are not drawn",
                 (int)format, snx_vk_result_str(r));
        if (log) log(text);
    }
    return vr->pipelines[index];
}

int snx_vk_rects_draw(SnxVkRects* vr, VkCommandBuffer cb, uint32_t slot, VkFormat format, uint32_t width,
                      uint32_t height, const SnxUiRects* rects, void (*log)(const char* text)) {
    if (!vr || slot >= vr->slots || !rects || !rects->count) return 0;
    if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_R8G8B8A8_UNORM) {
        if (!vr->format_said++ && log) {
            char text[160];
            snprintf(text, sizeof text,
                     "the on-screen controls are not drawn on a window of format %d, only on 8-bit UNORM RGBA or BGRA",
                     (int)format);
            log(text);
        }
        return 0;
    }
    VkPipeline pipeline = pipeline_for(vr, format == VK_FORMAT_B8G8R8A8_UNORM ? 0 : 1, format, log);
    RectsBuffer* b = pipeline ? buffer_for(vr, slot, rects, log) : NULL;
    if (!b) return 0;
    VkViewport vp = {0, 0, (float)width, (float)height, 0, 1};
    VkRect2D sc = {{0, 0}, {width, height}};
    VkDeviceSize at = 0;
    float size[2] = {(float)width, (float)height};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindVertexBuffers(cb, 0, 1, &b->buffer, &at);
    vkCmdPushConstants(cb, vr->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof size, size);
    vkCmdDraw(cb, 6, (uint32_t)rects->count, 0, 0);
    return 1;
}

int snx_vk_rects_record(SnxVkRects* vr, VkCommandBuffer cb, uint32_t slot, VkImage image, VkImageView view,
                        VkFormat format, uint32_t width, uint32_t height, const SnxUiRects* rects,
                        void (*log)(const char* text)) {
    if (!vr || !rects || !rects->count) return 0;
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask =
                                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                       .imageView = view,
                                       .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                       .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, {width, height}},
                          .layerCount = 1,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = &color};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL,
                         0, NULL, 1, &b);
    vkCmdBeginRendering(cb, &ri);
    int drawn = snx_vk_rects_draw(vr, cb, slot, format, width, height, rects, log);
    vkCmdEndRendering(cb);
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                         0, NULL, 1, &b);
    return drawn;
}
