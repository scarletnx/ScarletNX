/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA runner's Vulkan window (agb/runtime/vk.h). The board's 240x160 RGBA frame goes to a device image through a
   host-visible staging buffer, then the shared presentation effects run: the spatial upscaler, the anti aliasing and
   the HDR composition and encode. A host without a Vulkan window keeps the SDL renderer of host.c. */
#include "agb/runtime/vk.h"
#include "agb/runtime/host.h"
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/stats.h"
#include "common/gpu/vk/text.h"
#include "common/gpu/vk/toast.h"
#include "common/gpu/vk/ring.h"
#include "common/gpu/vk/swapchain.h"
#include "common/gpu/vk/vk_common.h"
#include "common/gpu/vk/window.h"
#include "common/utilities/bits.h"
#include "common/utilities/window_icon.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGB_VK_SLOTS SNX_VK_RING_SLOTS

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    unsigned char* mapped;
} AgbFrame;

struct AgbVk {
    SnxVkWindow host;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceProperties properties;
    bool fp16;
    bool hdr_metadata;
    SnxVkSwapchain swap;
    VkImageView swap_views[SNX_VK_SWAP_IMAGES];
    VkSemaphore acquired[AGB_VK_SLOTS], rendered[SNX_VK_SWAP_IMAGES];
    SnxVkRing ring;
    AgbFrame frame[AGB_VK_SLOTS];
    SnxFx* fx;
    SnxToast* toast;
    SnxText* overlay; /* F6's frame rate and held keys */
    SnxStats stats;
    uint16_t keys; /* the keys the player holds, for the overlay */
    int display_kind;
    int said_kind;  /* the display kind last said in the console, -1 before the first */
    int auto_tried; /* Auto has looked for the display's headroom once, which SDL reports after the first frame */
    int headroom_pct;
    int lost;
};

static uint32_t g_agb_slot;
static uint32_t agb_slot(void) { return g_agb_slot; }
static void agb_fx_log(const char* text) { fprintf(stderr, "%s\n", text); }

