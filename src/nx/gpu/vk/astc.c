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
   from the cache into the image, with no decode and no encode (bc7_from_cache).

   A very large level (astc_quick_mtexels, every layer counted) takes the quick build, mode 6 from one line, where the
   frame needs it, and its fast build is made in the background, on the GPU or the CPU, into the cache in pieces and
   into the texture (below split_before). */
#include <stdlib.h>
#include <string.h>
#include "common/utilities/common.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/texture/astc_cpu.h"       /* the background's decoder on the CPU */
#include "nx/gpu/texture/bc7_encode_cpu.h" /* and its encoder */
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
    VkShaderModule bc7_module, bc7_fast_module, bc7_mode6_module, bc7_quick_module;
    VkPipeline bc7_pipe, bc7_fast_pipe, bc7_mode6_pipe, bc7_quick_pipe;
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
    VkShaderModuleCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bc7_quick_spv_size, .pCode = bc7_quick_spv};
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &g_astc.bc7_module)) ||
        !VK_OK(vkCreateShaderModule(g_vk.dev, &fi, NULL, &g_astc.bc7_fast_module)) ||
        !VK_OK(vkCreateShaderModule(g_vk.dev, &si, NULL, &g_astc.bc7_mode6_module)) ||
        !VK_OK(vkCreateShaderModule(g_vk.dev, &qi, NULL, &g_astc.bc7_quick_module)))
        fatal("vulkan: cannot load the BC7 encoder");
    const VkShaderModule modules[4] = {g_astc.bc7_module, g_astc.bc7_fast_module, g_astc.bc7_mode6_module,
                                       g_astc.bc7_quick_module};
    VkComputePipelineCreateInfo ci[4];
    for (int i = 0; i < 4; i++)
        ci[i] = (VkComputePipelineCreateInfo){.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                              .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                        .module = modules[i],
                                                        .pName = "main"},
                                              .layout = g_astc.bc7_layout};
    VkPipeline made[4];
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 4, ci, NULL, made)))
        fatal("vulkan: cannot create the BC7 encoder's pipelines");
    g_astc.bc7_pipe = made[0];
    g_astc.bc7_fast_pipe = made[1];
    g_astc.bc7_mode6_pipe = made[2];
    g_astc.bc7_quick_pipe = made[3];
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
   The SPIR-V of the deswizzle, the decoder and the encoder's three builds whose blocks it keeps (not the quick one's,
   which it never takes), which the build makes from the shaders (an edit to any changes it); the encoder's version;
   and the GPU and its driver, whose compilers run that SPIR-V */
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

/* the last submission vk_astc_frame_done had: what the background waits for is done up to it */
static u64 g_bg_landed;
static int is_bc7(const Surf* t) { return t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK || t->f.vk == VK_FORMAT_BC7_SRGB_BLOCK; }
/* the BC7 blocks of a level of w x h texels and depth slices (or layers) */
static u64 level_blocks(u32 w, u32 h, u32 depth) { return (u64)((w + 3) / 4) * ((h + 3) / 4) * depth * 16; }

static int quick_level(u64 texels);
int vk_astc_cached(const Surf* t, u32 w, u32 h, u32 depth) {
    u64 hits, stored, restarts, bytes;
    vk_bc7_cache_stats(&hits, &stored, &restarts, &bytes);
    const u64 n = level_blocks(w, h, depth);
    /* a very large level's goes in pieces, whatever its size */
    return is_bc7(t) && !t->sparse && bytes &&
           (quick_level((u64)w * h * depth) || (n <= VK_BC7_CAPTURE_BYTES && n <= g_vk.alloc_cap));
}

