/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The text renderer of text.h: one pixel of a glyph is one rectangle, so no atlas or sampler is needed; the vertices go
   in a mapped buffer and one draw call fills the box and the letters. A buffer and a pipeline a recording slot keep:
   the frontends submit without waiting, and two windows (the 3DS's) draw before the first is done. */
#include "common/gpu/vk/text.h"
#include "common/gpu/vk/fx_spv.h"
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_VERTS  16384u /* a buffer's room, rectangles of 6 vertices */
#define TEXT_STRIDE 24u    /* x, y, then r, g, b, a */
#define TEXT_SLOTS  8u     /* the most recording slots one renderer serves */
#define TEXT_PIPES  8u     /* the target formats one renderer draws into */

typedef struct {
    float x, y, r, g, b, a;
} TextVert;

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    TextVert* verts;
    int coherent;
} TextSlot;

struct SnxText {
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkPipelineCache cache;
    uint32_t slots;
    VkPipeline pipe[TEXT_PIPES];
    VkFormat pipe_format[TEXT_PIPES];
    uint32_t pipe_count;
    VkPipelineLayout layout;
    TextSlot slot[TEXT_SLOTS];
    int failed;
};

static VkShaderModule module(VkDevice dev, const uint32_t* words, uint64_t bytes) {
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)bytes, .pCode = words};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

