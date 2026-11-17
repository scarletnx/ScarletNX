/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Block linear surfaces deswizzled and swizzled on the GPU (gpu/texture/blocklinear.comp). Guest bytes go to the GPU
   as they are, a copy of whole ranges into staging, and come back the same way: the 16 byte pieces of a GOB are
   moved by the shader, between the staged bytes and linear rows in a device-local scratch buffer that copies to
   and from images take. One pipeline serves both directions, and a second build of the shader (BL_RUNS) reads the
   runs that GPU results hold in place. Each is made on first use and kept for the device's lifetime, like the other
   internal pipelines. Uses are on g_vk.gfx, under vk_lock() */
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/texture/blocklinear.h"
#include "nx/gpu/texture/blocklinear_spv.h" /* compiled from blocklinear.comp by the build (cmake/glsl.cmake) */
#include "nx/gpu/texture/blocklinear_runs_spv.h" /* the same with BL_RUNS: runs read in place by address */
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* blocklinear.comp's push constants, member for member */
typedef struct {
    u32 runs, rows, lin_runs, y0, gob_y0, z0, bh, hmask, bd, dmask, bsh, block_row, slab, bl_bias, lin_bias, to_bl;
    u32 tail_mask[4];
    u32 native;
    u32 tab_bias, nruns; /* BL_RUNS only: where the staged range's first byte is in the bound range, and its runs */
} BlParams;

/* a pipeline of the moves: its layouts and itself */
typedef struct {
    VkDescriptorSetLayout ds;
    VkPipelineLayout layout;
    VkPipeline pipe;
} BlPipe;
/* the moves' pipelines (runs: a move that reads runs in place), and the scratch buffer with its memory and size, and
   the most asked of it since vk_bl_scratch_asked last read it */
static struct {
    BlPipe plain, runs;
    VkBuffer scratch;
    DevMem mem;
    u64 cap, asked;
    u64 recent; /* the most asked since vk_bl_scratch_trim last looked */
} g_blocklinear;

static const BlPipe* bl_init(int runs) {
    BlPipe* p = runs ? &g_blocklinear.runs : &g_blocklinear.plain;
    if (p->pipe) return p;
    if (!VK_OK(snx_vk_compute_layouts(g_vk.dev, runs ? 3 : 2, 0, sizeof(BlParams), &p->ds, &p->layout)))
        fatal("vulkan: cannot create the block linear descriptor and pipeline layouts");
    VkShaderModule module;
    if (!VK_OK(snx_vk_shader_module(g_vk.dev, runs ? blocklinear_runs_spv : blocklinear_spv,
                                    runs ? blocklinear_runs_spv_size : blocklinear_spv_size, &module)))
        fatal("vulkan: cannot load the block linear shader");
    u64 started = plat_ns();
    if (!VK_OK(snx_vk_module_pipeline(g_vk.dev, vk_pipeline_cache(), p->layout, module, NULL, &p->pipe)))
        fatal("vulkan: cannot create the block linear compute pipeline");
    vk_texture_pipeline_made(plat_ns() - started);
    vkDestroyShaderModule(g_vk.dev, module, NULL); /* the pipeline doesn't need it once made */
    return p;
}

/* The scratch buffer stays as big as the most asked of it, and in Mario Kart World on a GPU of 2 GB one target's load
   of 144 MB made it 256 MB for the rest of the run, where the DMA engine's moves after it asked 64 MB at most: a
   fifth of the device's budget held for nothing while its depth targets and storage images found no memory. Once a
   present with the heap past four fifths of its budget, one more than twice the most asked of it since the last
   present goes, and the next move makes one of the size it asks */
