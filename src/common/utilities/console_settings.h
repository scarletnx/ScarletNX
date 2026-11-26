/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Which of the Switch's settings (src/nx/runtime/settings.txt) each console's game takes, and why another does not:
   one table that the consoles' drivers read their settings by (--key=value, the game's .ini and its picks), that the
   launcher passes a run's picks by and shows a console's game's own Settings by. Every row of the Switch's tables has
   its line here, a console's own tables' (ctr_, vita_, nds_, gba_, linux_, android_) aside: tests/console_settings.c
   fails on a row without one. A console takes a row unless its line says why not: the console lacks what the row sets
   (no dock, no HDR screen, not the Switch's GPU), or what it needs is not made yet (later) */
#ifndef SNX_CONSOLE_SETTINGS_H
#define SNX_CONSOLE_SETTINGS_H
#include <stdbool.h>
#include "common/utilities/game_ini.h"
#include "snx_settings.h" /* IniOpt */

/* the consoles, as game_ini.h's SnxSystem has them */
#define SNX_SYSTEMS (SNX_SYSTEM_NDS + 1)
/* why a console's driver does not take a row: NULL text when it does */
typedef struct {
    const char* text; /* a few words, after "--docked=1 does nothing in a GBA game: " ("it has no dock") */
    bool later;       /* what it needs is not made yet, not something the console lacks */
} SnxConsoleWhy;
typedef struct {
    /* the row's key as settings.txt writes it: gamepad_p<N>_map for gamepad_p1_map to gamepad_p8_map, controls_<ID>
       for controls_a, vk_<extension> for every row of the Vulkan Extensions table */
    const char* key;
    SnxConsoleWhy why[SNX_SYSTEMS]; /* by console: a NULL text where it takes the row */
} SnxConsoleSetting;
extern const SnxConsoleSetting g_console_settings[]; /* the last line's key NULL */

/* the console's name, as a message says it ("GBA", "DS") */
const char* snx_console_name(SnxSystem console);
/* the line of the row of key (gamepad_p1_map, controls_a_shown, vk_khr_present_wait): NULL for none, which a console's
   own row and a key of no row have */
const SnxConsoleSetting* snx_console_setting(const char* key);
/* why console's driver does not take key's row; NULL when it does, a Switch's row always on the Switch */
const SnxConsoleWhy* snx_console_why(SnxSystem console, const char* key);
/* key's row (settings.txt's g_console_ini: its value and range, no set function, which is the Switch's) when console's
   driver takes it, else NULL */
const IniOpt* snx_console_opt(SnxSystem console, const char* key);
/* --KEY=VALUE (its _ a - or not) of a row console's driver takes, read into g_settings as ini_read reads it (a value
   the row does not take said with the one it keeps): 1; 0 for an argument that is no row's or is a console's own
   row's; -1 with error saying why for a row the console's driver does not take ("--docked=1 does nothing in a GBA
   game: it has no dock") */
int snx_console_arg(SnxSystem console, const char* arg, char* error, size_t size);
#endif
