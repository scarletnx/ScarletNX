/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The presentation effects of every Vulkan frontend (present_fx.h). The FSR 1, FXAA and SMAA shaders are built from
   the same GLSL the Switch frontend uses, and the HDR composition and encode are the Switch frontend's passes
   (src/nx/gpu/vk/hdr.c), with a device of the frontend's own. The module owns only its render targets and pipelines:
   the source and the target belong to the caller. */
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/fx/AreaTex.h"
#include "common/gpu/vk/fx/SearchTex.h"
#include "common/gpu/vk/fx/fx_fsr.h"
#include "common/gpu/vk/fx_spv.h"
#include "common/gpu/vk/swapchain.h"
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/bits.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNX_FX_SLOTS_MAX 8
#define SNX_FX_PUSH      128

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t width, height;
    VkFormat format;
} FxImage;

struct SnxFx {
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkPipelineCache cache;
    uint32_t slots;
    uint32_t (*slot)(void);
    void (*forget)(void);
    void (*log)(const char*);
    SnxFxConfig cfg;
    int display_kind;
    int headroom_pct;
    int fp16;
    int fmt_checked, fmt_ok;
    VkFormat fmt;
    int comp_checked, comp_ok;
    VkSampler sampler;
    VkDescriptorSetLayout dsl0, dsl1, dsl2, dsl3;
    VkPipelineLayout pl0, pl1, pl2, pl3;
    VkDescriptorPool pool;
    VkPipeline easu, rcas, easu16, rcas16;
    VkPipeline copy, copy_linear;
    VkPipeline blend;
    VkFormat blend_fmt;
    VkPipeline fxaa;
    VkPipeline smaa_edge, smaa_weights, smaa_blend;
    VkPipeline compose, encode[4], test;
    VkFormat encode_fmt[4];
    int pipes_ready, pipes_failed;
    FxImage main[SNX_FX_SLOTS_MAX];
    FxImage mid[SNX_FX_SLOTS_MAX];
    FxImage aa[SNX_FX_SLOTS_MAX];
    FxImage edge[SNX_FX_SLOTS_MAX];
    FxImage weight[SNX_FX_SLOTS_MAX];
    FxImage comp[SNX_FX_SLOTS_MAX];
    FxImage keep[SNX_FX_SLOTS_MAX];
    FxImage blend_img[SNX_FX_SLOTS_MAX];
    VkDescriptorSet fsr_easu[SNX_FX_SLOTS_MAX], fsr_rcas[SNX_FX_SLOTS_MAX];
    VkDescriptorSet copy_set[SNX_FX_SLOTS_MAX], fxaa_set[SNX_FX_SLOTS_MAX];
    VkDescriptorSet edge_set[SNX_FX_SLOTS_MAX], weight_set[SNX_FX_SLOTS_MAX], blend_set[SNX_FX_SLOTS_MAX];
    VkDescriptorSet mix_set[SNX_FX_SLOTS_MAX];
    VkDescriptorSet compose_set[SNX_FX_SLOTS_MAX], encode_set[SNX_FX_SLOTS_MAX];
    VkImage area, search;
    VkDeviceMemory area_mem, search_mem;
    VkImageView area_view, search_view;
    VkBuffer tex_buf;
    VkDeviceMemory tex_mem;
    int tex_uploaded;
    int aa_logged;
};

static int fx_ok(VkResult r) { return r == VK_SUCCESS; }

static void fx_echo(SnxFx* fx, const char* text) {
    if (fx->log) fx->log(text);
}

static int equal_ci(const char* a, const char* b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        ++a;
        ++b;
    }
    return !*a && !*b;
}

/* ---- the pure choices ---- */

static int parse_word(const char* value, const char* const* words, const signed char* ids, int count, int fallback) {
    char text[16];
    size_t i, n;
    if (!value) return fallback;
    while (*value == ' ' || *value == '\t') value++;
    n = strlen(value);
    while (n && (value[n - 1] == ' ' || value[n - 1] == '\t')) n--;
    if (n >= sizeof text) return fallback;
    memcpy(text, value, n);
    text[n] = 0;
    for (i = 0; i < (size_t)count; i++)
        if (equal_ci(text, words[i])) return ids[i];
    return fallback;
}

int snx_fx_upscale_parse(const char* value) {
    static const char* const words[] = {"off", "none", "bilinear", "fsr1", "fsr", "nis", "sgsr"};
    static const signed char ids[] = {SNX_FX_UPSCALE_OFF,  SNX_FX_UPSCALE_OFF, SNX_FX_UPSCALE_BILINEAR,
                                      SNX_FX_UPSCALE_FSR1, SNX_FX_UPSCALE_FSR1, SNX_FX_UPSCALE_NIS,
                                      SNX_FX_UPSCALE_SGSR};
    return parse_word(value, words, ids, (int)(sizeof ids / sizeof *ids), SNX_FX_UPSCALE_FSR1);
}
const char* snx_fx_upscale_key(int filter) {
    static const char* const keys[] = {"off", "bilinear", "fsr1", "nis", "sgsr"};
    return filter >= 0 && filter < SNX_FX_UPSCALE_COUNT ? keys[filter] : keys[SNX_FX_UPSCALE_FSR1];
}
const char* snx_fx_upscale_label(int filter) {
    static const char* const labels[] = {"None", "Bilinear", "AMD FSR 1", "NVIDIA NIS", "Qualcomm SGSR"};
    return filter >= 0 && filter < SNX_FX_UPSCALE_COUNT ? labels[filter] : labels[SNX_FX_UPSCALE_FSR1];
}

int snx_fx_aa_parse(const char* value) {
    static const char* const words[] = {"off", "none", "auto", "fxaa", "smaa", "ssaa2", "ssaa4", "ssaa 2x", "ssaa 4x"};
    static const signed char ids[] = {SNX_FX_AA_OFF,  SNX_FX_AA_OFF,  SNX_FX_AA_AUTO, SNX_FX_AA_FXAA, SNX_FX_AA_SMAA,
                                      SNX_FX_AA_SSAA2, SNX_FX_AA_SSAA4, SNX_FX_AA_SSAA2, SNX_FX_AA_SSAA4};
    return parse_word(value, words, ids, (int)(sizeof ids / sizeof *ids), SNX_FX_AA_AUTO);
}
const char* snx_fx_aa_key(int aa) {
    static const char* const keys[] = {"off", "auto", "fxaa", "smaa", "ssaa2", "ssaa4"};
    return aa >= 0 && aa < SNX_FX_AA_COUNT ? keys[aa] : keys[SNX_FX_AA_AUTO];
}
const char* snx_fx_aa_label(int aa) {
    static const char* const labels[] = {"Off", "Auto", "FXAA", "SMAA", "SSAA 2x", "SSAA 4x"};
    return aa >= 0 && aa < SNX_FX_AA_COUNT ? labels[aa] : labels[SNX_FX_AA_AUTO];
}

int snx_fx_hdr_parse(const char* value) {
    static const char* const words[] = {"off", "none", "sdr", "auto", "on", "hdr", "hdr10"};
    static const signed char ids[] = {SNX_FX_HDR_OFF, SNX_FX_HDR_OFF, SNX_FX_HDR_OFF, SNX_FX_HDR_AUTO,
                                      SNX_FX_HDR_ON,  SNX_FX_HDR_ON,  SNX_FX_HDR_ON};
    return parse_word(value, words, ids, (int)(sizeof ids / sizeof *ids), SNX_FX_HDR_OFF);
}
const char* snx_fx_hdr_key(int mode) {
    static const char* const keys[] = {"off", "auto", "on"};
    return mode >= 0 && mode < SNX_FX_HDR_COUNT ? keys[mode] : keys[SNX_FX_HDR_OFF];
}
const char* snx_fx_hdr_label(int mode) {
    static const char* const labels[] = {"Off", "Auto", "On"};
    return mode >= 0 && mode < SNX_FX_HDR_COUNT ? labels[mode] : labels[SNX_FX_HDR_OFF];
}

