/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen controls, the game's menu (runtime/platform/ui.h) and the shader loading screen before the game
   (runtime/platform/prepare_screen.h) over a presented frame: the quads they give, drawn opaque in the window's one
   pass (vk_ui_present), after the frame's own quad and before the present. Nothing is recorded while none shows.

   Each quad is an instance, its SnxUiQuad (20 bytes) as it is, whose corners the vertex shader makes: the CPU writes no
   vertices. The instances stay in a buffer on the device from frame to frame, and a frame copies into it only the
   quads that differ from the ones it holds. The stats box and the debug line keep their layout and their quads in the
   same places (ui_cells), so a frame over the game sends a few letters and a byte of the graph's ring, or nothing */
#include "nx/gpu/vk/descriptor_objects.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/present_quad.h"
#include "nx/gpu/vk/ui_spv.h"
#include "common/utilities/endian.h"
#include "common/utilities/font.h"
#include "gui/icons.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/vpad.h"

#define MAX_QUADS 4096
static SnxUiQuad g_quads[MAX_QUADS];
/* the font's and the icons' bits, a byte a row (ui.frag's Glyphs): what letters and icons read, not the atlas */
#define UI_GLYPHS_SIZE ((256 + 2 * GUI_ICON_COUNT) * 16)
static VkBuffer g_glyphs;
static VmaAllocation g_glyphs_mem;
/* The quads and the ring's bytes (ui.vert's Ring), a region for each frame in flight in one buffer the CPU writes and
   the device reads as it is (host-visible, mapped): the ring's bytes, then the quads as instances. A frame writes into
   its region the quads written since the region was last (the parts ui_dirty_end gives, from each frame since), and
   the ring's bytes when they changed: no copy, no barrier. Its region is free, as vk_cmd waited for the frame two back
   that read it */
#define UI_RING_BYTES SNX_UI_RING_MAX
static VkBuffer g_frames;
static VmaAllocation g_frames_mem;
static u8* g_frames_at;
static u64 g_region; /* a region's bytes, a multiple of the device's least storage buffer offset */
typedef struct {
    int first, end; /* the quads written since this region was */
    u32 ring;       /* the ring's version it holds */
    int fresh;      /* none written into it yet */
} UiSlot;
static UiSlot g_slot[VK_FRAMES];
static int g_slot_now; /* this frame's region */
static int g_frames_coherent; /* its memory host-coherent: what the CPU writes needs no flush */
/* the frame's runs of quads, each drawn with one call (ui_record), kept while no quad changes */
typedef struct {
    int first, count, set, pipe;
    u32 vertices;
} UiRun;
static UiRun g_runs[MAX_QUADS];
static int g_nruns, g_runs_n = -1; /* the quads they were made for, -1 for none */
static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_layout;
/* ui.frag's two pipelines (its MODE): plain, for a quad that covers every pixel of its own (a fill, a bar, a backed
   letter or icon), and sparse, for one that leaves its clear pixels alone (a shape, a letter, an icon, the atlas's
   texels): discarded, or on a GPU that renders in tiles written clear and blended away */
enum { UI_PLAIN, UI_SPARSE };
static VkPipeline g_pipes[2];
static VkFormat g_pipe_format;
static int g_pipe_tiler; /* the sparse pipeline blends: made for a GPU that renders in tiles */
typedef struct {
    u32 mode, srgb;
} UiSpec;
/* a GPU that renders in tiles, by its vendor: ARM's Mali, Qualcomm's Adreno, Apple's, Imagination's PowerVR and
   Broadcom's VideoCore, where a discard costs its tile's early tests */
