/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The HDR passes of the presented frame: hdr_compose.frag writes the frame's shown crop into a composition of linear
   light in nits and BT.2020's primaries, and hdr_encode.frag writes that into the window's image, PQ for HDR10 or the
   SDR range with a knee. The numbers are hdr_select.c's; the shaders carry the same ones in GLSL, and the tests hold
   the two to each other.

   The composition lives in the frame's slot, 16 bits a channel, at the window's size. The on-screen controls and the
   game's menu draw over it (vk/ui.c) in the same space, so an overlay's letters are not coded twice. */
#include <stdio.h>
#include <string.h>
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/swapchain.h"
#include "common/utilities/bits.h"
#include "nx/gpu/vk/hdr.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/hdr_spv.h"
#include "nx/runtime/platform/log.h"

#define HDR_COMPOSE_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
#define HDR_PC_COMPOSE   80 /* rect, crop, size, tone and expand, five vec4 */
#define HDR_PC_ENCODE    16 /* the tone vec4 */
#define HDR_ENC_PIPES    6  /* the pipelines kept: the swapchain's formats and the screenshot's RGBA8 */

typedef struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView view;
    VkDescriptorSet compose, encode;
} HdrFrame;

static struct {
    int mode;   /* atomic: SNX_HDR_OFF or ON */
    int source; /* SNX_HDR_SRC_*, from the setting */
    int paper, peak, expand, test;
    int fmt_checked, fmt_ok;
    int made, failed;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout compose_pl, encode_pl;
    VkPipeline compose_pipe, test_pipe;
    int display_kind; /* what the window's surface takes, asked once: -1 before */
    VkPipeline enc_pipe[HDR_ENC_PIPES];
    VkFormat enc_fmt[HDR_ENC_PIPES];
    int enc_n;
    VkDescriptorPool pool;
    VkSampler samp;
    HdrFrame frame[VK_FRAMES];
    u32 w, h;
    u32 last_src_mode, last_src_w, last_src_h, last_dst_w, last_dst_h;
    int last_dst_hdr;
} g_hdr = {.display_kind = -1}; /* what the surface takes is asked at the first present */

static int is_float_format(VkFormat f) {
    return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT ||
           f == VK_FORMAT_R16G16B16_SFLOAT || f == VK_FORMAT_R32G32B32_SFLOAT || f == VK_FORMAT_R16_SFLOAT ||
           f == VK_FORMAT_R32_SFLOAT || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
           f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
}

int vk_hdr_source_for(VkFormat fmt) {
    if (g_hdr.source != SNX_HDR_SRC_AUTO) return snx_hdr_source_value(g_hdr.source);
    return is_float_format(fmt) ? SNX_HDR_SRC_LINEAR : SNX_HDR_SRC_SDR;
}

int vk_hdr_frame_hdr(VkFormat fmt) { return vk_hdr_source_for(fmt) != SNX_HDR_SRC_SDR; }

int vk_hdr_frame_needs(VkFormat fmt) { return vk_hdr_active() || vk_hdr_frame_hdr(fmt); }

int vk_hdr_mode(void) { return __atomic_load_n(&g_hdr.mode, __ATOMIC_RELAXED); }

int vk_hdr_display_hdr(void) {
    int kind = vk_hdr_display_kind();
    if (kind == SNX_HDR_DISPLAY_NONE) return 0;
    /* an HDR10 target's signal is absolute: the surface taking it is the test. The extended kinds need the display's
       own headroom, which the system reports; 1.00 there means it can go no brighter than its SDR white */
    if (kind == SNX_HDR_DISPLAY_PQ) return 1;
    return __atomic_load_n(&g_display_edr_potential_pct, __ATOMIC_RELAXED) > 100;
}

int vk_hdr_active(void) {
    static int was = -1; /* the last answer logged: the display's headroom arrives after the window does */
    int mode = vk_hdr_mode(), on;
    on = mode == SNX_HDR_OFF ? 0 : mode == SNX_HDR_ON ? 1 : vk_hdr_display_hdr();
    if (on != __atomic_load_n(&was, __ATOMIC_RELAXED)) {
        __atomic_store_n(&was, on, __ATOMIC_RELAXED);
        if (on)
            LOG("vulkan: hdr on (%s, the display's EDR potential %u %%)\n",
                mode == SNX_HDR_AUTO ? "auto" : "on",
                __atomic_load_n(&g_display_edr_potential_pct, __ATOMIC_RELAXED));
        else
            LOG("vulkan: hdr off (%s)\n", mode == SNX_HDR_AUTO ? "auto: the display takes no HDR image or headroom"
                                                                : "off");
    }
    return on;
}

