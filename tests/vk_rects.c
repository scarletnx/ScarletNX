/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen controls' rectangles over a window's image (common/gpu/vk/rects.c), on whatever Vulkan 1.3 device there
   is, with no window: recorded after the transfers that drew the picture and drawn in the rendering that drew it, each
   pixel is the rectangle's colour where one is and the picture's everywhere else, on windows of either order; other
   rectangles in the same slot replace them, more of them too; none draws nothing, nor does a window format it does not
   write, said once. The Khronos layer's errors fail it where the layer is installed. No device is a skip (77) */
#include "common/gpu/vk/device.h"
#include "common/gpu/vk/rects.h"
#include "common/gpu/vk/ring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vk_rects:%d: %s\n", __LINE__, #x);                                                        \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

enum { W = 320, H = 200 };
static struct {
    char said[256];
    unsigned layer_errors;
} g_test;
static void log_line(const char* text) {
    puts(text);
    snprintf(g_test.said, sizeof g_test.said, "%s", text);
}
static VKAPI_ATTR VkBool32 VKAPI_CALL layer_said(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT type,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    (void)type;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_test.layer_errors;
        fprintf(stderr, "vk_rects: validation: %s\n", data->pMessage);
    }
    return VK_FALSE;
}
static bool choose(SnxVkChoice* c, void* user) {
    (void)user;
    VkPhysicalDeviceVulkan13Features want13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .dynamicRendering = VK_TRUE}; /* every 1.3 device has it */
    VkPhysicalDeviceFeatures2 want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &want13};
    return snx_vk_choice_create(c, &want, NULL, NULL, 0);
}

typedef struct {
    VkPhysicalDevice phys;
    VkDevice dev;
    SnxVkRing ring;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    unsigned char* got;
} Made;
static bool memory_for(const Made* m, VkMemoryRequirements req, VkMemoryPropertyFlags want, VkDeviceMemory* out) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(m->phys, &mp);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
    ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, want);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, 0);
    return ai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(m->dev, &ai, NULL, out) == VK_SUCCESS;
}
static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout old, VkImageLayout next) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = old,
                              .newLayout = next,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
}

/* rects over a green picture on a window of format, recorded after its transfers, or drawn in its rendering: the pixels
   that are neither the rectangles' nor the picture's; -1 when it could not be made. *drawn as the call says */
