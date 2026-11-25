/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the renderer draws over the game's picture: the on-screen controls (vpad.c) and the game's menu (overlay.c),
   as quads over the frame: shapes, letters, icons and fills drawn from their own bits and colours, and the user's
   picture and a save state's thumbnail from their images */
#ifndef SNX_UI_H
#define SNX_UI_H
#include "common/utilities/ui_quads.h"

/* the ground of every backed letter and icon, R G B A from the low byte: one for all that is drawn over the game (the
   stats, the debug line, the notification), the layer's ink (ui_colors.h) as a quad holds it (ui_rgba), so a fill of
   the ink beside a backed letter is the same pixel; whoever draws and whoever doesn't */
u32 ui_ground(void);
/* The launcher's ring that turns while a screen waits (src/gui/main.c's ring), centred at cx, cy for letters at scale
   (s): a thin outer circle 7 s pixels in radius in dim, its edge 0.5 s, one 0.62 of it in soft whose edge is 1.5 s,
   each rounded up to whole pixels, and on it a bar of 70 degrees in ink going round a turn every 1.44 s of now_ms,
   from 3 o'clock clockwise; at 0 degrees when still (the wait over). Shapes (UI_SHAPE_CIRCLE), the bar's a row of
   filled ones */
#define UI_RING_BAR_DOTS 8
void ui_loading_ring(SnxUiList* l, float cx, float cy, int scale, u64 now_ms, int still, u32 dim, u32 soft, u32 ink);
/* The ring of bars, one at most (stats.c's graph): n slots in a row that scrolls left, the newest's at the right, each
   a byte, the bar's height in 255ths of the graph's. A UI_RING quad is a slot's bar, laid out once over the whole
   graph (x, y, w, h), glyph its slot: the bar is w / n wide. A new bar writes its byte and moves the head, and the
   quads stay as they are: the renderer sends the byte and the GPU draws the bar (ui.vert). Under ui_lock */
#define SNX_UI_RING_MAX 256
typedef struct {
    u8 height[SNX_UI_RING_MAX];
    u16 head, n;  /* the newest's slot and the slots; n 0 for no ring */
    u8 slow_from; /* a height past it draws in the slow colour (a frame past 30 FPS's) */
    u32 slow;     /* that colour, R G B A from the low byte */
    u32 version;  /* the ring's maker's count of the changes to its bytes: the same, they are not copied again */
} SnxUiRing;
void ui_ring_set(const u8* height, int n, int head, int slow_from, u32 slow, u32 version);
/* the ring's n bars' quads over the graph at x, y, w x h, a slot each from 0, in rgba below slow_from */
void ui_ring_bars(SnxUiList* l, float x, float y, float w, float h, int n, u32 rgba);
const SnxUiRing* ui_ring(void);
/* q as it is drawn (ui_view's): a UI_RING quad as its slot's bar, w / n wide, a bar's width left of the graph's right
   edge for each slot newer than it, its height's share of h up from the graph's foot, in the slow colour past
   slow_from, and nothing (uw 0) at height 0; any other as it is */
SnxUiQuadView ui_drawn(const SnxUiQuad* q);
/* for a backed letter or icon (UI_BACKED), the fill of its ground under it, as one draws it before the letter where it
   can't draw both at once (the SDL renderer, the tests' CPU blend): 1 with it in *fill; 0 for any other quad */
int ui_drawn_ground(const SnxUiQuad* q, SnxUiQuadView* fill);

/* the picture of the user playing (common/utilities/users.h), side pixels square at x, y: drawn SNX_UI_PICTURE texels
   square in the YoRHa theme's [head] picture shape, square or round (the rest transparent); 0 when the theme shows
   none, which draws nothing */
#define SNX_UI_PICTURE 96
int ui_picture(SnxUiList* l, float x, float y, float side);
/* an icon of the menus' (gui/icons.h), two of the font's cells at a whole scale, as the launcher draws it beside a
   label; returns the width drawn, 0 for GUI_ICON_NONE. Its texels are in the atlas beside the picture, on its rows:
   icon i's 16x16 from x SNX_UI_ICONS_X + 16 * i */
#define SNX_UI_ICONS_X SNX_UI_PICTURE
float ui_icon(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba);
float ui_icon_backed(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba); /* backed (UI_BACKED) */
/* One picture apart from the atlas, SNX_UI_IMAGE_W x SNX_UI_IMAGE_H: the menu's save state thumbnail. ui_image draws it
   at x, y, w x h: 0 while there is none, which draws nothing */
#define SNX_UI_IMAGE_W 320
#define SNX_UI_IMAGE_H 180
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
#define SNX_UI_PICTURE_Y 256
void ui_atlas_build(void);
/* ui_atlas_build in two: the atlas made (NULL without memory), its rows into *rows; then put in under ui_lock, which the
   caller holds (with what is drawn from it changing at once) */
u32* ui_atlas_make(int* rows);
void ui_atlas_put(u32* atlas, int rows);
const u32* ui_atlas(int* w, int* h, u32* version);
void ui_atlas_done(void);

/* the controls and the menu change on the main thread while the renderer draws them on its own: both under this */
void ui_lock(void);
void ui_unlock(void);
#endif
