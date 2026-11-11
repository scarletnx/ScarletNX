/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Headless differential test of the actual Vulkan ASTC upload, block linear
   deswizzle and swizzle, frame/staging and allocation code (vk/astc.c,
   vk/blocklinear.c, vk/cmd.c, vk/memory.c) with the SPIR-V the build compiles
   from astc.comp and blocklinear.comp. No game assets. Run with
   VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation and
   VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
   to check synchronization too (docs/layers/gpu/astc.md). --vk_max_allocation_mb=N lowers
   the allocation cap, so images in pieces are tested at small sizes; --large
   decodes a 2D and a 3D texture of 512 MiB each. --upload-wait checks a BC7 upload across an internal submission.
   --upload-retire checks that completed uploads do not accumulate toward the encoder's wait threshold. */
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <process.h> /* _getpid */
#else
#include <unistd.h> /* getpid */
#endif
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "support/astc.h"
#include "nx/gpu/texture/blocklinear.h"
#include "support/etc2.h"
#include "support/bcn.h"
#include "bc7_cpu.h"
#include "bcn_etc2_cpu.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/nce/nce.h"
#include "nx/memory/own.h"
#include "nx/hle/services/nvdrv.h"

VkState g_vk;
VkSemaphore g_vk_csem;
u64 g_vk_cs_wait_value;
static unsigned errors;
/* uma.c isn't here: no guest memory read in place for cmd.c's draws to settle */
void vk_uma_settle(u64 ended) { (void)ended; }
/* Synthetic upload sources have no guest GPU mappings, ownership or pending in-place writes. */
u32 g_nown;
int vk_uma_pending(u64 gva, u64 size) { (void)gva; (void)size; return 0; }
u64 gmmu_run(u64 gva, u64 size, u64* cpu) { (void)gva; (void)size; *cpu = 0; return 0; }
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) {
    (void)addr; (void)size; (void)out; (void)max;
    return 0;
}
int own_trapped(u64 addr, u64 size) {
    (void)addr; (void)size;
    return 0;
}

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
void vk_render_end_at(const char* file, int line) {
    (void)file;
    (void)line;
}
/* surface.c isn't here: a surface's image moved into a layout for a use as surface.c moves it, from the layout, stages
   and accesses it holds (astc.c's background copies its pieces into the texture whose upload made them) */
void vk_surf_use(VkCommandBuffer cb, Surf* s, VkImageLayout layout, VkPipelineStageFlags2 stage,
                 VkAccessFlags2 access) {
    vk_barrier(cb, s->img, VK_IMAGE_ASPECT_COLOR_BIT, s->stage ? s->stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
               s->access, s->layout, stage, access, layout);
    s->layout = layout;
    s->stage = stage;
    s->access = access;
}
/* The standalone upload test has no guest buffer cache to flush or retire. */
void vk_buf_frame_done(u64 frame) { (void)frame; }
void vk_gfx_image_frame_done(u64 sub) { (void)sub; }
void vk_buf_host_barrier(VkCommandBuffer cb, u64 frame) {
    (void)cb;
    (void)frame;
}
VkPipelineCache vk_pipeline_cache(void) { return VK_NULL_HANDLE; }
static struct { u64 count, ns; } g_texture_pipelines;
void vk_texture_pipeline_made(u64 ns) {
    g_texture_pipelines.count++;
    g_texture_pipelines.ns += ns;
}
int vk_graphics_playout_is(VkPipelineLayout layout) { (void)layout; return 0; }
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
int vk_lock_held(void) { return 1; } /* cmd.c's wait for the next frame slot then waits under the lock, as before */
/* record.c resolves stand-ins for pipelines still being made (pipeline.c, not here): the test binds real ones */
VkPipeline vk_pipeline_resolve(VkPipeline h) { return h; }
VkPipeline vk_pipeline_now(VkPipeline h, int* pending) {
    *pending = 0;
    return h;
}
/* cmd.c reads the frame's GPU timestamps for gpuprof.c, which isn't here */
void vk_gpuprof_frame_done(u32 slot) { (void)slot; }
void vk_gpuprof_cb_begin(VkCommandBuffer cb) { (void)cb; }
void vk_gpuprof_cb_end(VkCommandBuffer cb, int why, u64 value) {
    (void)cb;
    (void)why;
    (void)value;
}
void vk_gpuprof_waited(u64 value, int kind) {
    (void)value;
    (void)kind;
}
/* and record.c's dispatches and copies, which it would time */
void vk_gpuprof_tag(u64 key) { (void)key; }
int vk_gpuprof_dispatch_begin(VkCommandBuffer cb) { return (void)cb, 0; }
void vk_gpuprof_dispatch_end(VkCommandBuffer cb) { (void)cb; }
int vk_gpuprof_profiled(VkCommandBuffer cb) { return (void)cb, 0; }
int vk_gpuprof_op_begin(VkCommandBuffer cb, u32 kind, u64 key, u64 bytes) {
    return (void)cb, (void)kind, (void)key, (void)bytes, 0;
}
void vk_gpuprof_op_end(VkCommandBuffer cb) { (void)cb; }
const void* vk_gpuprof_base(void) { return NULL; }
/* record.c's presents tell display.c they were made, which isn't here (nor are presents) */
void vk_display_made(u64 id, VkResult r) {
    (void)id;
    (void)r;
}
/* cmd.c's waits note themselves for gpu.c's stall report, which isn't here */
int gpu_wait_note(const char* what, u64 value) {
    (void)what;
    (void)value;
    return 0;
}
/* plat_posix.c's crash handler, which the test never installs, and the nce.c function it calls */
void log_crash_mode(void) {}
void nce_crash_context(const void* context, const void* fault) {
    (void)context;
    (void)fault;
}
void crash_report(void* host_pc, void* fault, int sig) {
    (void)host_pc;
    (void)fault;
    (void)sig;
}
int vk_dslayout_arrays(VkDescriptorSetLayout layout) {
    (void)layout;
    return 0;
}
void vk_pool_sizes(VkDescriptorPoolSize* sz, u32 sets, u32 array_sets) {
    (void)array_sets;
    sz[0] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 2};
    sz[1] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sets};
    sz[2] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets};
    sz[3] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets};
    sz[4] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, sets};
    sz[5] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sets * 2};
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
    g_vk.feat.textureCompressionETC2 = have.textureCompressionETC2; /* etc2_case too, of the BCn it encodes again */
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
        u8* p = vk_stage_upload(hi - lo, vk_stage_align(), &staged, &at);
        *buf = vk_bl_scratch((u64)l->nx * 16 * n * nz, &cap);
        *off = 0;
        if (!p || !*buf) return 0;
        memcpy(p, l->bl + lo, hi - lo);
        vk_bl_barrier(cb, 1);
        int ok = vk_bl_move(cb, &m, 0, staged, at, *buf, 0);
        vk_bl_barrier(cb, 0);
        return ok;
    }
    u8* p = vk_stage_upload((u64)n * nz * l->nx * 16, vk_stage_align(), buf, off);
    if (!p) return 0;
    for (u32 k = 0; k < nz; k++)
        memcpy(p + (size_t)k * n * l->nx * 16, l->blocks + ((size_t)(z + k) * l->ny + y) * l->nx * 16,
               (size_t)n * l->nx * 16);
    return 1;
}
/* each level of an array's layers uploaded at once, as texture.c uploads an array's ASTC levels
   (vk_astc_upload_layers): the layers' blocks one after another, as a volume's slices */
static int g_batch_layers;
static void upload_layers(Surf* t, u8** blocks, u32 w, u32 h, u32 layers, u32 levels, u32 bw, u32 bh) {
    for (u32 l = 0; l < levels; l++) {
        u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, nx = (lw + bw - 1) / bw, ny = (lh + bh - 1) / bh;
        size_t one = (size_t)nx * ny * 16;
        u8* all = malloc(one * layers);
        for (u32 layer = 0; layer < layers; layer++) memcpy(all + one * layer, blocks[layer * levels + l], one);
        Level lv = {all, nx, ny, NULL, 2, 0};
        if (!vk_astc_upload_layers(vk_cmd(), t, lw, lh, l, 0, layers, level_rows, &lv, NULL))
            fatal("layers upload failed\n");
        free(all);
    }
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
            if (!(g_batch_layers && layers > 1 && depth == 1) &&
                !vk_astc_upload(vk_cmd(), &t, 0, 0, lw, lh, ld, l, layer, level_rows, &lv, NULL))
                fatal("upload failed\n");
            free(bl); /* copied into staging already */
            bytes += (size_t)lw * lh * ld * 4;
        }
    if (g_batch_layers && layers > 1 && depth == 1) upload_layers(&t, blocks, w, h, layers, levels, bw, bh);
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

