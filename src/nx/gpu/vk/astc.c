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

   A BC7 image (format.c's TA, the astc_bc setting, on a GPU that samples BCn but not ASTC) takes a second
   dispatch: bc7.comp encodes the decoded texels into BC7 blocks, a quarter of RGBA8's bytes, and those are what
   the copy writes. Its strips then start on whole 4x4 blocks. */
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/texture/astc_spv.h" /* compiled from astc.comp by the build (cmake/glsl.cmake) */
#include "nx/gpu/texture/bc7_spv.h"  /* compiled from bc7.comp */
#include "nx/runtime/platform/log.h"

static VkDescriptorSetLayout g_astc_ds;
static VkPipelineLayout g_astc_layout;
static VkShaderModule g_astc_module;
/* a pipeline per footprint (4 to 12 texels each way), made on first use: the footprint is a specialization constant,
   so the driver folds what depends on it however little it optimizes otherwise */
static VkPipeline g_astc_pipes[9][9];
static VkBuffer g_astc_scratch;
static VmaAllocation g_astc_mem;
static u64 g_astc_cap;
/* the BC7 encoder, made on the first BC7 image, and the blocks it writes: as large as the texels' scratch, which a
   strip's blocks never pass (vk_astc_upload sizes strips by both) */
static VkPipelineLayout g_bc7_layout;
static VkShaderModule g_bc7_module;
static VkPipeline g_bc7_pipe;
static VkBuffer g_bc7_scratch;
static VmaAllocation g_bc7_mem;
/* image levels uploaded as BC7, their bytes, and what they would have taken as RGBA8 (vk_astc_stats) */
static u64 g_bc7_levels, g_bc7_bytes, g_bc7_rgba8_bytes;

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
    if (g_astc_module) return;
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_astc_ds)))
        fatal("vulkan: cannot create the ASTC descriptor layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcParams)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_astc_ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_astc_layout)))
        fatal("vulkan: cannot create the ASTC pipeline layout");
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = astc_spv_size, .pCode = astc_spv};
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &g_astc_module)))
        fatal("vulkan: cannot load the ASTC shader");
    /* Kept for the device lifetime, like the other internal pipelines. All uses
       are on g_vk.gfx, under vk_lock(), even when uploads originate in compute. */
    g_astc_cap = 16ull << 20; /* a 2048x2048 slice a strip: fewer barriers between decoding and copying */
    if (g_astc_cap > g_vk.props.limits.maxStorageBufferRange) g_astc_cap = g_vk.props.limits.maxStorageBufferRange;
    if (g_astc_cap > g_vk.alloc_cap) g_astc_cap = g_vk.alloc_cap;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = g_astc_cap,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_astc_scratch, &g_astc_mem, NULL)))
        fatal("vulkan: cannot allocate ASTC scratch memory");
    vk_mem_track(g_astc_mem);
}

/* bc7.comp's push constants: the strip's width and its slices' height in texels and in blocks */
typedef struct {
    u32 w, h, blocks_x, blocks_y;
} Bc7Params;

static void bc7_init(void) {
    if (g_bc7_pipe) return;
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Bc7Params)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_astc_ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_bc7_layout)))
        fatal("vulkan: cannot create the BC7 encoder's pipeline layout");
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bc7_spv_size, .pCode = bc7_spv};
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &g_bc7_module))) fatal("vulkan: cannot load the BC7 encoder");
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = g_bc7_module,
                                                .pName = "main"},
                                      .layout = g_bc7_layout};
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &g_bc7_pipe)))
        fatal("vulkan: cannot create the BC7 encoder's pipeline");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = g_astc_cap,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_bc7_scratch, &g_bc7_mem, NULL)))
        fatal("vulkan: cannot allocate the BC7 encoder's scratch memory");
    vk_mem_track(g_bc7_mem);
}

void vk_astc_stats(u64* levels, u64* bytes, u64* rgba8_bytes) {
    *levels = g_bc7_levels;
    *bytes = g_bc7_bytes;
    *rgba8_bytes = g_bc7_rgba8_bytes;
}

static VkPipeline astc_pipeline(u32 bw, u32 bh) {
    VkPipeline* pipe = &g_astc_pipes[bw - 4][bh - 4];
    if (*pipe) return *pipe;
    const u32 dims[2] = {bw, bh};
    const VkSpecializationMapEntry entries[2] = {{0, 0, sizeof(u32)}, {1, sizeof(u32), sizeof(u32)}};
    VkSpecializationInfo spec = {.mapEntryCount = 2, .pMapEntries = entries, .dataSize = sizeof dims, .pData = dims};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = g_astc_module,
                                                .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = g_astc_layout};
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
    VkDescriptorSet set = vk_desc_alloc(g_astc_ds);
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

