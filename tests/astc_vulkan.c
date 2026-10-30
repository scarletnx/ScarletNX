/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Headless differential test of the actual Vulkan ASTC upload, block linear
   deswizzle and swizzle, frame/staging and allocation code (vk/astc.c,
   vk/blocklinear.c, vk/cmd.c, vk/memory.c) with the SPIR-V the build compiles
   from astc.comp and blocklinear.comp. No game assets. Run with
   VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation and
   VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
   to check synchronization too (docs/ASTC.md). --vk_max_allocation_mb=N lowers
   the allocation cap, so images in pieces are tested at small sizes; --large
   decodes a 2D and a 3D texture of 512 MiB each. */
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "support/astc.h"
#include "nx/gpu/texture/blocklinear.h"
#include "support/etc2.h"
#include "support/bcn.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/plat.h"

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
void vk_gfx_image_frame_done(u64 sub) { (void)sub; }
void vk_buf_host_barrier(VkCommandBuffer cb, u64 frame) {
    (void)cb;
    (void)frame;
}
VkPipelineCache vk_pipeline_cache(void) { return VK_NULL_HANDLE; }
/* no occlusion queries, and no guest memory for cmd.c's sampled hashes */
void vk_query_flush(VkCommandBuffer cb) { (void)cb; }
/* cmd.c's sampled hash reads guest memory this way: there is none here */
void vk_guest_peek(u64 gva, void* dst, u64 size) {
    (void)gva;
    memset(dst, 0, size);
}
int gmem_read(u64 gva, void* dst, u64 size) {
    (void)gva;
    memset(dst, 0, size);
    return 0;
}
/* one thread adds commands here, so record.c's drain has no other producer to keep out (device.c's vk_lock) */
void vk_lock(void) {}
void vk_unlock(void) {}
/* record.c resolves stand-ins for pipelines still being made (pipeline.c, not here): the test binds real ones */
VkPipeline vk_pipeline_resolve(VkPipeline h) { return h; }
VkPipeline vk_pipeline_now(VkPipeline h, int* pending) {
    *pending = 0;
    return h;
}
/* cmd.c reads the frame's GPU timestamps for gpuprof.c, which isn't here */
void vk_gpuprof_frame_done(u32 slot) { (void)slot; }
/* cmd.c's waits note themselves for gpu.c's stall report, which isn't here */
void gpu_wait_note(const char* what, u64 value) {
    (void)what;
    (void)value;
}
/* plat_posix.c's crash handler, which the test never installs */
void log_crash_mode(void) {}
void crash_report(void* host_pc, void* fault, int sig) {
    (void)host_pc;
    (void)fault;
    (void)sig;
}
void vk_pool_sizes(VkDescriptorPoolSize* sz, u32 sets) {
    sz[0] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 2};
    sz[1] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sets};
    sz[2] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets};
    sz[3] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets};
    sz[4] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, sets};
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
    if (family == n) fatal("no compute queue\n");
    int sparse_queue = (families[family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;
    free(families);
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(g_vk.phys, &have);
    g_vk.feat.sparseBinding = sparse_queue && have.sparseBinding;
    g_vk.feat.textureCompressionBC = have.textureCompressionBC; /* bc_native_case compares the device's decoding */
    g_vk.gfx_family = family;
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = family,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .synchronization2 = VK_TRUE};
    /* cmd.c's frame fences are timeline semaphores */
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13, .timelineSemaphore = VK_TRUE};
    VkDeviceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .pNext = &f12,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi,
                             .pEnabledFeatures = &g_vk.feat};
    VK_OK(vkCreateDevice(g_vk.phys, &ci, NULL, &g_vk.dev));
    volkLoadDevice(g_vk.dev);
    vkGetDeviceQueue(g_vk.dev, family, 0, &g_vk.gfx);
    VmaVulkanFunctions funcs = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr,
                                .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    vk_mem_init(sparse_queue);
    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys,
                                 .device = g_vk.dev,
                                 .instance = g_vk.instance,
                                 .preferredLargeHeapBlockSize = g_vk.alloc_cap < (256ull << 20) ? g_vk.alloc_cap : 0,
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
    snx_le32_array(blocks, (size_t)count * 4); /* the words as a little-endian guest's bytes: the modes above hold */
    return (u8*)blocks;
}

