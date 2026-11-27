/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's touch screen as the frontend feeds it: fingers (and, when asked, the mouse's left button) in window
   pixels, each kept in a slot of TouchState while it stays down, placed on the game's picture */
#ifndef SNX_TOUCH_H
#define SNX_TOUCH_H
#include "nx/hle/services/hid_touch.h"

#include "common/utilities/ui_quads.h" /* SNX_TOUCH_MOUSE */

/* where the game's 16:9 picture lies in a window's clear part (x, y, w, h), as the renderer letterboxes it */
void touch_picture(int cx, int cy, int cw, int ch, int picture[4]);
/* a finger down at window pixel (px, py): its slot, or -1 when it is off the picture or every slot is taken */
int touch_down(TouchState* t, u64* ids, u64 id, const int picture[4], float px, float py);
/* a finger's move, kept on the picture's edge when it leaves it */
void touch_move(TouchState* t, const u64* ids, u64 id, const int picture[4], float px, float py);
void touch_up(TouchState* t, const u64* ids, u64 id);
#endif
