/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* DRAW_TEXTURE's rectangles as vkCmdBlitImage takes them. The console rasterizes the destination rectangle, so pixels
   past the target are not written, and samples the texture along it; a blit may neither read nor write past either
   image (VUID-vkCmdBlitImage-srcOffset-00243, dstOffset-00248), so both rectangles are cut together, at the same
   point of the draw, and keep their scale. No Vulkan here: tests/vk_draw_texture_blit.c calls it */
#ifndef SNX_VK_DRAW_TEXTURE_H
#define SNX_VK_DRAW_TEXTURE_H
#include "nx/gpu/draw_state.h"

/* an image a texture draw reads or writes */
typedef struct {
    double scale_x, scale_y; /* its pixels per guest pixel or texel: the resolution scale of a target or of a texture
                                made from one */
    u32 w, h;                /* its size in its pixels */
} DrawTextureImage;

/* the blit's corners, x0 y0 x1 y1, in each image's pixels; a first corner past the second flips that axis */
typedef struct {
    i32 src[4], dst[4];
    /* destination pixels whose texels lie outside the texture are not drawn: the console samples them by the
       sampler's wrap mode, which a blit has none of */
    int cut;
} DrawTextureBlit;

/* the blit that draws t from the texture src onto the target dst, t's y counted up from the bottom of a surface
   clip_height high when lower_left. 0 when it writes no pixel: out->cut then says whether the texture's edges were
   what left none */
int vk_draw_texture_blit(const DrawTexture* t, int lower_left, u32 clip_height, const DrawTextureImage* src,
                         const DrawTextureImage* dst, DrawTextureBlit* out);

#endif
