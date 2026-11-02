/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A runtime hotkey from a setting: SDL's key name with ctrl, shift, alt and gui/super between '+', and whether a
   keyboard event is that key with exactly those modifiers. */
#ifndef SNX_KEYBIND_H
#define SNX_KEYBIND_H
#include <SDL3/SDL.h>

typedef struct {
    SDL_Keycode key;
    SDL_Keymod mods;
} SnxKeybind;

/* the .ini's key (ctrl+u, alt+u, shift+f13, f13): 1 and its key and modifiers, 0 for one that doesn't parse */
int snx_keybind_parse(const char* spec, SnxKeybind* out);
/* whether e is that key with those modifiers (lock keys ignored) */
int snx_keybind_match(const SnxKeybind* bind, const SDL_KeyboardEvent* e);
#endif
