/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/renderdoc.h"
#include "vita/gpu/vk.h"
#include "common/gpu/vk/marks.h"
#include "common/gpu/vk/gpu_pstate.h"
#include "common/gpu/vk/device_settings.h"
#include "common/gpu/vk/gpu_driver_load.h"
#include "common/gpu/vk/notice.h"
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/rects.h"
#include "common/utilities/vpad_sdl.h"
#include "common/utilities/window_fullscreen.h"
#include "common/utilities/window_icon.h"
#include "snx_settings.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static struct {
    VitaVk vk;
    bool up;
    /* the device is chosen among those that import guest memory, then (copies) among all, which work on copies of it;
       skipped: a GPU was passed over for it, which is taken before a CPU that imports */
    bool copies, skipped;
    const char* copy_why; /* why the device chosen last does not import it, NULL when it does */
    /* the presenter's copy of the framebuffer, which the window's rendering samples */
    VkImage frame;
    VkDeviceMemory frame_memory;
    VkImageView frame_view; /* the upscaler's and the anti-aliasing's source */
    uint32_t frame_width, frame_height;
    VkImageLayout frame_layout;
    /* a line over the game for a few seconds, as a Switch game shows one (common/gpu/vk/notice.h) */
    SnxNotice notice;
    SnxVkNotice* notice_vk;
    SnxVkRects* controls;
    VkRect2D picture; /* where the framebuffer showed last, no size before the first */
    /* the frame drawn into the window (common/gpu/vk/present_fx.h): through the upscaler and the anti-aliasing as the
       settings say, or sampled smooth with both Off */
    SnxFxConfig fx_config;
    SnxFx* fx;
} g;

