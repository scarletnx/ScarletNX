/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* astc_decoder=1: Granite's ASTC compute decoder (assets/shaders/decode/astc.comp, compiled as its archive has it:
   cmake/dependencies.cmake) in astc.comp's place for a strip held as RGBA8, to compare the two (docs/layers/gpu/astc.md,
   "Granite's decoder").

   Granite's shader runs an invocation a texel, a workgroup the footprint's texels times four blocks (local_size_z 4,
   two blocks each way). It reads the blocks from an RGBA32UI image, a texel a block, and writes RGBA8UI texels into a
   storage image without a format (shaderStorageImageWriteWithoutFormat), in its 8 bit decode mode (DECODE_8BIT,
   specialization constant 2). Its tables are uniform texel buffers and, per footprint, an R8UI image of the partition
   each texel of each of the 1024 seeds falls in: made once on the device and shared by every texture, level and
   strip (set 1). The blocks and the texels go through two scratch images of fixed size (set 0, one descriptor set for
   the device's life): a strip is cut into tiles that fit them, each copied from the strip's buffer into the blocks'
   image, decoded, and copied into the texture.

   The tables below (granite_*) are a port of Granite's vulkan/texture/texture_decoder.cpp, under its license:

   Copyright (c) 2017-2026 Hans-Kristian Arntzen

   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
   documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
   Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
   COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/texture/granite_astc_spv.h" /* compiled from Granite's astc.comp by the build */
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* the scratch images: a tile is at most this many texels each way, the blocks' image the most blocks such a tile
   holds (4x4 blocks), 16 MiB of texels as astc.c's scratch buffer and 4 MiB of blocks */
#define GRANITE_TILE_W 4096u
#define GRANITE_TILE_H 1024u
#define GRANITE_LUTS   5u /* the uniform texel buffers of set 1, bindings 0 to 4 */

typedef struct {
    u8 bits, trits, quints;
} GraniteQuant;
/* the endpoints' quantization modes from the most levels to the fewest, and the weights' by their 4 bit index */
static const GraniteQuant granite_endpoint_modes[17] = {
    {8, 0, 0}, {6, 1, 0}, {5, 0, 1}, {7, 0, 0}, {5, 1, 0}, {4, 0, 1}, {6, 0, 0}, {4, 1, 0}, {3, 0, 1},
    {5, 0, 0}, {3, 1, 0}, {2, 0, 1}, {4, 0, 0}, {2, 1, 0}, {1, 0, 1}, {3, 0, 0}, {1, 1, 0}};
static const GraniteQuant granite_weight_modes[16] = {{0, 0, 0}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {2, 0, 0}, {0, 0, 1},
                                                      {1, 1, 0}, {3, 0, 0}, {0, 0, 0}, {0, 0, 0}, {1, 0, 1}, {2, 1, 0},
                                                      {4, 0, 0}, {2, 0, 1}, {3, 1, 0}, {5, 0, 0}};

/* the tables, as the shader reads them: the endpoint quantizer by values and bits left (R16G16B16A16_UINT: bits,
   trits, quints, the offset of its unquantization), the endpoints' unquantization (R8_UINT), the weight quantizer by
   its index (R8G8B8A8_UINT, the same four), the weights' unquantization (R8_UINT), the trit and quint blocks' digits
   (R16_UINT, 3 bits each) */
typedef struct {
    u16 endpoint_quant[9][128][4];
    u8 endpoint_unquant[2048];
    u8 weight_quant[16][4];
    u8 weight_unquant[256];
    u16 trits_quints[256 + 128];
    u32 endpoint_unquant_size, weight_unquant_size;
} GraniteTables;

typedef struct {
    VkDescriptorSetLayout io_ds, lut_ds;
    VkPipelineLayout layout;
    VkShaderModule module;
    VkPipeline pipes[9][9];
    VkDescriptorPool pool;
    VkDescriptorSet io_set, lut_sets[9][9];
    /* the tables' buffer and its views; each footprint's partition image */
    VkBuffer lut;
    DevMem lut_mem;
    VkBufferView lut_views[GRANITE_LUTS];
    VkImage part[9][9];
    DevMem part_mem[9][9];
    VkImageView part_views[9][9];
    /* the scratch images: texels (RGBA8UI, storage) and blocks (RGBA32UI, sampled), both in GENERAL for their life */
    VkImage out, in;
    DevMem out_mem, in_mem;
    VkImageView out_view, in_view;
    int made, refused; /* made: the above exist; refused: the device cannot run it (said once) */
    u64 dispatches, tiles, texels, table_bytes;
} AstcGranite;
static AstcGranite g_astc_granite;

static void granite_unquant_weights(u8* lut, u32 range, GraniteQuant m) {
    for (u32 i = 0; i < range; i++) {
        u32 v;
        if (!m.quints && !m.trits) {
            switch (m.bits) {
                case 1: v = i * 63; break;
                case 2: v = i * 0x15; break;
                case 3: v = i * 9; break;
                case 4: v = (i << 2) | (i >> 2); break;
                case 5: v = (i << 1) | (i >> 4); break;
                default: v = 0; break;
            }
        } else if (m.bits == 0)
            v = m.trits ? 32 * i : 16 * i;
        else {
            u32 b = (i >> 1) & 1, c = (i >> 2) & 1, A = 0x7f * (i & 1), D = i >> m.bits, B = 0, C;
            if (m.trits) {
                static const u32 cs[3] = {50, 23, 11};
                C = cs[m.bits - 1];
                if (m.bits == 2)
                    B = 0x45 * b;
                else if (m.bits == 3)
                    B = 0x21 * b + 0x42 * c;
            } else {
                static const u32 cs[2] = {28, 13};
                C = cs[m.bits - 1];
                if (m.bits == 2) B = 0x42 * b;
            }
            u32 unq = (D * C + B) ^ A;
            v = (A & 0x20) | (unq >> 2);
        }
        if (m.bits != 0 && v > 32) v++; /* [0, 63] to [0, 64] */
        lut[i] = (u8)v;
    }
}

static void granite_unquant_endpoints(u8* lut, u32 range, GraniteQuant m) {
    for (u32 i = 0; i < range; i++) {
        u32 v;
        if (!m.quints && !m.trits) {
            switch (m.bits) {
                case 1: v = i * 0xff; break;
                case 2: v = i * 0x55; break;
                case 3: v = (i << 5) | (i << 2) | (i >> 1); break;
                case 4: v = i * 0x11; break;
                case 5: v = (i << 3) | (i >> 2); break;
                case 6: v = (i << 2) | (i >> 4); break;
                case 7: v = (i << 1) | (i >> 6); break;
                default: v = i; break;
            }
        } else {
            u32 b = (i >> 1) & 1, c = (i >> 2) & 1, d = (i >> 3) & 1, e = (i >> 4) & 1, f = (i >> 5) & 1;
            u32 B = 0, D = i >> m.bits, A = (i & 1) * 0x1ff, C;
            if (m.trits) {
                static const u32 cs[6] = {204, 93, 44, 22, 11, 5};
                C = cs[m.bits - 1];
                switch (m.bits) {
                    case 2: B = b * 0x116; break;
                    case 3: B = b * 0x85 + c * 0x10a; break;
                    case 4: B = b * 0x41 + c * 0x82 + d * 0x104; break;
                    case 5: B = b * 0x20 + c * 0x40 + d * 0x81 + e * 0x102; break;
                    case 6: B = b * 0x10 + c * 0x20 + d * 0x40 + e * 0x80 + f * 0x101; break;
                    default: break;
                }
            } else {
                static const u32 cs[5] = {113, 54, 26, 13, 6};
                C = cs[m.bits - 1];
                switch (m.bits) {
                    case 2: B = b * 0x10c; break;
                    case 3: B = b * 0x82 + c * 0x105; break;
                    case 4: B = b * 0x40 + c * 0x81 + d * 0x102; break;
                    case 5: B = b * 0x20 + c * 0x40 + d * 0x80 + e * 0x101; break;
                    default: break;
                }
            }
            u32 unq = (D * C + B) ^ A;
            v = (A & 0x80) | (unq >> 2);
        }
        lut[i] = (u8)v;
    }
}

static u32 granite_range(GraniteQuant m) {
    u32 r = 1u << m.bits;
    if (m.trits) r *= 3;
    if (m.quints) r *= 5;
    return r == 1 ? 0 : r;
}

static void granite_tables(GraniteTables* t) {
    u32 offsets[17], at = 0;
    memset(t, 0, sizeof *t);
    for (u32 i = 0; i < 17; i++) {
        u32 range = granite_range(granite_endpoint_modes[i]);
        offsets[i] = at;
        granite_unquant_endpoints(t->endpoint_unquant + at, range, granite_endpoint_modes[i]);
        at += range;
    }
    t->endpoint_unquant_size = at;
    /* up to 9 endpoint pairs (18 values): the mode with the most levels whose bits fit in what is left */
    for (u32 pairs = 0; pairs < 9; pairs++)
        for (u32 left = 0; left < 128; left++)
            for (u32 i = 0; i < 17; i++) {
                const GraniteQuant m = granite_endpoint_modes[i];
                u32 values = (pairs + 1) * 2;
                u32 bits = m.bits * values + (m.quints * 7 * values + 2) / 3 + (m.trits * 8 * values + 4) / 5;
                if (bits <= left) {
                    t->endpoint_quant[pairs][left][0] = m.bits;
                    t->endpoint_quant[pairs][left][1] = m.trits;
                    t->endpoint_quant[pairs][left][2] = m.quints;
                    t->endpoint_quant[pairs][left][3] = (u16)offsets[i];
                    break;
                }
            }
    at = 0;
    for (u32 i = 0; i < 16; i++) {
        u32 range = granite_range(granite_weight_modes[i]);
        t->weight_quant[i][0] = granite_weight_modes[i].bits;
        t->weight_quant[i][1] = granite_weight_modes[i].trits;
        t->weight_quant[i][2] = granite_weight_modes[i].quints;
        t->weight_quant[i][3] = (u8)at;
        granite_unquant_weights(t->weight_unquant + at, range, granite_weight_modes[i]);
        at += range;
    }
    t->weight_unquant_size = at;
    /* the specification's trit and quint blocks */
    for (u32 T = 0; T < 256; T++) {
        u32 C, t0, t1, t2, t3, t4;
        if (((T >> 2) & 7) == 7) {
            C = (((T >> 5) & 7) << 2) | (T & 3);
            t4 = t3 = 2;
        } else {
            C = T & 0x1f;
            if (((T >> 5) & 3) == 3) {
                t4 = 2;
                t3 = (T >> 7) & 1;
            } else {
                t4 = (T >> 7) & 1;
                t3 = (T >> 5) & 3;
            }
        }
        if ((C & 3) == 3) {
            t2 = 2;
            t1 = (C >> 4) & 1;
            t0 = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1));
        } else if (((C >> 2) & 3) == 3) {
            t2 = 2;
            t1 = 2;
            t0 = C & 3;
        } else {
            t2 = (C >> 4) & 1;
            t1 = (C >> 2) & 3;
            t0 = (((C >> 1) & 1) << 1) | ((C & 1) & ~((C >> 1) & 1));
        }
        t->trits_quints[T] = (u16)(t0 | t1 << 3 | t2 << 6 | t3 << 9 | t4 << 12);
    }
    for (u32 Q = 0; Q < 128; Q++) {
        u32 C, q0, q1, q2;
        if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
            q2 = ((Q & 1) << 2) | ((((Q >> 4) & 1) & ~(Q & 1)) << 1) | (((Q >> 3) & 1) & ~(Q & 1));
            q1 = q0 = 4;
        } else {
            if (((Q >> 1) & 3) == 3) {
                q2 = 4;
                C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1);
            } else {
                q2 = (Q >> 5) & 3;
                C = Q & 0x1f;
            }
            if ((C & 7) == 5) {
                q1 = 4;
                q0 = (C >> 3) & 3;
            } else {
                q1 = (C >> 3) & 3;
                q0 = C & 7;
            }
        }
        t->trits_quints[256 + Q] = (u16)(q0 | q1 << 3 | q2 << 6);
    }
}