VkFormat vk_hdr_compose_format(void) { return HDR_COMPOSE_FORMAT; }
int vk_hdr_display_kind(void) {
    static const VkFormat hdr_formats[] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32};
    static const VkFormat scrgb_formats[] = {VK_FORMAT_R16G16B16A16_SFLOAT};
    VkSurfaceFormatKHR out = {0};
    if (g_hdr.display_kind >= 0) return g_hdr.display_kind;
    /* a frame asks this for each present: the surface's formats don't move under it */
    if (!g_vk.surface || !g_vk.has_swapchain_colorspace)
        g_hdr.display_kind = SNX_HDR_DISPLAY_NONE;
    else if (snx_vk_surface_format_cs(g_vk.phys, g_vk.surface, scrgb_formats, 1, VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT,
                                      &out))
        g_hdr.display_kind = SNX_HDR_DISPLAY_P3;
    else if (snx_vk_surface_format_cs(g_vk.phys, g_vk.surface, scrgb_formats, 1, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT,
                                      &out))
        g_hdr.display_kind = SNX_HDR_DISPLAY_SCRGB;
    else if (snx_vk_surface_format_cs(g_vk.phys, g_vk.surface, hdr_formats, 2, VK_COLOR_SPACE_HDR10_ST2084_EXT, &out))
        g_hdr.display_kind = SNX_HDR_DISPLAY_PQ;
    else
        g_hdr.display_kind = SNX_HDR_DISPLAY_NONE;
    LOG("vulkan: hdr: the window takes %s\n", snx_fx_display_label(g_hdr.display_kind));
    return g_hdr.display_kind;
}

int vk_hdr_paper_nits(void) { return g_hdr.paper; }
int vk_hdr_peak_nits(void) { return g_hdr.peak; }

int vk_hdr_available(void) {
    /* the composition's image is what the passes themselves need: the window's image is a format the surface lists */
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    VkFormatProperties p;
    if (g_hdr.fmt_checked) return g_hdr.fmt_ok;
    g_hdr.fmt_checked = 1;
    if (!g_vk.dev) return 0;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, HDR_COMPOSE_FORMAT, &p);
    g_hdr.fmt_ok = (p.optimalTilingFeatures & need) == need;
    return g_hdr.fmt_ok;
}

void vk_hdr_configure(int mode, int source, int paper_nits, int peak_nits, int expand, int test) {
    __atomic_store_n(&g_hdr.mode, mode >= SNX_HDR_OFF && mode < SNX_HDR_COUNT ? mode : SNX_HDR_OFF, __ATOMIC_RELAXED);
    g_hdr.source = source >= 0 && source < SNX_HDR_SRC_COUNT ? source : SNX_HDR_SRC_AUTO;
    g_hdr.paper = snx_clamp_int(paper_nits, 80, 480);
    g_hdr.peak = snx_clamp_int(peak_nits, 400, 4000);
    if (g_hdr.peak < g_hdr.paper) g_hdr.peak = g_hdr.paper;
    g_hdr.expand = snx_clamp_int(expand, 0, 100);
    g_hdr.test = test != 0;
    g_hdr.failed = 0;
    LOG("vulkan: hdr %s, source %s, paper white %d nits, peak %d nits, expansion %d%s\n", snx_hdr_key(vk_hdr_mode()),
        snx_hdr_source_key(g_hdr.source), g_hdr.paper, g_hdr.peak, g_hdr.expand,
        g_hdr.test ? ", the test pattern" : "");
    if (g_hdr.test)
        LOG("vulkan: hdr: the test pattern's patches are %d, %d, %d, %d, %d and %d nits, left to right\n", 0,
            g_hdr.paper, g_hdr.paper * 5 / 4, g_hdr.paper * 3 / 2, g_hdr.paper * 7 / 4, g_hdr.paper * 2);
}

int vk_hdr_toggle(void) {
    int mode = vk_hdr_active() ? SNX_HDR_OFF : SNX_HDR_ON;
    __atomic_store_n(&g_hdr.mode, mode, __ATOMIC_RELAXED);
    return mode;
}

/* the light the top of an SDR frame reaches, as a multiple of the paper white: the display's reported headroom (an
   HDR10 target's own peak standing in for it), its share being the expansion */
