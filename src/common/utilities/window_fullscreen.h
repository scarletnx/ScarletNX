/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A console's window to fullscreen and back, as it starts (the setting fullscreen, --fullscreen) and on its keys (F11,
   Alt+Return): every console's driver the same way, the system's refusal said with its reason */
#ifndef SNX_WINDOW_FULLSCREEN_H
#define SNX_WINDOW_FULLSCREEN_H
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

/* window fullscreen when on, else windowed; nothing asked of the system when it is so already. True when it is, else
   false with why ("the window cannot go fullscreen: <SDL's reason>", "... back from fullscreen: ...") for the caller to
   say on its log */
bool snx_window_fullscreen(SDL_Window* window, bool on, char* why, size_t size);
#endif
