/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The toast of the frontends with no menu (toast.h): a message in the common 8x16 VGA font, over the frame the
   presenter has just drawn. One pixel of a glyph is one rectangle, so no atlas or sampler is needed; the vertices go in
   a mapped buffer and one draw call fills the box and the letters. A buffer and a pipeline a recording slot keep: the
   frontends submit without waiting, and two windows (the 3DS's) draw the same message into different images before
   the first is done. */
#include "common/gpu/vk/toast.h"
#include "common/gpu/vk/fx_spv.h"
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOAST_MS     2200u     /* how long a message shows */
#define TOAST_TEXT   96        /* the message kept, its NUL included */
#define TOAST_VERTS  16384u    /* a buffer's room, rectangles of 6 vertices */
#define TOAST_STRIDE 24u       /* x, y, then r, g, b, a */
#define TOAST_SLOTS  8u        /* the most recording slots one toast serves */
#define TOAST_PIPES  8u        /* the target formats one toast draws into */

typedef struct {
    float x, y, r, g, b, a;
} ToastVert;

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    ToastVert* verts;
    int coherent;
} ToastSlot;

struct SnxToast {
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkPipelineCache cache;
    uint32_t slots;
    VkPipeline pipe[TOAST_PIPES];
    VkFormat pipe_format[TOAST_PIPES];
    uint32_t pipe_count;
    VkPipelineLayout layout;
    ToastSlot slot[TOAST_SLOTS];
    int failed;
    char text[TOAST_TEXT];
    uint64_t until_ns;
};

static VkShaderModule module(VkDevice dev, const uint32_t* words, uint64_t bytes) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = (size_t)bytes,
                                   .pCode = words};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

