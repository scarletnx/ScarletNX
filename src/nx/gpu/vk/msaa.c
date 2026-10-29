/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Multisampled surfaces' samples moved between their images and the guest's layout of them, on the GPU. The 3D
   engine lays a pixel's samples out in memory as a block of X by Y texels (gpu/draw_state.h DS_MSAA_): a
   multisampled target of w by h pixels is a surface of w X by h Y texels there, in its usual block linear or pitch
   linear layout, sample i of pixel (x, y) at texel (x X + i % X, y Y + i / X). Ryujinx (MIT) takes the samples in the
   same order. Copies can't reach a multisampled image's samples, so a draw moves them through a single-sampled image
   of the guest's size (the grid), which the copies to and from guest memory take as they take any target
   (surface.c's flush_copy and rt_load_copy):
     unpack  each grid texel takes its sample from the image (msaa.frag MS_UNPACK, MS_UNPACK_DEPTH)
     pack    each sample of the image takes its grid texel, shaded per sample (MS_PACK)
   Color goes through views in the unsigned format of the texels' bytes (vk_raw_format), so no bit changes on the way.
   One pipeline per kind, format and sample count, made on first use and kept for the device's lifetime as the other
   internal pipelines are. Under vk_lock, in the frame's command buffer, outside a rendering pass */
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/msaa_spv.h" /* compiled from msaa.vert and msaa.frag by the build (cmake/glsl.cmake) */
#include "nx/runtime/platform/log.h"

enum { MS_UNPACK, MS_UNPACK_DEPTH, MS_PACK, MS_KINDS };
#define MS_PIPES 32

typedef struct {
    u32 kind;
    VkFormat format; /* the render target's */
    VkSampleCountFlagBits samples;
    VkPipeline pipe;
} MsPipe;

static struct {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkSampler sampler;
    VkShaderModule vs, fs[MS_KINDS];
    MsPipe pipe[MS_PIPES];
    u32 npipe;
} g_msaa;

static VkShaderModule ms_module(const u32* code, u64 size) {
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)size, .pCode = code};
    VkShaderModule m;
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m))) fatal("vulkan: cannot load a multisample shader");
    return m;
}
static void ms_init(void) {
    if (g_msaa.layout) return;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                      NULL};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_msaa.dsl)))
        fatal("vulkan: cannot create the multisample moves' descriptor layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_msaa.dsl,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_msaa.layout)))
        fatal("vulkan: cannot create the multisample moves' pipeline layout");
    /* texel fetches only: the sampler's filtering is never used */
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (!VK_OK(vkCreateSampler(g_vk.dev, &si, NULL, &g_msaa.sampler)))
        fatal("vulkan: cannot create the multisample moves' sampler");
    g_msaa.vs = ms_module(msaa_vert_spv, msaa_vert_spv_size);
    g_msaa.fs[MS_UNPACK] = ms_module(msaa_unpack_spv, msaa_unpack_spv_size);
    g_msaa.fs[MS_UNPACK_DEPTH] = ms_module(msaa_unpack_depth_spv, msaa_unpack_depth_spv_size);
    g_msaa.fs[MS_PACK] = ms_module(msaa_pack_spv, msaa_pack_spv_size);
}

/* the pipeline drawing kind into a target of format and samples: VK_NULL_HANDLE when it can't be made */
static VkPipeline ms_pipe(u32 kind, VkFormat format, VkSampleCountFlagBits samples) {
    for (u32 i = 0; i < g_msaa.npipe; i++)
        if (g_msaa.pipe[i].kind == kind && g_msaa.pipe[i].format == format && g_msaa.pipe[i].samples == samples)
            return g_msaa.pipe[i].pipe;
    if (g_msaa.npipe == MS_PIPES) {
        LOG_LIMIT(1, "vulkan: more than %d multisample move pipelines: no more are made\n", MS_PIPES);
        return VK_NULL_HANDLE;
    }
    int depth = kind == MS_UNPACK_DEPTH;
    VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                              .module = g_msaa.vs,
                                              .pName = "main"},
                                             {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                              .module = g_msaa.fs[kind],
                                              .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL,
                                                 .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1.0f};
    /* packing shades every sample on its own: the shader reads gl_SampleID, which asks for it too */
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = samples,
                                               .sampleShadingEnable = kind == MS_PACK,
                                               .minSampleShading = 1.0f};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = depth,
                                                .depthWriteEnable = depth,
                                                .depthCompareOp = VK_COMPARE_OP_ALWAYS};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = depth ? 0 : 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = depth ? 0 : 1,
                                        .pColorAttachmentFormats = &format,
                                        .depthAttachmentFormat = depth ? format : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &ri,
                                       .stageCount = 2,
                                       .pStages = st,
                                       .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia,
                                       .pViewportState = &vp,
                                       .pRasterizationState = &rs,
                                       .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb,
                                       .pDynamicState = &dy,
                                       .layout = g_msaa.layout};
    VkPipeline p;
    if (!VK_OK(vkCreateGraphicsPipelines(g_vk.dev, vk_pipeline_cache(), 1, &pi, NULL, &p))) return VK_NULL_HANDLE;
    g_msaa.pipe[g_msaa.npipe++] = (MsPipe){kind, format, samples, p};
    return p;
}

/* a view of img's layer, a 2D one in format, of aspect, for usage only */
static VkImageView ms_view(VkImage img, VkFormat format, VkImageAspectFlags aspect, u32 layer,
                           VkImageUsageFlags usage) {
    VkImageViewUsageCreateInfo vu = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = usage};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .pNext = &vu,
                                .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .subresourceRange = {aspect, 0, 1, layer, 1}};
    VkImageView v;
    return VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &v)) ? v : VK_NULL_HANDLE;
}

