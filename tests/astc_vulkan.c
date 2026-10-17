/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Headless differential test of the actual Vulkan ASTC upload and frame/staging
   code. No game assets. Run with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
   and VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
   to check synchronization too. See docs/ASTC.md for the standalone build. */
#include <stdarg.h>
#include <stdlib.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/texture/astc.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <time.h>

VkState g_vk;
VkSemaphore g_vk_csem;
u64 g_vk_cs_wait_value;
static unsigned errors;

/* Only the window/game and guest pipeline services are stubbed. cmd.c and
   vk/astc.c are linked unchanged, including staging flushes and frame reuse. */
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}
int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    if (r != VK_SUCCESS) fatal("%s:%d: %s = %d\n", file, line, what, r);
    return 1;
}
void vk_render_end(void) {}
/* The standalone upload test has no guest buffer cache to flush or retire. */
void vk_buf_frame_done(u64 frame) { (void)frame; }
void vk_buf_host_barrier(VkCommandBuffer cb, u64 frame) {
    (void)cb;
    (void)frame;
}
VkPipelineCache vk_pipeline_cache(void) { return VK_NULL_HANDLE; }
/* cmd.c times its waits */
u64 plat_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000u + (u64)ts.tv_nsec;
}
void vk_pool_sizes(VkDescriptorPoolSize* sz, u32 sets) {
    sz[0] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 2};
    sz[1] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sets};
    sz[2] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets};
    sz[3] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets};
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT type,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    (void)type;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        fprintf(stderr, "%s\n", data->pMessage);
        errors++;
    }
    return VK_FALSE;
}
static void init(void) {
    VK_OK(volkInitialize());
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    const char* ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app,
                               .enabledExtensionCount = 1,
                               .ppEnabledExtensionNames = &ext};
    VK_OK(vkCreateInstance(&ii, NULL, &g_vk.instance));
    volkLoadInstance(g_vk.instance);
    VkDebugUtilsMessengerCreateInfoEXT di = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                                             .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                             .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
                                             .pfnUserCallback = debug};
    VK_OK(vkCreateDebugUtilsMessengerEXT(g_vk.instance, &di, NULL, &g_vk.debug));
    u32 n = 1;
    VkResult r = vkEnumeratePhysicalDevices(g_vk.instance, &n, &g_vk.phys);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) VK_OK(r);
    if (!n) fatal("no Vulkan device\n");
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    printf("Device: %s\n", g_vk.props.deviceName);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &n, NULL);
    VkQueueFamilyProperties* families = calloc(n, sizeof *families);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &n, families);
    u32 family = 0;
    while (family < n && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
    free(families);
    if (family == n) fatal("no compute queue\n");
    g_vk.gfx_family = family;
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = family,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .synchronization2 = VK_TRUE};
    VkDeviceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .pNext = &f13,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi};
    VK_OK(vkCreateDevice(g_vk.phys, &ci, NULL, &g_vk.dev));
    volkLoadDevice(g_vk.dev);
    vkGetDeviceQueue(g_vk.dev, family, 0, &g_vk.gfx);
    VmaVulkanFunctions funcs = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr,
                                .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys,
                                 .device = g_vk.dev,
                                 .instance = g_vk.instance,
                                 .vulkanApiVersion = VK_API_VERSION_1_3,
                                 .pVulkanFunctions = &funcs};
    VK_OK(vmaCreateAllocator(&ai, &g_vk.vma));
    vk_cmd_init();
}