/* the pipeline for one target format: dynamic rendering, no depth, the colour blended over the frame */
static VkPipeline make_pipeline(SnxToast* t, VkFormat format) {
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkVertexInputBindingDescription binding = {0, TOAST_STRIDE, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                                       {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8}};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1,
                                                  .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL,
                                                     .cullMode = VK_CULL_MODE_NONE,
                                                     .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState attachment = {.blendEnable = VK_TRUE,
                                                      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                                                      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                      .colorBlendOp = VK_BLEND_OP_ADD,
                                                      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                      .alphaBlendOp = VK_BLEND_OP_ADD,
                                                      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                                                        VK_COLOR_COMPONENT_G_BIT |
                                                                        VK_COLOR_COMPONENT_B_BIT |
                                                                        VK_COLOR_COMPONENT_A_BIT};
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

static int make_slot(SnxToast* t, ToastSlot* s) {
    VkPhysicalDeviceMemoryProperties mp;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = TOAST_VERTS * TOAST_STRIDE,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(t->dev, &bi, NULL, &s->buffer) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(t->dev, s->buffer, &req);
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    ai = (VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
    ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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

SnxToast* snx_toast_create(VkPhysicalDevice phys, VkDevice dev, VkQueue queue, uint32_t family, VkPipelineCache cache,
                           uint32_t slots) {
    SnxToast* t = (SnxToast*)calloc(1, sizeof *t);
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (!t) return NULL;
    t->phys = phys;
    t->dev = dev;
    t->queue = queue;
    t->family = family;
    t->cache = cache;
    t->slots = slots < 1 ? 1 : slots > TOAST_SLOTS ? TOAST_SLOTS : slots;
    if (vkCreatePipelineLayout(dev, &li, NULL, &t->layout) != VK_SUCCESS) {
        fprintf(stderr, "toast: no pipeline layout\n");
        snx_toast_destroy(t);
        return NULL;
    }
    for (uint32_t i = 0; i < t->slots; ++i)
        if (!make_slot(t, &t->slot[i])) {
            fprintf(stderr, "toast: no mapped vertex buffer\n");
            snx_toast_destroy(t);
            return NULL;
        }
    return t;
}

void snx_toast_destroy(SnxToast* t) {
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

void snx_toast_show(SnxToast* t, const char* text, uint64_t now_ns) {
    if (!t || !text) return;
    snprintf(t->text, sizeof t->text, "%s", text);
    t->until_ns = now_ns + (uint64_t)TOAST_MS * 1000000u;
}

int snx_toast_active(const SnxToast* t, uint64_t now_ns) { return t && t->text[0] && now_ns < t->until_ns; }

/* one rectangle, its pixels turned into clip space (the target's y down, as the presenters blit) */
static void rect(ToastVert* v, uint32_t* n, float x, float y, float w, float h, float r, float g, float b, float a,
                 uint32_t width, uint32_t height) {
    float x0 = x / (float)width * 2.0f - 1.0f, x1 = (x + w) / (float)width * 2.0f - 1.0f;
    float y0 = y / (float)height * 2.0f - 1.0f, y1 = (y + h) / (float)height * 2.0f - 1.0f;
    ToastVert p[6] = {{x0, y0, r, g, b, a}, {x1, y0, r, g, b, a}, {x1, y1, r, g, b, a},
                      {x0, y0, r, g, b, a}, {x1, y1, r, g, b, a}, {x0, y1, r, g, b, a}};
    if (*n + 6 > TOAST_VERTS) return;
    memcpy(&v[*n], p, sizeof p);
    *n += 6;
}

int snx_toast_draw(SnxToast* t, VkCommandBuffer cb, VkImageView view, VkFormat format, uint32_t width, uint32_t height,
                   float white, uint32_t slot, uint64_t now_ns) {
    const char* text;
    unsigned len, i, row, col, scale;
    float gx, tw, th, x0, y0;
    uint32_t n = 0;
    VkPipeline pipe;
    ToastSlot* s;
    VkViewport vp;
    VkRect2D sc;
    VkRenderingAttachmentInfo at;
    VkRenderingInfo ri;
    VkDeviceSize offset = 0;
    if (!snx_toast_active(t, now_ns) || !width || !height || !cb || !view) return 0;
    if (t->failed) return 0;
    pipe = VK_NULL_HANDLE;
    for (uint32_t k = 0; k < t->pipe_count; ++k)
        if (t->pipe_format[k] == format) {
            pipe = t->pipe[k];
            break;
        }
    if (!pipe) {
        pipe = make_pipeline(t, format);
        if (!pipe || t->pipe_count == TOAST_PIPES) {
            if (pipe && t->pipe_count == TOAST_PIPES) vkDestroyPipeline(t->dev, pipe, NULL);
            fprintf(stderr, "toast: no pipeline for format %d\n", (int)format);
            t->failed = 1;
            return 0;
        }
        t->pipe[t->pipe_count] = pipe;
        t->pipe_format[t->pipe_count++] = format;
    }
    s = &t->slot[slot % t->slots];
    text = t->text;
    len = (unsigned)strlen(text);
    scale = height >= 400 ? 2u : 1u;
    while (scale > 1 && (len * 8u * scale) + 16u * scale > width) --scale;
    if ((len * 8u * scale) + 16u * scale > width) len = (width / (8u * scale)) > 2 ? width / (8u * scale) - 2 : 0;
    tw = (float)(len * 8u * scale);
    th = (float)(16u * scale);
    x0 = ((float)width - tw) * 0.5f;
    y0 = (float)(8u * scale);
    rect(s->verts, &n, x0 - 4.0f * (float)scale, y0 - 4.0f * (float)scale, tw + 8.0f * (float)scale,
         th + 8.0f * (float)scale, 0.0f, 0.0f, 0.0f, 0.75f, width, height);
    gx = x0;
    for (i = 0; i < len; ++i) {
        unsigned glyph = gui_glyph((unsigned char)text[i]);
        const uint8_t* rows = gui_vga_font + glyph * 16;
        for (row = 0; row < 16; ++row) {
            for (col = 0; col < 8; ++col) {
                if (!(rows[row] & (0x80u >> col))) continue;
                rect(s->verts, &n, gx + (float)(col * scale), y0 + (float)(row * scale), (float)scale, (float)scale,
                     white, white, white, 1.0f, width, height);
            }
        }
        gx += 8.0f * (float)scale;
    }
    if (!n) return 0;
    if (!s->coherent) {
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                     .memory = s->memory,
                                     .offset = 0,
                                     .size = VK_WHOLE_SIZE};
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