void vk_astc_frame_done(u64 sub) {
    if (sub > g_bg_landed) g_bg_landed = sub;
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

/* capture slot's blocks, size bytes of them copied into back at its offset: the host's to read once the submission
   recording now is done, when vk_astc_frame_done stores them under key */
static void capture_done(VkCommandBuffer cb, int slot, VkBuffer back, const u64 key[2], u64 size) {
    VkBufferMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .buffer = back,
                                .offset = g_astc.captures[slot].off,
                                .size = size};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
    g_astc.captures[slot].key[0] = key[0];
    g_astc.captures[slot].key[1] = key[1];
    g_astc.captures[slot].size = size;
    g_astc.captures[slot].sub = vk_submit_number();
    g_astc.captured += size;
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

/* ---- Very large levels: the quick build now, the fast build in the background

   A level of astc_quick_mtexels megatexels or more, all its layers or slices counted, takes the encoder's quick build
   (mode 6 from one line) in the frame that needs it, and its fast build is made in the background: into the BC7 cache,
   so that the next upload of the same blocks (a later run's, or this run's after an eviction) copies them with no
   encode, and into the texture as it comes, while the texture holds that upload. One game's first 3D frame uploads two
   1365x1365 ASTC 4x4 arrays of 90 layers, 168 MB of BC7 blocks each in their first level. The fast build took a Radeon
   890M 10.1 seconds of that frame, as the levels counted one layer (1.9 megatexels, under VK_BC7_MODE6_TEXELS), and
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

   astc_background picks who encodes. 2 (the default): the GPU, a piece after each present in a submission of its own
   (vk_astc_background), the next one once it is done: on the 890M the two arrays took 13 seconds of its time over 133
   seconds of play, 2 to 10 ms a frame where the GPU idled 30, and the frame rate stayed within the runs' own spread
   (docs/ASTC.md). 1: worker threads at the system's idle priority (plat_thread_idle), which run the CPU decoder and
   the encoder's fast build (astc_cpu.c, bc7_encode_cpu.cpp). Their blocks are the GPU's byte for byte (integer
   arithmetic only, held to it by tests/astc_vulkan.c), so they go under the same keys. On that game's blocks a Zen 5
   core encodes 0.7 megatexels a second (most blocks try every mode), 10 minutes of CPU time for the arrays: for a
   game the GPU holds back, on a machine with cores to spare. The GPU thread stores what either made: bc7_cache.c is
   its alone */
#define BC7_PIECE_BYTES  (256u << 10)
/* the snapshots of the levels in the background at once */
#define BG_SNAPSHOTS_MAX (1ull << 30)
/* the pieces the workers encoded and the GPU thread has not stored yet: they wait while it presents nothing (a
   loading screen), which holds their blocks in memory */
#define BG_DONE_MAX      64
/* A hit reads the level's pieces on the GPU thread, which the frame waits for: 21 ms for a 168 MB level in the page
   cache, 4.2 s from a disk that gave 40 MB/s (an NVMe drive in a USB enclosure at USB 2 speed). Past this, for all the
   frame's hits, a level is encoded quickly instead, and its pieces load into the texture after the presents that follow, BG_LOAD_NS of reads
   after each (at least a piece): 6.5 ms a piece from that disk, 0.1 from memory. Where every hit stalls, a run after
   a reboot would have stalled longer than the first run did */
#define BG_HIT_NS        (100 * 1000000ull)
#define BG_LOAD_NS       (2 * 1000000ull)

static int quick_level(u64 texels) {
    const u64 m = g_settings.astc_quick_mtexels > 0 ? (u64)g_settings.astc_quick_mtexels : 0;
    return m && texels >= m << 20;
}

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
    u8* state; /* each piece's BG_* (g_bg.lock) */
    u32 next;  /* no piece before it is BG_TODO */
    u32 left;  /* pieces not BG_DONE */
    u32 busy;  /* pieces taken by workers and not stored yet: the snapshot is read, a Bc7Done names it (g_bg.lock) */
    u64 ready_sub;
    int ready;
    VkBuffer snap;
    VmaAllocation snap_mem;
    const u8* snap_ptr;
    u64 snap_bytes;
    u64 t0, cpu_ns;          /* when it was made; the workers' CPU time on it */
    u32 by_cpu, by_gpu, had; /* pieces the workers and the GPU encoded, and those the cache held already */
    /* the texture whose upload made it, while its image holds what that upload wrote there (vk_astc_forget): each
       piece goes into it too, at level, from layer (as_layers: the slices are its layers), so this run shows the
       better blocks as they come */
    Surf* surf;
    u32 level, layer;
    int as_layers;
} Bc7Job;
/* a piece a worker encoded, for the GPU thread to store */
typedef struct Bc7Done {
    struct Bc7Done* next;
    Bc7Job* job;
    u32 piece;
    u8* blocks;
} Bc7Done;
static struct {
    PMutex lock; /* the jobs' list and piece states, their busy counts and the done list: the GPU thread's and the
                    workers' */
    PCond work;
    int cond_made;
    Bc7Job** jobs;
    u32 njobs, cap;
    Bc7Done* done;
    u32 ndone; /* in done */
    u32 threads;
    u64 snap_bytes;
    u64 slice_sub; /* the GPU's piece in flight: its submission (0: none), its job and piece */
    Bc7Job* slice_job;
    u32 slice_piece;
    u64 quick, made, finished, hits, stored, cpu_ns, gpu_pieces, loaded;
    u32 loaded_now; /* pieces bg_load read after this present */
} g_bg = {.lock = PMUTEX_INIT};
u64 g_vk_astc_hit_ns = BG_HIT_NS;