static int tiler(void) {
    switch (g_vk.props.vendorID) {
    case 0x13B5: case 0x5143: case 0x106B: case 0x1010: case 0x14E4: return 1;
    default: return 0;
    }
}
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
    VkDescriptorSetLayoutBinding b[3] = {{.binding = 0,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                          .descriptorCount = 1,
                                          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                         {.binding = 1,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                          .descriptorCount = 1,
                                          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                         {.binding = 2,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
                                          .descriptorCount = 1,
                                          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &g_dsl,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &pc};
    VkDescriptorPoolSize size[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 3}};
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 3, .poolSizeCount = 3, .pPoolSizes = size};
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
    VkVertexInputAttributeDescription va[4] = {{0, 0, VK_FORMAT_R16G16_SINT, offsetof(SnxUiQuad, x)},
                                               {1, 0, VK_FORMAT_R16G16_UINT, offsetof(SnxUiQuad, w)},
                                               {2, 0, VK_FORMAT_R16G16B16A16_UINT, offsetof(SnxUiQuad, u)},
                                               {3, 0, VK_FORMAT_R32_UINT, offsetof(SnxUiQuad, bits)}};
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
    /* nothing in the layer is translucent: a pixel is written opaque or not at all (ui.frag), with no blending but the
       sparse pipeline's on a GPU that renders in tiles, which writes a pixel it doesn't draw clear (premultiplied) */
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
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
    /* ui.frag's MODE (0 plain, 1 sparse that discards, 2 sparse that blends) and SRGB */
    UiSpec spec;
    VkSpecializationMapEntry entries[2] = {{0, offsetof(UiSpec, mode), 4}, {1, offsetof(UiSpec, srgb), 4}};
    VkSpecializationInfo si = {2, entries, sizeof spec, &spec};
    /* premultiplied: a pixel written clear leaves the picture as it was */
    const VkPipelineColorBlendAttachmentState blended = {.blendEnable = VK_TRUE,
                                                         .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                         .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                         .colorBlendOp = VK_BLEND_OP_ADD,
                                                         .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                         .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                         .alphaBlendOp = VK_BLEND_OP_ADD,
                                                         .colorWriteMask = 0xF};
    int ok, i, blend = tiler();
    if (g_pipes[UI_PLAIN] && g_pipes[UI_SPARSE] && g_pipe_format == format && g_pipe_tiler == blend) return 1;
    for (i = 0; i < 2; ++i) { /* vk_finish ran when the swapchain changed format */
        if (g_pipes[i]) vkDestroyPipeline(g_vk.dev, g_pipes[i], NULL);
        g_pipes[i] = VK_NULL_HANDLE;
    }
    vs = vk_module_spv(snx_ui_vs_spv, (size_t)(snx_ui_vs_spv_size / 4));
    fs = vk_module_spv(snx_ui_fs_spv, (size_t)(snx_ui_fs_spv_size / 4));
    st[0].module = vs;
    st[1].module = fs;
    st[1].pSpecializationInfo = &si;
    ok = vs && fs;
    for (i = 0; ok && i < 2; ++i) {
        spec.mode = i == UI_PLAIN ? 0 : blend ? 2 : 1;
        spec.srgb = (u32)g_vk.sc_srgb;
        ba = i == UI_SPARSE && blend ? blended : (VkPipelineColorBlendAttachmentState){.colorWriteMask = 0xF};
        ok = VK_OK(vkCreateGraphicsPipelines(g_vk.dev, vk_pipeline_cache(), 1, &gci, NULL, &g_pipes[i]));
    }
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    g_pipe_format = format;
    g_pipe_tiler = blend;
    for (i = 0; i < 2 && !ok; ++i) {
        if (g_pipes[i]) vkDestroyPipeline(g_vk.dev, g_pipes[i], NULL);
        g_pipes[i] = VK_NULL_HANDLE;
    }
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
/* ui.vert's Picture: the picture's size, the ring's head and its slots from bit 16, the bars' slow colour and its
   height, and the backed letters' ground */
typedef struct {
    float w, h;
    u32 ring, slow, slow_from, ground;
} UiPicture;
static UiPicture g_pc;
/* The layer the window shows, as the last pass drew it: its safe area, its quads' number (-1: none drawn yet, or
   another swapchain since) and what ui.vert took beside them; and whether the list has moved on from it (a quad
   written, their number or those words another), which stays so until a pass draws it */
static struct {
    int safe[4], n;
    UiPicture pc;
    int changed;
} g_shown = {.n = -1, .changed = 1};

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
    g_shown.n = -1; /* the next swapchain's images show nothing of it */
}

