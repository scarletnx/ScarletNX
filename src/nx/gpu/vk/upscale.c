/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The spatial upscaler the presenter runs before its 1:1 blit: AMD FSR 1 (EASU and RCAS), NVIDIA Image Scaling's
   NVScaler, and Qualcomm SGSR 1. None and Bilinear are the presenter's own blit.

   FSR 1's shaders build on AMD's MIT ffx_fsr1.h (FidelityFX Super Resolution 1.0), NIS on NVIDIA's MIT NIS 1.0.3
   (NVScaler), and SGSR on Qualcomm's BSD-3-Clause sgsr1_shader_mobile.frag. The wrapper, the constants and the
   Vulkan plumbing are ScarletNX's. The sources' notices stay in their files (docs/CREDITS.md). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/upscale.h"
#include "nx/gpu/vk/upscale_fsr.h"
#include "nx/gpu/vk/upscale_spv.h"
#include "nx/gpu/vk/upscale/nis_coef.h"
#include "nx/runtime/platform/log.h"

#define NIS_BLOCK_W 32
#define NIS_BLOCK_H 24
#define UP_PC_FSR   96 /* four uvec4 constants, Flip and Crop */
#define UP_PC_SGSR  64 /* nine floats, padding, Flip */

/* NIS_Config.h's NISConfig, in the shader's uniform block's order. 28 32-bit words. */
typedef struct {
    float kDetectRatio, kDetectThres, kMinContrastRatio, kRatioNorm;
    float kContrastBoost, kEps, kSharpStartY, kSharpScaleY;
    float kSharpStrengthMin, kSharpStrengthScale, kSharpLimitMin, kSharpLimitScale;
    float kScaleX, kScaleY;
    float kDstNormX, kDstNormY, kSrcNormX, kSrcNormY;
    uint32_t kInputViewportOriginX, kInputViewportOriginY, kInputViewportWidth, kInputViewportHeight;
    uint32_t kOutputViewportOriginX, kOutputViewportOriginY, kOutputViewportWidth, kOutputViewportHeight;
    float reserved0, reserved1;
} NisConfig;

typedef struct {
    VkImage img, mid;
    VmaAllocation mem, mid_mem;
    VkImageView view, mid_view;
    VkDescriptorSet fsr_easu, fsr_rcas, nis, sgsr;
} UpFrame;

static struct {
    int configured, sharpness;
    int auto_state; /* Auto's ratio state: 0 off, 1 on, -1 off after it was on */
    VkFormat fmt;
    u32 w, h;
    int made, failed;
    VkDescriptorSetLayout fsr_dsl, nis_dsl, sgsr_dsl;
    VkPipelineLayout fsr_pl, nis_pl, sgsr_pl;
    VkPipeline easu, rcas, easu16, rcas16, nis, nis8, sgsr;
    VkDescriptorPool pool;
    VkSampler samp;
    VkImage coef[2];
    VmaAllocation coef_mem[2];
    VkImageView coef_view[2];
    UpFrame frame[VK_FRAMES];
    int last_filter;
    u32 last_src_w, last_src_h, last_dst_w, last_dst_h;
} g_up;

typedef char up_nis_size_check[sizeof(NisConfig) == 112 ? 1 : -1];

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static u32 div_up(u32 a, u32 b) { return (a + b - 1) / b; }

static VkFormat choose_format(void) {
    VkFormat f = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormatProperties p;
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (g_up.fmt) return g_up.fmt;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    if ((p.optimalTilingFeatures & need) != need) f = VK_FORMAT_R8G8B8A8_UNORM;
    g_up.fmt = f;
    return f;
}
VkFormat vk_upscale_format(void) { return choose_format(); }