/* uploads random blocks into a BC7 image, as texture.c does where the GPU samples BCn but not ASTC (astc_mode=0), and
   compares each level's blocks with the CPU decoder's texels encoded by the same encoder run on the CPU
   (tests/glsl/bc7_cpu.cpp): the strips that start on whole 4x4 blocks, the second dispatch and the copy of the
   blocks, byte for byte */
static void run_bc7(u32 bw, u32 bh, int srgb, u32 w, u32 h, u32 depth, u32 layers, u32 levels) {
    const VkFormat format = srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    VkImageFormatProperties props;
    if (!g_vk.feat.textureCompressionBC ||
        vkGetPhysicalDeviceImageFormatProperties(
            g_vk.phys, format, depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &props) != VK_SUCCESS) {
        printf("SKIP ASTC as BC7 %ux%u image=%ux%ux%u: the device has no such BC7 image\n", bw, bh, w, h, depth);
        return;
    }
    Surf t = {
        .w = w, .h = h, .layers = layers, .levels = levels, .depth3d = depth, .f = {.vk = format, .bw = bw, .bh = bh}};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {w, h, depth},
                            .mipLevels = levels,
                            .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    if (!vk_image_create(&ii, &t.img, &t.mem)) fatal("cannot create a %ux%ux%u BC7 image\n", w, h, depth);
    barrier(t.img, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    u8** blocks = calloc((size_t)layers * levels, sizeof *blocks);
    for (u32 layer = 0; layer < layers; layer++)
        for (u32 l = 0; l < levels; l++) {
            u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = depth >> l ? depth >> l : 1;
            u8* b = blocks[layer * levels + l] = blocks_new(lw, lh, ld, bw, bh);
            Level lv = {b, (lw + bw - 1) / bw, (lh + bh - 1) / bh, NULL, 2, ld > 1};
            if (!(g_batch_layers && layers > 1 && depth == 1) &&
                !vk_astc_upload(vk_cmd(), &t, 0, 0, lw, lh, ld, l, layer, level_rows, &lv, NULL))
                fatal("BC7 upload failed\n");
        }
    if (g_batch_layers && layers > 1 && depth == 1) upload_layers(&t, blocks, w, h, layers, levels, bw, bh);
    barrier(t.img, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    for (u32 layer = 0; layer < layers; layer++)
        for (u32 l = 0; l < levels; l++) {
            u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = depth >> l ? depth >> l : 1;
            u32 nx = (lw + bw - 1) / bw, ny = (lh + bh - 1) / bh;
            u64 size = (u64)((lw + 3) / 4) * ((lh + 3) / 4) * 16 * ld;
            VkBuffer readback;
            u8* actual = vk_readback_buffer(size, &readback);
            if (!actual) fatal("no readback buffer of %llu bytes\n", (unsigned long long)size);
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                                      .imageExtent = {lw, lh, ld}};
            rec_CmdCopyImageToBuffer(vk_cmd(), t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
            vk_finish();
            vk_readback_ready();
            u8* texels = malloc((size_t)lw * lh * ld * 4);
            u8* expected = malloc(size);
            for (u32 z = 0; z < ld; z++)
                astc_decode_image(blocks[layer * levels + l] + (size_t)z * nx * ny * 16, lw, lh, bw, bh,
                                  texels + (size_t)z * lw * lh * 4, srgb);
            bc7_encode_cpu_build(vk_bc7_build(lw, lh, ld), texels, lw, lh, ld, expected); /* the level's build */
            for (u64 i = 0; i < size; i++)
                if (actual[i] != expected[i])
                    fatal("ASTC as BC7 %ux%u srgb=%d %ux%ux%u level %u layer %u: byte %llu (block %llu) expected %u "
                          "got %u\n",
                          bw, bh, srgb, lw, lh, ld, l, layer, (unsigned long long)i, (unsigned long long)(i / 16),
                          expected[i], actual[i]);
            free(texels);
            free(expected);
            free(blocks[layer * levels + l]);
        }
    free(blocks);
    vk_image_destroy(t.img, t.mem);
    printf("PASS ASTC as BC7 %ux%u srgb=%d image=%ux%ux%u layers=%u mips=%u\n", bw, bh, srgb, w, h, depth, layers,
           levels);
    fflush(stdout);
}

typedef struct {
    Level level;
    u32 calls, changes, begun;
    u64 first_submit;
} UploadWaitSource;
static int upload_wait_rows(void* ctx, VkCommandBuffer* pcb, u32 z, u32 nz, u32 y, u32 n,
                            VkBuffer* buf, u64* off) {
    UploadWaitSource* source = ctx;
    /* This callback deliberately does not repair its command buffer. The upload must pass a live one after
       its own wait, even when the source callback has no reason to acquire another recording. */
    if (*pcb != vk_cmd()) fatal("BC7 upload wait: rows received a submitted command buffer\n");
    u32 begun = vk_cb_begun();
    if (!source->calls) source->first_submit = vk_submit_number();
    else if (source->begun != begun) source->changes++;
    source->begun = begun;
    source->calls++;
    return level_rows(&source->level, pcb, z, nz, y, n, buf, off);
}
/* 262656 BC7 blocks: one strip reaches a 256Ki-block threshold (bc7_queue=256) with the default 16MiB scratch, then
   another strip must record into a new command buffer. Constant blocks keep the GPU encoder's first fit exact;
   the final block row has a different color, so a missing continuation cannot pass the output comparison. */
static int upload_wait_case(int retire) {
    const u32 w = retire ? 1024 : 2048, h = retire ? 1024 : 2052, nx = w / 4, ny = h / 4;
    g_settings.bc7_queue = 256; /* the threshold this case crosses, whatever Auto gives this GPU */
    VkImageFormatProperties props;
    if (!g_vk.feat.textureCompressionBC ||
        vkGetPhysicalDeviceImageFormatProperties(g_vk.phys, VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            0, &props) != VK_SUCCESS || props.maxExtent.width < w || props.maxExtent.height < h) {
        puts("SKIP BC7 upload wait: the device has no suitable BC7 image");
        return 77;
    }
    Surf t = {.w = w, .h = h, .layers = 1, .levels = 1, .depth3d = 1,
              .f = {.vk = VK_FORMAT_BC7_UNORM_BLOCK, .bw = 4, .bh = 4}};
    VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = t.f.vk, .extent = {w, h, 1}, .mipLevels = 1,
        .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    if (!vk_image_create(&image, &t.img, &t.mem)) fatal("BC7 upload wait: cannot create the target image\n");
    u8* blocks = malloc((size_t)nx * ny * 16);
    if (!blocks) fatal("BC7 upload wait: no source memory\n");
    const u8 header[8] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    for (u32 y = 0; y < ny; y++)
        for (u32 x = 0; x < nx; x++) {
            u8* block = blocks + ((size_t)y * nx + x) * 16;
            memcpy(block, header, sizeof header);
            memset(block + 8, y + 1 == ny ? 255 : 0, 8);
        }
    u8 expected[2][16];
    for (u32 color = 0; color < 2; color++) {
        u8 texels[64], decoded[64];
        memset(texels, color ? 255 : 0, sizeof texels);
        bc7_encode_cpu_build(vk_bc7_build(4, 4, 1), texels, 4, 4, 1, expected[color]);
        bcn_decode_image(expected[color], 4, 4, BCN_BC7, decoded);
        if (memcmp(texels, decoded, sizeof texels)) fatal("BC7 upload wait: constant-color reference is not exact\n");
    }
    UploadWaitSource source = {.level = {blocks, nx, ny, NULL, 0, 0}};
    barrier(t.img, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (u32 upload = 0; upload < (retire ? 5u : 1u); upload++) {
        if (upload)
            barrier(t.img, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        u64 before = vk_last_submit();
        if (!vk_astc_upload(vk_cmd(), &t, 0, 0, w, h, 1, 0, 0, upload_wait_rows, &source, NULL))
            fatal("BC7 upload wait: upload failed\n");
        /* Each upload is below the limit, but five exceed it together. Every preceding upload is complete:
           a stale queued-block count would force an extra submission on the fourth one. */
        if (retire) {
            if (vk_last_submit() != before) fatal("BC7 completed uploads caused an unnecessary submit at %u\n", upload);
            vk_finish();
        }
    }
    if (g_texture_pipelines.count != 2 || !g_texture_pipelines.ns)
        fatal("BC7 upload wait: internal pipeline counters missed creation or counted a reused pipeline\n");
    if (!retire && (source.calls < 2 || !source.changes || vk_submit_number() <= source.first_submit))
        fatal("BC7 upload wait: no continuation across a submission (%u calls, %u changes)\n",
            source.calls, source.changes);
    barrier(t.img, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    u64 row_bytes = (u64)nx * 16;
    u32 rows = (u32)((g_vk.alloc_cap < (16ull << 20) ? g_vk.alloc_cap : 16ull << 20) / row_bytes);
    if (!rows) fatal("BC7 upload wait: the readback cap holds no block row\n");
    for (u32 y = 0; y < ny; y += rows) {
        u32 count = ny - y < rows ? ny - y : rows;
        VkBuffer readback;
        u8* bytes = vk_readback_buffer(row_bytes * count, &readback);
        if (!bytes) fatal("BC7 upload wait: no readback memory\n");
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {0, (i32)(y * 4), 0}, .imageExtent = {w, count * 4, 1}};
        rec_CmdCopyImageToBuffer(vk_cmd(), t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        vk_finish();
        vk_readback_ready();
        for (u32 row = 0; row < count; row++)
            for (u32 x = 0; x < nx; x++)
                if (memcmp(bytes + ((size_t)row * nx + x) * 16, expected[y + row + 1 == ny], 16))
                    fatal("BC7 upload wait: block (%u,%u) differs\n", x, y + row);
    }
    free(blocks);
    vk_image_destroy(t.img, t.mem);
    if (errors) fatal("BC7 upload wait: %u validation errors\n", errors);
    printf("PASS BC7 upload %s: %u strips, %u command-buffer changes, %u exact blocks\n",
        retire ? "retire" : "wait", source.calls, source.changes, nx * ny);
    return 0;
}

/* A region of a level uploaded again over its whole upload, as texture.c loads the blocks of the pages that changed:
   rnx by rny new blocks at block (rx, ry) of a w x h level, its sides on 4x4 blocks of a BC7 image or at the level's
   edges. The image is then the CPU decoder's (and the encoder's, for BC7) of all the level's blocks as they are now,
   byte for byte: the BC7 blocks at the region's sides take the texels of the blocks beside it as the whole upload's
   do. A BC7 region whose side isn't on 4x4 blocks is refused (0), the image left as it was */
static void run_region(u32 bw, u32 bh, int bc7, u32 w, u32 h, u32 rx, u32 ry, u32 rnx, u32 rny) {
    const VkFormat format = bc7 ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM;
    if (bc7 && !g_vk.feat.textureCompressionBC) {
        printf("SKIP ASTC region %ux%u as BC7: the device has no BC7 images\n", bw, bh);
        return;
    }
    Surf t = {.w = w, .h = h, .layers = 1, .levels = 1, .depth3d = 1, .f = {.vk = format, .bw = bw, .bh = bh}};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    if (!vk_image_create(&ii, &t.img, &t.mem)) fatal("cannot create a %ux%u image\n", w, h);
    barrier(t.img, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const u32 nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh, x0 = rx * bw, y0 = ry * bh;
    const u32 rw = rnx * bw < w - x0 ? rnx * bw : w - x0, rh = rny * bh < h - y0 ? rny * bh : h - y0;
    u8* b = blocks_new(w, h, 1, bw, bh);
    Level lv = {b, nx, ny, NULL, 2, 0};
    if (!vk_astc_upload(vk_cmd(), &t, 0, 0, w, h, 1, 0, 0, level_rows, &lv, NULL)) fatal("upload failed\n");
    u8* fresh = blocks_new(rnx * bw, rny * bh, 1, bw, bh);
    Level rv = {fresh, rnx, rny, NULL, 2, 0};
    barrier(t.img, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    int aligned = !bc7 || (x0 % 4 == 0 && y0 % 4 == 0);
    if (vk_astc_upload(vk_cmd(), &t, x0, y0, rw, rh, 1, 0, 0, level_rows, &rv, NULL) != aligned)
        fatal("ASTC region %ux%u at block (%u, %u): the upload %s\n", bw, bh, rx, ry,
              aligned ? "was refused" : "of a region off 4x4 blocks went through");
    if (aligned)
        for (u32 y = 0; y < rny && ry + y < ny; y++)
            memcpy(b + ((size_t)(ry + y) * nx + rx) * 16, fresh + (size_t)y * rnx * 16,
                   (size_t)(rnx < nx - rx ? rnx : nx - rx) * 16);
    barrier(t.img, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    u64 size = bc7 ? (u64)((w + 3) / 4) * ((h + 3) / 4) * 16 : (u64)w * h * 4;
    VkBuffer readback;
    u8* actual = vk_readback_buffer(size, &readback);
    if (!actual) fatal("no readback buffer of %llu bytes\n", (unsigned long long)size);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
    vk_finish();
    vk_readback_ready();
    u8* texels = malloc((size_t)w * h * 4);
    u8* expected = bc7 ? malloc(size) : texels;
    astc_decode_image(b, w, h, bw, bh, texels, 0);
    if (bc7) bc7_encode_cpu_build(vk_bc7_build(w, h, 1), texels, w, h, 1, expected);
    for (u64 i = 0; i < size; i++)
        if (actual[i] != expected[i])
            fatal("ASTC region %ux%u%s of %ux%u at block (%u, %u), %ux%u blocks: byte %llu expected %u got %u\n", bw,
                  bh, bc7 ? " as BC7" : "", w, h, rx, ry, rnx, rny, (unsigned long long)i, expected[i], actual[i]);
    if (bc7) free(expected);
    free(texels);
    free(fresh);
    free(b);
    vk_image_destroy(t.img, t.mem);
    printf("PASS ASTC region %ux%u%s of %ux%u: %ux%u blocks at block (%u, %u) %s\n", bw, bh, bc7 ? " as BC7" : "", w, h,
           rnx, rny, rx, ry, aligned ? "as a whole upload makes them" : "refused, off 4x4 blocks");
    fflush(stdout);
}

/* --bench [file.astc]: one w x h level of ASTC blocks uploaded into an RGBA8 image and into a BC7 one, as texture.c
   does for Exact and BC7, each runs times: the time from the upload's start to the GPU's end of it, the texels and the
   BC7 blocks a second. The blocks are random 8x8 ones, or a file's (astcenc's .astc) repeated over the level: random
   blocks have every block mode, many partitions and HDR errors, where a game's textures are mostly void extents and
   one partition, and the encoder's time depends on what it encodes */
typedef struct {
    const char* name; /* NULL: random 8x8 blocks */
    u32 bw, bh;       /* the footprint */
} BenchSource;
static u8* bench_blocks(BenchSource* src, u32 w, u32 h) {
    if (!src->name) return blocks_new(w, h, 1, src->bw = 8, src->bh = 8);
    FILE* f = fopen(src->name, "rb");
    u8 hdr[16];
    if (!f || fread(hdr, 1, 16, f) != 16) fatal("cannot read the header of %s\n", src->name);
    const u32 sw = hdr[7] | hdr[8] << 8 | hdr[9] << 16, sh = hdr[10] | hdr[11] << 8 | hdr[12] << 16;
    if (hdr[4] < 4 || hdr[4] > 12 || hdr[5] < 4 || hdr[5] > 12 || !sw || !sh)
        fatal("%s: a %ux%u footprint over %ux%u texels, not a 2D LDR .astc of 4x4 to 12x12\n", src->name, hdr[4],
              hdr[5], sw, sh);
    const u32 snx = (sw + hdr[4] - 1) / hdr[4], sny = (sh + hdr[5] - 1) / hdr[5];
    u8* file = malloc((size_t)snx * sny * 16);
    if (!file || fread(file, 16, (size_t)snx * sny, f) != (size_t)snx * sny)
        fatal("%s holds fewer than its %u blocks\n", src->name, snx * sny);
    fclose(f);
    src->bw = hdr[4];
    src->bh = hdr[5];
    const u32 nx = (w + src->bw - 1) / src->bw, ny = (h + src->bh - 1) / src->bh;
    u8* b = malloc((size_t)nx * ny * 16);
    if (!b) fatal("no memory for %ux%u blocks\n", nx, ny);
    for (u32 y = 0; y < ny; y++)
        for (u32 x = 0; x < nx; x++)
            memcpy(b + ((size_t)y * nx + x) * 16, file + ((size_t)(y % sny) * snx + x % snx) * 16, 16);
    free(file);
    return b;
}
static void bench(u32 w, u32 h, int runs, BenchSource src) {
    const VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_BC7_SRGB_BLOCK};
    u8* b = bench_blocks(&src, w, h);
    for (int fi = 0; fi < 2; fi++) {
        if (fi && !g_vk.feat.textureCompressionBC) {
            puts("BENCH BC7: the device samples no BCn");
            continue;
        }
        Surf t = {.w = w,
                  .h = h,
                  .layers = 1,
                  .levels = 1,
                  .depth3d = 1,
                  .f = {.vk = formats[fi], .bw = src.bw, .bh = src.bh}};
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = formats[fi],
                                .extent = {w, h, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        if (!vk_image_create(&ii, &t.img, &t.mem)) fatal("cannot create a %ux%u bench image\n", w, h);
        if (t.mem) {
            VmaAllocationInfo allocation;
            vmaGetAllocationInfo(g_vk.vma, t.mem, &allocation);
            printf("BENCH memory %s %ux%u: %llu bytes\n", fi ? "BC7" : "RGBA8", w, h,
                   (unsigned long long)allocation.size);
        } else {
            VkMemoryRequirements requirements;
            vkGetImageMemoryRequirements(g_vk.dev, t.img, &requirements);
            printf("BENCH memory %s %ux%u: %llu bytes required across pieces\n", fi ? "BC7" : "RGBA8", w, h,
                   (unsigned long long)requirements.size);
        }
        barrier(t.img, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vk_finish();
        for (int r = 0; r < runs; r++) {
            Level lv = {b, (w + src.bw - 1) / src.bw, (h + src.bh - 1) / src.bh, NULL, 2, 0};
            u64 count = g_texture_pipelines.count, pipeline_ns = g_texture_pipelines.ns;
            u64 t0 = plat_ns();
            if (!vk_astc_upload(vk_cmd(), &t, 0, 0, w, h, 1, 0, 0, level_rows, &lv, NULL))
                fatal("bench upload failed\n");
            vk_finish();
            double s = (plat_ns() - t0) / 1e9;
            printf("BENCH ASTC %ux%u as %s %ux%u (BC7 build %u): %.1f ms, %.0f Mtexels/s, %.2f M BC7 blocks/s, %llu "
                   "internal pipelines (%.1f ms)\n",
                   src.bw, src.bh, fi ? "BC7" : "RGBA8", w, h, vk_bc7_build(w, h, 1), s * 1e3, (double)w * h / s / 1e6,
                   (double)((w + 3) / 4) * ((h + 3) / 4) / s / 1e6,
                   (unsigned long long)(g_texture_pipelines.count - count),
                   (double)(g_texture_pipelines.ns - pipeline_ns) / 1e6);
            fflush(stdout);
        }
        vk_image_destroy(t.img, t.mem);
    }
    free(b);
}

/* the cache file, this process's own: ctest runs the astc_vulkan entries at once in one directory (--parallel 4 on
   the CI), and one run's remove or lock took another's file */
static const char* cache_path(void) {
    static char path[64];
#if defined(_WIN32)
    if (!path[0]) snprintf(path, sizeof path, "astc_vulkan_test.%u.vkbc7", (unsigned)_getpid());
#else
    if (!path[0]) snprintf(path, sizeof path, "astc_vulkan_test.%u.vkbc7", (unsigned)getpid());
#endif
    return path;
}
#define CACHE_PATH cache_path()
/* The BC7 cache (vk/bc7_cache.c through vk_astc_upload's content): one level of random blocks, all its layers or
   slices at once, uploaded into a new BC7 image each time and read back whole. The first upload encodes, and its blocks
   go into the cache when its submission is done; the second, and a third after the cache's file is opened again as a
   later run does, encode nothing and come out byte for byte as the first; other blocks miss */
typedef struct {
    u32 bw, bh, srgb, w, h, depth, layers;
} CacheCase;
/* the level's blocks uploaded into a new image with the cache's key made of them, its blocks read back (the caller
   frees them); NULL when the device has no such image or the cache doesn't take the level. *took: the time the upload
   took, ns. drain: the background encodes run to their end first, while the image lives (they copy their pieces into
   it) */
static u8* cache_upload(const CacheCase* c, const u8* blocks, u64* took, int drain) {
    const VkFormat format = c->srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    VkImageFormatProperties props;
    const u32 slices = c->depth * c->layers, nx = (c->w + c->bw - 1) / c->bw, ny = (c->h + c->bh - 1) / c->bh;
    if (!g_vk.feat.textureCompressionBC || !(vk_astc_cached(c->w, c->h, slices) || vk_astc_quick(c->w, c->h, slices)) ||
        vkGetPhysicalDeviceImageFormatProperties(
            g_vk.phys, format, c->depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &props) != VK_SUCCESS)
        return NULL;
    Surf t = {.w = c->w,
              .h = c->h,
              .layers = c->layers,
              .levels = 1,
              .depth3d = c->depth,
              .type = c->depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
              .f = {.vk = format, .bw = c->bw, .bh = c->bh}};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = c->depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {c->w, c->h, c->depth},
                            .mipLevels = 1,
                            .arrayLayers = c->layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    if (!vk_image_create(&ii, &t.img, &t.mem)) fatal("cannot create a %ux%ux%u BC7 image\n", c->w, c->h, c->depth);
    barrier(t.img, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    /* the key as texture.c makes it: its layout's words, then its bytes */
    const u32 layout[8] = {0, nx * 16, nx * 16, ny, 0, 0, 0, c->layers};
    u64 content[2] = {0, 0};
    vk_bc7_content(content, layout, sizeof layout);
    vk_bc7_content(content, blocks, (u64)nx * ny * slices * 16);
    const u64 size = (u64)((c->w + 3) / 4) * ((c->h + 3) / 4) * 16 * slices;
    /* blocks of noise first: an image whose memory an earlier one of the same bytes had shows what is not written */
    VkBuffer noise_buf;
    u64 noise_at;
    u8* noise = vk_stage(size, 16, &noise_buf, &noise_at);
    if (!noise) fatal("no staging for %llu bytes\n", (unsigned long long)size);
    for (u64 i = 0; i < size; i += 4) {
        u32 r = random_word();
        memcpy(noise + i, &r, 4);
    }
    VkBufferImageCopy fill = {.bufferOffset = noise_at,
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, c->layers},
                              .imageExtent = {c->w, c->h, c->depth}};
    rec_CmdCopyBufferToImage(vk_cmd(), noise_buf, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &fill);
    barrier(t.img, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    Level lv = {blocks, nx, ny, NULL, 2, 0};
    t.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; /* as the barriers above left it, for vk_surf_use */
    t.stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    t.access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    u64 t0 = plat_ns();
    int ok = c->layers > 1 ? vk_astc_upload_layers(vk_cmd(), &t, c->w, c->h, 0, 0, c->layers, level_rows, &lv, content)
                           : vk_astc_upload(vk_cmd(), &t, 0, 0, c->w, c->h, c->depth, 0, 0, level_rows, &lv, content);
    if (!ok) fatal("cached upload failed\n");
    *took = plat_ns() - t0;
    if (drain) vk_astc_background_drain();
    vk_astc_forget(&t, ~0u); /* the image goes */
    barrier(t.img, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBuffer readback;
    u8* actual = vk_readback_buffer(size, &readback);
    if (!actual) fatal("no readback buffer of %llu bytes\n", (unsigned long long)size);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, c->layers},
                              .imageExtent = {c->w, c->h, c->depth}};
    rec_CmdCopyImageToBuffer(vk_cmd(), t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
    vk_finish();
    vk_readback_ready();
    u8* got = malloc((size_t)size);
    if (!got) fatal("no memory for %llu bytes\n", (unsigned long long)size);
    memcpy(got, actual, (size_t)size);
    vk_image_destroy(t.img, t.mem);
    return got;
}
/* levels uploaded as BC7 so far, and those of them the cache gave */
static void bc7_counts(u64* levels, u64* hits) {
    u64 bytes, rgba8;
    vk_astc_stats(levels, hits, &bytes, &rgba8);
}
static void cache_case(const CacheCase* c) {
    const u32 slices = c->depth * c->layers;
    u8* blocks = blocks_new(c->w, c->h, slices, c->bw, c->bh);
    u64 levels, hits, levels2, hits2, took, stored, stored2, reads, restarts, bytes, encodes = vk_astc_dispatches();
    bc7_counts(&levels, &hits);
    vk_bc7_cache_stats(&reads, &stored, &restarts, &bytes);
    u8* first = cache_upload(c, blocks, &took, 0);
    if (!first) {
        printf("SKIP the BC7 cache %ux%u image=%ux%ux%u: the device has no such BC7 image, or the cache does not take "
               "the level\n",
               c->bw, c->bh, c->w, c->h, c->depth);
        free(blocks);
        return;
    }
    bc7_counts(&levels2, &hits2);
    vk_bc7_cache_stats(&reads, &stored2, &restarts, &bytes);
    if (levels2 != levels + 1 || hits2 != hits || stored2 != stored + 1 || vk_astc_dispatches() == encodes)
        fatal("the BC7 cache %ux%u %ux%ux%u: the first upload took %llu levels, %llu hits, %llu stored, %llu encoder "
              "dispatches\n",
              c->bw, c->bh, c->w, c->h, slices, (unsigned long long)(levels2 - levels),
              (unsigned long long)(hits2 - hits), (unsigned long long)(stored2 - stored),
              (unsigned long long)(vk_astc_dispatches() - encodes));
    for (int run = 0; run < 2; run++) {
        if (run) vk_astc_cache_open(CACHE_PATH); /* a later run's */
        encodes = vk_astc_dispatches();
        u8* again = cache_upload(c, blocks, &took, 0);
        bc7_counts(&levels, &hits);
        if (!again || levels != levels2 + 1 || hits != hits2 + 1 || vk_astc_dispatches() != encodes)
            fatal("the BC7 cache %ux%u %ux%ux%u: upload %d took %llu levels, %llu hits, %llu encoder dispatches\n",
                  c->bw, c->bh, c->w, c->h, slices, run + 2, (unsigned long long)(levels - levels2),
                  (unsigned long long)(hits - hits2), (unsigned long long)(vk_astc_dispatches() - encodes));
        const u64 size = (u64)((c->w + 3) / 4) * ((c->h + 3) / 4) * 16 * slices;
        for (u64 i = 0; i < size; i++)
            if (again[i] != first[i])
                fatal("the BC7 cache %ux%u %ux%ux%u: upload %d's byte %llu is %u, the encode's %u\n", c->bw, c->bh,
                      c->w, c->h, slices, run + 2, (unsigned long long)i, again[i], first[i]);
        free(again);
        levels2 = levels, hits2 = hits;
    }
    /* another block: another key, encoded */
    blocks[16 * (slices > 1 ? 1 : 0) + 5] ^= 0x40;
    free(cache_upload(c, blocks, &took, 0));
    bc7_counts(&levels, &hits);
    if (levels != levels2 + 1 || hits != hits2)
        fatal("the BC7 cache %ux%u %ux%ux%u: other blocks hit\n", c->bw, c->bh, c->w, c->h, slices);
    free(first);
    free(blocks);
    printf("PASS the BC7 cache %ux%u srgb=%u image=%ux%ux%u layers=%u: hits copy the encode's blocks, no encode\n",
           c->bw, c->bh, c->srgb, c->w, c->h, c->depth, c->layers);
    fflush(stdout);
}
/* a burst: more levels encoded in one submission than the 64 captures the slots start with, each its own image, one
   finish after them all: every one is stored (a game's menus upload hundreds of levels at once) */
static void cache_burst(void) {
    enum { N = 100, W = 16, H = 16 };
    const VkFormat format = VK_FORMAT_BC7_UNORM_BLOCK;
    if (!g_vk.feat.textureCompressionBC) return;
    Surf* t = calloc(N, sizeof *t);
    u64 reads, stored, stored2, restarts, bytes;
    if (!t) fatal("no memory for %d surfaces\n", N);
    vk_bc7_cache_stats(&reads, &stored, &restarts, &bytes);
    for (u32 i = 0; i < N; i++) {
        t[i] = (Surf){.w = W, .h = H, .layers = 1, .levels = 1, .depth3d = 1, .f = {.vk = format, .bw = 4, .bh = 4}};
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = format,
                                .extent = {W, H, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        if (!vk_image_create(&ii, &t[i].img, &t[i].mem)) fatal("cannot create a %ux%u BC7 image\n", W, H);
        barrier(t[i].img, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        u8* blocks = blocks_new(W, H, 1, 4, 4);
        const u32 layout[8] = {0, W / 4 * 16, W / 4 * 16, H / 4, 0, 0, 0, 1};
        u64 content[2] = {0, 0};
        vk_bc7_content(content, layout, sizeof layout);
        vk_bc7_content(content, blocks, (u64)(W / 4) * (H / 4) * 16);
        Level lv = {blocks, W / 4, H / 4, NULL, 2, 0};
        if (!vk_astc_upload(vk_cmd(), &t[i], 0, 0, W, H, 1, 0, 0, level_rows, &lv, content))
            fatal("burst upload %u failed\n", i);
        free(blocks);
    }
    vk_finish(); /* the captures land once their submission is done */
    vk_bc7_cache_stats(&reads, &stored2, &restarts, &bytes);
    for (u32 i = 0; i < N; i++) vk_image_destroy(t[i].img, t[i].mem);
    free(t);
    if (stored2 != stored + N)
        fatal("the BC7 cache: a burst of %d encodes stored %llu\n", N, (unsigned long long)(stored2 - stored));
    printf("PASS the BC7 cache: a burst of %d encodes in one submission, every one stored\n", N);
    fflush(stdout);
}
static void cache_cases(void) {
    remove(CACHE_PATH);
    vk_astc_cache_open(CACHE_PATH);
    /* strips of 4, 2 and 1 block rows; layers and slices; many strips under --vk_max_allocation_mb=4, and two strips
       without it (texels past the 16 MiB scratch) */
    static const CacheCase cases[] = {
        {6, 6, 0, 97, 61, 1, 1},      {5, 5, 1, 131, 77, 1, 1},     {8, 6, 0, 17, 13, 1, 6},    {4, 4, 1, 61, 37, 9, 1},
        {10, 5, 0, 2051, 1101, 1, 1}, {10, 5, 1, 4100, 1030, 1, 1}, {8, 8, 1, 2048, 2048, 1, 1}};
    for (u32 i = 0; i < sizeof cases / sizeof cases[0]; i++) cache_case(&cases[i]);
    cache_burst();
    vk_bc7_cache_close();
    remove(CACHE_PATH);
}

/* Very large levels (astc_quick_mtexels, 1 megatexel here): the first upload encodes the quick build (mode 6 from one
   line) and gives the level to the background, which stores its fast build in pieces; the next upload, and one after
   the cache's file is opened again, copy the pieces, which are the fast build of the whole level byte for byte. The
   workers on the CPU (astc_background=1) and the GPU (2) store the same bytes. An image kept while the background
   runs gets its pieces too, and so does one whose hit ran out of time (g_vk_astc_hit_ns 0), loading them. A background stopped after one piece (the GPU's, which makes one at a time) leaves that
   piece, and the next one makes only the others. Bands of block rows (one of them stepping 3 rows, for 6x6 blocks) and
   pieces of several layers */
static void bg_expect(const CacheCase* c, const u8* blocks, u8* quick, u8* fast) {
    const u32 slices = c->depth * c->layers, nx = (c->w + c->bw - 1) / c->bw, ny = (c->h + c->bh - 1) / c->bh;
    u8* texels = malloc((size_t)c->w * c->h * slices * 4);
    for (u32 z = 0; z < slices; z++)
        astc_decode_image(blocks + (size_t)z * nx * ny * 16, c->w, c->h, c->bw, c->bh,
                          texels + (size_t)z * c->w * c->h * 4, (int)c->srgb);
    bc7_encode_cpu_quick(texels, c->w, c->h, slices, quick);
    bc7_encode_cpu_fast(texels, c->w, c->h, slices, fast);
    free(texels);
}
static void bg_same(const CacheCase* c, int mode, const char* what, const u8* got, const u8* want, u64 size) {
    for (u64 i = 0; i < size; i++)
        if (got[i] != want[i])
            fatal("background %d %ux%u %ux%ux%u: %s's byte %llu (block %llu) is %u, not %u\n", mode, c->bw, c->bh, c->w,
                  c->h, c->layers, what, (unsigned long long)i, (unsigned long long)(i / 16), got[i], want[i]);
}
static void background_case(const CacheCase* c, int mode) {
    const u32 slices = c->depth * c->layers;
    const u64 size = (u64)((c->w + 3) / 4) * ((c->h + 3) / 4) * 16 * slices;
    u8 *blocks = blocks_new(c->w, c->h, slices, c->bw, c->bh), *quick = malloc(size), *fast = malloc(size);
    u64 took;
    VkAstcBackground b0, b;
    remove(CACHE_PATH);
    vk_astc_cache_open(CACHE_PATH);
    g_settings.astc_quick_mtexels = 1;
    g_settings.astc_background = mode;
    bg_expect(c, blocks, quick, fast);
    vk_astc_background_stats(&b0);
    u8* got = cache_upload(c, blocks, &took, 0);
    if (!got) {
        printf("SKIP the background %d %ux%u image=%ux%ux%u: the device has no such BC7 image\n", mode, c->bw, c->bh,
               c->w, c->h, slices);
        goto out;
    }
    bg_same(c, mode, "the quick encode", got, quick, size);
    free(got);
    vk_astc_background_stats(&b);
    if (b.quick != b0.quick + 1 || b.made != b0.made + 1 || b.hits != b0.hits)
        fatal("background %d: the first upload was %llu quick, %llu to the background, %llu hits\n", mode,
              (unsigned long long)(b.quick - b0.quick), (unsigned long long)(b.made - b0.made),
              (unsigned long long)(b.hits - b0.hits));
    vk_astc_background_drain();
    vk_astc_background_stats(&b);
    const u64 pieces = b.stored - b0.stored;
    if (b.finished != b0.finished + 1 || b.pending || !pieces || (mode == 2) != (b.gpu_pieces > b0.gpu_pieces))
        fatal("background %d: %llu finished, %llu pending, %llu pieces stored, %llu by the GPU\n", mode,
              (unsigned long long)(b.finished - b0.finished), (unsigned long long)b.pending, (unsigned long long)pieces,
              (unsigned long long)(b.gpu_pieces - b0.gpu_pieces));
    for (int run = 0; run < 2; run++) {
        if (run) vk_astc_cache_open(CACHE_PATH); /* a later run's */
        vk_astc_background_stats(&b0);
        got = cache_upload(c, blocks, &took, 0);
        vk_astc_background_stats(&b);
        if (b.hits != b0.hits + 1 || b.quick != b0.quick || b.made != b0.made)
            fatal("background %d: upload %d of the stored level encoded\n", mode, run + 2);
        bg_same(c, mode, run ? "the hit after reopening" : "the hit", got, fast, size);
        free(got);
    }
    /* a hit out of time (a slow disk): encoded quickly, and the pieces load into the image after it */
    g_vk_astc_hit_ns = 0;
    vk_astc_background_stats(&b0);
    got = cache_upload(c, blocks, &took, 1);
    vk_astc_background_stats(&b);
    g_vk_astc_hit_ns = ~0ull;
    if (b.quick != b0.quick + 1 || b.hits != b0.hits || b.stored != b0.stored || b.loaded != b0.loaded + pieces)
        fatal("background %d: a hit out of time was %llu quick, %llu hits, %llu pieces stored, %llu loaded\n", mode,
              (unsigned long long)(b.quick - b0.quick), (unsigned long long)(b.hits - b0.hits),
              (unsigned long long)(b.stored - b0.stored), (unsigned long long)(b.loaded - b0.loaded));
    bg_same(c, mode, "the image the pieces loaded into", got, fast, size);
    free(got);
    /* another level, its image alive until the background is done: the pieces go into it as they come */
    blocks[9] ^= 0x20;
    bg_expect(c, blocks, quick, fast);
    got = cache_upload(c, blocks, &took, 1);
    bg_same(c, mode, "the image the background wrote into", got, fast, size);
    free(got);
    if (mode == 2) {
        blocks[5] ^= 0x40; /* another level */
        bg_expect(c, blocks, quick, fast);
        vk_astc_background_stats(&b0);
        free(cache_upload(c, blocks, &took, 0));
        vk_finish();
        vk_astc_background(); /* the snapshot is whole: the first piece goes to the GPU */
        vk_finish();
        g_settings.astc_background = 0;
        vk_astc_background(); /* it is stored, and the level is dropped */
        vk_astc_background_stats(&b);
        if (b.stored != b0.stored + 1 || b.pending || b.finished != b0.finished)
            fatal("background stopped after a piece: %llu stored, %llu pending\n",
                  (unsigned long long)(b.stored - b0.stored), (unsigned long long)b.pending);
        g_settings.astc_background = 2;
        free(cache_upload(c, blocks, &took, 0));
        vk_astc_background_drain();
        vk_astc_background_stats(&b);
        if (b.stored != b0.stored + pieces || b.finished != b0.finished + 1)
            fatal("background after a stop: %llu pieces stored of %llu\n", (unsigned long long)(b.stored - b0.stored),
                  (unsigned long long)pieces);
        got = cache_upload(c, blocks, &took, 0);
        bg_same(c, mode, "the hit after a stop", got, fast, size);
        free(got);
    }
    printf("PASS the background %s %ux%u srgb=%u image=%ux%u layers=%u: the quick build first, then the fast build "
           "from the cache in %llu pieces\n",
           mode == 1 ? "CPU" : "GPU", c->bw, c->bh, c->srgb, c->w, c->h, c->layers, (unsigned long long)pieces);
    fflush(stdout);
out:
    g_settings.astc_quick_mtexels = 0;
    g_settings.astc_background = 0;
    vk_bc7_cache_close();
    remove(CACHE_PATH);
    free(blocks);
    free(quick);
    free(fast);
}
static void background_cases(void) {
    g_vk_astc_hit_ns = ~0ull; /* the test's frame never ends, so its hits' time adds up: unbounded but where 0 */
    static const CacheCase cases[] = {
        {4, 4, 0, 1030, 1030, 1, 1}, {6, 6, 1, 1540, 700, 1, 1}, {4, 4, 1, 260, 260, 1, 16}};
    for (u32 i = 0; i < sizeof cases / sizeof cases[0]; i++)
        for (int mode = 1; mode <= 2; mode++) background_case(&cases[i], mode);
}

/* --bench: the BC7 cache's costs for one w x h level of random 8x8 blocks: an encode with its blocks read back and
   stored, then hits, from the upload's start to the GPU's end and a readback */
static void bench_cache(u32 w, u32 h, int runs) {
    if (!g_vk.feat.textureCompressionBC) return;
    remove(CACHE_PATH);
    vk_astc_cache_open(CACHE_PATH);
    const CacheCase c = {8, 8, 1, w, h, 1, 1};
    u8* blocks = blocks_new(w, h, 1, 8, 8);
    for (int r = 0; r <= runs; r++) {
        u64 took, t0 = plat_ns();
        free(cache_upload(&c, blocks, &took, 0));
        printf("BENCH BC7 cache %ux%u %s: the upload %.2f ms on the CPU, %.1f ms to the GPU's end and a readback\n", w,
               h, r ? "hit" : "encode and store", took / 1e6, (plat_ns() - t0) / 1e6);
        fflush(stdout);
    }
    free(blocks);
    vk_bc7_cache_close();
    remove(CACHE_PATH);
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
   bcn.c's. BC6H's halves and BC7's bytes are exact by the specification. BC1 to BC5 leave the interpolants' precision
   to the device. An NVIDIA GPU decodes as the Switch's does, bcn_decode_image's values: its bytes and unsigned 16 bit
   values are exact, its signed ones within a step. Another GPU is held to the exact values rounded
   (bcn_decode_image_spec), within two steps (llvmpipe divides BC3's alpha by 5 as x * 51 >> 8: 918 / 5 is 183.6,
   which rounds to 184, and llvmpipe gives 182). This tells a reference drifted from what GPUs decode, which the
   comparison of texconv with the reference can't */
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
        int nv = g_vk.props.vendorID == 0x10DE;
        (nv ? bcn_decode_image : bcn_decode_image_spec)(blocks, w, h, kind, want);
        /* RGBA8 (BC1 to BC3, BC7), one or two 16 bit channels (BC4, BC5), or halves */
        int sn = kind == BCN_BC4_SNORM || kind == BCN_BC5_SNORM, ch = pb == 2 ? 1 : 2;
        int hdr = kind == BCN_BC6H_UF || kind == BCN_BC6H_SF, bytes = kind <= BCN_BC3 || kind == BCN_BC7;
        /* exact bytes within half a step, as the device's unorm to float conversion may round q / 255 otherwise */
        float step = 2.0f / (sn ? 127 : 255), worst = 0;
        if (hdr)
            step = 0;
        else if (bytes && (nv || kind == BCN_BC7))
            step = 0.5f / 255;
        else if (nv)
            step = sn ? 1.5f / 32767 : 0.5f / 65535;
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

/* the ETC2 or EAC format a BCn kind is encoded again into where the device samples ETC2 and not BCn (format.c's TB),
   and what it is decoded into elsewhere */
static VkFormat etc2_of(int kind) {
    static const VkFormat f[10] = {VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK,
                                   VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
                                   VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
                                   VK_FORMAT_EAC_R11_UNORM_BLOCK,
                                   VK_FORMAT_EAC_R11_SNORM_BLOCK,
                                   VK_FORMAT_EAC_R11G11_UNORM_BLOCK,
                                   VK_FORMAT_EAC_R11G11_SNORM_BLOCK,
                                   VK_FORMAT_UNDEFINED,
                                   VK_FORMAT_UNDEFINED,
                                   VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK};
    return f[kind];
}
static VkFormat decoded_of(int kind) {
    static const VkFormat f[10] = {VK_FORMAT_R8G8B8A8_UNORM,      VK_FORMAT_R8G8B8A8_UNORM,
                                   VK_FORMAT_R8G8B8A8_UNORM,      VK_FORMAT_R16_UNORM,
                                   VK_FORMAT_R16_SNORM,           VK_FORMAT_R16G16_UNORM,
                                   VK_FORMAT_R16G16_SNORM,        VK_FORMAT_R16G16B16A16_SFLOAT,
                                   VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM};
    return f[kind];
}
static int etc_kind_of(int kind) {
    static const int k[10] = {ETC2_RGB_PTA, ETC2_RGBA,    ETC2_RGBA, EAC_R, EAC_R_SNORM,
                              EAC_RG,       EAC_RG_SNORM, 0,         0,     ETC2_RGBA};
    return k[kind];
}
/* bw x bh blocks of kind through texconv (vk_tc_convert) as f says, into a readback buffer of out_size bytes */
static u8* tc_blocks(const VkFmt* f, const u8* blocks, u32 w, u32 bw, u32 bh, u64* out_size) {
    u32 bb = f->bpp, pitch = (bw * bb + 15) / 16 * 16;
    u64 at, cap;
    VkBuffer staged, rb;
    u8* p = vk_stage((u64)pitch * bh, vk_stage_align(), &staged, &at);
    for (u32 y = 0; y < bh; y++) memcpy(p + (u64)y * pitch, blocks + (size_t)y * bw * bb, (size_t)bw * bb);
    VkTcShape s = vk_tc_shape(f, w);
    *out_size = vk_tc_out_size(&s, bh, 1);
    VkBuffer scratch = vk_bl_scratch(*out_size, &cap);
    u8* out = vk_readback_buffer(*out_size, &rb);
    vk_bl_barrier(vk_cmd(), 1);
    if (!vk_tc_convert(vk_cmd(), f, w, bh, 1, staged, at, pitch, scratch, 0)) fatal("conversion refused\n");
    vk_bl_barrier(vk_cmd(), 0);
    VkBufferCopy c = {0, 0, *out_size};
    rec_CmdCopyBuffer(vk_cmd(), scratch, rb, 1, &c);
    vk_finish();
    vk_readback_ready();
    return out;
}
/* where the device samples ETC2: its own decoding of the blocks the transcoder wrote (w x h, whole blocks), blitted
   into RGBA32F, against tests/support/etc2.c's. ETC2's bytes are exact by the specification, EAC's 11 bits within half
   a step of their 16 bit widening */
static void etc2_native(int kind, const u8* etc, u32 w, u32 h) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, etc2_of(kind), &fp);
    if (!g_vk.feat.textureCompressionETC2 || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
        printf("SKIP native ETC2 of BCn kind %d: the device doesn't sample it or can't blit from it\n", kind);
        return;
    }
    u32 n = w / 4 * (h / 4), bb = bcn_block_bytes(kind);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = etc2_of(kind),
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage src, dst;
    VmaAllocation src_mem, dst_mem;
    if (!vk_image_create(&ii, &src, &src_mem)) fatal("cannot create an ETC2 image\n");
    ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    if (!vk_image_create(&ii, &dst, &dst_mem)) fatal("cannot create an RGBA32F image\n");
    u64 at;
    VkBuffer staged, rb;
    memcpy(vk_stage((u64)n * bb, vk_stage_align(), &staged, &at), etc, (size_t)n * bb);
    barrier(src, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barrier(dst, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy up = {
        .bufferOffset = at, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    rec_CmdCopyBufferToImage(vk_cmd(), staged, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
    barrier(src, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {(i32)w, (i32)h, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {(i32)w, (i32)h, 1}}};
    rec_CmdBlitImage(vk_cmd(), src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                     &blit, VK_FILTER_NEAREST);
    barrier(dst, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    float* got = (float*)vk_readback_buffer((u64)w * h * 16, &rb);
    VkBufferImageCopy down = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &down);
    vk_finish();
    vk_readback_ready();
    int ek = etc_kind_of(kind), sn = ek == EAC_R_SNORM || ek == EAC_RG_SNORM,
        ch = ek == EAC_R || ek == EAC_R_SNORM ? 1 : 2;
    u32 pb = etc2_pixel_bytes(ek), differ = 0;
    u8* want = malloc((size_t)w * h * pb);
    etc2_decode_image(etc, w, h, ek, want);
    float step = ek <= ETC2_RGBA ? 0.5f / 255 : sn ? 1.5f / 32767 : 1.5f / 65535, worst = 0;
    for (u32 i = 0; i < w * h; i++)
        for (int c = 0; c < 4; c++) {
            const u8* q = want + (size_t)i * pb;
            float e = ek <= ETC2_RGBA ? q[c] / 255.0f
                      : c >= ch       ? (c == 3 ? 1.0f : 0.0f)
                      : sn            ? fmaxf((i16)snx_ld_le16(q + 2 * c) / 32767.0f, -1.0f)
                                      : snx_ld_le16(q + 2 * c) / 65535.0f;
            float d = fabsf(got[(size_t)i * 4 + c] - e);
            if (d > worst) worst = d;
            if (d > step && differ++ < 4)
                fprintf(stderr, "native ETC2 of BCn kind %d: texel %u channel %d: the device %.9g, the CPU %.9g\n",
                        kind, i, c, got[(size_t)i * 4 + c], e);
        }
    if (differ)
        fatal("native ETC2 of BCn kind %d: %u channels differ from the CPU's by more than allowed\n", kind, differ);
    printf("PASS native ETC2 of BCn kind %d (largest difference %.3g)\n", kind, worst);
    free(want);
    vk_image_destroy(src, src_mem);
    vk_image_destroy(dst, dst_mem);
}
/* texconv.comp's ETC2 build on the device against the CPU's run of it (tests/glsl/bcn_etc2_cpu.cpp): random blocks of
   every kind but BC6H, which has no ETC2 counterpart, in an image whose sides aren't multiples of 4, byte for byte;
   then the device's own decoding of them, where it samples ETC2 */
static void etc2_case(void) {
    u32 w = 67, h = 37, bw = 17, bh = 10;
    for (int kind = BCN_BC1; kind <= BCN_BC7; kind++) {
        if (kind == BCN_BC6H_UF || kind == BCN_BC6H_SF) continue;
        u32 bb = bcn_block_bytes(kind);
        u8* blocks = malloc((size_t)bw * bh * bb);
        bc_blocks(kind, blocks, bw * bh);
        VkFmt f = {.vk = etc2_of(kind), .bpp = (u8)bb, .bw = 4, .bh = 4, .conv = (u8)(VKC_BC1 + kind)};
        u64 out_size;
        u8* out = tc_blocks(&f, blocks, w, bw, bh, &out_size);
        u8* want = malloc((size_t)bw * bh * bb);
        bcn_etc2_cpu(kind, blocks, bw, bh, 1, want);
        if (out_size != (u64)bw * bh * bb)
            fatal("texconv's ETC2 build: %llu bytes out, not %u\n", (unsigned long long)out_size, bw * bh * bb);
        for (u32 i = 0; i < bw * bh; i++)
            if (memcmp(out + (size_t)i * bb, want + (size_t)i * bb, bb))
                fatal("texconv's ETC2 build: BCn kind %d block %u differs from the CPU's\n", kind, i);
        etc2_native(kind, want, 64, 40);
        free(blocks);
        free(want);
    }
    puts("PASS texture conversions (BC1 to BC5 and BC7 encoded again as ETC2 and EAC)");
    fflush(stdout);
}
/* --bench-etc2: a 2048x2048 level of random blocks of each kind decoded (as on a GPU that samples neither BCn nor
   ETC2) and encoded again as ETC2 or EAC, the conversion's dispatch alone, runs times each; and the bytes each holds.
   Random blocks take every candidate (no block of one alpha or one colour stops early): a game's take less */
static void bench_etc2(int runs) {
    const u32 w = 2048, h = 2048, bw = w / 4, bh = h / 4;
    static const int kinds[5] = {BCN_BC1, BCN_BC3, BCN_BC4, BCN_BC5, BCN_BC7};
    for (int ki = 0; ki < 5; ki++) {
        int kind = kinds[ki];
        u32 bb = bcn_block_bytes(kind);
        u8* blocks = malloc((size_t)bw * bh * bb);
        bc_blocks(kind, blocks, bw * bh);
        for (int etc = 0; etc < 2; etc++) {
            VkFmt f = {.vk = etc ? etc2_of(kind) : decoded_of(kind),
                       .bpp = (u8)bb,
                       .bw = 4,
                       .bh = 4,
                       .conv = (u8)(VKC_BC1 + kind)};
            for (int r = 0; r < runs; r++) {
                u64 out_size, t0 = plat_ns();
                tc_blocks(&f, blocks, w, bw, bh, &out_size);
                double ms = (plat_ns() - t0) / 1e6;
                printf("BENCH BCn kind %d %ux%u %s: %.1f ms, %llu bytes held (%.2f bytes a texel)\n", kind, w, h,
                       etc ? "encoded again as ETC2/EAC" : "decoded", ms, (unsigned long long)out_size,
                       (double)out_size / ((double)w * h));
                fflush(stdout);
            }
        }
        free(blocks);
    }
}

int main(int argc, char** argv) {
    int large = 0, upload_wait = 0;
    BenchSource bench_src = {NULL, 8, 8};
    /* --vk_max_allocation_mb=N as the runtime takes it (ctest's astc_vulkan_pieces: 4), and --large */
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--vk_max_allocation_mb=", 23)) g_settings.vulkan_allocation_cap = atoi(argv[i] + 23);
        else if (!strcmp(argv[i], "--large")) large = 1;
        else if (!strcmp(argv[i], "--bench")) {
            large = 2;
            if (i + 1 < argc && argv[i + 1][0] != '-') bench_src.name = argv[++i];
        } else if (!strcmp(argv[i], "--upload-wait"))
            upload_wait = 1;
        else if (!strcmp(argv[i], "--upload-retire"))
            upload_wait = 2;
        else if (!strcmp(argv[i], "--bench-etc2"))
            large = 3;
    init();
    /* every level as before (the fast build, the cache's whole levels): background_cases turns the quick encode on */
    g_settings.astc_quick_mtexels = 0;
    g_settings.astc_background = 0;
    if (upload_wait) return upload_wait_case(upload_wait == 2);
    if (large == 3) {
        bench_etc2(3);
        return 0;
    }
    if (large == 2) {
        bench(4096, 4096, 5, bench_src);
        bench(2040, 2040, 5, bench_src);
        bench_cache(4096, 4096, 3);
        bench_cache(2040, 2040, 3);
        bench_cache(256, 256, 3);
        return 0;
    }
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
    etc2_case();
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
    /* ASTC re-encoded as BC7: footprints whose strips step 4, 2 and 1 block rows at a time to start on whole 4x4
       blocks (heights 5, 6 and 8 or 12), sizes not a multiple of either, mips, layers, slices, and an image of
       many strips (more under ctest's --vk_max_allocation_mb=4) */
    run_bc7(4, 4, 0, 97, 61, 1, 1, 1);
    run_bc7(5, 5, 1, 97, 61, 1, 1, 1);
    run_bc7(6, 6, 0, 17, 13, 1, 6, 5);
    run_bc7(8, 5, 1, 17, 13, 1, 6, 5);
    run_bc7(10, 6, 0, 131, 77, 1, 1, 3);
    run_bc7(12, 12, 1, 97, 61, 1, 1, 1);
    run_bc7(6, 6, 0, 61, 37, 9, 1, 1);
    run_bc7(5, 5, 0, 2051, 1101, 1, 1, 1);
    /* every level takes the fast build, the largest mode 6 alone (vk_bc7_build): the levels above held to it */
    if (vk_bc7_build(4, 4, 1) != 1 || vk_bc7_build(2040, 2040, 1) != 1 || vk_bc7_build(8191, 8192, 1) != 1 ||
        vk_bc7_build(8192, 8192, 1) != 2 || vk_bc7_build(512, 512, 256) != 2)
        fatal("BC7 builds: 4x4 %u, 2040x2040 %u, 8191x8192 %u, 8192x8192 %u, 512x512x256 %u (1 the fast, 2 mode 6)\n",
              vk_bc7_build(4, 4, 1), vk_bc7_build(2040, 2040, 1), vk_bc7_build(8191, 8192, 1),
              vk_bc7_build(8192, 8192, 1), vk_bc7_build(512, 512, 256));
    run_bc7(8, 8, 1, 2048, 2048, 1, 1, 1);
    /* arrays' layers uploaded a level at a time, all of them at once (texture.c's ASTC arrays, vk_astc_upload_layers):
       RGBA8 and BC7, strips stepping 1, 2 and 4 block rows */
    g_batch_layers = 1;
    run(6, 6, 0, 17, 13, 1, 6, 5, 0, NULL, NULL);
    run(4, 4, 1, 97, 61, 1, 5, 3, 0, NULL, NULL);
    run_bc7(6, 6, 0, 17, 13, 1, 6, 5);
    run_bc7(5, 5, 1, 97, 61, 1, 4, 2);
    run_bc7(8, 8, 0, 96, 96, 1, 144, 3);
    g_batch_layers = 0;
    cache_cases();
    background_cases();
    /* a region uploaded again: inside the level, at its right and bottom edges, as BC7 and as RGBA8, footprints that
       step 1, 2 and 4 blocks to a 4x4 BC7 block, and a BC7 region off those blocks */
    run_region(4, 4, 1, 97, 61, 2, 1, 3, 2);
    run_region(5, 5, 1, 97, 61, 4, 4, 8, 4);
    run_region(5, 5, 1, 97, 61, 16, 12, 4, 4);
    run_region(6, 6, 1, 97, 61, 2, 2, 4, 2);
    run_region(10, 6, 1, 97, 61, 2, 2, 2, 4);
    run_region(12, 12, 1, 97, 61, 1, 1, 2, 1);
    run_region(8, 8, 1, 2051, 1101, 64, 32, 64, 32);
    run_region(6, 6, 0, 97, 61, 1, 1, 3, 2);
    run_region(6, 6, 1, 97, 61, 1, 1, 3, 2);
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
