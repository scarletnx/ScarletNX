/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A hook module's upscale through the upscalers' interface (gpu/vk/upscaler/upscaler.h) on a real Vulkan device
   (lavapipe will do), as the replay of an upscale a module's CPU hook recorded asks the renderer for it
   (gpu/nvn/nvn_copy.c's OP_UPSCALE: GPU_UPSCALE_MODULE, the color and output resolved, the kinds of upscaler the
   module takes). This test's renderer runs the interface on its own images:
   - the choice with no device: DLSS without its runtime, the newer FSR and XeSS never. And on this device the fallback
     rule (FSR 2 for a temporal ask with depth and motion, the spatial upscaler or the copy only when taken, none for a
     color no device filters).
   - the scene upscaled by the spatial upscaler (Best's choice for a module that gives no depth or motion), near a
     bilinear upscale. Sharpened, EASU then RCAS: sharper, finite, and the same twice. The copy preferred and taken: a
     bilinear upscale. FSR 2 asked for alone, without depth: no upscaler runs (the replay's filtered copy then stands in,
     nvn_copy_formats).
   - SMAA before the copy blends the scene's stepped edges and leaves the rest. The copy of a picture taken back by a
     whole render pixel is the copy alone moved by two output pixels.
   - FSR 2 through the interface converges on a jittered still scene, as tests/vk_fsr2.c measures it directly.
   SNX_TEST_UPSCALE_DEVICE picks the device whose name holds it. SNX_TEST_UPSCALE_OUT is a folder for the pictures
   (.ppm). Exits 77 without a Vulkan device that can run it */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/upscaler/upscaler.h"
#include "nx/gpu/vk/upscaler/dlss.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

enum { RW = 64, RH = 36, OW = 128, OH = 72 };
#define SCENE_RT  0x400000ull /* the scene's target, a guest address the renderer maps to its image */
#define OUTPUT_RT 0x800000ull

Settings g_settings;
void logf_(int lvl, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    if (getenv("SNX_TEST_LOG") || !lvl) vprintf(fmt, a);
    va_end(a);
}

/* U when the upscalers' interface ran */
static char g_ran[16];
static void ran(char c) {
    size_t n = strlen(g_ran);
    if (n + 1 < sizeof g_ran) g_ran[n] = c, g_ran[n + 1] = 0;
}

/* ---- the device */
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
} g_vk_up;
#define G g_vk_up
#define VK_CHECK(call)                                                                                                 \
    do {                                                                                                               \
        VkResult r_ = (call);                                                                                          \
        if (r_ != VK_SUCCESS) {                                                                                        \
            fprintf(stderr, "FAIL %s: %d\n", #call, (int)r_);                                                          \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < G.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (G.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "FAIL no memory type\n");
    exit(1);
}
static SnxUpscaleImage image(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkDeviceMemory* mem) {
    SnxUpscaleImage i = {VK_NULL_HANDLE, VK_NULL_HANDLE, format, w, h};
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
    VK_CHECK(vkAllocateMemory(G.device, &ai, NULL, mem));
    VK_CHECK(vkBindImageMemory(G.device, i.image, *mem, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = i.image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(G.device, &vi, NULL, &i.view));
    return i;
}
static void drop(SnxUpscaleImage* i, VkDeviceMemory mem) {
    vkDestroyImageView(G.device, i->view, NULL);
    vkDestroyImage(G.device, i->image, NULL);
    vkFreeMemory(G.device, mem, NULL);
}
static void layout(VkImage i, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = from,
                              .newLayout = to,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = i,
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
/* the staging buffer's first texels (channels a texel as the format holds them) into i, left for sampling */
static void upload(const SnxUpscaleImage* i, VkImageLayout from) {
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {i->width, i->height, 1}};
    layout(i->image, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(G.cb, G.staging, i->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    layout(i->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
static int device(void) {
    const char* want = getenv("SNX_TEST_UPSCALE_DEVICE");
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
        vkGetPhysicalDeviceProperties(all[i], &p);
        if (want && !strstr(p.deviceName, want)) continue;
        if (!snx_upscaler_available(SNX_UPSCALER_FSR2, all[i], NULL)) continue;
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
                             .size = (VkDeviceSize)320 * 180 * 4 * sizeof(float), /* temporal()'s output */
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
static void teardown(void) {
    if (G.device) {
        VK_CHECK(vkDeviceWaitIdle(G.device));
        if (G.mapped) vkUnmapMemory(G.device, G.staging_memory);
        vkDestroyBuffer(G.device, G.staging, NULL);
        vkFreeMemory(G.device, G.staging_memory, NULL);
        vkDestroyFence(G.device, G.fence, NULL);
        vkDestroyCommandPool(G.device, G.pool, NULL);
        vkDestroyDevice(G.device, NULL);
    }
    if (G.instance) vkDestroyInstance(G.instance, NULL);
}

/* ---- the scene: diagonal bands with hard edges, a disc, and lines (vk_fsr2.c's) */
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
/* low (RW x RH, RGB) upscaled bilinearly to OW x OH, as a filtered copy does */
static void bilinear(const float* low, float* out, int rw, int rh, int ow, int oh) {
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            double sx = (x + 0.5) * rw / ow - 0.5, sy = (y + 0.5) * rh / oh - 0.5;
            int x0 = (int)floor(sx), y0 = (int)floor(sy);
            double fx = sx - x0, fy = sy - y0;
            for (int c = 0; c < 3; c++) {
                double v = 0;
                for (int k = 0; k < 4; k++) {
                    int px = x0 + (k & 1), py = y0 + (k >> 1);
                    px = px < 0 ? 0 : px >= rw ? rw - 1 : px;
                    py = py < 0 ? 0 : py >= rh ? rh - 1 : py;
                    v += low[(py * rw + px) * 3 + c] * ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                }
                out[(y * ow + x) * 3 + c] = (float)v;
            }
        }
}
/* the mean absolute difference of a (channels floats a pixel) from b (RGB) over n pixels */
static double difference(const float* a, int channels, const float* b, int n) {
    double sum = 0;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < 3; c++) {
            double v = a[i * channels + c];
            sum += v != v ? 1 : fabs(v - b[i * 3 + c]);
        }
    return sum / (n * 3);
}

/* ---- the renderer: a module's upscale run through the interface on this test's images */
static SnxUpscaleImage g_scene, g_output;
static SnxUpscalerId g_prefer = SNX_UPSCALER_BEST;
static float g_sharpness; /* RCAS after EASU when above 0 */
static int g_smaa;        /* SMAA before a copy or a spatial upscale: 1 for 1x */
static int g_smaa_quality = 2, g_smaa_edges; /* High, by luma */
static float g_shift[2];  /* the picture taken back by this many render pixels first */
static SnxUpscaler* g_up[SNX_UPSCALERS];
static SnxUpscalerId g_chosen;
static void clear_output(float r, float g, float b) {
    VkClearColorValue v = {{r, g, b, 1}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(G.cb, g_output.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
}
static int upscale(const GpuUpscale* u) {
    const char* why;
    if (u->color.addr != SCENE_RT || u->output.addr != OUTPUT_RT || !(u->flags & GPU_UPSCALE_MODULE)) return 0;
    SnxUpscaleNeeds needs = {(u->has_depth ? SNX_UP_DEPTH : 0) | (u->has_motion ? SNX_UP_MOTION : 0),
                             SNX_UP_TEMPORAL | (u->flags & GPU_UPSCALE_SPATIAL_OK ? SNX_UP_SPATIAL : 0) |
                                 (u->flags & GPU_UPSCALE_COPY_OK ? SNX_UP_COPY : 0),
                             u->flags & GPU_UPSCALE_HDR ? SNX_UP_HDR : SNX_UP_LDR,
                             {u->render[0], u->render[1]},
                             {u->out[0], u->out[1]},
                             g_scene.format,
                             g_output.format};
    SnxUpscalerId id = snx_upscaler_choose(g_prefer, G.physical, &needs, &why);
    g_chosen = id;
    if (id == SNX_UPSCALERS) {
        printf("no upscaler: %s\n", why);
        return 0;
    }
    if (!g_up[id]) {
        SnxUpscalerInfo info = {
            G.physical,      G.device, {u->render[0], u->render[1]}, {u->out[0], u->out[1]}, g_scene.format,
            g_output.format, 0};
        g_up[id] = snx_upscaler_create(id, &info, &why);
        if (!g_up[id]) {
            fprintf(stderr, "FAIL %s can't be made: %s\n", snx_upscaler_caps(id)->name, why);
            return 0;
        }
    }
    SnxUpscaleEval e = {.cb = G.cb,
                        .color = g_scene,
                        .output = g_output,
                        .render = {u->render[0], u->render[1]},
                        .sharpness = g_sharpness,
                        .smaa = g_smaa,
                        .smaa_quality = g_smaa_quality,
                        .smaa_edges = g_smaa_edges,
                        .shift = {g_shift[0], g_shift[1]}};
    if (!snx_upscaler_evaluate(g_up[id], &e, &why)) {
        fprintf(stderr, "FAIL %s's evaluation: %s\n", snx_upscaler_caps(id)->name, why);
        return 0;
    }
    ran('U');
    return 1;
}
static const Renderer g_test_renderer = {.upscale = upscale};
const Renderer* g_renderer = &g_test_renderer;

/* One module upscale of the scene onto the output, as the replay of a recorded upscale asks it of the renderer: the
   kinds of upscaler the module takes besides a temporal one in accept. The output, magenta before, read back. The
   renderer's answer */
static int once(u32 accept, float* out) {
    g_ran[0] = 0;
    begin();
    layout(g_output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    clear_output(1, 0, 1);
    layout(g_output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    GpuUpscale u = {0};
    u.color = (Surf2D){.format = 0xc0, .depth = 1, .width = RW, .height = RH, .addr = SCENE_RT};
    u.output = (Surf2D){.format = 0xc0, .depth = 1, .width = OW, .height = OH, .addr = OUTPUT_RT};
    u.render[0] = RW, u.render[1] = RH, u.out[0] = OW, u.out[1] = OH;
    u.pre_exposure = 1;
    u.flags = GPU_UPSCALE_MODULE | GPU_UPSCALE_RANGE_GIVEN | accept;
    int done = g_renderer->upscale(&u);
    layout(g_output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {OW, OH, 1}};
    vkCmdCopyImageToBuffer(G.cb, g_output.image, VK_IMAGE_LAYOUT_GENERAL, G.staging, 1, &c);
    submit();
    memcpy(out, G.mapped, (size_t)OW * OH * 4 * sizeof(float));
    return done;
}
static void ppm(const char* name, const float* out) {
    const char* dir = getenv("SNX_TEST_UPSCALE_OUT");
    char path[1024];
    if (!dir) return;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", OW, OH);
    for (int i = 0; i < OW * OH; i++)
        for (int c = 0; c < 3; c++) {
            float v = out[i * 4 + c];
            fputc(v != v || v < 0 ? 0 : v > 1 ? 255 : (int)(v * 255 + 0.5f), f);
        }
    fclose(f);
}
static int magenta(const float* out) {
    for (int i = 0; i < OW * OH; i++)
        if (out[i * 4] != 1 || out[i * 4 + 1] != 0 || out[i * 4 + 2] != 1) return 0;
    return 1;
}

/* ---- FSR 2 through the interface: vk_fsr2.c's still scene, 40 jittered frames from 160x90 to 320x180 */
static double temporal(double* e_bilinear) {
    enum { TW = 160, TH = 90, UW = 320, UH = 180, FRAMES = 40 };
    static float reference[UW * UH * 3], low[TW * TH * 3], lin[UW * UH * 3], out[UW * UH * 4];
    const char* why;
    for (int y = 0; y < UH; y++)
        for (int x = 0; x < UW; x++) {
            float sum[3] = {0, 0, 0}, s[3];
            for (int k = 0; k < 16; k++) {
                scene((x + (k % 4 + 0.5) / 4) / UW, (y + (k / 4 + 0.5) / 4) / UH, s);
                for (int c = 0; c < 3; c++) sum[c] += s[c] / 16;
            }
            memcpy(reference + (y * UW + x) * 3, sum, sizeof sum);
        }
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) scene((x + 0.5) / TW, (y + 0.5) / TH, low + (y * TW + x) * 3);
    bilinear(low, lin, TW, TH, UW, UH);
    *e_bilinear = difference(lin, 3, reference, UW * UH);
    SnxUpscalerInfo info = {
        G.physical, G.device, {TW, TH}, {UW, UH}, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    SnxUpscaler* f = snx_upscaler_create(SNX_UPSCALER_FSR2, &info, &why);
    if (!f) {
        fprintf(stderr, "FAIL FSR 2 through the interface: %s\n", why);
        return 1;
    }
    VkImageUsageFlags in = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkDeviceMemory mem[4];
    SnxUpscaleImage color = image(VK_FORMAT_R32G32B32A32_SFLOAT, TW, TH, in, &mem[0]);
    SnxUpscaleImage depth = image(VK_FORMAT_R32_SFLOAT, TW, TH, in, &mem[1]);
    SnxUpscaleImage motion = image(VK_FORMAT_R16G16_SFLOAT, TW, TH, in, &mem[2]);
    SnxUpscaleImage output =
        image(VK_FORMAT_R32G32B32A32_SFLOAT, UW, UH,
              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &mem[3]);
    for (int i = 0; i < TW * TH; i++) G.mapped[i] = 0.5f;
    begin();
    upload(&depth, VK_IMAGE_LAYOUT_UNDEFINED);
    submit();
    memset(G.mapped, 0, (size_t)TW * TH * 4);
    begin();
    upload(&motion, VK_IMAGE_LAYOUT_UNDEFINED);
    layout(output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    submit();
    int ok = 1;
    for (int frame = 0; frame < FRAMES && ok; frame++) {
        double jx = halton(frame % 32 + 1, 2) - 0.5, jy = halton(frame % 32 + 1, 3) - 0.5;
        for (int y = 0; y < TH; y++)
            for (int x = 0; x < TW; x++) {
                float* p = G.mapped + (y * TW + x) * 4;
                scene((x + 0.5 - jx) / TW, (y + 0.5 - jy) / TH, p);
                p[3] = 1;
            }
        begin();
        upload(&color, frame ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED);
        SnxUpscaleEval e = {.cb = G.cb,
                            .color = color,
                            .depth = depth,
                            .motion = motion,
                            .output = output,
                            .render = {TW, TH},
                            .jitter = {(float)jx, (float)jy},
                            .motion_scale = {1, 1},
                            .pre_exposure = 1,
                            .frame_ms = 16.7f,
                            .far_plane = 100,
                            .reset = frame == 0};
        ok = snx_upscaler_evaluate(f, &e, &why);
        if (!ok) fprintf(stderr, "FAIL FSR 2's evaluation at frame %d: %s\n", frame, why);
        submit();
    }
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {UW, UH, 1}};
    begin();
    vkCmdCopyImageToBuffer(G.cb, output.image, VK_IMAGE_LAYOUT_GENERAL, G.staging, 1, &c);
    submit();
    memcpy(out, G.mapped, sizeof out);
    VK_CHECK(vkDeviceWaitIdle(G.device));
    snx_upscaler_destroy(f);
    SnxUpscaleImage* made[4] = {&color, &depth, &motion, &output};
    for (int k = 0; k < 4; k++) drop(made[k], mem[k]);
    return ok ? difference(out, 4, reference, UW * UH) : 1;
}

int main(void) {
    static float low[RW * RH * 3], lin[OW * OH * 3], out[OW * OH * 4];
    int failures = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "FAIL %s\n", #x);                                                                          \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)
    /* the choice, with no device needed for the stubs: DLSS without Streamline running (never on this system, nor
       anywhere without the runtime's folder), newer FSR and XeSS never */
    {
        const char* why;
        uint64_t evaluated = 1, failed = 1;
        CHECK(!snx_dlss_start("", 0, NULL, &why) && why);
        CHECK(!snx_dlss_available(&why) && why && strstr(why, "DLSS"));
        SnxUpscalerInfo none = {0};
        CHECK(!snx_upscaler_create(SNX_UPSCALER_DLSS, &none, &why) && why && strstr(why, "DLSS"));
        snx_dlss_counts(&evaluated, &failed);
        CHECK(!evaluated && !failed);
        CHECK(!snx_upscaler_available(SNX_UPSCALER_DLSS, VK_NULL_HANDLE, &why) && why);
        CHECK(!snx_upscaler_available(SNX_UPSCALER_FSR_NEW, VK_NULL_HANDLE, &why) && why);
        CHECK(!snx_upscaler_available(SNX_UPSCALER_XESS, VK_NULL_HANDLE, &why) && why);
    }
    if (!device()) {
        printf("no Vulkan device that can run the upscalers: skipped\n");
        teardown();
        return failures ? 1 : 77;
    }
    /* the fallback rule on this device */
    {
        const char* why;
        SnxUpscaleNeeds all = {SNX_UP_DEPTH | SNX_UP_MOTION | SNX_UP_JITTER,
                               SNX_UP_TEMPORAL,
                               SNX_UP_HDR,
                               {640, 360},
                               {1280, 720},
                               VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                               VK_FORMAT_R16G16B16A16_SFLOAT};
        CHECK(snx_upscaler_choose(SNX_UPSCALER_BEST, G.physical, &all, &why) == SNX_UPSCALER_FSR2 && !why);
        /* a DLSS evaluation asking for the spatial upscaler gets FSR 2, and says why */
        CHECK(snx_upscaler_choose(SNX_UPSCALER_SPATIAL, G.physical, &all, &why) == SNX_UPSCALER_FSR2 && why);
        /* DLSS asked for: not running, FSR 2 stands in */
        CHECK(snx_upscaler_choose(SNX_UPSCALER_DLSS, G.physical, &all, &why) == SNX_UPSCALER_FSR2 && why &&
              strstr(why, "DLSS"));
        SnxUpscaleNeeds bare = {0,
                                SNX_UP_TEMPORAL,
                                SNX_UP_LDR,
                                {640, 360},
                                {1280, 720},
                                VK_FORMAT_R8G8B8A8_UNORM,
                                VK_FORMAT_R8G8B8A8_UNORM};
        CHECK(snx_upscaler_choose(SNX_UPSCALER_BEST, G.physical, &bare, &why) == SNX_UPSCALERS && why &&
              strstr(why, "depth"));
        bare.accept |= SNX_UP_SPATIAL;
        CHECK(snx_upscaler_choose(SNX_UPSCALER_BEST, G.physical, &bare, &why) == SNX_UPSCALER_SPATIAL);
        bare.range = SNX_UP_HDR; /* FSR 1 takes no HDR color: the copy, when taken */
        CHECK(snx_upscaler_choose(SNX_UPSCALER_BEST, G.physical, &bare, &why) == SNX_UPSCALERS);
        bare.accept |= SNX_UP_COPY;
        CHECK(snx_upscaler_choose(SNX_UPSCALER_BEST, G.physical, &bare, &why) == SNX_UPSCALER_COPY);
        /* an integer color, which no device filters: neither the copy nor the spatial upscaler, and the copy asked
           for says why */
        bare.color = VK_FORMAT_R8G8B8A8_UINT;
        CHECK(snx_upscaler_choose(SNX_UPSCALER_COPY, G.physical, &bare, &why) == SNX_UPSCALERS && why &&
              strstr(why, "filters"));
    }

    /* the images: the scene at the render size, and the output */
    VkDeviceMemory scene_mem, output_mem;
    g_scene = image(VK_FORMAT_R32G32B32A32_SFLOAT, RW, RH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &scene_mem);
    g_output = image(VK_FORMAT_R32G32B32A32_SFLOAT, OW, OH,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     &output_mem);
    for (int y = 0; y < RH; y++)
        for (int x = 0; x < RW; x++) {
            scene((x + 0.5) / RW, (y + 0.5) / RH, low + (y * RW + x) * 3);
            memcpy(G.mapped + (y * RW + x) * 4, low + (y * RW + x) * 3, 3 * sizeof(float));
            G.mapped[(y * RW + x) * 4 + 3] = 1;
        }
    begin();
    upload(&g_scene, VK_IMAGE_LAYOUT_UNDEFINED);
    submit();
    bilinear(low, lin, RW, RH, OW, OH);

    /* the spatial upscaler, Best's choice for a module that gives no depth or motion */
    CHECK(once(GPU_UPSCALE_SPATIAL_OK, out));
    double e_spatial = difference(out, 4, lin, OW * OH);
    ppm("spatial", out);
    printf("spatial: %s by %s, %.4f from a bilinear upscale\n", g_ran,
           g_chosen < SNX_UPSCALERS ? snx_upscaler_caps(g_chosen)->name : "none", e_spatial);
    CHECK(!strcmp(g_ran, "U") && g_chosen == SNX_UPSCALER_SPATIAL && !magenta(out));
    CHECK(e_spatial < 0.08); /* EASU's edges differ from a bilinear upscale, its flat areas don't */

    /* sharpened: EASU into the spatial upscaler's own image, then RCAS into the output, twice (the image new, then
       kept): finite, sharper than EASU alone, the same both times */
    {
        static float sharp[OW * OH * 4], easu[OW * OH * 4];
        g_sharpness = 0.8f;
        once(GPU_UPSCALE_SPATIAL_OK, sharp);
        once(GPU_UPSCALE_SPATIAL_OK, out);
        g_sharpness = 0;
        once(GPU_UPSCALE_SPATIAL_OK, easu);
        int finite = 1;
        double contrast = 0, contrast_easu = 0;
        for (int i = 0; i < OW * OH * 4; i++) finite &= out[i] == out[i] && out[i] > -1e6f && out[i] < 1e6f;
        for (int y = 0; y < OH; y++) /* how far each pixel is from its right neighbour: sharpening raises it */
            for (int x = 0; x + 1 < OW; x++)
                for (int c = 0; c < 3; c++) {
                    contrast += fabs(out[(y * OW + x + 1) * 4 + c] - out[(y * OW + x) * 4 + c]);
                    contrast_easu += fabs(easu[(y * OW + x + 1) * 4 + c] - easu[(y * OW + x) * 4 + c]);
                }
        ppm("spatial_sharp", out);
        printf("sharpened: contrast %.1f against EASU alone %.1f\n", contrast, contrast_easu);
        CHECK(finite && contrast > contrast_easu * 1.02);
        CHECK(!memcmp(sharp, out, sizeof sharp));
    }

    /* the copy preferred, and taken by the module: a bilinear upscale */
    g_prefer = SNX_UPSCALER_COPY;
    CHECK(once(GPU_UPSCALE_SPATIAL_OK | GPU_UPSCALE_COPY_OK, out));
    double e_copy = difference(out, 4, lin, OW * OH);
    ppm("copy", out);
    printf("copy: %s, %.5f from a bilinear upscale\n", g_ran, e_copy);
    CHECK(g_chosen == SNX_UPSCALER_COPY && e_copy < 0.005);

    /* the copy after SMAA: the bands' stepped edges blended, and the picture otherwise as the copy alone gives it */
    float plain[OW * OH * 4];
    memcpy(plain, out, sizeof plain);
    g_smaa = 1;
    CHECK(once(GPU_UPSCALE_SPATIAL_OK | GPU_UPSCALE_COPY_OK, out));
    g_smaa = 0;
    int blended = 0, finite_smaa = 1;
    double e_smaa = 0;
    for (int i = 0; i < OW * OH; i++)
        for (int c = 0; c < 3; c++) {
            double d = fabs(out[i * 4 + c] - plain[i * 4 + c]);
            finite_smaa &= out[i * 4 + c] == out[i * 4 + c];
            blended += d > 0.02;
            e_smaa += d;
        }
    e_smaa /= OW * OH * 3;
    ppm("copy_smaa", out);
    printf("copy after SMAA: %s, %d of %d values blended, %.4f from the copy alone\n", g_ran, blended, OW * OH * 3,
           e_smaa);
    CHECK(g_chosen == SNX_UPSCALER_COPY && finite_smaa && blended > 100 && e_smaa < 0.05);
    /* the Low preset by color: fewer searches, still blended, and T2x without motion vectors: 1x as it was */
    float high[OW * OH * 4];
    memcpy(high, out, sizeof high);
    g_smaa = 1, g_smaa_quality = 0, g_smaa_edges = 1;
    CHECK(once(GPU_UPSCALE_SPATIAL_OK | GPU_UPSCALE_COPY_OK, out));
    int blended_low = 0;
    for (int i = 0; i < OW * OH * 4; i++) blended_low += fabs(out[i] - plain[i]) > 0.02;
    g_smaa = 2, g_smaa_quality = 2, g_smaa_edges = 0;
    CHECK(once(GPU_UPSCALE_SPATIAL_OK | GPU_UPSCALE_COPY_OK, out));
    g_smaa = 0;
    printf("copy after SMAA Low by color: %d values blended. T2x without motion vectors %s 1x\n", blended_low,
           memcmp(out, high, sizeof high) ? "differs from" : "is");
    CHECK(blended_low > 0 && !memcmp(out, high, sizeof high));
    /* the copy of the picture taken back by a whole render pixel: the copy alone two output pixels over */
    g_shift[0] = 1;
    CHECK(once(GPU_UPSCALE_SPATIAL_OK | GPU_UPSCALE_COPY_OK, out));
    g_shift[0] = 0;
    double e_shift = 0;
    int counted = 0;
    for (int y = 0; y < OH; y++)
        for (int x = 0; x < OW - 4; x++, counted++)
            for (int c = 0; c < 3; c++) e_shift += fabs(out[(y * OW + x) * 4 + c] - plain[(y * OW + x + 2) * 4 + c]);
    e_shift /= counted * 3.0;
    printf("copy taken back by a render pixel: %.5f from the copy alone two output pixels over\n", e_shift);
    CHECK(e_shift < 0.005);

    /* FSR 2 asked for, and the module takes nothing else and gives no depth: no upscaler runs, the output as it was */
    g_prefer = SNX_UPSCALER_FSR2;
    CHECK(!once(0, out) && magenta(out) && !g_ran[0]);
    g_prefer = SNX_UPSCALER_BEST;

    double e_bilinear, e_fsr2 = temporal(&e_bilinear);
    printf("FSR 2 through the interface: %.4f against the scene at 320x180, bilinear %.4f\n", e_fsr2, e_bilinear);
    CHECK(e_fsr2 < e_bilinear * 0.8);

    VK_CHECK(vkDeviceWaitIdle(G.device));
    for (int i = 0; i < SNX_UPSCALERS; i++) snx_upscaler_destroy(g_up[i]);
    drop(&g_scene, scene_mem);
    drop(&g_output, output_mem);
    if (!failures) printf("ok\n");
    teardown();
    return failures ? 1 : 0;
}