static float lift_at_white(int display) {
    float peak_rel = (float)g_hdr.peak / (float)g_hdr.paper;
    float max = display == SNX_HDR_DISPLAY_PQ
                    ? peak_rel
                    : (float)__atomic_load_n(&g_display_headroom_pct, __ATOMIC_RELAXED) / 100.0f;
    if (max < 1.0f) max = 1.0f;
    if (max > peak_rel) max = peak_rel;
    return 1.0f + (float)g_hdr.expand / 100.0f * (max - 1.0f);
}

void vk_hdr_describe(int display, char* out, size_t n) {
    if (!vk_hdr_active()) {
        snprintf(out, n, vk_hdr_mode() == SNX_HDR_AUTO ? "Off (auto: this display takes no HDR)" : "Off");
        return;
    }
    {
        float lift = lift_at_white(display);
        float headroom = (float)__atomic_load_n(&g_display_headroom_pct, __ATOMIC_RELAXED) / 100.0f;
        snprintf(out, n, "On (%s, paper white %d nits, headroom %.2f, white at %.2f)",
                 display == SNX_HDR_DISPLAY_P3      ? "Display P3"
                 : display == SNX_HDR_DISPLAY_SCRGB ? "scRGB"
                 : display == SNX_HDR_DISPLAY_PQ    ? "HDR10"
                                                    : "SDR",
                 g_hdr.paper, (double)headroom, (double)lift);
    }
}

/* ---- the device objects ---- */
static VkPipeline make_gfx(const u32* vs, u64 vs_bytes, const u32* fs, u64 fs_bytes, VkPipelineLayout layout,
                           VkFormat fmt) {
    VkShaderModule v = vk_module_spv(vs, (size_t)(vs_bytes / 4)), f = vk_module_spv(fs, (size_t)(fs_bytes / 4));
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

static int make_layouts(void) {
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                      NULL};
    VkDescriptorSetLayoutCreateInfo dl = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
    VkPushConstantRange pc_compose = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, HDR_PC_COMPOSE};
    VkPushConstantRange pc_encode = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, HDR_PC_ENCODE};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_hdr.dsl,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pc_compose};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_hdr.dsl))) return 0;
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &g_hdr.compose_pl))) return 0;
    pl.pPushConstantRanges = &pc_encode;
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &g_hdr.encode_pl))) return 0;
    return 1;
}

static void free_frame(HdrFrame* f) {
    if (f->img || f->view) vk_defer_image(f->img, f->mem, f->view, VK_NULL_HANDLE);
    f->img = VK_NULL_HANDLE;
    f->mem = VK_NULL_HANDLE;
    f->view = VK_NULL_HANDLE;
}

void vk_hdr_release(void) {
    for (int i = 0; i < VK_FRAMES; ++i) free_frame(&g_hdr.frame[i]);
    for (int i = 0; i < g_hdr.enc_n; ++i)
        if (g_hdr.enc_pipe[i]) vkDestroyPipeline(g_vk.dev, g_hdr.enc_pipe[i], NULL);
    if (g_hdr.compose_pipe) vkDestroyPipeline(g_vk.dev, g_hdr.compose_pipe, NULL);
    if (g_hdr.test_pipe) vkDestroyPipeline(g_vk.dev, g_hdr.test_pipe, NULL);
    if (g_hdr.compose_pl) vkDestroyPipelineLayout(g_vk.dev, g_hdr.compose_pl, NULL);
    if (g_hdr.encode_pl) vkDestroyPipelineLayout(g_vk.dev, g_hdr.encode_pl, NULL);
    if (g_hdr.dsl) vkDestroyDescriptorSetLayout(g_vk.dev, g_hdr.dsl, NULL);
    if (g_hdr.pool) vkDestroyDescriptorPool(g_vk.dev, g_hdr.pool, NULL);
    if (g_hdr.samp) vkDestroySampler(g_vk.dev, g_hdr.samp, NULL);
    g_hdr.compose_pipe = g_hdr.test_pipe = VK_NULL_HANDLE;
    g_hdr.compose_pl = g_hdr.encode_pl = VK_NULL_HANDLE;
    g_hdr.dsl = VK_NULL_HANDLE;
    g_hdr.pool = VK_NULL_HANDLE;
    g_hdr.samp = VK_NULL_HANDLE;
    g_hdr.enc_n = 0;
    g_hdr.display_kind = -1; /* the surface's formats are asked anew */
    g_hdr.made = g_hdr.failed = 0;
    g_hdr.w = g_hdr.h = 0;
    g_hdr.last_src_mode = g_hdr.last_src_w = g_hdr.last_src_h = 0;
    g_hdr.last_dst_w = g_hdr.last_dst_h = 0;
    g_hdr.last_dst_hdr = -1;
    memset(g_hdr.frame, 0, sizeof g_hdr.frame);
    memset(g_hdr.enc_fmt, 0, sizeof g_hdr.enc_fmt);
}

