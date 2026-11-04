/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/gpu/vk.h"
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/stats.h"
#include "common/gpu/vk/text.h"
#include "common/gpu/vk/toast.h"
#include "common/utilities/bits.h"
#include "common/utilities/window_icon.h"
#include "snx_settings.h"
#include "vita/platform/input.h"
#include <stdio.h>
#include <string.h>

static struct {
    VitaVk vk;
    bool up;
    /* the presenter's copy of the framebuffer, which the swapchain image is blitted from */
    VkImage frame;
    VkDeviceMemory frame_memory;
    VkImageView frame_view;
    uint32_t frame_width, frame_height;
    VkImageLayout frame_layout;
    SnxFx* fx;
    SnxToast* toast;
    SnxText* overlay; /* F6's frame rate and held buttons */
    SnxStats stats;
    int display_kind;
    int said_kind;  /* the display kind last said in the console, -1 before the first */
    int auto_tried; /* Auto has looked for the display's headroom once, which SDL reports after the first frame */
    int headroom_pct;
    int have_frame;
    VkImageView swap_views[SNX_VK_SWAP_IMAGES];
    VkSwapchainKHR swap_views_for;
} g;

VitaVk* vita_vk(void) { return g.up ? &g.vk : NULL; }
void vita_vk_on_retire(void (*retire)(unsigned slot, uint64_t serial)) { g.vk.ring.retire = retire; }
uint32_t vita_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags properties) {
    return snx_vk_memory_type(&g.vk.memory_properties, bits, properties);
}
/* A device of this physical device and queue family, if it has what the Vita needs (snx_vk_first_device's make) */
static bool make_device(VkPhysicalDevice phys, uint32_t family, void* user) {
    VitaVk* v = user;
    SnxVkExtensions list;
    if (!snx_vk_device_extensions(phys, &list)) return false;
    const char* extensions[5];
    uint32_t n = 0;
    bool ok = snx_vk_extensions_has(&list, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    extensions[n++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    if (v->host.surface) {
        ok = ok && snx_vk_extensions_has(&list, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        extensions[n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        if (snx_vk_extensions_has(&list, VK_EXT_HDR_METADATA_EXTENSION_NAME))
            extensions[n++] = VK_EXT_HDR_METADATA_EXTENSION_NAME;
    }
    /* fragment programs that read their pixel's color (the program flag FRAGCOLOR_USED): the color attachment read
       as an input attachment while it is rendered to, in the order its primitives are drawn when the device keeps it */
    bool local_read = snx_vk_extensions_has(&list, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    bool raster_order = snx_vk_extensions_has(&list, VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME);
    if (local_read) extensions[n++] = VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
    if (raster_order) extensions[n++] = VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME;
    snx_vk_extensions_free(&list);
    if (!ok) return false;
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR lr = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT ro = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void* const query[] = {&features, &v12, &v13, &ro, &lr};
    snx_vk_chain(query, sizeof query / sizeof *query);
    vkGetPhysicalDeviceFeatures2(phys, &features);
    if (!v13.dynamicRendering || !v13.synchronization2) return false;
    v->local_read = local_read && lr.dynamicRenderingLocalRead;
    v->raster_order = raster_order && ro.rasterizationOrderColorAttachmentAccess;
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR want_lr = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR,
        .dynamicRenderingLocalRead = v->local_read};
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT want_ro = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT,
        .rasterizationOrderColorAttachmentAccess = v->raster_order};
    VkPhysicalDeviceVulkan13Features want13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .dynamicRendering = VK_TRUE,
                                               .synchronization2 = VK_TRUE};
    VkPhysicalDeviceVulkan12Features want12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                               .shaderFloat16 = v12.shaderFloat16};
    VkPhysicalDeviceFeatures2 want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    want.features.fragmentStoresAndAtomics = features.features.fragmentStoresAndAtomics;
    want.features.shaderClipDistance = features.features.shaderClipDistance;
    want.features.textureCompressionBC = features.features.textureCompressionBC;
    want.features.depthClamp = features.features.depthClamp;
    void* const enable[] = {&want, &want12, &want13, v->raster_order ? &want_ro : NULL,
                            v->local_read ? &want_lr : NULL};
    snx_vk_chain(enable, sizeof enable / sizeof *enable);
    if (snx_vk_device_create(phys, family, &want, extensions, n, &v->device) != VK_SUCCESS) return false;
    v->phys = phys;
    v->family = family;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
                                              .pNext = &host};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(phys, &properties);
    v->host_alignment = host.minImportedHostPointerAlignment ? host.minImportedHostPointerAlignment : 4096;
    v->preserve_infnan = p12.shaderSignedZeroInfNanPreserveFloat32;
    v->fp16 = v12.shaderFloat16 != 0;
    v->bc = features.features.textureCompressionBC;
    v->depth_clamp = features.features.depthClamp;
    VkFormatProperties d16;
    vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_D16_UNORM, &d16);
    const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    v->depth16_blit = (d16.optimalTilingFeatures & blit) == blit;
    static const VkFormat stencils[2] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    v->stencil_format = VK_FORMAT_UNDEFINED;
    for (unsigned i = 0; i < 2 && !v->stencil_format; ++i) {
        VkFormatProperties f;
        vkGetPhysicalDeviceFormatProperties(phys, stencils[i], &f);
        if (f.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) v->stencil_format = stencils[i];
    }
    return true;
}
/* the image the window takes: an HDR one where the settings and the surface allow it, else the SDR formats. The kind
   and the light the lift may reach are kept for the presenter. */