static u32 granite_hash52(u32 p) {
    p ^= p >> 15;
    p -= p << 17;
    p += p << 7;
    p += p << 4;
    p ^= p >> 5;
    p += p << 16;
    p ^= p >> 7;
    p ^= p >> 3;
    p ^= p << 6;
    p ^= p >> 17;
    return p;
}
/* the specification's partition of texel (x, y) */
static int granite_partition(int seed, int x, int y, int count, int small) {
    if (small) {
        x <<= 1;
        y <<= 1;
    }
    seed += (count - 1) * 1024;
    const u32 r = granite_hash52((u32)seed);
    u32 s[12] = {r & 0xF,         (r >> 4) & 0xF,  (r >> 8) & 0xF,  (r >> 12) & 0xF,
                 (r >> 16) & 0xF, (r >> 20) & 0xF, (r >> 24) & 0xF, (r >> 28) & 0xF,
                 (r >> 18) & 0xF, (r >> 22) & 0xF, (r >> 26) & 0xF, ((r >> 30) | (r << 2)) & 0xF};
    for (int i = 0; i < 12; i++) s[i] *= s[i];
    int sh1, sh2;
    if (seed & 1) {
        sh1 = seed & 2 ? 4 : 5;
        sh2 = count == 3 ? 6 : 5;
    } else {
        sh1 = count == 3 ? 6 : 5;
        sh2 = seed & 2 ? 4 : 5;
    }
    const int sh3 = seed & 0x10 ? sh1 : sh2;
    for (int i = 0; i < 8; i++) s[i] >>= i & 1 ? sh2 : sh1;
    for (int i = 8; i < 12; i++) s[i] >>= sh3;
    /* z is 0: 2D blocks */
    u32 a = (s[0] * (u32)x + s[1] * (u32)y + (r >> 14)) & 0x3f, b = (s[2] * (u32)x + s[3] * (u32)y + (r >> 10)) & 0x3f;
    u32 c = (s[4] * (u32)x + s[5] * (u32)y + (r >> 6)) & 0x3f, d = (s[6] * (u32)x + s[7] * (u32)y + (r >> 2)) & 0x3f;
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}
/* the partitions of a footprint's texels for its 1024 seeds: 32x32 seeds of bw x bh texels, each the partition for 2,
   3 and 4 partitions in bits 0, 2 and 4 */
