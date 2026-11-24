/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/window_icon.h"

extern const unsigned char snx_window_icon_png[];
extern const size_t snx_window_icon_png_size;
extern const unsigned char snx_window_icon_xmas_png[];
extern const size_t snx_window_icon_xmas_png_size;

SDL_Surface* snx_window_icon_surface_for(int month) {
    if (month == 12)
        return SDL_LoadPNG_IO(SDL_IOFromConstMem(snx_window_icon_xmas_png, snx_window_icon_xmas_png_size), true);
    return SDL_LoadPNG_IO(SDL_IOFromConstMem(snx_window_icon_png, snx_window_icon_png_size), true);
}

SDL_Surface* snx_window_icon_surface(void) {
    SDL_Time now = 0;
    SDL_DateTime date = {0};
    return snx_window_icon_surface_for(SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &date, true) ? date.month
                                                                                                        : 0);
}

void snx_window_icon(SDL_Window* window) {
#ifdef _WIN32
    (void)window;
#else
    SDL_Surface* icon = snx_window_icon_surface();
    if (!icon) return;
    SDL_SetWindowIcon(window, icon); /* it keeps a copy */
    SDL_DestroySurface(icon);
#endif
}