void vk_bl_scratch_trim(void) {
    const u64 recent = g_blocklinear.recent;
    g_blocklinear.recent = 0;
    if (!g_blocklinear.scratch || g_blocklinear.cap <= (4ull << 20) || g_blocklinear.cap / 2 <= recent) return;
    u64 usage, budget;
    vk_mem_use(&usage, &budget);
    if (usage < budget / 5 * 4) return;
    vk_defer_buffer(g_blocklinear.scratch, g_blocklinear.mem); /* once the frame's work that may use it is done */
    g_blocklinear.scratch = VK_NULL_HANDLE;
    g_blocklinear.cap = 0;
}
u64 vk_bl_scratch_asked(void) {
    u64 asked = g_blocklinear.asked;
    g_blocklinear.asked = 0;
    return asked;
}
VkBuffer vk_bl_scratch(u64 size, u64* cap_out) {
    if (size > g_blocklinear.asked) g_blocklinear.asked = size;
    if (size > g_blocklinear.recent) g_blocklinear.recent = size;
    *cap_out = g_blocklinear.cap;
    if (size <= g_blocklinear.cap) return g_blocklinear.scratch;
    if (size > g_vk.alloc_cap) return VK_NULL_HANDLE;
    /* the old one stays alive until the frame's work that may use it is done */
    if (g_blocklinear.scratch) vk_defer_buffer(g_blocklinear.scratch, g_blocklinear.mem);
    g_blocklinear.scratch = VK_NULL_HANDLE;
    g_blocklinear.cap = 0;
    u64 cap = 4ull << 20;
    while (cap < size) cap *= 2;
    if (cap > g_vk.alloc_cap) cap = g_vk.alloc_cap;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = cap,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant ai = {.at = DM_DEVICE};
    if (!VK_OK(vk_buffer_create(&bi, &ai, &g_blocklinear.scratch, &g_blocklinear.mem, NULL))) {
        g_blocklinear.scratch = VK_NULL_HANDLE;
        *cap_out = 0;
        return VK_NULL_HANDLE;
    }
    vk_mem_track(g_blocklinear.mem, VK_OWN_CONVERT);
    *cap_out = g_blocklinear.cap = cap;
    return g_blocklinear.scratch;
}

/* bytes from a row of blocks to the next, and from 2^bd slices to the next (gpu.c's bl_yz) */
static u64 block_row_bytes(const VkBlMove* m) {
    return (m->gobs_x ? (u64)m->gobs_x : (u64)(m->row_bytes + 63) >> 6) << (9 + m->bh + m->bd);
}
static u64 slab_bytes(const VkBlMove* m) {
    return (((((u64)m->height + 7) >> 3) + (1u << m->bh) - 1) >> m->bh) * block_row_bytes(m);
}
void vk_bl_range(const VkBlMove* m, u64* lo, u64* hi) {
    u32 last = m->y0 + m->rows - 1, zl = m->z0 + m->slices - 1;
    *lo = (u64)(m->z0 >> m->bd) * slab_bytes(m) + (u64)((m->y0 >> 3) >> m->bh) * block_row_bytes(m);
    *hi = (u64)(zl >> m->bd) * slab_bytes(m) + (u64)(((last >> 3) >> m->bh) + 1) * block_row_bytes(m);
}

/* global, as the buffers it orders are several and change */
void vk_bl_barrier(VkCommandBuffer cb, int before) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    if (before) {
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    } else {
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_HOST_READ_BIT;
    }
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}