static void granite_partitions(u8* lut, u32 bw, u32 bh) {
    const int small = bw * bh < 31;
    const u32 lw = bw * 32;
    for (u32 sy = 0; sy < 32; sy++)
        for (u32 sx = 0; sx < 32; sx++)
            for (u32 y = 0; y < bh; y++)
                for (u32 x = 0; x < bw; x++) {
                    const int seed = (int)(sy * 32 + sx);
                    lut[(sy * bh + y) * lw + sx * bw + x] =
                        (u8)(granite_partition(seed, (int)x, (int)y, 2, small) |
                             granite_partition(seed, (int)x, (int)y, 3, small) << 2 |
                             granite_partition(seed, (int)x, (int)y, 4, small) << 4);
                }
}

/* the device's limits for a footprint: its workgroup is bw x bh x 4 invocations */
static int granite_fits(u32 bw, u32 bh) {
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    return bw * bh * 4 <= lim->maxComputeWorkGroupInvocations && bw <= lim->maxComputeWorkGroupSize[0] &&
           bh <= lim->maxComputeWorkGroupSize[1] && 4 <= lim->maxComputeWorkGroupSize[2];
}

static void granite_image(VkFormat format, u32 w, u32 h, VkImageUsageFlags usage, VkImage* img, DevMem* mem,
                          VkImageView* view, const char* what) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage};
    VkResult res = VK_SUCCESS;
    if (!vk_image_create_r(&ii, img, mem, VK_OWN_ENCODER, &res))
        fatal("vulkan: cannot create Granite's ASTC %s image of %ux%u (%d)", what, w, h, (int)res);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, view)))
        fatal("vulkan: cannot create the view of Granite's ASTC %s image", what);
}