int snx_fx_colorspace_parse(const char* value) {
    static const char* const words[] = {"auto", "detect", "p3", "display p3", "p3 linear", "scrgb",
                                        "extended srgb", "linear", "hdr10", "pq", "2084"};
    static const signed char ids[] = {SNX_FX_CS_AUTO,  SNX_FX_CS_AUTO,  SNX_FX_CS_P3,    SNX_FX_CS_P3,
                                      SNX_FX_CS_P3,    SNX_FX_CS_SCRGB, SNX_FX_CS_SCRGB, SNX_FX_CS_SCRGB,
                                      SNX_FX_CS_HDR10, SNX_FX_CS_HDR10, SNX_FX_CS_HDR10};
    return parse_word(value, words, ids, (int)(sizeof ids / sizeof *ids), SNX_FX_CS_AUTO);
}
const char* snx_fx_colorspace_key(int cs) {
    static const char* const keys[] = {"auto", "p3", "scrgb", "hdr10"};
    return cs >= 0 && cs < SNX_FX_CS_COUNT ? keys[cs] : keys[SNX_FX_CS_AUTO];
}
const char* snx_fx_colorspace_label(int cs) {
    static const char* const labels[] = {"Auto", "Display P3", "scRGB", "HDR10 (PQ)"};
    return cs >= 0 && cs < SNX_FX_CS_COUNT ? labels[cs] : labels[SNX_FX_CS_AUTO];
}

int snx_fx_source_parse(const char* value) {
    static const char* const words[] = {"auto", "detect", "format", "sdr", "srgb", "linear", "hdr", "lin", "pq", "hdr10",
                                        "st2084", "2084"};
    static const signed char ids[] = {SNX_FX_SRC_AUTO,  SNX_FX_SRC_AUTO,  SNX_FX_SRC_AUTO,   SNX_FX_SRC_SDR,
                                      SNX_FX_SRC_SDR,   SNX_FX_SRC_LINEAR, SNX_FX_SRC_LINEAR, SNX_FX_SRC_LINEAR,
                                      SNX_FX_SRC_PQ,    SNX_FX_SRC_PQ,    SNX_FX_SRC_PQ,     SNX_FX_SRC_PQ};
    return parse_word(value, words, ids, (int)(sizeof ids / sizeof *ids), SNX_FX_SRC_AUTO);
}
const char* snx_fx_source_key(int source) {
    static const char* const keys[] = {"auto", "sdr", "linear", "pq"};
    return source >= 0 && source < SNX_FX_SRC_COUNT ? keys[source] : keys[SNX_FX_SRC_AUTO];
}
const char* snx_fx_source_label(int source) {
    static const char* const labels[] = {"Auto", "SDR", "Linear", "PQ (HDR10)"};
    return source >= 0 && source < SNX_FX_SRC_COUNT ? labels[source] : labels[SNX_FX_SRC_AUTO];
}

int snx_fx_interp_parse(const char* value) {
    static const char* const words[] = {"off", "none", "auto", "blend", "on"};
    static const signed char ids[] = {SNX_FX_INTERP_OFF, SNX_FX_INTERP_OFF, SNX_FX_INTERP_AUTO,
                                      SNX_FX_INTERP_BLEND, SNX_FX_INTERP_BLEND};
    return parse_word(value, words, ids, (int)(sizeof ids / sizeof *ids), SNX_FX_INTERP_OFF);
}
const char* snx_fx_interp_key(int mode) {
    static const char* const keys[] = {"off", "auto", "blend"};
    return mode >= 0 && mode < SNX_FX_INTERP_COUNT ? keys[mode] : keys[SNX_FX_INTERP_OFF];
}
const char* snx_fx_interp_label(int mode) {
    static const char* const labels[] = {"Off", "Auto", "Blend"};
    return mode >= 0 && mode < SNX_FX_INTERP_COUNT ? labels[mode] : labels[SNX_FX_INTERP_OFF];
}

int snx_fx_interp_auto(int display_hz, int game_hz) {
    if (display_hz <= 0 || game_hz <= 0) return 0;
    /* 1.9x: a 60 Hz game on a 120 Hz display runs, and a 60 Hz game on a 100 Hz one does not, because the second
       frame would wait for a refresh and the game would lose its pace. A 30 Hz game on 60 Hz runs. */
    return display_hz * 10 >= game_hz * 19;
}

int snx_fx_filter_uses_sharpness(int filter) {
    return filter == SNX_FX_UPSCALE_FSR1 || filter == SNX_FX_UPSCALE_NIS || filter == SNX_FX_UPSCALE_SGSR;
}

int snx_fx_frame_hdr(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R16G16B16_SFLOAT:
        case VK_FORMAT_R32G32B32_SFLOAT: return 1;
        default: return 0;
    }
}

int snx_fx_source_for(const SnxFx* fx, VkFormat fmt) {
    if (fx->cfg.source != SNX_FX_SRC_AUTO)
        return fx->cfg.source == SNX_FX_SRC_LINEAR || fx->cfg.source == SNX_FX_SRC_PQ ? fx->cfg.source : SNX_FX_SRC_SDR;
    return snx_fx_frame_hdr(fmt) ? SNX_FX_SRC_LINEAR : SNX_FX_SRC_SDR;
}

int snx_fx_display_kind(const SnxFx* fx) { return fx->display_kind; }

int snx_fx_hdr_active(const SnxFx* fx) {
    if (fx->cfg.hdr == SNX_FX_HDR_OFF) return 0;
    if (fx->cfg.hdr == SNX_FX_HDR_ON) return fx->display_kind != SNX_FX_DISPLAY_NONE;
    /* an HDR10 signal is absolute, so the surface taking it is the test; the extended kinds need headroom over white */
    if (fx->display_kind == SNX_FX_DISPLAY_PQ) return 1;
    return fx->display_kind != SNX_FX_DISPLAY_NONE && fx->headroom_pct > 100;
}

int snx_fx_frame_needs(const SnxFx* fx, VkFormat fmt) {
    return snx_fx_hdr_active(fx) || snx_fx_source_for(fx, fmt) != SNX_FX_SRC_SDR || snx_fx_frame_hdr(fmt);
}

VkFormat snx_fx_compose_format(void) { return VK_FORMAT_R16G16B16A16_SFLOAT; }

/* Auto anti aliasing: the displayed rectangle is at least 1.25x the rendered frame in each direction, and the frame
   is not larger than the rectangle (a downscale is already smoothed). 1.25 is the ratio dynamic resolution scalers
   use as the point where a lower render size starts to show its edges. */
int snx_fx_aa_auto(uint32_t render_w, uint32_t render_h, uint32_t shown_w, uint32_t shown_h) {
    if (!render_w || !render_h || !shown_w || !shown_h) return 0;
    if (shown_w < render_w || shown_h < render_h) return 0;
    return (uint64_t)shown_w * 4 >= (uint64_t)render_w * 5 && (uint64_t)shown_h * 4 >= (uint64_t)render_h * 5;
}