static int make_common(void) {
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_FRAMES * 2};
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = VK_FRAMES * 2, .poolSizeCount = 1,
        .pPoolSizes = &size};
    if (g_hdr.made) return 1;
    if (g_hdr.failed) return 0;
    if (!VK_OK(vkCreateSampler(g_vk.dev, &si, NULL, &g_hdr.samp)) || !make_layouts() ||
        !VK_OK(vkCreateDescriptorPool(g_vk.dev, &pci, NULL, &g_hdr.pool)))
        goto failed;
    g_hdr.compose_pipe = make_gfx(hdr_vert_spv, hdr_vert_spv_size, hdr_compose_frag_spv, hdr_compose_frag_spv_size,
                                  g_hdr.compose_pl, HDR_COMPOSE_FORMAT);
    /* the test pattern's push constants are the compose layout's, which are wide enough for its own */
    g_hdr.test_pipe = make_gfx(hdr_vert_spv, hdr_vert_spv_size, hdr_test_frag_spv, hdr_test_frag_spv_size,
                               g_hdr.compose_pl, HDR_COMPOSE_FORMAT);
    if (!g_hdr.compose_pipe || !g_hdr.test_pipe) goto failed;
    for (int i = 0; i < VK_FRAMES; ++i) {
        VkDescriptorSetLayout l[2] = {g_hdr.dsl, g_hdr.dsl};
        VkDescriptorSet sets[2];
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = g_hdr.pool,
                                          .descriptorSetCount = 2,
                                          .pSetLayouts = l};
        if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, sets))) goto failed;
        g_hdr.frame[i].compose = sets[0];
        g_hdr.frame[i].encode = sets[1];
    }
    g_hdr.made = 1;
    return 1;
failed:
    g_hdr.failed = 1;
    WARN("vulkan: the HDR passes could not be made, the frame is blitted\n");
    return 0;
}