/* the layouts, the module, the scratch images, the tables' buffer and the pool, recorded into cb the first time */
static void granite_make(VkCommandBuffer cb) {
    AstcGranite* g = &g_astc_granite;
    VkDescriptorSetLayoutBinding io[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                          {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutBinding lut[GRANITE_LUTS + 1];
    for (u32 i = 0; i < GRANITE_LUTS; i++)
        lut[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1,
                                                VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    lut[GRANITE_LUTS] = (VkDescriptorSetLayoutBinding){GRANITE_LUTS, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                       VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = io};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g->io_ds)))
        fatal("vulkan: cannot create Granite's ASTC descriptor set layouts");
    di.bindingCount = GRANITE_LUTS + 1;
    di.pBindings = lut;
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g->lut_ds)))
        fatal("vulkan: cannot create Granite's ASTC descriptor set layouts");
    /* uvec4 error_color, ivec2 resolution */
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkDescriptorSetLayout sets[2] = {g->io_ds, g->lut_ds};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 2,
                                     .pSetLayouts = sets,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g->layout)))
        fatal("vulkan: cannot create Granite's ASTC pipeline layout");
    if (!VK_OK(snx_vk_shader_module(g_vk.dev, granite_astc_spv, granite_astc_spv_size, &g->module)))
        fatal("vulkan: cannot load Granite's ASTC shader");
    /* one set of the scratch images, one of the tables a footprint */
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
                                     {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 + 81},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, GRANITE_LUTS * 81}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 1 + 81,
                                     .poolSizeCount = 3,
                                     .pPoolSizes = sizes};
    if (!VK_OK(vkCreateDescriptorPool(g_vk.dev, &pi, NULL, &g->pool)))
        fatal("vulkan: cannot create Granite's ASTC descriptor pool");
    granite_image(VK_FORMAT_R8G8B8A8_UINT, GRANITE_TILE_W, GRANITE_TILE_H,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &g->out, &g->out_mem, &g->out_view,
                  "texels");
    granite_image(VK_FORMAT_R32G32B32A32_UINT, GRANITE_TILE_W / 4, GRANITE_TILE_H / 4,
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &g->in, &g->in_mem, &g->in_view,
                  "blocks");
    vk_barrier(cb, g->out, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier(cb, g->in, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = g->pool,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &g->io_ds};
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, &g->io_set)))
        fatal("vulkan: cannot allocate Granite's ASTC descriptor sets");
    VkDescriptorImageInfo out = {VK_NULL_HANDLE, g->out_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo in = {VK_NULL_HANDLE, g->in_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = g->io_set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  .pImageInfo = &out},
                                 {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = g->io_set,
                                  .dstBinding = 1,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                  .pImageInfo = &in}};
    vkUpdateDescriptorSets(g_vk.dev, 2, w, 0, NULL);

    /* the tables into one buffer, each at the texel buffers' offset alignment, staged and copied */
    GraniteTables t;
    granite_tables(&t);
    const struct {
        const void* data;
        u32 size;
        VkFormat format;
    } parts[GRANITE_LUTS] = {{t.endpoint_quant, sizeof t.endpoint_quant, VK_FORMAT_R16G16B16A16_UINT},
                             {t.endpoint_unquant, t.endpoint_unquant_size, VK_FORMAT_R8_UINT},
                             {t.weight_quant, sizeof t.weight_quant, VK_FORMAT_R8G8B8A8_UINT},
                             {t.weight_unquant, t.weight_unquant_size, VK_FORMAT_R8_UINT},
                             {t.trits_quints, sizeof t.trits_quints, VK_FORMAT_R16_UINT}};
    u64 align =
        g_vk.props.limits.minTexelBufferOffsetAlignment < 16 ? 16 : g_vk.props.limits.minTexelBufferOffsetAlignment;
    u64 at[GRANITE_LUTS], size = 0;
    for (u32 i = 0; i < GRANITE_LUTS; i++) {
        at[i] = size;
        size = (size + parts[i].size + align - 1) / align * align;
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant want = {.at = DM_DEVICE};
    if (!VK_OK(vk_buffer_create(&bi, &want, &g->lut, &g->lut_mem, NULL)))
        fatal("vulkan: cannot allocate Granite's ASTC tables (%llu bytes)", (unsigned long long)size);
    vk_mem_track(g->lut_mem, VK_OWN_ENCODER);
    VkBuffer staged;
    u64 staged_at;
    u8* p = vk_stage_upload(size, vk_stage_align(), &staged, &staged_at);
    if (!p) fatal("vulkan: no staging for Granite's ASTC tables (%llu bytes)", (unsigned long long)size);
    for (u32 i = 0; i < GRANITE_LUTS; i++) {
        memcpy(p + at[i], parts[i].data, parts[i].size);
        VkBufferViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                                     .buffer = g->lut,
                                     .format = parts[i].format,
                                     .offset = at[i],
                                     .range = parts[i].size};
        if (!VK_OK(vkCreateBufferView(g_vk.dev, &vi, NULL, &g->lut_views[i])))
            fatal("vulkan: cannot create the views of Granite's ASTC tables");
    }
    VkBufferCopy copy = {staged_at, 0, size};
    rec_CmdCopyBuffer(cb, staged, g->lut, 1, &copy);
    VkBufferMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .buffer = g->lut,
                                .size = VK_WHOLE_SIZE};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
    g->table_bytes = size;
    g->made = 1;
    LOG("vulkan: astc_decoder=1 (Granite): ASTC textures held as RGBA8 decode with Granite's shader; its tables take "
        "%llu bytes, each footprint's partitions %u to 147456 more, its scratch images %u MiB\n",
        (unsigned long long)size, 16 * 1024,
        (GRANITE_TILE_W * GRANITE_TILE_H * 4 + GRANITE_TILE_W * GRANITE_TILE_H) >> 20);
}

