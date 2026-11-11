/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen controls, the game's menu (runtime/platform/ui.h) and the shader loading screen before the game
   (runtime/platform/prepare_screen.h) over a presented frame: the quads they give, blended onto the swapchain image
   in a pass of their own after the frame's blit. Nothing is recorded while none shows.

   Each quad is an instance, its SnxUiQuad as it is, whose corners the vertex shader makes: the CPU writes no
   vertices. The instances stay in a buffer on the device from frame to frame, and a frame copies into it only the
   quads that differ from the ones it holds. The stats box and the debug line keep their layout and their quads in the
   same places (ui_cells), so a frame over the game sends a few letters and the graph's bars, or nothing */
#include "nx/gpu/vk/descriptor_objects.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/ui_spv.h"
#include "common/utilities/endian.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/vpad.h"

#define MAX_QUADS 4096
#define UI_COPIES 16 /* the runs of changed quads one frame copies at most: past them, one copy to the last */
#define UI_GAP    4  /* unchanged quads between two changed ones that one copy takes rather than two */
static SnxUiQuad g_quads[MAX_QUADS];
static SnxUiQuad g_sent[MAX_QUADS]; /* what g_inst holds, as the device reads it */
static int g_sent_n;                /* g_sent's quads that g_inst holds: none for a new buffer */
static VkBuffer g_inst;
static VmaAllocation g_inst_mem;
static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_layout;
static VkPipeline g_pipe;
static VkFormat g_pipe_format;
static VkDescriptorPool g_pool;
/* by a quad's sample: the atlas blended (sprites), at its nearest texel (the font, fills), and the picture blended */
static VkDescriptorSet g_sets[3];
static VkSampler g_samplers[2];
static VkImage g_atlas;
static VmaAllocation g_atlas_mem;
static VkImageView g_atlas_view;
static u32 g_atlas_version;
static VkImage g_image; /* ui_image's picture */
static VmaAllocation g_image_mem;
static VkImageView g_image_view;
static u32 g_image_version;
static VkImage g_sc_img[VK_SC_MAX];
static VkImageView g_sc_view[VK_SC_MAX];

static int layout_make(void) {
    VkDescriptorSetLayoutBinding b = {.binding = 0,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .descriptorCount = 1,
                                      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &g_dsl,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &pc};
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 3, .poolSizeCount = 1, .pPoolSizes = &size};
    VkDescriptorSetLayout layouts[3];
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 3};
    int i;
    if (g_layout) return 1;
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dci, NULL, &g_dsl))) return 0;
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &lci, NULL, &g_layout))) return 0;
    if (!VK_OK(vkCreateDescriptorPool(g_vk.dev, &pci, NULL, &g_pool))) return 0;
    layouts[0] = layouts[1] = layouts[2] = g_dsl;
    ai.descriptorPool = g_pool;
    ai.pSetLayouts = layouts;
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, g_sets))) return 0;
    for (i = 0; i < 2; ++i) {
        VkFilter f = i ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        /* in texels, as the quads give them: no division by the image's size, on the CPU or in the shaders */
        VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                   .magFilter = f,
                                   .minFilter = f,
                                   .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                   .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .unnormalizedCoordinates = VK_TRUE};
        if (!VK_OK(vk_sampler_create(g_vk.dev, &sci, NULL, &g_samplers[i]))) return 0;
    }
    return 1;
}