/* the glyphs' rows to the device, once: gui_vga_font's, then gui_icon_font's, each set's binding 1 */
static int glyphs_upload(VkCommandBuffer cb) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = UI_GLYPHS_SIZE,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationInfo info;
    VkBufferMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .size = VK_WHOLE_SIZE};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b};
    VkBufferCopy copy = {.size = UI_GLYPHS_SIZE};
    VkBuffer buf;
    u64 off;
    u8* to;
    int i;
    if (g_glyphs) return 1;
    to = vk_stage(UI_GLYPHS_SIZE, 16, &buf, &off);
    if (!to) return 0;
    if (!VK_OK(vk_buffer_create(g_vk.vma, &bci, &ai, &g_glyphs, &g_glyphs_mem, &info))) {
        g_glyphs = VK_NULL_HANDLE;
        return 0;
    }
    vk_mem_track(g_glyphs_mem);
    memcpy(to, gui_vga_font, 256 * 16); /* bytes: the device's words read them little-endian, as the shader takes */
    memcpy(to + 256 * 16, gui_icon_font, 2 * GUI_ICON_COUNT * 16);
    copy.srcOffset = off;
    rec_CmdCopyBuffer(cb, buf, g_glyphs, 1, &copy);
    b.buffer = g_glyphs;
    rec_CmdPipelineBarrier2(cb, &d);
    for (i = 0; i < 3; ++i) {
        VkDescriptorBufferInfo bi = {g_glyphs, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = g_sets[i],
                                   .dstBinding = 1,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   .pBufferInfo = &bi};
        vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    }
    return 1;
}
/* the frames' buffer, made once, each set's binding 2 its region's ring (the region an offset at each bind) */
static int frames_make(void) {
    u64 align = g_vk.props.limits.minStorageBufferOffsetAlignment, region = UI_RING_BYTES + sizeof g_quads;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    /* written in order and never read by the CPU: the device's own memory where the CPU can write it (a BAR), else
       the host's */
    VmaAllocationCreateInfo ai = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationInfo info;
    int i;
    if (g_frames) return 1;
    align = align < 64 ? 64 : align;
    g_region = (region + align - 1) / align * align;
    bci.size = g_region * VK_FRAMES;
    if (!VK_OK(vk_buffer_create(g_vk.vma, &bci, &ai, &g_frames, &g_frames_mem, &info)) || !info.pMappedData) {
        if (g_frames) vmaDestroyBuffer(g_vk.vma, g_frames, g_frames_mem);
        g_frames = VK_NULL_HANDLE;
        return 0;
    }
    vk_mem_track(g_frames_mem);
    g_frames_at = (u8*)info.pMappedData;
    {
        VkMemoryPropertyFlags flags = 0;
        vmaGetAllocationMemoryProperties(g_vk.vma, g_frames_mem, &flags);
        g_frames_coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    }
    for (i = 0; i < VK_FRAMES; ++i) g_slot[i] = (UiSlot){.fresh = 1};
    for (i = 0; i < 3; ++i) {
        VkDescriptorBufferInfo bi = {g_frames, 0, UI_RING_BYTES};
        VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = g_sets[i],
                                   .dstBinding = 2,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
                                   .pBufferInfo = &bi};
        vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    }
    return 1;
}
/* the quads first to end, as the device reads them, at to: little-endian halves, and the word of bits */
static void quads_le(u8* to, int first, int end) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    int i;
    for (i = first; i < end; ++i, to += sizeof(SnxUiQuad)) {
        SnxUiQuad q = g_quads[i];
        snx_le16_array(&q.x, 8); /* x, y, w, h, u, v, uw, vh */
        q.bits = snx_le32(q.bits);
        memcpy(to, &q, sizeof q);
    }
#else
    memcpy(to, &g_quads[first], (size_t)(end - first) * sizeof(SnxUiQuad));
#endif
}
/* the quads written this frame (first to end) owed to every region */
static void frames_owe(int first, int end) {
    int k;
    for (k = 0; k < VK_FRAMES && first < end; ++k) {
        if (g_slot[k].first >= g_slot[k].end || first < g_slot[k].first) g_slot[k].first = first;
        if (end > g_slot[k].end) g_slot[k].end = end;
    }
}
/* the n quads and the ring into this frame's region, as much of them as it lacks: the region's number */
static int frames_send(int n, const SnxUiRing* ring) {
    int slot = (int)(vk_frame_id() % VK_FRAMES);
    u8* at = g_frames_at + g_region * (u64)slot;
    if (g_slot[slot].fresh) {
        g_slot[slot].first = 0;
        g_slot[slot].end = n;
    }
    if (g_slot[slot].end > n) g_slot[slot].end = n;
    if (g_slot[slot].first < g_slot[slot].end) {
        u64 off = UI_RING_BYTES + (u64)g_slot[slot].first * sizeof(SnxUiQuad);
        quads_le(at + off, g_slot[slot].first, g_slot[slot].end);
        if (!g_frames_coherent)
            vmaFlushAllocation(g_vk.vma, g_frames_mem, g_region * (u64)slot + off,
                               (u64)(g_slot[slot].end - g_slot[slot].first) * sizeof(SnxUiQuad));
    }
    if (g_slot[slot].fresh || g_slot[slot].ring != ring->version) { /* its maker's count of its changes */
        memcpy(at, ring->height, UI_RING_BYTES);
        if (!g_frames_coherent) vmaFlushAllocation(g_vk.vma, g_frames_mem, g_region * (u64)slot, UI_RING_BYTES);
        g_slot[slot].ring = ring->version;
    }
    g_slot[slot].first = g_slot[slot].end = 0;
    g_slot[slot].fresh = 0;
    return slot;
}
/* the quad's sample: its descriptor set */
static unsigned quad_sample(int i) { return ui_q_bits(&g_quads[i], UI_Q_SAMPLE, 2); }
/* an outline shape (an unfilled circle, round box or cross with its edge), which ui.vert draws as UI_OUTLINE_PARTS
   quads over its edge and its label, so that its inside takes no fragment: the vertices an instance of it takes */