uint32_t snx_fx_target(uint32_t w, uint32_t h, uint32_t mult, uint32_t max_dim, uint32_t* out_w, uint32_t* out_h) {
    if (mult < 1) mult = 1;
    while (mult > 1 && ((uint64_t)w * mult > max_dim || (uint64_t)h * mult > max_dim)) mult--;
    if (out_w) *out_w = w * mult;
    if (out_h) *out_h = h * mult;
    return mult;
}

int snx_fx_probe_display(VkPhysicalDevice phys, VkSurfaceKHR surface, int colorspace, VkFormat* out_format,
                         VkColorSpaceKHR* out_space) {
    static const VkFormat ten[2] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32};
    static const VkFormat fp16[1] = {VK_FORMAT_R16G16B16A16_SFLOAT};
    VkSurfaceFormatKHR f;
    if (!surface) return SNX_FX_DISPLAY_NONE;
    if (colorspace == SNX_FX_CS_AUTO || colorspace == SNX_FX_CS_P3) {
        if (snx_vk_surface_format_cs(phys, surface, fp16, 1, VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT, &f)) {
            *out_format = f.format;
            *out_space = f.colorSpace;
            return SNX_FX_DISPLAY_P3;
        }
        if (colorspace == SNX_FX_CS_P3) return SNX_FX_DISPLAY_NONE;
    }
    if (colorspace == SNX_FX_CS_AUTO || colorspace == SNX_FX_CS_SCRGB) {
        if (snx_vk_surface_format_cs(phys, surface, fp16, 1, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, &f)) {
            *out_format = f.format;
            *out_space = f.colorSpace;
            return SNX_FX_DISPLAY_SCRGB;
        }
        if (colorspace == SNX_FX_CS_SCRGB) return SNX_FX_DISPLAY_NONE;
    }
    if (snx_vk_surface_format_cs(phys, surface, ten, 2, VK_COLOR_SPACE_HDR10_ST2084_EXT, &f)) {
        *out_format = f.format;
        *out_space = f.colorSpace;
        return SNX_FX_DISPLAY_PQ;
    }
    return SNX_FX_DISPLAY_NONE;
}

/* ---- images ---- */

static void img_free(SnxFx* fx, FxImage* im) {
    if (im->view) vkDestroyImageView(fx->dev, im->view, NULL);
    if (im->image) vkDestroyImage(fx->dev, im->image, NULL);
    if (im->memory) vkFreeMemory(fx->dev, im->memory, NULL);
    *im = (FxImage){0};
}

static int img_make(SnxFx* fx, FxImage* im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    if (im->image && im->width == w && im->height == h && im->format == fmt) return 1;
    if (im->image) {
        vkDeviceWaitIdle(fx->dev);
        img_free(fx, im);
    }
    if (!w || !h) return 0;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (!fx_ok(vkCreateImage(fx->dev, &ii, NULL, &im->image))) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(fx->dev, im->image, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(fx->phys, &mp);
    ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, 0);
    if (ai.memoryTypeIndex == UINT32_MAX || !fx_ok(vkAllocateMemory(fx->dev, &ai, NULL, &im->memory)) ||
        !fx_ok(vkBindImageMemory(fx->dev, im->image, im->memory, 0))) {
        img_free(fx, im);
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = im->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!fx_ok(vkCreateImageView(fx->dev, &vi, NULL, &im->view))) {
        img_free(fx, im);
        return 0;
    }
    im->width = w;
    im->height = h;
    im->format = fmt;
    return 1;
}

static VkImageUsageFlags fx_usage(void) {
    return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           VK_IMAGE_USAGE_STORAGE_BIT;
}

static VkFormat choose_format(SnxFx* fx) {
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (fx->fmt) return fx->fmt;
    VkFormatProperties p;
    VkFormat f = VK_FORMAT_R16G16B16A16_SFLOAT;
    vkGetPhysicalDeviceFormatProperties(fx->phys, f, &p);
    if ((p.optimalTilingFeatures & need) != need) f = VK_FORMAT_R8G8B8A8_UNORM;
    fx->fmt = f;
    return f;
}

static int format_ok(SnxFx* fx) {
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (!fx->fmt_checked) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(fx->phys, choose_format(fx), &p);
        fx->fmt_ok = (p.optimalTilingFeatures & need) == need;
        fx->fmt_checked = 1;
    }
    return fx->fmt_ok;
}

static int compose_ok(SnxFx* fx) {
    if (!fx->comp_checked) {
        VkFormatProperties p;
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        vkGetPhysicalDeviceFormatProperties(fx->phys, snx_fx_compose_format(), &p);
        fx->comp_ok = (p.optimalTilingFeatures & need) == need;
        fx->comp_checked = 1;
    }
    return fx->comp_ok;
}

/* ---- barriers and draws ---- */

static void fx_barrier(SnxFx* fx, VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout old_layout,
                       VkImageLayout new_layout, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                       VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = src_stage,
                               .srcAccessMask = src_access,
                               .dstStageMask = dst_stage,
                               .dstAccessMask = dst_access,
                               .oldLayout = old_layout,
                               .newLayout = new_layout,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image,
                               .subresourceRange = {aspect, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                          .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    (void)fx;
    vkCmdPipelineBarrier2(cb, &d);
}

static void bind_combined(SnxFx* fx, VkDescriptorSet set, VkImageView view, uint32_t binding) {
    VkDescriptorImageInfo di = {fx->sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = set,
                               .dstBinding = binding,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &di};
    vkUpdateDescriptorSets(fx->dev, 1, &wr, 0, NULL);
}

static void fx_draw_scissor(SnxFx* fx, VkCommandBuffer cb, VkImageView view, VkPipeline pipe, VkPipelineLayout layout,
                            uint32_t w, uint32_t h, VkRect2D sc, VkDescriptorSet set, const void* pc, uint32_t pc_size,
                            VkAttachmentLoadOp load) {
    VkRenderingAttachmentInfo at = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = load,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkClearValue clear = {{{0, 0, 0, 1}}};
    at.clearValue = clear;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, {w, h}},
                          .layerCount = 1,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = &at};
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    if (pc && pc_size) vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0,
                                          pc_size, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    if (fx->forget) fx->forget();
}

static void fx_draw(SnxFx* fx, VkCommandBuffer cb, VkImageView view, VkPipeline pipe, VkPipelineLayout layout, uint32_t w,
                    uint32_t h, VkDescriptorSet set, const void* pc, uint32_t pc_size, VkAttachmentLoadOp load) {
    VkRect2D full = {{0, 0}, {w, h}};
    fx_draw_scissor(fx, cb, view, pipe, layout, w, h, full, set, pc, pc_size, load);
}

/* ---- pipelines ---- */

static VkShaderModule module(SnxFx* fx, const uint32_t* words, uint64_t bytes) {
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)bytes, .pCode = words};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!fx_ok(vkCreateShaderModule(fx->dev, &mi, NULL, &m))) return VK_NULL_HANDLE;
    return m;
}

static VkPipeline make_gfx(SnxFx* fx, const uint32_t* vs, uint64_t vs_bytes, const uint32_t* fs, uint64_t fs_bytes,
                           VkPipelineLayout layout, VkFormat fmt) {
    VkShaderModule v = module(fx, vs, vs_bytes), f = module(fx, fs, fs_bytes);
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
    if (!v || !f || !fx_ok(vkCreateGraphicsPipelines(fx->dev, fx->cache, 1, &ci, NULL, &pipe)))
        pipe = VK_NULL_HANDLE;
    if (v) vkDestroyShaderModule(fx->dev, v, NULL);
    if (f) vkDestroyShaderModule(fx->dev, f, NULL);
    return pipe;
}

