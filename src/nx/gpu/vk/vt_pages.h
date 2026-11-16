/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The calculations of the virtual textures (vt_probe.c): which blocks of a texture a sample's mark names, the blocks
   that cover a block in the coarser levels, and which blocks go when the blocks held pass their budget. No state and
   no Vulkan: a test gives them values and checks the blocks. */
#ifndef SNX_VK_VT_PAGES_H
#define SNX_VK_VT_PAGES_H
#include "common/cpu/aarch64/cpu.h"

#define VT_LEVELS 16
/* A texture's blocks, in the order of VkSparseShape (vk.h): layer by layer, in a layer level by level up to the mip
   tail, in a level row by row, left to right. A block is gw x gh texels; the levels from tail_first on are the mip
   tail, which has no blocks: it is held always */
typedef struct {
    u32 w, h, layers, levels;
    u32 gw, gh, tail_first, per_layer;
    u32 first[VT_LEVELS + 1], bx[VT_LEVELS], by[VT_LEVELS];
} VtShape;
/* s from the texture's size and the blocks'. 0 for a size or a block of zero, or more levels than VT_LEVELS */
int vt_shape(VtShape* s, u32 w, u32 h, u32 layers, u32 levels, u32 gw, u32 gh, u32 tail_first);

/* A mark: a sample of a texture's level and layer at cell (u, v), a cell 1/VT_CELLS of the level each way, taken
   from the coordinate's fraction. uo, vo: the coordinate was under 0 (1) or 1 and over (2), where the sampler's
   address mode, which the mark doesn't have, decides the texel: the cell is then the wrapped one, and the mirrored one
   and the edge's count too. resident: the sample found memory under every texel it read */
#define VT_CELLS       1024u
#define VT_REACH       16u /* texels past the cell a filter reads: an anisotropic one's 16 taps along its axis */
#define VT_MARK_BLOCKS 256u
typedef struct {
    u32 level, layer, u, v;
    u8 uo, vo, resident;
} VtMark;
/* The blocks a mark names: `under` the ones its cell lies in (one, or each candidate of a coordinate outside 0 to 1),
   `near` the others within VT_REACH texels of the cell, across the level's edges too (a repeating sampler reads
   there). Each array takes VT_MARK_BLOCKS indices at most, in no order, none twice. Nothing for a level in the mip
   tail or a layer the texture doesn't have */
void vt_mark_blocks(const VtShape* s, const VtMark* m, u32* under, u32* nunder, u32* near, u32* nnear);

/* the block of the next coarser level that holds block b's middle, ~0u when that level is in the mip tail */
u32 vt_block_parent(const VtShape* s, u32 b);

/* Letting blocks go by age. vt_age_count adds to hist the held blocks of one texture by the presents since each was
   seen (seen[b]: the present, 0 never; held: a bit a block), ages of VT_AGES - 1 and more together. vt_age_cut: the
   age from which blocks go so that no more than keep stay, never under floor: blocks seen in the last floor presents
   stay whatever the budget, so what frames are sampling is not taken from under them */
#define VT_AGES 1024u
void vt_age_count(u32 hist[VT_AGES], const u32* seen, const u8* held, u32 n, u32 now);
u32 vt_age_cut(const u32 hist[VT_AGES], u64 keep, u32 floor);
#endif