static bool make_device(VkPhysicalDevice phys, uint32_t family, void* user) {
    AgbVk* v = user;
    SnxVkExtensions list;
    if (!snx_vk_device_extensions(phys, &list)) return false;
    const char* extensions[3];
    uint32_t n = 0;
    bool ok = snx_vk_extensions_has(&list, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    extensions[n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (snx_vk_extensions_has(&list, VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
        v->hdr_metadata = true;
        extensions[n++] = VK_EXT_HDR_METADATA_EXTENSION_NAME;
    }
    if (snx_vk_extensions_has(&list, "VK_KHR_portability_subset")) extensions[n++] = "VK_KHR_portability_subset";
    snx_vk_extensions_free(&list);
    if (!ok) return false;
    {
        VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        void* const query[] = {&features, &v12, &v13};
        snx_vk_chain(query, sizeof query / sizeof *query);
        vkGetPhysicalDeviceFeatures2(phys, &features);
        if (!v13.dynamicRendering || !v13.synchronization2) return false;
        v->fp16 = v12.shaderFloat16 != 0;
        VkPhysicalDeviceVulkan13Features want13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                                   .dynamicRendering = VK_TRUE,
                                                   .synchronization2 = VK_TRUE};
        VkPhysicalDeviceVulkan12Features want12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                   .shaderFloat16 = v12.shaderFloat16};
        VkPhysicalDeviceFeatures2 want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        want.features.samplerAnisotropy = features.features.samplerAnisotropy;
        void* const enable[] = {&want, &want12, &want13};
        snx_vk_chain(enable, sizeof enable / sizeof *enable);
        if (snx_vk_device_create(phys, family, &want, extensions, n, &v->device) != VK_SUCCESS) return false;
    }
    v->phys = phys;
    v->family = family;
    return true;
}

static void views_free(AgbVk* v) {
    for (uint32_t i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
        if (v->swap_views[i]) vkDestroyImageView(v->device, v->swap_views[i], NULL);
    memset(v->swap_views, 0, sizeof v->swap_views);
}

static int choose_display(AgbVk* v, VkFormat* format, VkColorSpaceKHR* space) {
    static const VkFormat hdr_formats[] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32};
    static const VkFormat scrgb_formats[] = {VK_FORMAT_R16G16B16A16_SFLOAT};
    VkSurfaceFormatKHR f = {0};
    int mode = v->fx ? snx_fx_hdr_mode(v->fx) : snx_fx_hdr_parse(g_settings.hdr);
    int cs = snx_fx_colorspace_parse(g_settings.hdr_colours);
    int kind = SNX_FX_DISPLAY_NONE;
    if (!v->host.surface || mode == SNX_FX_HDR_OFF) return SNX_FX_DISPLAY_NONE;
    if (cs == SNX_FX_CS_AUTO || cs == SNX_FX_CS_P3) {
        if (snx_vk_surface_format_cs(v->phys, v->host.surface, scrgb_formats, 1, VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT,
                                     &f))
            kind = SNX_FX_DISPLAY_P3;
        else if (cs == SNX_FX_CS_P3)
            return SNX_FX_DISPLAY_NONE;
    }
    if (kind == SNX_FX_DISPLAY_NONE && (cs == SNX_FX_CS_AUTO || cs == SNX_FX_CS_SCRGB)) {
        if (snx_vk_surface_format_cs(v->phys, v->host.surface, scrgb_formats, 1,
                                     VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, &f))
            kind = SNX_FX_DISPLAY_SCRGB;
        else if (cs == SNX_FX_CS_SCRGB)
            return SNX_FX_DISPLAY_NONE;
    }
    if (kind == SNX_FX_DISPLAY_NONE &&
        snx_vk_surface_format_cs(v->phys, v->host.surface, hdr_formats, 2, VK_COLOR_SPACE_HDR10_ST2084_EXT, &f))
        kind = SNX_FX_DISPLAY_PQ;
    if (kind == SNX_FX_DISPLAY_NONE) return SNX_FX_DISPLAY_NONE;
    /* an extended-kind image's 1.0 is the display's SDR white: Auto takes one only where the display reports headroom
       over its white, as the Switch frontend does */
    if (mode == SNX_FX_HDR_AUTO && kind != SNX_FX_DISPLAY_PQ && snx_vk_window_headroom_pct(v->host.window) <= 100)
        return SNX_FX_DISPLAY_NONE;
    *format = f.format;
    *space = f.colorSpace;
    return kind;
}

static bool swapchain_make(AgbVk* v) {
    static const VkFormat sdr_formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    VkFormat hdr_format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR hdr_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    int kind = choose_display(v, &hdr_format, &hdr_space);
    SnxVkSwapchainInfo info = {
        .surface = v->host.surface,
        .formats = kind != SNX_FX_DISPLAY_NONE ? &hdr_format : sdr_formats,
        .format_count = kind != SNX_FX_DISPLAY_NONE ? 1 : (uint32_t)(sizeof sdr_formats / sizeof *sdr_formats),
        .color_space = kind != SNX_FX_DISPLAY_NONE ? hdr_space : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .window = snx_vk_window_extent(&v->host, (VkExtent2D){960, 640})};
    v->display_kind = kind;
    {
        int reported = snx_vk_window_headroom_pct(v->host.window);
        v->headroom_pct =
            kind == SNX_FX_DISPLAY_PQ
                ? snx_clamp_int(g_settings.hdr_peak * 100 / g_settings.hdr_paper_white, 100, 1000)
                : (reported > 100 ? snx_clamp_int(reported, 100, 1000) : 100);
    }
    if (kind != v->said_kind) {
        v->said_kind = kind;
        if (kind != SNX_FX_DISPLAY_NONE)
            printf("agb: the window takes %s, %d%% headroom\n", snx_fx_display_label(kind), v->headroom_pct);
        else
            printf("agb: the window takes %s\n", snx_fx_display_label(kind));
    }
    if (v->fx) snx_fx_set_display(v->fx, kind, v->headroom_pct);
    vkDeviceWaitIdle(v->device);
    views_free(v);
    if (!snx_vk_swapchain_make(v->phys, v->device, &info, &v->swap)) return false;
    for (uint32_t i = 0; i < v->swap.count; ++i) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = v->swap.images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = v->swap.format.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(v->device, &vi, NULL, &v->swap_views[i]) != VK_SUCCESS) {
            views_free(v);
            return false;
        }
    }
    return true;
}