/* gpu.c's bl_offset, which blocklinear.comp has to agree with */
static u64 ref_bl(u32 x, u32 y, u32 z, u32 wb, u32 h, u32 bh, u32 bd) {
    u64 gobs_x = (wb + 63) >> 6, gy = y >> 3;
    u64 rows = ((((u64)h + 7) >> 3) + (1u << bh) - 1) >> bh, block = (u64)512 << (bh + bd);
    u64 in_block = ((((u64)z & ((1u << bd) - 1)) << bh) + (gy & ((1u << bh) - 1))) << 9;
    return ((u64)(x >> 6) << (9 + bh + bd)) + ((x & 32) << 3) + ((x & 16) << 1) + (x & 15) +
           (((u64)(z >> bd) * rows + (gy >> bh)) * gobs_x) * block + in_block + (((y & 7) >> 1) << 6) + ((y & 1) << 4);
}

/* a level's blocks in memory, handed to vk_astc_upload as texture.c hands it guest memory: rows of blocks, or (bl not
   NULL) the same blocks in a block linear layout, which the GPU deswizzles as texture.c's astc_rows has it */
typedef struct {
    const u8* blocks;
    u32 nx, ny;
    const u8* bl;
    u32 bh, bd;
} Level;
static int level_rows(void* ctx, VkCommandBuffer* pcb, u32 z, u32 nz, u32 y, u32 n, VkBuffer* buf, u64* off) {
    const Level* l = ctx;
    VkCommandBuffer cb = *pcb;
    if (l->bl) {
        VkBlMove m = {l->nx * 16, l->ny, l->bh, l->bd, y, n, z, nz, l->nx * 16};
        u64 lo, hi, at, cap;
        VkBuffer staged;
        vk_bl_range(&m, &lo, &hi);
        u8* p = vk_stage(hi - lo, vk_stage_align(), &staged, &at);
        *buf = vk_bl_scratch((u64)l->nx * 16 * n * nz, &cap);
        *off = 0;
        if (!p || !*buf) return 0;
        memcpy(p, l->bl + lo, hi - lo);
        vk_bl_barrier(cb, 1);
        int ok = vk_bl_move(cb, &m, 0, staged, at, *buf, 0);
        vk_bl_barrier(cb, 0);
        return ok;
    }
    u8* p = vk_stage((u64)n * nz * l->nx * 16, vk_stage_align(), buf, off);
    if (!p) return 0;
    for (u32 k = 0; k < nz; k++)
        memcpy(p + (size_t)k * n * l->nx * 16, l->blocks + ((size_t)(z + k) * l->ny + y) * l->nx * 16,
               (size_t)n * l->nx * 16);
    return 1;
}
static int g_bl_source; /* run's blocks come block linear */

static void barrier(VkImage img, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src, VkImageLayout from,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst, VkImageLayout to) {
    vk_barrier(vk_cmd(), img, VK_IMAGE_ASPECT_COLOR_BIT, src_stage, src, from, dst_stage, dst, to);
}

/* reads a level of one layer back in bands of block rows of a slice that the readback buffer takes, comparing each
   with the CPU decoder's bytes for the band's blocks. keep, when given, receives the level's bytes */
static void compare(VkImage img, u32 bw, u32 bh, int srgb, u32 w, u32 h, u32 depth, u32 level, u32 layer,
                    const u8* blocks, u8* keep) {
    u32 nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh;
    u64 limit = g_vk.alloc_cap < (32ull << 20) ? g_vk.alloc_cap : 32ull << 20, row = (u64)w * bh * 4;
    u32 nb = limit / row ? (u32)(limit / row) : 1;
    u8* expected = malloc((size_t)row * nb);
    for (u32 z = 0; z < depth; z++)
        for (u32 y = 0; y < ny; y += nb) {
            u32 n = ny - y < nb ? ny - y : nb, rows = h - y * bh < n * bh ? h - y * bh : n * bh;
            u64 size = (u64)w * rows * 4;
            VkBuffer readback;
            u8* actual = vk_readback_buffer(size, &readback);
            if (!actual) fatal("no readback buffer of %llu bytes\n", (unsigned long long)size);
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
                                      .imageOffset = {0, (i32)(y * bh), (i32)z},
                                      .imageExtent = {w, rows, 1}};
            rec_CmdCopyImageToBuffer(vk_cmd(), img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
            vk_finish();
            vk_readback_ready();
            astc_decode_image(blocks + ((size_t)z * ny + y) * nx * 16, w, rows, bw, bh, expected, srgb);
            for (u64 i = 0; i < size; i++)
                if (actual[i] != expected[i])
                    fatal("%ux%u srgb=%d %ux%ux%u level %u layer %u: byte %llu of slice %u, block row %u: expected %u "
                          "got %u\n",
                          bw, bh, srgb, w, h, depth, level, layer, (unsigned long long)i, z, y, expected[i], actual[i]);
            if (keep) memcpy(keep + ((size_t)z * h + y * bh) * w * 4, actual, size);
        }
    free(expected);
}