static int make_frame_image(HdrFrame* f, u32 w, u32 h) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = HDR_COMPOSE_FORMAT,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = HDR_COMPOSE_FORMAT,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    free_frame(f);
    if (!vk_image_create(&ii, &f->img, &f->mem)) return 0;
    vi.image = f->img;
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &f->view))) {
        vk_image_destroy(f->img, f->mem);
        f->img = VK_NULL_HANDLE;
        f->mem = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

static int make_images(u32 w, u32 h) {
    if (g_hdr.w != w || g_hdr.h != h) {
        for (int i = 0; i < VK_FRAMES; ++i) free_frame(&g_hdr.frame[i]);
        g_hdr.w = w;
        g_hdr.h = h;
    }
    for (int i = 0; i < VK_FRAMES; ++i)
        if (!g_hdr.frame[i].img && !make_frame_image(&g_hdr.frame[i], w, h)) return 0;
    return 1;
}

static VkPipeline encode_pipeline(VkFormat fmt) {
    for (int i = 0; i < g_hdr.enc_n; ++i)
        if (g_hdr.enc_fmt[i] == fmt) return g_hdr.enc_pipe[i];
    if (g_hdr.enc_n >= HDR_ENC_PIPES) return VK_NULL_HANDLE;
    g_hdr.enc_pipe[g_hdr.enc_n] = make_gfx(hdr_vert_spv, hdr_vert_spv_size, hdr_encode_frag_spv,
                                           hdr_encode_frag_spv_size, g_hdr.encode_pl, fmt);
    if (!g_hdr.enc_pipe[g_hdr.enc_n]) return VK_NULL_HANDLE;
    g_hdr.enc_fmt[g_hdr.enc_n] = fmt;
    return g_hdr.enc_pipe[g_hdr.enc_n++];
}

/* ---- recording ---- */
static void put_combined(VkDescriptorSet set, VkImageView view) {
    VkDescriptorImageInfo di = {g_hdr.samp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = set,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &di};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
}

int vk_hdr_compose(VkCommandBuffer cb, VkImageView src, int source, u32 img_w, u32 img_h, u32 x, u32 y, u32 w, u32 h,
                   u32 transform, u32 dst_w, u32 dst_h, i32 dx, i32 dy, u32 dw, u32 dh, VkImage* out,
                   VkImageView* out_view) {
    HdrFrame* f;
    struct {
        float rect[4];
        float crop[4];
        float size[4];
        float tone[4];
        float expand[4]; /* the SDR expansion 0 to 1, the chroma's scale about the luminance, 0, 0 */
    } pc;
    VkRenderingAttachmentInfo at;
    VkRenderingInfo ri;
    VkViewport vp;
    VkRect2D sc;
    /* a NULL src is the composition cleared to black alone: the menu's screen before the game's first frame */
    if ((src && (!img_w || !img_h || !w || !h || !dw || !dh)) || !dst_w || !dst_h) return 0;
    if (!vk_hdr_available() || !make_common() || !make_images(dst_w, dst_h)) return 0;
    f = &g_hdr.frame[vk_frame_slot()];
    if (src) put_combined(f->compose, src);
    memset(&pc, 0, sizeof pc);
    pc.rect[0] = (float)dx;
    pc.rect[1] = (float)dy;
    pc.rect[2] = (float)dw;
    pc.rect[3] = (float)dh;
    pc.crop[0] = (float)x;
    pc.crop[1] = (float)y;
    pc.crop[2] = (float)w;
    pc.crop[3] = (float)h;
    pc.size[0] = (float)img_w;
    pc.size[1] = (float)img_h;
    pc.size[2] = (transform & 1) ? 1.0f : 0.0f;
    pc.size[3] = (transform & 2) ? 1.0f : 0.0f;
    pc.tone[0] = (float)g_hdr.paper;
    /* the light the top of the frame may reach: the display's own headroom where it reports one, else the peak the
       settings name (an HDR10 target's), as a multiple of the paper white */
    pc.tone[1] = lift_at_white(g_vk.sc_hdr && !g_vk.sc_p3 && !g_vk.sc_scrgb ? SNX_HDR_DISPLAY_PQ : SNX_HDR_DISPLAY_P3);
    pc.tone[2] = (float)g_hdr.peak;
    pc.tone[3] = (float)source;
    pc.expand[0] = (float)g_hdr.expand / 100.0f;
    at = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                     .imageView = f->view,
                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    at.clearValue.color.float32[0] = 0.0f;
    at.clearValue.color.float32[1] = 0.0f;
    at.clearValue.color.float32[2] = 0.0f;
    at.clearValue.color.float32[3] = 1.0f;
    ri = (VkRenderingInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                           .renderArea = {{0, 0}, {dst_w, dst_h}},
                           .layerCount = 1,
                           .colorAttachmentCount = 1,
                           .pColorAttachments = &at};
    vp = (VkViewport){0, 0, (float)dst_w, (float)dst_h, 0, 1};
    {
        i32 sx = dx < 0 ? 0 : dx, sy = dy < 0 ? 0 : dy;
        i32 ex = dx + (i32)dw, ey = dy + (i32)dh;
        if (ex > (i32)dst_w) ex = (i32)dst_w;
        if (ey > (i32)dst_h) ey = (i32)dst_h;
        sc.offset.x = sx;
        sc.offset.y = sy;
        sc.extent.width = ex > sx ? (u32)(ex - sx) : 0;
        sc.extent.height = ey > sy ? (u32)(ey - sy) : 0;
    }
    vk_barrier(cb, f->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rec_CmdBeginRendering(cb, &ri);
    if (src) {
        vk_render_forget_state();
        rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_hdr.compose_pipe);
        rec_CmdSetViewport(cb, 0, 1, &vp);
        rec_CmdSetScissor(cb, 0, 1, &sc);
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_hdr.compose_pl, 0, 1, &f->compose, 0, NULL);
        rec_CmdPushConstants(cb, g_hdr.compose_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        rec_CmdDraw(cb, 3, 1, 0, 0);
    }
    rec_CmdEndRendering(cb);
    if (g_hdr.test) {
        /* six patches of known light over the frame's bottom eighth: 0, the paper white, and 1.25 to 2 times it */
        u32 band = dst_h / 8 ? dst_h / 8 : 1;
        VkRenderingAttachmentInfo tat = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                         .imageView = f->view,
                                         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo tri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                               .renderArea = {{0, (i32)(dst_h - band)}, {dst_w, band}},
                               .layerCount = 1,
                               .colorAttachmentCount = 1,
                               .pColorAttachments = &tat};
        VkViewport tvp = {0, 0, (float)dst_w, (float)dst_h, 0, 1};
        VkRect2D tsc = {{0, (i32)(dst_h - band)}, {dst_w, band}};
        float tp[4] = {(float)g_hdr.paper, (float)g_hdr.peak, (float)dst_w, (float)band};
        rec_CmdBeginRendering(cb, &tri);
        vk_render_forget_state();
        rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_hdr.test_pipe);
        rec_CmdSetViewport(cb, 0, 1, &tvp);
        rec_CmdSetScissor(cb, 0, 1, &tsc);
        rec_CmdPushConstants(cb, g_hdr.compose_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, tp);
        rec_CmdDraw(cb, 3, 1, 0, 0);
        rec_CmdEndRendering(cb);
    }
    if (out) *out = f->img;
    if (out_view) *out_view = f->view;
    {
        int kind = g_vk.sc_p3      ? VK_HDR_ENC_P3
                   : g_vk.sc_scrgb ? VK_HDR_ENC_SCRGB
                   : g_vk.sc_hdr   ? VK_HDR_ENC_PQ
                                   : VK_HDR_ENC_SDR;
        int changed = g_hdr.last_src_mode != (u32)source || g_hdr.last_src_w != w || g_hdr.last_src_h != h ||
                      g_hdr.last_dst_w != dst_w || g_hdr.last_dst_h != dst_h || g_hdr.last_dst_hdr != kind;
        if (changed) {
            LOG("vulkan: hdr %s: %ux%u from %ux%u %s at %d,%d, paper white %d nits, peak %d nits, %s (headroom %.2f, "
                "white at %.2f)\n",
                vk_hdr_active() ? "on" : "off", dst_w, dst_h, w, h, snx_hdr_source_key(source), dx, dy,
                g_hdr.paper, g_hdr.peak,
                kind == VK_HDR_ENC_P3      ? "Display P3"
                : kind == VK_HDR_ENC_SCRGB ? "scRGB"
                : kind == VK_HDR_ENC_PQ    ? "HDR10 (PQ)"
                                           : "the SDR range",
                (double)__atomic_load_n(&g_display_headroom_pct, __ATOMIC_RELAXED) / 100.0,
                (double)pc.tone[1]);
            g_hdr.last_src_mode = (u32)source;
            g_hdr.last_src_w = w;
            g_hdr.last_src_h = h;
            g_hdr.last_dst_w = dst_w;
            g_hdr.last_dst_h = dst_h;
            g_hdr.last_dst_hdr = kind;
        }
    }
    return 1;
}

