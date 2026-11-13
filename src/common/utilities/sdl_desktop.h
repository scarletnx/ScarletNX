/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What every program of ours asks of SDL before its video starts, on any desktop */
#ifndef SNX_COMMON_SDL_DESKTOP_H
#define SNX_COMMON_SDL_DESKTOP_H
#include <SDL3/SDL_hints.h>

/* No _NET_WM_BYPASS_COMPOSITOR on our windows: SDL asks for it by default, and KWin on X11 answers by suspending
   compositing for the whole desktop while such a window is open; resuming it when the window closes turns the screen
   black for about a second */
static inline void snx_sdl_desktop_hints(void) { SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0"); }

#endif
