/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the renderer draws over the game besides the on-screen controls (vpad.h) and the menu (overlay.h): the
   Debug settings not at their default and the GDB stub, a line at the top's middle while the game runs, so a game
   slowed or stopped by one says so on its screen */
#ifndef SNX_HUD_H
#define SNX_HUD_H
#include "nx/runtime/platform/ui.h"

#define HUD_DEBUG_MAX   2048       /* the list's room, with its NUL */
#define HUD_DEBUG_TITLE "Debug on" /* the list's first row */
#define HUD_ROWS_MAX    32         /* the rows it shows at most, its title's included: past them a row says how many */
#define HUD_SLIDE_MS    120        /* a row too long for its room slides a column this often, as the launcher's do */
/* the Debug settings in effect, key=value between ", " (options_debug), "" for none: 1 when it differs from the one
   before. On the main thread; it takes ui_lock */
int hud_debug_set(const char* text);
/* over a picture whose safe area is safe (x, y, w, h), in letters of scale times the 8x16 font (the stats panel's,
   stats_scale): a list as wide as the stats panel's lines (STATS_GRAPH_W letters), its title HUD_DEBUG_TITLE in the
   failure colour, then a row for each setting, its key at the left and its value at the right; a row too long for
   that slides back and forth (snx_slide, a column every HUD_SLIDE_MS ms of now_ms). Under the stats' box (stats,
   x y w h; NULL for none) in its column, to its right when there is no room under it, else at the top's middle;
   rows past the room the picture has under it give theirs to one that says how many more there are. Nothing for
   none. Under ui_lock */
void hud_draw(SnxUiList* l, const int safe[4], int scale, const float* stats, u64 now_ms);
#endif
