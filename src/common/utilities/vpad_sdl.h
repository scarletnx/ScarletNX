/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen controls (vpad.h) in an SDL window: its fingers and its mouse's left button on them, what Auto goes by
   (SDL's touch screens and gamepads) and Android's vibrator for their buzz. For the consoles whose window has no menu
   (snx_vpad_window_*), all of it: the keys that edit their layout, the .ini that keeps it, and their rectangles over
   the picture. Beside SDL's own events, on the thread that reads them */
#ifndef SNX_VPAD_SDL_H
#define SNX_VPAD_SDL_H
#include <SDL3/SDL.h>
#include "common/utilities/ui_rects.h"
#include "common/utilities/vpad.h"

#define SNX_VPAD_MOUSE (~(u64)0) /* the mouse's id among the fingers' */

/* what they ask of SDL before its video starts: a finger and the mouse each reach a window once, not as the other too,
   and Android's Back is a key and leaves nothing */
void snx_vpad_sdl_hints(void);
/* a finger's id, the pen's apart: SDL gives a pen's touch the id 1 on every platform, the id Android and Wayland give a
   first finger and any finger may have, and no real finger's id sets bit 63 */
u64 snx_vpad_sdl_finger(const SDL_TouchFingerEvent* f);
/* a finger's event, or the mouse's left button's or motion, at x, y in the window's pixels: 1 for one that landed on
   the controls (and a lift of the button off them), which the game's touch screen then does not get */
int snx_vpad_sdl_pointer(const SDL_Event* e, float x, float y, const int safe[4]);
/* vpad_auto with the touch screens SDL knows, or a finger seen on one, and the gamepads connected */
void snx_vpad_sdl_auto(int finger_seen);
/* Android's vibrator, which SDL lists as VIBRATOR_SERVICE, for the controls' buzz: NULL for none, and elsewhere (a
   gamepad's motors are no buzz for a finger) */
SDL_Haptic* snx_vpad_sdl_buzzer(void);
void snx_vpad_sdl_buzz(SDL_Haptic* buzzer);

/* window's controls, their layout kept in [On-screen controls] of the game's .ini at ini as it is edited, what they say
   on say. After vpad_console and vpad_settings */
void snx_vpad_window_open(SDL_Window* window, const char* ini, void (*say)(const char* line));
void snx_vpad_window_close(void);
/* window's clear part (its safe area, whole when fullscreen) in its pixels: x, y, w, h */
void snx_vpad_window_safe(int safe[4]);
/* where a finger's event, or the mouse's motion or button, is in the window's pixels: false for the mouse's that SDL made
   of a finger, and where the window has no size */
bool snx_vpad_window_point(const SDL_Event* e, float* x, float* y);
/* an event of the window's: PASS for one that is not the controls', TAKEN for one that is (the game gets none of it),
   QUIT for the key that quits. Android's Back edits the layout, as F2 does, and a second one within quit_key.h's
   time quits, which the first says over the picture; later, or with Esc or F2, it ends the editing */
typedef enum { SNX_VPAD_PASS, SNX_VPAD_TAKEN, SNX_VPAD_QUIT } SnxVpadEvent;
SnxVpadEvent snx_vpad_window_event(const SDL_Event* e);
/* vpad_state, with the buzz they ask for: nothing held while their layout is edited */
void snx_vpad_window_state(u64* buttons, i32 sticks[4]);
/* their rectangles over a picture w by h pixels, the window's: NULL while there are none to draw */
const SnxUiRects* snx_vpad_window_rects(int w, int h);
/* those drawn by SDL's renderer, over what it drew */
void snx_vpad_window_render(SDL_Renderer* renderer);
#endif
