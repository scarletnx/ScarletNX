/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's registers as the neutral draw state. Register numbers are method / 4 (NVIDIA open-gpu-doc
   classes/3d/clb197.h). Compare functions and stencil operations come in two encodings, the hardware's from 1 and
   OpenGL's enumerants, both in Vulkan's order */
#include "nx/gpu/maxwell/draw_state.h"

static u32 cmp_op(u32 v) {
    if (v >= 0x200 && v <= 0x207) return v - 0x200;
    if (v >= 1 && v <= 8) return v - 1;
    return DS_CMP_ALWAYS;
}
static u32 stencil_op(u32 v) {
    switch (v) {
        case 1:
        case 0x1E00: return DS_STENCIL_KEEP;
        case 3:
        case 0x1E01: return DS_STENCIL_REPLACE;
        case 4:
        case 0x1E02: return DS_STENCIL_INCREMENT_AND_CLAMP;
        case 5:
        case 0x1E03: return DS_STENCIL_DECREMENT_AND_CLAMP;
        case 6:
        case 0x150A: return DS_STENCIL_INVERT;
        case 7:
        case 0x8507: return DS_STENCIL_INCREMENT_AND_WRAP;
        case 8:
        case 0x8508: return DS_STENCIL_DECREMENT_AND_WRAP;
    }
    return DS_STENCIL_ZERO;
}

void mx_draw_dynamic(const u32* r, u32 polygon, u32 viewports, DsDynamic* d, DsViewports* v) {
    memset(d, 0, sizeof *d);
    d->cull = (u8)(!r[0x646]           ? DS_CULL_NONE
                   : r[0x648] == 0x404 ? DS_CULL_FRONT
                   : r[0x648] == 0x408 ? DS_CULL_FRONT_AND_BACK
                                       : DS_CULL_BACK);
    d->front_cw = r[0x647] == 0x900;
    d->discard = !r[0xDF]; /* RASTERIZE_ENABLE */
    /* enabled per polygon mode: 0x370 points, 0x371 lines, 0x372 filled */
    d->bias_on = r[polygon == DS_POLYGON_POINT ? 0x370 : polygon == DS_POLYGON_LINE ? 0x371 : 0x372] != 0;
    d->depth_test = r[0x4B3] != 0;
    d->depth_write = r[0x4BA] != 0;
    d->depth_cmp = (u8)cmp_op(r[0x4C3]);
    d->stencil_test = r[0x4E0] != 0;
    d->alpha_test = r[0x4BB] & 1;
    d->alpha_func = (u8)cmp_op(r[0x4C5]);
    d->window_origin = (u8)(r[0x4EB] & (DS_ORIGIN_LOWER_LEFT | DS_ORIGIN_FLIP_Y));
    d->sprite_origin = (r[0x581] >> 2) & 1;
    d->depth_mode = r[0x35F] & 1;
    d->point_nudge = !r[0x597] && !r[0x649]; /* the OpenGL point center (0x597) and half-integer pixel centers (0x649) */
    /* GL order in the registers: fail, depth fail, pass; then the function, reference, function mask and write mask
       (the back's 0x3D5 reference, 0x3D6 write mask, 0x3D7 function mask) */
    int two = r[0x565] != 0;
    d->face[0].fail = (u8)stencil_op(r[0x4E1]);
    d->face[0].zfail = (u8)stencil_op(r[0x4E2]);
    d->face[0].pass = (u8)stencil_op(r[0x4E3]);
    d->face[0].cmp = (u8)cmp_op(r[0x4E4]);
    d->face[0].ref = r[0x4E5];
    d->face[0].cmask = r[0x4E6];
    d->face[0].wmask = r[0x4E7];
    d->face[1].fail = (u8)stencil_op(r[two ? 0x566 : 0x4E1]);
    d->face[1].zfail = (u8)stencil_op(r[two ? 0x567 : 0x4E2]);
    d->face[1].pass = (u8)stencil_op(r[two ? 0x568 : 0x4E3]);
    d->face[1].cmp = (u8)cmp_op(r[two ? 0x569 : 0x4E4]);
    d->face[1].ref = r[two ? 0x3D5 : 0x4E5];
    d->face[1].cmask = r[two ? 0x3D7 : 0x4E6];
    d->face[1].wmask = r[two ? 0x3D6 : 0x4E7];
    memcpy(&d->line_width, &r[r[0x55C] ? 0x4EC : 0x4ED], 4); /* smooth lines (0x55C) have a width of their own */
    memcpy(&d->bias_units, &r[0x56F], 4);
    memcpy(&d->bias_slope, &r[0x55B], 4);
    memcpy(&d->bias_clamp, &r[0x61F], 4);
    memcpy(d->blend_const, &r[0x4C7], 16);
    memcpy(&d->alpha_ref, &r[0x4C4], 4);
    d->clip_height = r[0x3FE] >> 16;
    d->msaa_mode = r[0x574];
    d->depth_word = r[0x50E];

    /* SET_VIEWPORT_SCALE_X(j) at register 0x280 + 8 j (scale x, y, z, offset x, y, z, then the coordinate swizzle),
       SET_VIEWPORT_CLIP_HORIZONTAL(j) at 0x300 + 4 j (then vertical, min z, max z) and SET_SCISSOR_ENABLE(j) at
       0x380 + 4 j (then horizontal, vertical) */
    v->count = viewports < DS_VIEWPORTS_MAX ? viewports : DS_VIEWPORTS_MAX; /* the rest are left as they were */
    v->transform_on = r[0x64B] & 1;
    v->scissor_on = 0;
    for (u32 j = 0; j < v->count; j++) {
        memcpy(v->scale[j], &r[0x280 + j * 8], 12);
        memcpy(v->translate[j], &r[0x283 + j * 8], 12);
        v->swizzle[j] = r[0x286 + j * 8];
        v->clip_x[j] = r[0x300 + j * 4];
        v->clip_y[j] = r[0x301 + j * 4];
        memcpy(&v->z_near[j], &r[0x302 + j * 4], 4);
        memcpy(&v->z_far[j], &r[0x303 + j * 4], 4);
        v->scissor_on |= (r[0x380 + j * 4] & 1) << j;
        v->scissor_x[j] = r[0x381 + j * 4];
        v->scissor_y[j] = r[0x382 + j * 4];
    }
}
