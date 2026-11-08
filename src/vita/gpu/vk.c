/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/gpu/vk.h"
#include "common/utilities/window_icon.h"
#include <stdio.h>
#include <string.h>

static struct {
    VitaVk vk;
    bool up;
    /* the presenter's copy of the framebuffer, which the swapchain image is blitted from */
    VkImage frame;
    VkDeviceMemory frame_memory;
    uint32_t frame_width, frame_height;
    VkImageLayout frame_layout;
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
    const char* extensions[4];
    uint32_t n = 0;
    bool ok = snx_vk_extensions_has(&list, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    extensions[n++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    if (v->host.surface) {
        ok = ok && snx_vk_extensions_has(&list, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        extensions[n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
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
/* the window's swapchain, made anew for its size */
static bool swapchain_make(VitaVk* v) {
    static const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    SnxVkSwapchainInfo info = {.surface = v->host.surface,
                               .formats = formats,
                               .format_count = sizeof formats / sizeof *formats,
                               .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                               .window = snx_vk_window_extent(&v->host, (VkExtent2D){960, 544})};
    return snx_vk_swapchain_make(v->phys, v->device, &info, &v->swap);
}
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
    if (v->host.surface && !swapchain_make(v)) fprintf(stderr, "scarletnx-vita: no swapchain for the window yet\n");
    return true;
}
void vita_vk_free(void) {
    VitaVk* v = &g.vk;
    if (v->device) {
        vkDeviceWaitIdle(v->device);
        snx_vk_swapchain_free(v->device, &v->swap);
        if (g.frame) vkDestroyImage(v->device, g.frame, NULL);
        if (g.frame_memory) vkFreeMemory(v->device, g.frame_memory, NULL);
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
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}
/* the presenter's image of the framebuffer's size */
static bool frame_image(VitaVk* v, uint32_t width, uint32_t height) {
    if (g.frame && g.frame_width == width && g.frame_height == height) return true;
    vkDeviceWaitIdle(v->device);
    if (g.frame) vkDestroyImage(v->device, g.frame, NULL);
    if (g.frame_memory) vkFreeMemory(v->device, g.frame_memory, NULL);
    g.frame = VK_NULL_HANDLE, g.frame_memory = VK_NULL_HANDLE;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {width, height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(v->device, &ii, NULL, &g.frame) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->device, g.frame, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits,
                                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits, 0);
    if (vkAllocateMemory(v->device, &ai, NULL, &g.frame_memory) != VK_SUCCESS ||
        vkBindImageMemory(v->device, g.frame, g.frame_memory, 0) != VK_SUCCESS)
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
    image_barrier(cb, g.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT);
    g.frame_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImage target = v->swap.images[index];
    image_barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
    /* the frame scaled to the window, its aspect kept */
    uint32_t w = v->swap.extent.width, h = v->swap.extent.height;
    uint32_t sw = w, sh = (uint32_t)((uint64_t)w * height / width);
    if (sh > h) sh = h, sw = (uint32_t)((uint64_t)h * width / height);
    int32_t x = (int32_t)(w - sw) / 2, y = (int32_t)(h - sh) / 2;
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {(int32_t)width, (int32_t)height, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{x, y, 0}, {x + (int32_t)sw, y + (int32_t)sh, 1}}};
    vkCmdBlitImage(cb, g.frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);
    image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    submit_with(v->acquired[slot], v->rendered[index]);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &v->rendered[index],
                           .swapchainCount = 1,
                           .pSwapchains = &v->swap.swapchain,
                           .pImageIndices = &index};
    r = vkQueuePresentKHR(v->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchain_make(v);
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}
