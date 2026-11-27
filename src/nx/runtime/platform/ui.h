/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the renderer draws over the game's picture: the on-screen controls (vpad.c) and the game's menu (overlay.c),
   as quads over the frame: shapes, letters, icons and fills drawn from their own bits and colours, and the user's
   picture and a save state's thumbnail from their images */
#ifndef SNX_UI_H
#define SNX_UI_H
#include "common/utilities/ui_quads.h"
#include "common/utilities/ui_ring.h"


/* the picture of the user playing (common/utilities/users.h), side pixels square at x, y: drawn SNX_UI_PICTURE texels
   square in the YoRHa theme's [head] picture shape, square or round (the rest transparent); 0 when the theme shows
   none, which draws nothing */
int ui_picture(SnxUiList* l, float x, float y, float side);
/* ui_image draws the picture apart from the atlas (ui_quads.h's SNX_UI_IMAGE_W x SNX_UI_IMAGE_H, the menu's save state
   thumbnail) at x, y, w x h: 0 while there is none, which draws nothing */
int ui_image(SnxUiList* l, float x, float y, float w, float h);
/* the same, of the picture's texels u, v, uw x vh alone */
int ui_image_part(SnxUiList* l, float x, float y, float w, float h, int u, int v, int uw, int vh);
/* the picture's texels, R G B A bytes, opaque, copied (NULL: none), under ui_lock */
void ui_image_set(const u8* rgba);
/* the picture for the renderer, under ui_lock: its texels, NULL for none, and version counting the pictures given */
const u8* ui_image_get(u32* version);

/* The atlas, each texel the bytes R G B A (a little-endian u32) with alpha premultiplied, made when the window opens,
   the users read by then, and freed by ui_atlas_done once the renderer has it, both under ui_lock: the 8x16 font's 256
   letters (16 a row from SNX_UI_FONT_X, SNX_UI_FONT_Y), a white block for fills (SNX_UI_WHITE_X, SNX_UI_WHITE_Y, 4 x 4),
   and from row SNX_UI_PICTURE_Y the picture of the user playing and the icons. The GPU draws the letters, the icons and
   the fills from their bits and colours (ui.frag); the SDL renderer's fatal panel and the tests read the texels.
   version counts the atlases made, 0 for none yet */
#define SNX_UI_ATLAS_W   1024
void ui_atlas_build(void);
/* ui_atlas_build in two: the atlas made (NULL without memory), its rows into *rows; then put in under ui_lock, which the
   caller holds (with what is drawn from it changing at once) */
u32* ui_atlas_make(int* rows);
void ui_atlas_put(u32* atlas, int rows);
const u32* ui_atlas(int* w, int* h, u32* version);
void ui_atlas_done(void);

#endif
