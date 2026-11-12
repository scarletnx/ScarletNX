/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ASTC LDR uploads on the graphics queue. The caller hands the compressed blocks
   over a strip at a time as rows of blocks in a buffer: block linear ones are
   deswizzled on the GPU (vk/blocklinear.c), pitch linear ones staged a row at a
   time. The shader writes packed RGBA8 to device memory, then a transfer copies those bytes to the
   image (including sRGB images, without an unwanted gamma conversion). Strips
   bound scratch memory, staging and SSBO ranges, so a level of any size goes
   through: one of a 512 MiB texture is 32 strips. A strip is block rows of one
   slice, or whole slices when they fit, so a 3D texture's thin slices share a
   dispatch and a copy instead of taking one each.

   A BC7 image (format.c's TA, astc_mode's Auto, on a GPU that samples BCn but not ASTC) takes another shader in the
   decoder's place: astc_bc7.comp makes a strip's ASTC blocks straight into BC7 blocks, a quarter of RGBA8's bytes,
   in one dispatch, and those are what the copy writes. No texel is written out between the two: decoding into RGBA8
   and encoding from it was two dispatches with four bytes a texel written and read back between them. Its strips
   start on whole 4x4 blocks. A level of VK_BC7_FAST_TEXELS or more takes the
   encoder's fast build, which tries every mode only on the blocks mode 6 does not hold closely (the full encoder on
   every block mode 6 does not hold exactly), and one of VK_BC7_MODE6_TEXELS or more mode 6 alone: a game's sparse
   texture of seven mapped layers of 8192x8192 took the full encoder 14 seconds on a GeForce RTX 2080, which mode 6
   alone encodes at 1200 megatexels a second and the fast build at 65 to 1400 (one of its normal maps, its albedo).

   A region of a level goes the same way (texture.c loads the blocks of the pages that changed): its texels from its
   own blocks, at its place in the image. A BC7 block is encoded from its 16 texels alone, a strip's last texel
   repeated past its edge, so a region whose sides are on 4x4 blocks, or the level's own edges, encodes the blocks the
   whole level's upload does. The build goes by the level's size, not the region's, so a level's blocks are encoded
   alike however it loads (vk_bc7_build).

   The blocks of each whole level encoded also go into the BC7 cache (bc7_cache.c): copied into the frame's readback
   ring strip by strip, and stored once their submission is done (vk_astc_frame_done). A later upload of the same ASTC
   bytes copies them from the cache into the image, with no decode and no encode (bc7_from_cache), and so does
   texture.c's encoder between frames (vk_bc7_cache_level, vk_bc7_capture_level).

   A texture held as BC7 goes up as RGBA8 first, which the game samples at once, and texture.c has its BC7 image made
   between frames (vk_tex_later_frame): the texture keeps the ASTC blocks its RGBA8 image was decoded from on the GPU
   (vk_astc_keep), and vk_bc7_encode_kept makes a strip of them BC7 the same way, so the BC7 blocks are the ones an
   upload straight to BC7 makes.

   A very large level held as BC7 at load (astc_quick_mtexels, every layer counted) takes the quick build, mode 6 from
   one line, where the frame needs it, and its fast build is made in the background, on the GPU, into the
   cache in pieces and into the texture (below bc7_queued_limit). */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/texture/astc_spv.h" /* compiled from astc.comp by the build (cmake/glsl.cmake) */
#include "nx/gpu/texture/astc_bc7_spv.h" /* compiled from astc_bc7.comp, in each of the encoder's builds */
#include "nx/gpu/texture/blocklinear_spv.h"
#include "nx/gpu/texture/astc_bc.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* encodes' blocks on their way to the BC7 cache: in the readback ring of the frame they were recorded in, each until
   its submission is done (vk_astc_frame_done) */
typedef struct {
    u64 key[2];
    u8* back;
    VmaAllocation mem;
    u64 off, size;
    u64 sub; /* the submission that copies them: 0, a free slot */
} Bc7Capture;
/* Kept for the device lifetime, like the other internal pipelines. All uses are on g_vk.gfx, under vk_lock(), even
   when uploads originate in compute */
typedef struct {
    VkDescriptorSetLayout ds;
    VkPipelineLayout layout;
    VkShaderModule module;
    /* a pipeline per footprint (4 to 12 texels each way), made on first use: the footprint is a specialization
       constant, so the driver folds what depends on it however little it optimizes otherwise */
    VkPipeline pipes[9][9];
    VkBuffer scratch;
    VmaAllocation mem;
    u64 cap;
    /* the BC7 encoder, made on the first BC7 image, and the blocks it writes: as large as the texels' scratch, which a
       strip's blocks never pass (vk_astc_upload sizes strips by both) */
    VkPipelineLayout bc7_layout;
    /* its builds (vk_bc7_build): the full one, the fast one, mode 6 alone, and the quick one of a very large level
       (mode 6 from one line), each made on its first use */
    VkShaderModule bc7_modules[4];
    VkPipeline bc7_pipes[4];
    VkBuffer bc7_scratch;
    VmaAllocation bc7_mem;
    /* image levels (or regions of them) uploaded as BC7, those the cache gave, their bytes, and what they would have
       taken as RGBA8 (vk_astc_stats) */
    u64 bc7_levels, bc7_hits, bc7_bytes, bc7_rgba8_bytes;
    /* the encoder's dispatches (vk_astc_dispatches) */
    u64 bc7_dispatches;
    /* encodes' blocks copied into the readback ring, each until its submission is done (vk_astc_frame_done), and
       their bytes: at most VK_BC7_CAPTURE_BYTES, which bounds the ring's growth. As many as that holds: 64 at a time
       dropped two of three encodes in one game's menus, whose textures come in bursts of hundreds of levels */
    Bc7Capture* captures;
    u32 ncaptures;
    u64 captured;
    /* the encoder's blocks queued since the GPU was last waited for, and the submission that will include the last
       one counted (bc7_queued_limit). Completed uploads do not contribute to the limit: retaining their blocks
       across frames made a small later upload flush and wait for unrelated graphics work */
    struct {
        u64 blocks, until;
    } bc7_queued;
    /* the bytes of the ASTC levels decoded to RGBA8 (vk_astc_rgba8_bytes) */
    u64 rgba8_bytes;
    /* the bytes of the ASTC blocks textures keep for their BC7 images, made between frames (vk_astc_kept_bytes) */
    u64 kept_bytes;
} Astc;
static Astc g_astc;
/* The encoder is slow: a small GPU takes about a second over 256 thousand blocks, so a load of many textures queued up
   tens of seconds of it ahead of the GPU, and the device was lost in most runs, with no error in the validation layer
   and no fault in the kernel's log. Past this many blocks an upload waits for the GPU to do them. A wait costs little,
   as the game is held by what the GPU has to do anyway. Splitting them over submissions with no wait did not help */
static u64 bc7_queued_limit(void) {
    static u64 limit;
    if (!limit) {
        /* the GPU's own memory, its biggest device-local heap; an integrated GPU shares the CPU's */
        VkPhysicalDeviceMemoryProperties mp;
        u64 heap = 0;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
        for (u32 h = 0; h < mp.memoryHeapCount; h++)
            if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[h].size > heap)
                heap = mp.memoryHeaps[h].size;
        const int shared = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        limit = snx_bc7_queue_blocks(g_settings.bc7_queue, heap, shared);
        LOG("vulkan: the BC7 encoder queues up to %llu thousand blocks before an upload waits for the GPU (bc7_queue=%d%s, "
            "%llu MB of the GPU's own memory%s)\n",
            (unsigned long long)(limit >> 10), g_settings.bc7_queue, g_settings.bc7_queue ? "" : ", Auto",
            (unsigned long long)(heap >> 20), shared ? ", shared with the CPU" : "");
    }
    return limit;
}

/* astc.comp's push constants: the strip's width and its slices' height in texels and in blocks, and whether to use
   the specification's sRGB decode mode, whose void extents' R, G and B bytes differ from the UNORM mode's. An SRGB
   image (the texture header's sRGB bit, vk_tic_format) gets the sRGB mode, so the bytes are those an sRGB ASTC
   texture decodes to on the console. Then the tile's shape (tile_shift). */
typedef struct {
    u32 w, h, blocks_x, blocks_y, srgb, tile_shift;
} AstcParams;

/* A workgroup decodes a tile of 64 blocks (astc_decode.glsl), 1 << shift wide: the shape that leaves the fewest of
   a strip's slots empty, the widest of equal ones, as its rows of texels are the longest runs of each store */
static u32 tile_shift(u32 nx, u32 n) {
    u32 best = 6;
    u64 best_slots = ~0ull;
    for (u32 s = 7; s-- > 0;) {
        u32 tw = 1u << s, th = 64u >> s;
        u64 slots = (u64)((nx + tw - 1) / tw) * tw * ((n + th - 1) / th) * th;
        if (slots < best_slots) {
            best_slots = slots;
            best = s;
        }
    }
    return best;
}

static void astc_init(void) {
    if (g_astc.module) return;
    if (!VK_OK(snx_vk_compute_layouts(g_vk.dev, 2, 0, sizeof(AstcParams), &g_astc.ds, &g_astc.layout)))
        fatal("vulkan: cannot create the ASTC descriptor and pipeline layouts");
    if (!VK_OK(snx_vk_shader_module(g_vk.dev, astc_spv, astc_spv_size, &g_astc.module)))
        fatal("vulkan: cannot load the ASTC shader");
    g_astc.cap = 16ull << 20; /* a 2048x2048 slice a strip: fewer barriers between decoding and copying */
    if (g_astc.cap > g_vk.props.limits.maxStorageBufferRange) g_astc.cap = g_vk.props.limits.maxStorageBufferRange;
    if (g_astc.cap > g_vk.alloc_cap) g_astc.cap = g_vk.alloc_cap;
    /* also a copy's destination: an RGBA8 image's texels for the BC7 encoder (vk_bc7_encode_image) */
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = g_astc.cap,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vk_buffer_create(g_vk.vma, &bi, &ai, &g_astc.scratch, &g_astc.mem, NULL)))
        fatal("vulkan: cannot allocate ASTC scratch memory");
    vk_mem_track(g_astc.mem);
}