static int choose_display(VitaVk* v, VkFormat* format, VkColorSpaceKHR* space) {
    static const VkFormat hdr_formats[] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32};
    static const VkFormat scrgb_formats[] = {VK_FORMAT_R16G16B16A16_SFLOAT};
    VkSurfaceFormatKHR f = {0};
    int mode = snx_fx_hdr_parse(g_settings.hdr);
    int cs = snx_fx_colorspace_parse(g_settings.hdr_colours);
    int kind = SNX_FX_DISPLAY_NONE;
    if (!v->host.surface || mode == SNX_FX_HDR_OFF) return SNX_FX_DISPLAY_NONE;
    if (cs == SNX_FX_CS_AUTO || cs == SNX_FX_CS_P3) {
        if (snx_vk_surface_format_cs(v->phys, v->host.surface, scrgb_formats, 1, VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT,
                                     &f)) {
            kind = SNX_FX_DISPLAY_P3;
        } else if (cs == SNX_FX_CS_P3) {
            return SNX_FX_DISPLAY_NONE;
        }
    }
    if (kind == SNX_FX_DISPLAY_NONE && (cs == SNX_FX_CS_AUTO || cs == SNX_FX_CS_SCRGB)) {
        if (snx_vk_surface_format_cs(v->phys, v->host.surface, scrgb_formats, 1,
                                     VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, &f)) {
            kind = SNX_FX_DISPLAY_SCRGB;
        } else if (cs == SNX_FX_CS_SCRGB) {
            return SNX_FX_DISPLAY_NONE;
        }
    }
    if (kind == SNX_FX_DISPLAY_NONE &&
        snx_vk_surface_format_cs(v->phys, v->host.surface, hdr_formats, 2, VK_COLOR_SPACE_HDR10_ST2084_EXT, &f))
        kind = SNX_FX_DISPLAY_PQ;
    if (kind == SNX_FX_DISPLAY_NONE) return SNX_FX_DISPLAY_NONE;
    /* an extended-kind image's 1.0 is the display's SDR white: Auto takes one only where the display reports headroom
       over its white, as the Switch frontend does. On is explicit, and the display's headroom (SDL's, else the peak)
       bounds the lift. */
    if (mode == SNX_FX_HDR_AUTO && kind != SNX_FX_DISPLAY_PQ && snx_vk_window_headroom_pct(v->host.window) <= 100)
        return SNX_FX_DISPLAY_NONE;
    *format = f.format;
    *space = f.colorSpace;
    return kind;
}
/* the window's swapchain, made anew for its size and the HDR choice */
static bool swapchain_make(VitaVk* v) {
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
        .window = snx_vk_window_extent(&v->host, (VkExtent2D){960, 544})};
    g.display_kind = kind;
    {
        int reported = snx_vk_window_headroom_pct(v->host.window);
        g.headroom_pct =
            kind != SNX_FX_DISPLAY_NONE
                ? (reported > 100 ? snx_clamp_int(reported, 100, 1000)
                                  : snx_clamp_int(g_settings.hdr_peak * 100 / g_settings.hdr_paper_white, 100, 1000))
                : 100;
    }
    if (kind != g.said_kind) {
        g.said_kind = kind;
        if (kind != SNX_FX_DISPLAY_NONE)
            printf("vita: the window takes %s, %d%% headroom\n", snx_fx_display_label(kind), g.headroom_pct);
        else
            printf("vita: the window takes %s\n", snx_fx_display_label(kind));
    }
    if (g.fx) snx_fx_set_display(g.fx, g.display_kind, g.headroom_pct);
    if (g.swap_views_for) {
        vkDeviceWaitIdle(v->device);
        for (uint32_t i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
            if (g.swap_views[i]) vkDestroyImageView(v->device, g.swap_views[i], NULL);
        memset(g.swap_views, 0, sizeof g.swap_views);
        g.swap_views_for = VK_NULL_HANDLE;
    }
    if (!snx_vk_swapchain_make(v->phys, v->device, &info, &v->swap)) return false;
    for (uint32_t i = 0; i < v->swap.count; ++i) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = v->swap.images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = v->swap.format.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(v->device, &vi, NULL, &g.swap_views[i]) != VK_SUCCESS) {
            for (uint32_t k = 0; k < i; ++k) vkDestroyImageView(v->device, g.swap_views[k], NULL);
            memset(g.swap_views, 0, sizeof g.swap_views);
            return false;
        }
    }
    g.swap_views_for = v->swap.swapchain;
    return true;
}
/* the effects' short messages: the console, where this frontend has no log file */
static void present_fx_log(const char* text) { fprintf(stderr, "%s\n", text); }
bool vita_vk_init(const char* title, char* error, size_t error_size) {
    VitaVk* v = &g.vk;
    if (g.up) return true;
    memset(v, 0, sizeof *v);
    if (!snx_vk_window_open("scarletnx-vita", title ? title : "ScarletNX Vita", 960, 544, &v->host, error, error_size))
        return false;
    if (v->host.window) snx_window_icon(v->host.window);
    if (!snx_vk_first_device(v->host.instance.instance, VK_API_VERSION_1_3, VK_QUEUE_GRAPHICS_BIT, v->host.surface,
                             make_device, v)) {
        snprintf(error, error_size, "no Vulkan 1.3 device with dynamic rendering and VK_EXT_external_memory_host");
        vita_vk_free();
        return false;
    }
    volkLoadDevice(v->device);
    vkGetDeviceQueue(v->device, v->family, 0, &v->queue);
    vkGetPhysicalDeviceProperties(v->phys, &v->properties);
    vkGetPhysicalDeviceMemoryProperties(v->phys, &v->memory_properties);
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 8192},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * 4096},
                                    {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 4096}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 4096,
                                     .poolSizeCount = sizeof sizes / sizeof *sizes,
                                     .pPoolSizes = sizes};
    bool made = snx_vk_ring_init(&v->ring, v->device, v->queue, v->family, VITA_VK_SLOTS, &dp);
    for (unsigned i = 0; i < VITA_VK_SLOTS && made; ++i)
        made = vkCreateSemaphore(v->device, &semaphore, NULL, &v->acquired[i]) == VK_SUCCESS;
    for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES && made; ++i)
        made = vkCreateSemaphore(v->device, &semaphore, NULL, &v->rendered[i]) == VK_SUCCESS;
    if (!made) {
        snprintf(error, error_size, "cannot make the Vulkan device's command pool");
        vita_vk_free();
        return false;
    }
    g.up = true;
    g.said_kind = -1;
    g.fx = snx_fx_create(v->phys, v->device, v->queue, v->family, VK_NULL_HANDLE, VITA_VK_SLOTS, vita_vk_slot, NULL,
                         present_fx_log);
    if (g.fx) {
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
        snx_fx_configure(g.fx, &cfg);
        g.toast = snx_toast_create(v->phys, v->device, v->queue, v->family, VK_NULL_HANDLE, VITA_VK_SLOTS);
        g.overlay = snx_text_create(v->phys, v->device, v->queue, v->family, VK_NULL_HANDLE, VITA_VK_SLOTS);
        snx_stats_init(&g.stats);
        g.stats.on = g_settings.stats != 0; /* the setting starts it, F6 turns it over for the run */
        snx_fx_set_fp16(g.fx, v->fp16);
    }
    if (v->host.surface && !swapchain_make(v)) fprintf(stderr, "scarletnx-vita: no swapchain for the window yet\n");
    return true;
}
void vita_vk_free(void) {
    VitaVk* v = &g.vk;
    if (v->device) {
        vkDeviceWaitIdle(v->device);
        for (uint32_t i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
            if (g.swap_views[i]) vkDestroyImageView(v->device, g.swap_views[i], NULL);
        g.swap_views_for = VK_NULL_HANDLE;
        snx_vk_swapchain_free(v->device, &v->swap);
        if (g.frame_view) vkDestroyImageView(v->device, g.frame_view, NULL);
        if (g.frame) vkDestroyImage(v->device, g.frame, NULL);
        if (g.frame_memory) vkFreeMemory(v->device, g.frame_memory, NULL);
        if (g.toast) snx_toast_destroy(g.toast);
        g.toast = NULL;
        if (g.overlay) snx_text_destroy(g.overlay);
        g.overlay = NULL;
        snx_fx_destroy(g.fx);
        g.fx = NULL;
        for (unsigned i = 0; i < VITA_VK_SLOTS; ++i)
            if (v->acquired[i]) vkDestroySemaphore(v->device, v->acquired[i], NULL);
        for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
            if (v->rendered[i]) vkDestroySemaphore(v->device, v->rendered[i], NULL);
        snx_vk_ring_free(&v->ring);
        vkDestroyDevice(v->device, NULL);
    }
    snx_vk_window_close(&v->host);
    memset(v, 0, sizeof *v);
    g.frame = VK_NULL_HANDLE, g.frame_memory = VK_NULL_HANDLE, g.frame_width = g.frame_height = 0;
    g.up = false;
}
uint64_t vita_vk_poll(void) { return g.up ? snx_vk_ring_poll(&g.vk.ring) : 0; }
bool vita_vk_wait(uint64_t serial) { return !g.up || snx_vk_ring_wait(&g.vk.ring, serial, UINT64_MAX); }
VkCommandBuffer vita_vk_commands(void) { return snx_vk_ring_commands(&g.vk.ring); }
unsigned vita_vk_slot(void) { return g.vk.ring.slot; }
static uint64_t submit_with(VkSemaphore wait, VkSemaphore signal) {
    return g.up ? snx_vk_ring_submit(&g.vk.ring, wait, signal) : 0;
}
uint64_t vita_vk_submit(void) { return submit_with(VK_NULL_HANDLE, VK_NULL_HANDLE); }
VkDescriptorSet vita_vk_set(VkDescriptorSetLayout layout) { return snx_vk_ring_set(&g.vk.ring, layout); }
static void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags2 src_stage, VkAccessFlags2 src, VkPipelineStageFlags2 dst_stage,
                          VkAccessFlags2 dst) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = src_stage,
                               .srcAccessMask = src,
                               .dstStageMask = dst_stage,
                               .dstAccessMask = dst,
                               .oldLayout = from,
                               .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}
