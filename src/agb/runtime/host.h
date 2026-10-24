/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runner's side of the screen, the speaker and the controls (SDL3): the board's frame in a window at a whole
   scale, its sound at the console's rate, the keyboard and gamepads as the keypad, and the console's 59.73 frames a
   second. The board knows none of it (tests/architecture.py keeps SDL out of scarletnx-agb) */
#ifndef SNX_AGB_HOST_H
#define SNX_AGB_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* KEYINPUT's bits, set for a key held (the register reads them inverted) */
enum {
    AGB_KEY_A = 1 << 0,
    AGB_KEY_B = 1 << 1,
    AGB_KEY_SELECT = 1 << 2,
    AGB_KEY_START = 1 << 3,
    AGB_KEY_RIGHT = 1 << 4,
    AGB_KEY_LEFT = 1 << 5,
    AGB_KEY_UP = 1 << 6,
    AGB_KEY_DOWN = 1 << 7,
    AGB_KEY_R = 1 << 8,
    AGB_KEY_L = 1 << 9,
};

/* the keys a keyboard key holds, the Switch runtime's map (arrows, X or Space A, Z B, Q L, E R, Enter Start,
   Backspace Select): 0 for one it does not use. scancode is SDL's */
uint16_t agb_host_key(int scancode);
/* the keys a console button holds (pad_map.h's bits: A, B, L and ZL, R and ZR, Plus, Minus, the D-pad): 0 for X, Y and
   the sticks' clicks */
uint16_t agb_host_button(int bit);

typedef struct AgbHost AgbHost;
/* the window titled title, fullscreen or not, and the sound: NULL with error saying why. A host with no sound device
   plays without one, and says so on stderr */
AgbHost* agb_host_open(const char* title, bool fullscreen, char* error, size_t error_size);
void agb_host_close(AgbHost* host);
/* the events since the last call: the keys held after them (AGB_KEY_), false once the window is closed */
bool agb_host_poll(AgbHost* host, uint16_t* keys);
/* the 240x160 frame (RGBA, a byte each), its count frames of stereo samples, then the wait to the next frame's time */
void agb_host_frame(AgbHost* host, const uint32_t* framebuffer, const int16_t* samples, uint32_t count);
#endif