/* astc_bc7.comp's push constants: the strip as astc.comp has it, then the footprint, which is no constant of its
   pipeline, with what the decoder divides by it worked out (the reciprocals of its sides, the weight grid's infill
   steps), and the strip's first block in the input */
typedef struct {
    u32 w, h, blocks_x, blocks_y, srgb, bw, bh, recip_w, recip_h, infill_ds, infill_dt, in_base;
} AstcBc7Params;

static void bc7_init(void) {
    if (g_astc.bc7_layout) return;
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcBc7Params)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_astc.ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_astc.bc7_layout)))
        fatal("vulkan: cannot create the BC7 encoder's pipeline layout");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = g_astc.cap,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vk_buffer_create(g_vk.vma, &bi, &ai, &g_astc.bc7_scratch, &g_astc.bc7_mem, NULL)))
        fatal("vulkan: cannot allocate the BC7 encoder's scratch memory");
    vk_mem_track(g_astc.bc7_mem);
}

u32 vk_bc7_build(u32 w, u32 h, u32 d) {
    const u64 texels = (u64)w * h * d;
    return texels >= VK_BC7_MODE6_TEXELS ? 2 : texels >= VK_BC7_FAST_TEXELS ? 1 : 0;
}
/* the encoder's pipeline of build `build` (vk_bc7_build), made the first time it is asked for: a run that loads no
   level of a build compiles none of it */
static VkPipeline bc7_pipe(u32 build) {
    /* each build spelled out, and with SPIR-V's packed dot product (bc7_encode.glsl's Dot4), for a GPU that has an
       instruction for it (g_vk.bc7_dot): the same blocks either way */
    static const struct {
        const u32* code;
        const u64* size;
        const char* name;
    } builds[2][4] = {{{astc_bc7_spv, &astc_bc7_spv_size, "full"},
                       {astc_bc7_fast_spv, &astc_bc7_fast_spv_size, "fast"},
                       {astc_bc7_mode6_spv, &astc_bc7_mode6_spv_size, "mode 6"},
                       {astc_bc7_quick_spv, &astc_bc7_quick_spv_size, "quick"}},
                      {{astc_bc7_dot_spv, &astc_bc7_dot_spv_size, "full, dot products"},
                       {astc_bc7_fast_dot_spv, &astc_bc7_fast_dot_spv_size, "fast, dot products"},
                       {astc_bc7_mode6_dot_spv, &astc_bc7_mode6_dot_spv_size, "mode 6, dot products"},
                       {astc_bc7_quick_dot_spv, &astc_bc7_quick_dot_spv_size, "quick, dot products"}}};
    const int dot = g_vk.bc7_dot != 0;
    if (g_astc.bc7_pipes[build]) return g_astc.bc7_pipes[build];
    if (!VK_OK(snx_vk_shader_module(g_vk.dev, builds[dot][build].code, *builds[dot][build].size,
                                    &g_astc.bc7_modules[build])))
        fatal("vulkan: cannot load the BC7 encoder's %s build", builds[dot][build].name);
    u64 started = plat_ns();
    if (!VK_OK(snx_vk_module_pipeline(g_vk.dev, vk_pipeline_cache(), g_astc.bc7_layout, g_astc.bc7_modules[build], NULL,
                                      &g_astc.bc7_pipes[build])))
        fatal("vulkan: cannot create the BC7 encoder's %s build", builds[dot][build].name);
    vk_texture_pipeline_made(plat_ns() - started);
    return g_astc.bc7_pipes[build];
}

/* The cache's version: whatever changes the blocks the same ASTC bytes become, other than what each key holds. The
   SPIR-V of the deswizzle and of the three builds of astc_bc7.comp whose blocks it keeps (not the quick one's, which
   it never takes), which the build makes from the shaders (an edit to any changes it); the encoder's version; and the
   GPU and its driver, whose compilers run that SPIR-V */
void vk_astc_cache_open(const char* path) {
    const VkPhysicalDeviceProperties* d = &g_vk.props;
    const int dot = g_vk.bc7_dot != 0; /* the builds this device runs */
    const u64 spirv[4] = {vk_hash(blocklinear_spv, blocklinear_spv_size),
                          dot ? vk_hash(astc_bc7_dot_spv, astc_bc7_dot_spv_size)
                              : vk_hash(astc_bc7_spv, astc_bc7_spv_size),
                          dot ? vk_hash(astc_bc7_fast_dot_spv, astc_bc7_fast_dot_spv_size)
                              : vk_hash(astc_bc7_fast_spv, astc_bc7_fast_spv_size),
                          dot ? vk_hash(astc_bc7_mode6_dot_spv, astc_bc7_mode6_dot_spv_size)
                              : vk_hash(astc_bc7_mode6_spv, astc_bc7_mode6_spv_size)};
    char uuid[33], key[256];
    for (int i = 0; i < 16; i++) snprintf(uuid + 2 * i, 3, "%02x", d->pipelineCacheUUID[i]);
    int n = snprintf(key, sizeof key, "bc7 encoder=%d spirv=%016llx gpu=%04x:%04x driver=%u uuid=%s",
                     VK_BC7_ENCODER_VERSION, (unsigned long long)vk_hash(spirv, sizeof spirv), d->vendorID, d->deviceID,
                     d->driverVersion, uuid);
    u64 version[2];
    vk_hash128(key, n > 0 && (size_t)n < sizeof key ? (u64)n : 0, version);
    /* encodes still on their way belong to the file open when they were recorded, not to this one: dropped (their
       levels are encoded again when they come back). Kept, they landed in the new file at its first landing: a
       headless test that opened its own file found the levels a texture of the checks before it had encoded, 10 more
       than its own 9, because the landing that would have stored them in the old file had not come yet */
    for (u32 i = 0; i < g_astc.ncaptures; i++) g_astc.captures[i].sub = 0;
    g_astc.captured = 0;
    vk_bc7_cache_open(path, version, VK_BC7_CACHE_MAX);
}

/* the last submission vk_astc_frame_done had: what the background waits for is done up to it */
static u64 g_bg_landed;
/* the BC7 blocks of a level of w x h texels and depth slices or layers */
static u64 level_blocks(u32 w, u32 h, u32 depth) { return (u64)((w + 3) / 4) * ((h + 3) / 4) * depth * 16; }
/* a level of that many texels, all its layers or slices counted, is a very large one (astc_quick_mtexels) */
static int quick_level(u64 texels) {
    const u64 m = g_settings.astc_quick_mtexels > 0 ? (u64)g_settings.astc_quick_mtexels : 0;
    return m && texels >= m << 20;
}

int vk_astc_cached(u32 w, u32 h, u32 depth) {
    u64 hits, stored, restarts, bytes;
    vk_bc7_cache_stats(&hits, &stored, &restarts, &bytes);
    const u64 n = level_blocks(w, h, depth);
    return bytes && n <= VK_BC7_CAPTURE_BYTES && n <= g_vk.alloc_cap;
}
int vk_astc_quick(u32 w, u32 h, u32 depth) {
    u64 hits, stored, restarts, bytes;
    vk_bc7_cache_stats(&hits, &stored, &restarts, &bytes);
    return bytes && quick_level((u64)w * h * depth); /* its pieces, whatever its size */
}

void vk_bc7_level_key(const u64 content[2], u32 w, u32 h, u32 depth, u32 layers, u32 bw, u32 bh, int srgb, u32 build,
                      u64 key[2]) {
    const VkBc7Level lv = {w, h, depth, layers, bw, bh, (u32)srgb, build};
    vk_bc7_key(content, &lv, key);
}

/* astc_dump: each whole level's blocks as the decoder reads them (deswizzled on the GPU, so the CPU never has them in
   rows), copied strip by strip into the frame's readback ring and written into the level's .astc file once their
   submission is done. DUMP_MAX bytes a run, a debugging aid */
#define DUMP_MAX (2ull << 30)
typedef struct {
    u8* p;
    VmaAllocation mem;
    u64 off, size, sub, at;
    u32 file;
} AstcDump;
static struct {
    AstcDump* d;
    u32 n, cap, files;
    u64 bytes;
} g_dump;
static void dump_path(u32 file, char* path, size_t n) {
    snprintf(path, n, "%s/%06u.astc", g_settings.astc_dump, file);
}
/* a new level's file with astcenc's header: the footprint, then width, height and depth in 24 bits each. Its number,
   or ~0 when the dump is off, full, or the file can't be made */
