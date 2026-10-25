/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The fields of the GPU's texture and sampler descriptors (TIC and TSC entries, 8 words each) the renderer reads:
   gpu/vk/texture.c makes its textures and samplers from these, and the NVN front end's encoders (gpu/nvn/nvn_tic.c)
   are checked against them (tests/nvn_descriptors.c). The words as host numbers */
#ifndef SNX_TEXTURE_DESCRIPTOR_H
#define SNX_TEXTURE_DESCRIPTOR_H
#include "common/cpu/aarch64/cpu.h"

typedef struct {
    u64 address;
    u32 format;             /* word 0: format, components and swizzle */
    u32 header;             /* 0 buffer, 2 pitch, 3 block linear (4 with a color key) */
    u32 width, height, depth; /* each one more than its field */
    u32 buffer_width;       /* a buffer's texels, the field across words 3 and 4 plus one */
    u32 type;               /* 0 1D, 1 2D, 2 3D, 3 cube, 4 1D array, 5 2D array, 6 buffer, 7 2D no mipmap, 8 cube array */
    u32 srgb, levels;       /* levels: the header's max level plus one */
    u32 block_h, block_d;   /* block height and depth, log2 of the GOBs */
    u32 pitch;              /* a pitch-linear texture's row bytes */
    u32 msaa;               /* the multisample mode, 0 for one sample */
    u32 base_level, max_level; /* the levels the view shows */
} TicFields;

static inline void tic_decode(const u32* tic, TicFields* f) {
    f->address = ((u64)(tic[2] & 0xFFFF) << 32) | tic[1];
    f->format = tic[0];
    f->header = (tic[2] >> 21) & 7;
    f->width = (tic[4] & 0xFFFF) + 1;
    f->height = (tic[5] & 0xFFFF) + 1;
    f->depth = ((tic[5] >> 16) & 0x3FFF) + 1;
    f->buffer_width = (((tic[3] & 0xFFFFu) << 16) | (tic[4] & 0xFFFFu)) + 1;
    f->type = (tic[4] >> 23) & 0xF;
    f->srgb = (tic[4] >> 22) & 1;
    f->levels = ((tic[3] >> 28) & 0xF) + 1;
    f->block_h = (tic[3] >> 3) & 7;
    f->block_d = (tic[3] >> 6) & 7;
    f->pitch = (tic[3] & 0xFFFF) << 5;
    f->msaa = (tic[7] >> 8) & 0xF;
    f->base_level = tic[7] & 0xF;
    f->max_level = (tic[7] >> 4) & 0xF;
}

typedef struct {
    u32 wrap_u, wrap_v, wrap_p; /* 0 repeat, 1 mirrored repeat, 2 clamp to edge, 3 border, 4 clamp, 5-7 mirror once */
    u32 compare, compare_op;    /* compare_op: a VkCompareOp */
    u32 anisotropy;             /* log2-ish: 0 1x ... 7 16x */
    u32 mag, min, mip;          /* filters: 1 nearest, 2 linear; mip 1 none, 2 nearest, 3 linear */
    u32 reduction;              /* 0 average, 1 min, 2 max */
    i32 lod_bias;               /* fixed 5.8, signed */
    u32 min_lod, max_lod;       /* fixed 4.8 */
    u32 border[4];              /* the color's words */
} TscFields;

static inline void tsc_decode(const u32* tsc, TscFields* f) {
    i32 bias = (i32)((tsc[1] >> 12) & 0x1FFF);
    if (bias & 0x1000) bias |= ~0x1FFF;
    f->wrap_u = tsc[0] & 7;
    f->wrap_v = (tsc[0] >> 3) & 7;
    f->wrap_p = (tsc[0] >> 6) & 7;
    f->compare = (tsc[0] >> 9) & 1;
    f->compare_op = (tsc[0] >> 10) & 7;
    f->anisotropy = (tsc[0] >> 20) & 7;
    f->mag = tsc[1] & 3;
    f->min = (tsc[1] >> 4) & 3;
    f->mip = (tsc[1] >> 6) & 3;
    f->reduction = (tsc[1] >> 10) & 3;
    f->lod_bias = bias;
    f->min_lod = tsc[2] & 0xFFF;
    f->max_lod = (tsc[2] >> 12) & 0xFFF;
    f->border[0] = tsc[4];
    f->border[1] = tsc[5];
    f->border[2] = tsc[6];
    f->border[3] = tsc[7];
}

#endif
