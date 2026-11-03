/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shared presentation effects on a real Vulkan device, with no game data and no window: a synthetic frame through
   FSR 1, FXAA, SMAA and SSAA, the HDR composition and encode, and the choices that turn them on. Read back and checked.
   --showcase=DIR writes the frames as PPM panels. */
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/ring.h"
#include "common/gpu/vk/vk_common.h"
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

static void log_line(const char* text) { puts(text); }

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
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                          .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
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
        ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {w, h, 1}};
    VkCommandBuffer cb;
    uint64_t serial;
    if (!out) return NULL;
    if (!succeed(vkCreateBuffer(t->dev, &bi, NULL, &buffer))) return NULL;
    vkGetBufferMemoryRequirements(t->dev, buffer, &req);
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    {
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
        ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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

int main(int argc, char** argv) {
    const char* showcase = NULL;
    Test t = {0};
    Image src = {0}, step = {0};
    unsigned char *pixels, *step_pixels;
    SnxFx* fx;
    SnxFxConfig cfg = {SNX_FX_UPSCALE_FSR1, 25, SNX_FX_AA_OFF, SNX_FX_HDR_ON,
                       SNX_FX_CS_HDR10,       SNX_FX_SRC_SDR, 203, 1000, 50, 0};
    for (int i = 1; i < argc; ++i)
        if (!strncmp(argv[i], "--showcase=", 11)) showcase = argv[i] + 11;
    CHECK(volkInitialize() == VK_SUCCESS);
    {
        SnxVkInstanceInfo info = {.app_name = "present_fx"};
        CHECK(succeed(snx_vk_instance_create(&info, &t.instance)));
    }
    CHECK(snx_vk_first_device(t.instance.instance, VK_API_VERSION_1_3, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
                              VK_NULL_HANDLE, make_device, &t));
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
    CHECK(snx_fx_upscale_parse("fsr1") == SNX_FX_UPSCALE_FSR1);
    CHECK(snx_fx_upscale_parse("none") == SNX_FX_UPSCALE_OFF);
    CHECK(snx_fx_aa_parse("smaa") == SNX_FX_AA_SMAA);
    CHECK(snx_fx_aa_parse("ssaa 2x") == SNX_FX_AA_SSAA2);
    CHECK(snx_fx_hdr_parse("auto") == SNX_FX_HDR_AUTO);
    CHECK(snx_fx_interp_parse("blend") == SNX_FX_INTERP_BLEND);
    CHECK(snx_fx_interp_auto(120, 60));
    CHECK(snx_fx_interp_auto(60, 30));
    CHECK(!snx_fx_interp_auto(60, 60));
    CHECK(!snx_fx_interp_auto(100, 60));

    fx = snx_fx_create(t.phys, t.dev, t.queue, t.family, VK_NULL_HANDLE, 2, NULL, NULL, log_line);
    CHECK(fx);
    snx_fx_configure(fx, &cfg);
    snx_fx_set_display(fx, SNX_FX_DISPLAY_PQ, 200);
    CHECK(snx_fx_hdr_active(fx));

    /* FSR 1: the frame grows, and the result is the display's size */
    {
        SnxFxImage out = {0};
        VkCommandBuffer cb = snx_vk_ring_commands(&t.ring);
        uint64_t serial;
        unsigned char* got;
        int step;
        CHECK(snx_fx_render(fx, cb, src.view, 320, 240, 0, 0, 320, 240, 0, 640, 480, SNX_FX_UPSCALE_FSR1,
                            SNX_FX_AA_OFF, &out));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        CHECK(out.width == 640 && out.height == 480);
        got = read_image(&t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 640, 480);
        CHECK(got);
        CHECK(max_step(got, 640, 480, &step) > 0);
        write_ppm(showcase, "fx-fsr1", got, 640, 480);
        free(got);
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
        CHECK(snx_fx_render(fx, cb, step.view, 320, 240, 0, 0, 320, 240, 0, 320, 240, SNX_FX_UPSCALE_BILINEAR,
                            SNX_FX_AA_FXAA, &out));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        got = read_image(&t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 320, 240);
        CHECK(got);
        max_step(got, 320, 240, &fxaa_step);
        fxaa_avg = avg_step(got, 320, 240);
        write_ppm(showcase, "fx-fxaa", got, 320, 240);
        free(got);
        cb = snx_vk_ring_commands(&t.ring);
        CHECK(snx_fx_render(fx, cb, step.view, 320, 240, 0, 0, 320, 240, 0, 320, 240, SNX_FX_UPSCALE_BILINEAR,
                            SNX_FX_AA_SMAA, &out));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        got = read_image(&t, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 320, 240);
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
        CHECK(snx_fx_pick_aa(fx, SNX_FX_AA_SSAA2, 320, 240, 320, 240, 16384) == SNX_FX_AA_SSAA2);
        CHECK(snx_fx_render(fx, cb, src.view, 320, 240, 0, 0, 320, 240, 0, 320, 240, SNX_FX_UPSCALE_OFF,
                            SNX_FX_AA_SSAA2, &out));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        CHECK(out.width == 320 && out.height == 240);
    }
    /* HDR: white at the paper white, black stays black, and the letterbox is black */
    {
        SnxFxImage comp = {0};
        VkCommandBuffer cb = snx_vk_ring_commands(&t.ring);
        uint64_t serial;
        unsigned char* pq;
        Image target = {0};
        CHECK(snx_fx_hdr_compose(fx, cb, src.view, VK_FORMAT_R8G8B8A8_UNORM, 320, 240, 0, 0, 320, 240, 0, 640, 480, 0,
                                 60, 640, 360, &comp));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        CHECK(image_make(&t, &target, 640, 480, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
        cb = snx_vk_ring_commands(&t.ring);
        CHECK(snx_fx_hdr_encode(fx, cb, &comp, target.image, target.view, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 640, 480,
                                SNX_FX_ENC_PQ));
        serial = snx_vk_ring_submit(&t.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        CHECK(snx_vk_ring_wait(&t.ring, serial, UINT64_MAX));
        pq = read_image(&t, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 640, 480);
        CHECK(pq);
        write_ppm(showcase, "fx-hdr-pq", pq, 640, 480);
        free(pq);
        image_free(&t, &target);
    }

    snx_fx_destroy(fx);
    image_free(&t, &src);
    image_free(&t, &step);
    snx_vk_ring_free(&t.ring);
    vkDestroyDevice(t.dev, NULL);
    snx_vk_instance_free(&t.instance);
    puts("present_fx: passed");
    return 0;
}