/* a footprint's pipeline and tables' set: its partition image made, staged and copied into cb the first time */
static VkPipeline granite_footprint(VkCommandBuffer cb, u32 bw, u32 bh) {
    AstcGranite* g = &g_astc_granite;
    VkPipeline* pipe = &g->pipes[bw - 4][bh - 4];
    if (*pipe) return *pipe;
    const u32 lw = bw * 32, lh = bh * 32;
    granite_image(VK_FORMAT_R8_UINT, lw, lh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  &g->part[bw - 4][bh - 4], &g->part_mem[bw - 4][bh - 4], &g->part_views[bw - 4][bh - 4], "partitions");
    VkImage part = g->part[bw - 4][bh - 4];
    VkBuffer staged;
    u64 staged_at;
    u8* p = vk_stage_upload((u64)lw * lh, vk_stage_align(), &staged, &staged_at);
    if (!p) fatal("vulkan: no staging for Granite's ASTC %ux%u partitions (%u bytes)", bw, bh, lw * lh);
    granite_partitions(p, bw, bh);
    g->table_bytes += (u64)lw * lh;
    vk_barrier(cb, part, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy = {.bufferOffset = staged_at,
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {lw, lh, 1}};
    rec_CmdCopyBufferToImage(cb, staged, part, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_barrier(cb, part, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorSet* set = &g->lut_sets[bw - 4][bh - 4];
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = g->pool,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &g->lut_ds};
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, set)))
        fatal("vulkan: cannot allocate Granite's ASTC %ux%u tables' set", bw, bh);
    VkDescriptorImageInfo pi = {VK_NULL_HANDLE, g->part_views[bw - 4][bh - 4],
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[GRANITE_LUTS + 1];
    for (u32 i = 0; i < GRANITE_LUTS; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = *set,
                                      .dstBinding = i,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                      .pTexelBufferView = &g->lut_views[i]};
    w[GRANITE_LUTS] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = *set,
                                             .dstBinding = GRANITE_LUTS,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                             .pImageInfo = &pi};
    vkUpdateDescriptorSets(g_vk.dev, GRANITE_LUTS + 1, w, 0, NULL);
    /* the workgroup's x and y (the footprint) and the 8 bit decode */
    const u32 spec_data[3] = {bw, bh, 1};
    const VkSpecializationMapEntry entries[3] = {
        {0, 0, sizeof(u32)}, {1, sizeof(u32), sizeof(u32)}, {2, 2 * sizeof(u32), sizeof(u32)}};
    VkSpecializationInfo spec = {
        .mapEntryCount = 3, .pMapEntries = entries, .dataSize = sizeof spec_data, .pData = spec_data};
    u64 started = plat_ns();
    if (!VK_OK(snx_vk_module_pipeline(g_vk.dev, vk_pipeline_cache(), g->layout, g->module, &spec, pipe)))
        fatal("vulkan: cannot create Granite's ASTC %ux%u compute pipeline", bw, bh);
    vk_texture_pipeline_made(plat_ns() - started);
    return *pipe;
}