VitaVk* vita_vk(void) { return g.up ? &g.vk : NULL; }
void vita_vk_on_retire(void (*retire)(unsigned slot, uint64_t serial)) { g.vk.ring.retire = retire; }
void vita_vk_on_submit(void (*submit)(void)) { g.vk.ring.close = submit; }
uint32_t vita_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags properties) {
    return snx_vk_memory_type(&g.vk.memory_properties, bits, properties);
}
/* a line of the extension bans (extension_bans.h) for the log */
static void ban_say(const char* line, int warn) { fprintf(stderr, "%s%s\n", warn ? "warning: " : "", line); }
/* what the device can't go without, into need: how many */
static uint32_t extension_needs(bool windowed, VkExtensionNeed* need) {
    uint32_t needs = 0;
    if (windowed) need[needs++] = (VkExtensionNeed){VK_KHR_SWAPCHAIN_EXTENSION_NAME, "the window is drawn with it"};
    return needs;
}
/* The Vita's device on a physical device and queue family (common/gpu/vk/device.h) */
static bool choose(SnxVkChoice* c, void* user) {
    VitaVk* v = user;
    VkPhysicalDevice phys = c->phys;
    /* a driver that takes an import's pages again at each submission (extension_bans.txt) refuses it, EPERM on amdgpu,
       while the watch has one of them write-protected (watch.c), as a scene's surface when it ends: nothing would be
       drawn */
    bool listed = snx_vk_choice_has(c, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    bool refuses = vk_ban("VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT", listed, ban_say) != 0;
    v->host_import = !refuses && snx_vk_choice_enable(c, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    g.copy_why = v->host_import ? NULL
                 : refuses      ? "refuses work on imported memory while a page of it is write-protected"
                                : "has no VK_EXT_external_memory_host to import the guest's memory";
    bool cpu = c->properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    if (!g.copies && (!v->host_import || (cpu && g.skipped))) {
        if (!cpu) g.skipped = true;
        return snx_vk_choice_refuse(c, g.copy_why ? g.copy_why : "is a CPU, after a GPU that can work on copies");
    }
    if (v->host.surface && !snx_vk_choice_enable(c, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        return snx_vk_choice_refuse(c, "has no VK_KHR_swapchain to draw the window with");
    /* fragment programs that read their pixel's color (the program flag FRAGCOLOR_USED): the color attachment read
       as an input attachment while it is rendered to, in the order its primitives are drawn when the device keeps it */
    bool local_read = snx_vk_choice_enable(c, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    bool raster_order = snx_vk_choice_enable(c, VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME);
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
    if (!v13.dynamicRendering || !v13.synchronization2)
        return snx_vk_choice_refuse(c, "has no dynamicRendering or no synchronization2");
    /* without timeline semaphores the ring waits for the fence of each slot (the table turns them off on Android) */
    if (vk_ban("timelineSemaphore", v12.timelineSemaphore, ban_say)) v12.timelineSemaphore = VK_FALSE;
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
                                               .shaderFloat16 = v12.shaderFloat16,
                                               .timelineSemaphore = v12.timelineSemaphore};
    VkPhysicalDeviceFeatures2 want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    want.features.fragmentStoresAndAtomics = features.features.fragmentStoresAndAtomics;
    want.features.shaderClipDistance = features.features.shaderClipDistance;
    want.features.textureCompressionBC = features.features.textureCompressionBC;
    want.features.depthClamp = features.features.depthClamp;
    want.features.samplerAnisotropy = features.features.samplerAnisotropy;
    void* const enable[] = {&want, &want12, &want13, v->raster_order ? &want_ro : NULL,
                            v->local_read ? &want_lr : NULL};
    snx_vk_chain(enable, sizeof enable / sizeof *enable);
    v->checkpoints = snx_vk_marks_choose(c, g_settings.gpu_checkpoints);
    want.pNext = snx_vk_pacing_choose(&v->pacing, c, g_settings.frame_pacing, want.pNext);
    char gpl_line[256];
    want.pNext = snx_vk_gpl_choose(&v->gpl, c, g_settings.pipeline_libraries, want.pNext, gpl_line, sizeof gpl_line);
    fprintf(stderr, "scarletnx-vita: %s\n", gpl_line);
    if (!snx_vk_choice_create(c, &want, NULL, NULL, 0)) return false;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
                                              .pNext = v->host_import ? &host : NULL};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(phys, &properties);
    v->host_alignment = host.minImportedHostPointerAlignment ? host.minImportedHostPointerAlignment : 4096;
    v->preserve_infnan = p12.shaderSignedZeroInfNanPreserveFloat32;
    v->bc = features.features.textureCompressionBC;
    v->depth_clamp = features.features.depthClamp;
    v->anisotropic = features.features.samplerAnisotropy;
    v->timeline = v12.timelineSemaphore;
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
/* the window's swapchain, made anew for its size */
static bool swapchain_make(VitaVk* v) {
    static const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    SnxVkSwapchainInfo info = {.surface = v->host.surface,
                               .formats = formats,
                               .format_count = sizeof formats / sizeof *formats,
                               .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                               .window = snx_vk_window_extent(&v->host, (VkExtent2D){960, 544}),
                               .views = true,
                               .unsynced = !g_settings.vsync};
    return snx_vk_swapchain_make(v->phys, v->device, &info, &v->swap);
}
static void fx_log(const char* text) { fprintf(stderr, "scarletnx-vita: %s\n", text); }
/* the upscaler and the anti-aliasing the settings ask for, said once, and the frame's draw: false where it could not
   be made */
static bool fx_init(VitaVk* v) {
    char said[512];
    SnxFxConfig* c = &g.fx_config;
    if (snx_fx_settings(c, g_settings.upscaler, g_settings.upscaler_sharpness, g_settings.anti_aliasing, said,
                        sizeof said))
        fprintf(stderr, "scarletnx-vita: settings: %s\n", said);
    if (!(g.fx = snx_fx_create(v->phys, v->device, VK_NULL_HANDLE, VITA_VK_SLOTS, fx_log))) return false;
    snx_fx_set_sharpness(g.fx, c->sharpness);
    fprintf(stderr, "scarletnx-vita: upscaler %s, anti-aliasing %s\n", snx_fx_upscale_label(c->upscaler),
            snx_fx_aa_label(c->aa));
    return true;
}
bool vita_vk_init(const char* title, char* error, size_t error_size) {
    VitaVk* v = &g.vk;
    if (g.up) return true;
    memset(v, 0, sizeof *v);
    snx_renderdoc_load_window(); /* before the instance: RenderDoc's capture layer is an implicit one */
    if (!snx_vk_window_open("scarletnx-vita", title ? title : "ScarletNX Vita", 960, 544, &v->host, error, error_size))
        return false;
    if (v->host.window) snx_window_icon(v->host.window);
    /* the window on the whole screen, as the Switch's fullscreen=1 asks, which a phone's launcher always gives */
    char why[256];
    if (v->host.window && !snx_window_fullscreen(v->host.window, g_settings.fullscreen, why, sizeof why))
        fprintf(stderr, "scarletnx-vita: %s\n", why);
    VkExtensionNeed need[2];
    VkExtensionSetting settings[VK_EXTENSION_SETTINGS];
    SnxVkDeviceRequest request = {.who = "scarletnx-vita",
                                  .none = "no Vulkan 1.3 device with dynamic rendering",
                                  .instance = v->host.instance.instance,
                                  .api = VK_API_VERSION_1_3,
                                  .queues = VK_QUEUE_GRAPHICS_BIT,
                                  .surface = v->host.surface,
                                  .choose = choose,
                                  .user = v,
                                  .need = need,
                                  .needs = extension_needs(v->host.surface != VK_NULL_HANDLE, need)};
    snx_vk_device_settings(&request, settings);
    SnxVkDevice device;
    g.copies = g.skipped = false;
    bool device_made = snx_vk_device_make(&request, &device, error, error_size);
    if (!device_made && device.passed_over) {
        g.copies = true;
        device_made = snx_vk_device_make(&request, &device, error, error_size);
    }
    if (!device_made) {
        /* a package whose driver lists no device at all: its name and this device's GPU, not the generic line. A device
           it lists and the renderer refuses has its reason in error already */
        char said[512];
        uint32_t listed = 0;
        vkEnumeratePhysicalDevices(v->host.instance.instance, &listed, NULL);
        if (!listed && gpu_driver_no_device(said, sizeof said)) snprintf(error, error_size, "%s", said);
        vita_vk_free();
        return false;
    }
    v->phys = device.phys;
    v->device = device.device;
    v->family = device.family;
    vkGetDeviceQueue(v->device, v->family, 0, &v->queue);
    vkGetPhysicalDeviceProperties(v->phys, &v->properties);
    /* the Switch's anisotropy for the samplers render.c makes, as the 3DS's, said with the value in effect */
    char aniso[256];
    v->anisotropy = snx_vk_anisotropy(g_settings.anisotropic_filtering, v->anisotropic,
                                      v->properties.limits.maxSamplerAnisotropy, aniso, sizeof aniso);
    if (aniso[0]) fprintf(stderr, "scarletnx-vita: %s\n", aniso);
    if (g.copy_why)
        fprintf(stderr, "scarletnx-vita: %s %s: guest memory is copied\n", v->properties.deviceName, g.copy_why);
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
    bool made = snx_vk_ring_init(&v->ring, v->device, v->queue, v->family, VITA_VK_SLOTS, &dp, v->timeline);
    if (made)
        v->ring.marks = snx_vk_marks_new(v->phys, v->device, v->family, g_settings.gpu_profile,
                                         g_settings.gpu_checkpoints, v->checkpoints, "scarletnx-vita", "frame");
    for (unsigned i = 0; i < VITA_VK_SLOTS && made; ++i)
        made = vkCreateSemaphore(v->device, &semaphore, NULL, &v->acquired[i]) == VK_SUCCESS;
    for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES && made; ++i)
        made = vkCreateSemaphore(v->device, &semaphore, NULL, &v->rendered[i]) == VK_SUCCESS;
    if (!made) {
        snprintf(error, error_size, "cannot make the Vulkan device's command buffers, descriptor pools and semaphores");
        vita_vk_free();
        return false;
    }
    if (!(g.notice_vk = snx_vk_notice_create(v->phys, v->device, VITA_VK_SLOTS)) ||
        !(g.controls = snx_vk_rects_create(v->phys, v->device, VITA_VK_SLOTS))) {
        snprintf(error, error_size, "out of memory for the window's notices and on-screen controls");
        vita_vk_free();
        return false;
    }
    g.up = true;
    if (v->host.surface && !swapchain_make(v)) fprintf(stderr, "scarletnx-vita: no swapchain for the window yet\n");
    if (v->swap.swapchain) {
        char said[160];
        snx_vk_present_said(v->swap.mode, g_settings.vsync, said, sizeof said);
        fprintf(stderr, "scarletnx-vita: %s\n", said);
        snx_vk_pacing_said(&v->pacing, g_settings.frame_pacing, said, sizeof said);
        fprintf(stderr, "scarletnx-vita: %s\n", said);
    }
    if (!fx_init(v)) {
        snprintf(error, error_size, "the frame's draw into the window could not be made on this device");
        vita_vk_free();
        return false;
    }
    return true;
}
void vita_vk_free(void) {
    VitaVk* v = &g.vk;
    if (v->device) {
        vkDeviceWaitIdle(v->device);
        snx_vk_swapchain_free(v->device, &v->swap);
        snx_vk_notice_destroy(g.notice_vk);
        snx_vk_rects_destroy(g.controls);
        snx_fx_destroy(g.fx);
        if (g.frame_view) vkDestroyImageView(v->device, g.frame_view, NULL);
        if (g.frame) vkDestroyImage(v->device, g.frame, NULL);
        if (g.frame_memory) vkFreeMemory(v->device, g.frame_memory, NULL);
        for (unsigned i = 0; i < VITA_VK_SLOTS; ++i)
            if (v->acquired[i]) vkDestroySemaphore(v->device, v->acquired[i], NULL);
        for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
            if (v->rendered[i]) vkDestroySemaphore(v->device, v->rendered[i], NULL);
        snx_vk_marks_free(v->ring.marks, v->device);
        snx_vk_ring_free(&v->ring);
        vkDestroyDevice(v->device, NULL);
    }
    snx_vk_window_close(&v->host);
    memset(v, 0, sizeof *v);
    g.frame = VK_NULL_HANDLE, g.frame_memory = VK_NULL_HANDLE, g.frame_view = VK_NULL_HANDLE;
    g.frame_width = g.frame_height = 0;
    g.fx = NULL;
    g.notice_vk = NULL;
    g.controls = NULL;
    g.picture = (VkRect2D){0};
    g.up = false;
}
static void notice_log(const char* text) { fprintf(stderr, "scarletnx-vita: %s\n", text); }
bool vita_vk_picture(int rect[4]) {
    int w = 0, h = 0;
    if (!g.up || !g.vk.host.window || !SDL_GetWindowSizeInPixels(g.vk.host.window, &w, &h)) return false;
    rect[0] = g.picture.offset.x, rect[1] = g.picture.offset.y;
    rect[2] = (int)g.picture.extent.width, rect[3] = (int)g.picture.extent.height;
    if (!rect[2] || !rect[3]) rect[0] = rect[1] = 0, rect[2] = w, rect[3] = h;
    return rect[2] > 0 && rect[3] > 0;
}
const char* vita_vk_effect_step(bool aa) {
    int* at = aa ? &g.fx_config.aa : &g.fx_config.upscaler;
    *at = aa ? snx_fx_aa_next(*at) : snx_fx_upscale_next(*at);
    return aa ? snx_fx_aa_label(*at) : snx_fx_upscale_label(*at);
}
void vita_vk_notice(const char* text) { snx_notice_show(&g.notice, text, SDL_GetTicksNS()); }
const char* vita_vk_notice_text(void) { return snx_notice_shown(&g.notice, SDL_GetTicksNS()) ? g.notice.text : ""; }
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
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
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
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(v->device, &ii, NULL, &g.frame) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->device, g.frame, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits,
                                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits, 0);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = g.frame,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkAllocateMemory(v->device, &ai, NULL, &g.frame_memory) != VK_SUCCESS ||
        vkBindImageMemory(v->device, g.frame, g.frame_memory, 0) != VK_SUCCESS ||
        vkCreateImageView(v->device, &vi, NULL, &g.frame_view) != VK_SUCCESS)
        return false;
    g.frame_width = width, g.frame_height = height;
    g.frame_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}