static bool frame_make(AgbVk* v, AgbFrame* f) {
    const VkDeviceSize size = 240u * 160u * 4u;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {240, 160, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    if (vkCreateImage(v->device, &ii, NULL, &f->image) != VK_SUCCESS ||
        vkCreateBuffer(v->device, &bi, NULL, &f->staging) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(v->phys, &mp);
    vkGetImageMemoryRequirements(v->device, f->image, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
    ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, 0);
    if (vkAllocateMemory(v->device, &ai, NULL, &f->memory) != VK_SUCCESS ||
        vkBindImageMemory(v->device, f->image, f->memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = f->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(v->device, &vi, NULL, &f->view) != VK_SUCCESS) return false;
    vkGetBufferMemoryRequirements(v->device, f->staging, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(v->device, &ai, NULL, &f->staging_memory) != VK_SUCCESS ||
        vkBindBufferMemory(v->device, f->staging, f->staging_memory, 0) != VK_SUCCESS ||
        vkMapMemory(v->device, f->staging_memory, 0, VK_WHOLE_SIZE, 0, (void**)&f->mapped) != VK_SUCCESS)
        return false;
    return true;
}

static void frame_free(AgbVk* v, AgbFrame* f) {
    if (f->mapped) vkUnmapMemory(v->device, f->staging_memory);
    if (f->view) vkDestroyImageView(v->device, f->view, NULL);
    if (f->image) vkDestroyImage(v->device, f->image, NULL);
    if (f->memory) vkFreeMemory(v->device, f->memory, NULL);
    if (f->staging) vkDestroyBuffer(v->device, f->staging, NULL);
    if (f->staging_memory) vkFreeMemory(v->device, f->staging_memory, NULL);
    *f = (AgbFrame){0};
}

static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout old, VkImageLayout next,
                    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src, VkPipelineStageFlags2 dst_stage,
                    VkAccessFlags2 dst) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = src_stage,
                               .srcAccessMask = src,
                               .dstStageMask = dst_stage,
                               .dstAccessMask = dst,
                               .oldLayout = old,
                               .newLayout = next,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
    (void)old;
}

AgbVk* agb_vk_open(const char* title, bool fullscreen, char* error, size_t error_size) {
    AgbVk* v = calloc(1, sizeof *v);
    if (!v) return NULL;
    if (!snx_vk_window_open("scarletnx-agb", title, 960, 640, &v->host, error, error_size) || !v->host.window ||
        !v->host.surface) {
        agb_vk_close(v);
        return NULL;
    }
    if (fullscreen) SDL_SetWindowFullscreen(v->host.window, true);
    snx_window_icon(v->host.window);
    if (!snx_vk_first_device(v->host.instance.instance, VK_API_VERSION_1_3,
                             VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, v->host.surface, make_device, v)) {
        snprintf(error, error_size, "no Vulkan 1.3 device with dynamic rendering");
        agb_vk_close(v);
        return NULL;
    }
    volkLoadDevice(v->device);
    vkGetDeviceQueue(v->device, v->family, 0, &v->queue);
    vkGetPhysicalDeviceProperties(v->phys, &v->properties);
    {
        VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
        VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                         .maxSets = 32,
                                         .poolSizeCount = 1,
                                         .pPoolSizes = &size};
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!snx_vk_ring_init(&v->ring, v->device, v->queue, v->family, AGB_VK_SLOTS, &dp)) goto failed;
        for (unsigned i = 0; i < AGB_VK_SLOTS; ++i)
            if (vkCreateSemaphore(v->device, &si, NULL, &v->acquired[i]) != VK_SUCCESS) goto failed;
        for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
            if (vkCreateSemaphore(v->device, &si, NULL, &v->rendered[i]) != VK_SUCCESS) goto failed;
    }
    for (unsigned i = 0; i < AGB_VK_SLOTS; ++i)
        if (!frame_make(v, &v->frame[i])) goto failed;
    v->said_kind = -1;
    v->fx = snx_fx_create(v->phys, v->device, v->queue, v->family, VK_NULL_HANDLE, AGB_VK_SLOTS, agb_slot, NULL,
                          agb_fx_log);
    if (v->fx) {
        SnxFxConfig cfg = {snx_fx_upscale_parse(g_settings.upscaler),
                           g_settings.sharpness,
                           snx_fx_aa_parse(g_settings.anti_aliasing),
                           snx_fx_hdr_parse(g_settings.hdr),
                           snx_fx_colorspace_parse(g_settings.hdr_colours),
                           snx_fx_source_parse(g_settings.hdr_source),
                           g_settings.hdr_paper_white,
                           g_settings.hdr_peak,
                           g_settings.hdr_expansion,
                           g_settings.hdr_test_pattern};
        snx_fx_configure(v->fx, &cfg);
        v->toast = snx_toast_create(v->phys, v->device, v->queue, v->family, VK_NULL_HANDLE, AGB_VK_SLOTS);
        v->overlay = snx_text_create(v->phys, v->device, v->queue, v->family, VK_NULL_HANDLE, AGB_VK_SLOTS);
        snx_stats_init(&v->stats);
        v->stats.on = g_settings.stats != 0; /* the setting starts it, F6 turns it over for the run */
        snx_fx_set_fp16(v->fx, v->fp16);
    }
    if (!swapchain_make(v)) goto failed;
    return v;
