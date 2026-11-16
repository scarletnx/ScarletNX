/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS's and the Vita's notice copied over a window's image (common/gpu/vk/notice.c), on whatever Vulkan device
   there is, with no window: after the picture's own transfers, the band's pixels where snx_notice_rect puts it, in the
   window's byte order, and the picture's everywhere else; a slot's buffer grown for a longer text; nothing drawn once
   its time is up, nor on a window format it does not write, said once. The Khronos layer's errors fail it where the
   layer is installed. None is a skip (77) */
#include "common/gpu/vk/notice.h"
#include "common/gpu/vk/ring.h"
#include "common/gpu/vk/device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vk_notice:%d: %s\n", __LINE__, #x);                                                       \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

typedef struct {
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t family;
} Made;

/* the notice's log, its last line kept for the checks of what it says; the layer's errors counted */
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
        fprintf(stderr, "vk_notice: validation: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

/* the device, with no feature asked for (common/gpu/vk/device.h) */
static bool choose(SnxVkChoice* c, void* user) {
    (void)user;
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    return snx_vk_choice_create(c, &features, NULL, NULL, 0);
}

/* memory of the properties for req, bound to image or buffer */
static bool bind(const Made* m, VkMemoryRequirements req, VkMemoryPropertyFlags want, VkImage image, VkBuffer buffer,
                 VkDeviceMemory* memory) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(m->phys, &mp);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
    ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, want);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, 0);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m->dev, &ai, NULL, memory) != VK_SUCCESS) return false;
    return image ? vkBindImageMemory(m->dev, image, *memory, 0) == VK_SUCCESS
                 : vkBindBufferMemory(m->dev, buffer, *memory, 0) == VK_SUCCESS;
}

static void layout(VkCommandBuffer cb, VkImage image, VkImageLayout old, VkImageLayout next, VkAccessFlags src,
                   VkAccessFlags dst) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = src,
                              .dstAccessMask = dst,
                              .oldLayout = old,
                              .newLayout = next,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &b);
}