int vk_hdr_encode(VkCommandBuffer cb, VkImage composed, VkImageView composed_view, u32 w, u32 h, VkImage dst,
                  VkImageView dst_view, VkFormat dst_fmt, int enc) {
    HdrFrame* f = &g_hdr.frame[vk_frame_slot()];
    VkPipeline pipe = encode_pipeline(dst_fmt);
    struct {
        float tone[4];
    } pc = {{(float)g_hdr.paper, (float)g_hdr.peak, (float)enc,
             (dst_fmt == VK_FORMAT_R8G8B8A8_SRGB || dst_fmt == VK_FORMAT_B8G8R8A8_SRGB) ? 1.0f : 0.0f}};
    VkRenderingAttachmentInfo at;
    VkRenderingInfo ri;
    VkViewport vp;
    VkRect2D sc;
    if (!composed || !composed_view || !dst || !dst_view || !w || !h) return 0;
    if (!pipe) return 0; /* the target's pipeline could not be made */
    /* the composition comes from the compose pass or the menu's over it, an attachment either way */
    vk_barrier(cb, composed, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    put_combined(f->encode, composed_view);
    at = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                     .imageView = dst_view,
                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    ri = (VkRenderingInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                           .renderArea = {{0, 0}, {w, h}},
                           .layerCount = 1,
                           .colorAttachmentCount = 1,
                           .pColorAttachments = &at};
    vp = (VkViewport){0, 0, (float)w, (float)h, 0, 1};
    sc = (VkRect2D){{0, 0}, {w, h}};
    vk_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rec_CmdBeginRendering(cb, &ri);
    vk_render_forget_state();
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_hdr.encode_pl, 0, 1, &f->encode, 0, NULL);
    rec_CmdPushConstants(cb, g_hdr.encode_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    return 1;
}