static u32 dump_begin(u32 w, u32 h, u32 depth, u32 bw, u32 bh, u64 bytes) {
    char path[1100];
    if (!g_settings.astc_dump[0] || g_dump.bytes + bytes > DUMP_MAX) return ~0u;
    const u8 hdr[16] = {0x13, 0xAB, 0xA1, 0x5C, (u8)bw, (u8)bh, 1, (u8)w, (u8)(w >> 8), (u8)(w >> 16), (u8)h,
                        (u8)(h >> 8), (u8)(h >> 16), (u8)depth, (u8)(depth >> 8), (u8)(depth >> 16)};
    dump_path(g_dump.files, path, sizeof path);
    FILE* f = fopen(path, "wb");
    if (!f || fwrite(hdr, 1, 16, f) != 16) {
        LOG_LIMIT(1, "vulkan: astc_dump cannot write %s\n", path);
        if (f) fclose(f);
        return ~0u;
    }
    fclose(f);
    g_dump.bytes += bytes;
    return g_dump.files++;
}
/* a strip's blocks, size bytes at off in buf, read back for file `file`, where they go at byte `at` of its blocks */
static void dump_strip(VkCommandBuffer cb, u32 file, VkBuffer buf, u64 off, u64 size, u64 at) {
    VkBuffer back;
    AstcDump d = {.size = size, .at = at, .file = file};
    if (file == ~0u || !(d.p = vk_stage_back(size, &back, &d.off, &d.mem))) return;
    if (g_dump.n == g_dump.cap) {
        u32 cap = g_dump.cap ? g_dump.cap * 2 : 64;
        AstcDump* grown = realloc(g_dump.d, cap * sizeof *grown);
        if (!grown) return;
        g_dump.d = grown;
        g_dump.cap = cap;
    }
    const VkBufferCopy r = {off, d.off, size};
    rec_CmdCopyBuffer(cb, buf, back, 1, &r);
    VkBufferMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .buffer = back,
                                .offset = d.off,
                                .size = size};
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &dep);
    d.sub = vk_submit_number();
    g_dump.d[g_dump.n++] = d;
}
static void dump_landed(u64 sub) {
    u32 kept = 0;
    for (u32 i = 0; i < g_dump.n; i++) {
        AstcDump* d = &g_dump.d[i];
        if (d->sub > sub) {
            g_dump.d[kept++] = *d;
            continue;
        }
        char path[1100];
        dump_path(d->file, path, sizeof path);
        FILE* f = fopen(path, "r+b");
        vmaInvalidateAllocation(g_vk.vma, d->mem, d->off, d->size);
        if (f && !fseek(f, (long)(16 + d->at), SEEK_SET)) fwrite(d->p, 1, d->size, f);
        if (f) fclose(f);
    }
    g_dump.n = kept;
}

void vk_astc_frame_done(u64 sub) {
    if (sub > g_bg_landed) g_bg_landed = sub;
    if (g_dump.n) dump_landed(sub);
    for (u32 i = 0; g_astc.captured && i < g_astc.ncaptures; i++) {
        Bc7Capture* c = &g_astc.captures[i];
        if (!c->sub || c->sub > sub) continue;
        vmaInvalidateAllocation(g_vk.vma, c->mem, c->off, c->size);
        vk_bc7_cache_put(c->key, c->back, c->size);
        g_astc.captured -= c->size;
        c->sub = 0;
    }
}

/* a level's blocks from the cache (size bytes of them) staged and copied into level `level` of img, w x h texels and
   depth slices, or with layered depth layers from layer: 0 when they aren't there, staging has no room, or they don't
   check out (then dropped) */
static int bc7_from_cache(VkCommandBuffer cb, VkImage img, const u64 key[2], u64 size, u32 w, u32 h, u32 depth,
                          u32 level, u32 layer, int layered) {
    VkBuffer buf;
    u64 off;
    u8* p;
    if (vk_bc7_cache_find(key) != size || !(p = vk_stage(size, 16, &buf, &off)) || !vk_bc7_cache_read(key, p, size))
        return 0;
    VkBufferImageCopy copy = {.bufferOffset = off,
                              .bufferRowLength = (w + 3) / 4 * 4,
                              .bufferImageHeight = (h + 3) / 4 * 4,
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, layered ? depth : 1},
                              .imageExtent = {w, h, layered ? 1 : depth}};
    rec_CmdCopyBufferToImage(cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    g_astc.bc7_hits++;
    return 1;
}

/* a free slot for an encode's blocks, size bytes of them, with room taken in the readback ring (its buffer in *buf): -1
   when there is no slot or room, the bound is reached, or the same blocks are on their way already */
static int bc7_capture(const u64 key[2], u64 size, VkBuffer* buf) {
    int slot = -1;
    if (g_astc.captured + size > VK_BC7_CAPTURE_BYTES) return -1;
    for (u32 i = 0; i < g_astc.ncaptures; i++) {
        const Bc7Capture* c = &g_astc.captures[i];
        if (!c->sub && slot < 0)
            slot = (int)i;
        else if (c->sub && c->key[0] == key[0] && c->key[1] == key[1])
            return -1;
    }
    if (slot < 0) {
        u32 n = g_astc.ncaptures ? g_astc.ncaptures * 2 : 64;
        Bc7Capture* grown = realloc(g_astc.captures, (size_t)n * sizeof *grown);
        if (!grown) return -1;
        for (u32 i = g_astc.ncaptures; i < n; i++) grown[i] = (Bc7Capture){0};
        slot = (int)g_astc.ncaptures;
        g_astc.captures = grown;
        g_astc.ncaptures = n;
    }
    Bc7Capture* c = &g_astc.captures[slot];
    c->back = vk_stage_back(size, buf, &c->off, &c->mem);
    return c->back ? slot : -1;
}
/* the blocks capture slot took are the host's to read once the submission recording now is done: a barrier to the host
   after the copies into them, and the slot keyed */
static void bc7_capture_done(VkCommandBuffer cb, int slot, VkBuffer buf, const u64 key[2], u64 size) {
    Bc7Capture* c = &g_astc.captures[slot];
    VkBufferMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .buffer = buf,
                                .offset = c->off,
                                .size = size};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
    c->key[0] = key[0];
    c->key[1] = key[1];
    c->size = size;
    c->sub = vk_submit_number();
    g_astc.captured += size;
}

int vk_bc7_cache_level(VkCommandBuffer cb, VkImage img, const u64 key[2], u32 level, u32 w, u32 h, u32 slices,
                       int is3d) {
    return bc7_from_cache(cb, img, key, level_blocks(w, h, slices), w, h, slices, level, 0, !is3d);
}

int vk_bc7_capture_level(VkCommandBuffer cb, VkImage img, const u64 key[2], u32 level, u32 w, u32 h, u32 slices,
                         int is3d) {
    const u64 size = level_blocks(w, h, slices);
    VkBuffer buf;
    if (vk_bc7_cache_find(key)) return 1;
    const int slot = bc7_capture(key, size, &buf);
    if (slot < 0) return 0;
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img,
                               .subresourceRange = range};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
    VkBufferImageCopy copy = {.bufferOffset = g_astc.captures[slot].off,
                              .bufferRowLength = (w + 3) / 4 * 4,
                              .bufferImageHeight = (h + 3) / 4 * 4,
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, is3d ? 1 : slices},
                              .imageExtent = {w, h, is3d ? slices : 1}};
    rec_CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
    /* the level back to where the encoder's copies leave the rest of the image */
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    rec_CmdPipelineBarrier2(cb, &d);
    bc7_capture_done(cb, slot, buf, key, size);
    return 1;
}

void vk_astc_stats(u64* levels, u64* hits, u64* bytes, u64* rgba8_bytes) {
    *hits = g_astc.bc7_hits;
    *levels = g_astc.bc7_levels;
    *bytes = g_astc.bc7_bytes;
    *rgba8_bytes = g_astc.bc7_rgba8_bytes;
}
u64 vk_astc_rgba8_bytes(void) { return g_astc.rgba8_bytes; }
u64 vk_astc_dispatches(void) { return g_astc.bc7_dispatches; }

static VkPipeline astc_pipeline(u32 bw, u32 bh) {
    VkPipeline* pipe = &g_astc.pipes[bw - 4][bh - 4];
    if (*pipe) return *pipe;
    const u32 dims[2] = {bw, bh};
    const VkSpecializationMapEntry entries[2] = {{0, 0, sizeof(u32)}, {1, sizeof(u32), sizeof(u32)}};
    VkSpecializationInfo spec = {.mapEntryCount = 2, .pMapEntries = entries, .dataSize = sizeof dims, .pData = dims};
    u64 started = plat_ns();
    if (!VK_OK(snx_vk_module_pipeline(g_vk.dev, vk_pipeline_cache(), g_astc.layout, g_astc.module, &spec, pipe)))
        fatal("vulkan: cannot create the ASTC compute pipeline");
    vk_texture_pipeline_made(plat_ns() - started);
    return *pipe;
}

/* the stages and accesses of a scratch buffer's uses: a shader writing it, a shader reading it, a copy reading it */
enum { USE_WRITE, USE_READ, USE_COPY };
static const VkPipelineStageFlags2 use_stage[3] = {
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COPY_BIT};
static const VkAccessFlags2 use_access[3] = {VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                             VK_ACCESS_2_TRANSFER_READ_BIT};

/* orders use `to` of a scratch buffer after use `from`, or after either reader with from_readers */
static void scratch_barrier(VkCommandBuffer cb, VkBuffer buf, int from, int from_readers, int to) {
    VkBufferMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = from_readers ? use_stage[USE_READ] | use_stage[USE_COPY] : use_stage[from],
        .srcAccessMask = from_readers ? use_access[USE_READ] | use_access[USE_COPY] : use_access[from],
        .dstStageMask = use_stage[to],
        .dstAccessMask = use_access[to],
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buf,
        .size = VK_WHOLE_SIZE};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}

static VkDescriptorSet two_buffers(VkBuffer in, u64 in_off, u64 in_size, VkBuffer out, u64 out_size) {
    VkDescriptorSet set = vk_desc_alloc(g_astc.ds);
    if (!set) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo buffers[2] = {{in, in_off, in_size}, {out, 0, out_size}};
    snx_vk_storage_buffers(g_vk.dev, set, buffers, 2);
    return set;
}

/* A strip's ASTC blocks straight into BC7 blocks, one dispatch of astc_bc7.comp in the encoder's build pipe, an
   invocation a BC7 block: nx x n ASTC blocks a slice of nz slices, w x height texels a slice (16384 a side at most:
   the shader's division by the footprint), footprint bw x bh, from block in_base of set's input into the encoder's
   scratch buffer from its start, which a copy reads after. The strip starts on a row of 4x4 blocks */