/* uploads random blocks into a UNORM or SRGB image and compares the readback with the CPU decoder in the same
   decode mode. keep, when given, receives the checked bytes of the first level and layer (the caller frees them) */
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
    if (!vk_image_create(&ii, &t.img, &t.mem)) fatal("cannot create a %ux%ux%u image\n", w, h, depth);
    int pieces = !t.mem;
    barrier(t.img, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    u8** blocks = calloc((size_t)layers * levels, sizeof *blocks);
    size_t bytes = 0;
    for (u32 layer = 0; layer < layers; layer++)
        for (u32 l = 0; l < levels; l++) {
            u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = depth >> l ? depth >> l : 1;
            u8* b = blocks[layer * levels + l] = blocks_new(lw, lh, ld, bw, bh);
            Level lv = {b, (lw + bw - 1) / bw, (lh + bh - 1) / bh, NULL, 2, ld > 1};
            u8* bl = NULL;
            if (g_bl_source) {
                VkBlMove whole = {lv.nx * 16, lv.ny, lv.bh, lv.bd, 0, lv.ny, 0, ld, lv.nx * 16};
                u64 lo, hi;
                vk_bl_range(&whole, &lo, &hi);
                lv.bl = bl = calloc(hi, 1);
                for (u32 z = 0; z < ld; z++)
                    for (u32 y = 0; y < lv.ny; y++)
                        for (u32 x = 0; x < lv.nx; x++)
                            memcpy(bl + ref_bl(x * 16, y, z, lv.nx * 16, lv.ny, lv.bh, lv.bd),
                                   b + (((size_t)z * lv.ny + y) * lv.nx + x) * 16, 16);
            }
            if (!vk_astc_upload(vk_cmd(), &t, lw, lh, ld, l, layer, level_rows, &lv)) fatal("upload failed\n");
            free(bl); /* copied into staging already */
            bytes += (size_t)lw * lh * ld * 4;
        }
    if (grow) {
        VkBuffer ignored;
        u64 off;
        if (!vk_stage(40ull << 20 < g_vk.alloc_cap ? 40ull << 20 : g_vk.alloc_cap, 16, &ignored, &off))
            fatal("stage growth failed\n");
    }
    barrier(t.img, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (keep) {
        *kept = (size_t)w * h * depth * 4;
        *keep = malloc(*kept);
    }
    for (u32 layer = 0; layer < layers; layer++)
        for (u32 l = 0; l < levels; l++) {
            u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = depth >> l ? depth >> l : 1;
            compare(t.img, bw, bh, srgb, lw, lh, ld, l, layer, blocks[layer * levels + l],
                    keep && !layer && !l ? *keep : NULL);
            free(blocks[layer * levels + l]);
        }
    free(blocks);
    vk_image_destroy(t.img, t.mem);
    printf("PASS %ux%u srgb=%d image=%ux%ux%u layers=%u mips=%u (%zu bytes%s)\n", bw, bh, srgb, w, h, depth, layers,
           levels, bytes, pieces ? ", memory in pieces" : "");
    fflush(stdout);
}

/* The same blocks in both decode modes: each matches the CPU decoder in its own mode (run checks that), alpha
   is the same in both, and R, G and B differ by at most one step, in some texels and not all. Only void extents
   differ: the sRGB mode takes the top 8 bits of their color where UNORM rounds, while for interpolated texels its
   higher endpoints and truncation give the same byte as UNORM's rounding. */
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

/* block linear bytes deswizzled and swizzled on the GPU (vk/blocklinear.c), compared with ref_bl: a surface of
   row_bytes x h x d in blocks of 2^bh x 2^bd GOBs, moved in bands of band rows (not GOB aligned, as ASTC's strips
   aren't) of all slices, or a slice at a time. Swizzling leaves all bytes outside the valid row width as they were,
   and bl_land then lands exactly the surface's bytes over other guest memory */
static void bl_case(u32 row_bytes, u32 h, u32 d, u32 bh, u32 bd, u32 band, int by_slice) {
    u32 pitch = (row_bytes + 15) & ~15u;
    VkBlMove whole = {row_bytes, h, bh, bd, 0, h, 0, d, pitch};
    u64 lo, hi, lin_size = (u64)pitch * h * d, at, cap;
    vk_bl_range(&whole, &lo, &hi);
    u8* guest = malloc(hi);
    u8* lin = malloc(lin_size);
    for (u64 i = 0; i < hi; i++) guest[i] = (u8)random_word();
    for (u64 i = 0; i < lin_size; i++) lin[i] = (u8)random_word();
    VkBuffer rb, staged;
    /* deswizzle, band by band, each band's rows (slices after another) after the last band's in the readback */
    u8* out = vk_readback_buffer(lin_size > hi ? lin_size : hi, &rb);
    if (!out) fatal("no readback buffer\n");
    u64 done = 0;
    for (u32 z = 0; z < d; z += by_slice ? 1 : d)
        for (u32 y = 0; y < h; y += band) {
            VkBlMove m = {row_bytes, h, bh, bd, y, h - y < band ? h - y : band, z, by_slice ? 1 : d, pitch};
            u64 mlo, mhi, size = (u64)m.slices * m.rows * pitch;
            vk_bl_range(&m, &mlo, &mhi);
            u8* p = vk_stage(mhi - mlo, vk_stage_align(), &staged, &at);
            VkBuffer scratch = vk_bl_scratch(size, &cap);
            if (!p || !scratch) fatal("no staging or scratch\n");
            memcpy(p, guest + mlo, mhi - mlo);
            vk_bl_barrier(vk_cmd(), 1);
            if (!vk_bl_move(vk_cmd(), &m, 0, staged, at, scratch, 0)) fatal("move refused\n");
            vk_bl_barrier(vk_cmd(), 0);
            VkBufferCopy c = {0, done, size};
            rec_CmdCopyBuffer(vk_cmd(), scratch, rb, 1, &c);
            done += size;
        }
    vk_finish();
    vk_readback_ready();
    done = 0;
    for (u32 z = 0; z < d; z += by_slice ? 1 : d)
        for (u32 y0 = 0; y0 < h; y0 += band) {
            u32 n = h - y0 < band ? h - y0 : band, nz = by_slice ? 1 : d;
            for (u32 k = 0; k < nz; k++)
                for (u32 y = 0; y < n; y++)
                    for (u32 x = 0; x < row_bytes; x++) {
                        u8 want = guest[ref_bl(x, y0 + y, z + k, row_bytes, h, bh, bd)];
                        u8 got = out[done + ((u64)k * n + y) * pitch + x];
                        if (got != want)
                            fatal("deswizzle %ux%ux%u bh %u bd %u band %u: (%u, %u, %u) is %u, not %u\n", row_bytes, h,
                                  d, bh, bd, band, x, y0 + y, z + k, got, want);
                    }
            done += (u64)nz * n * pitch;
        }
    /* swizzle into the readback, which holds 0xA5 first */
    u8* p = vk_stage(hi, vk_stage_align(), &staged, &at);
    if (!p) fatal("no staging\n");
    memset(p, 0xA5, hi);
    VkBufferCopy c = {at, 0, hi};
    rec_CmdCopyBuffer(vk_cmd(), staged, rb, 1, &c);
    for (u32 z = 0; z < d; z += by_slice ? 1 : d)
        for (u32 y = 0; y < h; y += band) {
            VkBlMove m = {row_bytes, h, bh, bd, y, h - y < band ? h - y : band, z, by_slice ? 1 : d, pitch};
            u64 mlo, mhi, size = (u64)m.slices * m.rows * pitch;
            vk_bl_range(&m, &mlo, &mhi);
            u8* q = vk_stage(size, vk_stage_align(), &staged, &at);
            if (!q) fatal("no staging\n");
            for (u32 k = 0; k < m.slices; k++)
                memcpy(q + (u64)k * m.rows * pitch, lin + ((u64)(z + k) * h + y) * pitch, (u64)m.rows * pitch);
            vk_bl_barrier(vk_cmd(), 1);
            if (!vk_bl_move(vk_cmd(), &m, 1, rb, mlo, staged, at)) fatal("move refused\n");
            vk_bl_barrier(vk_cmd(), 0);
        }
    vk_finish();
    vk_readback_ready();
    u8* moved = calloc(hi, 1);
    for (u32 z = 0; z < d; z++)
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < row_bytes; x++) {
                u64 o = ref_bl(x, y, z, row_bytes, h, bh, bd);
                moved[o] = 1;
                if (out[o] != lin[((u64)z * h + y) * pitch + x])
                    fatal("swizzle %ux%ux%u bh %u bd %u band %u: (%u, %u, %u) is %u, not %u\n", row_bytes, h, d, bh,
                          bd, band, x, y, z, out[o], lin[((u64)z * h + y) * pitch + x]);
            }
    for (u64 i = 0; i < hi; i++)
        if (!moved[i] && out[i] != 0xA5)
            fatal("swizzle %ux%ux%u bh %u bd %u: byte %llu outside the runs was written\n", row_bytes, h, d, bh, bd,
                  (unsigned long long)i);
    /* landed over guest memory: the surface's bytes and nothing else */
    u8* landed = malloc(hi);
    memcpy(landed, guest, hi);
    bl_land(landed, out, row_bytes, h, d, bh, bd);
    memset(moved, 0, hi);
    for (u32 z = 0; z < d; z++)
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < row_bytes; x++) {
                u64 o = ref_bl(x, y, z, row_bytes, h, bh, bd);
                moved[o] = 1;
                if (landed[o] != lin[((u64)z * h + y) * pitch + x])
                    fatal("bl_land %ux%ux%u bh %u bd %u: (%u, %u, %u) wrong\n", row_bytes, h, d, bh, bd, x, y, z);
            }
    for (u64 i = 0; i < hi; i++)
        if (!moved[i] && landed[i] != guest[i])
            fatal("bl_land %ux%ux%u bh %u bd %u: padding byte %llu changed\n", row_bytes, h, d, bh, bd,
                  (unsigned long long)i);
    printf("PASS block linear %ux%ux%u bh %u bd %u band %u%s\n", row_bytes, h, d, bh, bd, band,
           by_slice ? " by slice" : "");
    fflush(stdout);
    free(guest);
    free(lin);
    free(moved);
    free(landed);
}