/* vk_bl_move, and with s (out of block linear) its runs read in place */
static int bl_move(VkCommandBuffer cb, const VkBlMove* m, int to_bl, VkBuffer bl, u64 bl_at, VkBuffer lin, u64 lin_at,
                   const VkBlSrc* s) {
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    u64 align = lim->minStorageBufferOffsetAlignment < 16 ? 16 : lim->minStorageBufferOffsetAlignment;
    u64 lo, hi;
    if (!m->rows || !m->slices || !m->row_bytes) return 1;
    vk_bl_range(m, &lo, &hi);
    /* the bound ranges start on the device's alignment: the biases say where the data is in them. The layout's range
       ends at the last GOB column the move takes in its last row of blocks, short of hi by the columns of gobs_x past
       row_bytes: a move of a row's columns from one further in (its bl_at that many blocks on) stays in the buffer */
    u64 bl_off = bl_at & ~(align - 1), lin_off = lin_at & ~(align - 1);
    u64 past = m->gobs_x ? ((u64)m->gobs_x - ((m->row_bytes + 63) >> 6)) << (9 + m->bh + m->bd) : 0;
    u64 bl_size = bl_at + (hi - lo) - past - bl_off,
        lin_size = lin_at + (u64)m->slices * m->rows * m->lin_pitch - lin_off;
    u32 runs = (m->row_bytes + 15) >> 4, gx = (runs + 7) >> 3, gy = ((m->y0 + m->rows + 7) >> 3) - (m->y0 >> 3);
    if ((bl_at | lin_at | m->lin_pitch) & 15 || m->lin_pitch < runs * 16 || bl_size > lim->maxStorageBufferRange ||
        lin_size > lim->maxStorageBufferRange || gx > lim->maxComputeWorkGroupCount[0] ||
        gy > lim->maxComputeWorkGroupCount[1] || m->slices > lim->maxComputeWorkGroupCount[2]) {
        LOG_LIMIT(1, "vulkan: a %ux%u block linear surface exceeds the device's limits (left out)\n", m->row_bytes,
                  m->rows);
        return 0;
    }
    const int in_place = s && s->n;
    const BlPipe* pipe = bl_init(in_place);
    VkDescriptorSet set = vk_desc_alloc(pipe->ds);
    if (!set) return 0;
    const VkDescriptorBufferInfo bl_side = {bl, bl_off, bl_size}, lin_side = {lin, lin_off, lin_size};
    /* read, written, and the runs read in place */
    VkDescriptorBufferInfo sides[3] = {to_bl ? lin_side : bl_side, to_bl ? bl_side : lin_side,
                                       {in_place ? s->tab : VK_NULL_HANDLE, in_place ? s->tab_at : 0,
                                        in_place ? (u64)s->n * sizeof(VkInRun) : 0}};
    snx_vk_storage_buffers(g_vk.dev, set, sides, in_place ? 3 : 2);
    BlParams p = {.runs = runs,
                  .rows = m->rows,
                  .lin_runs = m->lin_pitch >> 4,
                  .y0 = m->y0,
                  .gob_y0 = m->y0 >> 3,
                  .z0 = m->z0,
                  .bh = m->bh,
                  .hmask = (1u << m->bh) - 1,
                  .bd = m->bd,
                  .dmask = (1u << m->bd) - 1,
                  .bsh = 5 + m->bh + m->bd,
                  .block_row = (u32)(block_row_bytes(m) >> 4),
                  .slab = (u32)(slab_bytes(m) >> 4),
                  /* the run at lo is at bl_at: modulo 2^32, as the shader adds it */
                  .bl_bias = (u32)((bl_at - bl_off) >> 4) - (u32)(lo >> 4),
                  .lin_bias = (u32)((lin_at - lin_off) >> 4),
                  .to_bl = (u32)to_bl,
                  .native = (u32)g_gob_native,
                  .tab_bias = (u32)((bl_at - bl_off) >> 4),
                  .nruns = in_place ? s->n : 0};
    for (u32 i = 0; i < 4; i++) {
        u32 tail = m->row_bytes & 15;
        u32 bytes = !tail ? 4 : tail > 4 * i ? tail - 4 * i : 0;
        p.tail_mask[i] = bytes >= 4 ? ~0u : (1u << (bytes * 8)) - 1;
    }
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, pipe->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vk_ck(cb, VK_CK_DESWIZZLE, m->row_bytes, m->rows, m->slices, (u32)to_bl);
    vk_gpuprof_tag_at(VK_PROF_BL_MOVE, __builtin_return_address(0)); /* summed by its caller: many move */
    rec_CmdDispatch(cb, gx, gy, m->slices);
    return 1;
}
int vk_bl_move(VkCommandBuffer cb, const VkBlMove* m, int to_bl, VkBuffer bl, u64 bl_at, VkBuffer lin, u64 lin_at) {
    return bl_move(cb, m, to_bl, bl, bl_at, lin, lin_at, NULL);
}
int vk_bl_move_src(VkCommandBuffer cb, const VkBlMove* m, const VkBlSrc* s, VkBuffer lin, u64 lin_at) {
    return bl_move(cb, m, 0, s->buf, s->at, lin, lin_at, s);
}
