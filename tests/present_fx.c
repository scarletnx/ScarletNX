/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shared presentation effects on a real Vulkan device, with no game data and no window: a synthetic frame through
   FSR 1, FXAA, SMAA and SSAA, and the choices that turn them on. Read back and checked.
   --showcase=DIR writes the frames as PPM panels. */
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/ring.h"
#include "common/gpu/vk/vk_common.h"
#include "support/fxaa_ref.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "present_fx:%d: %s\n", __LINE__, #x);                                                      \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

typedef struct {
    SnxVkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    SnxVkRing ring;
} Test;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t width, height;
} Image;

static int succeed(VkResult r) { return r == VK_SUCCESS; }

/* the test's state: the effects' last line, for the checks of what they say, and the Khronos layer's errors */
static struct {
    char said[256];
    unsigned val_errors;
} g_test;

/* the effects' log: printed, and the last line kept */
static void log_line(const char* text) {
    puts(text);
    snprintf(g_test.said, sizeof g_test.said, "%s", text);
}

/* --validation: the Khronos layer's errors fail the test. The SSAA pass once updated a descriptor set while the
   command buffer that bound it was recording, which the layer names and a plain run does not */
static VKAPI_ATTR VkBool32 VKAPI_CALL val_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT type,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    (void)type;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_test.val_errors;
        fprintf(stderr, "present_fx validation: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

static bool make_device(VkPhysicalDevice phys, uint32_t family, void* user) {
    Test* t = user;
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void* const query[] = {&features, &v12, &v13};
    snx_vk_chain(query, sizeof query / sizeof *query);
    vkGetPhysicalDeviceFeatures2(phys, &features);
    if (!v13.dynamicRendering || !v13.synchronization2) return 0;
    VkPhysicalDeviceVulkan13Features want13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .dynamicRendering = VK_TRUE,
                                               .synchronization2 = VK_TRUE};
    VkPhysicalDeviceVulkan12Features want12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                               .shaderFloat16 = v12.shaderFloat16};
    VkPhysicalDeviceFeatures2 want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    want.features.samplerAnisotropy = features.features.samplerAnisotropy;
    void* const enable[] = {&want, &want12, &want13};
    snx_vk_chain(enable, sizeof enable / sizeof *enable);
    if (!succeed(snx_vk_device_create(phys, family, &want, NULL, 0, &t->dev))) return 0;
    t->phys = phys;
    t->family = family;
    return 1;
}

static int image_make(Test* t, Image* im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage};
    if (!succeed(vkCreateImage(t->dev, &ii, NULL, &im->image))) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(t->dev, im->image, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
    ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, 0);
    if (!succeed(vkAllocateMemory(t->dev, &ai, NULL, &im->memory)) ||
        !succeed(vkBindImageMemory(t->dev, im->image, im->memory, 0)))
        return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = im->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!succeed(vkCreateImageView(t->dev, &vi, NULL, &im->view))) return 0;
    im->width = w;
    im->height = h;
    return 1;
}

static void image_free(Test* t, Image* im) {
    if (im->view) vkDestroyImageView(t->dev, im->view, NULL);
    if (im->image) vkDestroyImage(t->dev, im->image, NULL);
    if (im->memory) vkFreeMemory(t->dev, im->memory, NULL);
    *im = (Image){0};
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
}

/* a pattern with a hard vertical step, a diagonal edge and a gradient: the aliasing the filters work on */
static void pattern(unsigned char* p, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            unsigned char* at = p + ((size_t)y * w + x) * 4;
            int dx = (int)x - (int)w / 2, dy = (int)y - (int)h / 2;
            at[0] = (unsigned char)(x * 200 / (w ? w : 1) + 24);
            at[1] = (unsigned char)(y * 180 / (h ? h : 1) + 20);
            at[2] = (unsigned char)(64 + ((x / 8 + y / 8) & 1) * 80);
            at[3] = 255;
            if (dx * dx + dy * dy < (int)(w / 5) * (int)(w / 5)) {
                at[0] = 236;
                at[1] = 232;
                at[2] = 220;
            }
            if (x > w * 2 / 3 && x < w * 2 / 3 + 3) {
                at[0] = 248;
                at[1] = 248;
                at[2] = 248;
            }
            if (x > 12 && x < 15) {
                at[0] = 8;
                at[1] = 8;
                at[2] = 8;
            }
        }
}

/* a step and a diagonal, the edges the post process filters smooth */
static void step_pattern(unsigned char* p, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            unsigned char* at = p + ((size_t)y * w + x) * 4;
            at[0] = x < w / 2 ? 16 : 240;
            at[1] = y < h / 2 ? 20 : 220;
            at[2] = 64;
            at[3] = 255;
            if (x > w / 3 && x < w / 3 + 2) at[0] = 250;
            if (y > h / 3 && y < h / 3 + 2) at[1] = 250;
        }
}

