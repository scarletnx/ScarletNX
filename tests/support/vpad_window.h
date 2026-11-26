/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Fingers, the mouse's left button and keys on a console's window and its on-screen controls (common/utilities/
   vpad_sdl.h), as SDL's events, for the tests of the consoles' windows: the next poll of the window reads them */
#ifndef SNX_TEST_VPAD_WINDOW_H
#define SNX_TEST_VPAD_WINDOW_H
#include "common/utilities/vpad_sdl.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* a finger of touch on window, at x, y of its pixels */
static inline bool vpad_test_finger(SDL_Window* window, Uint32 type, SDL_TouchID touch, SDL_FingerID id, float x,
                                    float y) {
    int w = 0, h = 0;
    SDL_Event e = {0};
    if (!SDL_GetWindowSizeInPixels(window, &w, &h)) return false;
    e.tfinger.type = (SDL_EventType)type;
    e.tfinger.touchID = touch;
    e.tfinger.fingerID = id;
    e.tfinger.x = x / (float)w;
    e.tfinger.y = y / (float)h;
    e.tfinger.windowID = SDL_GetWindowID(window);
    return SDL_PushEvent(&e);
}
/* the mouse's left button, or its motion, at x, y of window's pixels (its points, at the density 1 of the tests) */
static inline bool vpad_test_mouse(SDL_Window* window, Uint32 type, float x, float y) {
    SDL_Event e = {0};
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.windowID = SDL_GetWindowID(window);
        e.motion.state = SDL_BUTTON_LMASK;
        e.motion.x = x, e.motion.y = y;
    } else {
        e.button.windowID = SDL_GetWindowID(window);
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.x = x, e.button.y = y;
    }
    return SDL_PushEvent(&e);
}
/* key down in window, with mod held */
static inline bool vpad_test_key_mod(SDL_Window* window, SDL_Keycode key, SDL_Keymod mod) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = SDL_GetWindowID(window);
    e.key.key = key;
    e.key.mod = mod;
    e.key.down = true;
    return SDL_PushEvent(&e);
}
static inline bool vpad_test_key(SDL_Window* window, SDL_Keycode key) { return vpad_test_key_mod(window, key, 0); }
/* a control's middle in the window's pixels */
static inline void vpad_test_middle(int control, float* x, float* y) {
    int safe[4];
    snx_vpad_window_safe(safe);
    *x = (float)safe[0] + vpad_config()->at[control].x * (float)safe[2];
    *y = (float)safe[1] + vpad_config()->at[control].y * (float)safe[3];
}
/* a control's longer side there, of the clear part's shorter one at 100% (common/utilities/vpad.c) */
static inline float vpad_test_side(float size) {
    int safe[4];
    snx_vpad_window_safe(safe);
    return size * vpad_side(safe);
}
/* a virtual gamepad connected: its id, 0 where SDL has none */
static inline SDL_JoystickID vpad_test_gamepad(void) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    return SDL_AttachVirtualJoystick(&d);
}
/* text in the file at path: how many times */
static inline int vpad_test_in_file(const char* path, const char* text) {
    size_t size = 0;
    int n = 0;
    char* all = (char*)SDL_LoadFile(path, &size);
    for (const char* at = all; at && (at = strstr(at, text)) != NULL; at += strlen(text)) ++n;
    SDL_free(all);
    return n;
}
#endif
