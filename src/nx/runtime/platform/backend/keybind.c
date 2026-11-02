/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/backend/keybind.h"
#include "common/utilities/ascii.h"
#include <stdio.h>
#include <string.h>

#define KEYBIND_MODS (SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI)

static char* trim(char* s) {
    char* end;
    while (snx_is_blank((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && snx_is_blank((unsigned char)end[-1])) *--end = 0;
    return s;
}
int snx_keybind_parse(const char* spec, SnxKeybind* out) {
    char text[64], *save = NULL, *part;
    SDL_Keymod mods = SDL_KMOD_NONE;
    SDL_Keycode key;
    if (!spec || !*spec) return 0;
    snprintf(text, sizeof text, "%s", spec);
    part = SDL_strtok_r(text, "+", &save);
    if (!part) return 0;
    for (;;) {
        char* next = SDL_strtok_r(NULL, "+", &save);
        part = trim(part);
        if (!*part) return 0;
        if (!next) break;
        if (!SDL_strcasecmp(part, "ctrl") || !SDL_strcasecmp(part, "control")) mods |= SDL_KMOD_CTRL;
        else if (!SDL_strcasecmp(part, "shift")) mods |= SDL_KMOD_SHIFT;
        else if (!SDL_strcasecmp(part, "alt")) mods |= SDL_KMOD_ALT;
        else if (!SDL_strcasecmp(part, "gui") || !SDL_strcasecmp(part, "super") || !SDL_strcasecmp(part, "meta"))
            mods |= SDL_KMOD_GUI;
        else
            return 0;
        part = next;
    }
    key = SDL_GetKeyFromName(part);
    if (key == SDLK_UNKNOWN) return 0;
    out->key = key;
    out->mods = mods;
    return 1;
}
int snx_keybind_match(const SnxKeybind* bind, const SDL_KeyboardEvent* e) {
    return bind->key == e->key && (e->mod & KEYBIND_MODS) == bind->mods;
}
