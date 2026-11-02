/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/nvn_tex.h, whose algorithm is deko3d's
   (zlib), Copyright (C) 2018-2020 fincs */
/* An NVN texture's storage in the Tegra X1's block-linear layout: its size, alignment and level offsets, as
   nvnTextureBuilderGetStorageSize and its kin report them (gpu/nvn/nvn_tex.c) */
#ifndef SNX_NVN_TEX_H
#define SNX_NVN_TEX_H
#include "nx/gpu/nvn/nvn_obj.h"

/* the flags that make a texture pitch linear: LINEAR, or LINEAR_RENDER_TARGET alone (the game's driver lays such a
   texture out at its stride and describes it as pitch linear) */
#define NVN_TEX_LINEAR (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)

typedef struct {
    u64 size, layer_size;      /* the storage's bytes, and a layer's */
    u32 align, layers, bpb;    /* alignment, layer count, bytes per block */
    unsigned tile_h, tile_d;   /* block height and depth (log2 of the GOB count) */
    unsigned tile_w;           /* a sparse texture's tile, its GOBs across (log2): a level at least a tile wide, high
                                  and deep has rows of whole tiles. 0 for a texture that isn't sparse */
} NvnTexLayout;

void nvn_tex_layout(const NvnTexDesc* d, NvnTexLayout* o);
/* The 64 KiB tile of a sparse texture (SPARSE, block linear; 2D, 3D, an array or a cube; 1, 2, 4, 8 or 16 bytes a
   block), in blocks across, down and deep: the standard shapes the game's driver answers, 2D 256x256, 256x128,
   128x128, 128x64 and 64x64, 3D 64x32x32, 32x32x32, 32x32x16, 32x16x16 and 16x16x16 by those bytes. 0 for a texture
   that has none. nvn_tex_layout lays such a texture out in blocks as high and deep as the tile, so that each 64 KiB
   page holds one tile, the tiles across, then down, then deep, a row of them whole where the texture ends inside its
   last tile (tile_w) */
int nvn_tex_sparse_tile(const NvnTexDesc* d, u32 tile[3]);
/* a level's offset from its layer's start */
u64 nvn_tex_level_offset(const NvnTexDesc* d, unsigned level);
/* the block height and depth level `level` is laid out with (log2 of the GOBs): they shrink with its rows and slices */
void nvn_tex_level_tiles(const NvnTexDesc* d, unsigned level, unsigned* tile_h, unsigned* tile_d);
/* the bytes from a block-linear volume level's start to its depth slice z */
u64 nvn_tex_slice_offset(const NvnTexDesc* d, unsigned level, u32 z);
/* the 3D engine's sample layout (gpu/draw_state.h DS_MSAA_) of NVN's sample count, as the game's driver writes it
   into a texture header: 2 samples 2x1 (D3D), 4 2x2, 8 4x2 (D3D), others one sample */
u32 nvn_tex_msaa(int samples);
/* What nvnTextureBuilderGetStorageSize and its kin answer for a texture, as the game's NVN driver computes them
   (gpu/nvn/nvn_tex.c nvn_tex_storage). The layout (nvn_tex_layout) stays the texture's extent, never past size */
typedef struct {
    u64 size;  /* the layout's size, rounded up to the big page (64 KiB) when the kind is a compression kind */
    u32 align; /* 0x80 linear render target, 0x20 linear, a buffer's element size, 0x200, or 64 KiB as above */
    u32 kind;  /* the GMMU page kind: 0 pitch, 0xfe block linear, else a depth or compression kind */
    u64 zcull; /* the ZCull save area of a depth texture, 0 for any other */
} NvnTexStorage;
void nvn_tex_storage(const NvnTexDesc* d, NvnTexStorage* s);
/* the ZCull save area of a depth (or zcull stencil) texture of d's extent */
u64 nvn_tex_zcull(const NvnTexDesc* d);
/* bytes per block (0 for a format it doesn't know), and the block's width and height */
int nvn_fmt_bpb(int format, int* bw, int* bh);

#endif
