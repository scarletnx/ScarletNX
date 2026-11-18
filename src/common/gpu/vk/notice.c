/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/notice.h"
#include "common/gpu/vk/fx_spv.h"
#include "common/gpu/vk/notice_spv.h"
#include "common/gpu/vk/vk_common.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char* mapped;
    VkDeviceSize size;
    VkDescriptorSet set; /* the draw's, which reads the buffer it holds */
    VkBuffer holds;
} NoticeBuffer;

#define NOTICE_FORMATS 2 /* the windows' 8-bit UNORM formats the draw writes: B8G8R8A8 and R8G8B8A8 */

struct SnxVkNotice {
    VkDevice dev;
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t slots;
    int format_said; /* a window format it does not write was said */
    /* snx_vk_notice_draw's, made at its first: the band read from a slot's buffer, a pipeline for each format */
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkDescriptorPool pool;
    VkPipeline pipelines[NOTICE_FORMATS];
    int draw_failed; /* said */
    NoticeBuffer buffers[];
};

SnxVkNotice* snx_vk_notice_create(VkPhysicalDevice phys, VkDevice dev, uint32_t slots) {
    SnxVkNotice* vn = calloc(1, sizeof *vn + slots * sizeof(NoticeBuffer));
    if (!vn) return NULL;
    vn->dev = dev;
    vn->slots = slots;
    vkGetPhysicalDeviceMemoryProperties(phys, &vn->memory);
    return vn;
}

/* the buffer gone, its set kept for the next one */
static void buffer_free(SnxVkNotice* vn, NoticeBuffer* b) {
    if (b->memory) vkFreeMemory(vn->dev, b->memory, NULL); /* unmapped with it */
    if (b->buffer) vkDestroyBuffer(vn->dev, b->buffer, NULL);
    *b = (NoticeBuffer){.set = b->set};
}

void snx_vk_notice_destroy(SnxVkNotice* vn) {
    if (!vn) return;
    for (uint32_t i = 0; i < vn->slots; ++i) buffer_free(vn, &vn->buffers[i]);
    for (uint32_t i = 0; i < NOTICE_FORMATS; ++i)
        if (vn->pipelines[i]) vkDestroyPipeline(vn->dev, vn->pipelines[i], NULL);
    if (vn->pool) vkDestroyDescriptorPool(vn->dev, vn->pool, NULL); /* its sets with it */
    if (vn->layout) vkDestroyPipelineLayout(vn->dev, vn->layout, NULL);
    if (vn->set_layout) vkDestroyDescriptorSetLayout(vn->dev, vn->set_layout, NULL);
    free(vn);
}