int main(void) {
    static const VkFormat formats[2] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    static const char* const texts[2] = {"Saved to slot 2", "Press Esc again within 3 seconds to quit"};
    enum { WW = 640, WH = 360 };
    const unsigned char ground[3] = {0, 255, 0}; /* the same bytes in an sRGB image */
    SnxVkInstanceInfo info = {.app_name = "snx-test-vk-notice", .validation = true, .debug = layer_said};
    SnxVkInstance instance;
    Made m = {0};
    if (volkInitialize() != VK_SUCCESS) {
        printf("vk_notice: no Vulkan loader: skipped\n");
        return 77;
    }
    if (snx_vk_instance_create(&info, &instance) != VK_SUCCESS) {
        printf("vk_notice: no Vulkan instance: skipped\n");
        return 77;
    }
    SnxVkDeviceRequest request = {.who = "vk_notice",
                                  .none = "no Vulkan 1.2 device with a graphics queue",
                                  .instance = instance.instance,
                                  .api = VK_API_VERSION_1_2,
                                  .queues = VK_QUEUE_GRAPHICS_BIT,
                                  .choose = choose};
    SnxVkDevice made;
    char why[1024];
    if (!snx_vk_device_make(&request, &made, why, sizeof why)) {
        printf("vk_notice: %s: %s\n", why, made.passed_over ? "skipped" : "failed");
        snx_vk_instance_free(&instance);
        return made.passed_over ? 77 : 1;
    }
    m.phys = made.phys;
    m.dev = made.device;
    m.family = made.family;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(m.phys, &properties);
    printf("vk_notice: on %s, the Khronos layer %s\n", properties.deviceName,
           instance.validation ? "on" : "not installed");
    VkQueue queue;
    vkGetDeviceQueue(m.dev, m.family, 0, &queue);
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 1,
                                     .poolSizeCount = 1,
                                     .pPoolSizes = &pool_size};
    SnxVkRing ring;
    CHECK(snx_vk_ring_init(&ring, m.dev, queue, m.family, 2, &dp, false));
    /* the readback: the window's pixels, a host buffer */
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (VkDeviceSize)WW * WH * 4u,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    VkMemoryRequirements req;
    unsigned char* got;
    CHECK(vkCreateBuffer(m.dev, &bi, NULL, &readback) == VK_SUCCESS);
    vkGetBufferMemoryRequirements(m.dev, readback, &req);
    CHECK(bind(&m, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_NULL_HANDLE,
               readback, &readback_memory));
    CHECK(vkMapMemory(m.dev, readback_memory, 0, VK_WHOLE_SIZE, 0, (void**)&got) == VK_SUCCESS);

    SnxVkNotice* vn = snx_vk_notice_create(m.phys, m.dev, 2);
    SnxNotice n = {0};
    unsigned char* band = malloc((size_t)WW * WH * 4);
    CHECK(vn && band);
    /* the short text, then the long one in the same slot (its buffer grown), then the short one past its time */
    for (int k = 0; k < 4; ++k) {
        VkFormat format = formats[k & 1];
        int bgra = format == VK_FORMAT_B8G8R8A8_UNORM, rect[4], scale = snx_notice_scale(WW, WH, 100), drawn;
        uint64_t now = k < 3 ? 0 : (uint64_t)SNX_NOTICE_MS * 1000000u;
        unsigned bad = 0, inside = 0;
        VkClearColorValue clear = {.float32 = {0}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = format,
                                .extent = {WW, WH, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VkImage win;
        VkDeviceMemory win_memory;
        snx_notice_show(&n, texts[k == 2], 0);
        for (int c = 0; c < 3; ++c) clear.float32[c] = (float)ground[c] / 255.0f;
        clear.float32[3] = 1.0f;
        CHECK(vkCreateImage(m.dev, &ii, NULL, &win) == VK_SUCCESS);
        vkGetImageMemoryRequirements(m.dev, win, &req);
        CHECK(bind(&m, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, win, VK_NULL_HANDLE, &win_memory));
        VkCommandBuffer cb = snx_vk_ring_commands(&ring);
        layout(cb, win, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(cb, win, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &all);
        drawn = snx_vk_notice_record(vn, cb, ring.slot, win, format, WW, WH, 100, &n, now, log_line);
        layout(cb, win, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {WW, WH, 1}};
        vkCmdCopyImageToBuffer(cb, win, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        CHECK(snx_vk_ring_wait(&ring, snx_vk_ring_submit(&ring, VK_NULL_HANDLE, VK_NULL_HANDLE), UINT64_MAX));
        CHECK(drawn == (k < 3));
        CHECK(snx_notice_rect(&n, WW, WH, scale, rect));
        snx_notice_pixels(&n, scale, bgra, rect, band, (size_t)rect[2] * 4u);
        for (int y = 0; y < WH; ++y)
            for (int x = 0; x < WW; ++x) {
                const unsigned char* p = got + ((size_t)y * WW + (size_t)x) * 4u;
                int in = drawn && x >= rect[0] && x < rect[0] + rect[2] && y >= rect[1] && y < rect[1] + rect[3];
                inside += in;
                if (in)
                    bad += memcmp(p, band + ((size_t)(y - rect[1]) * (size_t)rect[2] + (size_t)(x - rect[0])) * 4u,
                                  4) != 0;
                else
                    bad += abs((int)p[bgra ? 2 : 0] - ground[0]) > 1 || abs((int)p[1] - ground[1]) > 1 ||
                           abs((int)p[bgra ? 0 : 2] - ground[2]) > 1;
            }
        printf("vk_notice: \"%s\" over a %ux%u %s window%s: %u pixels of its band, %u wrong\n", n.text, WW, WH,
               bgra ? "BGRA" : "RGBA sRGB", k == 3 ? " past its time" : "", inside, bad);
        CHECK(!bad && (k == 3 || inside == (unsigned)(rect[2] * rect[3])));
        vkDestroyImage(m.dev, win, NULL);
        vkFreeMemory(m.dev, win_memory, NULL);
    }
    /* a window format it does not write: nothing recorded, said once */
    snx_notice_show(&n, texts[0], 0);
    g_test.said[0] = 0;
    CHECK(!snx_vk_notice_record(vn, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_FORMAT_A2B10G10R10_UNORM_PACK32, WW, WH, 100,
                                &n, 0, log_line));
    CHECK(strstr(g_test.said, "format 64") && strstr(g_test.said, "only on 8-bit RGBA or BGRA"));
    g_test.said[0] = 0;
    CHECK(!snx_vk_notice_record(vn, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_FORMAT_A2B10G10R10_UNORM_PACK32, WW, WH, 100,
                                &n, 0, log_line));
    CHECK(!g_test.said[0]); /* once */
    free(band);
    snx_vk_notice_destroy(vn);
    vkUnmapMemory(m.dev, readback_memory);
    vkDestroyBuffer(m.dev, readback, NULL);
    vkFreeMemory(m.dev, readback_memory, NULL);
    snx_vk_ring_free(&ring);
    vkDestroyDevice(m.dev, NULL);
    snx_vk_instance_free(&instance);
    CHECK(!g_test.layer_errors);
    printf("vk_notice: passed\n");
    return 0;
}
