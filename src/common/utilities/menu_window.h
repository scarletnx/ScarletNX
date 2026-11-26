/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu in a console's window (GBA, DS, Vita, 3DS): Back, F2 or a pad's Home opens it as on the Switch, with
   the console's own: its save states' slots, the on-screen controls, the runtime's keys, the settings the console
   reads (console_settings.h), its pick kept in the game's picks, and Exit */
#ifndef SNX_MENU_WINDOW_H
#define SNX_MENU_WINDOW_H
#include "common/utilities/game_ini.h"

/* the menu over the window snx_vpad_window_open opened, for console's game name in folder (its states/ and its
   picks) */
void snx_menu_window_open(SnxSystem console, const char* folder, const char* name);
void snx_menu_window_close(void);
#endif