/* the presenter's image of the framebuffer's size */
static bool frame_image(VitaVk* v, uint32_t width, uint32_t height) {
    if (g.frame && g.frame_width == width && g.frame_height == height) return true;
    vkDeviceWaitIdle(v->device);
    if (g.frame_view) vkDestroyImageView(v->device, g.frame_view, NULL);
    if (g.frame) vkDestroyImage(v->device, g.frame, NULL);
    if (g.frame_memory) vkFreeMemory(v->device, g.frame_memory, NULL);
    g.frame = VK_NULL_HANDLE, g.frame_memory = VK_NULL_HANDLE, g.frame_view = VK_NULL_HANDLE;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {width, height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(v->device, &ii, NULL, &g.frame) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->device, g.frame, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex =
                                   vita_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits, 0);
    if (vkAllocateMemory(v->device, &ai, NULL, &g.frame_memory) != VK_SUCCESS ||
        vkBindImageMemory(v->device, g.frame, g.frame_memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = g.frame,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(v->device, &vi, NULL, &g.frame_view) != VK_SUCCESS) return false;
    g.frame_width = width, g.frame_height = height;
    g.frame_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}
/* one presented image: an acquired swapchain image, the effects on the source, HDR or the blit, and the present. The
   source is in SHADER_READ_ONLY_OPTIMAL. Each call takes the next ring slot, so two calls can present the blend and
   then the current frame. */
int vita_vk_cycle_upscaler(void) { return g.fx ? snx_fx_upscale_cycle(g.fx) : -1; }
int vita_vk_cycle_aa(void) { return g.fx ? snx_fx_aa_cycle(g.fx) : -1; }
int vita_vk_toggle_hdr(void) { return g.fx ? snx_fx_hdr_toggle(g.fx) : -1; }
void vita_vk_toast(const char* text) {
    if (g.toast) snx_toast_show(g.toast, text, SDL_GetTicksNS());
}
int vita_vk_stats_toggle(void) { return snx_stats_toggle(&g.stats); }
void vita_vk_toggle_fullscreen(void) {
    SDL_Window* window = g.vk.host.window;
    if (window) SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
}

/* the buttons the player holds, in the overlay's words, with the sticks where they are off their rest */
static const SnxStatsButton ctrl_buttons[] = {{VITA_CTRL_CROSS, "Cross"},   {VITA_CTRL_CIRCLE, "Circle"},
                                              {VITA_CTRL_SQUARE, "Square"}, {VITA_CTRL_TRIANGLE, "Triangle"},
                                              {VITA_CTRL_LTRIGGER, "L"},    {VITA_CTRL_RTRIGGER, "R"},
                                              {VITA_CTRL_START, "Start"},   {VITA_CTRL_SELECT, "Select"},
                                              {VITA_CTRL_UP, "^"},          {VITA_CTRL_DOWN, "v"},
                                              {VITA_CTRL_LEFT, "<"},        {VITA_CTRL_RIGHT, ">"}};
static void stats_inputs(char* out, size_t size) {
    VitaInput in;
    size_t at;
    int lx, ly, rx, ry;
    vita_input_read(&in);
    snx_stats_inputs(out, size, in.buttons, ctrl_buttons, (unsigned)(sizeof ctrl_buttons / sizeof *ctrl_buttons));
    lx = (int)in.lx - 128, ly = (int)in.ly - 128, rx = (int)in.rx - 128, ry = (int)in.ry - 128;
    at = strlen(out);
    if ((lx > 20 || lx < -20 || ly > 20 || ly < -20 || rx > 20 || rx < -20 || ry > 20 || ry < -20) && at + 40 < size) {
        if (!strcmp(out, "-")) out[at = 0] = 0;
        snprintf(out + at, size - at, "%sL %+.1f,%+.1f R %+.1f,%+.1f", at ? "  " : "", (double)lx / 128.0,
                 (double)ly / 128.0, (double)rx / 128.0, (double)ry / 128.0);
    }
}
/* the frame rate and the held buttons, at the window's bottom left */
static void stats_overlay(VkCommandBuffer cb, VkImageView view, VkFormat format, float white, uint32_t w, uint32_t h,
                          unsigned slot) {
    char inputs[128];
    if (!g.overlay || !g.stats.on) return;
    stats_inputs(inputs, sizeof inputs);
    snx_stats_draw(g.overlay, &g.stats, cb, view, format, w, h, white, slot, SDL_GetTicksNS(), inputs);
}

static bool present_image(VitaVk* v, VkImage src_image, VkImageView src_view, uint32_t width, uint32_t height,
                          uint32_t frame_w, uint32_t frame_h) {
    unsigned slot = v->ring.slot;
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(v->device, v->swap.swapchain, 0, v->acquired[slot], VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_make(v);
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;
    VkCommandBuffer cb = vita_vk_commands();
    VkImage target = v->swap.images[index];
    VkImageView target_view = g.swap_views[index];
    uint32_t w = v->swap.extent.width, h = v->swap.extent.height;
    uint32_t sw = w, sh = (uint32_t)((uint64_t)w * height / width);
    if (sh > h) sh = h, sw = (uint32_t)((uint64_t)h * width / height);
    int32_t x = (int32_t)(w - sw) / 2, y = (int32_t)(h - sh) / 2;
    int have_fx = 0;
    SnxFxImage fxout = {0};
    uint32_t out_w = width, out_h = height;
    if (g.fx) {
        int filter = snx_fx_pick_filter(g.fx, frame_w, frame_h, sw, sh, 0);
        int aa = snx_fx_pick_aa(g.fx, snx_fx_aa_parse(g_settings.anti_aliasing), frame_w, frame_h, sw, sh,
                                v->properties.limits.maxImageDimension2D);
        have_fx = snx_fx_render(g.fx, cb, src_view, width, height, 0, 0, width, height, 0, sw, sh, filter, aa, &fxout);
        if (have_fx) {
            src_image = fxout.image;
            src_view = fxout.view;
            out_w = fxout.width;
            out_h = fxout.height;
            image_barrier(cb, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    }
    if (g.fx && g.display_kind != SNX_FX_DISPLAY_NONE && snx_fx_hdr_active(g.fx)) {
        SnxFxImage comp = {0};
        if (snx_fx_hdr_compose(g.fx, cb, src_view, VK_FORMAT_R8G8B8A8_UNORM, out_w, out_h, 0, 0, out_w, out_h, 0, w, h,
                               x, y, sw, sh, &comp)) {
            if (g_settings.hdr_test_pattern) snx_fx_hdr_test(g.fx, cb, &comp);
            if (g.toast)
                snx_toast_draw(g.toast, cb, comp.view, snx_fx_compose_format(), w, h, (float)g_settings.hdr_paper_white,
                               slot, SDL_GetTicksNS());
            stats_overlay(cb, comp.view, snx_fx_compose_format(), (float)g_settings.hdr_paper_white, w, h, slot);
            if (snx_fx_hdr_encode(g.fx, cb, &comp, target, target_view, v->swap.format.format, w, h,
                                  snx_fx_encode_kind(g.fx))) {
                image_barrier(cb, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_NONE, 0);
                submit_with(v->acquired[slot], v->rendered[index]);
                goto present;
            }
        }
    }
    if (!have_fx) {
        image_barrier(cb, src_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    image_barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
    {
        VkClearColorValue black = {{0, 0, 0, 1}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .srcOffsets = {{0, 0, 0}, {(int32_t)out_w, (int32_t)out_h, 1}},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .dstOffsets = {{x, y, 0}, {x + (int32_t)sw, y + (int32_t)sh, 1}}};
        vkCmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdBlitImage(cb, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    if (g.toast) snx_toast_draw(g.toast, cb, target_view, v->swap.format.format, w, h, 1.0f, slot, SDL_GetTicksNS());
    stats_overlay(cb, target_view, v->swap.format.format, 1.0f, w, h, slot);
    image_barrier(cb, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_NONE, 0);
    submit_with(v->acquired[slot], v->rendered[index]);
present:;
    {
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                               .waitSemaphoreCount = 1,
                               .pWaitSemaphores = &v->rendered[index],
                               .swapchainCount = 1,
                               .pSwapchains = &v->swap.swapchain,
                               .pImageIndices = &index};
        r = vkQueuePresentKHR(v->queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchain_make(v);
    }
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

bool vita_vk_present(VkBuffer buffer, VkDeviceSize offset, uint32_t pitch, uint32_t width, uint32_t height) {
    VitaVk* v = &g.vk;
    if (!g.up || !v->host.surface) return false;
    /* Auto follows the display's headroom, which SDL reports only once the window is on screen: the first frame that
       sees it takes the HDR swapchain, and no more than once where the surface offers none */
    if (g.fx && snx_fx_hdr_mode(g.fx) == SNX_FX_HDR_AUTO && !g.auto_tried && g.display_kind == SNX_FX_DISPLAY_NONE &&
        snx_vk_window_headroom_pct(v->host.window) > 100) {
        g.auto_tried = 1;
        if (!swapchain_make(v)) return false;
    }
    if (!v->swap.swapchain && !swapchain_make(v)) return false;
    if (!frame_image(v, width, height)) return false;
    VkCommandBuffer cb = vita_vk_commands();
    int interp =
        snx_fx_interp_parse(g_settings.frame_interpolation) == SNX_FX_INTERP_BLEND ||
        (snx_fx_interp_parse(g_settings.frame_interpolation) == SNX_FX_INTERP_AUTO && snx_fx_interp_auto(60, 60));
    SnxFxImage keep = {0};
    int have_keep = g.fx && interp && g.have_frame && snx_fx_keep(g.fx, cb, g.frame_view, width, height, &keep);
    if (have_keep)
        image_barrier(cb, keep.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    image_barrier(cb, g.frame, g.frame_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
    {
        VkBufferImageCopy copy = {.bufferOffset = offset,
                                  .bufferRowLength = pitch,
                                  .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {width, height, 1}};
        vkCmdCopyBufferToImage(cb, buffer, g.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    image_barrier(cb, g.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    g.frame_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (have_keep) {
        SnxFxImage blend = {0};
        if (snx_fx_blend(g.fx, cb, keep.view, g.frame_view, width, height, 0.5f, &blend)) {
            image_barrier(cb, blend.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            (void)present_image(v, blend.image, blend.view, width, height, width, height);
        }
    }
    bool ok = present_image(v, g.frame, g.frame_view, width, height, width, height);
    if (ok) {
        snx_stats_frame(&g.stats, SDL_GetTicksNS());
        if (have_keep) snx_stats_generated(&g.stats);
    }
    g.have_frame = 1;
    return ok;
}
