/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Named after Eden (src/video_core/engines/sw_blitter/blitter.cpp, its SoftwareBlitEngine and NearestNeighbor):
   SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project */
/* The CPU reference of a 2D engine blit from memory, as Eden's software blitter does it: done in guest memory, pixel
   by pixel, nearest neighbor, from the source columns and rows the engine worked out (maxwell/fermi2d.c). The tests
   hold the renderer's blit_mem to it (tests/fermi2d_blit.c, tests/vulkan_headless.c). The runtime blits on the GPU
   (vk/blit.c, docs/GPU_OWNERSHIP.md), so this file isn't built into it. Between float formats of different sizes each
   component is converted (half.c), as the software blitter does */
#include "fermi2d_ref.h"
#include <string.h>
#include "common/utilities/endian.h"
#include "half.h"

/* the largest pixel a blit copies, in bytes (a 128-bit format) */
#define BLIT_MAX_BPP 16u

/* the byte offset of pixel (x, y) in a plane: through the GOBs when block linear, else rows pitch apart */
static u64 plane_pixel_offset(const GpuDmaSide* s, u32 bpp, u32 x, u32 y) {
    return s->bl ? bl_offset(x * bpp, y, s->z, s->wb, s->h, s->bh, s->bd) : (u64)y * s->pitch + (u64)x * bpp;
}
/* a pixel between float formats (gpu.h's GPU_BLIT_FLOAT): each component as a float, 0 where the source has none and
   its alpha 1, rounded to the nearest half (ties to even) when the destination's are halves */
static void convert_pixel(u32 fmt, const u8* in, u8* out) {
    u32 sn = fmt & 15, sb = fmt >> 4 & 15, dn = fmt >> 8 & 15, db = fmt >> 12 & 15, k;
    float v[4] = {0, 0, 0, 1};
    for (k = 0; k < sn; k++) {
        if (sb == 2) {
            v[k] = half_to_float(snx_ld_le16(in + 2 * k));
        } else {
            u32 w = snx_ld_le32(in + 4 * k);
            memcpy(&v[k], &w, 4);
        }
    }
    if (fmt >> 16 & 1) v[3] = 1;
    for (k = 0; k < dn; k++) {
        if (db == 2) {
            snx_st_le16(out + 2 * k, float_to_half(v[k]));
        } else {
            u32 w;
            memcpy(&w, &v[k], 4);
            snx_st_le32(out + 4 * k, w);
        }
    }
}
/* each destination pixel of the blit's rectangle from the source pixel its column and row tables name */
void blit_mem_ref(const GpuBlit* b) {
    u32 x, y, bd = GPU_BLIT_DST_BPP(b);
    u8 px[BLIT_MAX_BPP], out[BLIT_MAX_BPP];
    if (b->bpp > BLIT_MAX_BPP || bd > BLIT_MAX_BPP) return;
    for (y = b->y0; y < b->y1; y++)
        for (x = b->x0; x < b->x1; x++) {
            u64 from = plane_pixel_offset(&b->src, b->bpp, b->scol[x - b->x0], b->srow[y - b->y0]);
            gmem_read_raw(b->src.addr + from, px, b->bpp);
            if (b->fmt) convert_pixel(b->fmt, px, out);
            gmem_write_raw(b->dst.addr + plane_pixel_offset(&b->dst, bd, x, y), b->fmt ? out : px, bd);
        }
}
