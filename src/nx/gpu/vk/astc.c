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

   A BC7 image (format.c's TA, astc_mode's Auto, on a GPU that samples BCn but not ASTC) takes a second
   dispatch: bc7.comp encodes the decoded texels into BC7 blocks, a quarter of RGBA8's bytes, and those are what
   the copy writes. Its strips then start on whole 4x4 blocks. A level of VK_BC7_FAST_TEXELS or more takes the
   encoder's fast build, which tries every mode only on the blocks mode 6 does not hold closely (the full encoder on
   every block mode 6 does not hold exactly), and one of VK_BC7_MODE6_TEXELS or more mode 6 alone: a game's sparse
   texture of seven mapped layers of 8192x8192 took the full encoder 14 seconds on a GeForce RTX 2080, which mode 6
   alone encodes at 1200 megatexels a second and the fast build at 65 to 1400 (one of its normal maps, its albedo).

   A region of a level goes the same way (texture.c loads the blocks of the pages that changed): its texels from its
   own blocks, at its place in the image. bc7.comp encodes a block from its 16 texels alone, repeating a strip's last
   texel past its edge, so a region whose sides are on 4x4 blocks, or the level's own edges, encodes the blocks the
   whole level's upload does.

   The blocks of each encode also go into the BC7 cache (bc7_cache.c): copied into the readback ring strip by strip,
   and stored when their submission is done (vk_astc_frame_done). A later upload of the same ASTC bytes copies them
   from the cache into the image, with no decode and no encode (bc7_from_cache). */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/texture/astc_spv.h" /* compiled from astc.comp by the build (cmake/glsl.cmake) */
#include "nx/gpu/texture/bc7_spv.h"  /* compiled from bc7.comp, and its fast build */
#include "nx/gpu/texture/blocklinear_spv.h"
#include "nx/runtime/platform/log.h"

/* an encode's blocks in the readback ring, until its submission is done */
typedef struct {
    u64 key[2];
    u64 sub; /* the submission that copies them: 0, a free slot */
    u8* back;
    VmaAllocation mem;
    u64 off, size;
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
    VkShaderModule bc7_module, bc7_fast_module, bc7_mode6_module;
    VkPipeline bc7_pipe, bc7_fast_pipe, bc7_mode6_pipe;
    VkBuffer bc7_scratch;
    VmaAllocation bc7_mem;
    /* image levels uploaded as BC7, those the cache gave, their bytes, and what they would have taken as RGBA8
       (vk_astc_stats) */
    u64 bc7_levels, bc7_hits, bc7_bytes, bc7_rgba8_bytes;
    /* encodes' blocks copied into the readback ring, each until its submission is done (vk_astc_frame_done), and
       their bytes: at most VK_BC7_CAPTURE_BYTES. As many as that holds: 64 at a time dropped two of three encodes in
       one game's menus, whose textures come in bursts of hundreds of levels */
    Bc7Capture* captures;
    u32 ncaptures;
    u64 captured;
    /* the bytes of the ASTC levels decoded to RGBA8 (vk_astc_rgba8_bytes) */
    u64 rgba8_bytes;
} Astc;
static Astc g_astc;

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
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_astc.ds)))
        fatal("vulkan: cannot create the ASTC descriptor layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcParams)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_astc.ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_astc.layout)))
        fatal("vulkan: cannot create the ASTC pipeline layout");
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = astc_spv_size, .pCode = astc_spv};
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &g_astc.module))) fatal("vulkan: cannot load the ASTC shader");
    g_astc.cap = 16ull << 20; /* a 2048x2048 slice a strip: fewer barriers between decoding and copying */
    if (g_astc.cap > g_vk.props.limits.maxStorageBufferRange) g_astc.cap = g_vk.props.limits.maxStorageBufferRange;
    if (g_astc.cap > g_vk.alloc_cap) g_astc.cap = g_vk.alloc_cap;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = g_astc.cap,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vk_buffer_create(g_vk.vma, &bi, &ai, &g_astc.scratch, &g_astc.mem, NULL)))
        fatal("vulkan: cannot allocate ASTC scratch memory");
    vk_mem_track(g_astc.mem);
}