/* a ready job's piece that nobody encodes yet, taken (g_bg.lock): 0 when there is none, or BG_DONE_MAX wait to be
   stored */
static int bg_take(Bc7Job** job, u32* piece) {
    if (g_bg.ndone >= BG_DONE_MAX) return 0;
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

/* piece i of j on the CPU: its rows decoded from the snapshot and encoded, its blocks (malloc'd), NULL when memory ran
   out. A strip as the GPU's: the level's own height at its last band, whose blocks repeat their last row */
static u8* bg_encode_cpu(const Bc7Job* j, u32 i) {
    Bc7Piece q;
    piece_at(&j->p, j->key, i, &q);
    const u32 y = q.r * 4 / j->bh, height = j->h - q.r * 4 < q.nr * 4 ? j->h - q.r * 4 : q.nr * 4;
    u8 *rgba = malloc((size_t)j->w * height * q.nz * 4), *out = malloc((size_t)q.bytes);
    if (!rgba || !out) {
        free(rgba);
        free(out);
        return NULL;
    }
    for (u32 k = 0; k < q.nz; k++)
        astc_decode_image(j->snap_ptr + ((u64)(q.z + k) * j->ny + y) * j->nx * 16, j->w, height, j->bw, j->bh,
                          rgba + (size_t)k * j->w * height * 4, (int)j->srgb);
    bc7_encode_cpu_strip(rgba, j->w, height, q.nz, out);
    free(rgba);
    return out;
}

/* A worker: pieces of ready jobs while astc_background is 1, else asleep. Below every thread of the game: a CPU the
   game wants is never the encoder's, and the GPU thread never waits for a worker */
static void bg_worker(void* arg) {
    (void)arg;
    SNX_THREAD_NAME("BC7 background");
    plat_thread_idle();
    const SnxU64 clock = snx_thread_clock(); /* its CPU time, for the log (0: the system has none) */
    pm_lock(&g_bg.lock);
    for (;;) {
        Bc7Job* j;
        u32 i;
        if (g_settings.astc_background != 1 || !bg_take(&j, &i)) {
            pc_wait(&g_bg.work, &g_bg.lock, -1);
            continue;
        }
        j->busy++;
        pm_unlock(&g_bg.lock);
        SnxU64 c0 = 0, c1 = 0;
        if (clock) snx_thread_cpu_ns(clock, &c0);
        u8* blocks = bg_encode_cpu(j, i);
        if (clock) snx_thread_cpu_ns(clock, &c1);
        const u64 spent = c1 > c0 ? c1 - c0 : 0;
        Bc7Done* d = blocks ? malloc(sizeof *d) : NULL;
        pm_lock(&g_bg.lock);
        j->cpu_ns += spent;
        g_bg.cpu_ns += spent;
        if (!d) { /* out of memory: the piece is left to a later run */
            free(blocks);
            j->state[i] = BG_DONE;
            j->left--;
            j->busy--;
            continue;
        }
        *d = (Bc7Done){g_bg.done, j, i, blocks};
        g_bg.done = d;
        g_bg.ndone++;
        j->by_cpu++;
    }
}
/* astc_background_threads, or a quarter of the CPUs the game may use, from 1 to 8: the workers only take CPUs the
   game leaves idle, but more of them take more of a laptop's power, which its game threads' clocks pay for */
static u32 bg_workers(void) {
    static int cpus = -1;
    if (g_settings.astc_background_threads > 0) return (u32)g_settings.astc_background_threads;
    if (cpus < 0) {
        int ids[256];
        cpus = plat_cpus(ids, 256);
    }
    return cpus <= 0 ? 2 : cpus / 4 < 1 ? 1 : cpus / 4 > 8 ? 8 : (u32)(cpus / 4);
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

/* the pieces the workers encoded, into the cache and into their texture (after a present: vk_lock, outside any
   rendering pass) */
static void bg_store_done(void) {
    if (!__atomic_load_n(&g_bg.done, __ATOMIC_RELAXED)) return;
    pm_lock(&g_bg.lock);
    Bc7Done* d = g_bg.done;
    g_bg.done = NULL;
    g_bg.ndone = 0;
    pm_unlock(&g_bg.lock);
    while (d) {
        Bc7Done* next = d->next;
        Bc7Piece q;
        piece_at(&d->job->p, d->job->key, d->piece, &q);
        vk_bc7_cache_put(q.key, d->blocks, q.bytes);
        g_bg.stored += vk_bc7_cache_find(q.key) == q.bytes;
        VkBuffer buf;
        u64 off;
        u8* to = d->job->surf ? vk_stage(q.bytes, 16, &buf, &off) : NULL;
        if (to) {
            memcpy(to, d->blocks, (size_t)q.bytes);
            bg_to_image(vk_cmd(), d->job, &q, buf, off);
        }
        pm_lock(&g_bg.lock);
        d->job->state[d->piece] = BG_DONE;
        d->job->left--;
        d->job->busy--;
        pm_unlock(&g_bg.lock);
        free(d->blocks);
        free(d);
        d = next;
    }
}

/* j's snapshot freed once the GPU's copies into it are done (it is in host memory, but GPU memory on some devices),
   and j with it (g_bg.lock not held; no worker reads it) */
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
    VmaAllocationCreateInfo ai = {.flags =
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
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
        j->snap_ptr = info.pMappedData;
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
                                .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                                .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
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
        pm_lock(&g_bg.lock);
        g_bg.jobs = grown;
        g_bg.cap = cap;
        pm_unlock(&g_bg.lock);
    }
    pm_lock(&g_bg.lock);
    g_bg.jobs[g_bg.njobs++] = j;
    pm_unlock(&g_bg.lock);
    g_bg.made++;
    LOG_LIMIT(16,
              "vulkan: a %ux%ux%u ASTC level of %llu megatexels is in the quick BC7 build for now; its fast build "
              "comes in the background (%s): %u pieces of %u to make, %u to load from the BC7 cache\n",
              j->w, j->h, j->depth, (unsigned long long)(((u64)j->w * j->h * j->depth) >> 20),
              g_settings.astc_background == 2 ? "on the GPU" : "on the CPU", j->left - j->had, j->p.n, j->had);
}

/* The GPU's piece: the snapshot's rows of it copied into the frame's device memory, decoded, encoded with the fast
   build and read back into the cache as an encode's blocks are (bc7_capture), in a submission of its own after the
   frame's. The next waits until it is done, so the GPU has one piece of the background's at a time */
static void bg_gpu_piece(void) {
    Bc7Job* j;
    u32 i;
    pm_lock(&g_bg.lock);
    int took = bg_take(&j, &i);
    pm_unlock(&g_bg.lock);
    if (!took) return;
    Bc7Piece q;
    piece_at(&j->p, j->key, i, &q);
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    const u32 y = q.r * 4 / j->bh, height = j->h - q.r * 4 < q.nr * 4 ? j->h - q.r * 4 : q.nr * 4;
    const u32 n = (height + j->bh - 1) / j->bh;
    const u64 row_in = (u64)j->nx * 16, in_size = row_in * n * q.nz, texels = (u64)j->w * height * q.nz * 4;
    const u64 align = lim->minStorageBufferOffsetAlignment < 16 ? 16 : lim->minStorageBufferOffsetAlignment;
    VkBuffer back, in;
    u64 in_at;
    VkDescriptorSet set = VK_NULL_HANDLE, set7 = VK_NULL_HANDLE;
    int slot = bc7_capture(q.key, q.bytes, &back);
    if (slot >= 0 && vk_stage_device(in_size, align, &in, &in_at)) {
        set = two_buffers(in, in_at, in_size, g_astc.scratch, texels);
        set7 = set ? two_buffers(g_astc.scratch, 0, texels, g_astc.bc7_scratch, q.bytes) : VK_NULL_HANDLE;
    }
    if (!set7) { /* the ring or the descriptors ran out this frame: again after the next present */
        pm_lock(&g_bg.lock);
        j->state[i] = BG_TODO;
        pm_unlock(&g_bg.lock);
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
    scratch_barrier(cb, g_astc.scratch, 0, 1, USE_WRITE);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, astc_pipeline(j->bw, j->bh));
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc.layout, 0, 1, &set, 0, NULL);
    u32 shift = tile_shift(j->nx, n), tw = 1u << shift, th = 64u >> shift;
    AstcParams p = {j->w, height, j->nx, n, j->srgb, shift};
    rec_CmdPushConstants(cb, g_astc.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vk_gpuprof_tag(VK_PROF_BC7_BACKGROUND);
    rec_CmdDispatch(cb, (j->nx + tw - 1) / tw, (n + th - 1) / th, q.nz);
    scratch_barrier(cb, g_astc.scratch, USE_WRITE, 0, USE_READ);
    scratch_barrier(cb, g_astc.bc7_scratch, USE_COPY, 0, USE_WRITE);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc.bc7_fast_pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc.bc7_layout, 0, 1, &set7, 0, NULL);
    Bc7Params bp = {j->w, height, j->p.cx, q.nr};
    rec_CmdPushConstants(cb, g_astc.bc7_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof bp, &bp);
    vk_gpuprof_tag(VK_PROF_BC7_BACKGROUND);
    rec_CmdDispatch(cb, (j->p.cx + 63) / 64, q.nr, q.nz);
    scratch_barrier(cb, g_astc.bc7_scratch, USE_WRITE, 0, USE_COPY);
    const VkBufferCopy to = {0, g_astc.captures[slot].off, q.bytes};
    rec_CmdCopyBuffer(cb, g_astc.bc7_scratch, back, 1, &to);
    bg_to_image(cb, j, &q, g_astc.bc7_scratch, 0);
    capture_done(cb, slot, back, q.key, q.bytes);
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
            pm_lock(&g_bg.lock);
            j->state[i] = ok || !j->snap ? BG_DONE : BG_TODO;
            j->left -= ok || !j->snap;
            pm_unlock(&g_bg.lock);
        }
    }
}