/* texconv.comp's SPIR-V on the device against the CPU: random ETC2 RGBA blocks and random D24S8 texels (as floats,
   for D32_SFLOAT_S8_UINT), in rows not a multiple of the four texels an invocation owns */
static void tc_case(void) {
    u32 w = 13, h = 9, bw = 4, bh = 3, pitch = 64; /* 4 blocks of 16 bytes a row */
    u64 in_size = (u64)pitch * bh, at, cap;
    VkBuffer staged, rb;
    u8* p = vk_stage(in_size, vk_stage_align(), &staged, &at);
    u8* blocks = malloc(bw * bh * 16);
    for (u32 i = 0; i < bw * bh * 16; i++) blocks[i] = (u8)random_word();
    memcpy(p, blocks, in_size);
    VkFmt f = {.vk = VK_FORMAT_R8G8B8A8_UNORM, .bpp = 16, .bw = 4, .bh = 4, .conv = VKC_ETC2_RGBA};
    VkTcShape s = vk_tc_shape(&f, w);
    u64 out_size = vk_tc_out_size(&s, bh, 1);
    VkBuffer scratch = vk_bl_scratch(out_size, &cap);
    u8* out = vk_readback_buffer(out_size, &rb);
    vk_bl_barrier(vk_cmd(), 1);
    if (!vk_tc_convert(vk_cmd(), &f, w, bh, 1, staged, at, pitch, scratch, 0)) fatal("conversion refused\n");
    vk_bl_barrier(vk_cmd(), 0);
    VkBufferCopy c = {0, 0, out_size};
    rec_CmdCopyBuffer(vk_cmd(), scratch, rb, 1, &c);
    vk_finish();
    vk_readback_ready();
    u8* want = malloc((size_t)w * h * 4);
    etc2_decode_image(blocks, w, h, ETC2_RGBA, want);
    for (u32 y = 0; y < h; y++)
        if (memcmp(out + (u64)y * s.out_texels * 4, want + (u64)y * w * 4, w * 4))
            fatal("texconv ETC2 RGBA: row %u differs from the CPU's\n", y);
    free(blocks);
    free(want);
    /* D24S8 to D32_SFLOAT_S8_UINT: depth as the float nearest z / (2^24 - 1), then the stencil bytes */
    VkFmt d = {.vk = VK_FORMAT_D32_SFLOAT_S8_UINT, .bpp = 4, .bw = 1, .bh = 1, .depth = 1, .stencil = 1,
               .conv = VKC_D24S8};
    u32 dw = 11, rows = 6, dpitch = 48;
    u32* texels = malloc((size_t)dpitch * rows);
    for (u32 i = 0; i < dpitch * rows / 4; i++) texels[i] = random_word();
    texels[0] = 0xFFFFFFFF, texels[1] = 0;
    p = vk_stage((u64)dpitch * rows, vk_stage_align(), &staged, &at);
    memcpy(p, texels, (size_t)dpitch * rows);
    snx_le32_array(p, (size_t)dpitch * rows / 4); /* the guest's bytes, little-endian words */
    s = vk_tc_shape(&d, dw);
    out_size = vk_tc_out_size(&s, rows, 1);
    scratch = vk_bl_scratch(out_size, &cap);
    out = vk_readback_buffer(out_size, &rb);
    vk_bl_barrier(vk_cmd(), 1);
    if (!vk_tc_convert(vk_cmd(), &d, dw, rows, 1, staged, at, dpitch, scratch, 0)) fatal("conversion refused\n");
    vk_bl_barrier(vk_cmd(), 0);
    c.size = out_size;
    rec_CmdCopyBuffer(vk_cmd(), scratch, rb, 1, &c);
    vk_finish();
    vk_readback_ready();
    const u8* stencil = out + (u64)s.out_texels * rows * 4;
    for (u32 y = 0; y < rows; y++)
        for (u32 x = 0; x < dw; x++) {
            u32 v = texels[y * dpitch / 4 + x], got;
            float z = (float)(v & 0xFFFFFF) / 16777215.0f;
            got = snx_ld_le32(out + ((u64)y * s.out_texels + x) * 4); /* the device's float, little-endian */
            if (memcmp(&got, &z, 4) || stencil[y * s.out_texels + x] != v >> 24)
                fatal("texconv D24S8: texel (%u, %u) differs from the CPU's\n", x, y);
        }
    free(texels);
    puts("PASS texture conversions (ETC2 RGBA, D24S8 as float)");
    fflush(stdout);
}