/* bc7.comp's push constants: the strip's width and its slices' height in texels and in blocks */
typedef struct {
    u32 w, h, blocks_x, blocks_y;
} Bc7Params;

static void bc7_init(void) {
    if (g_astc.bc7_pipe) return;
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Bc7Params)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_astc.ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_astc.bc7_layout)))
        fatal("vulkan: cannot create the BC7 encoder's pipeline layout");
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bc7_spv_size, .pCode = bc7_spv};
    VkShaderModuleCreateInfo fi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bc7_fast_spv_size, .pCode = bc7_fast_spv};
    VkShaderModuleCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bc7_mode6_spv_size, .pCode = bc7_mode6_spv};
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &g_astc.bc7_module)) ||
        !VK_OK(vkCreateShaderModule(g_vk.dev, &fi, NULL, &g_astc.bc7_fast_module)) ||
        !VK_OK(vkCreateShaderModule(g_vk.dev, &si, NULL, &g_astc.bc7_mode6_module)))
        fatal("vulkan: cannot load the BC7 encoder");
    const VkShaderModule modules[3] = {g_astc.bc7_module, g_astc.bc7_fast_module, g_astc.bc7_mode6_module};
    VkComputePipelineCreateInfo ci[3];
    for (int i = 0; i < 3; i++)
        ci[i] = (VkComputePipelineCreateInfo){.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                              .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                        .module = modules[i],
                                                        .pName = "main"},
                                              .layout = g_astc.bc7_layout};
    VkPipeline made[3];
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 3, ci, NULL, made)))
        fatal("vulkan: cannot create the BC7 encoder's pipelines");
    g_astc.bc7_pipe = made[0];
    g_astc.bc7_fast_pipe = made[1];
    g_astc.bc7_mode6_pipe = made[2];
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = g_astc.cap,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vk_buffer_create(g_vk.vma, &bi, &ai, &g_astc.bc7_scratch, &g_astc.bc7_mem, NULL)))
        fatal("vulkan: cannot allocate the BC7 encoder's scratch memory");
    vk_mem_track(g_astc.bc7_mem);
}

void vk_astc_stats(u64* levels, u64* hits, u64* bytes, u64* rgba8_bytes) {
    *levels = g_astc.bc7_levels;
    *hits = g_astc.bc7_hits;
    *bytes = g_astc.bc7_bytes;
    *rgba8_bytes = g_astc.bc7_rgba8_bytes;
}

/* The cache's version: whatever changes the blocks that the same ASTC bytes become, other than what each key holds.
   The SPIR-V of the deswizzle, the decoder and the encoder's three builds, which the build makes from the shaders
   (an edit to any changes it); the encoder's version; and the GPU and its driver, whose compilers run that SPIR-V */
void vk_astc_cache_open(const char* path) {
    const VkPhysicalDeviceProperties* d = &g_vk.props;
    const u64 spirv[5] = {vk_hash(blocklinear_spv, blocklinear_spv_size), vk_hash(astc_spv, astc_spv_size),
                          vk_hash(bc7_spv, bc7_spv_size), vk_hash(bc7_fast_spv, bc7_fast_spv_size),
                          vk_hash(bc7_mode6_spv, bc7_mode6_spv_size)};
    char uuid[33], key[256];
    for (int i = 0; i < 16; i++) snprintf(uuid + 2 * i, 3, "%02x", d->pipelineCacheUUID[i]);
    int n = snprintf(key, sizeof key, "bc7 encoder=%d spirv=%016llx gpu=%04x:%04x driver=%u uuid=%s",
                     VK_BC7_ENCODER_VERSION, (unsigned long long)vk_hash(spirv, sizeof spirv), d->vendorID, d->deviceID,
                     d->driverVersion, uuid);
    u64 version[2];
    vk_hash128(key, n > 0 && (size_t)n < sizeof key ? (u64)n : 0, version);
    vk_bc7_cache_open(path, version, VK_BC7_CACHE_MAX);
}