failed:
    snprintf(error, error_size, "cannot make the Vulkan window's resources");
    agb_vk_close(v);
    return NULL;
}

SDL_Window* agb_vk_window(AgbVk* vk) { return vk ? vk->host.window : NULL; }

void agb_vk_close(AgbVk* v) {
    if (!v) return;
    if (v->device) {
        vkDeviceWaitIdle(v->device);
        if (v->toast) snx_toast_destroy(v->toast);
        v->toast = NULL;
        if (v->overlay) snx_text_destroy(v->overlay);
        v->overlay = NULL;
        if (v->fx) snx_fx_destroy(v->fx);
        v->fx = NULL;
        views_free(v);
        snx_vk_swapchain_free(v->device, &v->swap);
        for (unsigned i = 0; i < AGB_VK_SLOTS; ++i) frame_free(v, &v->frame[i]);
        for (unsigned i = 0; i < AGB_VK_SLOTS; ++i)
            if (v->acquired[i]) vkDestroySemaphore(v->device, v->acquired[i], NULL);
        for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
            if (v->rendered[i]) vkDestroySemaphore(v->device, v->rendered[i], NULL);
        snx_vk_ring_free(&v->ring);
        vkDestroyDevice(v->device, NULL);
    }
    snx_vk_window_close(&v->host);
    free(v);
}

int agb_vk_cycle_upscaler(AgbVk* v) { return v && v->fx ? snx_fx_upscale_cycle(v->fx) : -1; }
int agb_vk_cycle_aa(AgbVk* v) { return v && v->fx ? snx_fx_aa_cycle(v->fx) : -1; }
int agb_vk_toggle_hdr(AgbVk* v) { return v && v->fx ? snx_fx_hdr_toggle(v->fx) : -1; }
void agb_vk_toast(AgbVk* v, const char* text) {
    if (v && v->toast) snx_toast_show(v->toast, text, SDL_GetTicksNS());
}
void agb_vk_toast_ms(AgbVk* v, const char* text, unsigned ms) {
    if (v && v->toast) snx_toast_show_ms(v->toast, text, SDL_GetTicksNS(), ms);
}
void agb_vk_keys(AgbVk* v, uint16_t keys) {
    if (v) v->keys = keys;
}
int agb_vk_stats_toggle(AgbVk* v, int showcase) { return v ? snx_stats_toggle(&v->stats, showcase) : 0; }