int vk_astc_upload(VkCommandBuffer cb, Surf* t, u32 w, u32 h, u32 depth, u32 level, u32 layer, VkAstcRows rows,
                   void* ctx) {
    astc_init();
    const int bc7 = t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK || t->f.vk == VK_FORMAT_BC7_SRGB_BLOCK;
    if (bc7) bc7_init();
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    u32 bw = t->f.bw, bh = t->f.bh;
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
    if ((u64)w * h * 4 <= g_astc_cap && slice_blocks <= g_astc_cap && row_in * ny <= in_cap &&
        ny <= lim->maxComputeWorkGroupCount[1]) {
        /* whole slices */
        u64 by_out = g_astc_cap / ((u64)w * h * 4), by_in = in_cap / (row_in * ny);
        slices = (u32)(by_out < by_in ? by_out : by_in);
        if (bc7 && g_astc_cap / slice_blocks < slices) slices = (u32)(g_astc_cap / slice_blocks);
        if (slices > lim->maxComputeWorkGroupCount[2]) slices = lim->maxComputeWorkGroupCount[2];
        if (slices > depth) slices = depth;
        nrows = ny;
    } else {
        u64 by_out = g_astc_cap / row_out, by_in = in_cap / row_in;
        nrows = (u32)(by_out < by_in ? by_out : by_in);
        if (nrows > lim->maxComputeWorkGroupCount[1]) nrows = lim->maxComputeWorkGroupCount[1];
        nrows -= nrows % unit;
    }
    if (bw < 4 || bw > 12 || bh < 4 || bh > 12 || !nrows || nx > lim->maxComputeWorkGroupCount[0] ||
        (bc7 && (cx + 63) / 64 > lim->maxComputeWorkGroupCount[0])) {
        LOG_LIMIT(1, "vulkan: a %ux%u ASTC texture exceeds the device's limits (left out)\n", w, h);
        return 0;
    }
    VkPipeline pipe = astc_pipeline(bw, bh);
    if (bc7) g_bc7_levels++;
    for (u32 z = 0; z < depth; z += slices) {
        u32 nz = depth - z < slices ? depth - z : slices;
        for (u32 y = 0; y < ny; y += nrows) {
            u32 n = ny - y < nrows ? ny - y : nrows;
            u32 height = h - y * bh < n * bh ? h - y * bh : n * bh;
            u64 size = row_in * n * nz, offset, texels = (u64)w * height * nz * 4;
            VkBuffer input;
            VkDescriptorSet set = rows(ctx, &cb, z, nz, y, n, &input, &offset)
                                      ? two_buffers(input, offset, size, g_astc_scratch, texels)
                                      : VK_NULL_HANDLE;
            if (!set) {
                LOG_LIMIT(1, "vulkan: out of staging memory for a %ux%u ASTC texture (left out)\n", w, h);
                return 0;
            }
            /* Submission makes the flushed host staging writes visible. Scratch
               reuse also needs a WAR dependency on the preceding image copy (or
               the BC7 encoder's read), including when that belongs to an earlier frame. */
            scratch_barrier(cb, g_astc_scratch, 0, 1, USE_WRITE);
            rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc_layout, 0, 1, &set, 0, NULL);
            u32 shift = tile_shift(nx, n), tw = 1u << shift, th = 64u >> shift;
            AstcParams p = {
                w, height, nx, n, t->f.vk == VK_FORMAT_R8G8B8A8_SRGB || t->f.vk == VK_FORMAT_BC7_SRGB_BLOCK, shift};
            rec_CmdPushConstants(cb, g_astc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            rec_CmdDispatch(cb, (nx + tw - 1) / tw, (n + th - 1) / th, nz);
            VkBuffer from = g_astc_scratch;
            u32 row_texels = 0, image_rows = 0;
            if (bc7) {
                /* the texels into BC7 blocks, a block an invocation (bc7.comp), in rows of cx blocks */
                const u32 cy = (height + 3) / 4;
                VkDescriptorSet set7 = two_buffers(g_astc_scratch, 0, texels, g_bc7_scratch, (u64)cx * cy * nz * 16);
                if (!set7) {
                    LOG_LIMIT(1, "vulkan: no descriptor set left to encode a %ux%u ASTC texture as BC7 (left out)\n", w,
                              h);
                    return 0;
                }
                scratch_barrier(cb, g_astc_scratch, USE_WRITE, 0, USE_READ);
                scratch_barrier(cb, g_bc7_scratch, USE_COPY, 0, USE_WRITE);
                rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_bc7_pipe);
                rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_bc7_layout, 0, 1, &set7, 0, NULL);
                Bc7Params q = {w, height, cx, cy};
                rec_CmdPushConstants(cb, g_bc7_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof q, &q);
                rec_CmdDispatch(cb, (cx + 63) / 64, cy, nz);
                scratch_barrier(cb, g_bc7_scratch, USE_WRITE, 0, USE_COPY);
                from = g_bc7_scratch;
                row_texels = cx * 4;
                image_rows = cy * 4;
                g_bc7_bytes += (u64)cx * cy * nz * 16;
                g_bc7_rgba8_bytes += texels;
            } else {
                scratch_barrier(cb, g_astc_scratch, USE_WRITE, 0, USE_COPY);
            }
            VkBufferImageCopy copy = {.bufferRowLength = row_texels,
                                      .bufferImageHeight = image_rows,
                                      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
                                      .imageOffset = {0, (i32)(y * bh), (i32)z},
                                      .imageExtent = {w, height, nz}};
            rec_CmdCopyBufferToImage(cb, from, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
    }
    return 1;
}