/* the format s's samples move in: its texels' raw one, or a depth format as it is */
static VkFormat ms_format(const Surf* s) { return s->f.depth ? s->f.vk : vk_raw_format(s->f.bpp); }

/* a draw of kind over the w by h target view (its format and samples), its fragments reading src in SHADER_READ_ONLY
   layout. The target is in its attachment layout, its contents replaced */
static int ms_draw(VkCommandBuffer cb, u32 kind, VkImageView target, VkFormat format, VkSampleCountFlagBits samples,
                   u32 w, u32 h, VkImageView src, u32 msaa) {
    ms_init();
    VkPipeline p = ms_pipe(kind, format, samples);
    VkDescriptorSet set = p ? vk_desc_alloc(g_msaa.dsl) : VK_NULL_HANDLE;
    if (!set) return 0;
    VkDescriptorImageInfo ii = {g_msaa.sampler, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w1 = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = set,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w1, 0, NULL);
    int depth = kind == MS_UNPACK_DEPTH;
    VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                     .imageView = target,
                                     .imageLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                          : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, {w, h}},
                          .layerCount = 1,
                          .colorAttachmentCount = depth ? 0 : 1,
                          .pColorAttachments = depth ? NULL : &att,
                          .pDepthAttachment = depth ? &att : NULL};
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    i32 lg[2] = {(i32)ds_msaa_log2_x(msaa), (i32)ds_msaa_log2_y(msaa)};
    rec_CmdBeginRendering(cb, &ri);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_msaa.layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_msaa.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof lg, lg);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_render_forget_state(); /* the renderer's next draw sets its pipeline and state again */
    return 1;
}

int vk_ms_moves(const Surf* s) {
    if (!s->msaa || !s->ms_raw) return 0;
    return s->sw == s->w && s->sh == s->h; /* a scaled image's samples are other pixels than the guest's */
}

/* the single-sampled image of s's guest size in its move format, for usage: VK_NULL_HANDLE when it can't be made */
static VkImage ms_grid(const Surf* s, VkImageUsageFlags usage, VmaAllocation* mem) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = ms_format(s),
                            .extent = {s->w << ds_msaa_log2_x(s->msaa), s->h << ds_msaa_log2_y(s->msaa), 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage img = VK_NULL_HANDLE;
    return vk_image_create(&ii, &img, mem) ? img : VK_NULL_HANDLE;
}

VkImage vk_ms_unpack(VkCommandBuffer cb, Surf* s, u32 layer) {
    if (!vk_ms_moves(s) || layer >= s->layers || s->layout == VK_IMAGE_LAYOUT_UNDEFINED) return VK_NULL_HANDLE;
    int depth = s->f.depth;
    VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VmaAllocation mem;
    VkImage grid = ms_grid(s,
                           (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           &mem);
    if (!grid) return VK_NULL_HANDLE;
    VkFormat f = ms_format(s);
    VkImageView src = ms_view(s->img, f, aspect, layer, VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImageView dst = ms_view(grid, f, aspect, 0,
                              depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    vk_defer_image(grid, mem, src, dst); /* once the frame's commands that read it are done */
    if (!src || !dst) return VK_NULL_HANDLE;
    vk_render_end();
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkPipelineStageFlags2 out = depth ? VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
                                      : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkAccessFlags2 wr = depth ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    VkImageLayout al = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    /* a depth grid holds only depth: a packed format's stencil aspect goes along in the layouts it is moved through */
    VkImageAspectFlags grid_aspect = vk_aspect(&s->f);
    vk_barrier(cb, grid, grid_aspect, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED, out, wr, al);
    if (!ms_draw(cb, depth ? MS_UNPACK_DEPTH : MS_UNPACK, dst, f, VK_SAMPLE_COUNT_1_BIT,
                 s->w << ds_msaa_log2_x(s->msaa), s->h << ds_msaa_log2_y(s->msaa), src, s->msaa))
        return VK_NULL_HANDLE;
    vk_barrier(cb, grid, grid_aspect, out, wr, al, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return grid;
}

int vk_ms_pack(VkCommandBuffer cb, Surf* s, u32 layer, VkBuffer buf, u64 off, u32 pitch) {
    if (!vk_ms_moves(s) || s->f.depth || layer >= s->layers || !g_vk.feat.sampleRateShading) return 0;
    VmaAllocation mem;
    VkImage grid = ms_grid(s, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &mem);
    if (!grid) return 0;
    VkFormat f = ms_format(s);
    VkImageView src = ms_view(grid, f, VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImageView dst = ms_view(s->img, f, VK_IMAGE_ASPECT_COLOR_BIT, layer, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    vk_defer_image(grid, mem, src, dst);
    if (!src || !dst) return 0;
    vk_render_end();
    u32 gw = s->w << ds_msaa_log2_x(s->msaa), gh = s->h << ds_msaa_log2_y(s->msaa);
    vk_barrier(cb, grid, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = {.bufferOffset = off,
                           .bufferRowLength = pitch / s->f.bpp,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {gw, gh, 1}};
    rec_CmdCopyBufferToImage(cb, buf, grid, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    vk_barrier(cb, grid, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    return ms_draw(cb, MS_PACK, dst, f, vk_surf_samples(s), s->w, s->h, src, s->msaa);
}