static int is_bc7(const Surf* t) { return t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK || t->f.vk == VK_FORMAT_BC7_SRGB_BLOCK; }
/* the BC7 blocks of a level of w x h texels and depth slices (or layers) */
static u64 level_blocks(u32 w, u32 h, u32 depth) { return (u64)((w + 3) / 4) * ((h + 3) / 4) * depth * 16; }

int vk_astc_cached(const Surf* t, u32 w, u32 h, u32 depth) {
    u64 hits, stored, restarts, bytes;
    vk_bc7_cache_stats(&hits, &stored, &restarts, &bytes);
    const u64 n = level_blocks(w, h, depth);
    return is_bc7(t) && !t->sparse && bytes && n <= VK_BC7_CAPTURE_BYTES && n <= g_vk.alloc_cap;
}

void vk_astc_frame_done(u64 sub) {
    for (u32 i = 0; g_astc.captured && i < g_astc.ncaptures; i++) {
        if (!g_astc.captures[i].sub || g_astc.captures[i].sub > sub) continue;
        vmaInvalidateAllocation(g_vk.vma, g_astc.captures[i].mem, g_astc.captures[i].off, g_astc.captures[i].size);
        vk_bc7_cache_put(g_astc.captures[i].key, g_astc.captures[i].back, g_astc.captures[i].size);
        g_astc.captured -= g_astc.captures[i].size;
        g_astc.captures[i].sub = 0;
    }
}

/* a level's blocks from the cache (size bytes of them, cx blocks across and cy down a slice) staged and copied into
   the image: 0 when they aren't there, staging has no room, or they don't check out */
static int bc7_from_cache(VkCommandBuffer cb, Surf* t, const u64 key[2], u64 size, u32 w, u32 h, u32 depth, u32 level,
                          u32 layer, int as_layers) {
    VkBuffer buf;
    u64 off;
    u8* p;
    if (vk_bc7_cache_find(key) != size || !(p = vk_stage(size, 16, &buf, &off)) || !vk_bc7_cache_read(key, p, size))
        return 0;
    VkBufferImageCopy copy = {.bufferOffset = off,
                              .bufferRowLength = (w + 3) / 4 * 4,
                              .bufferImageHeight = (h + 3) / 4 * 4,
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, as_layers ? depth : 1},
                              .imageExtent = {w, h, as_layers ? 1 : depth}};
    rec_CmdCopyBufferToImage(cb, buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    return 1;
}

/* a free slot for an encode's blocks, size bytes of them, with room taken in the readback ring: -1 when there is no
   slot or room, the bound is reached, or the same blocks are on their way already */
static int bc7_capture(const u64 key[2], u64 size, VkBuffer* buf) {
    int slot = -1;
    if (g_astc.captured + size > VK_BC7_CAPTURE_BYTES) return -1;
    for (u32 i = 0; i < g_astc.ncaptures; i++)
        if (!g_astc.captures[i].sub && slot < 0)
            slot = (int)i;
        else if (g_astc.captures[i].sub && g_astc.captures[i].key[0] == key[0] && g_astc.captures[i].key[1] == key[1])
            return -1;
    if (slot < 0) {
        u32 n = g_astc.ncaptures ? g_astc.ncaptures * 2 : 64;
        Bc7Capture* grown = realloc(g_astc.captures, (size_t)n * sizeof *grown);
        if (!grown) return -1;
        for (u32 i = g_astc.ncaptures; i < n; i++) grown[i] = (Bc7Capture){0};
        slot = (int)g_astc.ncaptures;
        g_astc.captures = grown;
        g_astc.ncaptures = n;
    }
    g_astc.captures[slot].back = vk_stage_back(size, buf, &g_astc.captures[slot].off, &g_astc.captures[slot].mem);
    return g_astc.captures[slot].back ? slot : -1;
}
u64 vk_astc_rgba8_bytes(void) { return g_astc.rgba8_bytes; }