void vk_astc_background(void) {
    if (!g_bg.njobs) return;
    g_bg.loaded_now = 0;
    const int mode = g_settings.astc_background;
    bg_store_done();
    if (g_bg.slice_sub && g_bg_landed >= g_bg.slice_sub) { /* the GPU's piece, which its landing stored */
        Bc7Job* j = g_bg.slice_job;
        Bc7Piece q;
        piece_at(&j->p, j->key, g_bg.slice_piece, &q);
        const int stored = vk_bc7_cache_find(q.key) == q.bytes;
        g_bg.stored += stored;
        pm_lock(&g_bg.lock);
        j->state[g_bg.slice_piece] = stored ? BG_DONE : BG_TODO;
        j->left -= stored;
        pm_unlock(&g_bg.lock);
        g_bg.slice_sub = 0;
        g_bg.slice_job = NULL;
    }
    int wake = 0;
    for (u32 k = 0; k < g_bg.njobs;) {
        Bc7Job* j = g_bg.jobs[k];
        if (!j->ready && g_bg_landed >= j->ready_sub) {
            if (j->snap) vmaInvalidateAllocation(g_vk.vma, j->snap_mem, 0, VK_WHOLE_SIZE);
            pm_lock(&g_bg.lock);
            j->ready = 1;
            pm_unlock(&g_bg.lock);
            wake = 1;
        }
        pm_lock(&g_bg.lock);
        const int end = (!j->left || !mode) && !j->busy && g_bg.slice_job != j;
        if (end) g_bg.jobs[k] = g_bg.jobs[--g_bg.njobs];
        pm_unlock(&g_bg.lock);
        if (!end) {
            k++;
            continue;
        }
        if (!j->left) {
            g_bg.finished++;
            LOG("vulkan: the fast build of a %ux%ux%u ASTC level is in the BC7 cache: %u pieces (%llu MB) in %.1f s, "
                "%u on the CPU (%.1f s of its time), %u on the GPU, %u there already\n",
                j->w, j->h, j->depth, j->p.n, (unsigned long long)(((u64)j->p.cx * j->p.cy * j->depth * 16) >> 20),
                (double)(plat_ns() - j->t0) / 1e9, j->by_cpu, (double)j->cpu_ns / 1e9, j->by_gpu, j->had);
        }
        bg_job_free(j);
    }
    if (mode == 1 && g_bg.njobs) {
        if (!g_bg.cond_made) {
            pc_init(&g_bg.work);
            g_bg.cond_made = 1;
        }
        for (u32 n = bg_workers(); g_bg.threads < n; g_bg.threads++)
            if (plat_thread(bg_worker, NULL, (size_t)1 << 20)) break;
        if (wake || g_bg.njobs) pc_broadcast(&g_bg.work);
    }
    bg_load();
    if (mode == 2 && !g_bg.slice_sub) bg_gpu_piece();
}