static void bc7_transcode(VkCommandBuffer cb, VkDescriptorSet set, VkPipeline pipe, u32 w, u32 height, u32 nx, u32 n,
                          u32 nz, u32 bw, u32 bh, u32 srgb, u32 in_base, int tag) {
    const u32 cx = (w + 3) / 4, cy = (height + 3) / 4;
    const AstcBc7Params p = {w,
                             height,
                             nx,
                             n,
                             srgb,
                             bw,
                             bh,
                             (262144u + bw - 1) / bw,
                             (262144u + bh - 1) / bh,
                             (1024u + bw / 2) / (bw - 1),
                             (1024u + bh / 2) / (bh - 1),
                             in_base};
    scratch_barrier(cb, g_astc.bc7_scratch, USE_COPY, 0, USE_WRITE);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc.bc7_layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_astc.bc7_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vk_ck(cb, VK_CK_BC7, cx, cy, nz, bw << 8 | bh);
    vk_gpuprof_tag(tag);
    rec_CmdDispatch(cb, (cx + 63) / 64, cy, nz);
    g_astc.bc7_dispatches++;
    scratch_barrier(cb, g_astc.bc7_scratch, USE_WRITE, 0, USE_COPY);
}

/* The texels decoded (or made BC7) in the submission recording now, g_split_sub. A submission is one job of the
   kernel's GPU scheduler, which takes the GPU for hung and resets it when a job runs past its timeout (amdgpu's is 2
   seconds). One game's first 3D frame uploads two 1365x1365 ASTC arrays of 90 layers each as BC7: with their smaller
   levels 110 strips, whose encodes took a Radeon 890M 92 ms each, 10 seconds in all. In one submission the GPU's ring
   was reset and the device lost. A strip that would take a submission past ASTC_SUBMIT_TEXELS goes in the next one
   (vk_submit_split). A strip has 4 Mi texels at most (g_astc.cap's RGBA8). 1 when it submitted: the strip's
   commands then go in the new command buffer (vk_cmd), as rows gives one only after a submission of its own. The
   image keeps the layout its caller made, whose barrier orders the copies of the later submissions too */
#define ASTC_SUBMIT_TEXELS (4ull << 20)
static u64 g_split_sub, g_split_texels;
static int split_before(u64 texels) {
    int split = 0;
    if (g_split_sub != vk_submit_number()) { /* submitted since: that submission's texels are the GPU's */
        g_split_sub = vk_submit_number();
        g_split_texels = 0;
    }
    if (g_split_texels && g_split_texels + texels > ASTC_SUBMIT_TEXELS) {
        LOG_LIMIT(1, "vulkan: ASTC uploads of more than %llu Mi texels go to the GPU in several submissions\n",
                  (unsigned long long)(ASTC_SUBMIT_TEXELS >> 20));
        vk_submit_split();
        g_split_sub = vk_submit_number();
        g_split_texels = 0;
        split = 1;
    }
    g_split_texels += texels;
    return split;
}

/* ---- Very large levels: the quick build now, the fast build in the background

   A level held as BC7 at load (astc_mode's BC7 at load, or its Auto for an Ounce game) of astc_quick_mtexels
   megatexels or more, all its layers or slices counted, takes the encoder's quick build (mode 6 from one line) in the
   frame that needs it, and its fast build is made in the background: into the BC7 cache,
   so that the next upload of the same blocks (a later run's, or this run's after an eviction) copies them with no
   encode, and into the texture as it comes, while the texture holds that upload. One game's first 3D frame uploads two
   1365x1365 ASTC 4x4 arrays of 90 layers, 168 MB of BC7 blocks each in their first level. The fast build took a Radeon
   890M 10.1 seconds of that frame, as vk_bc7_build counts one layer (1.9 megatexels, under VK_BC7_MODE6_TEXELS), and
   being past VK_BC7_CAPTURE_BYTES they never reached the cache, so every run took them again. The quick build takes
   0.46 seconds of the GPU for them (mode 6 alone took 1.1), and the next run copies the pieces in 56 ms when they are
   in the system's page cache; from a slow disk it encodes quickly again and loads them after the frame (BG_HIT_NS).

   The background works from a copy of the level's ASTC blocks as the decoder read them (the snapshot, copied on the
   GPU strip by strip beside the quick encode into host memory), never from guest memory again: the key names those
   bytes, and the texture may change, move or be evicted before the encode ends without changing what is stored under
   it. The level goes into the cache in pieces (Bc7Pieces) of at most BC7_PIECE_BYTES, each a record under a key of
   its own, so the file takes a level of any size, and a run that ends before the last piece leaves the others to the
   next, whose background encodes only the rest. An upload finds the better level when every piece is there, and
   copies them; with any piece missing, or the reads too slow, it encodes the level quickly again, and the pieces
   there load into it after the presents.

   The GPU encodes it (astc_background 1, the default; 0 leaves the quick build), a piece after each present in a
   submission of its own (vk_astc_background), the next one once it is done: on the 890M the two arrays took 13 seconds
   of its time over 133 seconds of play, 2 to 10 ms a frame where the GPU idled 30, and the frame rate stayed within
   the runs' own spread (docs/layers/gpu/astc.md). All of it is the GPU thread's, as bc7_cache.c is */
#define BC7_PIECE_BYTES  (256u << 10)
/* the snapshots of the levels in the background at once */
#define BG_SNAPSHOTS_MAX (1ull << 30)
/* A hit reads the level's pieces on the GPU thread, which the frame waits for: 21 ms for a 168 MB level in the page
   cache, 4.2 s from a disk that gave 40 MB/s (an NVMe drive in a USB enclosure at USB 2 speed). Past this, for all the
   frame's hits, a level is encoded quickly instead, and its pieces load into the texture after the presents that follow, BG_LOAD_NS of reads
   after each (at least a piece): 6.5 ms a piece from that disk, 0.1 from memory. Where every hit stalls, a run after
   a reboot would have stalled longer than the first run did */
#define BG_HIT_NS        (100 * 1000000ull)
#define BG_LOAD_NS       (2 * 1000000ull)

/* A level's pieces: whole slices (or layers), as many as BC7_PIECE_BYTES hold, or bands of block rows of one slice
   where a slice is more. The level's size and footprint alone say what they are, so a later run's keys are the same.
   A band's rows are a multiple of the BC7 rows that start on a row of ASTC blocks too (bh 4: 1, 5: 5, 6: 3, 8: 2) */
typedef struct {
    u32 cx, cy, depth; /* BC7 blocks across and down a slice, and its slices */
    u32 slices;        /* a piece's slices: 1 for bands */
    u32 rows;          /* a piece's BC7 block rows: cy for whole slices */
    u32 bands;         /* pieces a slice: 1 for whole slices */
    u32 n;
} Bc7Pieces;
static void pieces_of(u32 w, u32 h, u32 depth, u32 bh, Bc7Pieces* p) {
    p->cx = (w + 3) / 4;
    p->cy = (h + 3) / 4;
    p->depth = depth;
    const u64 slice = (u64)p->cx * p->cy * 16;
    if (slice <= BC7_PIECE_BYTES) {
        p->slices = (u32)(BC7_PIECE_BYTES / slice) < depth ? (u32)(BC7_PIECE_BYTES / slice) : depth;
        p->rows = p->cy;
        p->bands = 1;
        p->n = (depth + p->slices - 1) / p->slices;
        return;
    }
    const u32 step = bh / (bh % 4 == 0 ? 4 : bh % 2 == 0 ? 2 : 1);
    u32 rows = (u32)(BC7_PIECE_BYTES / ((u64)p->cx * 16));
    rows -= rows % step;
    p->slices = 1;
    p->rows = rows ? rows : step;
    p->bands = (p->cy + p->rows - 1) / p->rows;
    p->n = depth * p->bands;
}
/* piece i: BC7 block rows r to r + nr of slices z to z + nz, and its key, the level's with where it lies. Its blocks
   are one range of the level's, from ((z * cy + r) * cx) * 16 */
typedef struct {
    u32 z, nz, r, nr;
    u64 bytes;
    u64 key[2];
} Bc7Piece;
static void piece_at(const Bc7Pieces* p, const u64 level[2], u32 i, Bc7Piece* q) {
    q->z = i / p->bands * p->slices;
    q->nz = p->depth - q->z < p->slices ? p->depth - q->z : p->slices;
    q->r = i % p->bands * p->rows;
    q->nr = p->cy - q->r < p->rows ? p->cy - q->r : p->rows;
    q->bytes = (u64)p->cx * q->nr * q->nz * 16;
    const u64 words[5] = {level[0], level[1], i | (u64)BC7_PIECE_BYTES << 32, q->z | (u64)q->nz << 32,
                          q->r | (u64)q->nr << 32};
    vk_hash128(words, sizeof words, q->key);
}

/* a level in the background: its snapshot (the ASTC blocks, rows of nx a slice, ny rows a slice, slice after slice),
   whole once ready_sub is done, and its pieces' states */
/* a piece: to encode, being encoded, in the cache to load into the texture (after presents, vk_astc_background), done */
enum { BG_TODO, BG_TAKEN, BG_DONE, BG_LOAD };
typedef struct {
    u64 key[2]; /* the level's, its fast build's */
    u32 w, h, depth, bw, bh, srgb, nx, ny;
    Bc7Pieces p;
    u8* state; /* each piece's BG_* */
    u32 next;  /* no piece before it is BG_TODO */
    u32 left;  /* pieces not BG_DONE */
    u64 ready_sub;
    int ready;
    VkBuffer snap;
    VmaAllocation snap_mem;
    u64 snap_bytes;
    u64 t0;          /* when it was made */
    u32 by_gpu, had; /* pieces the GPU encoded, and those the cache held already */
    /* the texture whose upload made it, while its image holds what that upload wrote there (vk_astc_forget): each
       piece goes into it too, at level, from layer (as_layers: the slices are its layers), so this run shows the
       better blocks as they come */
    Surf* surf;
    u32 level, layer;
    int as_layers;
} Bc7Job;
static struct {
    Bc7Job** jobs;
    u32 njobs, cap;
    u64 snap_bytes;
    u64 slice_sub; /* the GPU's piece in flight: its submission (0: none), its job and piece */
    Bc7Job* slice_job;
    u32 slice_piece;
    u64 quick, made, finished, hits, stored, gpu_pieces, loaded;
    u32 loaded_now; /* pieces bg_load read after this present */
} g_bg;
u64 g_vk_astc_hit_ns = BG_HIT_NS;