/* random BCn blocks: BC6H's codes and BC7's modes each made as often as the others, a reserved one now and then */
static void bc_blocks(int kind, u8* b, u32 n) {
    static const u8 codes[14] = {0, 1, 2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15};
    for (u32 i = 0; i < n; i++, b += bcn_block_bytes(kind)) {
        for (u32 j = 0; j < bcn_block_bytes(kind); j++) b[j] = (u8)random_word();
        if (random_word() % 16 == 0) continue;
        if (kind == BCN_BC6H_UF || kind == BCN_BC6H_SF) {
            u8 c = codes[random_word() % 14];
            b[0] = (u8)((b[0] & (c < 2 ? ~3 : ~31)) | c);
        }
        if (kind == BCN_BC7) {
            u32 m = random_word() % 8;
            b[0] = (u8)((b[0] & ~((2u << m) - 1)) | 1u << m);
        }
    }
}

/* texconv.comp's SPIR-V on the device against the CPU's BCn decoder (tests/support/bcn.c), random blocks of every kind in an image
   whose sides aren't multiples of 4: byte for byte */
static void bc_case(void) {
    u32 w = 13, h = 9, bw = 4, bh = 3;
    for (int kind = BCN_BC1; kind <= BCN_BC7; kind++) {
        u32 bb = bcn_block_bytes(kind), pb = bcn_pixel_bytes(kind), pitch = (bw * bb + 15) / 16 * 16;
        u64 at, cap;
        VkBuffer staged, rb;
        u8* blocks = malloc((size_t)bw * bh * bb);
        bc_blocks(kind, blocks, bw * bh);
        u8* p = vk_stage((u64)pitch * bh, vk_stage_align(), &staged, &at);
        for (u32 y = 0; y < bh; y++) memcpy(p + (u64)y * pitch, blocks + (size_t)y * bw * bb, bw * bb);
        VkFmt f = {.vk = VK_FORMAT_UNDEFINED, .bpp = (u8)bb, .bw = 4, .bh = 4, .conv = (u8)(VKC_BC1 + kind)};
        VkTcShape s = vk_tc_shape(&f, w);
        u64 out_size = vk_tc_out_size(&s, bh, 1);
        VkBuffer scratch = vk_bl_scratch(out_size, &cap);
        u8* out = vk_readback_buffer(out_size, &rb);
        vk_bl_barrier(vk_cmd(), 1);
        if (!vk_tc_convert(vk_cmd(), &f, w, bh, 1, staged, at, pitch, scratch, 0)) fatal("conversion refused\n");
        vk_bl_barrier(vk_cmd(), 0);
        VkBufferCopy c = {0, 0, out_size};
        rec_CmdCopyBuffer(vk_cmd(), scratch, rb, 1, &c);
        vk_finish();
        vk_readback_ready();
        u8* want = malloc((size_t)w * h * pb);
        bcn_decode_image(blocks, w, h, kind, want);
        for (u32 y = 0; y < h; y++)
            if (memcmp(out + (u64)y * s.out_texels * pb, want + (u64)y * w * pb, (size_t)w * pb))
                fatal("texconv BCn kind %d: row %u differs from the CPU's\n", kind, y);
        free(blocks);
        free(want);
    }
    puts("PASS texture conversions (BC1 to BC7)");
    fflush(stdout);
}

