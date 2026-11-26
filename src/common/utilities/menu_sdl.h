/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu (overlay.h) in an SDL window, the Switch's and every console's: what a key, a button, the wheel, a
   finger or the mouse does to it, and what of its answers the window's runtime does */
#ifndef SNX_MENU_SDL_H
#define SNX_MENU_SDL_H
#include <SDL3/SDL.h>

/* the window's runtime's part: its window, size and safe area in pixels; the menu opened or closed (its game paused,
   its keys let go); an OVERLAY_ answer done (exit, a state saved or loaded, the layout edited, a capture); another
   dialog over the game that takes the events while it shows (the controller applet's), and the keyboard's box, NULL
   each where it has none */
typedef struct {
    SDL_Window* (*window)(void);
    void (*size)(int* w, int* h);
    void (*safe)(int safe[4]);
    void (*open)(int open);
    void (*act)(int action);
    int (*dialog_shown)(void);
    int (*dialog_event)(const SDL_Event* e, int action);
    int (*keyboard_event)(const SDL_Event* e, int action);
} SnxMenuWindow;
/* e for the menu, action its runtime key (hotkeys.h, -1 none): 1 when the menu took it, which then reaches nothing
   else. The menu key, Back and a pad's Home open it and close it, and end the layout's editing. Under ui_lock */
int snx_menu_event(const SnxMenuWindow* m, const SDL_Event* e, int action);
#endif