/* a ready job's piece that nobody encodes yet, taken: 0 when there is none */
static int bg_take(Bc7Job** job, u32* piece) {
    for (u32 k = 0; k < g_bg.njobs; k++) {
        Bc7Job* j = g_bg.jobs[k];
        if (!j->ready) continue;
        while (j->next < j->p.n && j->state[j->next] == BG_DONE) j->next++;
        for (u32 i = j->next; i < j->p.n; i++)
            if (j->state[i] == BG_TODO) {
                j->state[i] = BG_TAKEN;
                *job = j;
                *piece = i;
                return 1;
            }
    }
    return 0;
}

/* the copy of piece q of a level (cx blocks across, h texels high) from off in a buffer into the image, at level, from
   layer: the slices are its layers (as_layers) or its 3D slices */
static VkBufferImageCopy piece_copy(const Bc7Pieces* p, const Bc7Piece* q, u64 off, u32 w, u32 h, u32 level, u32 layer,
                                    int as_layers) {
    return (VkBufferImageCopy){
        .bufferOffset = off,
        .bufferRowLength = p->cx * 4,
        .bufferImageHeight = q->nr * 4,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, as_layers ? layer + q->z : layer, as_layers ? q->nz : 1},
        .imageOffset = {0, (i32)(q->r * 4), as_layers ? 0 : (i32)q->z},
        .imageExtent = {w, h - q->r * 4 < q->nr * 4 ? h - q->r * 4 : q->nr * 4, as_layers ? 1 : q->nz}};
}
/* piece q of j's level, its blocks at off in buf, into the texture the level is in while it holds what j's upload
   wrote: not after another upload into it (vk_astc_forget), nor once render targets were copied in (tex_take_drawn) */
static void bg_to_image(VkCommandBuffer cb, const Bc7Job* j, const Bc7Piece* q, VkBuffer buf, u64 off) {
    Surf* t = j->surf;
    if (!t || t->took_drawn) return;
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy copy = piece_copy(&j->p, q, off, j->w, j->h, j->level, j->layer, j->as_layers);
    rec_CmdCopyBufferToImage(cb, buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}
void vk_astc_forget(const Surf* t, u32 level) {
    for (u32 k = 0; k < g_bg.njobs; k++)
        if (g_bg.jobs[k]->surf == t && (level == ~0u || g_bg.jobs[k]->level == level)) g_bg.jobs[k]->surf = NULL;
}

/* j's snapshot freed once the GPU's copies into it are done (it is in host memory, but GPU memory on some devices),
   and j with it */
static void bg_job_free(Bc7Job* j) {
    if (j->snap) {
        vk_defer_buffer(j->snap, j->snap_mem);
        g_bg.snap_bytes -= j->snap_bytes;
    }
    free(j->state);
    free(j);
}

/* a level's job for the background, its snapshot to fill (the caller copies its strips' blocks into it), or none when
   the cache holds every piece: NULL when astc_background is 0, the level is on its way already, or the snapshot finds
   no room. The pieces the cache holds (a run before ended first, or the hit ran past g_vk_astc_hit_ns) load from it */
static Bc7Job* bg_job_new(const u64 key[2], u32 w, u32 h, u32 depth, u32 bw, u32 bh, u32 srgb, Surf* t, u32 level,
                          u32 layer, int as_layers) {
    for (u32 k = 0; k < g_bg.njobs; k++)
        if (g_bg.jobs[k]->key[0] == key[0] && g_bg.jobs[k]->key[1] == key[1]) {
            /* uploaded again quickly (an eviction): the pieces still to come go into this image */
            Bc7Job* j = g_bg.jobs[k];
            j->surf = t;
            j->level = level;
            j->layer = layer;
            j->as_layers = as_layers;
            return NULL;
        }
    if (!g_settings.astc_background) return NULL;
    const u32 nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh;
    const u64 bytes = (u64)nx * ny * depth * 16;
    Bc7Pieces p;
    u32 have = 0;
    pieces_of(w, h, depth, bh, &p);
    for (u32 i = 0; i < p.n; i++) {
        Bc7Piece q;
        piece_at(&p, key, i, &q);
        have += vk_bc7_cache_find(q.key) == q.bytes;
    }
    if (have < p.n && (bytes > g_vk.alloc_cap || g_bg.snap_bytes + bytes > BG_SNAPSHOTS_MAX)) {
        LOG_LIMIT(4,
                  "vulkan: a %ux%ux%u ASTC level stays in the quick BC7 build: no room to keep its %llu MB of "
                  "blocks for the background encode\n",
                  w, h, depth, (unsigned long long)(bytes >> 20));
        return NULL;
    }
    Bc7Job* j = calloc(1, sizeof *j);
    if (!j) return NULL;
    *j = (Bc7Job){{key[0], key[1]}, w, h, depth, bw, bh, srgb, nx, ny};
    j->surf = t;
    j->level = level;
    j->layer = layer;
    j->as_layers = as_layers;
    j->p = p;
    j->state = calloc(j->p.n, 1);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = bytes,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST}; /* only the GPU reads it */
    VmaAllocationInfo info;
    if (!j->state || (have < p.n && !VK_OK(vk_buffer_create(g_vk.vma, &bi, &ai, &j->snap, &j->snap_mem, &info)))) {
        LOG_LIMIT(4, "vulkan: no memory to keep a %ux%ux%u ASTC level's blocks for the background encode\n", w, h,
                  depth);
        free(j->state);
        free(j);
        return NULL;
    }
    if (j->snap) {
        vk_mem_track(j->snap_mem);
        j->snap_bytes = bytes;
        g_bg.snap_bytes += bytes;
    }
    j->left = j->p.n;
    for (u32 i = 0; i < j->p.n; i++) {
        Bc7Piece q;
        piece_at(&j->p, key, i, &q);
        if (vk_bc7_cache_find(q.key) == q.bytes) {
            j->state[i] = BG_LOAD;
            j->had++;
        }
    }
    j->t0 = plat_ns();
    return j;
}
/* j in the background once its snapshot's copies (and the quick strips), recorded up to now, are done */
static void bg_job_add(Bc7Job* j, VkCommandBuffer cb) {
    if (j->snap) {
        VkBufferMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .buffer = j->snap,
                                    .size = VK_WHOLE_SIZE};
        VkDependencyInfo d = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b};
        rec_CmdPipelineBarrier2(cb, &d);
    }
    j->ready_sub = vk_submit_number();
    if (g_bg.njobs == g_bg.cap) {
        u32 cap = g_bg.cap ? g_bg.cap * 2 : 8;
        Bc7Job** grown = realloc(g_bg.jobs, cap * sizeof *grown);
        if (!grown) {
            bg_job_free(j);
            return;
        }
        g_bg.jobs = grown;
        g_bg.cap = cap;
    }
    g_bg.jobs[g_bg.njobs++] = j;
    g_bg.made++;
    LOG_LIMIT(16,
              "vulkan: a %ux%ux%u ASTC level of %llu megatexels is in the quick BC7 build for now; its fast build "
              "comes in the background on the GPU: %u pieces of %u to make, %u to load from the BC7 cache\n",
              j->w, j->h, j->depth, (unsigned long long)(((u64)j->w * j->h * j->depth) >> 20), j->left - j->had,
              j->p.n, j->had);
}

/* The GPU's piece: the snapshot's rows of it copied into the frame's device memory, made BC7 blocks by the fast build
   and read back into the cache as an upload's blocks are (bc7_capture), in a submission of its own after the frame's
   (VK_SUB_BACKGROUND). The next waits until it is done, so the GPU has one piece of the background's at a time */
static void bg_gpu_piece(void) {
    Bc7Job* j;
    u32 i;
    if (!bg_take(&j, &i)) return;
    Bc7Piece q;
    piece_at(&j->p, j->key, i, &q);
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    const u32 y = q.r * 4 / j->bh, height = j->h - q.r * 4 < q.nr * 4 ? j->h - q.r * 4 : q.nr * 4;
    const u32 n = (height + j->bh - 1) / j->bh;
    const u64 row_in = (u64)j->nx * 16, in_size = row_in * n * q.nz;
    const u64 align = lim->minStorageBufferOffsetAlignment < 16 ? 16 : lim->minStorageBufferOffsetAlignment;
    VkBuffer back, in;
    u64 in_at;
    VkDescriptorSet set = VK_NULL_HANDLE;
    int slot = bc7_capture(q.key, q.bytes, &back);
    if (slot >= 0 && vk_stage_device(in_size, align, &in, &in_at))
        set = two_buffers(in, in_at, in_size, g_astc.bc7_scratch, q.bytes);
    if (!set) { /* the ring or the descriptors ran out this frame: again after the next present */
        j->state[i] = BG_TODO;
        return;
    }
    VkCommandBuffer cb = vk_cmd();
    VkBufferCopy from = {((u64)q.z * j->ny + y) * row_in, in_at, in_size};
    rec_CmdCopyBuffer(cb, j->snap, in, 1, &from);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    VkDependencyInfo md = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &md);
    bc7_transcode(cb, set, bc7_pipe(1), j->w, height, j->nx, n, q.nz, j->bw, j->bh, j->srgb, 0,
                  VK_PROF_BC7_BACKGROUND);
    const VkBufferCopy to = {0, g_astc.captures[slot].off, q.bytes};
    rec_CmdCopyBuffer(cb, g_astc.bc7_scratch, back, 1, &to);
    bg_to_image(cb, j, &q, g_astc.bc7_scratch, 0);
    bc7_capture_done(cb, slot, back, q.key, q.bytes);
    g_bg.slice_sub = vk_submit_number();
    g_bg.slice_job = j;
    g_bg.slice_piece = i;
    j->by_gpu++;
    g_bg.gpu_pieces++;
    vk_submit_background();
}