static float half_float(u16 h) {
    int e = h >> 10 & 31, m = h & 1023;
    float v = e == 0 ? m / 16777216.0f : e == 31 ? (m ? NAN : INFINITY) : (1024 + m) * ldexpf(1, e - 25);
    return h & 0x8000 ? -v : v;
}
/* where the device samples BCn: its own decoding of random blocks of every kind, blitted into RGBA32F, against
   bcn.c's. BC6H's halves and BC7's bytes are exact by the specification; BC1 to BC5 leave the interpolants' precision
   to the device, so they may differ by two steps (llvmpipe divides BC3's alpha by 5 as x * 51 >> 8: 918 / 5 is 183.6,
   which bcn.c rounds to 184, and llvmpipe gives 182). This tells a reference drifted from what GPUs decode, which
   the comparison of texconv with the reference can't */
static void bc_native_case(void) {
    static const VkFormat formats[10] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
                                         VK_FORMAT_BC3_UNORM_BLOCK,      VK_FORMAT_BC4_UNORM_BLOCK,
                                         VK_FORMAT_BC4_SNORM_BLOCK,      VK_FORMAT_BC5_UNORM_BLOCK,
                                         VK_FORMAT_BC5_SNORM_BLOCK,      VK_FORMAT_BC6H_UFLOAT_BLOCK,
                                         VK_FORMAT_BC6H_SFLOAT_BLOCK,    VK_FORMAT_BC7_UNORM_BLOCK};
    if (!g_vk.feat.textureCompressionBC) {
        puts("SKIP native BCn: the device doesn't sample BCn");
        return;
    }
    u32 w = 64, h = 64, nb = w / 4 * (h / 4);
    for (int kind = BCN_BC1; kind <= BCN_BC7; kind++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, formats[kind], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
            printf("SKIP native BCn kind %d: the device can't blit from it\n", kind);
            continue;
        }
        u32 bb = bcn_block_bytes(kind), pb = bcn_pixel_bytes(kind);
        u8* blocks = malloc((size_t)nb * bb);
        bc_blocks(kind, blocks, nb);
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = formats[kind],
                                .extent = {w, h, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VkImage src, dst;
        VmaAllocation src_mem, dst_mem;
        if (!vk_image_create(&ii, &src, &src_mem)) fatal("cannot create a native BCn image\n");
        ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        if (!vk_image_create(&ii, &dst, &dst_mem)) fatal("cannot create an RGBA32F image\n");
        u64 at;
        VkBuffer staged, rb;
        memcpy(vk_stage((u64)nb * bb, vk_stage_align(), &staged, &at), blocks, (size_t)nb * bb);
        barrier(src, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        barrier(dst, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy up = {.bufferOffset = at,
                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {w, h, 1}};
        rec_CmdCopyBufferToImage(vk_cmd(), staged, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
        barrier(src, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .srcOffsets = {{0, 0, 0}, {(i32)w, (i32)h, 1}},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .dstOffsets = {{0, 0, 0}, {(i32)w, (i32)h, 1}}};
        rec_CmdBlitImage(vk_cmd(), src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &blit, VK_FILTER_NEAREST);
        barrier(dst, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        float* got = (float*)vk_readback_buffer((u64)w * h * 16, &rb);
        VkBufferImageCopy down = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        rec_CmdCopyImageToBuffer(vk_cmd(), dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &down);
        vk_finish();
        vk_readback_ready();
        u8* want = malloc((size_t)w * h * pb);
        bcn_decode_image(blocks, w, h, kind, want);
        /* RGBA8 (BC1 to BC3, BC7), one or two 16 bit channels (BC4, BC5), or halves */
        int sn = kind == BCN_BC4_SNORM || kind == BCN_BC5_SNORM, ch = pb == 2 ? 1 : 2;
        int hdr = kind == BCN_BC6H_UF || kind == BCN_BC6H_SF, bytes = kind <= BCN_BC3 || kind == BCN_BC7;
        /* BC7's bytes within half a step, as the device's unorm to float conversion may round q / 255 otherwise */
        float step = hdr ? 0 : kind == BCN_BC7 ? 0.5f / 255 : sn ? 2.0f / 127 : 2.0f / 255, worst = 0;
        u32 differ = 0;
        for (u32 i = 0; i < w * h; i++)
            for (int c = 0; c < 4; c++) {
                const u8* q = want + (size_t)i * pb;
                float e;
                if (hdr)
                    e = half_float(snx_ld_le16(q + 2 * c));
                else if (bytes)
                    e = q[c] / 255.0f;
                else if (c >= ch)
                    e = c == 3 ? 1.0f : 0.0f;
                else if (sn)
                    e = fmaxf((i16)snx_ld_le16(q + 2 * c) / 32767.0f, -1.0f);
                else
                    e = snx_ld_le16(q + 2 * c) / 65535.0f;
                float g = got[(size_t)i * 4 + c], d = fabsf(g - e);
                if (d > worst) worst = d;
                if (!(d <= step) && !(e != e && g != g)) { /* the same NaN aside */
                    if (differ++ < 4)
                        fprintf(stderr, "native BCn kind %d: texel %u channel %d: the device %.9g, the CPU %.9g\n",
                                kind, i, c, g, e);
                }
            }
        if (differ) fatal("native BCn kind %d: %u channels differ from the CPU's by more than allowed\n", kind, differ);
        printf("PASS native BCn kind %d (largest difference %.3g)\n", kind, worst);
        free(blocks);
        free(want);
        vk_image_destroy(src, src_mem);
        vk_image_destroy(dst, dst_mem);
    }
    fflush(stdout);
}

int main(int argc, char** argv) {
    int large = 0;
    /* --vk_max_allocation_mb=N as the runtime takes it (ctest's astc_vulkan_pieces: 4), and --large */
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--vk_max_allocation_mb=", 23)) g_settings.vulkan_allocation_cap = atoi(argv[i] + 23);
        else if (!strcmp(argv[i], "--large")) large = 1;
    init();
    if (large) {
        /* 512 MiB each once decoded, as some games' textures are: a square 2D one (11584 is a multiple of 4 and 8)
           and a cube of slices, both past the 256 MiB allocation cap, so their memory is in pieces where the device
           has sparse binding, and strips go through scratch, staging rings and readbacks of at most the cap */
        run(4, 4, 0, 11584, 11584, 1, 1, 1, 0, NULL, NULL);
        run(8, 8, 1, 512, 512, 512, 1, 1, 0, NULL, NULL);
        if (errors) fatal("%u validation errors\n", errors);
        puts("The 512 MiB ASTC textures decode as the CPU reference does");
        return 0;
    }
    /* block linear moves: whole GOBs, rows not a multiple of 16 or 64 bytes, partial GOB rows and blocks, bands that
       start inside a GOB, volumes whose depth isn't a multiple of their blocks' */
    for (u32 width = 1; width < 16; width++) bl_case(width, 3, 2, 0, 0, 2, 1);
    bl_case(64, 8, 1, 0, 0, 8, 0);
    bl_case(100, 37, 1, 2, 0, 37, 0);
    bl_case(100, 37, 1, 2, 0, 5, 0);
    bl_case(48, 1, 1, 0, 0, 1, 0);
    bl_case(4 * 97, 61, 1, 3, 0, 13, 0);
    bl_case(1280 * 4, 720, 1, 4, 0, 720, 0);
    bl_case(1280 * 4, 720, 1, 4, 0, 128, 0);
    bl_case(64 * 3 + 32, 19, 5, 1, 2, 19, 0);
    bl_case(64 * 3 + 32, 19, 5, 1, 2, 7, 1);
    bl_case(12 * 33, 70, 3, 2, 1, 70, 0);
    bl_case(2 * 1000, 300, 2, 5, 0, 300, 1);
    /* ASTC blocks coming block linear, deswizzled on the GPU before decoding */
    g_bl_source = 1;
    run(4, 4, 0, 4 * 67 - 1, 4 * 123 - 2, 1, 1, 1, 0, NULL, NULL);
    run(8, 6, 1, 17, 13, 1, 6, 5, 0, NULL, NULL);
    run(6, 6, 0, 301, 97, 37, 1, 1, 0, NULL, NULL);
    run(4, 4, 1, 2051, 1101, 1, 1, 1, 0, NULL, NULL);
    g_bl_source = 0;
    tc_case();
    bc_case();
    bc_native_case();
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
    /* a 3D texture whose slices share strips, and one past the cap (8.6 MiB: in pieces under ctest's
       --vk_max_allocation_mb=4) */
    run(6, 6, 0, 301, 97, 37, 1, 1, 0, NULL, NULL);
    run(4, 4, 1, 2051, 1101, 1, 1, 1, 0, NULL, NULL);
    if (errors) fatal("%u validation errors\n", errors);
    puts("All ASTC Vulkan comparisons passed (zero validation errors).");
    /* The backend's internal pipelines/rings have device lifetime, as in the
       runtime. Process exit releases these rather than testing a new teardown. */
    return 0;
}