static int pipeline_make(VkFormat format) {
    VkShaderModule vs, fs;
    VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                              .pName = "main"},
                                             {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                              .pName = "main"}};
    /* a quad an instance, read as ui.h lays it out */
    VkVertexInputBindingDescription vb = {0, sizeof(SnxUiQuad), VK_VERTEX_INPUT_RATE_INSTANCE};
    VkVertexInputAttributeDescription va[4] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SnxUiQuad, x)},
                                               {1, 0, VK_FORMAT_R16G16B16A16_UINT, offsetof(SnxUiQuad, u)},
                                               {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(SnxUiQuad, rgba)},
                                               {3, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(SnxUiQuad, turn)}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1,
                                               .pVertexBindingDescriptions = &vb,
                                               .vertexAttributeDescriptionCount = 4,
                                               .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL,
                                                 .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    /* alpha premultiplied: the atlas is, and the shader multiplies the colour's in */
    VkPipelineColorBlendAttachmentState ba = {.blendEnable = VK_TRUE,
                                              .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                              .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              .colorBlendOp = VK_BLEND_OP_ADD,
                                              .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                              .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              .alphaBlendOp = VK_BLEND_OP_ADD,
                                              .colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo gci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        .pNext = &ri,
                                        .stageCount = 2,
                                        .pStages = st,
                                        .pVertexInputState = &vi,
                                        .pInputAssemblyState = &ia,
                                        .pViewportState = &vp,
                                        .pRasterizationState = &rs,
                                        .pMultisampleState = &ms,
                                        .pColorBlendState = &cb,
                                        .pDynamicState = &ds,
                                        .layout = g_layout};
    int ok;
    if (g_pipe && g_pipe_format == format) return 1;
    if (g_pipe) vkDestroyPipeline(g_vk.dev, g_pipe, NULL); /* vk_finish ran when the swapchain changed format */
    g_pipe = VK_NULL_HANDLE;
    vs = vk_module_spv(snx_ui_vs_spv, (size_t)(snx_ui_vs_spv_size / 4));
    fs = vk_module_spv(snx_ui_fs_spv, (size_t)(snx_ui_fs_spv_size / 4));
    st[0].module = vs;
    st[1].module = fs;
    ok = vs && fs && VK_OK(vkCreateGraphicsPipelines(g_vk.dev, vk_pipeline_cache(), 1, &gci, NULL, &g_pipe));
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    g_pipe_format = format;
    if (!ok) g_pipe = VK_NULL_HANDLE;
    return ok;
}