void vk_upscale_configure(int filter, int sharpness) {
    __atomic_store_n(&g_up.configured, filter, __ATOMIC_RELAXED);
    g_up.sharpness = sharpness < 0 ? 0 : sharpness > 100 ? 100 : sharpness;
    __atomic_store_n(&g_up.auto_state, 0, __ATOMIC_RELAXED);
    g_up.failed = 0;
    if (filter == SNX_UPSCALE_AUTO) {
        int pick = snx_upscale_auto_pick(g_vk.props.vendorID, g_vk.props.deviceID, g_vk.props.deviceName);
        LOG("vulkan: upscaler auto: %s chooses %s (sharpness %d)\n", g_vk.props.deviceName, snx_upscale_label(pick),
            g_up.sharpness);
    } else
        LOG("vulkan: upscaler %s (sharpness %d)\n", snx_upscale_label(filter), g_up.sharpness);
}

int vk_upscale_filter_uses_sharpness(int filter) { return snx_upscale_uses_sharpness(filter); }

int vk_upscale_cycle(void) {
    /* FSR 1, Bilinear, NVIDIA NIS, Qualcomm SGSR, None. Auto enters the cycle at FSR 1 */
    static const int order[] = {SNX_UPSCALE_FSR1, SNX_UPSCALE_BILINEAR, SNX_UPSCALE_NIS, SNX_UPSCALE_SGSR,
                                SNX_UPSCALE_OFF};
    int f = __atomic_load_n(&g_up.configured, __ATOMIC_RELAXED), i, n = (int)(sizeof order / sizeof order[0]), next;
    for (i = 0; i < n && order[i] != f; ++i) continue;
    next = i == n ? order[0] : order[(i + 1) % n];
    __atomic_store_n(&g_up.configured, next, __ATOMIC_RELAXED);
    __atomic_store_n(&g_up.auto_state, 0, __ATOMIC_RELAXED);
    return next;
}

int vk_upscale_pick(u32 src_w, u32 src_h, u32 dst_w, u32 dst_h, u32 transform) {
    int f = __atomic_load_n(&g_up.configured, __ATOMIC_RELAXED);
    if (!src_w || !src_h || !dst_w || !dst_h) return -1;
    if (f == SNX_UPSCALE_AUTO) {
        int state = __atomic_load_n(&g_up.auto_state, __ATOMIC_RELAXED);
        f = snx_upscale_auto_pick(g_vk.props.vendorID, g_vk.props.deviceID, g_vk.props.deviceName);
        if (!snx_upscale_ratio(&state, src_w, src_h, dst_w, dst_h)) {
            __atomic_store_n(&g_up.auto_state, state, __ATOMIC_RELAXED);
            return SNX_UPSCALE_OFF;
        }
        __atomic_store_n(&g_up.auto_state, state, __ATOMIC_RELAXED);
    }
    if (f == SNX_UPSCALE_OFF || f == SNX_UPSCALE_BILINEAR) return f;
    /* NIS samples the input itself and supports at most 2x each axis, and its coordinates have no flip: FSR 1's
       shaders do both, so they stand in for it there */
    if (f == SNX_UPSCALE_NIS &&
        (transform || dst_w < src_w || dst_h < src_h || (u64)dst_w > (u64)src_w * 2 || (u64)dst_h > (u64)src_h * 2))
        f = SNX_UPSCALE_FSR1;
    return f;
}

/* ---- the device objects ---- */

static VkShaderModule module(const u32* words, u64 bytes) { return vk_module_spv(words, (size_t)(bytes / 4)); }

static VkPipeline make_gfx(const u32* vs, u64 vs_bytes, const u32* fs, u64 fs_bytes, VkPipelineLayout layout,
                           VkFormat fmt) {
    VkShaderModule v = module(vs, vs_bytes), f = module(fs, fs_bytes);
    VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                              .pName = "main"},
                                             {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                              .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
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
                                       .layout = layout};
    VkPipeline pipe = VK_NULL_HANDLE;
    st[0].module = v;
    st[1].module = f;
    if (!v || !f || !VK_OK(vkCreateGraphicsPipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &pipe)))
        pipe = VK_NULL_HANDLE;
    if (v) vkDestroyShaderModule(g_vk.dev, v, NULL);
    if (f) vkDestroyShaderModule(g_vk.dev, f, NULL);
    return pipe;
}

