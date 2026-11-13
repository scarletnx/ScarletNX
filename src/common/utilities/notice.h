/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A notice over a running game's picture in the windows with no menu of their own (the 3DS's, the Vita's and the
   GBA's), drawn as the Switch's game draws its own (nx/runtime/platform/overlay.c's toast): one row of the 8x16 font
   (font.h) in the picture's top left corner, a cell in from its left and a row down from its top, its letters the
   YoRHa theme's page colour on its ink, a blank cell at either end, for SNX_NOTICE_MS. The band is opaque, so a window
   draws it by copying its pixels over the picture */
#ifndef SNX_NOTICE_H
#define SNX_NOTICE_H
#include <stddef.h>
#include <stdint.h>

#define SNX_NOTICE_MS   3000
#define SNX_MENU_COLS   40 /* the Switch game's menu panel, in cells of the font: the narrowest */
#define SNX_MENU_ROWS   16 /* and its tallest page's rows */
#define SNX_NOTICE_COLS 72 /* a notice's letters at most: the widest panel's */

typedef struct {
    char text[SNX_NOTICE_COLS + 1]; /* a glyph of the font (CP437) a byte, as the Switch's notice takes them */
    uint64_t until_ns;
} SnxNotice;

/* text shown from now_ns, its first SNX_NOTICE_COLS letters */
void snx_notice_show(SnxNotice* n, const char* text, uint64_t now_ns);
/* whether one shows at now_ns */
int snx_notice_shown(const SnxNotice* n, uint64_t now_ns);
/* a display's scale (SDL_GetWindowDisplayScale's, 0 where it has none) as the percent of its pixels the scales take */
static inline int snx_display_percent(float scale) { return scale > 0.0f ? (int)(scale * 100.0f) : 100; }
/* the Switch game's menu letters' scale on a picture sw by sh, its display at percent of its pixels (SDL's display
   scale times 100): its tallest page in half the picture, or twice the display's scale where that is more, within
   three quarters of the picture; 1 at least */
int snx_menu_scale(int sw, int sh, int percent);
/* a notice's letters' scale there: one under the menu's, as the Switch's notice and stats box draw theirs */
int snx_notice_scale(int sw, int sh, int percent);
/* the band of n's text at scale on a picture sw by sh: its left, top, width and height into rect, cut by the picture's
   edges; 0 when none of it is on the picture */
int snx_notice_rect(const SnxNotice* n, int sw, int sh, int scale, int rect[4]);
/* the band's pixels, the rect[2] by rect[3] of it from its top left (snx_notice_rect's), a row each stride bytes:
   R G B A, or B G R A with bgra, opaque */
void snx_notice_pixels(const SnxNotice* n, int scale, int bgra, const int rect[4], unsigned char* out, size_t stride);
#endif