/* the atlas to the GPU, once each time the frontend gives one (under ui_lock) */
static void atlas_upload(VkCommandBuffer cb) {
    int w, h, i;
    u32 version;
    const u32* px = ui_atlas(&w, &h, &version);
    VkBuffer buf;
    VmaAllocation mem;
    VmaAllocationInfo info;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = (VkDeviceSize)w * (VkDeviceSize)h * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo bai = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {(u32)w, (u32)h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo iai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {(u32)w, (u32)h, 1}};
    if (!px || version == g_atlas_version) return;
    if (!VK_OK(vk_buffer_create(g_vk.vma, &bci, &bai, &buf, &mem, &info))) return;
    vk_mem_track(mem);
    memcpy(info.pMappedData, px, (size_t)w * (size_t)h * 4);
    vmaFlushAllocation(g_vk.vma, mem, 0, VK_WHOLE_SIZE);
    vk_defer_buffer(buf, mem);
    if (g_atlas) vk_defer_image(g_atlas, g_atlas_mem, g_atlas_view, VK_NULL_HANDLE);
    g_atlas = VK_NULL_HANDLE;
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &iai, &g_atlas, &g_atlas_mem, NULL))) {
        g_atlas = VK_NULL_HANDLE;
        return;
    }
    vk_mem_track(g_atlas_mem);
    vci.image = g_atlas;
    if (!VK_OK(vk_image_view_create(g_vk.dev, &vci, NULL, &g_atlas_view))) {
        vk_mem_untrack(g_atlas_mem);
        vmaDestroyImage(g_vk.vma, g_atlas, g_atlas_mem);
        g_atlas = VK_NULL_HANDLE;
        return;
    }
    vk_barrier(cb, g_atlas, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rec_CmdCopyBufferToImage(cb, buf, g_atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_barrier(cb, g_atlas, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (i = 0; i < 2; ++i) {
        VkDescriptorImageInfo di = {g_samplers[i], g_atlas_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = g_sets[i],
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &di};
        vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    }
    g_atlas_version = version;
    ui_atlas_done();
    LOG("vulkan: the on-screen controls' atlas is %dx%d\n", w, h);
}

/* ui_image's picture to the GPU when it changed (under ui_lock): one image made the first time, written over after */
static void image_upload(VkCommandBuffer cb) {
    u32 version;
    const u8* px = ui_image_get(&version);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {SNX_UI_IMAGE_W, SNX_UI_IMAGE_H, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo iai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {SNX_UI_IMAGE_W, SNX_UI_IMAGE_H, 1}};
    const u64 size = (u64)SNX_UI_IMAGE_W * SNX_UI_IMAGE_H * 4;
    VkBuffer buf;
    u64 off;
    u8* to;
    if (!px || version == g_image_version) return;
    if (!g_image) {
        VkDescriptorImageInfo di = {g_samplers[0], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = g_sets[UI_IMAGE],
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &di};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &iai, &g_image, &g_image_mem, NULL))) {
            g_image = VK_NULL_HANDLE;
            return;
        }
        vci.image = g_image;
        if (!VK_OK(vk_image_view_create(g_vk.dev, &vci, NULL, &g_image_view))) {
            vmaDestroyImage(g_vk.vma, g_image, g_image_mem);
            g_image = VK_NULL_HANDLE;
            return;
        }
        di.imageView = g_image_view;
        vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    }
    to = (u8*)vk_stage(size, 4, &buf, &off);
    if (!to) return;
    memcpy(to, px, (size_t)size);
    copy.bufferOffset = off;
    /* the last frame's draws sampled it: they finish before the copy writes over it */
    vk_barrier(cb, g_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rec_CmdCopyBufferToImage(cb, buf, g_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_barrier(cb, g_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g_image_version = version;
}

/* the swapchain image's view, made once per image */
static VkImageView sc_view(VkImage sc) {
    u32 i;
    for (i = 0; i < VK_SC_MAX; ++i)
        if (g_sc_img[i] == sc) return g_sc_view[i];
    for (i = 0; i < VK_SC_MAX; ++i)
        if (!g_sc_img[i]) {
            VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = sc,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = g_vk.sc_format,
                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            if (!VK_OK(vk_image_view_create(g_vk.dev, &vci, NULL, &g_sc_view[i]))) return VK_NULL_HANDLE;
            g_sc_img[i] = sc;
            return g_sc_view[i];
        }
    return VK_NULL_HANDLE;
}
void vk_ui_swapchain_gone(void) {
    u32 i;
    for (i = 0; i < VK_SC_MAX; ++i) {
        if (g_sc_view[i]) vk_image_view_destroy(g_vk.dev, g_sc_view[i], NULL);
        g_sc_view[i] = VK_NULL_HANDLE;
        g_sc_img[i] = VK_NULL_HANDLE;
    }
}

/* the device's buffer of instances, made once */
static int instances_make(void) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = sizeof g_quads,
                              .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationInfo info;
    if (g_inst) return 1;
    if (!VK_OK(vk_buffer_create(g_vk.vma, &bci, &ai, &g_inst, &g_inst_mem, &info))) {
        g_inst = VK_NULL_HANDLE;
        return 0;
    }
    vk_mem_track(g_inst_mem);
    g_sent_n = 0;
    return 1;
}
static int quad_same(int i) { return i < g_sent_n && !memcmp(&g_quads[i], &g_sent[i], sizeof *g_quads); }
/* the quads from first to end as the device reads them, at to: little-endian words, and halves for the texels */
static void quads_le(u8* to, int first, int end) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    int i;
    for (i = first; i < end; ++i, to += sizeof(SnxUiQuad)) {
        SnxUiQuad q = g_quads[i];
        snx_le32_array(&q.x, 4);
        snx_le16_array(&q.u, 4);
        q.rgba = snx_le32(q.rgba);
        memcpy(to, &q, sizeof q);
    }
#else
    memcpy(to, &g_quads[first], (size_t)(end - first) * sizeof(SnxUiQuad));
#endif
}
/* the n quads into the device's buffer: only the runs that differ from what it holds, staged and copied after the
   last frame's draws read it, before this one's. 0 when it holds none of them (no buffer, or no staging) */