static VkPipeline make_compute(const u32* words, u64 bytes, VkPipelineLayout layout) {
    VkShaderModule m = module(words, bytes);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .pName = "main",
                                                .module = m},
                                      .layout = layout};
    VkPipeline pipe = VK_NULL_HANDLE;
    if (!m || !VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &pipe)))
        pipe = VK_NULL_HANDLE;
    if (m) vkDestroyShaderModule(g_vk.dev, m, NULL);
    return pipe;
}

static int make_layouts(void) {
    VkPushConstantRange fsr_pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, UP_PC_FSR};
    VkPushConstantRange sgsr_pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, UP_PC_SGSR};
    VkDescriptorSetLayoutBinding fsr_b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                          NULL};
    VkDescriptorSetLayoutCreateInfo dl = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &fsr_b};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_up.fsr_dsl,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &fsr_pc};
    VkDescriptorSetLayoutBinding nis_b[6] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo nis_dl = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 6, .pBindings = nis_b};
    VkPipelineLayoutCreateInfo nis_pl = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_up.nis_dsl};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_up.fsr_dsl))) return 0;
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_up.sgsr_dsl))) return 0;
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &nis_dl, NULL, &g_up.nis_dsl))) return 0;
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &g_up.fsr_pl))) return 0;
    pl.pSetLayouts = &g_up.sgsr_dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &sgsr_pc;
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &g_up.sgsr_pl))) return 0;
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &nis_pl, NULL, &g_up.nis_pl))) return 0;
    return 1;
}

static int make_pipelines(void) {
    VkFormat fmt = choose_format();
    g_up.easu = make_gfx(fsr_vert_spv, fsr_vert_spv_size, fsr_easu_spv, fsr_easu_spv_size, g_up.fsr_pl, fmt);
    g_up.rcas = make_gfx(fsr_vert_spv, fsr_vert_spv_size, fsr_rcas_spv, fsr_rcas_spv_size, g_up.fsr_pl, fmt);
    if (g_shader_float16) {
        g_up.easu16 =
            make_gfx(fsr_vert_spv, fsr_vert_spv_size, fsr_easu_fp16_spv, fsr_easu_fp16_spv_size, g_up.fsr_pl, fmt);
        g_up.rcas16 =
            make_gfx(fsr_vert_spv, fsr_vert_spv_size, fsr_rcas_fp16_spv, fsr_rcas_fp16_spv_size, g_up.fsr_pl, fmt);
    }
    g_up.sgsr = make_gfx(sgsr_vert_spv, sgsr_vert_spv_size, sgsr_frag_spv, sgsr_frag_spv_size, g_up.sgsr_pl, fmt);
    g_up.nis = make_compute(nis_spv, nis_spv_size, g_up.nis_pl);
    g_up.nis8 = make_compute(nis8_spv, nis8_spv_size, g_up.nis_pl);
    return g_up.easu && g_up.rcas && g_up.sgsr && (!g_shader_float16 || (g_up.easu16 && g_up.rcas16)) &&
           (fmt == VK_FORMAT_R8G8B8A8_UNORM ? g_up.nis8 != VK_NULL_HANDLE : g_up.nis != VK_NULL_HANDLE);
}

static void free_image(VkImage* img, VmaAllocation* mem, VkImageView* view) {
    if (*img || *view) vk_defer_image(*img, *mem, *view, VK_NULL_HANDLE);
    *img = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
    *view = VK_NULL_HANDLE;
}

