/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/host1x/vic.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project */
/* The VIC composition on the CPU, byte by byte as vic.comp does it: the reference the tests hold the shader to
   (tests/glsl/vic_jobs.c, tests/vic_compose.c). The runtime composes every job on the GPU (docs/layers/gpu/ownership.md),
   so this file isn't built into it.

   It takes the VicJob vic.c resolved. Each output pixel starts as the background, every enabled slot covering it
   replaces it in slot order (Eden's Vic::Blend without blending), and the 10 bit result is stored as 8 bit: an RGBA
   word, or for NV12 and NV21 a luma byte and, on even pixels of even rows, the pair's two chroma bytes. Planes are
   pitch linear or block linear, addressed through gpu.c's bl_offset */
#include <string.h>
#include "vic_ref.h"

/* the 10 bit pixels VIC composes in: an 8 bit sample << 2, and >> 2 back to the output's 8 bit */
#define SAMPLE_TO_PIXEL_SHIFT 2u
/* the colour matrix's result is 8 fractional bits above the pixel (Eden's Vic::Blend) */
#define MATRIX_FRAC_BITS 8

/* byte x of row y in plane p */
static u64 plane_offset(const VicPlane* p, u32 x, u32 y) {
    return p->blk ? bl_offset(x, y, 0, p->pitch, p->h, p->bh, 0) : (u64)y * p->pitch + x;
}
/* bytes past the mapping read as 0 and take no writes */
static u32 plane_read(const VicPlane* p, u32 x, u32 y) {
    u64 o = plane_offset(p, x, y);
    return o < p->lim ? p->p[o] : 0;
}
static void plane_write(const VicPlane* p, u64 o, u32 pixel) {
    if (o < p->lim) p->p[o] = (u8)(pixel >> SAMPLE_TO_PIXEL_SHIFT);
}

/* slot sl's pixel at output (x, y) into px (R, G, B, A, 10 bit): its Y and the pair's U and V, through the colour
   matrix and the soft clamp when the matrix is enabled, and the planar alpha */
static void slot_pixel(const VicSlot* sl, u32 x, u32 y, u32 px[4]) {
    u32 sx = sl->sx0 + (x - sl->dx0), sy = sl->sy0 + (y - sl->dy0), pair = sx & ~1u, c;
    int q[3];
    q[0] = (int)(plane_read(&sl->iy, sx, sy) << SAMPLE_TO_PIXEL_SHIFT);
    q[1] = (int)(plane_read(&sl->ic, pair + sl->vu, sy >> 1) << SAMPLE_TO_PIXEL_SHIFT);
    q[2] = (int)(plane_read(&sl->ic, pair + 1 - sl->vu, sy >> 1) << SAMPLE_TO_PIXEL_SHIFT);
    for (c = 0; c < 3; c++) {
        int v = q[c];
        if (sl->mat_enable) {
            v = q[0] * sl->mat[c][0] + q[1] * sl->mat[c][1] + q[2] * sl->mat[c][2];
            v = ((v >> sl->mat_shift) + sl->mat[c][3]) >> MATRIX_FRAC_BITS;
            v = v < (int)sl->clamp_lo ? (int)sl->clamp_lo : v > (int)sl->clamp_hi ? (int)sl->clamp_hi : v;
        }
        px[c] = (u32)v;
    }
    px[3] = sl->alpha;
}
void vic_compose_ref(const VicJob* j) {
    u32 x, y, s, k, yuv = j->fmt == VIC_PIXEL_Y8__V8U8_N420 || j->fmt == VIC_PIXEL_Y8__U8V8_N420;
    u32 vu = j->fmt == VIC_PIXEL_Y8__U8V8_N420; /* NV21: V in each pair's even byte */
    for (y = 0; y < j->sh; y++)
        for (x = 0; x < j->sw; x++) {
            u32 px[4];
            int covered = x >= j->tx0 && x - j->tx0 < j->tw && y >= j->ty0 && y - j->ty0 < j->th;
            memcpy(px, j->bg, sizeof px);
            for (s = 0; s < VIC_SLOTS; s++) {
                const VicSlot* sl = &j->slots[s];
                if (!sl->on || x < sl->dx0 || x - sl->dx0 >= sl->w || y < sl->dy0 || y - sl->dy0 >= sl->h) continue;
                slot_pixel(sl, x, y, px);
                covered = 1;
            }
            if (!covered) continue;
            if (!yuv) {
                u64 o = plane_offset(&j->oy, 4 * x, y);
                u32 rb = j->fmt == VIC_PIXEL_A8R8G8B8 ? 2 : 0; /* A8R8G8B8 stores R and B the other way around */
                plane_write(&j->oy, o + rb, px[0]);
                plane_write(&j->oy, o + 1, px[1]);
                plane_write(&j->oy, o + 2 - rb, px[2]);
                plane_write(&j->oy, o + 3, px[3]);
                continue;
            }
            plane_write(&j->oy, plane_offset(&j->oy, x, y), px[0]);
            /* a pair's chroma comes from its even pixel on the even row */
            if (!(x & 1) && !(y & 1) && j->oc.p) {
                u64 o = plane_offset(&j->oc, x, y >> 1);
                for (k = 0; k < 2; k++) plane_write(&j->oc, o + (k ^ vu), px[1 + k]);
            }
        }
}