#define UI_OUTLINE_PARTS 16 /* ui.vert's OUTLINE_PARTS */
/* the pipeline a quad is drawn with: plain for a fill, a bar, a backed letter or icon, sparse for the rest */
static int quad_pipe(int i) {
    unsigned ink = ui_q_bits(&g_quads[i], UI_Q_INK, 3);
    return ink == UI_SOLID || ink == UI_RING ||
                   ((ink == UI_GLYPH || ink == UI_ICON) && ui_q_bits(&g_quads[i], UI_Q_BACKED, 1))
               ? UI_PLAIN
               : UI_SPARSE;
}
static u32 quad_vertices(int i) {
    const SnxUiQuad* q = &g_quads[i];
    int outline = ui_q_bits(q, UI_Q_INK, 3) == UI_SHAPE && !(q->uw & (UI_SHAPE_FILLED | UI_SHAPE_BARE)) &&
                  q->u <= UI_SHAPE_CROSS;
    return outline ? 6 * UI_OUTLINE_PARTS : 6;
}
/* the parts' quads into the renderer's list as a frame makes them, the span of it they wrote owed to every region and
   g_pc set: their number. Under ui_lock */
static int ui_list(const int safe[4]) {
    SnxUiList list = {g_quads, 0, MAX_QUADS};
    int sw = (int)g_vk.sc_extent.width, sh = (int)g_vk.sc_extent.height, first, end;
    const SnxUiRing* ring = ui_ring(); /* the renderer's thread's alone, which writes it in stats_draw */
    u64 now = plat_ns();               /* the frame's time, for every part */
    ui_dirty_begin(g_quads);
    /* the screen before the game starts, under the menu, hides the on-screen controls: they aren't drawn under it,
       where they would take quads for nothing (nor do they take fingers then, sdl3.c's press_event) */
    if (prepare_screen_shown())
        prepare_screen_draw(&list, sw, sh, safe, overlay_display_percent());
    else {
        int px, py, scale;
        vpad_draw(&list, safe);
        vpad_showcase_draw(&list, safe, vpad_showcase_fps(g_vk_present, now));
        overlay_place(sw, sh, &px, &py, &scale); /* the menu's letters */
        stats_draw(&list, safe, scale, now);
    }
    {
        int px, py, scale;
        overlay_place(sw, sh, &px, &py, &scale); /* the menu's letters */
        hud_draw(&list, safe, scale);
    }
    overlay_draw(&list, sw, sh, now);
    ui_dirty_end(&first, &end);
    g_pc = (UiPicture){(float)sw,  (float)sh,       (u32)ring->head | (u32)ring->n << 16,
                       ring->slow, ring->slow_from, ui_ground()};
    frames_owe(first, end);
    if (first < end || list.n != g_runs_n) g_runs_n = -1; /* a quad changed: the runs are made again */
    if (first < end || list.n != g_shown.n || memcmp(&g_pc, &g_shown.pc, sizeof g_pc)) g_shown.changed = 1;
    return list.n;
}
/* the frame's layer, before a rendering pass: its quads made (the producers, under ui_lock), what changed written into
   this frame's region, the push constants set; the quads to draw, 0 for none. It is the layer shown from here */
