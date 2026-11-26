/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen controls (vpad.h) of each console but the Switch, whose own are vpad.c's: for its window, and for the
   launcher's Settings of its games, which list the settings it has */
#ifndef SNX_VPAD_CONSOLES_H
#define SNX_VPAD_CONSOLES_H
#include "common/utilities/vpad.h"

/* the GBA's: A, B, L, R, Start, Select and the D-pad, beside the screen in a window wider than tall and under it in
   another */
extern const SnxVpadConsole snx_vpad_gba;
/* the DS's: those, X and Y, around the screens, clear of them in a phone held upright. Its touch screen is the lower
   one */
extern const SnxVpadConsole snx_vpad_nds;
/* the Vita's keys and its two sticks. Its touch screen is the front one */
extern const SnxVpadConsole snx_vpad_vita;
/* the 3DS's: the Circle Pad the left stick, the C-Stick the right one, ZL and ZR the New 3DS's */
extern const SnxVpadConsole snx_vpad_3ds;
#endif