int vk_astc_granite_takes(u32 bw, u32 bh, u64 in_off) {
    AstcGranite* g = &g_astc_granite;
    if (g_settings.astc_decoder != 1 || g->refused) return 0;
    if (!g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        g->refused = 1;
        WARN("vulkan: astc_decoder=1 (Granite) needs shaderStorageImageWriteWithoutFormat, which this device lacks: "
             "ASTC textures decode with astc_decoder=0 (ScarletNX's astc.comp)\n");
        return 0;
    }
    if (!granite_fits(bw, bh)) {
        LOG_LIMIT(4,
                  "vulkan: astc_decoder=1 (Granite): a %ux%u level's workgroup of %u invocations is past this device's "
                  "%u (maxComputeWorkGroupInvocations): it decodes with astc_decoder=0 (ScarletNX's astc.comp)\n",
                  bw, bh, bw * bh * 4, g_vk.props.limits.maxComputeWorkGroupInvocations);
        return 0;
    }
    if (in_off % 16) {
        LOG_LIMIT(4,
                  "vulkan: astc_decoder=1 (Granite): a strip's blocks start at byte %llu of their buffer, not on a "
                  "16 byte block, which a copy into the blocks' image needs: it decodes with astc_decoder=0 "
                  "(ScarletNX's astc.comp)\n",
                  (unsigned long long)in_off);
        return 0;
    }
    return 1;
}