/* the keys the player holds, in the overlay's words */
static const SnxStatsButton agb_buttons[] = {
    {AGB_KEY_A, "A"},           {AGB_KEY_B, "B"},  {AGB_KEY_L, "L"},    {AGB_KEY_R, "R"},    {AGB_KEY_START, "Start"},
    {AGB_KEY_SELECT, "Select"}, {AGB_KEY_UP, "^"}, {AGB_KEY_DOWN, "v"}, {AGB_KEY_LEFT, "<"}, {AGB_KEY_RIGHT, ">"}};
/* the frame rate and the held keys, at the window's bottom left */
static void stats_overlay(AgbVk* v, VkCommandBuffer cb, VkImageView view, VkFormat format, float white, uint32_t w,
                          uint32_t h, unsigned slot) {
    char inputs[80];
    if (!v->overlay || !snx_stats_visible(&v->stats)) return;
    if (v->stats.showcase)
        snx_stats_inputs(inputs, sizeof inputs, v->keys, agb_buttons,
                         (unsigned)(sizeof agb_buttons / sizeof *agb_buttons));
    else
        inputs[0] = 0;
    snx_stats_draw(v->overlay, &v->stats, cb, view, format, w, h, white, slot, SDL_GetTicksNS(), inputs);
}

bool agb_vk_frame(AgbVk* v, const uint32_t* framebuffer, int generated) {
    uint32_t slot, index = 0;
    AgbFrame* f;
    VkCommandBuffer cb;
    VkResult r;
    if (!v || !v->swap.swapchain) return false;
    /* Auto follows the display's headroom, which SDL reports only once the window is on screen: the first frame that
       sees it takes the HDR swapchain, and no more than once where the surface offers none */
    if (v->fx && snx_fx_hdr_mode(v->fx) == SNX_FX_HDR_AUTO && !v->auto_tried &&
        v->display_kind == SNX_FX_DISPLAY_NONE && snx_vk_window_headroom_pct(v->host.window) > 100) {
        v->auto_tried = 1;
        if (!swapchain_make(v)) return false;
    }
    if (snx_fx_hdr_active(v->fx) != (v->display_kind != SNX_FX_DISPLAY_NONE)) {
        if (!swapchain_make(v)) return false;
    }
    slot = v->ring.slot;
    g_agb_slot = slot % AGB_VK_SLOTS;
    f = &v->frame[g_agb_slot];
    memcpy(f->mapped, framebuffer, (size_t)240 * 160 * 4);
    r = vkAcquireNextImageKHR(v->device, v->swap.swapchain, 0, v->acquired[slot % AGB_VK_SLOTS], VK_NULL_HANDLE,
                              &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        if (swapchain_make(v)) return false;
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        static unsigned failures;
        if (failures++ == 0) fprintf(stderr, "agb vk: acquire %d\n", (int)r);
        return false;
    }
    cb = snx_vk_ring_commands(&v->ring);
    barrier(cb, f->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    {
        VkBufferImageCopy copy = {.bufferRowLength = 240,
                                  .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {240, 160, 1}};
        vkCmdCopyBufferToImage(cb, f->staging, f->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    barrier(cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    {
        uint32_t w = v->swap.extent.width, h = v->swap.extent.height;
        uint32_t sw = w, sh = (uint32_t)((uint64_t)w * 160 / 240);
        int32_t x, y;
        VkImage src_image = f->image;
        VkImageView src_view = f->view;
        uint32_t src_w = 240, src_h = 160;
        int have_fx = 0;
        SnxFxImage out = {0};
        if (sh > h) sh = h, sw = (uint32_t)((uint64_t)h * 240 / 160);
        x = (int32_t)(w - sw) / 2, y = (int32_t)(h - sh) / 2;
        if (v->fx) {
            int filter = snx_fx_pick_filter(v->fx, 240, 160, sw, sh, 0);
            int aa = snx_fx_pick_aa(v->fx, snx_fx_aa_parse(g_settings.anti_aliasing), 240, 160, sw, sh,
                                    v->properties.limits.maxImageDimension2D);
            have_fx = snx_fx_render(v->fx, cb, f->view, 240, 160, 0, 0, 240, 160, 0, sw, sh, filter, aa, &out);
            if (have_fx) {
                src_image = out.image;
                src_view = out.view;
                src_w = out.width;
                src_h = out.height;
                barrier(cb, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
        }
        if (v->fx && v->display_kind != SNX_FX_DISPLAY_NONE && snx_fx_hdr_active(v->fx)) {
            SnxFxImage comp = {0};
            if (snx_fx_hdr_compose(v->fx, cb, src_view, VK_FORMAT_R8G8B8A8_UNORM, src_w, src_h, 0, 0, src_w, src_h, 0,
                                   w, h, x, y, sw, sh, &comp)) {
                if (g_settings.hdr_test_pattern) snx_fx_hdr_test(v->fx, cb, &comp);
                if (v->toast)
                    snx_toast_draw(v->toast, cb, comp.view, snx_fx_compose_format(), w, h,
                                   (float)g_settings.hdr_paper_white, slot % AGB_VK_SLOTS, SDL_GetTicksNS());
                stats_overlay(v, cb, comp.view, snx_fx_compose_format(), (float)g_settings.hdr_paper_white, w, h,
                              slot % AGB_VK_SLOTS);
                if (snx_fx_hdr_encode(v->fx, cb, &comp, v->swap.images[index], v->swap_views[index],
                                      v->swap.format.format, w, h, snx_fx_encode_kind(v->fx))) {
                    barrier(cb, v->swap.images[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
                    goto present;
                }
            }
        }
        if (have_fx)
            barrier(cb, src_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(cb, v->swap.images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        {
            VkClearColorValue black = {{0, 0, 0, 1}};
            VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .srcOffsets = {{0, 0, 0}, {(int32_t)src_w, (int32_t)src_h, 1}},
                                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .dstOffsets = {{x, y, 0}, {x + (int32_t)sw, y + (int32_t)sh, 1}}};
            vkCmdClearColorImage(cb, v->swap.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
            barrier(cb, v->swap.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdBlitImage(cb, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, v->swap.images[index],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        }
        /* the toast and the overlay draw into it as an attachment, and the present waits for that layout */
        barrier(cb, v->swap.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
        if (v->toast)
            snx_toast_draw(v->toast, cb, v->swap_views[index], v->swap.format.format, w, h, 1.0f, slot % AGB_VK_SLOTS,
                           SDL_GetTicksNS());
        stats_overlay(v, cb, v->swap_views[index], v->swap.format.format, 1.0f, w, h, slot % AGB_VK_SLOTS);
        barrier(cb, v->swap.images[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE, 0);
    }
present:;
    if (generated)
        snx_stats_generated(&v->stats);
    else
        snx_stats_frame(&v->stats, SDL_GetTicksNS());
    snx_vk_ring_submit(&v->ring, v->acquired[slot % AGB_VK_SLOTS], v->rendered[index]);
    {
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                               .waitSemaphoreCount = 1,
                               .pWaitSemaphores = &v->rendered[index],
                               .swapchainCount = 1,
                               .pSwapchains = &v->swap.swapchain,
                               .pImageIndices = &index};
        r = vkQueuePresentKHR(v->queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) (void)swapchain_make(v);
    }
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}