static int make_layouts(SnxFx* fx) {
    VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, SNX_FX_PUSH};
    VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                          NULL},
                                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                          NULL},
                                         {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                          NULL}};
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pc};
    VkDescriptorSetLayout* layouts[4] = {&fx->dsl0, &fx->dsl1, &fx->dsl2, &fx->dsl3};
    VkPipelineLayout* pipes[4] = {&fx->pl0, &fx->pl1, &fx->pl2, &fx->pl3};
    for (uint32_t i = 0; i < 4; ++i) {
        if (i) {
            dl.bindingCount = i;
            dl.pBindings = b;
        }
        if (!fx_ok(vkCreateDescriptorSetLayout(fx->dev, &dl, NULL, layouts[i]))) return 0;
        pl.pSetLayouts = layouts[i];
        if (!fx_ok(vkCreatePipelineLayout(fx->dev, &pl, NULL, pipes[i]))) return 0;
    }
    return 1;
}

static int make_pipelines(SnxFx* fx) {
    VkFormat fmt = choose_format(fx);
    if (fx->pipes_ready) return 1;
    if (fx->pipes_failed) return 0;
    fx->copy = make_gfx(fx, fx_fsr_vert_spv, fx_fsr_vert_spv_size, fx_copy_spv, fx_copy_spv_size, fx->pl1, fmt);
    fx->copy_linear =
        make_gfx(fx, fx_fsr_vert_spv, fx_fsr_vert_spv_size, fx_copy_linear_spv, fx_copy_linear_spv_size, fx->pl1, fmt);
    fx->easu = make_gfx(fx, fx_fsr_vert_spv, fx_fsr_vert_spv_size, fx_fsr_easu_spv, fx_fsr_easu_spv_size, fx->pl1, fmt);
    fx->rcas = make_gfx(fx, fx_fsr_vert_spv, fx_fsr_vert_spv_size, fx_fsr_rcas_spv, fx_fsr_rcas_spv_size, fx->pl1, fmt);
    if (fx->fp16) {
        fx->easu16 = make_gfx(fx, fx_fsr_vert_spv, fx_fsr_vert_spv_size, fx_fsr_easu_fp16_spv,
                              fx_fsr_easu_fp16_spv_size, fx->pl1, fmt);
        fx->rcas16 = make_gfx(fx, fx_fsr_vert_spv, fx_fsr_vert_spv_size, fx_fsr_rcas_fp16_spv,
                              fx_fsr_rcas_fp16_spv_size, fx->pl1, fmt);
    }
    fx->fxaa = make_gfx(fx, fx_fsr_vert_spv, fx_fsr_vert_spv_size, fx_fxaa_spv, fx_fxaa_spv_size, fx->pl1, fmt);
    fx->smaa_edge =
        make_gfx(fx, fx_fsr_vert_spv, fx_fsr_vert_spv_size, fx_smaa_edge_spv, fx_smaa_edge_spv_size, fx->pl1,
                 VK_FORMAT_R8G8_UNORM);
    fx->smaa_weights = make_gfx(fx, fx_fsr_vert_spv, fx_fsr_vert_spv_size, fx_smaa_weights_spv, fx_smaa_weights_spv_size,
                                fx->pl3, VK_FORMAT_R8G8B8A8_UNORM);
    fx->smaa_blend = make_gfx(fx, fx_fsr_vert_spv, fx_fsr_vert_spv_size, fx_smaa_blend_spv, fx_smaa_blend_spv_size,
                              fx->pl2, fmt);
    if (!fx->copy || !fx->copy_linear || !fx->easu || !fx->rcas || !fx->fxaa || !fx->smaa_edge ||
        !fx->smaa_weights || !fx->smaa_blend) {
        fx->pipes_failed = 1;
        fx_echo(fx, "vulkan: the presentation effects could not be made, the frame is blitted");
        return 0;
    }
    fx->pipes_ready = 1;
    return 1;
}

static int make_hdr_pipelines(SnxFx* fx) {
    VkFormat fmt = snx_fx_compose_format();
    if (fx->compose) return 1;
    if (!compose_ok(fx)) return 0;
    fx->compose =
        make_gfx(fx, fx_hdr_vert_spv, fx_hdr_vert_spv_size, fx_hdr_compose_spv, fx_hdr_compose_spv_size, fx->pl1, fmt);
    fx->test = make_gfx(fx, fx_hdr_vert_spv, fx_hdr_vert_spv_size, fx_hdr_test_spv, fx_hdr_test_spv_size, fx->pl0, fmt);
    return fx->compose && fx->test;
}

/* ---- create and destroy ---- */

SnxFx* snx_fx_create(VkPhysicalDevice phys, VkDevice dev, VkQueue queue, uint32_t family, VkPipelineCache cache,
                     uint32_t slots, uint32_t (*slot)(void), void (*forget)(void), void (*log)(const char*)) {
    SnxFx* fx = calloc(1, sizeof *fx);
    if (!fx) return NULL;
    fx->phys = phys;
    fx->dev = dev;
    fx->queue = queue;
    fx->family = family;
    fx->cache = cache;
    fx->slots = slots < 1 ? 1 : slots > SNX_FX_SLOTS_MAX ? SNX_FX_SLOTS_MAX : slots;
    fx->slot = slot;
    fx->forget = forget;
    fx->log = log;
    fx->headroom_pct = 100;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 128,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &size};
    if (!fx_ok(vkCreateSampler(fx->dev, &si, NULL, &fx->sampler)) || !make_layouts(fx) ||
        !fx_ok(vkCreateDescriptorPool(fx->dev, &pci, NULL, &fx->pool)))
        goto failed;
    for (uint32_t i = 0; i < fx->slots; ++i) {
        VkDescriptorSetLayout one = fx->dsl1;
        VkDescriptorSetLayout all[1] = {one};
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = fx->pool,
                                          .descriptorSetCount = 1,
                                          .pSetLayouts = all};
        VkDescriptorSet* sets[] = {&fx->fsr_easu[i], &fx->fsr_rcas[i], &fx->copy_set[i], &fx->fxaa_set[i],
                                   &fx->edge_set[i], &fx->compose_set[i], &fx->encode_set[i]};
        for (uint32_t s = 0; s < sizeof sets / sizeof *sets; ++s) {
            ai.pSetLayouts = &one;
            if (!fx_ok(vkAllocateDescriptorSets(fx->dev, &ai, sets[s]))) goto failed;
        }
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &fx->dsl3;
        if (!fx_ok(vkAllocateDescriptorSets(fx->dev, &ai, &fx->weight_set[i]))) goto failed;
        ai.pSetLayouts = &fx->dsl2;
        if (!fx_ok(vkAllocateDescriptorSets(fx->dev, &ai, &fx->blend_set[i]))) goto failed;
        ai.pSetLayouts = &fx->dsl2;
        if (!fx_ok(vkAllocateDescriptorSets(fx->dev, &ai, &fx->mix_set[i]))) goto failed;
    }
    return fx;
failed:
    snx_fx_destroy(fx);
    return NULL;
}