void vk_astc_granite_strip(VkCommandBuffer cb, VkBuffer in, u64 in_off, u32 nx, u32 n, u32 nz, u32 w, u32 height,
                           u32 bw, u32 bh, VkImage dst, u32 level, u32 x, u32 y, u32 layer, int layered, u32 z) {
    AstcGranite* g = &g_astc_granite;
    if (!g->made) granite_make(cb);
    VkPipeline pipe = granite_footprint(cb, bw, bh);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    const VkDescriptorSet sets[2] = {g->io_set, g->lut_sets[bw - 4][bh - 4]};
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->layout, 0, 2, sets, 0, NULL);
    /* a tile: whole blocks, as many as the scratch images take */
    const u32 tbx = GRANITE_TILE_W / bw, tby = GRANITE_TILE_H / bh;
    const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (u32 k = 0; k < nz; k++)
        for (u32 by = 0; by < n; by += tby)
            for (u32 bx = 0; bx < nx; bx += tbx) {
                const u32 cx = nx - bx < tbx ? nx - bx : tbx, cy = n - by < tby ? n - by : tby;
                const u32 tw = w - bx * bw < cx * bw ? w - bx * bw : cx * bw;
                const u32 th = height - by * bh < cy * bh ? height - by * bh : cy * bh;
                /* the blocks' image: written once the last tile's dispatch has read it */
                VkImageMemoryBarrier2 b[2] = {{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                               .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                               .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                               .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                               .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                               .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                               .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                               .image = g->in,
                                               .subresourceRange = whole}};
                VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = b};
                rec_CmdPipelineBarrier2(cb, &d);
                /* rows of nx blocks, a block a texel of the RGBA32UI image */
                VkBufferImageCopy blocks = {.bufferOffset = in_off + (((u64)k * n + by) * nx + bx) * 16,
                                            .bufferRowLength = nx,
                                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                            .imageExtent = {cx, cy, 1}};
                rec_CmdCopyBufferToImage(cb, in, g->in, VK_IMAGE_LAYOUT_GENERAL, 1, &blocks);
                /* the blocks for the shader; the texels' image written once the last tile's copy has read it */
                b[0].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                b[0].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                b[0].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                b[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                b[1] = b[0];
                b[1].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                b[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                b[1].image = g->out;
                d.imageMemoryBarrierCount = 2;
                rec_CmdPipelineBarrier2(cb, &d);
                /* opaque magenta, the LDR profile's error colour, as astc.comp has it; the tile's texels */
                const u32 push[6] = {255, 0, 255, 255, tw, th};
                rec_CmdPushConstants(cb, g->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
                vk_astc_study_mark(cb, 0);
                vk_gpuprof_tag(VK_PROF_GRANITE_ASTC);
                rec_CmdDispatch(cb, (cx + 1) / 2, (cy + 1) / 2, 1);
                vk_astc_study_mark(cb, 1);
                b[1].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                b[1].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                b[1].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                b[1].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                d.imageMemoryBarrierCount = 1;
                d.pImageMemoryBarriers = &b[1];
                rec_CmdPipelineBarrier2(cb, &d);
                /* the texels into the texture: the same bytes, RGBA8UI into its UNORM or SRGB format */
                VkImageCopy texels = {
                    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layered ? layer + k : layer, 1},
                    .dstOffset = {(i32)(x + bx * bw), (i32)(y + by * bh), layered ? 0 : (i32)(z + k)},
                    .extent = {tw, th, 1}};
                rec_CmdCopyImage(cb, g->out, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &texels);
                g->tiles++;
                g->dispatches++;
                g->texels += (u64)tw * th;
            }
}

void vk_astc_granite_stats(u64* dispatches, u64* texels, u64* table_bytes) {
    *dispatches = g_astc_granite.dispatches;
    *texels = g_astc_granite.texels;
    *table_bytes = g_astc_granite.table_bytes;
}