static long wrong_pixels(Made* m, SnxVkRects* vr, VkFormat format, bool record, const SnxUiRects* rects, int* drawn) {
    static uint32_t want[H][W];
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {W, H, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage win;
    VkImageView view;
    VkDeviceMemory memory;
    VkMemoryRequirements req;
    if (vkCreateImage(m->dev, &ii, NULL, &win) != VK_SUCCESS) return -1;
    vkGetImageMemoryRequirements(m->dev, win, &req);
    if (!memory_for(m, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory) ||
        vkBindImageMemory(m->dev, win, memory, 0) != VK_SUCCESS)
        return -1;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = win,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(m->dev, &vi, NULL, &view) != VK_SUCCESS) return -1;
    VkClearColorValue green = {{0, 1, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkCommandBuffer cb = snx_vk_ring_commands(&m->ring);
    if (record) {
        barrier(cb, win, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb, win, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &green, 1, &all);
        *drawn = snx_vk_rects_record(vr, cb, m->ring.slot, win, view, format, W, H, rects, log_line);
        barrier(cb, win, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    } else {
        VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                           .imageView = view,
                                           .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                           .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                           .clearValue = {.color = green}};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {{0, 0}, {W, H}},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &color};
        barrier(cb, win, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vkCmdBeginRendering(cb, &ri);
        *drawn = snx_vk_rects_draw(vr, cb, m->ring.slot, format, W, H, rects, log_line);
        vkCmdEndRendering(cb);
        barrier(cb, win, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
    vkCmdCopyImageToBuffer(cb, win, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m->readback, 1, &copy);
    if (!snx_vk_ring_wait(&m->ring, snx_vk_ring_submit(&m->ring, VK_NULL_HANDLE, VK_NULL_HANDLE), UINT64_MAX))
        return -1;
    long bad = 0;
    int bgra = format == VK_FORMAT_B8G8R8A8_UNORM;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) want[y][x] = 0xFF00FF00u;
    for (size_t i = 0; rects && i < rects->count; ++i)
        for (int y = rects->rect[i].y; y < rects->rect[i].y + rects->rect[i].h; ++y)
            for (int x = rects->rect[i].x; x < rects->rect[i].x + rects->rect[i].w; ++x)
                want[y][x] = rects->rect[i].rgba;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const unsigned char* p = m->got + ((size_t)y * W + (size_t)x) * 4u;
            uint32_t pixel = (uint32_t)p[bgra ? 2 : 0] | (uint32_t)p[1] << 8 | (uint32_t)p[bgra ? 0 : 2] << 16 |
                             (uint32_t)p[3] << 24;
            bad += pixel != want[y][x];
        }
    vkDestroyImageView(m->dev, view, NULL);
    vkDestroyImage(m->dev, win, NULL);
    vkFreeMemory(m->dev, memory, NULL);
    return bad;
}

int main(void) {
    static SnxUiQuad quads[64];
    SnxVkInstanceInfo info = {.app_name = "snx-test-vk-rects", .validation = true, .debug = layer_said};
    SnxVkInstance instance;
    Made m = {0};
    if (volkInitialize() != VK_SUCCESS || snx_vk_instance_create(&info, &instance) != VK_SUCCESS) {
        printf("vk_rects: no Vulkan loader or instance: skipped\n");
        return 77;
    }
    SnxVkDeviceRequest request = {.who = "vk_rects",
                                  .none = "no Vulkan 1.3 device with a graphics queue",
                                  .instance = instance.instance,
                                  .api = VK_API_VERSION_1_3,
                                  .queues = VK_QUEUE_GRAPHICS_BIT,
                                  .choose = choose};
    SnxVkDevice made;
    char why[1024];
    if (!snx_vk_device_make(&request, &made, why, sizeof why)) {
        printf("vk_rects: %s: %s\n", why, made.passed_over ? "skipped" : "failed");
        snx_vk_instance_free(&instance);
        return made.passed_over ? 77 : 1;
    }
    m.phys = made.phys;
    m.dev = made.device;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(m.phys, &properties);
    printf("vk_rects: on %s, the Khronos layer %s\n", properties.deviceName,
           instance.validation ? "on" : "not installed");
    VkQueue queue;
    vkGetDeviceQueue(m.dev, made.family, 0, &queue);
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dp = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    CHECK(snx_vk_ring_init(&m.ring, m.dev, queue, made.family, 2, &dp, false));
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (VkDeviceSize)W * H * 4u,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkMemoryRequirements req;
    CHECK(vkCreateBuffer(m.dev, &bi, NULL, &m.readback) == VK_SUCCESS);
    vkGetBufferMemoryRequirements(m.dev, m.readback, &req);
    CHECK(memory_for(&m, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &m.readback_memory));
    CHECK(vkBindBufferMemory(m.dev, m.readback, m.readback_memory, 0) == VK_SUCCESS);
    CHECK(vkMapMemory(m.dev, m.readback_memory, 0, VK_WHOLE_SIZE, 0, (void**)&m.got) == VK_SUCCESS);

    SnxVkRects* vr = snx_vk_rects_create(m.phys, m.dev, 2);
    SnxUiRects rects = {0};
    CHECK(vr);
    for (int k = 0; k < 16; ++k) { /* each slot in turn, each way on each format, few rectangles then many */
        VkFormat format = k & 1 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
        bool record = k & 2;
        SnxUiList l = {quads, 0, 64};
        int drawn = -1;
        ui_shape(&l, 20, 10, 120, 120, UI_SHAPE_CIRCLE, "A", k & 4 ? UI_SHAPE_FILLED : 0, 3, 0xFFFFFFFFu);
        if (k >= 8)
            for (int i = 0; i < 12; ++i)
                ui_shape(&l, (float)(5 + 25 * i), (float)(60 + 8 * i), 70, 60, UI_SHAPE_CIRCLE, "", 0, 1 + i,
                         0xFF0000FFu);
        ui_text(&l, 200, 4, 2, "Done", 0xFFFF0000u);
        CHECK(snx_ui_rects(&rects, &l, W, H));
        long bad = wrong_pixels(&m, vr, format, record, &rects, &drawn);
        printf("vk_rects: %zu rectangles %s a %s window: %ld pixels wrong\n", rects.count,
               record ? "recorded over" : "drawn in the rendering of", k & 1 ? "RGBA" : "BGRA", bad);
        CHECK(!bad && drawn == 1);
    }
    int drawn = -1;
    SnxUiRects empty = {0};
    CHECK(wrong_pixels(&m, vr, VK_FORMAT_B8G8R8A8_UNORM, true, &empty, &drawn) == 0 && !drawn);
    CHECK(wrong_pixels(&m, vr, VK_FORMAT_B8G8R8A8_UNORM, false, NULL, &drawn) == 0 && !drawn);
    g_test.said[0] = 0;
    CHECK(!snx_vk_rects_draw(vr, VK_NULL_HANDLE, 0, VK_FORMAT_R8G8B8A8_SRGB, W, H, &rects, log_line));
    CHECK(strstr(g_test.said, "format 43") && strstr(g_test.said, "only on 8-bit UNORM RGBA or BGRA"));
    g_test.said[0] = 0;
    CHECK(!snx_vk_rects_draw(vr, VK_NULL_HANDLE, 0, VK_FORMAT_R8G8B8A8_SRGB, W, H, &rects, log_line));
    CHECK(!g_test.said[0]); /* once */
    vkDeviceWaitIdle(m.dev);
    snx_vk_rects_destroy(vr);
    snx_ui_rects_free(&rects);
    vkDestroyBuffer(m.dev, m.readback, NULL);
    vkFreeMemory(m.dev, m.readback_memory, NULL);
    snx_vk_ring_free(&m.ring);
    vkDestroyDevice(m.dev, NULL);
    snx_vk_instance_free(&instance);
    CHECK(!g_test.layer_errors);
    puts("vk_rects: passed");
    return 0;
}
