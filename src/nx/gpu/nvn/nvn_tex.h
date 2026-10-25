/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/nvn_tex.h, whose algorithm is deko3d's
   (zlib), Copyright (C) 2018-2020 fincs */
/* An NVN texture's storage in the Tegra X1's block-linear layout: its size, alignment and level offsets, as
   nvnTextureBuilderGetStorageSize and its kin report them (gpu/nvn/nvn_tex.c) */
#ifndef SNX_NVN_TEX_H
#define SNX_NVN_TEX_H
#include "nx/gpu/nvn/nvn_obj.h"

typedef struct {
    u64 size, layer_size;      /* the storage's bytes, and a layer's */
    u32 align, layers, bpb;    /* alignment, layer count, bytes per block */
    unsigned tile_h, tile_d;   /* block height and depth (log2 of the GOB count) */
} NvnTexLayout;

void nvn_tex_layout(const NvnTexDesc* d, NvnTexLayout* o);
/* a level's offset from its layer's start */
u64 nvn_tex_level_offset(const NvnTexDesc* d, unsigned level);
/* the block height and depth level `level` is laid out with (log2 of the GOBs): they shrink with its rows and slices */
void nvn_tex_level_tiles(const NvnTexDesc* d, unsigned level, unsigned* tile_h, unsigned* tile_d);
/* bytes per block (0 for a format it doesn't know), and the block's width and height */
int nvn_fmt_bpb(int format, int* bw, int* bh);

#endif