static int make_frame_image(UpFrame* f, int mid, u32 w, u32 h, VkFormat fmt) {
    VkImage* img = mid ? &f->mid : &f->img;
    VmaAllocation* mem = mid ? &f->mid_mem : &f->mem;
    VkImageView* view = mid ? &f->mid_view : &f->view;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    free_image(img, mem, view);
    if (!vk_image_create(&ii, img, mem)) return 0;
    vi.image = *img;
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, view))) {
        vk_image_destroy(*img, *mem);
        *img = VK_NULL_HANDLE;
        *mem = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

static int make_images(u32 w, u32 h, int want_mid) {
    VkFormat fmt = choose_format();
    if (g_up.w != w || g_up.h != h) {
        for (int i = 0; i < VK_FRAMES; ++i) {
            free_image(&g_up.frame[i].img, &g_up.frame[i].mem, &g_up.frame[i].view);
            free_image(&g_up.frame[i].mid, &g_up.frame[i].mid_mem, &g_up.frame[i].mid_view);
        }
        g_up.w = w;
        g_up.h = h;
    }
    for (int i = 0; i < VK_FRAMES; ++i) {
        UpFrame* f = &g_up.frame[i];
        if (!f->img && !make_frame_image(f, 0, w, h, fmt)) return 0;
        if (want_mid && !f->mid && !make_frame_image(f, 1, w, h, fmt)) return 0;
    }
    return 1;
}

static int make_coef(VkCommandBuffer cb) {
    const u32 w = kFilterSize / 4, h = kPhaseCount;
    const u64 one = (u64)w * h * 4 * sizeof(uint16_t);
    VkBuffer buf;
    u64 off;
    u8* p = vk_stage(one * 2, 16, &buf, &off);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!p) return 0;
    memcpy(p, coef_scale_fp16, sizeof coef_scale_fp16);
    memcpy(p + one, coef_usm_fp16, sizeof coef_usm_fp16);
    for (int i = 0; i < 2; ++i) {
        VkBufferImageCopy c = {.bufferOffset = off + (u64)i * one,
                               .bufferRowLength = w,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {w, h, 1}};
        if (!vk_image_create(&ii, &g_up.coef[i], &g_up.coef_mem[i])) return 0;
        vi.image = g_up.coef[i];
        if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_up.coef_view[i]))) return 0;
        vk_barrier(cb, g_up.coef[i], VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        rec_CmdCopyBufferToImage(cb, buf, g_up.coef[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        vk_barrier(cb, g_up.coef[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return 1;
}

static int make_common(VkCommandBuffer cb) {
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkDescriptorPoolSize sizes[5] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, 4},
                                     {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 12},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
    /* 16: two FSR sets, one NIS and one SGSR for each of the two frames, and room for a remake */
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 16, .poolSizeCount = 5, .pPoolSizes = sizes};
    if (g_up.made) return 1;
    if (g_up.failed) return 0;
    choose_format();
    if (!VK_OK(vkCreateSampler(g_vk.dev, &si, NULL, &g_up.samp)) || !make_layouts() || !make_pipelines() ||
        !VK_OK(vkCreateDescriptorPool(g_vk.dev, &pci, NULL, &g_up.pool)) || !make_coef(cb))
        goto failed;
    for (int i = 0; i < VK_FRAMES; ++i) {
        VkDescriptorSetLayout l[2] = {g_up.fsr_dsl, g_up.fsr_dsl};
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = g_up.pool,
                                          .descriptorSetCount = 2,
                                          .pSetLayouts = l};
        if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, &g_up.frame[i].fsr_easu))) goto failed;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_up.nis_dsl;
        if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, &g_up.frame[i].nis))) goto failed;
        ai.pSetLayouts = &g_up.sgsr_dsl;
        if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, &g_up.frame[i].sgsr))) goto failed;
    }
    g_up.made = 1;
    return 1;
failed:
    g_up.failed = 1;
    WARN("vulkan: the upscaler could not be made, the frame is blitted\n");
    return 0;
}

/* ---- recording ---- */

static void put_combined(VkDescriptorSet set, VkImageView view) {
    VkDescriptorImageInfo di = {g_up.samp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = set,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &di};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
}

static void draw_pass(VkCommandBuffer cb, VkImageView view, VkPipeline pipe, VkPipelineLayout layout, u32 w, u32 h,
                      VkDescriptorSet set, const void* pc, u32 pc_size, VkShaderStageFlags stages) {
    VkRenderingAttachmentInfo at = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, {w, h}},
                          .layerCount = 1,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = &at};
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    rec_CmdBeginRendering(cb, &ri);
    vk_render_forget_state();
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    if (pc && pc_size) rec_CmdPushConstants(cb, layout, stages, 0, pc_size, pc);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
}