/* slot's buffer, size bytes at least: made again where it holds less, which its slot no longer reads */
static NoticeBuffer* buffer_for(SnxVkNotice* vn, uint32_t slot, VkDeviceSize size, void (*log)(const char* text)) {
    NoticeBuffer* b = &vn->buffers[slot];
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkResult r;
    void* mapped = NULL;
    char text[160];
    if (b->size >= size) return b;
    buffer_free(vn, b);
    r = vkCreateBuffer(vn->dev, &bi, NULL, &b->buffer);
    if (r == VK_SUCCESS) {
        vkGetBufferMemoryRequirements(vn->dev, b->buffer, &req);
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            snx_vk_memory_type(&vn->memory, req.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        r = ai.memoryTypeIndex == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT
                                             : vkAllocateMemory(vn->dev, &ai, NULL, &b->memory);
    }
    if (r == VK_SUCCESS) r = vkBindBufferMemory(vn->dev, b->buffer, b->memory, 0);
    if (r == VK_SUCCESS) r = vkMapMemory(vn->dev, b->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r != VK_SUCCESS) {
        snprintf(text, sizeof text, "the notice's buffer of %llu bytes could not be made (%s): it is not drawn",
                 (unsigned long long)size, snx_vk_result_str(r));
        if (log) log(text);
        buffer_free(vn, b);
        return NULL;
    }
    b->mapped = mapped;
    b->size = size;
    return b;
}

int snx_vk_notice_record(SnxVkNotice* vn, VkCommandBuffer cb, uint32_t slot, VkImage image, VkFormat format,
                         uint32_t width, uint32_t height, int percent, const SnxNotice* n, uint64_t now_ns,
                         void (*log)(const char* text)) {
    int rect[4], scale, bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    NoticeBuffer* b;
    if (!vn || slot >= vn->slots || !snx_notice_shown(n, now_ns)) return 0;
    if (!bgra && format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB) {
        if (!vn->format_said++ && log) {
            char text[128];
            snprintf(text, sizeof text, "a notice is not drawn on a window of format %d, only on 8-bit RGBA or BGRA",
                     (int)format);
            log(text);
        }
        return 0;
    }
    scale = snx_notice_scale((int)width, (int)height, percent);
    if (!snx_notice_rect(n, (int)width, (int)height, scale, rect)) return 0;
    b = buffer_for(vn, slot, (VkDeviceSize)rect[2] * (VkDeviceSize)rect[3] * 4u, log);
    if (!b) return 0;
    snx_notice_pixels(n, scale, bgra, rect, b->mapped, (size_t)rect[2] * 4u);
    /* after the picture's own copies and blits to the image, whose pixels the band's replace */
    VkImageMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = image,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &after);
    VkBufferImageCopy copy = {.bufferRowLength = (uint32_t)rect[2],
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageOffset = {rect[0], rect[1], 0},
                              .imageExtent = {(uint32_t)rect[2], (uint32_t)rect[3], 1}};
    vkCmdCopyBufferToImage(cb, b->buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    return 1;
}

/* the draw's layouts and sets, then its pipeline for format (one of NOTICE_FORMATS' at index), made at the first draw
   into a window of it: VK_NULL_HANDLE where they can't be, said once */
static VkPipeline draw_pipeline(SnxVkNotice* vn, uint32_t index, VkFormat format, void (*log)(const char* text)) {
    VkResult r = VK_SUCCESS;
    if (vn->pipelines[index] || vn->draw_failed) return vn->pipelines[index];
    if (!vn->layout) {
        VkDescriptorSetLayoutBinding band = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                             NULL};
        VkDescriptorSetLayoutCreateInfo dl = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &band};
        VkPushConstantRange rect = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * sizeof(int32_t)};
        VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vn->slots};
        VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                         .maxSets = vn->slots,
                                         .poolSizeCount = 1,
                                         .pPoolSizes = &size};
        r = vkCreateDescriptorSetLayout(vn->dev, &dl, NULL, &vn->set_layout);
        if (r == VK_SUCCESS) {
            VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                             .setLayoutCount = 1,
                                             .pSetLayouts = &vn->set_layout,
                                             .pushConstantRangeCount = 1,
                                             .pPushConstantRanges = &rect};
            r = vkCreatePipelineLayout(vn->dev, &pl, NULL, &vn->layout);
        }
        if (r == VK_SUCCESS) r = vkCreateDescriptorPool(vn->dev, &dp, NULL, &vn->pool);
        for (uint32_t i = 0; r == VK_SUCCESS && i < vn->slots; ++i) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = vn->pool,
                                              .descriptorSetCount = 1,
                                              .pSetLayouts = &vn->set_layout};
            r = vkAllocateDescriptorSets(vn->dev, &ai, &vn->buffers[i].set);
        }
    }
    if (r == VK_SUCCESS) {
        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = (size_t)fx_fsr_vert_spv_size,
                                       .pCode = fx_fsr_vert_spv};
        VkShaderModuleCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = (size_t)notice_frag_spv_size,
                                       .pCode = notice_frag_spv};
        r = vkCreateShaderModule(vn->dev, &vi, NULL, &vs);
        if (r == VK_SUCCESS) r = vkCreateShaderModule(vn->dev, &fi, NULL, &fs);
        if (r == VK_SUCCESS) {
            VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                      .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                      .module = vs,
                                                      .pName = "main"},
                                                     {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                      .module = fs,
                                                      .pName = "main"}};
            VkPipelineVertexInputStateCreateInfo vin = {.sType =
                                                            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
            VkPipelineViewportStateCreateInfo vp = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
            VkPipelineRasterizationStateCreateInfo rs = {.sType =
                                                             VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                         .polygonMode = VK_POLYGON_MODE_FILL,
                                                         .cullMode = VK_CULL_MODE_NONE,
                                                         .lineWidth = 1};
            VkPipelineMultisampleStateCreateInfo ms = {.sType =
                                                           VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                       .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
            VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
            VkPipelineColorBlendStateCreateInfo cbs = {.sType =
                                                           VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
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
                                               .layout = vn->layout};
            r = vkCreateGraphicsPipelines(vn->dev, VK_NULL_HANDLE, 1, &ci, NULL, &vn->pipelines[index]);
            if (r != VK_SUCCESS) vn->pipelines[index] = VK_NULL_HANDLE;
        }
        if (vs) vkDestroyShaderModule(vn->dev, vs, NULL);
        if (fs) vkDestroyShaderModule(vn->dev, fs, NULL);
    }
    if (r != VK_SUCCESS) {
        vn->draw_failed = 1;
        if (log) {
            char text[160];
            snprintf(text, sizeof text,
                     "the notice's draw for a window of format %d could not be made (%s): no notice "
                     "is drawn",
                     (int)format, snx_vk_result_str(r));
            log(text);
        }
    }
    return vn->pipelines[index];
}

