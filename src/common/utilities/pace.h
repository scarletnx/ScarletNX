/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Switch's fps_limit as every console's game reads it (pace.c): its text, as the Switch's options_fps_set reads
   it, made a pace, and F4's step between none and the pace before it */
#ifndef SNX_PACE_H
#define SNX_PACE_H
#include <stdbool.h>
#include <stddef.h>

/* a pace: SNX_PACE_CONSOLE (fps_limit=game, the default), SNX_PACE_NONE (fps_limit=0), else 10 to 1000 frames a second */
enum { SNX_PACE_CONSOLE = 0, SNX_PACE_NONE = -1 };
/* The pace fps_limit's text says: "game" in any case the console's, a number below 0 or from 1 to below 2 the
   console's too (an older file's values), 0 to below 1 none, else that many frames a second held to 10 to 1000. False
   for anything else, with *pace as it was. line gets what is said of a value not taken as given, with the value in
   effect ("" when taken as given) */
bool snx_pace_parse(const char* text, int* pace, char* line, size_t line_size);
/* the pace as fps_limit writes it: "game", "0" or the frames a second */
void snx_pace_text(int pace, char* out, size_t size);
/* F4's: *pace none, or back from none to *previous (the console's when it started with none), *previous keeping the
   one before none. F4's line in line (size bytes, 0 for none): the limit now and before it, as a Switch game says it
   (key_said.h) */
void snx_pace_toggle(int* pace, int* previous, char* line, size_t size);
#endif