static int record_fsr(VkCommandBuffer cb, VkImageView src, u32 image_w, u32 image_h, u32 x, u32 y, u32 w, u32 h,
                      u32 transform, u32 dw, u32 dh) {
    UpFrame* f = &g_up.frame[vk_frame_slot()];
    uint32_t con[4][4], rcas[4];
    struct {
        uint32_t con[4][4];
        float flip[4];
        float crop[4];
    } pc_easu, pc_rcas;
    float sharp = (float)g_up.sharpness / 100.0f;
    if (!f->mid && !make_frame_image(f, 1, dw, dh, choose_format())) return 0;
    vk_upscale_fsr_easu_con(con, (float)w, (float)h, (float)image_w, (float)image_h, (float)dw, (float)dh, (float)x,
                            (float)y);
    vk_upscale_fsr_rcas_con(rcas, sharp);
    memset(&pc_easu, 0, sizeof pc_easu);
    memcpy(pc_easu.con, con, sizeof con);
    pc_easu.flip[0] = (transform & 1) ? 1.0f : 0.0f;
    pc_easu.flip[1] = (transform & 2) ? 1.0f : 0.0f;
    pc_easu.crop[0] = (float)x / (float)image_w;
    pc_easu.crop[1] = (float)y / (float)image_h;
    pc_easu.crop[2] = (float)(x + w) / (float)image_w;
    pc_easu.crop[3] = (float)(y + h) / (float)image_h;
    /* RCAS reads the EASU output, which EASU already mirrored: flipping it again would cancel the frame's transform */
    pc_rcas = pc_easu;
    memcpy(pc_rcas.con[0], rcas, sizeof rcas);
    pc_rcas.flip[0] = pc_rcas.flip[1] = 0.0f;
    pc_rcas.crop[0] = pc_rcas.crop[1] = 0.0f;
    pc_rcas.crop[2] = pc_rcas.crop[3] = 1.0f;

    put_combined(f->fsr_easu, src);
    put_combined(f->fsr_rcas, f->mid_view);
    vk_barrier(cb, f->mid, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    draw_pass(cb, f->mid_view, g_shader_float16 ? g_up.easu16 : g_up.easu, g_up.fsr_pl, dw, dh, f->fsr_easu, &pc_easu,
              sizeof pc_easu, VK_SHADER_STAGE_FRAGMENT_BIT);
    vk_barrier(cb, f->mid, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_barrier(cb, f->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    draw_pass(cb, f->view, g_shader_float16 ? g_up.rcas16 : g_up.rcas, g_up.fsr_pl, dw, dh, f->fsr_rcas, &pc_rcas,
              sizeof pc_rcas, VK_SHADER_STAGE_FRAGMENT_BIT);
    vk_barrier(cb, f->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return 1;
}

static int record_sgsr(VkCommandBuffer cb, VkImageView src, u32 image_w, u32 image_h, u32 x, u32 y, u32 w, u32 h,
                       u32 transform, u32 dw, u32 dh) {
    UpFrame* f = &g_up.frame[vk_frame_slot()];
    float pc[16] = {0};
    pc[0] = 1.0f / (float)image_w;
    pc[1] = 1.0f / (float)image_h;
    pc[2] = (float)image_w;
    pc[3] = (float)image_h;
    pc[4] = (float)w / (float)image_w;
    pc[5] = (float)h / (float)image_h;
    pc[6] = (float)x / (float)image_w;
    pc[7] = (float)y / (float)image_h;
    pc[8] = 2.0f - (float)g_up.sharpness / 200.0f;
    pc[12] = (transform & 1) ? 1.0f : 0.0f;
    pc[13] = (transform & 2) ? 1.0f : 0.0f;
    put_combined(f->sgsr, src);
    vk_barrier(cb, f->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    {
        VkRenderingAttachmentInfo at = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                        .imageView = f->view,
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {{0, 0}, {dw, dh}},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &at};
        VkViewport vp = {0, 0, (float)dw, (float)dh, 0, 1};
        VkRect2D sc = {{0, 0}, {dw, dh}};
        rec_CmdBeginRendering(cb, &ri);
        vk_render_forget_state();
        rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_up.sgsr);
        rec_CmdSetViewport(cb, 0, 1, &vp);
        rec_CmdSetScissor(cb, 0, 1, &sc);
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_up.sgsr_pl, 0, 1, &f->sgsr, 0, NULL);
        rec_CmdPushConstants(cb, g_up.sgsr_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc,
                             pc);
        rec_CmdDraw(cb, 3, 1, 0, 0);
        rec_CmdEndRendering(cb);
    }
    vk_barrier(cb, f->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return 1;
}

static int nis_config(NisConfig* c, float sharpness, u32 image_w, u32 image_h, u32 x, u32 y, u32 w, u32 h, u32 dw,
                      u32 dh) {
    float slider, sharpen_slider, max_scale, min_scale, limit_scale;
    float sharp_start_y, sharp_end_y, sharp_strength_min, sharp_strength_max, sharp_limit_min, sharp_limit_max;
    sharpness = clampf(sharpness, 0.0f, 1.0f);
    sharpen_slider = sharpness - 0.5f;
    max_scale = sharpen_slider >= 0.0f ? 1.25f : 1.75f;
    min_scale = sharpen_slider >= 0.0f ? 1.25f : 1.0f;
    limit_scale = sharpen_slider >= 0.0f ? 1.25f : 1.0f;
    slider = sharpen_slider;
    sharp_start_y = 0.45f;
    sharp_end_y = 0.9f;
    sharp_strength_min = fmaxf(0.0f, 0.4f + slider * min_scale * 1.2f);
    sharp_strength_max = 1.6f + slider * max_scale * 1.8f;
    sharp_limit_min = fmaxf(0.1f, 0.14f + slider * limit_scale * 0.32f);
    sharp_limit_max = 0.5f + slider * limit_scale * 0.6f;
    memset(c, 0, sizeof *c);
    c->kInputViewportOriginX = x;
    c->kInputViewportOriginY = y;
    c->kInputViewportWidth = w;
    c->kInputViewportHeight = h;
    c->kOutputViewportWidth = dw;
    c->kOutputViewportHeight = dh;
    c->kSrcNormX = 1.0f / (float)image_w;
    c->kSrcNormY = 1.0f / (float)image_h;
    c->kDstNormX = 1.0f / (float)dw;
    c->kDstNormY = 1.0f / (float)dh;
    c->kScaleX = (float)w / (float)dw;
    c->kScaleY = (float)h / (float)dh;
    if (c->kScaleX < 0.5f || c->kScaleX > 1.0f || c->kScaleY < 0.5f || c->kScaleY > 1.0f) return 0;
    c->kDetectRatio = 2.0f * 1127.0f / 1024.0f;
    c->kDetectThres = 64.0f / 1024.0f;
    c->kMinContrastRatio = 2.0f;
    c->kRatioNorm = 1.0f / (10.0f - 2.0f);
    c->kContrastBoost = 1.0f;
    c->kEps = 1.0f / 255.0f;
    c->kSharpStartY = sharp_start_y;
    c->kSharpScaleY = 1.0f / (sharp_end_y - sharp_start_y);
    c->kSharpStrengthMin = sharp_strength_min;
    c->kSharpStrengthScale = sharp_strength_max - sharp_strength_min;
    c->kSharpLimitMin = sharp_limit_min;
    c->kSharpLimitScale = sharp_limit_max - sharp_limit_min;
    return 1;
}

static int record_nis(VkCommandBuffer cb, VkImageView src, u32 image_w, u32 image_h, u32 x, u32 y, u32 w, u32 h, u32 dw,
                      u32 dh) {
    UpFrame* f = &g_up.frame[vk_frame_slot()];
    NisConfig cfg;
    VkBuffer buf;
    u64 off;
    u8* mapped;
    /* sharpness 0 is the sharpest, as FSR's; NIS's 1.0 is the sharpest and 0.5 is none */
    if (!nis_config(&cfg, 1.0f - (float)g_up.sharpness / 200.0f, image_w, image_h, x, y, w, h, dw, dh)) return 0;
    mapped = vk_stage(sizeof cfg, vk_stage_align(), &buf, &off);
    if (!mapped) return 0;
    memcpy(mapped, &cfg, sizeof cfg);

    {
        VkDescriptorBufferInfo bi = {buf, off, sizeof cfg};
        VkDescriptorImageInfo in = {VK_NULL_HANDLE, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo out = {VK_NULL_HANDLE, f->view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo smp = {g_up.samp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkDescriptorImageInfo cs = {VK_NULL_HANDLE, g_up.coef_view[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo cu = {VK_NULL_HANDLE, g_up.coef_view[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr[6] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = f->nis,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                       .pBufferInfo = &bi},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = f->nis,
                                       .dstBinding = 1,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                                       .pImageInfo = &smp},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = f->nis,
                                       .dstBinding = 2,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       .pImageInfo = &in},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = f->nis,
                                       .dstBinding = 3,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &out},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = f->nis,
                                       .dstBinding = 4,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       .pImageInfo = &cs},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = f->nis,
                                       .dstBinding = 5,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       .pImageInfo = &cu}};
        vkUpdateDescriptorSets(g_vk.dev, 6, wr, 0, NULL);
    }
    vk_barrier(cb, f->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                        g_up.fmt == VK_FORMAT_R8G8B8A8_UNORM ? g_up.nis8 : g_up.nis);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.nis_pl, 0, 1, &f->nis, 0, NULL);
    rec_CmdDispatch(cb, div_up(dw, NIS_BLOCK_W), div_up(dh, NIS_BLOCK_H), 1);
    vk_barrier(cb, f->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return 1;
}

int vk_upscale_process(VkCommandBuffer cb, VkImageView src, u32 image_w, u32 image_h, u32 x, u32 y, u32 w, u32 h,
                       u32 transform, u32 dst_w, u32 dst_h, int filter, int sharpness, VkImage* out,
                       VkImageView* out_view) {
    UpFrame* f;
    int ok;
    if (!src || !image_w || !image_h || !w || !h || !dst_w || !dst_h) return 0;
    g_up.sharpness = sharpness < 0 ? 0 : sharpness > 100 ? 100 : sharpness;
    if (filter == SNX_UPSCALE_AUTO)
        filter = snx_upscale_auto_pick(g_vk.props.vendorID, g_vk.props.deviceID, g_vk.props.deviceName);
    if (!snx_upscale_uses_sharpness(filter)) return 0;
    if (filter == SNX_UPSCALE_NIS && (transform || (u64)dst_w > (u64)w * 2 || (u64)dst_h > (u64)h * 2))
        filter = SNX_UPSCALE_FSR1;
    if (!make_common(cb) || !make_images(dst_w, dst_h, filter == SNX_UPSCALE_FSR1)) return 0;
    f = &g_up.frame[vk_frame_slot()];
    ok = filter == SNX_UPSCALE_NIS    ? record_nis(cb, src, image_w, image_h, x, y, w, h, dst_w, dst_h)
         : filter == SNX_UPSCALE_SGSR ? record_sgsr(cb, src, image_w, image_h, x, y, w, h, transform, dst_w, dst_h)
                                      : record_fsr(cb, src, image_w, image_h, x, y, w, h, transform, dst_w, dst_h);
    if (!ok) return 0;
    if (g_up.last_filter != filter || g_up.last_src_w != w || g_up.last_src_h != h || g_up.last_dst_w != dst_w ||
        g_up.last_dst_h != dst_h) {
        LOG("vulkan: upscaling %ux%u to %ux%u with %s (sharpness %d)\n", w, h, dst_w, dst_h, snx_upscale_label(filter),
            g_up.sharpness);
        g_up.last_filter = filter;
        g_up.last_src_w = w;
        g_up.last_src_h = h;
        g_up.last_dst_w = dst_w;
        g_up.last_dst_h = dst_h;
    }
    if (out) *out = f->img;
    if (out_view) *out_view = f->view;
    return 1;
}