/* pieces the cache holds, read from its file into the textures of their ready jobs, BG_LOAD_NS of them at most (at
   least one) on the GPU thread. One that doesn't check out is encoded where the job has a snapshot, else its level
   keeps the quick blocks there for this run, and the next encodes it */
static void bg_load(void) {
    const u64 t0 = plat_ns();
    for (u32 k = 0; k < g_bg.njobs; k++) {
        Bc7Job* j = g_bg.jobs[k];
        for (u32 i = j->next; j->ready && i < j->p.n; i++) {
            if (j->state[i] != BG_LOAD) continue;
            if (plat_ns() - t0 >= BG_LOAD_NS && g_bg.loaded_now) return;
            Bc7Piece q;
            VkBuffer buf;
            u64 off;
            u8* to = NULL;
            piece_at(&j->p, j->key, i, &q);
            const int ok = (to = vk_stage(q.bytes, 16, &buf, &off)) && vk_bc7_cache_read(q.key, to, q.bytes);
            if (!to) return; /* staging ran out this frame */
            if (ok) bg_to_image(vk_cmd(), j, &q, buf, off);
            g_bg.loaded += ok;
            g_bg.loaded_now++;
            j->state[i] = ok || !j->snap ? BG_DONE : BG_TODO;
            j->left -= ok || !j->snap;
        }
    }
}

void vk_astc_background(void) {
    if (!g_bg.njobs) return;
    g_bg.loaded_now = 0;
    const int on = g_settings.astc_background;
    if (g_bg.slice_sub && g_bg_landed >= g_bg.slice_sub) { /* the GPU's piece, which its landing stored */
        Bc7Job* j = g_bg.slice_job;
        Bc7Piece q;
        piece_at(&j->p, j->key, g_bg.slice_piece, &q);
        const int stored = vk_bc7_cache_find(q.key) == q.bytes;
        g_bg.stored += stored;
        j->state[g_bg.slice_piece] = stored ? BG_DONE : BG_TODO;
        j->left -= stored;
        g_bg.slice_sub = 0;
        g_bg.slice_job = NULL;
    }
    for (u32 k = 0; k < g_bg.njobs;) {
        Bc7Job* j = g_bg.jobs[k];
        if (!j->ready && g_bg_landed >= j->ready_sub) j->ready = 1;
        const int end = (!j->left || !on) && g_bg.slice_job != j;
        if (end) g_bg.jobs[k] = g_bg.jobs[--g_bg.njobs];
        if (!end) {
            k++;
            continue;
        }
        if (!j->left) {
            g_bg.finished++;
            LOG("vulkan: the fast build of a %ux%ux%u ASTC level is in the BC7 cache: %u pieces (%llu MB) in %.1f s, "
                "%u on the GPU, %u there already\n",
                j->w, j->h, j->depth, j->p.n, (unsigned long long)(((u64)j->p.cx * j->p.cy * j->depth * 16) >> 20),
                (double)(plat_ns() - j->t0) / 1e9, j->by_gpu, j->had);
        }
        bg_job_free(j);
    }
    bg_load();
    if (on && !g_bg.slice_sub) bg_gpu_piece();
}

void vk_astc_background_drain(void) {
    while (g_bg.njobs) {
        int copying = 0;
        for (u32 k = 0; k < g_bg.njobs; k++) copying |= !g_bg.jobs[k]->ready;
        if (copying || g_bg.slice_sub) vk_finish(); /* lands the snapshots' copies, or the GPU's piece */
        vk_astc_background();
    }
}

void vk_astc_background_stats(VkAstcBackground* s) {
    *s = (VkAstcBackground){g_bg.quick, g_bg.made,  g_bg.finished,   g_bg.njobs,
                            g_bg.hits,  g_bg.stored, g_bg.gpu_pieces, g_bg.loaded};
}

/* a very large level's pieces from the cache, staged and copied into the image: 0 when one is missing or doesn't check
   out, or the frame's hits have read for g_vk_astc_hit_ns (BG_HIT_NS) already: then the level is encoded quickly, over
   whatever was copied. The frame's levels share the time, so a slow disk costs a frame that much once */