static int ui_prepare(VkCommandBuffer cb, const int safe[4]) {
    int n;
    if (!layout_make()) return 0;
    ui_lock();
    atlas_upload(cb);
    image_upload(cb);
    n = ui_list(safe);
    ui_unlock();
    memcpy(g_shown.safe, safe, sizeof g_shown.safe);
    g_shown.n = n;
    g_shown.pc = g_pc;
    g_shown.changed = 0;
    if (!n || !g_atlas || !pipeline_make(g_vk.sc_format) || !glyphs_upload(cb) || !frames_make()) return 0;
    g_slot_now = frames_send(n, ui_ring());
    return n;
}
int vk_ui_changed(void) {
    int w, h;
    u32 version;
    if (g_shown.n < 0 || !layout_make()) return 1;
    ui_lock();
    /* an atlas or a picture the device has not taken yet is another layer too */
    if (ui_atlas(&w, &h, &version) && version != g_atlas_version) g_shown.changed = 1;
    if (ui_image_get(&version) && version != g_image_version) g_shown.changed = 1;
    ui_list(g_shown.safe);
    ui_unlock();
    return g_shown.changed;
}
/* the runs of the n quads: of one pipeline, sampling the same way and taking as many vertices, in their order (six
   corners for a quad, or an outline shape's parts) */
static void runs_make(int n) {
    int i, run;
    for (i = 0, g_nruns = 0; i < n; i = run) {
        UiRun r = {.first = i, .set = (int)quad_sample(i), .pipe = quad_pipe(i), .vertices = quad_vertices(i)};
        for (run = i + 1; run < n && (int)quad_sample(run) == r.set && quad_vertices(run) == r.vertices &&
                          quad_pipe(run) == r.pipe;
             ++run)
            continue;
        r.count = run - i;
        g_runs[g_nruns++] = r;
    }
    g_runs_n = n;
}
/* the n quads ui_prepare made, into the rendering pass open on cb, over the whole picture: from this frame's region,
   each run with one call, the pipeline and the set bound where they change */
static void ui_record(VkCommandBuffer cb, int n) {
    VkViewport vp = {0, 0, g_pc.w, g_pc.h, 0, 1};
    VkRect2D sci = {{0, 0}, g_vk.sc_extent};
    VkDeviceSize vo = g_region * (u64)g_slot_now + UI_RING_BYTES;
    u32 ring_at = (u32)(g_region * (u64)g_slot_now);
    int i, set = -1, pipe = -1;
    if (g_runs_n != n) runs_make(n);
    vk_render_forget_state(); /* another pipeline and layout: a draw after it sets everything again */
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sci);
    rec_CmdPushConstants(cb, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof g_pc,
                         &g_pc);
    rec_CmdBindVertexBuffers(cb, 0, 1, &g_frames, &vo);
    for (i = 0; i < g_nruns; ++i) {
        const UiRun* r = &g_runs[i];
        if (r->set == UI_IMAGE && !g_image) continue; /* its upload failed */
        if (pipe != r->pipe) rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipes[pipe = r->pipe]);
        if (set != r->set)
            rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &g_sets[set = r->set], 1,
                                      &ring_at);
        rec_CmdDraw(cb, r->vertices, (u32)r->count, 0, (u32)r->first);
    }
}
/* a rendering pass on sc's view, cleared to black or loaded */
static void ui_pass_begin(VkCommandBuffer cb, VkImageView view, int clear) {
    VkRenderingAttachmentInfo at = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.color = {{0, 0, 0, 1}}}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, g_vk.sc_extent},
                          .layerCount = 1,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = &at};
    rec_CmdBeginRendering(cb, &ri);
}
void vk_ui_draw(VkCommandBuffer cb, VkImage sc, const int safe[4]) {
    int n = ui_prepare(cb, safe);
    VkImageView view;
    if (!n || !(view = sc_view(sc))) return;
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ui_pass_begin(cb, view, 0);
    ui_record(cb, n);
    rec_CmdEndRendering(cb);
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}
void vk_ui_present(VkCommandBuffer cb, VkImage sc, Surf* src, VkRect2D crop, VkRect2D to, u32 transform,
                   const int safe[4]) {
    SnxPresentSource from;
    int n = ui_prepare(cb, safe), framed = src && vk_present_quad_source(cb, src, 1, !g_vk.sc_srgb, &from);
    VkImageView view = sc_view(sc);
    /* the acquire's wait is at the colour output (renderer.c's submit): the image's first use */
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (view) {
        ui_pass_begin(cb, view, 1);
        if (framed) vk_present_quad_draw(cb, &from, g_vk.sc_format, crop, to, transform);
        if (n) ui_record(cb, n);
        rec_CmdEndRendering(cb);
        vk_render_forget_state();
    }
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}