void snx_fx_destroy(SnxFx* fx) {
    if (!fx) return;
    vkDeviceWaitIdle(fx->dev);
    for (uint32_t i = 0; i < SNX_FX_SLOTS_MAX; ++i) {
        img_free(fx, &fx->main[i]);
        img_free(fx, &fx->mid[i]);
        img_free(fx, &fx->aa[i]);
        img_free(fx, &fx->edge[i]);
        img_free(fx, &fx->weight[i]);
        img_free(fx, &fx->comp[i]);
        img_free(fx, &fx->keep[i]);
        img_free(fx, &fx->blend_img[i]);
    }
    if (fx->area_view) vkDestroyImageView(fx->dev, fx->area_view, NULL);
    if (fx->search_view) vkDestroyImageView(fx->dev, fx->search_view, NULL);
    if (fx->area) vkDestroyImage(fx->dev, fx->area, NULL);
    if (fx->search) vkDestroyImage(fx->dev, fx->search, NULL);
    if (fx->area_mem) vkFreeMemory(fx->dev, fx->area_mem, NULL);
    if (fx->search_mem) vkFreeMemory(fx->dev, fx->search_mem, NULL);
    if (fx->tex_buf) vkDestroyBuffer(fx->dev, fx->tex_buf, NULL);
    if (fx->tex_mem) vkFreeMemory(fx->dev, fx->tex_mem, NULL);
#define DESTROY_PIPE(p)                                                                                                \
    if (fx->p) vkDestroyPipeline(fx->dev, fx->p, NULL)
    DESTROY_PIPE(easu);
    DESTROY_PIPE(rcas);
    DESTROY_PIPE(easu16);
    DESTROY_PIPE(rcas16);
    DESTROY_PIPE(copy);
    DESTROY_PIPE(copy_linear);
    DESTROY_PIPE(blend);
    DESTROY_PIPE(fxaa);
    DESTROY_PIPE(smaa_edge);
    DESTROY_PIPE(smaa_weights);
    DESTROY_PIPE(smaa_blend);
    DESTROY_PIPE(compose);
    DESTROY_PIPE(test);
#undef DESTROY_PIPE
    for (int i = 0; i < 4; ++i)
        if (fx->encode[i]) vkDestroyPipeline(fx->dev, fx->encode[i], NULL);
    if (fx->pl0) vkDestroyPipelineLayout(fx->dev, fx->pl0, NULL);
    if (fx->pl1) vkDestroyPipelineLayout(fx->dev, fx->pl1, NULL);
    if (fx->pl2) vkDestroyPipelineLayout(fx->dev, fx->pl2, NULL);
    if (fx->pl3) vkDestroyPipelineLayout(fx->dev, fx->pl3, NULL);
    if (fx->dsl0) vkDestroyDescriptorSetLayout(fx->dev, fx->dsl0, NULL);
    if (fx->dsl1) vkDestroyDescriptorSetLayout(fx->dev, fx->dsl1, NULL);
    if (fx->dsl2) vkDestroyDescriptorSetLayout(fx->dev, fx->dsl2, NULL);
    if (fx->dsl3) vkDestroyDescriptorSetLayout(fx->dev, fx->dsl3, NULL);
    if (fx->pool) vkDestroyDescriptorPool(fx->dev, fx->pool, NULL);
    if (fx->sampler) vkDestroySampler(fx->dev, fx->sampler, NULL);
    free(fx);
}

void snx_fx_configure(SnxFx* fx, const SnxFxConfig* cfg) {
    if (!fx || !cfg) return;
    fx->cfg = *cfg;
    fx->cfg.paper_nits = snx_clamp_int(cfg->paper_nits, 80, 480);
    fx->cfg.peak_nits = snx_clamp_int(cfg->peak_nits, 400, 4000);
    fx->cfg.expand = snx_clamp_int(cfg->expand, 0, 100);
    fx->cfg.sharpness = snx_clamp_int(cfg->sharpness, 0, 100);
    fx->cfg.test = cfg->test != 0;
}

void snx_fx_set_display(SnxFx* fx, int kind, int headroom_pct) {
    if (!fx) return;
    fx->display_kind = kind;
    fx->headroom_pct = headroom_pct < 100 ? 100 : headroom_pct > 1000 ? 1000 : headroom_pct;
}

void snx_fx_set_aa(SnxFx* fx, int aa) {
    if (fx) fx->cfg.aa = aa;
}

void snx_fx_set_fp16(SnxFx* fx, int on) {
    if (fx) fx->fp16 = on != 0;
}

int snx_fx_upscale_cycle(SnxFx* fx) {
    static const int order[] = {SNX_FX_UPSCALE_FSR1, SNX_FX_UPSCALE_BILINEAR, SNX_FX_UPSCALE_NIS,
                                SNX_FX_UPSCALE_SGSR, SNX_FX_UPSCALE_OFF};
    int i;
    if (!fx) return 0;
    for (i = 0; i < (int)(sizeof order / sizeof *order) && order[i] != fx->cfg.upscaler; ++i) {}
    fx->cfg.upscaler = i == (int)(sizeof order / sizeof *order) ? order[0]
                                                                : order[(i + 1) % (int)(sizeof order / sizeof *order)];
    return fx->cfg.upscaler;
}

int snx_fx_aa_cycle(SnxFx* fx) {
    static const int order[] = {SNX_FX_AA_AUTO, SNX_FX_AA_FXAA, SNX_FX_AA_SMAA, SNX_FX_AA_SSAA2,
                                SNX_FX_AA_SSAA4, SNX_FX_AA_OFF};
    int i;
    if (!fx) return 0;
    for (i = 0; i < (int)(sizeof order / sizeof *order) && order[i] != fx->cfg.aa; ++i) {}
    fx->cfg.aa = i == (int)(sizeof order / sizeof *order) ? order[0] : order[(i + 1) % (int)(sizeof order / sizeof *order)];
    return fx->cfg.aa;
}

int snx_fx_hdr_toggle(SnxFx* fx) {
    if (!fx) return 0;
    fx->cfg.hdr = fx->cfg.hdr == SNX_FX_HDR_OFF ? SNX_FX_HDR_ON : SNX_FX_HDR_OFF;
    return fx->cfg.hdr;
}

int snx_fx_hdr_mode(const SnxFx* fx) { return fx ? fx->cfg.hdr : SNX_FX_HDR_OFF; }
int snx_fx_upscaler(const SnxFx* fx) { return fx ? fx->cfg.upscaler : SNX_FX_UPSCALE_FSR1; }
int snx_fx_aa(const SnxFx* fx) { return fx ? fx->cfg.aa : SNX_FX_AA_AUTO; }

/* ---- a copy, for the filters that are not a filter ---- */