static u64 g_hit_frame = ~0ull, g_hit_ns;
static int pieces_from_cache(VkCommandBuffer cb, Surf* t, const u64 key[2], u32 w, u32 h, u32 depth, u32 bh, u32 level,
                             u32 layer, int as_layers) {
    Bc7Pieces p;
    pieces_of(w, h, depth, bh, &p);
    for (u32 i = 0; i < p.n; i++) {
        Bc7Piece q;
        piece_at(&p, key, i, &q);
        if (vk_bc7_cache_find(q.key) != q.bytes) return 0;
    }
    if (g_hit_frame != vk_frame_id()) {
        g_hit_frame = vk_frame_id();
        g_hit_ns = 0;
    }
    const u64 t0 = plat_ns() - g_hit_ns;
    for (u32 i = 0; i < p.n; i++) {
        Bc7Piece q;
        VkBuffer buf;
        u64 off;
        u8* to;
        piece_at(&p, key, i, &q);
        g_hit_ns = plat_ns() - t0;
        if (g_hit_ns >= g_vk_astc_hit_ns) { /* a slow disk: encoded quickly, and loaded after the presents */
            LOG_LIMIT(4,
                      "vulkan: a %ux%ux%u ASTC level's pieces read slowly from the BC7 cache: it is encoded quickly "
                      "and they load over the next frames\n",
                      w, h, depth);
            return 0;
        }
        if (!(to = vk_stage(q.bytes, 16, &buf, &off)) || !vk_bc7_cache_read(q.key, to, q.bytes)) return 0;
        const VkBufferImageCopy copy = piece_copy(&p, &q, off, w, h, level, layer, as_layers);
        rec_CmdCopyBufferToImage(cb, buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    g_hit_ns = plat_ns() - t0;
    return 1;
}

static void keep_strip(VkCommandBuffer cb, const Surf* t, u32 level, u32 bx, u32 by, u32 z, VkBuffer buf, u64 off,
                       u32 nx, u32 n, u32 nz);
/* vk_astc_upload's and vk_astc_upload_layers': depth counts the level's slices, or (layered) the layers from layer */
static int astc_upload(VkCommandBuffer cb, Surf* t, u32 x, u32 y0, u32 w, u32 h, u32 depth, u32 level, u32 layer,
                       int layered, VkAstcRows rows, void* ctx, const u64* content) {
    vk_astc_forget(t, level); /* what this writes is not the background's to overwrite */
    astc_init();
    const int bc7 = t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK || t->f.vk == VK_FORMAT_BC7_SRGB_BLOCK;
    if (bc7) {
        bc7_init();
        if (g_astc.bc7_queued.blocks && g_astc.bc7_queued.until <= vk_last_submit()) {
            u64 done = 0;
            if (VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, vk_gfx_timeline(), &done)) &&
                done >= g_astc.bc7_queued.until)
                g_astc.bc7_queued.blocks = 0;
        }
    }
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    u32 bw = t->f.bw, bh = t->f.bh;
    /* the region's origin on its blocks, and on BC7's too: on multiples of lcm(bw, 4) and lcm(bh, 4) */
    const u32 mx = bc7 ? bw * 4 / (bw % 4 ? bw % 2 ? 1 : 2 : 4) : bw;
    const u32 my = bc7 ? bh * 4 / (bh % 4 ? bh % 2 ? 1 : 2 : 4) : bh;
    if (x % mx || y0 % my) {
        LOG_LIMIT(1,
                  "vulkan: a %ux%u region at (%u, %u) of a %ux%u ASTC texture held as %s is not decoded: a region "
                  "starts on multiples of (%u, %u)\n",
                  w, h, x, y0, bw, bh, bc7 ? "BC7" : "RGBA8", mx, my);
        return 0;
    }
    u32 nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh;
    u64 row_in = (u64)nx * 16, row_out = (u64)w * bh * 4;
    /* BC7: blocks a row, and the ASTC block rows a strip is a multiple of, so that each strip starts on a row of
       whole 4x4 blocks (bh of 5 is four of them, 6 two). A strip's blocks take no more bytes than its texels, but a
       whole slice smaller than a block can, so slices are counted by both */
    const u32 cx = (w + 3) / 4, unit = !bc7 || bh % 4 == 0 ? 1 : bh % 2 == 0 ? 2 : 4;
    const u64 slice_blocks = (u64)cx * ((h + 3) / 4) * 16;
    /* the input is staging memory and a storage buffer range */
    u64 in_cap = lim->maxStorageBufferRange < g_vk.alloc_cap ? lim->maxStorageBufferRange : g_vk.alloc_cap;
    u32 slices = 1, nrows;
    if ((u64)w * h * 4 <= g_astc.cap && slice_blocks <= g_astc.cap && row_in * ny <= in_cap &&
        ny <= lim->maxComputeWorkGroupCount[1]) {
        /* whole slices */
        u64 by_out = g_astc.cap / ((u64)w * h * 4), by_in = in_cap / (row_in * ny);
        slices = (u32)(by_out < by_in ? by_out : by_in);
        if (bc7 && g_astc.cap / slice_blocks < slices) slices = (u32)(g_astc.cap / slice_blocks);
        if (slices > lim->maxComputeWorkGroupCount[2]) slices = lim->maxComputeWorkGroupCount[2];
        if (slices > depth) slices = depth;
        nrows = ny;
    } else {
        u64 by_out = g_astc.cap / row_out, by_in = in_cap / row_in;
        nrows = (u32)(by_out < by_in ? by_out : by_in);
        if (nrows > lim->maxComputeWorkGroupCount[1]) nrows = lim->maxComputeWorkGroupCount[1];
        nrows -= nrows % unit;
    }
    /* a sparse texture's level above its mip tail: strips as high as the device's sparse blocks, and those without
       memory left out (gpu/vk/texture.c tex_bind), as the game maps a small part of such a texture */
    const VkSparseShape* sh = t->sparse ? vk_image_sparse_shape(t->img) : NULL;
    if (sh && level >= sh->tail_first) sh = NULL;
    if (sh) {
        u32 k = sh->gran.height / bh ? sh->gran.height / bh : 1;
        k = (k + unit - 1) / unit * unit;
        if (nrows > k) nrows = k;
        if (slices > sh->gran.depth) slices = sh->gran.depth;
    }
    if (bw < 4 || bw > 12 || bh < 4 || bh > 12 || !nrows || nx > lim->maxComputeWorkGroupCount[0] || w > 16384 ||
        h > 16384 || (bc7 && (u64)nrows * bh / 4 + 1 > lim->maxComputeWorkGroupCount[1])) {
        LOG_LIMIT(1, "vulkan: a %ux%u ASTC texture exceeds the device's limits (left out)\n", w, h);
        return 0;
    }
    const int srgb = t->f.vk == VK_FORMAT_R8G8B8A8_SRGB || t->f.vk == VK_FORMAT_BC7_SRGB_BLOCK;
    const int is3d = t->type == VK_IMAGE_VIEW_TYPE_3D;
    VkPipeline pipe = bc7 ? VK_NULL_HANDLE : astc_pipeline(bw, bh);
    VkPipeline bc7_build = VK_NULL_HANDLE;
    /* the BC7 cache: a whole level's blocks from it, or the encode's copied into the readback ring as well as into the
       image, a strip at a time in its place in the level, for vk_astc_frame_done to put in it. A very large level's
       are its fast build's, in pieces, which the background makes from a snapshot of its blocks */
    int capture = -1;
    VkBuffer back = VK_NULL_HANDLE;
    u64 key[2] = {0, 0};
    Bc7Job* job = NULL;
    const u64 blocks = (u64)cx * ((h + 3) / 4) * depth * 16;
    if (bc7) {
        /* the encoder's build, by the level's size and not the region's (vk_bc7_build): the fast one from
           VK_BC7_FAST_TEXELS, mode 6 alone from VK_BC7_MODE6_TEXELS. A level loaded a piece at a time (a resident
           texture's bricks, its patches) then encodes each block as its whole upload does, where the full build chose
           other modes for some of them. A very large level, every layer of a 2D array counted (quick_level), takes
           the quick build, mode 6 from one line, and the fast build in the background; a sparse one keeps the rule
           of a layer's size, as only its mapped strips are decoded and none goes to the cache, so the quick blocks
           would stay */
        const u32 lw = t->w >> level ? t->w >> level : 1, lh = t->h >> level ? t->h >> level : 1;
        const u32 ld = t->type == VK_IMAGE_VIEW_TYPE_3D && t->depth3d >> level ? t->depth3d >> level : 1;
        const int quick = !t->sparse && quick_level((u64)lw * lh * ld *
                                                    (t->type == VK_IMAGE_VIEW_TYPE_3D || !t->layers ? 1 : t->layers));
        const u32 build = quick ? 3 : vk_bc7_build(lw, lh, ld);
        bc7_build = bc7_pipe(build);
        g_astc.bc7_levels++;
        if (content && !sh && !x && !y0 && w == lw && h == lh &&
            (layered ? !layer && depth == t->layers : depth == ld)) {
            /* a very large level's pieces are its fast build's */
            vk_bc7_level_key(content, w, h, layered ? 1 : depth, layered ? depth : 1, bw, bh, srgb, quick ? 1 : build,
                             key);
            if (quick ? pieces_from_cache(cb, t, key, w, h, depth, bh, level, layer, layered)
                      : bc7_from_cache(cb, t->img, key, blocks, w, h, depth, level, layer, layered)) {
                g_astc.bc7_hits += quick; /* bc7_from_cache counts its own */
                g_astc.bc7_bytes += blocks;
                g_astc.bc7_rgba8_bytes += (u64)w * h * depth * 4;
                g_bg.hits += quick;
                if (quick)
                    LOG_LIMIT(16, "vulkan: a %ux%ux%u ASTC level's fast build came from the BC7 cache, in pieces\n", w,
                              h, depth);
                return 1;
            }
            if (!quick)
                capture = bc7_capture(key, blocks, &back);
            else
                job = bg_job_new(key, w, h, depth, bw, bh, (u32)srgb, t, level, layer, layered);
        }
        g_bg.quick += quick && !x && !y0;
    }
    /* astc_dump: a whole level's blocks, as the decoder reads them */
    u32 dump = ~0u;
    if (g_settings.astc_dump[0] && !x && !y0 && !sh && w == (t->w >> level ? t->w >> level : 1) &&
        h == (t->h >> level ? t->h >> level : 1))
        dump = dump_begin(w, h, depth, bw, bh, row_in * ny * depth);
    for (u32 z = 0; z < depth; z += slices) {
        u32 nz = depth - z < slices ? depth - z : slices;
        for (u32 y = 0; y < ny; y += nrows) {
            u32 n = ny - y < nrows ? ny - y : nrows;
            u32 height = h - y * bh < n * bh ? h - y * bh : n * bh;
            u64 size = row_in * n * nz, offset, texels = (u64)w * height * nz * 4;
            if (sh && !vk_image_sparse_bound(t->img, layer, level, y * bh, y * bh + height, z, z + nz, 0)) continue;
            if (split_before((u64)w * height * nz)) cb = vk_cmd();
            VkBuffer input;
            const u32 cy = (height + 3) / 4;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (rows(ctx, &cb, z, nz, y, n, &input, &offset))
                set = bc7 ? two_buffers(input, offset, size, g_astc.bc7_scratch, (u64)cx * cy * nz * 16)
                          : two_buffers(input, offset, size, g_astc.scratch, texels);
            if (!set) {
                LOG_LIMIT(1, "vulkan: out of staging memory for a %ux%u ASTC texture (left out)\n", w, h);
                if (job) bg_job_free(job);
                return 0;
            }
            dump_strip(cb, dump, input, offset, size, ((u64)z * ny + y) * row_in);
            /* the strip's blocks into their place in the snapshot: rows of a slice, or slices */
            if (job && job->snap) {
                const VkBufferCopy snap = {offset, ((u64)z * ny + y) * row_in, size};
                rec_CmdCopyBuffer(cb, input, job->snap, 1, &snap);
            }
            /* and into the blocks the texture keeps for its BC7 image, made between frames */
            if (!bc7 && t->bc7.kept) keep_strip(cb, t, level, x / bw, y0 / bh + y, layered ? layer + z : is3d ? z : layer,
                                                input, offset, nx, n, nz);
            VkBuffer from = g_astc.scratch;
            u32 row_texels = 0, image_rows = 0;
            if (bc7) {
                /* the blocks straight into BC7 blocks (astc_bc7.comp), in rows of cx blocks */
                bc7_transcode(cb, set, bc7_build, w, height, nx, n, nz, bw, bh, (u32)srgb, 0, VK_PROF_BC7);
                if (capture >= 0) { /* strips start on whole rows of BC7 blocks, so each is one range of the level's */
                    const VkBufferCopy r = {
                        0, g_astc.captures[capture].off + ((u64)z * ((h + 3) / 4) + y * bh / 4) * cx * 16,
                        (u64)cx * cy * nz * 16};
                    rec_CmdCopyBuffer(cb, g_astc.bc7_scratch, back, 1, &r);
                }
                from = g_astc.bc7_scratch;
                row_texels = cx * 4;
                image_rows = cy * 4;
                g_astc.bc7_bytes += (u64)cx * cy * nz * 16;
                g_astc.bc7_rgba8_bytes += texels;
            } else {
                /* Submission makes the flushed host staging writes visible. Scratch
                   reuse also needs a WAR dependency on the preceding image copy,
                   including when that belongs to an earlier frame. */
                scratch_barrier(cb, g_astc.scratch, 0, 1, USE_WRITE);
                rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
                rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc.layout, 0, 1, &set, 0, NULL);
                u32 shift = tile_shift(nx, n), tw = 1u << shift, th = 64u >> shift;
                AstcParams p = {w, height, nx, n, (u32)srgb, shift};
                rec_CmdPushConstants(cb, g_astc.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
                vk_ck(cb, VK_CK_ASTC, nx, n, nz, bw << 8 | bh);
                vk_gpuprof_tag(VK_PROF_ASTC);
                rec_CmdDispatch(cb, (nx + tw - 1) / tw, (n + th - 1) / th, nz);
                scratch_barrier(cb, g_astc.scratch, USE_WRITE, 0, USE_COPY);
                g_astc.rgba8_bytes += texels;
            }
            /* a layer after another in the buffer, as slices are */
            VkBufferImageCopy copy = {
                .bufferRowLength = row_texels,
                .bufferImageHeight = image_rows,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layered ? layer + z : layer, layered ? nz : 1},
                .imageOffset = {(i32)x, (i32)(y0 + y * bh), layered ? 0 : (i32)z},
                .imageExtent = {w, height, layered ? 1 : nz}};
            vk_ck(cb, VK_CK_COPY, w, height, nz, level);
            rec_CmdCopyBufferToImage(cb, from, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            vk_ck(cb, VK_CK_STRIP_DONE, w, height, nz, level);
            if (bc7) {
                g_astc.bc7_queued.blocks += (u64)cx * ((height + 3) / 4) * nz;
                g_astc.bc7_queued.until = vk_last_submit() + 1;
                if (g_astc.bc7_queued.blocks >= bc7_queued_limit()) {
                    g_astc.bc7_queued.blocks = 0;
                    vk_submit_upload_wait();
                    /* This upload continues after the submission too. Rows callbacks may only stage bytes;
                       do not leave them, or the next strip's barriers, with the ended command buffer. */
                    cb = vk_cmd();
                }
            }
        }
    }
    if (capture >= 0) bc7_capture_done(cb, capture, back, key, blocks);
    if (job) bg_job_add(job, cb);
    return 1;
}
int vk_astc_upload(VkCommandBuffer cb, Surf* t, u32 x, u32 y, u32 w, u32 h, u32 depth, u32 level, u32 layer,
                   VkAstcRows rows, void* ctx, const u64* content) {
    return astc_upload(cb, t, x, y, w, h, depth, level, layer, 0, rows, ctx, content);
}
int vk_astc_upload_layers(VkCommandBuffer cb, Surf* t, u32 w, u32 h, u32 level, u32 layer, u32 layers, VkAstcRows rows,
                          void* ctx, const u64* content) {
    return astc_upload(cb, t, 0, 0, w, h, layers, level, layer, 1, rows, ctx, content);
}