void vk_astc_background_drain(void) {
    while (g_bg.njobs) {
        int copying = 0;
        for (u32 k = 0; k < g_bg.njobs; k++) copying |= !g_bg.jobs[k]->ready;
        if (copying || g_bg.slice_sub) vk_finish(); /* lands the snapshots' copies, or the GPU's piece */
        vk_astc_background();
        if (g_bg.njobs && !g_bg.slice_sub) plat_sleep_ns(1000000); /* the workers' */
    }
}

void vk_astc_background_stats(VkAstcBackground* s) {
    *s = (VkAstcBackground){g_bg.quick,  g_bg.made,   g_bg.finished,   g_bg.njobs,   g_bg.hits,
                            g_bg.stored, g_bg.cpu_ns, g_bg.gpu_pieces, g_bg.loaded, g_bg.threads};
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
            LOG_LIMIT(4, "vulkan: a %ux%ux%u ASTC level's pieces read slowly from the BC7 cache: it is encoded quickly "
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

int vk_astc_upload(VkCommandBuffer cb, Surf* t, u32 x, u32 y0, u32 w, u32 h, u32 depth, u32 level, u32 layer,
                   u32 layers, VkAstcRows rows, void* ctx, const u64* content) {
    /* layers of a 2D array: decoded as the slices of a volume are, each strip of them copied at once */
    const int as_layers = layers > 1 && depth == 1;
    if (as_layers) depth = layers;
    vk_astc_forget(t, level); /* what this writes is not the background's to overwrite */
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
       encodes each block as its whole upload does, where the full build chose other modes for some of them. A very
       large level, every layer of a 2D array counted (quick_level), takes the quick build, mode 6 from one line, and
       the fast build in the background; a sparse one keeps the rule of a layer's size, as only its mapped strips are
       decoded and none goes to the cache, so the quick blocks would stay */
    const u32 level_w = t->w >> level ? t->w >> level : 1, level_h = t->h >> level ? t->h >> level : 1;
    const u32 level_d = t->type == VK_IMAGE_VIEW_TYPE_3D && t->depth3d >> level ? t->depth3d >> level : 1;
    const u64 level_texels = (u64)level_w * level_h * level_d;
    const int quick = bc7 && !t->sparse &&
                      quick_level(level_texels * (t->type == VK_IMAGE_VIEW_TYPE_3D || !t->layers ? 1 : t->layers));
    const u32 build = quick ? 3 : level_texels >= VK_BC7_MODE6_TEXELS ? 2 : level_texels >= VK_BC7_FAST_TEXELS ? 1 : 0;
    /* the BC7 cache: the level's blocks from it, or the encode's copied into the readback ring as well as into the
       image, a strip at a time in their place in the level, for vk_astc_frame_done to put in it. A very large level's
       are its fast build's, in pieces, which the background makes from a snapshot of its blocks */
    const u64 blocks = level_blocks(w, h, depth);
    u64 key[2];
    int capture = -1;
    VkBuffer back = VK_NULL_HANDLE;
    Bc7Job* job = NULL;
    if (bc7 && content && !sh && vk_astc_cached(t, w, h, depth)) {
        const u32 kept = quick ? 1 : build; /* a very large level's pieces are its fast build's */
        const VkBc7Level lv = {w, h, as_layers ? 1 : depth, as_layers ? depth : 1, bw, bh, (u32)srgb, kept};
        vk_bc7_key(content, &lv, key);
        if (quick ? !x && !y0 && pieces_from_cache(cb, t, key, w, h, depth, bh, level, layer, as_layers)
                  : bc7_from_cache(cb, t, key, blocks, w, h, depth, level, layer, as_layers)) {
            g_astc.bc7_levels++;
            g_astc.bc7_hits++;
            g_astc.bc7_bytes += blocks;
            g_astc.bc7_rgba8_bytes += (u64)w * h * depth * 4;
            g_bg.hits += quick;
            if (quick)
                LOG_LIMIT(16, "vulkan: a %ux%ux%u ASTC level's fast build came from the BC7 cache, in pieces\n", w, h,
                          depth);
            return 1;
        }
        if (!quick)
            capture = bc7_capture(key, blocks, &back);
        else if (!x && !y0)
            job = bg_job_new(key, w, h, depth, bw, bh, (u32)srgb, t, level, layer, as_layers);
    }
    g_bg.quick += quick && !x && !y0;
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
                if (job) bg_job_free(job);
                return 0;
            }
            if (job && job->snap) { /* the strip's blocks into their place in the snapshot: rows of a slice, or slices */
                const VkBufferCopy snap = {offset, ((u64)z * ny + y) * row_in, size};
                rec_CmdCopyBuffer(cb, input, job->snap, 1, &snap);
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
                    if (job) bg_job_free(job);
                    return 0;
                }
                scratch_barrier(cb, g_astc.scratch, USE_WRITE, 0, USE_READ);
                scratch_barrier(cb, g_astc.bc7_scratch, USE_COPY, 0, USE_WRITE);
                rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    build == 3   ? g_astc.bc7_quick_pipe
                                    : build == 2 ? g_astc.bc7_mode6_pipe
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
    if (capture >= 0) capture_done(cb, capture, back, key, blocks);
    if (job) bg_job_add(job, cb);
    return 1;
}
