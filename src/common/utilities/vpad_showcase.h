/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The showcase, drawn with the on-screen controls' looks (common/utilities/vpad.h): the controller as the game gets it,
   in every console's window */
#ifndef SNX_VPAD_SHOWCASE_H
#define SNX_VPAD_SHOWCASE_H
#include "common/utilities/vpad.h"
#include "common/utilities/ui_ring.h"

/* counts the changes to showcase's place (its corner): what stats' box beside it lays out by */
u32 vpad_showcase_place_version(void);
/* Showcase (settings.txt's showcase): player 1's buttons (HidNpadButton bits) and sticks (-32767 to 32767, up
   positive) as the game got them, drawn with these controls' sprites in showcase_corner's corner of safe, and the
   frames a second (fps, below 0 for none yet) over them. Both under ui_lock */
void vpad_showcase_input(u64 buttons, const i32 sticks[4]);
/* the frames a second the showcase shows: the game's presents (a count that only grows) over the last half second or
   more, at now_ns; -1 until the first half second is over */
int vpad_showcase_fps(u32 presents, u64 now_ns);
/* scale: the menu's letters' (overlay_place), the frames a second's too, a step smaller while they would not keep
   above the box's row under them. 2 when it is off (nothing drawn), 1 when laid out again, 0 for the kept quads. Only
   the controls the window's console has (vpad_console) */
int vpad_showcase_draw(SnxUiList* l, const int safe[4], int fps, int scale);
/* the showcase's box on safe (x, y, w, h into box) and its corner, 0 top left, 1 top right, 2 bottom left, 3 bottom
   right; -1 while it is off */
int vpad_showcase_box(const int safe[4], float box[4]);
/* a corner's word, tl tr bl br, as showcase_corner and stats_corner take them: its index as above, -1 for another */
int vpad_corner(const char* word);
const char* vpad_corner_word(int corner);
#endif