u64 vk_bc7_strip_blocks(void) {
    astc_init();
    return g_astc.cap / 16; /* the encoder's scratch, 16 bytes a block */
}

/* ---- The blocks a texture keeps for its BC7 image, made between frames (texture.c's vk_tex_later_frame)

   A texture held as BC7 goes up decoded to RGBA8, and its BC7 image is made after the presents that follow. That image
   comes from the ASTC blocks, as an upload straight to BC7 makes it (astc_bc7.comp), not from the RGBA8 image's
   texels: those were four bytes a texel copied out of the image and read back by the encoder. So the blocks the
   decoder read are kept on the GPU beside the RGBA8 image, a copy of each strip as it is decoded (keep_strip), until
   the BC7 image is whole: at most a quarter of the RGBA8 image's bytes (4x4 blocks), a ninth for 6x6. Guest memory
   is not read again for them: the game may have written or unmapped it by then, and what the BC7 image holds has to
   be what the RGBA8 one showed. Level after level, each as an upload's rows are: a slice (a layer, or a 3D level's
   depth slice) after another, rows of blocks */
static void keep_level(const Surf* t, u32 level, u32* nx, u32* ny, u32* slices) {
    const u32 lw = t->w >> level ? t->w >> level : 1, lh = t->h >> level ? t->h >> level : 1;
    *nx = (lw + t->f.bw - 1) / t->f.bw;
    *ny = (lh + t->f.bh - 1) / t->f.bh;
    *slices = t->type == VK_IMAGE_VIEW_TYPE_3D ? (t->depth3d >> level ? t->depth3d >> level : 1) : t->layers;
}
/* where level `level`'s blocks start in what t keeps: t->levels for all of it */
static u64 keep_offset(const Surf* t, u32 level) {
    u64 at = 0;
    for (u32 l = 0; l < level; l++) {
        u32 nx, ny, slices;
        keep_level(t, l, &nx, &ny, &slices);
        at += (u64)nx * ny * slices * 16;
    }
    return at;
}
/* orders a use of what t keeps after its others: a copy into it (write), or the transcoder's read */
static void keep_barrier(VkCommandBuffer cb, const Surf* t, int write) {
    VkBufferMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | (write ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : 0),
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | (write ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT : 0),
        .dstStageMask = write ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = write ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = t->bc7.kept,
        .size = VK_WHOLE_SIZE};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}
/* a strip an upload decodes, nx x n blocks a slice of nz slices at off in buf, into its place in what t keeps: from
   block (bx, by) of slice z of level `level`. A strip as wide as the level is one range a slice (one in all when its
   slices are whole); a region's rows go one by one */
static void keep_strip(VkCommandBuffer cb, const Surf* t, u32 level, u32 bx, u32 by, u32 z, VkBuffer buf, u64 off,
                       u32 nx, u32 n, u32 nz) {
    u32 lnx, lny, slices;
    keep_level(t, level, &lnx, &lny, &slices);
    if (bx + nx > lnx || by + n > lny || z + nz > slices) return; /* no strip of this level: nothing an upload makes */
    const u64 base = keep_offset(t, level), row = (u64)nx * 16;
    const int wide = nx == lnx, whole = wide && n == lny;
    const u32 count = whole ? 1 : wide ? nz : nz * n;
    VkBufferCopy one, *r = count > 1 ? malloc(count * sizeof *r) : &one;
    if (!r) fatal("vulkan: no memory for the copies of an ASTC strip's blocks");
    for (u32 i = 0; i < count; i++) {
        const u32 k = whole ? 0 : wide ? i : i / n, j = wide ? 0 : i % n;
        r[i] = (VkBufferCopy){off + ((u64)k * n + j) * row, base + (((u64)(z + k) * lny + by + j) * lnx + bx) * 16,
                              whole ? row * n * nz : wide ? row * n : row};
    }
    keep_barrier(cb, t, 1);
    rec_CmdCopyBuffer(cb, buf, t->bc7.kept, count, r);
    if (r != &one) free(r);
}

int vk_astc_keep(Surf* t) {
    if (t->bc7.kept) return 1;
    const u64 bytes = keep_offset(t, t->levels);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = bytes,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!bytes || bytes > g_vk.alloc_cap ||
        !VK_OK(vk_buffer_create(g_vk.vma, &bi, &ai, &t->bc7.kept, &t->bc7.kept_mem, NULL))) {
        t->bc7.kept = VK_NULL_HANDLE;
        t->bc7.kept_mem = VK_NULL_HANDLE;
        return 0;
    }
    vk_mem_track(t->bc7.kept_mem);
    t->bc7.kept_bytes = bytes;
    g_astc.kept_bytes += bytes;
    return 1;
}
void vk_astc_unkeep(Surf* t) {
    if (!t->bc7.kept) return;
    vk_defer_buffer(t->bc7.kept, t->bc7.kept_mem); /* once the copies into it and the transcoder's reads are done */
    g_astc.kept_bytes -= t->bc7.kept_bytes;
    t->bc7.kept = VK_NULL_HANDLE;
    t->bc7.kept_mem = VK_NULL_HANDLE;
    t->bc7.kept_bytes = 0;
}
u64 vk_astc_kept_bytes(void) { return g_astc.kept_bytes; }

u32 vk_bc7_kept_unit(const Surf* t) { return t->f.bh % 4 == 0 ? 1 : t->f.bh % 2 == 0 ? 2 : 4; }

int vk_bc7_encode_kept(VkCommandBuffer cb, Surf* t, VkImage dst, u32 level, u32 z, u32 nz, u32 y, u32 n) {
    astc_init();
    bc7_init();
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    const int is3d = t->type == VK_IMAGE_VIEW_TYPE_3D;
    const u32 bw = t->f.bw, bh = t->f.bh, lw = t->w >> level ? t->w >> level : 1, lh = t->h >> level ? t->h >> level : 1;
    u32 nx, ny, slices;
    keep_level(t, level, &nx, &ny, &slices);
    const u32 unit = vk_bc7_kept_unit(t);
    const u32 height = n && y < ny ? (lh - y * bh < n * bh ? lh - y * bh : n * bh) : 0;
    const u32 cx = (lw + 3) / 4, cy = (height + 3) / 4;
    const u64 blocks = (u64)cx * cy * nz * 16, in_size = (u64)nx * n * nz * 16;
    const u64 align = lim->minStorageBufferOffsetAlignment < 16 ? 16 : lim->minStorageBufferOffsetAlignment;
    if (!t->bc7.kept || !height || y % unit || y + n > ny || (n % unit && y + n != ny) || z + nz > slices ||
        (nz > 1 && (y || n != ny)) || blocks > g_astc.cap || lw > 16384 || lh > 16384 ||
        cy > lim->maxComputeWorkGroupCount[1] || nz > lim->maxComputeWorkGroupCount[2] || align % 16) {
        LOG_LIMIT(1,
                  "vulkan: block rows %u to %u of %u slices of a %ux%u ASTC level are no strip of the BC7 encoder's "
                  "(rows from a multiple of %u, %llu BC7 blocks at most): it is not made BC7\n",
                  y, y + n, nz, lw, lh, unit, (unsigned long long)(g_astc.cap / 16));
        return 0;
    }
    /* the strip's blocks are one range of what t keeps; a storage buffer's offset is a multiple of the device's
       alignment, so the range starts on one before them and the shader skips the blocks between (in_base) */
    const u64 at = keep_offset(t, level) + ((u64)z * ny + y) * nx * 16, start = at - at % align;
    if (at - start + in_size > lim->maxStorageBufferRange) return 0;
    VkDescriptorSet set = two_buffers(t->bc7.kept, start, at - start + in_size, g_astc.bc7_scratch, blocks);
    if (!set) return 0;
    const int srgb = t->f.vk == VK_FORMAT_R8G8B8A8_SRGB || t->f.vk == VK_FORMAT_BC7_SRGB_BLOCK;
    keep_barrier(cb, t, 0);
    bc7_transcode(cb, set, bc7_pipe(vk_bc7_build(lw, lh, is3d ? slices : 1)), lw, height, nx, n, nz, bw, bh,
                  (u32)srgb, (u32)((at - start) / 16), VK_PROF_BC7);
    VkBufferImageCopy out = {.bufferRowLength = cx * 4,
                             .bufferImageHeight = cy * 4,
                             .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, is3d ? 0 : z, is3d ? 1 : nz},
                             .imageOffset = {0, (i32)(y * bh), is3d ? (i32)z : 0},
                             .imageExtent = {lw, height, is3d ? nz : 1}};
    vk_ck(cb, VK_CK_COPY, lw, height, nz, level);
    rec_CmdCopyBufferToImage(cb, g_astc.bc7_scratch, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &out);
    vk_ck(cb, VK_CK_STRIP_DONE, lw, height, nz, level);
    return 1;
}