static VkPipeline astc_pipeline(u32 bw, u32 bh) {
    VkPipeline* pipe = &g_astc.pipes[bw - 4][bh - 4];
    if (*pipe) return *pipe;
    const u32 dims[2] = {bw, bh};
    const VkSpecializationMapEntry entries[2] = {{0, 0, sizeof(u32)}, {1, sizeof(u32), sizeof(u32)}};
    VkSpecializationInfo spec = {.mapEntryCount = 2, .pMapEntries = entries, .dataSize = sizeof dims, .pData = dims};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = g_astc.module,
                                                .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = g_astc.layout};
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, pipe)))
        fatal("vulkan: cannot create the ASTC compute pipeline");
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
    VkWriteDescriptorSet writes[2];
    for (u32 i = 0; i < 2; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(g_vk.dev, 2, writes, 0, NULL);
    return set;
}

/* The texels decoded (and encoded as BC7) in the submission recording now, g_split_sub. A submission is one job of the
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

int vk_astc_upload(VkCommandBuffer cb, Surf* t, u32 x, u32 y0, u32 w, u32 h, u32 depth, u32 level, u32 layer,
                   u32 layers, VkAstcRows rows, void* ctx, const u64* content) {
    /* layers of a 2D array: decoded as the slices of a volume are, each strip of them copied at once */
    const int as_layers = layers > 1 && depth == 1;
    if (as_layers) depth = layers;
    astc_init();
    const int bc7 = is_bc7(t);
    if (bc7) bc7_init();
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
    if (bw < 4 || bw > 12 || bh < 4 || bh > 12 || !nrows || nx > lim->maxComputeWorkGroupCount[0] ||
        (bc7 && (cx + 63) / 64 > lim->maxComputeWorkGroupCount[0])) {
        LOG_LIMIT(1, "vulkan: a %ux%u ASTC texture exceeds the device's limits (left out)\n", w, h);
        return 0;
    }
    const int srgb = t->f.vk == VK_FORMAT_R8G8B8A8_SRGB || t->f.vk == VK_FORMAT_BC7_SRGB_BLOCK;
    /* the encoder's build, by the level's size and not the region's: the fast one from VK_BC7_FAST_TEXELS, mode 6 alone
       from VK_BC7_MODE6_TEXELS. A level loaded a piece at a time (a resident texture's bricks, its patches) then
       encodes each block as its whole upload does, where the full build chose other modes for some of them */
    const u32 level_w = t->w >> level ? t->w >> level : 1, level_h = t->h >> level ? t->h >> level : 1;
    const u32 level_d = t->type == VK_IMAGE_VIEW_TYPE_3D && t->depth3d >> level ? t->depth3d >> level : 1;
    const u64 level_texels = (u64)level_w * level_h * level_d;
    const u32 build = level_texels >= VK_BC7_MODE6_TEXELS ? 2 : level_texels >= VK_BC7_FAST_TEXELS ? 1 : 0;
    /* the BC7 cache: the level's blocks from it, or the encode's copied into the readback ring as well as into the
       image, a strip at a time in their place in the level, for vk_astc_frame_done to put in it */
    const u64 blocks = level_blocks(w, h, depth);
    u64 key[2];
    int capture = -1;
    VkBuffer back = VK_NULL_HANDLE;
    if (bc7 && content && !sh && vk_astc_cached(t, w, h, depth)) {
        const VkBc7Level lv = {w, h, as_layers ? 1 : depth, as_layers ? depth : 1, bw, bh, (u32)srgb, build};
        vk_bc7_key(content, &lv, key);
        if (bc7_from_cache(cb, t, key, blocks, w, h, depth, level, layer, as_layers)) {
            g_astc.bc7_levels++;
            g_astc.bc7_hits++;
            g_astc.bc7_bytes += blocks;
            g_astc.bc7_rgba8_bytes += (u64)w * h * depth * 4;
            return 1;
        }
        capture = bc7_capture(key, blocks, &back);
    }
    VkPipeline pipe = astc_pipeline(bw, bh);
    if (bc7) g_astc.bc7_levels++;
    for (u32 z = 0; z < depth; z += slices) {
        u32 nz = depth - z < slices ? depth - z : slices;
        for (u32 y = 0; y < ny; y += nrows) {
            u32 n = ny - y < nrows ? ny - y : nrows;
            u32 height = h - y * bh < n * bh ? h - y * bh : n * bh;
            u64 size = row_in * n * nz, offset, texels = (u64)w * height * nz * 4;
            if (sh && !vk_image_sparse_bound(t->img, layer, level, y * bh, y * bh + height, z, z + nz, 0)) continue;
            if (split_before((u64)w * height * nz)) cb = vk_cmd();
            VkBuffer input;
            VkDescriptorSet set = rows(ctx, &cb, z, nz, y, n, &input, &offset)
                                      ? two_buffers(input, offset, size, g_astc.scratch, texels)
                                      : VK_NULL_HANDLE;
            if (!set) {
                LOG_LIMIT(1, "vulkan: out of staging memory for a %ux%u ASTC texture (left out)\n", w, h);
                return 0;
            }
            /* Submission makes the flushed host staging writes visible. Scratch
               reuse also needs a WAR dependency on the preceding image copy (or
               the BC7 encoder's read), including when that belongs to an earlier frame. */
            scratch_barrier(cb, g_astc.scratch, 0, 1, USE_WRITE);
            rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc.layout, 0, 1, &set, 0, NULL);
            u32 shift = tile_shift(nx, n), tw = 1u << shift, th = 64u >> shift;
            AstcParams p = {w, height, nx, n, (u32)srgb, shift};
            rec_CmdPushConstants(cb, g_astc.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            vk_gpuprof_tag(VK_PROF_ASTC);
            rec_CmdDispatch(cb, (nx + tw - 1) / tw, (n + th - 1) / th, nz);
            VkBuffer from = g_astc.scratch;
            u32 row_texels = 0, image_rows = 0;
            if (bc7) {
                /* the texels into BC7 blocks, a block an invocation (bc7.comp), in rows of cx blocks */
                const u32 cy = (height + 3) / 4;
                VkDescriptorSet set7 =
                    two_buffers(g_astc.scratch, 0, texels, g_astc.bc7_scratch, (u64)cx * cy * nz * 16);
                if (!set7) {
                    LOG_LIMIT(1, "vulkan: no descriptor set left to encode a %ux%u ASTC texture as BC7 (left out)\n", w,
                              h);
                    return 0;
                }
                scratch_barrier(cb, g_astc.scratch, USE_WRITE, 0, USE_READ);
                scratch_barrier(cb, g_astc.bc7_scratch, USE_COPY, 0, USE_WRITE);
                rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    build == 2   ? g_astc.bc7_mode6_pipe
                                    : build == 1 ? g_astc.bc7_fast_pipe
                                                 : g_astc.bc7_pipe);
                rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc.bc7_layout, 0, 1, &set7, 0, NULL);
                Bc7Params q = {w, height, cx, cy};
                rec_CmdPushConstants(cb, g_astc.bc7_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof q, &q);
                vk_gpuprof_tag(VK_PROF_BC7);
                rec_CmdDispatch(cb, (cx + 63) / 64, cy, nz);
                scratch_barrier(cb, g_astc.bc7_scratch, USE_WRITE, 0, USE_COPY);
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
                scratch_barrier(cb, g_astc.scratch, USE_WRITE, 0, USE_COPY);
                g_astc.rgba8_bytes += texels;
            }
            VkBufferImageCopy copy = {
                .bufferRowLength = row_texels,
                .bufferImageHeight = image_rows,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, as_layers ? layer + z : layer, as_layers ? nz : 1},
                .imageOffset = {(i32)x, (i32)(y0 + y * bh), as_layers ? 0 : (i32)z},
                .imageExtent = {w, height, as_layers ? 1 : nz}};
            rec_CmdCopyBufferToImage(cb, from, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
    }
    if (capture >= 0) { /* the blocks are the host's to read once the submission recording now is done */
        VkBufferMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                    .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .buffer = back,
                                    .offset = g_astc.captures[capture].off,
                                    .size = blocks};
        VkDependencyInfo d = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b};
        rec_CmdPipelineBarrier2(cb, &d);
        g_astc.captures[capture].key[0] = key[0];
        g_astc.captures[capture].key[1] = key[1];
        g_astc.captures[capture].size = blocks;
        g_astc.captures[capture].sub = vk_submit_number();
        g_astc.captured += blocks;
    }
    return 1;
}
