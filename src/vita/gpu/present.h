/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The screen (present.c): the window, the framebuffer sceDisplaySetFrameBuf names shown at each vblank, and the run's
   screenshots */
#ifndef SNX_VITA_PRESENT_H
#define SNX_VITA_PRESENT_H
#include "vita/runtime/process.h"

/* the window titled title (none headless or without a display), its input and the device; ini: the game's, which keeps
   the on-screen controls' layout (NULL: none kept), whose settings are read by then (vpad_settings); shot: where the
   run's last frame goes (a PPM), NULL for none */
void vita_present_init(const char* title, const char* ini, const char* shot);
/* what the on-screen controls say, on stdout, as the run's other lines */
void vita_present_say(const char* line);
/* a vblank: the framebuffer shown, the window's events read. False once the player closed the window */
bool vita_present_vblank(VitaProcess* p);
/* between two slices (no thread running, p->current NULL): a save states' key asked (F7 save, F8 load, F9 the next
   slot) and the unattended saves and loads due at ms of the run's clock (state_save, state_load), as a Switch game's,
   in states/ of the game's folder; the framebuffer shown their picture */
void vita_present_states(VitaProcess* p, uint64_t ms);
/* whether the thread dump's key (F5) was pressed since the last call: the run lists its threads between two slices */
bool vita_present_dump_asked(void);
/* the screenshot of the framebuffer shown last, at the end of the run */
void vita_present_shot(VitaProcess* p);
void vita_present_free(void);
#endif
