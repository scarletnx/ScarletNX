/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* FSR 2.2.1 through gpu/vk/fsr2/fsr2.h on a real Vulkan device (lavapipe will do), with no renderer: a still scene of
   hard edges and thin lines, drawn at 160x90 with a new jitter each frame, as a game draws for a temporal upscaler, and
   upscaled to 320x180 over 40 frames. The output must be finite and nearer the scene drawn at 320x180 than a
   bilinear upscale of one unjittered frame is: that holds only when the passes run, the history accumulates and the
   jitter is taken with the sign fsr2.h says (the other sign is measured too, and must be worse).
   SNX_TEST_FSR2_DEVICE picks the device whose name holds it; SNX_TEST_FSR2_OUT is a folder for the pictures (.ppm).
   Exits 77 without a Vulkan device that can run it */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/fsr2/fsr2.h"

enum { RW = 160, RH = 90, OW = 320, OH = 180, FRAMES = 40 };

static struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    float* mapped;
    VkPhysicalDeviceMemoryProperties memory;
} g_vk_fsr2;
#define G g_vk_fsr2

#define VK_CHECK(call)                                                                                                 \
    do {                                                                                                               \
        VkResult r_ = (call);                                                                                          \
        if (r_ != VK_SUCCESS) {                                                                                        \
            fprintf(stderr, "FAIL %s: %d\n", #call, (int)r_);                                                          \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* the scene at (u, v), each 0 to 1: diagonal bands with hard edges, a disc, and lines 1.6 render pixels wide */
static void scene(double u, double v, float rgb[3]) {
    double band = fmod(u * 7 + v * 5, 1.0) < 0.5 ? 1 : 0;
    double line = fabs(fmod(u * 20 + v * 3, 1.0) - 0.5) < 0.1 ? 1 : 0;
    double ring = hypot(u - 0.5, (v - 0.5) * 9.0 / 16) < 0.2 ? 1 : 0;
    rgb[0] = (float)(0.1 + 0.8 * band);
    rgb[1] = (float)(0.1 + 0.8 * line);
    rgb[2] = (float)(0.1 + 0.8 * ring);
}
static double halton(int i, int base) {
    double f = 1, r = 0;
    for (; i > 0; i /= base) {
        f /= base;
        r += f * (i % base);
    }
    return r;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < G.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (G.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "FAIL no memory type\n");
    exit(1);
}
static SnxFsr2Image image(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    SnxFsr2Image i = {VK_NULL_HANDLE, VK_NULL_HANDLE, format, w, h};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .usage = usage};
    VK_CHECK(vkCreateImage(G.device, &ci, NULL, &i.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(G.device, i.image, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = memory_type(req.memoryTypeBits, 0)};
    VkDeviceMemory memory;
    VK_CHECK(vkAllocateMemory(G.device, &ai, NULL, &memory));
    VK_CHECK(vkBindImageMemory(G.device, i.image, memory, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = i.image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(G.device, &vi, NULL, &i.view));
    return i;
}
static void layout(const SnxFsr2Image* i, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = from,
                              .newLayout = to,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = i->image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(G.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
}
static void begin(void) {
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkResetCommandBuffer(G.cb, 0));
    VK_CHECK(vkBeginCommandBuffer(G.cb, &bi));
}
static void submit(void) {
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &G.cb};
    VK_CHECK(vkEndCommandBuffer(G.cb));
    VK_CHECK(vkResetFences(G.device, 1, &G.fence));
    VK_CHECK(vkQueueSubmit(G.queue, 1, &si, G.fence));
    VK_CHECK(vkWaitForFences(G.device, 1, &G.fence, VK_TRUE, UINT64_MAX));
}
/* the staging buffer's first texels into i (as channels floats a texel), left for sampling */
static void upload(const SnxFsr2Image* i, VkImageLayout from) {
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {i->width, i->height, 1}};
    layout(i, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(G.cb, G.staging, i->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    layout(i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static int device(void) {
    const char* want = getenv("SNX_TEST_FSR2_DEVICE");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkPhysicalDevice all[16];
    uint32_t n = 16, family = 0, families = 16;
    VkQueueFamilyProperties qf[16];
    if (volkInitialize() != VK_SUCCESS || vkCreateInstance(&ici, NULL, &G.instance) != VK_SUCCESS) return 0;
    volkLoadInstance(G.instance);
    if (vkEnumeratePhysicalDevices(G.instance, &n, all) < 0 || !n) return 0;
    for (uint32_t i = 0; i < n && !G.physical; i++) {
        VkPhysicalDeviceProperties p;
        VkPhysicalDeviceFeatures f;
        vkGetPhysicalDeviceProperties(all[i], &p);
        vkGetPhysicalDeviceFeatures(all[i], &f);
        if (want && !strstr(p.deviceName, want)) continue;
        if (!f.shaderStorageImageReadWithoutFormat || !f.shaderStorageImageWriteWithoutFormat) continue;
        G.physical = all[i];
        printf("device: %s\n", p.deviceName);
    }
    if (!G.physical) return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(G.physical, &families, qf);
    while (family < families && !(qf[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
    if (family == families) return 0;
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = family,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    VkPhysicalDeviceFeatures features = {.shaderStorageImageReadWithoutFormat = VK_TRUE,
                                         .shaderStorageImageWriteWithoutFormat = VK_TRUE};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi,
                             .pEnabledFeatures = &features};
    if (vkCreateDevice(G.physical, &di, NULL, &G.device) != VK_SUCCESS) return 0;
    volkLoadDevice(G.device);
    vkGetDeviceQueue(G.device, family, 0, &G.queue);
    vkGetPhysicalDeviceMemoryProperties(G.physical, &G.memory);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = family};
    VK_CHECK(vkCreateCommandPool(G.device, &pi, NULL, &G.pool));
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = G.pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VK_CHECK(vkAllocateCommandBuffers(G.device, &ai, &G.cb));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(G.device, &fi, NULL, &G.fence));
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (VkDeviceSize)OW * OH * 4 * sizeof(float),
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VK_CHECK(vkCreateBuffer(G.device, &bi, NULL, &G.staging));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(G.device, G.staging, &req);
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex =
                                   memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK_CHECK(vkAllocateMemory(G.device, &mi, NULL, &G.staging_memory));
    VK_CHECK(vkBindBufferMemory(G.device, G.staging, G.staging_memory, 0));
    void* p;
    VK_CHECK(vkMapMemory(G.device, G.staging_memory, 0, VK_WHOLE_SIZE, 0, &p));
    G.mapped = (float*)p;
    return 1;
}

static void ppm(const char* name, const float* rgb, int channels) {
    const char* dir = getenv("SNX_TEST_FSR2_OUT");
    char path[1024];
    if (!dir) return;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", OW, OH);
    for (int i = 0; i < OW * OH; i++)
        for (int c = 0; c < 3; c++) {
            float v = rgb[i * channels + c];
            fputc(v != v || v < 0 ? 0 : v > 1 ? 255 : (int)(v * 255 + 0.5f), f);
        }
    fclose(f);
}

/* FRAMES frames of the scene through a new context, the camera moved by sign * jitter: the output in out (RGBA), and
   0 when a call fails */
static int run(double sign, float sharpness, float* out) {
    SnxFsr2Info info = {G.physical, G.device, {RW, RH}, {OW, OH}, 0};
    SnxFsr2* f = snx_fsr2_create(&info);
    if (!f) {
        fprintf(stderr, "FAIL snx_fsr2_create: FSR error %#x\n", (unsigned)snx_fsr2_error());
        return 0;
    }
    VkImageUsageFlags in = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    SnxFsr2Image color = image(VK_FORMAT_R32G32B32A32_SFLOAT, RW, RH, in);
    SnxFsr2Image depth = image(VK_FORMAT_R32_SFLOAT, RW, RH, in);
    SnxFsr2Image motion = image(VK_FORMAT_R16G16_SFLOAT, RW, RH, in);
    SnxFsr2Image output = image(VK_FORMAT_R32G32B32A32_SFLOAT, OW, OH,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    int ok = 1;
    /* depth one plane, no motion */
    for (int i = 0; i < RW * RH; i++) G.mapped[i] = 0.5f;
    begin();
    upload(&depth, VK_IMAGE_LAYOUT_UNDEFINED);
    submit();
    memset(G.mapped, 0, (size_t)RW * RH * 4);
    begin();
    upload(&motion, VK_IMAGE_LAYOUT_UNDEFINED);
    layout(&output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    submit();
    for (int frame = 0; frame < FRAMES && ok; frame++) {
        double jx = halton(frame % 32 + 1, 2) - 0.5, jy = halton(frame % 32 + 1, 3) - 0.5;
        for (int y = 0; y < RH; y++)
            for (int x = 0; x < RW; x++) {
                float* p = G.mapped + (y * RW + x) * 4;
                /* the picture moved by +jitter: a pixel's centre sees what was jitter before it */
                scene((x + 0.5 - sign * jx) / RW, (y + 0.5 - sign * jy) / RH, p);
                p[3] = 1;
            }
        begin();
        upload(&color, frame ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED);
        SnxFsr2Dispatch d = {.cb = G.cb,
                             .color = color,
                             .depth = depth,
                             .motion = motion,
                             .output = output,
                             .render = {RW, RH},
                             .jitter = {(float)jx, (float)jy},
                             .motion_scale = {1, 1},
                             .pre_exposure = 1,
                             .frame_ms = 16.7f,
                             .near_plane = 0.1f,
                             .far_plane = 100,
                             .fov_y = 1,
                             .reset = frame == 0,
                             .sharpness = sharpness};
        ok = snx_fsr2_dispatch(f, &d);
        if (!ok) fprintf(stderr, "FAIL snx_fsr2_dispatch at frame %d: FSR error %#x\n", frame, (unsigned)snx_fsr2_error());
        submit();
    }
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {OW, OH, 1}};
    begin();
    vkCmdCopyImageToBuffer(G.cb, output.image, VK_IMAGE_LAYOUT_GENERAL, G.staging, 1, &c);
    submit();
    memcpy(out, G.mapped, (size_t)OW * OH * 4 * sizeof(float));
    VK_CHECK(vkDeviceWaitIdle(G.device));
    snx_fsr2_destroy(f);
    return ok;
}

/* the mean absolute difference of a's color (channels floats a pixel) from the reference's, and whether all is finite */
static double difference(const float* a, int channels, const float* reference, int* finite, double* mean) {
    double sum = 0, total = 0;
    *finite = 1;
    for (int i = 0; i < OW * OH; i++)
        for (int c = 0; c < 3; c++) {
            float v = a[i * channels + c];
            if (v != v || v > 1e6f || v < -1e6f) *finite = 0;
            sum += fabs((double)v - reference[i * 3 + c]);
            total += v;
        }
    *mean = total / (OW * OH * 3);
    return sum / (OW * OH * 3);
}

int main(void) {
    static float reference[OW * OH * 3], low[RW * RH * 3], bilinear[OW * OH * 3], fsr[OW * OH * 4], other[OW * OH * 4];
    static float sharp[OW * OH * 4];
    if (!device()) {
        printf("no Vulkan device that can run FSR 2: skipped\n");
        return 77;
    }
    /* the scene at the output's size, 4x4 samples a pixel */
    for (int y = 0; y < OH; y++)
        for (int x = 0; x < OW; x++) {
            float sum[3] = {0, 0, 0}, s[3];
            for (int k = 0; k < 16; k++) {
                scene((x + (k % 4 + 0.5) / 4) / OW, (y + (k / 4 + 0.5) / 4) / OH, s);
                for (int c = 0; c < 3; c++) sum[c] += s[c] / 16;
            }
            memcpy(reference + (y * OW + x) * 3, sum, sizeof sum);
        }
    /* one unjittered frame at the render size, upscaled with a linear filter: what the copy stand-in shows */
    for (int y = 0; y < RH; y++)
        for (int x = 0; x < RW; x++) scene((x + 0.5) / RW, (y + 0.5) / RH, low + (y * RW + x) * 3);
    for (int y = 0; y < OH; y++)
        for (int x = 0; x < OW; x++) {
            double sx = (x + 0.5) * RW / OW - 0.5, sy = (y + 0.5) * RH / OH - 0.5;
            int x0 = (int)floor(sx), y0 = (int)floor(sy);
            double fx = sx - x0, fy = sy - y0;
            for (int c = 0; c < 3; c++) {
                double v = 0;
                for (int k = 0; k < 4; k++) {
                    int px = x0 + (k & 1), py = y0 + (k >> 1);
                    px = px < 0 ? 0 : px >= RW ? RW - 1 : px;
                    py = py < 0 ? 0 : py >= RH ? RH - 1 : py;
                    v += low[(py * RW + px) * 3 + c] * ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                }
                bilinear[(y * OW + x) * 3 + c] = (float)v;
            }
        }
    if (!run(1, 0, fsr) || !run(-1, 0, other) || !run(1, 0.8f, sharp)) return 1;
    int finite, finite_other, finite_sharp, finite_bilinear, failures = 0;
    double mean, mean_other, mean_sharp, mean_bilinear;
    double e_bilinear = difference(bilinear, 3, reference, &finite_bilinear, &mean_bilinear);
    double e = difference(fsr, 4, reference, &finite, &mean);
    double e_other = difference(other, 4, reference, &finite_other, &mean_other);
    double e_sharp = difference(sharp, 4, reference, &finite_sharp, &mean_sharp);
    ppm("reference", reference, 3);
    ppm("bilinear", bilinear, 3);
    ppm("fsr2", fsr, 4);
    ppm("fsr2_other_sign", other, 4);
    ppm("fsr2_sharp", sharp, 4);
    printf("mean absolute error against the scene at %dx%d: bilinear %.4f, FSR 2 %.4f (mean %.3f), FSR 2 with the "
           "jitter's sign reversed %.4f, FSR 2 sharpened %.4f (mean %.3f); the scene's mean %.3f\n",
           OW, OH, e_bilinear, e, mean, e_other, e_sharp, mean_sharp, mean_bilinear);
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "FAIL %s\n", #x);                                                                          \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)
    CHECK(finite && finite_other && finite_sharp);
    CHECK(mean > 0.2 && mean < 0.7); /* not black, not blown: the scene's mean is near 0.4 */
    CHECK(mean_sharp > 0.2 && mean_sharp < 0.7);
    CHECK(e < e_bilinear * 0.8);     /* the history resolves what one frame's filter can't */
    CHECK(e < e_other);              /* and only with the jitter's sign right */
    if (!failures) printf("ok\n");
    return failures ? 1 : 0;
}
