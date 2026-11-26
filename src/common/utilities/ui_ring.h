/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the layer over the game draws besides letters and shapes (ui_ring.c): the ground of backed letters and icons,
   the menus' icons and the stats' ring of bars, for the Switch's renderer and the rectangles of the other consoles'
   windows alike */
#ifndef SNX_UI_RING_H
#define SNX_UI_RING_H
#include "common/utilities/ui_quads.h"

/* the ground of every backed letter and icon, R G B A from the low byte: one for all that is drawn over the game (the
   stats, the debug line, the notification), the layer's ink (ui_colors.h) as a quad holds it (ui_rgba), so a fill of
   the ink beside a backed letter is the same pixel; whoever draws and whoever doesn't */
u32 ui_ground(void);
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
/* an icon of the menus' (gui/icons.h), two of the font's cells at a whole scale, as the launcher draws it beside a
   label; returns the width drawn, 0 for GUI_ICON_NONE. Its texels are in the atlas beside the picture, on its rows:
   icon i's 16x16 from x SNX_UI_ICONS_X + 16 * i */
#define SNX_UI_ICONS_X SNX_UI_PICTURE
float ui_icon(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba);
float ui_icon_backed(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba); /* backed (UI_BACKED) */
#endif