static u32 rng = 0x12345678;
static u32 random_word(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static u8* blocks_new(u32 w, u32 h, u32 depth, u32 bw, u32 bh) {
    u32 count = ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * depth;
    u32* blocks = malloc((size_t)count * 16);
    for (u32 i = 0; i < count; i++) {
        for (u32 k = 0; k < 4; k++) blocks[i * 4 + k] = random_word();
        /* Exercise every block mode, plus valid and invalid solid-color blocks. */
        blocks[i * 4] = (blocks[i * 4] & ~2047u) | (i & 2047);
        if (i % 29 == 0) {
            blocks[i * 4] = 0xfffffdfc;
            blocks[i * 4 + 1] = ~0u;
        } else if (i % 31 == 0) {
            blocks[i * 4] = 0xfffff9fc; /* invalid reserved bit */
        }
    }
    return (u8*)blocks;
}

/* uploads random blocks into a UNORM or SRGB image and compares the readback with the CPU decoder in the same
   decode mode. keep, when given, receives the checked bytes (the caller frees them) */
static void run(u32 bw, u32 bh, int srgb, u32 w, u32 h, u32 depth, u32 layers, u32 levels, int grow, u8** keep,
                size_t* kept) {
    Surf t = {.w = w,
              .h = h,
              .layers = layers,
              .levels = levels,
              .depth3d = depth,
              .f = {.vk = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, .bw = bw, .bh = bh}};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                            .format = t.f.vk,
                            .extent = {w, h, depth},
                            .mipLevels = levels,
                            .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_OK(vmaCreateImage(g_vk.vma, &ii, &ai, &t.img, &t.mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_barrier(cb, t.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    size_t capacity = (size_t)w * h * depth * layers * 8;
    u8* expected = malloc(capacity);
    VkBufferImageCopy* copies = calloc((size_t)layers * levels, sizeof *copies);
    size_t offset = 0;
    u32 nr = 0;
    for (u32 layer = 0; layer < layers; layer++)
        for (u32 l = 0; l < levels; l++) {
            u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = depth >> l ? depth >> l : 1;
            u8* blocks = blocks_new(lw, lh, ld, bw, bh);
            size_t slice = (size_t)((lw + bw - 1) / bw) * ((lh + bh - 1) / bh) * 16;
            for (u32 z = 0; z < ld; z++)
                astc_decode_image(blocks + z * slice, lw, lh, bw, bh, expected + offset + (size_t)z * lw * lh * 4,
                                  srgb);
            vk_astc_upload(cb, &t, blocks, lw, lh, ld, l, layer);
            free(blocks);
            copies[nr++] = (VkBufferImageCopy){.bufferOffset = offset,
                                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                                               .imageExtent = {lw, lh, ld}};
            offset += (size_t)lw * lh * ld * 4;
        }
    if (grow) {
        VkBuffer ignored;
        u64 off;
        if (!vk_stage(40ull << 20, 16, &ignored, &off)) fatal("stage growth failed\n");
    }
    vk_barrier(cb, t.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBuffer readback;
    u8* actual = vk_readback_buffer(offset, &readback);
    vkCmdCopyImageToBuffer(cb, t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, nr, copies);
    vk_finish();
    vk_readback_ready();
    if (memcmp(actual, expected, offset)) {
        for (size_t i = 0; i < offset; i++)
            if (actual[i] != expected[i])
                fatal("%ux%u srgb=%d: byte %zu expected %u got %u\n", bw, bh, srgb, i, expected[i], actual[i]);
    }
    if (keep) {
        *keep = expected;
        *kept = offset;
    } else {
        free(expected);
    }
    free(copies);
    vmaDestroyImage(g_vk.vma, t.img, t.mem);
    printf("PASS %ux%u srgb=%d image=%ux%ux%u layers=%u mips=%u (%zu bytes)\n", bw, bh, srgb, w, h, depth, layers,
           levels, offset);
    fflush(stdout);
}

/* The same blocks in both decode modes: each matches the CPU decoder in its own mode (run checks that), alpha
   is the same in both, and R, G and B differ by at most one step (the sRGB mode's endpoints sit half a step
   higher, and it truncates where UNORM rounds), in some texels and not all. */
static void both_modes(u32 bw, u32 bh) {
    u8 *unorm, *srgb;
    size_t n, n2, differ = 0;
    rng = 0x2468ace1;
    run(bw, bh, 0, 97, 61, 1, 1, 1, 0, &unorm, &n);
    rng = 0x2468ace1;
    run(bw, bh, 1, 97, 61, 1, 1, 1, 0, &srgb, &n2);
    if (n != n2) fatal("both modes: sizes differ\n");
    for (size_t i = 0; i < n; i++) {
        int d = (int)srgb[i] - (int)unorm[i];
        if (i % 4 == 3 && d) fatal("both modes %ux%u: alpha byte %zu differs (%u, %u)\n", bw, bh, i, unorm[i], srgb[i]);
        if (d < -1 || d > 1) fatal("both modes %ux%u: byte %zu differs by %d\n", bw, bh, i, d);
        differ += d != 0;
    }
    if (!differ || differ == n / 4 * 3) fatal("both modes %ux%u: %zu of %zu bytes differ\n", bw, bh, differ, n);
    printf("PASS both decode modes %ux%u: %zu of %zu color bytes differ\n", bw, bh, differ, n / 4 * 3);
    free(unorm);
    free(srgb);
}

int main(void) {
    init();
    /* Known solid color, independent of the CPU reference, catches its old
       11-bit header bug. The GPU is compared to this reference below. */
    const u8 solid[16] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0x12, 0, 0x34, 0, 0x56, 0, 0x78};
    u8 pixel[4];
    astc_decode_image(solid, 1, 1, 4, 4, pixel, 0);
    if (memcmp(pixel, "\x12\x34\x56\x78", 4)) fatal("solid-color regression\n");
    const u32 footprints[][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                 {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    for (u32 i = 0; i < sizeof footprints / sizeof footprints[0]; i++)
        for (int srgb = 0; srgb < 2; srgb++) {
            u32 bw = footprints[i][0], bh = footprints[i][1];
            run(bw, bh, srgb, bw * 67 - 1, bh * 123 - 2, 1, 1, 1, 0, NULL, NULL);
            run(bw, bh, srgb, 17, 13, 1, 6, 5, 0, NULL, NULL);
            run(bw, bh, srgb, 17, 13, 5, 1, 5, 0, NULL, NULL);
        }
    for (u32 i = 0; i < sizeof footprints / sizeof footprints[0]; i++) both_modes(footprints[i][0], footprints[i][1]);
    /* Multiple scratch chunks, staging-ring retirement, and frame reuse. */
    run(4, 4, 0, 2051, 1055, 1, 1, 1, 1, NULL, NULL);
    run(12, 12, 1, 2051, 1055, 1, 1, 1, 0, NULL, NULL);
    if (errors) fatal("%u validation errors\n", errors);
    puts("All ASTC Vulkan comparisons passed (zero validation errors).");
    /* The backend's internal pipelines/rings have device lifetime, as in the
       runtime. Process exit releases these rather than testing a new teardown. */
    return 0;
}