static void record_copy(SnxFx* fx, VkCommandBuffer cb, VkDescriptorSet set, VkImageView src, uint32_t image_w,
                        uint32_t image_h, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t transform,
                        uint32_t dw, uint32_t dh, int linear, FxImage* out) {
    struct {
        float rect[4];
        float crop[4];
        float size[4];
    } pc = {{0, 0, (float)dw, (float)dh},
            {(float)x, (float)y, (float)w, (float)h},
            {(float)image_w, (float)image_h, (transform & 1) ? 1.0f : 0.0f, (transform & 2) ? 1.0f : 0.0f}};
    bind_combined(fx, set, src, 0);
    fx_barrier(fx, cb, out->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    fx_draw(fx, cb, out->view, linear ? fx->copy_linear : fx->copy, fx->pl1, dw, dh, set, &pc, sizeof pc,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    fx_barrier(fx, cb, out->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT);
}

/* ---- FSR 1 ---- */

static void record_fsr(SnxFx* fx, VkCommandBuffer cb, uint32_t slot, VkImageView src, uint32_t image_w, uint32_t image_h,
                       uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t transform, uint32_t dw, uint32_t dh) {
    FxImage* f = &fx->main[slot];
    struct {
        uint32_t con[4][4];
        float flip[4];
        float crop[4];
    } easu, rcas;
    uint32_t rcas_con[4];
    float sharp = (float)fx->cfg.sharpness / 100.0f;
    VkPipeline easu_pipe = fx->fp16 && fx->easu16 ? fx->easu16 : fx->easu;
    VkPipeline rcas_pipe = fx->fp16 && fx->rcas16 ? fx->rcas16 : fx->rcas;
    snx_fx_fsr_easu_con(easu.con, (float)w, (float)h, (float)image_w, (float)image_h, (float)dw, (float)dh, (float)x,
                        (float)y);
    snx_fx_fsr_rcas_con(rcas_con, sharp);
    memset(&easu.flip, 0, sizeof easu.flip);
    easu.crop[0] = (float)x / (float)image_w;
    easu.crop[1] = (float)y / (float)image_h;
    easu.crop[2] = (float)(x + w) / (float)image_w;
    easu.crop[3] = (float)(y + h) / (float)image_h;
    easu.flip[0] = (transform & 1) ? 1.0f : 0.0f;
    easu.flip[1] = (transform & 2) ? 1.0f : 0.0f;
    rcas = easu;
    memcpy(rcas.con[0], rcas_con, sizeof rcas_con);
    rcas.flip[0] = rcas.flip[1] = 0.0f;
    rcas.crop[0] = rcas.crop[1] = 0.0f;
    rcas.crop[2] = rcas.crop[3] = 1.0f;
    bind_combined(fx, fx->fsr_easu[slot], src, 0);
    bind_combined(fx, fx->fsr_rcas[slot], fx->mid[slot].view, 0);
    fx_barrier(fx, cb, fx->mid[slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    fx_draw(fx, cb, fx->mid[slot].view, easu_pipe, fx->pl1, dw, dh, fx->fsr_easu[slot], &easu, sizeof easu,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    fx_barrier(fx, cb, fx->mid[slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    fx_barrier(fx, cb, f->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    fx_draw(fx, cb, f->view, rcas_pipe, fx->pl1, dw, dh, fx->fsr_rcas[slot], &rcas, sizeof rcas,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    fx_barrier(fx, cb, f->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT);
}

/* ---- anti aliasing ---- */

static void record_fxaa(SnxFx* fx, VkCommandBuffer cb, uint32_t slot, FxImage* src, FxImage* dst) {
    float rcp[4] = {1.0f / (float)src->width, 1.0f / (float)src->height, 0, 0};
    bind_combined(fx, fx->fxaa_set[slot], src->view, 0);
    fx_barrier(fx, cb, dst->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    fx_draw(fx, cb, dst->view, fx->fxaa, fx->pl1, dst->width, dst->height, fx->fxaa_set[slot], rcp, sizeof rcp,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    fx_barrier(fx, cb, dst->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT);
}

/* the SMAA lookup textures (MIT, Jorge Jimenez and the SMAA contributors): AreaTex is 160x560 RG8 and SearchTex is
   64x16 R8, as the SMAA distribution packs them */
static int smaa_textures(SnxFx* fx, VkCommandBuffer cb) {
    FxImage ai = {0}, si = {0};
    VkDeviceSize total = (VkDeviceSize)AREATEX_SIZE + SEARCHTEX_SIZE;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = total,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBufferImageCopy copies[2] = {
        {.bufferOffset = 0,
         .bufferRowLength = AREATEX_WIDTH,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {AREATEX_WIDTH, AREATEX_HEIGHT, 1}},
        {.bufferOffset = AREATEX_SIZE,
         .bufferRowLength = SEARCHTEX_WIDTH,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1}}};
    if (fx->tex_uploaded) return 1;
    if (!img_make(fx, &ai, AREATEX_WIDTH, AREATEX_HEIGHT, VK_FORMAT_R8G8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !img_make(fx, &si, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, VK_FORMAT_R8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        goto failed;
    if (!fx_ok(vkCreateBuffer(fx->dev, &bi, NULL, &fx->tex_buf))) goto failed;
    {
        VkMemoryRequirements req;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetBufferMemoryRequirements(fx->dev, fx->tex_buf, &req);
        vkGetPhysicalDeviceMemoryProperties(fx->phys, &mp);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
        mai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX ||
            !fx_ok(vkAllocateMemory(fx->dev, &mai, NULL, &fx->tex_mem)) ||
            !fx_ok(vkBindBufferMemory(fx->dev, fx->tex_buf, fx->tex_mem, 0)))
            goto failed;
    }
    {
        void* p = NULL;
        if (!fx_ok(vkMapMemory(fx->dev, fx->tex_mem, 0, VK_WHOLE_SIZE, 0, &p)) || !p) goto failed;
        memcpy(p, areaTexBytes, AREATEX_SIZE);
        memcpy((unsigned char*)p + AREATEX_SIZE, searchTexBytes, SEARCHTEX_SIZE);
        vkUnmapMemory(fx->dev, fx->tex_mem);
    }
    fx->area = ai.image;
    fx->area_mem = ai.memory;
    fx->area_view = ai.view;
    fx->search = si.image;
    fx->search_mem = si.memory;
    fx->search_view = si.view;
    fx_barrier(fx, cb, fx->area, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT);
    fx_barrier(fx, cb, fx->search, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cb, fx->tex_buf, fx->area, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[0]);
    vkCmdCopyBufferToImage(cb, fx->tex_buf, fx->search, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[1]);
    fx_barrier(fx, cb, fx->area, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    fx_barrier(fx, cb, fx->search, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    fx->tex_uploaded = 1;
    return 1;
failed:
    img_free(fx, &ai);
    img_free(fx, &si);
    return 0;
}

int snx_fx_pick_aa(SnxFx* fx, int aa, uint32_t render_w, uint32_t render_h, uint32_t shown_w, uint32_t shown_h,
                   uint32_t max_dim) {
    if (aa == SNX_FX_AA_AUTO) return snx_fx_aa_auto(render_w, render_h, shown_w, shown_h) ? SNX_FX_AA_FXAA : SNX_FX_AA_OFF;
    if (aa == SNX_FX_AA_SSAA2) return (uint64_t)shown_w * 2 <= max_dim && (uint64_t)shown_h * 2 <= max_dim ? aa : SNX_FX_AA_FXAA;
    if (aa == SNX_FX_AA_SSAA4) return (uint64_t)shown_w * 4 <= max_dim && (uint64_t)shown_h * 4 <= max_dim ? aa : SNX_FX_AA_SSAA2;
    if (aa == SNX_FX_AA_FXAA || aa == SNX_FX_AA_SMAA) return format_ok(fx) ? aa : SNX_FX_AA_OFF;
    return SNX_FX_AA_OFF;
}

int snx_fx_filter_available(SnxFx* fx, int filter) {
    if (filter == SNX_FX_UPSCALE_OFF || filter == SNX_FX_UPSCALE_BILINEAR) return 1;
    if (filter == SNX_FX_UPSCALE_NIS || filter == SNX_FX_UPSCALE_SGSR) return 0;
    return filter == SNX_FX_UPSCALE_FSR1 && format_ok(fx);
}

int snx_fx_pick_filter(SnxFx* fx, uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h, uint32_t transform) {
    int f = fx->cfg.upscaler;
    (void)transform;
    if (!src_w || !src_h || !dst_w || !dst_h) return -1;
    if (f == SNX_FX_UPSCALE_OFF || f == SNX_FX_UPSCALE_BILINEAR) return f;
    if (!snx_fx_filter_available(fx, f)) f = SNX_FX_UPSCALE_FSR1;
    if (!snx_fx_filter_available(fx, f)) return SNX_FX_UPSCALE_BILINEAR;
    return f;
}

/* ---- the public render ---- */

int snx_fx_render(SnxFx* fx, VkCommandBuffer cb, VkImageView src, uint32_t img_w, uint32_t img_h, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h, uint32_t transform, uint32_t out_w, uint32_t out_h, int filter, int aa,
                  SnxFxImage* out) {
    uint32_t slot;
    uint32_t work_w = out_w, work_h = out_h;
    int ssaa = 0;
    if (!fx || !cb || !src || !img_w || !img_h || !w || !h || !out_w || !out_h || !out) return 0;
    slot = fx->slot ? fx->slot() % fx->slots : 0;
    aa = snx_fx_pick_aa(fx, aa, w, h, out_w, out_h, 16384);
    if (filter != SNX_FX_UPSCALE_FSR1 && aa == SNX_FX_AA_OFF) return 0;
    if (!make_pipelines(fx)) return 0;
    if (aa == SNX_FX_AA_SSAA2 || aa == SNX_FX_AA_SSAA4) {
        uint32_t mult = aa == SNX_FX_AA_SSAA4 ? 4 : 2;
        ssaa = (int)mult;
        work_w = out_w * mult;
        work_h = out_h * mult;
    }
    if (!img_make(fx, &fx->main[slot], work_w, work_h, choose_format(fx), fx_usage())) return 0;
    if (filter == SNX_FX_UPSCALE_FSR1) {
        if (!img_make(fx, &fx->mid[slot], work_w, work_h, choose_format(fx), fx_usage())) return 0;
        record_fsr(fx, cb, slot, src, img_w, img_h, x, y, w, h, transform, work_w, work_h);
    } else {
        record_copy(fx, cb, fx->copy_set[slot], src, img_w, img_h, x, y, w, h, transform, work_w, work_h, 1,
                    &fx->main[slot]);
    }
    if (aa != SNX_FX_AA_OFF)
        fx_barrier(fx, cb, fx->main[slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    if (ssaa) {
        if (!img_make(fx, &fx->aa[slot], out_w, out_h, choose_format(fx), fx_usage())) return 0;
        record_copy(fx, cb, fx->copy_set[slot], fx->main[slot].view, fx->main[slot].width, fx->main[slot].height, 0, 0,
                    fx->main[slot].width, fx->main[slot].height, 0, out_w, out_h, 1, &fx->aa[slot]);
        out->image = fx->aa[slot].image;
        out->view = fx->aa[slot].view;
    } else if (aa == SNX_FX_AA_FXAA) {
        if (!img_make(fx, &fx->aa[slot], out_w, out_h, choose_format(fx), fx_usage())) return 0;
        record_fxaa(fx, cb, slot, &fx->main[slot], &fx->aa[slot]);
        out->image = fx->aa[slot].image;
        out->view = fx->aa[slot].view;
    } else if (aa == SNX_FX_AA_SMAA) {
        if (!img_make(fx, &fx->edge[slot], out_w, out_h, VK_FORMAT_R8G8_UNORM,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !img_make(fx, &fx->weight[slot], out_w, out_h, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !img_make(fx, &fx->aa[slot], out_w, out_h, choose_format(fx), fx_usage()))
            return 0;
        {
            float inv[4] = {1.0f / (float)out_w, 1.0f / (float)out_h, 0, 0};
            bind_combined(fx, fx->edge_set[slot], fx->main[slot].view, 0);
            fx_barrier(fx, cb, fx->edge[slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            fx_draw(fx, cb, fx->edge[slot].view, fx->smaa_edge, fx->pl1, out_w, out_h, fx->edge_set[slot], inv,
                    sizeof inv, VK_ATTACHMENT_LOAD_OP_CLEAR);
            fx_barrier(fx, cb, fx->edge[slot].image, VK_IMAGE_ASPECT_COLOR_BIT,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        /* the weights need the area and search textures; they are made once, on the first SMAA frame */
        if (!smaa_textures(fx, cb)) return 0;
        {
            float inv[4] = {1.0f / (float)out_w, 1.0f / (float)out_h, 0, 0};
            bind_combined(fx, fx->weight_set[slot], fx->edge[slot].view, 0);
            bind_combined(fx, fx->weight_set[slot], fx->area_view, 1);
            bind_combined(fx, fx->weight_set[slot], fx->search_view, 2);
            fx_barrier(fx, cb, fx->weight[slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            fx_draw(fx, cb, fx->weight[slot].view, fx->smaa_weights, fx->pl3, out_w, out_h, fx->weight_set[slot], inv,
                    sizeof inv, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
            fx_barrier(fx, cb, fx->weight[slot].image, VK_IMAGE_ASPECT_COLOR_BIT,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        {
            float inv[4] = {1.0f / (float)out_w, 1.0f / (float)out_h, 0, 0};
            bind_combined(fx, fx->blend_set[slot], fx->main[slot].view, 0);
            bind_combined(fx, fx->blend_set[slot], fx->weight[slot].view, 1);
            fx_barrier(fx, cb, fx->aa[slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            fx_draw(fx, cb, fx->aa[slot].view, fx->smaa_blend, fx->pl2, out_w, out_h, fx->blend_set[slot], inv,
                    sizeof inv, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
            fx_barrier(fx, cb, fx->aa[slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        out->image = fx->aa[slot].image;
        out->view = fx->aa[slot].view;
    } else {
        out->image = fx->main[slot].image;
        out->view = fx->main[slot].view;
    }
    out->width = out_w;
    out->height = out_h;
    if (!fx->aa_logged) {
        fx->aa_logged = 1;
        fx_echo(fx, "vulkan: presentation effects ready");
    }
    return 1;
}

/* ---- the frame between two frames ---- */

int snx_fx_blend(SnxFx* fx, VkCommandBuffer cb, VkImageView previous, VkImageView current, uint32_t width,
                 uint32_t height, float step, SnxFxImage* out) {
    uint32_t slot;
    float mixv[4] = {step, 0, 0, 0};
    if (!fx || !cb || !previous || !current || !width || !height || !out) return 0;
    if (!make_pipelines(fx)) return 0;
    slot = fx->slot ? fx->slot() % fx->slots : 0;
    if (!fx->blend || fx->blend_fmt != choose_format(fx)) {
        VkPipeline p = make_gfx(fx, fx_fsr_vert_spv, fx_fsr_vert_spv_size, fx_blend_spv, fx_blend_spv_size, fx->pl2,
                                choose_format(fx));
        if (!p) return 0;
        if (fx->blend) {
            vkDeviceWaitIdle(fx->dev);
            vkDestroyPipeline(fx->dev, fx->blend, NULL);
        }
        fx->blend = p;
        fx->blend_fmt = choose_format(fx);
    }
    if (!img_make(fx, &fx->blend_img[slot], width, height, choose_format(fx), fx_usage())) return 0;
    bind_combined(fx, fx->mix_set[slot], previous, 0);
    bind_combined(fx, fx->mix_set[slot], current, 1);
    fx_barrier(fx, cb, fx->blend_img[slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    fx_draw(fx, cb, fx->blend_img[slot].view, fx->blend, fx->pl2, width, height, fx->mix_set[slot], mixv, sizeof mixv,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    fx_barrier(fx, cb, fx->blend_img[slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT);
    out->image = fx->blend_img[slot].image;
    out->view = fx->blend_img[slot].view;
    out->width = width;
    out->height = height;
    return 1;
}

int snx_fx_keep(SnxFx* fx, VkCommandBuffer cb, VkImageView source, uint32_t width, uint32_t height, SnxFxImage* out) {
    uint32_t slot;
    if (!fx || !cb || !source || !width || !height || !out) return 0;
    if (!make_pipelines(fx)) return 0;
    slot = fx->slot ? fx->slot() % fx->slots : 0;
    if (!img_make(fx, &fx->keep[slot], width, height, choose_format(fx), fx_usage())) return 0;
    record_copy(fx, cb, fx->copy_set[slot], source, width, height, 0, 0, width, height, 0, width, height, 1,
                &fx->keep[slot]);
    out->image = fx->keep[slot].image;
    out->view = fx->keep[slot].view;
    out->width = width;
    out->height = height;
    return 1;
}

/* ---- HDR ---- */

int snx_fx_encode_kind(const SnxFx* fx) {
    if (fx->display_kind == SNX_FX_DISPLAY_P3) return SNX_FX_ENC_P3;
    if (fx->display_kind == SNX_FX_DISPLAY_SCRGB) return SNX_FX_ENC_SCRGB;
    if (fx->display_kind == SNX_FX_DISPLAY_PQ) return SNX_FX_ENC_PQ;
    return SNX_FX_ENC_SDR;
}

/* the light the top of an SDR frame reaches, as a multiple of the paper white: the display's reported headroom (an
   HDR10 target's own peak standing in for it), its share being the expansion (src/nx/gpu/vk/hdr.c's lift_at_white) */
static float lift_at_white(const SnxFx* fx) {
    float peak_rel = (float)fx->cfg.peak_nits / (float)fx->cfg.paper_nits;
    float max = fx->display_kind == SNX_FX_DISPLAY_PQ ? peak_rel : (float)fx->headroom_pct / 100.0f;
    if (max < 1.0f) max = 1.0f;
    if (max > peak_rel) max = peak_rel;
    return 1.0f + (float)fx->cfg.expand / 100.0f * (max - 1.0f);
}

int snx_fx_hdr_compose(SnxFx* fx, VkCommandBuffer cb, VkImageView src, VkFormat src_fmt, uint32_t img_w, uint32_t img_h,
                       uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t transform, uint32_t dst_w,
                       uint32_t dst_h, int32_t dx, int32_t dy, uint32_t dw, uint32_t dh, SnxFxImage* out) {
    uint32_t slot;
    struct {
        float rect[4], crop[4], size[4], tone[4], expand[4];
    } pc = {0};
    VkRect2D sc;
    if (!fx || !cb || !dst_w || !dst_h || !out || !compose_ok(fx) || !make_hdr_pipelines(fx)) return 0;
    if (src && (!img_w || !img_h || !w || !h || !dw || !dh)) return 0;
    slot = fx->slot ? fx->slot() % fx->slots : 0;
    if (!img_make(fx, &fx->comp[slot], dst_w, dst_h, snx_fx_compose_format(),
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return 0;
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
    pc.tone[0] = (float)fx->cfg.paper_nits;
    pc.tone[1] = lift_at_white(fx);
    pc.tone[2] = (float)fx->cfg.peak_nits;
    pc.tone[3] = (float)(src ? snx_fx_source_for(fx, src_fmt) : SNX_FX_SRC_SDR);
    pc.expand[0] = (float)fx->cfg.expand / 100.0f;
    if (src) bind_combined(fx, fx->compose_set[slot], src, 0);
    sc.offset.x = dx < 0 ? 0 : dx;
    sc.offset.y = dy < 0 ? 0 : dy;
    sc.extent.width = (uint32_t)((int64_t)dx + dw > (int64_t)dst_w ? (int64_t)dst_w - sc.offset.x : (int64_t)dw);
    sc.extent.height = (uint32_t)((int64_t)dy + dh > (int64_t)dst_h ? (int64_t)dst_h - sc.offset.y : (int64_t)dh);
    if ((int64_t)sc.offset.x + sc.extent.width > dst_w) sc.extent.width = dst_w - sc.offset.x;
    if ((int64_t)sc.offset.y + sc.extent.height > dst_h) sc.extent.height = dst_h - sc.offset.y;
    fx_barrier(fx, cb, fx->comp[slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    fx_draw_scissor(fx, cb, fx->comp[slot].view, fx->compose, fx->pl1, dst_w, dst_h, sc,
                    src ? fx->compose_set[slot] : VK_NULL_HANDLE, &pc, sizeof pc, VK_ATTACHMENT_LOAD_OP_CLEAR);
    out->image = fx->comp[slot].image;
    out->view = fx->comp[slot].view;
    out->width = dst_w;
    out->height = dst_h;
    return 1;
}

int snx_fx_hdr_encode(SnxFx* fx, VkCommandBuffer cb, const SnxFxImage* composed, VkImage dst, VkImageView dst_view,
                      VkFormat dst_fmt, uint32_t w, uint32_t h, int enc) {
    uint32_t slot;
    float tone[4];
    VkPipeline pipe;
    if (!fx || !cb || !composed || !dst || !dst_view || !w || !h || enc < 0 || enc > 3) return 0;
    if (!make_hdr_pipelines(fx)) return 0;
    slot = fx->slot ? fx->slot() % fx->slots : 0;
    if (!fx->encode[enc] || fx->encode_fmt[enc] != dst_fmt) {
        VkPipeline p = make_gfx(fx, fx_hdr_vert_spv, fx_hdr_vert_spv_size, fx_hdr_encode_spv, fx_hdr_encode_spv_size,
                                fx->pl1, dst_fmt);
        if (!p) return 0;
        if (fx->encode[enc]) {
            vkDeviceWaitIdle(fx->dev);
            vkDestroyPipeline(fx->dev, fx->encode[enc], NULL);
        }
        fx->encode[enc] = p;
        fx->encode_fmt[enc] = dst_fmt;
    }
    pipe = fx->encode[enc];
    tone[0] = (float)fx->cfg.paper_nits;
    tone[1] = (float)fx->cfg.peak_nits;
    tone[2] = (float)enc;
    tone[3] = dst_fmt == VK_FORMAT_B8G8R8A8_SRGB || dst_fmt == VK_FORMAT_R8G8B8A8_SRGB ? 1.0f : 0.0f;
    bind_combined(fx, fx->encode_set[slot], composed->view, 0);
    fx_barrier(fx, cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    fx_draw(fx, cb, dst_view, pipe, fx->pl1, w, h, fx->encode_set[slot], tone, sizeof tone,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    return 1;
}

int snx_fx_hdr_test(SnxFx* fx, VkCommandBuffer cb, const SnxFxImage* composed) {
    struct {
        float v[4];
    } pc;
    uint32_t band;
    VkRect2D sc;
    if (!fx || !cb || !composed || !make_hdr_pipelines(fx)) return 0;
    band = composed->height / 8 ? composed->height / 8 : 1;
    pc.v[0] = (float)fx->cfg.paper_nits;
    pc.v[1] = (float)fx->cfg.peak_nits;
    pc.v[2] = (float)composed->width;
    pc.v[3] = (float)band;
    sc = (VkRect2D){{0, (int32_t)(composed->height - band)}, {composed->width, band}};
    fx_barrier(fx, cb, composed->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    fx_draw_scissor(fx, cb, composed->view, fx->test, fx->pl0, composed->width, composed->height, sc, VK_NULL_HANDLE, &pc,
                    sizeof pc, VK_ATTACHMENT_LOAD_OP_LOAD);
    return 1;
}