static int instances_send(VkCommandBuffer cb, int n) {
    int start[UI_COPIES], end[UI_COPIES], runs = 0, quads = 0, i, k;
    VkBufferCopy copy[UI_COPIES];
    VkBufferMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .size = VK_WHOLE_SIZE};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b};
    VkBuffer buf;
    u64 off;
    u8* to;
    if (!instances_make()) return 0;
    for (i = 0; i < n;) {
        int e = i + 1;
        if (quad_same(i)) {
            ++i;
            continue;
        }
        /* a run from here to its last changed quad that has fewer than UI_GAP same ones before the next */
        for (k = e; k < n && k < e + UI_GAP; ++k)
            if (!quad_same(k)) e = k + 1;
        if (runs == UI_COPIES) /* too many: the last one goes on over this one */
            end[runs - 1] = e;
        else {
            start[runs] = i;
            end[runs++] = e;
        }
        i = e;
    }
    if (!runs) return 1; /* it holds them all */
    for (k = 0; k < runs; ++k) quads += end[k] - start[k];
    to = vk_stage((u64)quads * sizeof(SnxUiQuad), 16, &buf, &off);
    if (!to) return 0;
    for (k = 0, quads = 0; k < runs; ++k) {
        int count = end[k] - start[k];
        quads_le(to + (size_t)quads * sizeof(SnxUiQuad), start[k], end[k]);
        copy[k] = (VkBufferCopy){.srcOffset = off + (u64)quads * sizeof(SnxUiQuad),
                                 .dstOffset = (u64)start[k] * sizeof(SnxUiQuad),
                                 .size = (u64)count * sizeof(SnxUiQuad)};
        memcpy(&g_sent[start[k]], &g_quads[start[k]], (size_t)count * sizeof(SnxUiQuad));
        quads += count;
    }
    if (n > g_sent_n) g_sent_n = n; /* the quads past the old end were all sent */
    /* the last frame's draws read it before the copy writes, and the copy writes before this frame's draws read */
    b.buffer = g_inst;
    rec_CmdPipelineBarrier2(cb, &d);
    rec_CmdCopyBuffer(cb, buf, g_inst, (u32)runs, copy);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    rec_CmdPipelineBarrier2(cb, &d);
    return 1;
}
void vk_ui_draw(VkCommandBuffer cb, VkImage sc, const int safe[4]) {
    SnxUiList list = {g_quads, 0, MAX_QUADS};
    int sw = (int)g_vk.sc_extent.width, sh = (int)g_vk.sc_extent.height, n, i, run;
    VkImageView view;
    struct {
        float w, h;
        u32 srgb, pad;
    } pc = {(float)sw, (float)sh, (u32)g_vk.sc_srgb, 0};
    if (!layout_make()) return;
    ui_lock();
    atlas_upload(cb);
    image_upload(cb);
    /* the screen before the game starts, under the menu, hides the on-screen controls: they aren't drawn under it,
       where they would take quads for nothing (nor do they take fingers then, sdl3.c's press_event) */
    if (prepare_screen_shown())
        prepare_screen_draw(&list, sw, sh, safe, overlay_display_percent());
    else {
        int px, py, scale;
        vpad_draw(&list, safe);
        vpad_showcase_draw(&list, safe, vpad_showcase_fps(g_vk_present, plat_ns()));
        overlay_place(sw, sh, &px, &py, &scale); /* the menu's letters */
        stats_draw(&list, safe, scale, plat_ns());
    }
    {
        int px, py, scale;
        overlay_place(sw, sh, &px, &py, &scale); /* the menu's letters */
        hud_draw(&list, safe, scale);
    }
    overlay_draw(&list, sw, sh, plat_ns());
    ui_unlock();
    n = list.n;
    if (!n || !g_atlas || !pipeline_make(g_vk.sc_format) || !(view = sc_view(sc))) return;
    if (!instances_send(cb, n)) return;
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    {
        VkRenderingAttachmentInfo at = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                        .imageView = view,
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {{0, 0}, g_vk.sc_extent},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &at};
        VkViewport vp = {0, 0, (float)sw, (float)sh, 0, 1};
        VkRect2D sci = {{0, 0}, g_vk.sc_extent};
        VkDeviceSize vo = 0;
        rec_CmdBeginRendering(cb, &ri);
        vk_render_forget_state(); /* another pipeline and layout: a draw after it sets everything again */
        rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
        rec_CmdSetViewport(cb, 0, 1, &vp);
        rec_CmdSetScissor(cb, 0, 1, &sci);
        rec_CmdPushConstants(cb, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        rec_CmdBindVertexBuffers(cb, 0, 1, &g_inst, &vo);
        /* runs of quads that sample the same way, in their order: six corners for each instance */
        for (i = 0; i < n; i = run) {
            for (run = i + 1; run < n && g_quads[run].sample == g_quads[i].sample; ++run) continue;
            if (g_quads[i].sample == UI_IMAGE && !g_image) continue; /* its upload failed */
            rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &g_sets[g_quads[i].sample],
                                      0, NULL);
            rec_CmdDraw(cb, 6, (u32)(run - i), 0, (u32)i);
        }
        rec_CmdEndRendering(cb);
    }
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}