bool vita_vk_present(VkBuffer buffer, VkDeviceSize offset, uint32_t pitch, uint32_t width, uint32_t height) {
    VitaVk* v = &g.vk;
    if (!g.up || !v->host.surface) return false;
    if (!v->swap.swapchain && !swapchain_make(v)) return false;
    if (!frame_image(v, width, height)) return false;
    unsigned slot = v->ring.slot;
    uint32_t index = 0;
    /* an image the window can take now, or none this vblank: the game's threads run on this one, which a wait for
       the display's refresh (FIFO) would stall each vblank. The framebuffer stays in guest memory for the next */
    VkResult r = vkAcquireNextImageKHR(v->device, v->swap.swapchain, 0, v->acquired[slot], VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_make(v);
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false; /* VK_NOT_READY, VK_TIMEOUT: none free yet */
    VkCommandBuffer cb = vita_vk_commands();
    image_barrier(cb, g.frame, g.frame_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = {.bufferOffset = offset,
                              .bufferRowLength = pitch,
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {width, height, 1}};
    vkCmdCopyBufferToImage(cb, buffer, g.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(cb, g.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    g.frame_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    /* the frame scaled to the window, its aspect kept */
    uint32_t w = v->swap.extent.width, h = v->swap.extent.height;
    uint32_t sw = w, sh = (uint32_t)((uint64_t)w * height / width);
    if (sh > h) sh = h, sw = (uint32_t)((uint64_t)h * width / height);
    VkRect2D to = {{(int32_t)(w - sw) / 2, (int32_t)(h - sh) / 2}, {sw, sh}};
    /* the upscaler's and the anti-aliasing's passes but their last, which draws in the window's rendering, as the
       frame sampled smooth does with both off */
    bool drawn = sw && sh &&
                 snx_fx_prepare(g.fx, cb, slot, g.frame_view, g.frame_layout, width, height, 0, 0, width, height, 0, to,
                                v->swap.format.format, g.fx_config.upscaler, g.fx_config.aa);
    snx_vk_swapchain_begin(cb, &v->swap, index);
    if (drawn) snx_fx_draw(g.fx, cb, slot);
    (void)snx_vk_notice_draw(g.notice_vk, cb, slot, v->swap.format.format, w, h,
                             snx_display_percent(v->host.window ? SDL_GetWindowDisplayScale(v->host.window) : 0.0f),
                             &g.notice, SDL_GetTicksNS(), notice_log);
    (void)snx_vk_rects_draw(g.controls, cb, slot, v->swap.format.format, w, h, snx_vpad_window_rects((int)w, (int)h),
                            notice_log);
    g.picture = to;
    snx_vk_swapchain_end(cb, &v->swap, index);
    submit_with(v->acquired[slot], v->rendered[index]);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &v->rendered[index],
                           .swapchainCount = 1,
                           .pSwapchains = &v->swap.swapchain,
                           .pImageIndices = &index};
    snx_renderdoc_present(v->host.instance.instance, ++v->presents);
    snx_gpu_pstate_present(); /* a kgsl hold lasts while frames are presented */
    snx_vk_pacing_present(&v->pacing, &v->paced, v->device, v->swap.swapchain,
                          g_settings.vsync && snx_vpad_window_frame_ns(1) == 1, &pi, "scarletnx-vita");
    r = vkQueuePresentKHR(v->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchain_make(v);
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}
