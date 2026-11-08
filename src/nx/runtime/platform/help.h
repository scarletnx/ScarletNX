/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* F1's text (backend/sdl3.c): the game's buttons on the keyboard and the runtime's keys, as keymap, hotkey and
   menu_event in backend/sdl3.c take them; Ctrl+Shift+F12 among them while RenderDoc can capture a frame, and the
   debugger tracing the game after them while one does */
#ifndef SNX_HELP_H
#define SNX_HELP_H
#include <stddef.h>

#define HELP_MAX 2048 /* the text's room, with its NUL */
/* the text into out (size bytes): Ctrl+Shift+F12 with capture, the debugger's name with debugger (? where the system
   doesn't name it; NULL or "" for none). Its length, as snprintf counts it */
int help_text(char* out, size_t size, int capture, const char* debugger);
#endif