/* the pipeline for one target format: dynamic rendering, no depth, the colour blended over the frame */
static VkPipeline make_pipeline(SnxText* t, VkFormat format) {
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkVertexInputBindingDescription binding = {0, TEXT_STRIDE, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                                       {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8}};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType =
                                                           VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                       .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {.sType =
                                                         VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL,
                                                     .cullMode = VK_CULL_MODE_NONE,
                                                     .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {.sType =
                                                            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState attachment = {.blendEnable = VK_TRUE,
                                                      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                                                      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                      .colorBlendOp = VK_BLEND_OP_ADD,
                                                      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                      .alphaBlendOp = VK_BLEND_OP_ADD,
                                                      .colorWriteMask =
                                                          VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                 .attachmentCount = 1,
                                                 .pAttachments = &attachment};
    VkDynamicState states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = 2,
                                                .pDynamicStates = states};
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                               .colorAttachmentCount = 1,
                                               .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &rendering,
                                       .stageCount = 2,
                                       .pStages = stages,
                                       .pVertexInputState = &vertex,
                                       .pInputAssemblyState = &assembly,
                                       .pViewportState = &viewport,
                                       .pRasterizationState = &raster,
                                       .pMultisampleState = &multisample,
                                       .pColorBlendState = &blend,
                                       .pDynamicState = &dynamic,
                                       .layout = t->layout};
    VkShaderModule v = module(t->dev, fx_toast_vert_spv, fx_toast_vert_spv_size);
    VkShaderModule f = module(t->dev, fx_toast_frag_spv, fx_toast_frag_spv_size);
    VkPipeline pipe = VK_NULL_HANDLE;
    stages[0] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                  .module = v,
                                                  .pName = "main"};
    stages[1] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                  .module = f,
                                                  .pName = "main"};
    vertex.vertexBindingDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &binding;
    vertex.vertexAttributeDescriptionCount = 2;
    vertex.pVertexAttributeDescriptions = attributes;
    if (v && f && vkCreateGraphicsPipelines(t->dev, t->cache, 1, &ci, NULL, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    if (v) vkDestroyShaderModule(t->dev, v, NULL);
    if (f) vkDestroyShaderModule(t->dev, f, NULL);
    return pipe;
}

static int make_slot(SnxText* t, TextSlot* s) {
    VkPhysicalDeviceMemoryProperties mp;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = TEXT_VERTS * TEXT_STRIDE,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(t->dev, &bi, NULL, &s->buffer) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(t->dev, s->buffer, &req);
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    ai = (VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
    ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    s->coherent = 1;
    if (ai.memoryTypeIndex == UINT32_MAX) {
        ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        s->coherent = 0;
    }
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(t->dev, &ai, NULL, &s->memory) != VK_SUCCESS ||
        vkBindBufferMemory(t->dev, s->buffer, s->memory, 0) != VK_SUCCESS ||
        vkMapMemory(t->dev, s->memory, 0, VK_WHOLE_SIZE, 0, (void**)&s->verts) != VK_SUCCESS || !s->verts)
        return 0;
    return 1;
}

SnxText* snx_text_create(VkPhysicalDevice phys, VkDevice dev, VkQueue queue, uint32_t family, VkPipelineCache cache,
                         uint32_t slots) {
    SnxText* t = (SnxText*)calloc(1, sizeof *t);
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (!t) return NULL;
    t->phys = phys;
    t->dev = dev;
    t->queue = queue;
    t->family = family;
    t->cache = cache;
    t->slots = slots < 1 ? 1 : slots > TEXT_SLOTS ? TEXT_SLOTS : slots;
    if (vkCreatePipelineLayout(dev, &li, NULL, &t->layout) != VK_SUCCESS) {
        fprintf(stderr, "text: no pipeline layout\n");
        snx_text_destroy(t);
        return NULL;
    }
    for (uint32_t i = 0; i < t->slots; ++i)
        if (!make_slot(t, &t->slot[i])) {
            fprintf(stderr, "text: no mapped vertex buffer\n");
            snx_text_destroy(t);
            return NULL;
        }
    return t;
}

void snx_text_destroy(SnxText* t) {
    if (!t) return;
    if (t->dev) {
        for (uint32_t i = 0; i < t->slots; ++i) {
            if (t->slot[i].verts) vkUnmapMemory(t->dev, t->slot[i].memory);
            if (t->slot[i].buffer) vkDestroyBuffer(t->dev, t->slot[i].buffer, NULL);
            if (t->slot[i].memory) vkFreeMemory(t->dev, t->slot[i].memory, NULL);
        }
        for (uint32_t i = 0; i < t->pipe_count; ++i)
            if (t->pipe[i]) vkDestroyPipeline(t->dev, t->pipe[i], NULL);
        if (t->layout) vkDestroyPipelineLayout(t->dev, t->layout, NULL);
    }
    free(t);
}

/* one rectangle, its pixels turned into clip space (the target's y down, as the presenters blit) */
static void rect(TextVert* v, uint32_t* n, float x, float y, float w, float h, float r, float g, float b, float a,
                 uint32_t width, uint32_t height) {
    float x0 = x / (float)width * 2.0f - 1.0f, x1 = (x + w) / (float)width * 2.0f - 1.0f;
    float y0 = y / (float)height * 2.0f - 1.0f, y1 = (y + h) / (float)height * 2.0f - 1.0f;
    TextVert p[6] = {{x0, y0, r, g, b, a}, {x1, y0, r, g, b, a}, {x1, y1, r, g, b, a},
                     {x0, y0, r, g, b, a}, {x1, y1, r, g, b, a}, {x0, y1, r, g, b, a}};
    if (*n + 6 > TEXT_VERTS) return;
    memcpy(&v[*n], p, sizeof p);
    *n += 6;
}

int snx_text_draw(SnxText* t, VkCommandBuffer cb, VkImageView view, VkFormat format, uint32_t width, uint32_t height,
                  float white, uint32_t slot, float x, float y, int scale, const char* const* lines, unsigned count) {
    unsigned i, row, col, longest = 0, len;
    float gx, pad;
    uint32_t n = 0;
    VkPipeline pipe;
    TextSlot* s;
    VkViewport vp;
    VkRect2D sc;
    VkRenderingAttachmentInfo at;
    VkRenderingInfo ri;
    VkDeviceSize offset = 0;
    if (!t || t->failed || !count || !lines || !cb || !view || !width || !height) return 0;
    if (scale < 1) scale = 1;
    for (i = 0; i < count; ++i) {
        len = lines[i] ? (unsigned)strlen(lines[i]) : 0;
        if (len > longest) longest = len;
    }
    if (!longest) return 0;
    pipe = VK_NULL_HANDLE;
    for (uint32_t k = 0; k < t->pipe_count; ++k)
        if (t->pipe_format[k] == format) {
            pipe = t->pipe[k];
            break;
        }
    if (!pipe) {
        pipe = make_pipeline(t, format);
        if (!pipe || t->pipe_count == TEXT_PIPES) {
            if (pipe && t->pipe_count == TEXT_PIPES) vkDestroyPipeline(t->dev, pipe, NULL);
            fprintf(stderr, "text: no pipeline for format %d\n", (int)format);
            t->failed = 1;
            return 0;
        }
        t->pipe[t->pipe_count] = pipe;
        t->pipe_format[t->pipe_count++] = format;
    }
    s = &t->slot[slot % t->slots];
    pad = 4.0f * (float)scale;
    rect(s->verts, &n, x - pad, y - pad, (float)(longest * 8u * (unsigned)scale) + 2.0f * pad,
         (float)(count * 16u * (unsigned)scale) + 2.0f * pad, 0.0f, 0.0f, 0.0f, 0.75f, width, height);
    for (i = 0; i < count; ++i) {
        if (!lines[i]) continue;
        gx = x;
        for (len = 0; lines[i][len]; ++len) {
            unsigned glyph = gui_glyph((unsigned char)lines[i][len]);
            const uint8_t* rows = gui_vga_font + glyph * 16;
            for (row = 0; row < 16; ++row) {
                for (col = 0; col < 8; ++col) {
                    if (!(rows[row] & (0x80u >> col))) continue;
                    rect(s->verts, &n, gx + (float)(col * (unsigned)scale),
                         y + (float)(i * 16u * (unsigned)scale) + (float)(row * (unsigned)scale), (float)scale,
                         (float)scale, white, white, white, 1.0f, width, height);
                }
            }
            gx += 8.0f * (float)scale;
        }
    }
    if (!n) return 0;
    if (!s->coherent) {
        VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = s->memory, .offset = 0, .size = VK_WHOLE_SIZE};
        vkFlushMappedMemoryRanges(t->dev, 1, &range);
    }
    at = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                     .imageView = view,
                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    ri = (VkRenderingInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                           .renderArea = {{0, 0}, {width, height}},
                           .layerCount = 1,
                           .colorAttachmentCount = 1,
                           .pColorAttachments = &at};
    vkCmdBeginRendering(cb, &ri);
    vp = (VkViewport){0, 0, (float)width, (float)height, 0.0f, 1.0f};
    sc = (VkRect2D){{0, 0}, {width, height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindVertexBuffers(cb, 0, 1, &s->buffer, &offset);
    vkCmdDraw(cb, n, 1, 0, 0);
    vkCmdEndRendering(cb);
    return 1;
}
