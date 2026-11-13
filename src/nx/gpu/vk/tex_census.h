/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The calculations of the texture cache's measurements (tex_report.c): a texture's key as one hash, the table of the
   keys the cache evicted, a texture's place among the guest's live texture objects, and the sums by host format and
   by ASTC block size. No state and no Vulkan: a test gives them values and checks the numbers. */
#ifndef SNX_VK_TEX_CENSUS_H
#define SNX_VK_TEX_CENSUS_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/gpu/vk/range_shape.h"

/* What the cache finds a texture by (texture.c tex_from_tic): its address and layout. No padding, as its bytes are
   hashed: the caller zeroes it first */
typedef struct {
    u64 gva, layer_size;
    u32 w, h, layers, depth, levels, type, bl, bh, bd, tile_w, pitch, msaa, fkey, zero;
} VkTexKey;
/* the key as one value, never 0 */
u64 vk_tex_key_hash(const VkTexKey* k);

/* The keys the cache evicted, as their hashes, in sets of VK_TEX_EVICTED_WAYS: a set keeps its newest. A key that
   went from a full set is forgotten, so the count of textures made again is a lower bound. All zeros is empty */
#define VK_TEX_EVICTED_SETS 4096 /* a power of two */
#define VK_TEX_EVICTED_WAYS 4
typedef struct {
    u64 hash[VK_TEX_EVICTED_SETS][VK_TEX_EVICTED_WAYS]; /* the oldest first, the free ones (0) before them */
} VkTexEvicted;
/* a key evicted: its hash kept, one time */
void vk_tex_evicted_add(VkTexEvicted* e, u64 hash);
/* a texture of that key made: 1 when the table had its hash, which goes from it */
int vk_tex_evicted_take(VkTexEvicted* e, u64 hash);

/* A cached texture among the live texture objects of the guest: at the first address of one, inside one (a view of a
   later layer or level has its own address), or neither, which is a dead texture */
enum { VK_TEX_DEAD, VK_TEX_INSIDE, VK_TEX_LIVE, VK_TEX_CLASSES };
/* starts: the objects' first addresses, sorted. joined: their ranges joined (vk_range_join) */
int vk_tex_live_class(const u64* starts, u32 nstarts, const VkRange* joined, u32 njoined, u64 gva);

/* The bytes and the count of the textures of each host format (a VkFormat's value). The formats past VK_TEX_FORMATS
   of them go in other */
#define VK_TEX_FORMATS 32
typedef struct {
    u32 format, count;
    u64 bytes;
} VkTexFormatRow;
typedef struct {
    VkTexFormatRow row[VK_TEX_FORMATS], other;
    u32 n;
} VkTexFormats;
void vk_tex_formats_add(VkTexFormats* f, u32 format, u64 bytes);
/* the rows with the most bytes first */
void vk_tex_formats_sort(VkTexFormats* f);

/* The bytes of the textures the guest gave as ASTC, by block size (4 to 12 texels a side, bytes[bw - 4][bh - 4]) and
   by what the cache holds them as: BC7, RGBA8, or ASTC where the device samples it */
#define VK_ASTC_SIDES 9
enum { VK_ASTC_AS_BC7, VK_ASTC_AS_RGBA8, VK_ASTC_AS_ASTC, VK_ASTC_AS_N };
typedef struct {
    u64 bytes[VK_ASTC_SIDES][VK_ASTC_SIDES][VK_ASTC_AS_N];
} VkAstcSizes;
/* a texture of bw by bh blocks held as `as`: nothing for a block size ASTC has not */
void vk_astc_sizes_add(VkAstcSizes* a, u32 bw, u32 bh, int as, u64 bytes);
#endif