static int upload(Test* t, Image* im, const unsigned char* pixels) {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped;
    VkBufferImageCopy copy;
    VkCommandBuffer cb = snx_vk_ring_commands(&t->ring);
    uint64_t serial;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (VkDeviceSize)im->width * im->height * 4,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkPhysicalDeviceMemoryProperties mp;
    VkMemoryRequirements req;
    if (!succeed(vkCreateBuffer(t->dev, &bi, NULL, &buffer))) return 0;
    vkGetBufferMemoryRequirements(t->dev, buffer, &req);
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    {
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
        ai.memoryTypeIndex = snx_vk_memory_type(
            &mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!succeed(vkAllocateMemory(t->dev, &ai, NULL, &memory)) ||
            !succeed(vkBindBufferMemory(t->dev, buffer, memory, 0)) ||
            !succeed(vkMapMemory(t->dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped)))
            return 0;
    }
    memcpy(mapped, pixels, (size_t)im->width * im->height * 4);
    copy = (VkBufferImageCopy){.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {im->width, im->height, 1}};
    barrier(cb, im->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cb, buffer, im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cb, im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    serial = snx_vk_ring_submit(&t->ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
    (void)snx_vk_ring_wait(&t->ring, serial, UINT64_MAX);
    vkUnmapMemory(t->dev, memory);
    vkDestroyBuffer(t->dev, buffer, NULL);
    vkFreeMemory(t->dev, memory, NULL);
    return 1;
}

/* the image as RGBA8, allocating the readback each time (a test, not a runtime path) */
static unsigned char* read_image(Test* t, VkImage image, VkImageLayout layout, uint32_t w, uint32_t h) {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped;
    unsigned char* out = malloc((size_t)w * h * 4);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (VkDeviceSize)w * h * 4,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkPhysicalDeviceMemoryProperties mp;
    VkMemoryRequirements req;
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    VkCommandBuffer cb;
    uint64_t serial;
    if (!out) return NULL;
    if (!succeed(vkCreateBuffer(t->dev, &bi, NULL, &buffer))) return NULL;
    vkGetBufferMemoryRequirements(t->dev, buffer, &req);
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    {
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
        ai.memoryTypeIndex = snx_vk_memory_type(
            &mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!succeed(vkAllocateMemory(t->dev, &ai, NULL, &memory)) ||
            !succeed(vkBindBufferMemory(t->dev, buffer, memory, 0)) ||
            !succeed(vkMapMemory(t->dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped)))
            return NULL;
    }
    cb = snx_vk_ring_commands(&t->ring);
    barrier(cb, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    serial = snx_vk_ring_submit(&t->ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
    (void)snx_vk_ring_wait(&t->ring, serial, UINT64_MAX);
    memcpy(out, mapped, (size_t)w * h * 4);
    vkUnmapMemory(t->dev, memory);
    vkDestroyBuffer(t->dev, buffer, NULL);
    vkFreeMemory(t->dev, memory, NULL);
    return out;
}

/* the effects' images are R16G16B16A16_SFLOAT: read them back as 8-bit linear values */
static double half1(unsigned short h) {
    int e = (h >> 10) & 0x1F, m = h & 0x3FF, sign = h >> 15;
    double v;
    if (!e)
        v = m / 1024.0 * pow(2.0, -14.0);
    else if (e == 31)
        v = m ? NAN : INFINITY;
    else
        v = (1.0 + m / 1024.0) * pow(2.0, e - 15.0);
    return sign ? -v : v;
}
static unsigned char* read_image_f16(Test* t, VkImage image, VkImageLayout layout, uint32_t w, uint32_t h) {
    size_t n = (size_t)w * h * 8;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped;
    unsigned char* out = malloc((size_t)w * h * 4);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (VkDeviceSize)n,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkPhysicalDeviceMemoryProperties mp;
    VkMemoryRequirements req;
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    VkCommandBuffer cb;
    uint64_t serial;
    if (!out) return NULL;
    if (!succeed(vkCreateBuffer(t->dev, &bi, NULL, &buffer))) return NULL;
    vkGetBufferMemoryRequirements(t->dev, buffer, &req);
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    {
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
        ai.memoryTypeIndex = snx_vk_memory_type(
            &mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!succeed(vkAllocateMemory(t->dev, &ai, NULL, &memory)) ||
            !succeed(vkBindBufferMemory(t->dev, buffer, memory, 0)) ||
            !succeed(vkMapMemory(t->dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped)))
            return NULL;
    }
    cb = snx_vk_ring_commands(&t->ring);
    barrier(cb, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    serial = snx_vk_ring_submit(&t->ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
    (void)snx_vk_ring_wait(&t->ring, serial, UINT64_MAX);
    {
        const unsigned short* src = mapped;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            for (int c = 0; c < 3; ++c) {
                double v = half1(src[i * 4 + c]);
                out[i * 4 + c] = v <= 0.0 ? 0 : v >= 1.0 ? 255 : (unsigned char)(v * 255.0 + 0.5);
            }
            out[i * 4 + 3] = 255;
        }
    }
    vkUnmapMemory(t->dev, memory);
    vkDestroyBuffer(t->dev, buffer, NULL);
    vkFreeMemory(t->dev, memory, NULL);
    return out;
}

static void write_ppm(const char* dir, const char* name, const unsigned char* rgba, uint32_t w, uint32_t h) {
    char path[1024];
    FILE* f;
    if (!dir) return;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t i = 0; i < w * h; ++i) fwrite(rgba + (size_t)i * 4, 1, 3, f);
    fclose(f);
}

/* the luma of a pixel, and the largest step between neighbours: FXAA and SMAA lower the step at an edge */
static double avg_step(const unsigned char* p, uint32_t w, uint32_t h) {
    double sum = 0;
    uint64_t n = 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x + 1 < w; ++x) {
            const unsigned char* a = p + ((size_t)y * w + x) * 4;
            const unsigned char* b = a + 4;
            sum += (double)abs((int)a[0] - (int)b[0]) + abs((int)a[1] - (int)b[1]) + abs((int)a[2] - (int)b[2]);
            ++n;
        }
    return n ? sum / (double)n : 0;
}

static int max_step(const unsigned char* p, uint32_t w, uint32_t h, int* max_seen) {
    int max = 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x + 1 < w; ++x) {
            const unsigned char* a = p + ((size_t)y * w + x) * 4;
            const unsigned char* b = a + 4;
            int d = abs((int)a[0] - (int)b[0]) + abs((int)a[1] - (int)b[1]) + abs((int)a[2] - (int)b[2]);
            if (d > max) max = d;
        }
    if (max_seen) *max_seen = max;
    return max;
}

/* a P6 PPM into RGBA8 (*w by *h), NULL when it is not one */
static unsigned char* read_ppm(const char* path, uint32_t* w, uint32_t* h) {
    FILE* f = fopen(path, "rb");
    unsigned max = 0;
    unsigned char* out = NULL;
    if (!f) return NULL;
    if (fscanf(f, "P6 %u %u %u", w, h, &max) == 3 && max == 255 && fgetc(f) != EOF && *w && *h && *w <= 8192 &&
        *h <= 8192 && (out = malloc((size_t)*w * *h * 4))) {
        for (size_t i = 0; i < (size_t)*w * *h; ++i) {
            if (fread(out + i * 4, 1, 3, f) != 3) {
                free(out);
                out = NULL;
                break;
            }
            out[i * 4 + 3] = 255;
        }
    }
    fclose(f);
    return out;
}

/* --bench [--frame=PPM] [--showcase=DIR]: each pass's GPU time on the device (timestamps around snx_fx_render), the
   passes taken in turn 100 times after 5 warm rounds, so the device's clock moves them alike, and the median of each:
   a 1280x720 frame (or the PPM's) shown at 1920x1080 and at 3840x2160, and a 400x240 one at 1600x960 scaled in sharp
   pixels with the upscaler Off, as the 3DS's window does, the effects' output of each written to DIR. A measure, not a
   check: run it under lock.sh gpu-measure */
#define BENCH_ROUNDS 105
static int bench(Test* t, SnxFx* fx, const char* frame, const char* showcase) {
    static const struct {
        int filter, aa;
        const char* name;
    } runs[] = {
        {SNX_FX_UPSCALE_OFF, SNX_FX_AA_FXAA, "fxaa"},       {SNX_FX_UPSCALE_BILINEAR, SNX_FX_AA_FXAA, "bilinear-fxaa"},
        {SNX_FX_UPSCALE_OFF, SNX_FX_AA_SMAA, "smaa"},       {SNX_FX_UPSCALE_FSR1, SNX_FX_AA_OFF, "fsr1"},
        {SNX_FX_UPSCALE_FSR1, SNX_FX_AA_FXAA, "fsr1-fxaa"}, {SNX_FX_UPSCALE_NIS, SNX_FX_AA_OFF, "nis"},
        {SNX_FX_UPSCALE_SGSR, SNX_FX_AA_OFF, "sgsr"},       {SNX_FX_UPSCALE_OFF, SNX_FX_AA_SSAA2, "ssaa2"}};
    enum { RUNS = sizeof runs / sizeof *runs };
    static const uint32_t sizes[3][4] = {{1280, 720, 1920, 1080}, {1280, 720, 3840, 2160}, {400, 240, 1600, 960}};
    static double ms[RUNS][BENCH_ROUNDS];
    VkPhysicalDeviceProperties pp;
    VkQueryPoolCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
    VkQueryPool pool;
    vkGetPhysicalDeviceProperties(t->phys, &pp);
    CHECK(succeed(vkCreateQueryPool(t->dev, &qi, NULL, &pool)));
    printf("bench: %s, timestamps of %.3f ns\n", pp.deviceName, (double)pp.limits.timestampPeriod);
    for (int size = 0; size < 3; ++size) {
        uint32_t w = sizes[size][0], h = sizes[size][1], ow = sizes[size][2], oh = sizes[size][3];
        unsigned char* p = size < 2 && frame ? read_ppm(frame, &w, &h) : malloc((size_t)w * h * 4);
        Image src = {0};
        CHECK(p);
        if (size == 2 || !frame) pattern(p, w, h);
        CHECK(image_make(t, &src, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        CHECK(upload(t, &src, p));
        free(p);
        snx_fx_set_off_sharp(fx, size == 2);
        for (int i = 0; i < BENCH_ROUNDS; ++i)
            for (int r = 0; r < RUNS; ++r) {
                SnxFxImage out = {0};
                uint64_t stamps[2], serial;
                VkCommandBuffer cb = snx_vk_ring_commands(&t->ring);
                vkCmdResetQueryPool(cb, pool, 0, 2);
                vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, 0);
                CHECK(snx_fx_render(fx, cb, (uint32_t)r, src.view, w, h, 0, 0, w, h, 0, ow, oh, runs[r].filter,
                                    runs[r].aa, &out));
                vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, 1);
                serial = snx_vk_ring_submit(&t->ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
                CHECK(snx_vk_ring_wait(&t->ring, serial, UINT64_MAX));
                CHECK(succeed(vkGetQueryPoolResults(t->dev, pool, 0, 2, sizeof stamps, stamps, sizeof *stamps,
                                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)));
                ms[r][i] = (double)(stamps[1] - stamps[0]) * pp.limits.timestampPeriod / 1e6;
                if (i == BENCH_ROUNDS - 1 && showcase && size != 1) {
                    char name[64];
                    unsigned char* got =
                        read_image_f16(t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.width, out.height);
                    snprintf(name, sizeof name, "bench-%ux%u-%s", w, h, runs[r].name);
                    if (got) write_ppm(showcase, name, got, out.width, out.height);
                    free(got);
                }
            }
        for (int r = 0; r < RUNS; ++r) {
            double* m = ms[r] + 5; /* the warm rounds left out */
            int n = BENCH_ROUNDS - 5;
            for (int a = 1; a < n; ++a) /* sorted, for the median */
                for (int b = a; b > 0 && m[b] < m[b - 1]; --b) {
                    double x = m[b];
                    m[b] = m[b - 1];
                    m[b - 1] = x;
                }
            printf("bench: %ux%u to %ux%u %-14s median %.3f ms (%.3f to %.3f)\n", w, h, ow, oh, runs[r].name, m[n / 2],
                   m[0], m[n - 1]);
        }
        image_free(t, &src);
    }
    snx_fx_set_off_sharp(fx, 0);
    vkDestroyQueryPool(t->dev, pool, NULL);
    return 0;
}

int main(int argc, char** argv) {
    const char* showcase = NULL;
    Test t = {0};
    Image src = {0}, step = {0};
    unsigned char *pixels, *step_pixels;
    SnxFx* fx;
    for (int i = 1; i < argc; ++i)
        if (!strncmp(argv[i], "--showcase=", 11)) showcase = argv[i] + 11;
    {
        int validation = 0;
        for (int i = 1; i < argc; ++i)
            if (!strcmp(argv[i], "--validation")) validation = 1;
        SnxVkInstanceInfo info = {.app_name = "present_fx", .validation = validation != 0, .debug = val_cb};
        if (volkInitialize() != VK_SUCCESS) {
            fprintf(stderr, "present_fx: no Vulkan loader\n");
            return 77;
        }
        if (!succeed(snx_vk_instance_create(&info, &t.instance))) {
            fprintf(stderr, "present_fx: no Vulkan instance\n");
            return 77;
        }
    }
    if (!snx_vk_first_device(t.instance.instance, VK_API_VERSION_1_3, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
                             VK_NULL_HANDLE, make_device, &t)) {
        fprintf(stderr, "present_fx: no Vulkan device\n");
        return 77;
    }
    volkLoadDevice(t.dev);
    vkGetDeviceQueue(t.dev, t.family, 0, &t.queue);
    {
        VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32};
        VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                         .maxSets = 16,
                                         .poolSizeCount = 1,
                                         .pPoolSizes = &size};
        CHECK(snx_vk_ring_init(&t.ring, t.dev, t.queue, t.family, 2, &dp));
    }
    CHECK(image_make(&t, &src, 320, 240, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
    pixels = malloc((size_t)320 * 240 * 4);
    CHECK(pixels);
    pattern(pixels, 320, 240);
    CHECK(upload(&t, &src, pixels));
    CHECK(image_make(&t, &step, 320, 240, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
    step_pixels = malloc((size_t)320 * 240 * 4);
    CHECK(step_pixels);
    step_pattern(step_pixels, 320, 240);
    CHECK(upload(&t, &step, step_pixels));

    /* the pure choices */
    CHECK(snx_fx_aa_auto(320, 240, 640, 480));
    CHECK(!snx_fx_aa_auto(320, 240, 380, 285));
    CHECK(snx_fx_aa_auto(320, 240, 400, 300));
    CHECK(!snx_fx_aa_auto(400, 300, 320, 240));
    CHECK(snx_fx_upscale_parse("fsr1") == SNX_FX_UPSCALE_FSR1 && snx_fx_upscale_parse("NIS") == SNX_FX_UPSCALE_NIS);
    CHECK(snx_fx_upscale_parse("none") == -1 && snx_fx_upscale_parse("") == -1 && snx_fx_upscale_parse(NULL) == -1);
    CHECK(snx_fx_aa_parse("smaa") == SNX_FX_AA_SMAA && snx_fx_aa_parse("ssaa2") == SNX_FX_AA_SSAA2);
    CHECK(snx_fx_aa_parse("ssaa 2x") == -1);
    CHECK(!strcmp(snx_fx_upscale_key(SNX_FX_UPSCALE_SGSR), "sgsr") && !snx_fx_upscale_key(SNX_FX_UPSCALE_COUNT));
    CHECK(!strcmp(snx_fx_aa_label(SNX_FX_AA_SSAA4), "SSAA 4x") && !snx_fx_aa_label(-1));
    /* the shortcuts step in the settings' order, round */
    CHECK(snx_fx_upscale_next(SNX_FX_UPSCALE_OFF) == SNX_FX_UPSCALE_BILINEAR &&
          snx_fx_upscale_next(SNX_FX_UPSCALE_FSR1) == SNX_FX_UPSCALE_NIS &&
          snx_fx_upscale_next(SNX_FX_UPSCALE_SGSR) == SNX_FX_UPSCALE_OFF);
    CHECK(snx_fx_aa_next(SNX_FX_AA_OFF) == SNX_FX_AA_AUTO && snx_fx_aa_next(SNX_FX_AA_SSAA4) == SNX_FX_AA_OFF);
    /* the settings as the .ini gives them: a word none of the setting's is said, with Off, which runs for it */
    {
        SnxFxConfig cfg;
        char said[256];
        CHECK(!snx_fx_settings(&cfg, "fsr1", 80, "smaa", said, sizeof said) && !said[0]);
        CHECK(cfg.upscaler == SNX_FX_UPSCALE_FSR1 && cfg.sharpness == 80 && cfg.aa == SNX_FX_AA_SMAA);
        CHECK(snx_fx_settings(&cfg, "fsr", 120, "msaa", said, sizeof said) == 2);
        CHECK(cfg.upscaler == SNX_FX_UPSCALE_OFF && cfg.sharpness == 100 && cfg.aa == SNX_FX_AA_OFF);
        CHECK(!strcmp(said, "upscaler=fsr is not one of off, bilinear, fsr1, nis, sgsr: it runs off; "
                            "anti_aliasing=msaa is not one of off, auto, fxaa, smaa, ssaa2, ssaa4: it runs off"));
    }

    fx = snx_fx_create(t.phys, t.dev, VK_NULL_HANDLE, 2, log_line);
    CHECK(fx);
    snx_fx_set_sharpness(fx, 75);
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--bench")) {
            const char* frame = NULL;
            for (int k = 1; k < argc; ++k)
                if (!strncmp(argv[k], "--frame=", 8)) frame = argv[k] + 8;
            CHECK(!bench(&t, fx, frame, showcase));
            snx_fx_destroy(fx);
            snx_vk_ring_free(&t.ring);
            vkDestroyDevice(t.dev, NULL);
            snx_vk_instance_free(&t.instance);
            return 0;
        }

    /* FSR 1: the frame grows, and the result is the display's size */
    {
        SnxFxImage out = {0};
        VkCommandBuffer cb = snx_vk_ring_commands(&t.ring);
        uint64_t serial;
        unsigned char* got;
        int step;
        CHECK(snx_fx_render(fx, cb, 0, src.view, 320, 240, 0, 0, 320, 240, 0, 640, 480, SNX_FX_UPSCALE_FSR1,
                            SNX_FX_AA_OFF, &out));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        CHECK(out.width == 640 && out.height == 480);
        got = read_image_f16(&t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 640, 480);
        CHECK(got);
        CHECK(max_step(got, 640, 480, &step) > 0);
        write_ppm(showcase, "fx-fsr1", got, 640, 480);
        free(got);
    }
    /* NIS and SGSR grow the frame half as large again */
    {
        int filters[2] = {SNX_FX_UPSCALE_NIS, SNX_FX_UPSCALE_SGSR};
        for (int i = 0; i < 2; ++i) {
            SnxFxImage out = {0};
            VkCommandBuffer cb = snx_vk_ring_commands(&t.ring);
            uint64_t serial;
            CHECK(snx_fx_render(fx, cb, 0, step.view, 320, 240, 0, 0, 320, 240, 0, 480, 360, filters[i], SNX_FX_AA_OFF,
                                &out));
            serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
            CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
            CHECK(out.width == 480 && out.height == 360);
            CHECK(strstr(g_test.said, i ? "upscaler Qualcomm SGSR and" : "upscaler NVIDIA NIS and"));
        }
    }
    /* what runs where it is not what was asked, said with why: an upscaler where the frame does not grow is the caller's
       smooth blit, NIS past twice is FSR 1, and Auto's FXAA where the frame grows 1.25 times */
    {
        SnxFxImage out = {0};
        VkCommandBuffer cb = snx_vk_ring_commands(&t.ring);
        uint64_t serial;
        CHECK(!snx_fx_render(fx, cb, 0, step.view, 320, 240, 0, 0, 320, 240, 0, 320, 240, SNX_FX_UPSCALE_FSR1,
                             SNX_FX_AA_OFF, &out));
        CHECK(!strcmp(g_test.said, "upscaler Bilinear and anti-aliasing Off, 320x240 to 320x240 (AMD FSR 1 grows a "
                                   "frame, and this one is not shown larger)"));
        CHECK(snx_fx_render(fx, cb, 0, step.view, 320, 240, 0, 0, 320, 240, 0, 960, 720, SNX_FX_UPSCALE_NIS,
                            SNX_FX_AA_AUTO, &out));
        CHECK(!strcmp(g_test.said, "upscaler AMD FSR 1 and anti-aliasing FXAA, 320x240 to 960x720 (NVIDIA NIS grows "
                                   "a frame twice at most)"));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
    }
    /* upscaler_sharpness: 100 draws the edges harder than 0 */
    {
        double soft = 0, sharp = 0;
        for (int i = 0; i < 2; ++i) {
            SnxFxImage out = {0};
            VkCommandBuffer cb = snx_vk_ring_commands(&t.ring);
            uint64_t serial;
            unsigned char* got;
            snx_fx_set_sharpness(fx, i ? 100 : 0);
            CHECK(snx_fx_render(fx, cb, 0, src.view, 320, 240, 0, 0, 320, 240, 0, 640, 480, SNX_FX_UPSCALE_FSR1,
                                SNX_FX_AA_OFF, &out));
            serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
            CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
            got = read_image_f16(&t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 640, 480);
            CHECK(got);
            *(i ? &sharp : &soft) = avg_step(got, 640, 480);
            free(got);
        }
        fprintf(stderr, "present_fx: FSR 1's edge step at sharpness 0 %.3f, at 100 %.3f\n", soft, sharp);
        CHECK(sharp > soft);
        snx_fx_set_sharpness(fx, 75);
    }
    /* two frames' NIS in flight: each slot reads its own config. Both are recorded before either runs, as the next
       frame's can be while the GPU still reads the last one's; one config for both took the second's sizes into the
       first, whose pixels past them stayed unwritten */
    {
        static const unsigned char colours[2][3] = {{200, 40, 40}, {40, 200, 40}};
        const uint32_t side[2] = {64, 32}, out_side[2] = {128, 48};
        Image flat[2] = {{0}};
        SnxFxImage out[2] = {{0}};
        VkCommandBuffer cb;
        uint64_t serial;
        for (int i = 0; i < 2; ++i) {
            unsigned char* p = malloc((size_t)side[i] * side[i] * 4);
            CHECK(p);
            for (size_t k = 0; k < (size_t)side[i] * side[i]; ++k) {
                memcpy(p + k * 4, colours[i], 3);
                p[k * 4 + 3] = 255;
            }
            CHECK(image_make(&t, &flat[i], side[i], side[i], VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
            CHECK(upload(&t, &flat[i], p));
            free(p);
        }
        cb = snx_vk_ring_commands(&t.ring);
        for (uint32_t i = 0; i < 2; ++i)
            CHECK(snx_fx_render(fx, cb, i, flat[i].view, side[i], side[i], 0, 0, side[i], side[i], 0, out_side[i],
                                out_side[i], SNX_FX_UPSCALE_NIS, SNX_FX_AA_OFF, &out[i]));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        for (int i = 0; i < 2; ++i) {
            unsigned char* got =
                read_image_f16(&t, out[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out_side[i], out_side[i]);
            uint32_t bad = 0, first = 0;
            CHECK(got);
            for (uint32_t k = 0; k < out_side[i] * out_side[i]; ++k)
                for (int c = 0; c < 3; ++c)
                    if (abs((int)got[(size_t)k * 4 + c] - (int)colours[i][c]) > 1) {
                        if (!bad++) first = k;
                        break;
                    }
            if (bad)
                fprintf(stderr,
                        "present_fx: NIS in slot %d, %ux%u to %ux%u: %u pixels are not its frame's colour, the first "
                        "(%u, %u) = %u %u %u\n",
                        i, side[i], side[i], out_side[i], out_side[i], bad, first % out_side[i], first / out_side[i],
                        got[(size_t)first * 4], got[(size_t)first * 4 + 1], got[(size_t)first * 4 + 2]);
            free(got);
            CHECK(!bad);
            image_free(&t, &flat[i]);
        }
    }
    /* FXAA against its CPU reference (support/fxaa_ref.c, the shader step for step): a picture of a triangle's
       shallow and steep edges, a circle and a nearly level line over flat colours and a gradient. A pixel with no edge
       in its taps keeps its colour; an edge pixel is the reference's colour, or the other pair of taps' where its luma
       sits at the range's end, within what the device's bilinear weights (subTexelPrecisionBits) differ by; and a
       third of the edge pixels change at least */
    {
        enum { PW = 96, PH = 64 };
        static unsigned char pic[PW * PH * 4], edge[PW * PH];
        static float want[PW * PH * 4], other[PW * PH * 4];
        VkPhysicalDeviceProperties props;
        Image picture = {0};
        SnxFxImage out = {0};
        VkCommandBuffer cb;
        uint64_t serial;
        unsigned char* got;
        unsigned bits, tolerance, edges = 0, changed = 0, worst = 0;
        for (unsigned y = 0; y < PH; ++y)
            for (unsigned x = 0; x < PW; ++x) {
                unsigned char* px = pic + (y * PW + x) * 4;
                unsigned grey = x < PW / 2 ? 40u : 60u + (x - PW / 2) * 2u;
                float dx = (float)x - 70.5f, dy = (float)y - 30.5f;
                px[0] = px[1] = px[2] = (unsigned char)grey;
                px[3] = 255;
                if (y >= 8 && y < 56 && (int)x >= 8 + (int)(56 - y) / 3 && x < 8 + (y - 8) + 4)
                    px[0] = px[1] = px[2] = 230;
                if (dx * dx + dy * dy < 15.0f * 15.0f) px[0] = 220, px[1] = 30, px[2] = 30;
                if (y == 58 + x / 24) px[0] = px[1] = px[2] = 250;
            }
        fxaa_ref(pic, PW, PH, want, other, edge);
        vkGetPhysicalDeviceProperties(t.phys, &props);
        bits = props.limits.subTexelPrecisionBits;
        tolerance = 1u + (255u + (1u << bits) - 1u) / (1u << bits);
        CHECK(image_make(&t, &picture, PW, PH, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        CHECK(upload(&t, &picture, pic));
        cb = snx_vk_ring_commands(&t.ring);
        CHECK(snx_fx_render(fx, cb, 0, picture.view, PW, PH, 0, 0, PW, PH, 0, PW, PH, SNX_FX_UPSCALE_OFF,
                            SNX_FX_AA_FXAA, &out));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        CHECK(out.width == PW && out.height == PH);
        got = read_image_f16(&t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, PW, PH);
        CHECK(got);
        for (unsigned i = 0; i < PW * PH; ++i) {
            const unsigned char *g = got + i * 4, *p = pic + i * 4;
            unsigned d = 0, alternative = 0;
            if (!edge[i]) {
                if (memcmp(g, p, 3))
                    fprintf(stderr, "present_fx: FXAA, pixel (%u, %u) with no edge: %u %u %u, the picture's %u %u %u\n",
                            i % PW, i / PW, g[0], g[1], g[2], p[0], p[1], p[2]);
                CHECK(!memcmp(g, p, 3));
                continue;
            }
            ++edges;
            changed += memcmp(g, p, 3) != 0;
            for (int c = 0; c < 3; ++c) {
                int w = (int)lrintf(fminf(fmaxf(want[i * 4 + c], 0.0f), 1.0f) * 255.0f);
                int o = (int)lrintf(fminf(fmaxf(other[i * 4 + c], 0.0f), 1.0f) * 255.0f);
                unsigned dw = (unsigned)abs((int)g[c] - w), dob = (unsigned)abs((int)g[c] - o);
                d = dw > d ? dw : d;
                alternative = dob > alternative ? dob : alternative;
            }
            d = alternative < d ? alternative : d;
            worst = d > worst ? d : worst;
            if (d > tolerance)
                fprintf(stderr, "present_fx: FXAA, edge pixel (%u, %u): %u %u %u, %u off the reference (%u allowed)\n",
                        i % PW, i / PW, g[0], g[1], g[2], d, tolerance);
            CHECK(d <= tolerance);
        }
        free(got);
        image_free(&t, &picture);
        fprintf(stderr,
                "present_fx: FXAA: %u of %u pixels at an edge, %u smoothed, each within %u of the reference (%u "
                "allowed, %u sub-texel bits)\n",
                edges, PW * PH, changed, worst, tolerance, bits);
        CHECK(edges >= 200 && changed * 3 >= edges);
    }
    /* the upscaler Off on a window that keeps sharp pixels (the 3DS's): the frame scaled in whole pixels, then FXAA at
       the window's size. A one-pixel font's strokes (a diagonal, its mirror, a column and a row) shown 4 times as large
       keep each pixel's middle 2x2 its own colour, so the letters stay sharp; FXAA only touches the steps' edges, and
       does touch some */
    {
        enum { GW = 16, GH = 12, GS = 4 };
        static unsigned char glyph[GW * GH * 4];
        Image font = {0};
        SnxFxImage out = {0};
        VkCommandBuffer cb;
        uint64_t serial;
        unsigned char* got;
        unsigned kept = 0, smoothed = 0;
        for (unsigned y = 0; y < GH; ++y)
            for (unsigned x = 0; x < GW; ++x) {
                unsigned char* px = glyph + (y * GW + x) * 4;
                int stroke = (x == y && x >= 1 && x <= 9) || (x + y == 10 && x >= 1 && x <= 9) || (x == 12 && y >= 1) ||
                             (y == 10 && x >= 1 && x <= 14);
                px[0] = px[1] = px[2] = stroke ? 230 : 20;
                px[3] = 255;
            }
        CHECK(image_make(&t, &font, GW, GH, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        CHECK(upload(&t, &font, glyph));
        snx_fx_set_off_sharp(fx, 1);
        cb = snx_vk_ring_commands(&t.ring);
        CHECK(snx_fx_render(fx, cb, 0, font.view, GW, GH, 0, 0, GW, GH, 0, GW * GS, GH * GS, SNX_FX_UPSCALE_OFF,
                            SNX_FX_AA_FXAA, &out));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        snx_fx_set_off_sharp(fx, 0);
        CHECK(out.width == GW * GS && out.height == GH * GS);
        got = read_image_f16(&t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, GW * GS, GH * GS);
        CHECK(got);
        for (unsigned y = 0; y < GH * GS; ++y)
            for (unsigned x = 0; x < GW * GS; ++x) {
                const unsigned char *g = got + ((size_t)y * GW * GS + x) * 4, *p = glyph + ((y / GS) * GW + x / GS) * 4;
                int middle = (x % GS == 1 || x % GS == 2) && (y % GS == 1 || y % GS == 2);
                int same = abs((int)g[0] - (int)p[0]) <= 1 && abs((int)g[1] - (int)p[1]) <= 1 &&
                           abs((int)g[2] - (int)p[2]) <= 1;
                if (middle) {
                    if (!same)
                        fprintf(stderr, "present_fx: a glyph pixel's middle (%u, %u) is %u %u %u, the pixel's %u\n", x,
                                y, g[0], g[1], g[2], p[0]);
                    CHECK(same);
                    ++kept;
                } else
                    smoothed += !same;
            }
        free(got);
        image_free(&t, &font);
        fprintf(stderr, "present_fx: a one-pixel font at 4x, Off and FXAA: %u middles kept, %u edge pixels smoothed\n",
                kept, smoothed);
        CHECK(smoothed > 0);
    }
    /* FXAA and SMAA over a staircase, a dark half under a slope of one pixel up for two across: the steps' pixels
       take values between the two sides, where the frame has none */
    {
        const uint32_t side = 64;
        Image stairs = {0};
        unsigned char* p = malloc((size_t)side * side * 4);
        CHECK(p);
        for (uint32_t y = 0; y < side; ++y)
            for (uint32_t x = 0; x < side; ++x) {
                unsigned char v = 2 * y > x + side / 2 ? 230 : 20;
                p[((size_t)y * side + x) * 4 + 0] = p[((size_t)y * side + x) * 4 + 1] = v;
                p[((size_t)y * side + x) * 4 + 2] = v;
                p[((size_t)y * side + x) * 4 + 3] = 255;
            }
        CHECK(image_make(&t, &stairs, side, side, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        CHECK(upload(&t, &stairs, p));
        free(p);
        for (int k = 0; k < 2; ++k) {
            SnxFxImage out = {0};
            VkCommandBuffer cb = snx_vk_ring_commands(&t.ring);
            uint64_t serial;
            unsigned char* got;
            uint32_t between = 0;
            CHECK(snx_fx_render(fx, cb, 0, stairs.view, side, side, 0, 0, side, side, 0, side, side, SNX_FX_UPSCALE_OFF,
                                k ? SNX_FX_AA_SMAA : SNX_FX_AA_FXAA, &out));
            serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
            CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
            got = read_image_f16(&t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, side, side);
            CHECK(got);
            for (uint32_t i = 0; i < side * side; ++i) between += got[(size_t)i * 4] > 30 && got[(size_t)i * 4] < 220;
            write_ppm(showcase, k ? "fx-smaa-stairs" : "fx-fxaa-stairs", got, side, side);
            free(got);
            fprintf(stderr, "present_fx: %s over the staircase: %u pixels between its sides\n", k ? "SMAA" : "FXAA",
                    between);
            CHECK(between >= side / 2); /* the staircase has a step every two columns, 48 of them in the frame */
        }
        image_free(&t, &stairs);
    }
    /* FXAA and SMAA at 1:1: the source's hard step is smoothed */
    {
        double plain_avg, fxaa_avg, smaa_avg;
        int plain_step = 0, fxaa_step = 0, smaa_step = 0;
        SnxFxImage out = {0};
        VkCommandBuffer cb;
        uint64_t serial;
        unsigned char* got;
        max_step(step_pixels, 320, 240, &plain_step);
        plain_avg = avg_step(step_pixels, 320, 240);
        cb = snx_vk_ring_commands(&t.ring);
        CHECK(snx_fx_render(fx, cb, 0, step.view, 320, 240, 0, 0, 320, 240, 0, 320, 240, SNX_FX_UPSCALE_BILINEAR,
                            SNX_FX_AA_FXAA, &out));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        got = read_image_f16(&t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 320, 240);
        CHECK(got);
        max_step(got, 320, 240, &fxaa_step);
        fxaa_avg = avg_step(got, 320, 240);
        write_ppm(showcase, "fx-fxaa", got, 320, 240);
        free(got);
        cb = snx_vk_ring_commands(&t.ring);
        CHECK(snx_fx_render(fx, cb, 0, step.view, 320, 240, 0, 0, 320, 240, 0, 320, 240, SNX_FX_UPSCALE_BILINEAR,
                            SNX_FX_AA_SMAA, &out));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        got = read_image_f16(&t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 320, 240);
        CHECK(got);
        max_step(got, 320, 240, &smaa_step);
        smaa_avg = avg_step(got, 320, 240);
        write_ppm(showcase, "fx-smaa", got, 320, 240);
        free(got);
        fprintf(stderr, "present_fx: edge step plain %d/%.2f, FXAA %d/%.2f, SMAA %d/%.2f\n", plain_step, plain_avg,
                fxaa_step, fxaa_avg, smaa_step, smaa_avg);
        CHECK(fxaa_step > 0 && smaa_step > 0);
    }
    free(pixels);
    free(step_pixels);
    /* SSAA 2x: the pass is larger and the frame comes back at the display's size */
    {
        SnxFxImage out = {0};
        VkCommandBuffer cb = snx_vk_ring_commands(&t.ring);
        uint64_t serial;
        CHECK(snx_fx_render(fx, cb, 0, src.view, 320, 240, 0, 0, 320, 240, 0, 320, 240, SNX_FX_UPSCALE_OFF,
                            SNX_FX_AA_SSAA2, &out));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        CHECK(out.width == 320 && out.height == 240);
    }
    /* SSAA 4x with the upscaler off: the pass runs at 4x and a flat frame keeps its colour (no second encode) */
    {
        Image flat = {0};
        unsigned char* flat_pixels;
        unsigned char* got;
        SnxFxImage out = {0};
        VkCommandBuffer cb;
        uint64_t serial;
        CHECK(image_make(&t, &flat, 320, 240, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        flat_pixels = malloc((size_t)320 * 240 * 4);
        CHECK(flat_pixels);
        for (size_t i = 0; i < (size_t)320 * 240; ++i) {
            flat_pixels[i * 4 + 0] = 128;
            flat_pixels[i * 4 + 1] = 64;
            flat_pixels[i * 4 + 2] = 32;
            flat_pixels[i * 4 + 3] = 255;
        }
        CHECK(upload(&t, &flat, flat_pixels));
        cb = snx_vk_ring_commands(&t.ring);
        CHECK(snx_fx_render(fx, cb, 0, flat.view, 320, 240, 0, 0, 320, 240, 0, 320, 240, SNX_FX_UPSCALE_OFF,
                            SNX_FX_AA_SSAA4, &out));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        CHECK(out.width == 320 && out.height == 240);
        got = read_image_f16(&t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 320, 240);
        CHECK(got);
        {
            size_t at = ((size_t)120 * 320 + 160) * 4;
            CHECK(got[at + 0] >= 127 && got[at + 0] <= 129);
            CHECK(got[at + 1] >= 63 && got[at + 1] <= 65);
            CHECK(got[at + 2] >= 31 && got[at + 2] <= 33);
        }
        write_ppm(showcase, "fx-ssaa4", got, 320, 240);
        free(got);
        free(flat_pixels);
        image_free(&t, &flat);
    }
    /* a crop smaller than its image (a game drawing a dynamic resolution into part of its target): no filter takes a
       pixel from outside it. A flat crop inside a checker comes out flat from every filter and anti-aliasing, where
       their taps past the crop's edge once took the checker into its last rows and columns */
    {
        static const struct {
            int filter, aa;
            const char* name;
        } runs[] = {{SNX_FX_UPSCALE_FSR1, SNX_FX_AA_OFF, "FSR 1"},
                    {SNX_FX_UPSCALE_NIS, SNX_FX_AA_OFF, "NIS"},
                    {SNX_FX_UPSCALE_SGSR, SNX_FX_AA_OFF, "SGSR"},
                    {SNX_FX_UPSCALE_BILINEAR, SNX_FX_AA_FXAA, "bilinear and FXAA"},
                    {SNX_FX_UPSCALE_OFF, SNX_FX_AA_FXAA, "FXAA"},
                    {SNX_FX_UPSCALE_OFF, SNX_FX_AA_SMAA, "SMAA"},
                    {SNX_FX_UPSCALE_OFF, SNX_FX_AA_SSAA2, "SSAA 2x"}};
        static const unsigned char flat[3] = {100, 150, 200};
        const uint32_t iw = 64, ih = 64, cx = 8, cy = 12, cw = 48, ch = 40, ow = 96, oh = 80;
        Image framed = {0};
        unsigned char* framed_pixels = malloc((size_t)iw * ih * 4);
        CHECK(framed_pixels);
        for (uint32_t y = 0; y < ih; ++y)
            for (uint32_t x = 0; x < iw; ++x) {
                unsigned char* at = framed_pixels + ((size_t)y * iw + x) * 4;
                int inside = x >= cx && x < cx + cw && y >= cy && y < cy + ch;
                unsigned char checker = ((x ^ y) & 1) ? 255 : 0;
                for (int c = 0; c < 3; ++c) at[c] = inside ? flat[c] : checker;
                at[3] = 255;
            }
        CHECK(image_make(&t, &framed, iw, ih, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        CHECK(upload(&t, &framed, framed_pixels));
        free(framed_pixels);
        for (size_t r = 0; r < sizeof runs / sizeof *runs; ++r) {
            SnxFxImage out = {0};
            VkCommandBuffer cb = snx_vk_ring_commands(&t.ring);
            uint64_t serial;
            unsigned char* got;
            uint32_t bad = 0, first = 0;
            uint32_t gw = ow, gh = oh;
            CHECK(snx_fx_render(fx, cb, 0, framed.view, iw, ih, cx, cy, cw, ch, 0, ow, oh, runs[r].filter, runs[r].aa,
                                &out));
            serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
            CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
            CHECK(out.width == gw && out.height == gh);
            got = read_image_f16(&t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gw, gh);
            CHECK(got);
            for (uint32_t i = 0; i < gw * gh; ++i)
                for (int c = 0; c < 3; ++c)
                    if (abs((int)got[(size_t)i * 4 + c] - (int)flat[c]) > 1) {
                        if (!bad++) first = i;
                        break;
                    }
            if (bad)
                fprintf(stderr,
                        "present_fx: %s over a %ux%u crop of a %ux%u frame: %u of %u pixels took the frame outside it, "
                        "the first (%u, %u) = %u %u %u where the crop is %u %u %u\n",
                        runs[r].name, cw, ch, iw, ih, bad, gw * gh, first % gw, first / gw, got[(size_t)first * 4],
                        got[(size_t)first * 4 + 1], got[(size_t)first * 4 + 2], flat[0], flat[1], flat[2]);
            free(got);
            CHECK(!bad);
        }
        image_free(&t, &framed);
    }

    snx_fx_destroy(fx);
    image_free(&t, &src);
    image_free(&t, &step);
    snx_vk_ring_free(&t.ring);
    vkDestroyDevice(t.dev, NULL);
    snx_vk_instance_free(&t.instance);
    CHECK(g_test.val_errors == 0);
    puts("present_fx: passed");
    return 0;
}
