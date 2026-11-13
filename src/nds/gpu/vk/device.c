/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nds/gpu/vk/vk.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static struct {
    NdsVk vk;
    bool up;
} g_device;

NdsVk* nds_vk(void) { return g_device.up ? &g_device.vk : NULL; }

/* a device of this physical device and queue family, if it has what the NDS needs (snx_vk_first_device's make) */
static bool make_device(VkPhysicalDevice phys, uint32_t family, void* user) {
    NdsVk* v = user;
    SnxVkExtensions list;
    if (!snx_vk_device_extensions(phys, &list)) return false;
    const char* extensions[2];
    uint32_t n = 0;
    bool ok = true;
    if (v->host.surface) {
        ok = snx_vk_extensions_has(&list, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        extensions[n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }
    if (snx_vk_extensions_has(&list, "VK_KHR_portability_subset")) extensions[n++] = "VK_KHR_portability_subset";
    snx_vk_extensions_free(&list);
    if (!ok) return false;
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v13};
    vkGetPhysicalDeviceFeatures2(phys, &features);
    if (!v13.dynamicRendering || !features.features.independentBlend) return false;
    VkPhysicalDeviceVulkan13Features want13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceFeatures2 want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &want13};
    want.features.independentBlend = VK_TRUE;
    if (snx_vk_device_create(phys, family, &want, extensions, n, &v->device) != VK_SUCCESS) return false;
    v->phys = phys;
    v->family = family;
    return true;
}

bool nds_vk_init(const char* title, bool headless, char* error, size_t error_size) {
    NdsVk* v = &g_device.vk;
    memset(v, 0, sizeof *v);
    v->headless = headless;
    if (headless) {
        /* no window: the loader and an instance alone */
        if (volkInitialize() != VK_SUCCESS) {
            snprintf(error, error_size, "no Vulkan loader");
            return false;
        }
        SnxVkInstanceInfo info = {.app_name = "scarletnx-nds", .portability = true};
        if (snx_vk_instance_create(&info, &v->host.instance) != VK_SUCCESS) {
            snprintf(error, error_size, "no Vulkan 1.3 instance");
            return false;
        }
    } else if (!snx_vk_window_open("scarletnx-nds", title, NDS_SCREEN_WIDTH * 2, NDS_SCREEN_HEIGHT * 4, &v->host, error,
                                   error_size))
        return false;
    VkQueueFlags queue = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    if (!snx_vk_first_device(v->host.instance.instance, VK_API_VERSION_1_3, queue, v->host.surface, make_device, v)) {
        snprintf(error, error_size, "no Vulkan 1.3 device with dynamic rendering");
        snx_vk_window_close(&v->host);
        return false;
    }
    volkLoadDevice(v->device);
    vkGetDeviceQueue(v->device, v->family, 0, &v->queue);
    vkGetPhysicalDeviceProperties(v->phys, &v->properties);
    vkGetPhysicalDeviceMemoryProperties(v->phys, &v->memory_properties);
    /* a depth buffer of 24 bits or more with a stencil: the 3D engine's depth is 24 bits */
    static const VkFormat depth[] = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    for (size_t i = 0; i < sizeof depth / sizeof *depth && !v->depth_format; ++i) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(v->phys, depth[i], &p);
        if ((p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
            (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            v->depth_format = depth[i];
    }
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 64,
                                     .poolSizeCount = sizeof sizes / sizeof *sizes,
                                     .pPoolSizes = sizes};
    if (!v->depth_format || !snx_vk_ring_init(&v->ring, v->device, v->queue, v->family, NDS_VK_SLOTS, &dp)) {
        snprintf(error, error_size, "cannot make the Vulkan device's command pool and depth buffer");
        vkDestroyDevice(v->device, NULL);
        snx_vk_window_close(&v->host);
        return false;
    }
    g_device.up = true;
    fprintf(stderr, "nds gpu: %s\n", v->properties.deviceName);
    return true;
}

void nds_vk_free(void) {
    NdsVk* v = &g_device.vk;
    if (!g_device.up) return;
    vkDeviceWaitIdle(v->device);
    snx_vk_ring_free(&v->ring);
    vkDestroyDevice(v->device, NULL);
    snx_vk_window_close(&v->host);
    g_device.up = false;
}

bool nds_vk_buffer(NdsVkBuffer* out, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
    NdsVk* v = &g_device.vk;
    *out = (NdsVkBuffer){.size = size};
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    if (vkCreateBuffer(v->device, &info, NULL, &out->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->device, out->buffer, &req);
    VkMemoryPropertyFlags flags = host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                               : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = snx_vk_memory_type(&v->memory_properties, req.memoryTypeBits, flags);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size,
                                  .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(v->device, &alloc, NULL, &out->memory) != VK_SUCCESS ||
        vkBindBufferMemory(v->device, out->buffer, out->memory, 0) != VK_SUCCESS) {
        nds_vk_buffer_free(out);
        return false;
    }
    if (host_visible) {
        void* map = NULL;
        if (vkMapMemory(v->device, out->memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
            nds_vk_buffer_free(out);
            return false;
        }
        out->map = map;
    }
    return true;
}

void nds_vk_buffer_free(NdsVkBuffer* buffer) {
    NdsVk* v = &g_device.vk;
    if (buffer->buffer) vkDestroyBuffer(v->device, buffer->buffer, NULL);
    if (buffer->memory) vkFreeMemory(v->device, buffer->memory, NULL);
    *buffer = (NdsVkBuffer){0};
}

bool nds_vk_image(NdsVkImage* out, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                  VkImageAspectFlags aspect) {
    NdsVk* v = &g_device.vk;
    *out = (NdsVkImage){.format = format, .width = width, .height = height, .layout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {width, height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = usage};
    if (vkCreateImage(v->device, &info, NULL, &out->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->device, out->image, &req);
    uint32_t type = snx_vk_memory_type(&v->memory_properties, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size,
                                  .memoryTypeIndex = type};
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = format,
                                  .subresourceRange = {aspect, 0, 1, 0, 1}};
    if (type == UINT32_MAX || vkAllocateMemory(v->device, &alloc, NULL, &out->memory) != VK_SUCCESS ||
        vkBindImageMemory(v->device, out->image, out->memory, 0) != VK_SUCCESS) {
        nds_vk_image_free(out);
        return false;
    }
    view.image = out->image;
    if (vkCreateImageView(v->device, &view, NULL, &out->view) != VK_SUCCESS) {
        nds_vk_image_free(out);
        return false;
    }
    return true;
}

void nds_vk_image_free(NdsVkImage* image) {
    NdsVk* v = &g_device.vk;
    if (image->view) vkDestroyImageView(v->device, image->view, NULL);
    if (image->image) vkDestroyImage(v->device, image->image, NULL);
    if (image->memory) vkFreeMemory(v->device, image->memory, NULL);
    *image = (NdsVkImage){0};
}

void nds_vk_layout(VkCommandBuffer cb, NdsVkImage* image, VkImageLayout layout) {
    if (image->layout == layout) return;
    bool depth = image->format == VK_FORMAT_D32_SFLOAT_S8_UINT || image->format == VK_FORMAT_D24_UNORM_S8_UINT;
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = image->layout,
        .newLayout = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->image,
        .subresourceRange = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                             0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier);
    image->layout = layout;
}

void nds_vk_buffer_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0,
                         NULL, 0, NULL);
}
