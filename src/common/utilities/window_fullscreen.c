/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/window_fullscreen.h"
#include <stdio.h>

bool snx_window_fullscreen(SDL_Window* window, bool on, char* why, size_t size) {
    if (window && !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == !on) return true;
    if (SDL_SetWindowFullscreen(window, on)) return true;
    snprintf(why, size, "the window cannot go %s: %s", on ? "fullscreen" : "back from fullscreen", SDL_GetError());
    return false;
}
