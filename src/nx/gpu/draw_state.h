/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A draw's state as the game asked for it, for any front end (the Maxwell 3D engine's registers today) and read by
   the renderer, which decides what the device does with it: the feature clamps, the resolution scale, empty viewports
   and the render area are its business, not this struct's. Where no guest memory shows the state, the values are
   Vulkan's enumerants as plain integers, so the header needs no Vulkan header (vk/render.c asserts they are equal) */
#ifndef SNX_GPU_DRAW_STATE_H
#define SNX_GPU_DRAW_STATE_H
#include "common/cpu/aarch64/cpu.h"

#define DS_VIEWPORTS_MAX 16

/* VkPolygonMode, VkCullModeFlags, VkCompareOp, VkStencilOp */
enum { DS_POLYGON_FILL, DS_POLYGON_LINE, DS_POLYGON_POINT };
enum { DS_CULL_NONE, DS_CULL_FRONT, DS_CULL_BACK, DS_CULL_FRONT_AND_BACK };
enum {
    DS_CMP_NEVER,
    DS_CMP_LESS,
    DS_CMP_EQUAL,
    DS_CMP_LESS_OR_EQUAL,
    DS_CMP_GREATER,
    DS_CMP_NOT_EQUAL,
    DS_CMP_GREATER_OR_EQUAL,
    DS_CMP_ALWAYS
};
enum {
    DS_STENCIL_KEEP,
    DS_STENCIL_ZERO,
    DS_STENCIL_REPLACE,
    DS_STENCIL_INCREMENT_AND_CLAMP,
    DS_STENCIL_DECREMENT_AND_CLAMP,
    DS_STENCIL_INVERT,
    DS_STENCIL_INCREMENT_AND_WRAP,
    DS_STENCIL_DECREMENT_AND_WRAP
};
/* window_origin's bits */
#define DS_ORIGIN_LOWER_LEFT 0x01 /* y counts up from the bottom of a surface clip_height high */
#define DS_ORIGIN_FLIP_Y     0x10 /* the guest's facing is reversed */

/* The state Vulkan 1.3 sets dynamically, except the viewports */
typedef struct {
    u8 cull;                               /* DS_CULL_ */
    u8 front_cw;                           /* clockwise is front, before the window origin's flip */
    u8 discard;                            /* rasterization is off (it only matters while primitives are captured) */
    u8 bias_on;                            /* depth bias, for the polygon mode it was asked for */
    u8 depth_test, depth_write, depth_cmp; /* with a depth target */
    u8 stencil_test;                       /* with a stencil target */
    u8 alpha_test, alpha_func;             /* DS_CMP_ */
    u8 window_origin;                      /* DS_ORIGIN_ */
    u8 sprite_origin;                      /* a point sprite's T is 0 at its top */
    u8 depth_mode;                         /* 1: depth from 0 to 1, 0: from -1 to 1 */
    u8 point_nudge;                        /* points on a pixel border go to the pixel after it (OpenGL's point
                                              center and half-integer pixel centers, where that rule is known) */
    u8 pad[2];
    struct {
        u8 fail, zfail, pass, cmp; /* DS_STENCIL_, DS_CMP_ */
        u32 ref, cmask, wmask;
    } face[2];        /* front and back: the back is the front when the guest's stencil is one-sided */
    float line_width; /* of the line kind drawn (smooth or aliased) */
    float bias_units, bias_slope, bias_clamp;
    float blend_const[4];
    float alpha_ref;
    u32 clip_height; /* the surface clip's height, which a lower-left origin measures from */
    u32 msaa_mode;   /* the 3D engine's sample layout code */
    u32 depth_word;  /* the vertex stage's viewport and depth word, pushed as it is */
} DsDynamic;

/* The viewports, struct of arrays: only the first count are filled */
typedef struct {
    u32 count;        /* 1, or DS_VIEWPORTS_MAX when the last vertex stage selects one */
    u32 transform_on; /* the guest's viewport transform enable (the renderer picks by the scale, and logs a mismatch) */
    u32 scissor_on;   /* bit j: viewport j's scissor */
    float scale[DS_VIEWPORTS_MAX][3], translate[DS_VIEWPORTS_MAX][3];
    u32 swizzle[DS_VIEWPORTS_MAX]; /* a 3-bit code per component at bits 0, 4, 8, 12 (x, y, z, w): y's 3 negates y */
    u32 clip_x[DS_VIEWPORTS_MAX], clip_y[DS_VIEWPORTS_MAX]; /* start in bits 0-15, extent in 16-31 */
    float z_near[DS_VIEWPORTS_MAX], z_far[DS_VIEWPORTS_MAX];
    u32 scissor_x[DS_VIEWPORTS_MAX], scissor_y[DS_VIEWPORTS_MAX]; /* min in bits 0-15, max in 16-31 */
} DsViewports;

#endif
