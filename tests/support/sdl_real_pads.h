/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A test on SDL's virtual gamepads, with no real controller in it. SDL's real drivers (evdev, HIDAPI, XInput and the
   others) each ask SDL_ShouldIgnoreJoystick about a device they find, and that applies the gamepad allow list. Its
   virtual driver does not ask. So an allow list that names a vendor and product no controller has keeps every real
   controller out, also one with the same vendor and product as a virtual pad (a Pro Controller or a clone of it in its
   Switch mode), and lets each virtual pad in */
#ifndef SNX_TEST_SDL_REAL_PADS_H
#define SNX_TEST_SDL_REAL_PADS_H
#include <SDL3/SDL.h>

/* before SDL finds the controllers, at the start of its joystick and gamepad subsystem (SDL_Init with
   SDL_INIT_GAMEPAD): a controller that SDL found before stays */
static void sdl_real_pads_hide(void) { SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xffff/0xffff"); }
#endif
