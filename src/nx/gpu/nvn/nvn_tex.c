/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/nvn_tex.c, whose algorithm is deko3d's
   (zlib, source/dk_image.cpp), Copyright (C) 2018-2020 fincs */
/* NVN texture storage: one GOB is 64 bytes by 8 rows (512 bytes), the block height (in GOBs) is picked from the
   height in blocks times 1.5, and the levels follow each other at block alignment. A packaged texture's 8 layout
   bytes (SetPackagedTextureLayout) give the block height in byte 0 (log2), which then wins */
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/draw_state.h"

typedef struct {
    u8 bpb, bw, bh; /* bytes per block, block width and height (1 for uncompressed formats) */
} Fmt;

static const u8 astc_blocks[14][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},   {6, 6},   {8, 5},   {8, 6},
                                      {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
static const struct {
    u8 lo, hi, bpb;
} fmt_ranges[] = {
    {0x01, 0x04, 1},  {0x05, 0x09, 2},  {0x0A, 0x0C, 4}, {0x0D, 0x10, 2}, {0x11, 0x15, 4}, {0x16, 0x18, 8},
    {0x19, 0x1C, 3},  {0x1D, 0x21, 6},  {0x22, 0x24, 12}, {0x25, 0x28, 4}, {0x29, 0x2D, 8}, {0x2E, 0x30, 16},
    {0x31, 0x31, 1},  {0x32, 0x32, 2},  {0x33, 0x35, 4}, {0x36, 0x36, 8}, {0x37, 0x38, 4}, {0x39, 0x3C, 2},
    {0x3D, 0x40, 4},  {0x51, 0x52, 1},  {0x53, 0x54, 2}, {0x55, 0x56, 4}, {0x57, 0x58, 2}, {0x59, 0x5A, 4},
    {0x5B, 0x5C, 8},  {0x5D, 0x5E, 3},  {0x5F, 0x60, 6}, {0x61, 0x62, 12}, {0x63, 0x64, 4}, {0x65, 0x66, 8},
    {0x67, 0x68, 16}, {0x69, 0x6C, 4},  {0x6D, 0x70, 4}, {0x71, 0x75, 8}, {0x76, 0x78, 16}, {0x95, 0x98, 2},
    {0x99, 0x9C, 4},
};

static Fmt fmt_of(int f) {
    Fmt t = {0, 1, 1};
    unsigned i;
    if (f >= 0x79 && f <= 0x94) { /* ASTC */
        int k = (f - 0x79) % 14;
        t.bpb = 16;
        t.bw = astc_blocks[k][0];
        t.bh = astc_blocks[k][1];
        return t;
    }
    if (f >= 0x41 && f <= 0x50) { /* DXT, RGTC, BPTC: 4x4 blocks */
        int b8 = f <= 0x42 || f == 0x45 || f == 0x46 || f == 0x49 || f == 0x4A;
        t.bpb = b8 ? 8 : 16;
        t.bw = t.bh = 4;
        return t;
    }
    for (i = 0; i < sizeof fmt_ranges / sizeof *fmt_ranges; i++)
        if (f >= fmt_ranges[i].lo && f <= fmt_ranges[i].hi) {
            t.bpb = fmt_ranges[i].bpb;
            return t;
        }
    return t;
}

int nvn_fmt_bpb(int format, int* bw, int* bh) {
    Fmt t = fmt_of(format);
    if (bw) *bw = t.bw;
    if (bh) *bh = t.bh;
    return t.bpb;
}

static u32 mip(u32 s, unsigned l) {
    s >>= l;
    return s ? s : 1;
}
static u32 blk(u32 s, u32 b) { return (s + b - 1) / b; }
static unsigned pick_tile(u32 gobs) { return gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0; }
/* a level's block height or depth shrinks with its own rows (or slices) */
static unsigned adj_tile(unsigned shift, u32 unit, u32 dim) {
    u32 x;
    if (!shift) return 0;
    x = unit << (shift - 1);
    if (x >= dim)
        while (--shift) {
            x >>= 1;
            if (x < dim) break;
        }
    return shift;
}

/* the dimensions per level (dpl), whether it has layers, and its extent: from the target */
static void shape(const NvnTexDesc* d, unsigned* dpl, int* layered, u32* w, u32* h, u32* z) {
    *w = d->width;
    *h = d->height;
    *z = d->depth;
    *layered = 0;
    switch (d->target) {
        case NVN_TEXTURE_TARGET_1D:
            *dpl = 1;
            *h = 1;
            *z = 1;
            break;
        case NVN_TEXTURE_TARGET_1D_ARRAY: /* the layer count is the height */
            *dpl = 1;
            *z = d->height;
            *h = 1;
            *layered = 1;
            break;
        case NVN_TEXTURE_TARGET_3D: *dpl = 3; break;
        case NVN_TEXTURE_TARGET_2D_ARRAY:
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
            *dpl = 2;
            *layered = 1;
            break;
        case NVN_TEXTURE_TARGET_CUBEMAP:
            *dpl = 2;
            *z = 6;
            *layered = 1;
            break;
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: /* the depth counts layer-faces */
            *dpl = 2;
            *layered = 1;
            break;
        default: /* 2D, multisample, rectangle */
            *dpl = 2;
            *z = 1;
            break;
    }
    if (!*z) *z = 1;
}

u32 nvn_tex_msaa(int samples) {
    return samples == 2 ? DS_MSAA_2X1_D3D : samples == 4 ? DS_MSAA_2X2 : samples == 8 ? DS_MSAA_4X2_D3D : DS_MSAA_1X1;
}
static void samples_xy(int s, u32* sx, u32* sy) {
    *sx = *sy = 1;
    if (s == 2)
        *sx = 2;
    else if (s == 4)
        *sx = *sy = 2;
    else if (s == 8) {
        *sx = 4;
        *sy = 2;
    }
}

/* sparse block-linear textures are mapped in 64 KB tiles: a level at least a tile wide, high and deep is laid out with
   its width padded to whole tiles, the levels from the first smaller one on form the mip tail, and the storage is
   whole tiles, 64 KB aligned */
#define SPARSE_TILE 0x10000u
static int sparse_of(const NvnTexDesc* d) {
    return (d->flags & NVN_TEXTURE_FLAGS_SPARSE) && !(d->flags & NVN_TEX_LINEAR) &&
           d->target != NVN_TEXTURE_TARGET_BUFFER;
}
/* the offset of a level from its layer's start */
static u64 level_offset(const NvnTexDesc* d, unsigned level, unsigned dpl, u32 w, u32 h, u32 z, unsigned tile_h,
                        unsigned tile_d, Fmt t) {
    u32 sx, sy, tw = 128u >> (tile_h + tile_d);
    int tiled = sparse_of(d);
    u64 off = 0;
    unsigned i;
    samples_xy(d->samples, &sx, &sy);
    if (!tw) tw = 1; /* GOB columns of a 64 KB tile */
    for (i = 0;; i++) {
        u32 lw = blk(mip(w * sx, i), t.bw), lh = dpl >= 2 ? blk(mip(h * sy, i), t.bh) : 1, ld = dpl >= 3 ? mip(z, i) : 1;
        u32 lwb = lw * t.bpb, wg = (lwb + 63) / 64, hg = (lh + 7) / 8, ht, dt;
        unsigned th = adj_tile(tile_h, 8, lh), td = adj_tile(tile_d, 1, ld);
        tiled = tiled && lwb >= tw * 64 && lh >= 8u << tile_h && ld >= 1u << tile_d;
        if (tiled) wg = (wg + tw - 1) / tw * tw;
        if (i == level) return off;
        ht = (hg + (1u << th) - 1) >> th;
        dt = (ld + (1u << td) - 1) >> td;
        off += (u64)wg * ht * dt << (9 + th + td);
    }
}

void nvn_tex_layout(const NvnTexDesc* d, NvnTexLayout* o) {
    Fmt t = fmt_of(d->format);
    unsigned dpl, tile_h = 0, tile_d = 0, levels;
    int layered, pkg_h = d->bits & NVN_TEX_BLOCK_H;
    u32 w, h, z, sx, sy;
    u64 ls;
    shape(d, &dpl, &layered, &w, &h, &z);
    samples_xy(d->samples, &sx, &sy);
    o->bpb = t.bpb;
    o->tile_h = o->tile_d = 0;
    o->layers = layered ? z : 1;
    if (!t.bpb) {
        o->size = o->layer_size = 0;
        o->align = 512;
        return;
    }
    if (d->target == NVN_TEXTURE_TARGET_BUFFER) {
        o->layer_size = o->size = (u64)t.bpb * d->width;
        o->align = t.bpb == 12 ? 4 : t.bpb;
        return;
    }
    if (d->flags & NVN_TEX_LINEAR) { /* the row pitch: the stride, or aligned to 32 */
        u32 stride;
        o->align = (d->flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET) ? 128 : 32;
        stride = d->stride ? d->stride : (t.bpb * blk(w, t.bw) + o->align - 1) & ~(o->align - 1);
        o->layer_size = o->size = (u64)stride * blk(h, t.bh);
        return;
    }
    /* MINIMAL_LAYOUT: blocks of one GOB, as the game's driver describes such a texture */
    if (dpl == 2)
        tile_h = pkg_h                                         ? (unsigned)pkg_h - 1
                 : d->flags & NVN_TEXTURE_FLAGS_MINIMAL_LAYOUT ? 0
                                                               : pick_tile(blk(h * sy, t.bh) * 3 / 16);
    else if (dpl == 3 && !(d->flags & NVN_TEXTURE_FLAGS_MINIMAL_LAYOUT))
        tile_d = pick_tile(z + z / 2);
    o->tile_h = tile_h;
    o->tile_d = tile_d;
    /* multisampled and rectangle textures have one level, whatever the builder says (the driver's layout) */
    levels = d->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE || d->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY ||
                     d->target == NVN_TEXTURE_TARGET_RECTANGLE || !d->levels
                 ? 1
                 : d->levels;
    ls = level_offset(d, levels, dpl, w, h, z, tile_h, tile_d, t);
    if (sparse_of(d)) ls = (ls + SPARSE_TILE - 1) & ~(u64)(SPARSE_TILE - 1);
    if (layered) { /* a layer is block aligned */
        u32 hh = blk(h, t.bh);
        unsigned th = tile_h, td = tile_d, sh;
        u64 g;
        while (th && hh <= (8u << (th - 1))) th--;
        while (td && z <= (1u << (td - 1))) td--;
        sh = th + td + 9;
        g = ls >> sh;
        if (ls != g << sh) ls = (g + 1) << sh;
    }
    o->layer_size = ls;
    o->size = layered ? ls * z : ls;
    o->align = sparse_of(d) ? SPARSE_TILE : 512;
}

u64 nvn_tex_level_offset(const NvnTexDesc* d, unsigned level) {
    Fmt t = fmt_of(d->format);
    unsigned dpl;
    int layered;
    u32 w, h, z;
    NvnTexLayout l;
    shape(d, &dpl, &layered, &w, &h, &z);
    if (!t.bpb || (d->flags & NVN_TEX_LINEAR) || d->target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    nvn_tex_layout(d, &l);
    return level_offset(d, level, dpl, w, h, z, l.tile_h, l.tile_d, t);
}

void nvn_tex_level_tiles(const NvnTexDesc* d, unsigned level, unsigned* tile_h, unsigned* tile_d) {
    Fmt t = fmt_of(d->format);
    unsigned dpl;
    int layered;
    u32 w, h, z, sx, sy, lh, ld;
    NvnTexLayout l;
    shape(d, &dpl, &layered, &w, &h, &z);
    samples_xy(d->samples, &sx, &sy);
    nvn_tex_layout(d, &l);
    lh = dpl >= 2 && t.bh ? blk(mip(h * sy, level), t.bh) : 1;
    ld = dpl >= 3 ? mip(z, level) : 1;
    *tile_h = adj_tile(l.tile_h, 8, lh);
    *tile_d = adj_tile(l.tile_d, 1, ld);
}

/* A block-linear volume level keeps 2^tile_d slices in each block, one GOB (512 bytes, 64 by 8) per block row and
   slice: slice z of the first block is z * 2^tile_h GOBs on, and the blocks of later slices follow a whole slab of
   the level's blocks, its GOBs across times its block rows times the block's GOBs */
u64 nvn_tex_slice_offset(const NvnTexDesc* d, unsigned level, u32 z) {
    Fmt t = fmt_of(d->format);
    unsigned th, td;
    u64 gobs_x, rows, slab;
    if (!t.bpb) return 0;
    nvn_tex_level_tiles(d, level, &th, &td);
    gobs_x = blk(blk(mip(d->width, level), t.bw) * t.bpb, 64);
    rows = blk(blk(mip(d->height, level), t.bh), 8u << th);
    slab = gobs_x * rows * (512ull << th << td);
    return (u64)(z >> td) * slab + (u64)(z & ((1u << td) - 1)) * (512ull << th);
}

/* The game's NVN driver's storage answers, read from its code (two drivers, NVN 12.3 and 55.15, the same rules):
   - the kind (the GMMU page kind, GetStorageClass's low byte, above 0x100): 0 for a buffer or a linear texture; for a
     block-linear one, a depth kind for a depth or stencil format, a compression kind for a COMPRESSIBLE color format
     of one-texel blocks and 4 bytes or more, else 0xfe. The driver asks the GPU library to choose it from the format's
     kind class and the sample count, and the GPU's kind table to accept it: these are the kinds a Tegra X1 accepts
   - the alignment: 0x80 for LINEAR_RENDER_TARGET, 0x20 for LINEAR (both before the buffer test), a buffer's element
     size, else 512, and the big page (64 KiB) for every kind but 0 and 0xfe
   - the size: the layout's, rounded up to the big page for those kinds too. A format the driver doesn't take gives 0
   - the ZCull area: 0 unless the format has depth bits (or stencil bits with ZCULL_SUPPORT_STENCIL), else 0x20 bytes
     for each 32x32 tile of each layer (the depth field, not times 6 for a cube) and 0xE0 of headers: the ZCULL_GET_INFO
     answer nvdrv gives (region byte multiplier 0x20, alignments 0x20, 0x400 pixels a tile, headers 0x20 and 0xC0) */
#define BIG_PAGE 0x10000u
static u32 kind_of(const NvnTexDesc* d, Fmt t) {
    u32 f = (u32)d->format, comp = (d->flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) != 0, s = d->samples;
    static const struct {
        u32 format;
        u8 plain, comp;
    } depth[] = {{NVN_FORMAT_STENCIL8, 0x2a, 0x2b},       {NVN_FORMAT_DEPTH16, 0x01, 0x08},
                 {NVN_FORMAT_DEPTH24, 0x46, 0x52},        {NVN_FORMAT_DEPTH32F, 0x7b, 0x87},
                 {NVN_FORMAT_DEPTH24_STENCIL8, 0x46, 0x52}, {NVN_FORMAT_DEPTH32F_STENCIL8, 0xc3, 0xcf}};
    size_t i;
    if (d->target == NVN_TEXTURE_TARGET_BUFFER || (d->flags & NVN_TEX_LINEAR)) return 0;
    for (i = 0; i < sizeof depth / sizeof depth[0]; i++)
        if (depth[i].format == f) {
            if (f == NVN_FORMAT_DEPTH32F && comp && s >= 4) return s >= 8 ? 0x89 : 0x88;
            return comp ? depth[i].comp : depth[i].plain;
        }
    if (!comp || t.bw > 1 || t.bpb < 4) return 0xfe;
    /* the compression kinds by element size and samples (1 and 2 samples share one) */
    if (t.bpb >= 16) return s >= 8 ? 0xfb : s >= 4 ? 0xf9 : 0xf7;
    if (t.bpb >= 8) return s >= 8 ? 0xf3 : s >= 4 ? 0xee : 0xcd;
    return s >= 8 ? 0xe5 : s >= 4 ? 0xe0 : 0xcc;
}
void nvn_tex_storage(const NvnTexDesc* d, NvnTexStorage* s) {
    Fmt t = fmt_of(d->format);
    NvnTexLayout l;
    u32 f = (u32)d->format;
    int depth_bits = f >= NVN_FORMAT_DEPTH16 && f <= NVN_FORMAT_DEPTH32F_STENCIL8;
    int stencil_bits = f == NVN_FORMAT_STENCIL8 || f == NVN_FORMAT_DEPTH24_STENCIL8 || f == NVN_FORMAT_DEPTH32F_STENCIL8;
    nvn_tex_layout(d, &l);
    s->kind = kind_of(d, t);
    s->size = l.size;
    s->align = (d->flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET) ? 0x80
               : (d->flags & NVN_TEXTURE_FLAGS_LINEAR)              ? 0x20
               : d->target == NVN_TEXTURE_TARGET_BUFFER             ? (t.bpb == 12 ? 4u : t.bpb)
                                                                    : 0x200;
    if (s->kind && s->kind != 0xfe) {
        s->align = BIG_PAGE;
        s->size = (s->size + BIG_PAGE - 1) & ~(u64)(BIG_PAGE - 1);
    }
    s->zcull = 0;
    if (depth_bits || (stencil_bits && (d->flags & NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL))) {
        u64 w = (d->width + 31u) & ~31u, h = (d->height + 31u) & ~31u, n = (w * h + 0x3FF) / 0x400;
        s->zcull = 0x20 * n * (d->depth ? d->depth : 1) + 0x20 + 0xC0;
    }
}
