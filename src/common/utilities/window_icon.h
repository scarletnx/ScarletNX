/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_WINDOW_ICON_H
#define SNX_WINDOW_ICON_H
#include <SDL3/SDL.h>

/* the program's icon for month (1 to 12; 0 when the date is not known), decoded from the PNGs built in
   (cmake/window_icon.cmake): the Christmas Wimsy in December, Wimsy the rest of the year; NULL if it could not be */
SDL_Surface* snx_window_icon_surface_for(int month);
/* the icon for this month, by the local time */
SDL_Surface* snx_window_icon_surface(void);
/* Gives a window the program's icon where the window system takes it from the program. Windows takes the
   executable's own (runtime.rc), with every size, so nothing is set there */
void snx_window_icon(SDL_Window* window);
#endif