int snx_vk_notice_draw(SnxVkNotice* vn, VkCommandBuffer cb, uint32_t slot, VkFormat format, uint32_t width,
                       uint32_t height, int percent, const SnxNotice* n, uint64_t now_ns,
                       void (*log)(const char* text)) {
    int rect[4], scale;
    uint32_t index = format == VK_FORMAT_B8G8R8A8_UNORM ? 0 : 1;
    NoticeBuffer* b;
    VkPipeline pipeline;
    if (!vn || slot >= vn->slots || !snx_notice_shown(n, now_ns)) return 0;
    if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_R8G8B8A8_UNORM) {
        if (!vn->format_said++ && log) {
            char text[128];
            snprintf(text, sizeof text,
                     "a notice is not drawn on a window of format %d, only on 8-bit UNORM RGBA or BGRA", (int)format);
            log(text);
        }
        return 0;
    }
    scale = snx_notice_scale((int)width, (int)height, percent);
    if (!snx_notice_rect(n, (int)width, (int)height, scale, rect)) return 0;
    if (!(pipeline = draw_pipeline(vn, index, format, log))) return 0;
    b = buffer_for(vn, slot, (VkDeviceSize)rect[2] * (VkDeviceSize)rect[3] * 4u, log);
    if (!b) return 0;
    /* R G B A: the attachment's format puts each channel where its bytes keep it */
    snx_notice_pixels(n, scale, 0, rect, b->mapped, (size_t)rect[2] * 4u);
    if (b->holds != b->buffer) { /* a buffer made since the set was written; the slot's last draw is done with it */
        VkDescriptorBufferInfo bi = {b->buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = b->set,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   .pBufferInfo = &bi};
        vkUpdateDescriptorSets(vn->dev, 1, &wr, 0, NULL);
        b->holds = b->buffer;
    }
    {
        VkViewport vp = {(float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], 0, 1};
        VkRect2D sc = {{rect[0], rect[1]}, {(uint32_t)rect[2], (uint32_t)rect[3]}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vn->layout, 0, 1, &b->set, 0, NULL);
        vkCmdPushConstants(cb, vn->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof rect, rect);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    return 1;
}
