/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/keybind.h"
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
    /* SDL_KMOD_CTRL is both Ctrl keys: an event has only the side that is down, so compare whether each modifier is
       held, not its bits. The lock keys and the sides then do not matter. */
    return bind->key == e->key && !!(e->mod & SDL_KMOD_CTRL) == !!(bind->mods & SDL_KMOD_CTRL) &&
           !!(e->mod & SDL_KMOD_SHIFT) == !!(bind->mods & SDL_KMOD_SHIFT) &&
           !!(e->mod & SDL_KMOD_ALT) == !!(bind->mods & SDL_KMOD_ALT) &&
           !!(e->mod & SDL_KMOD_GUI) == !!(bind->mods & SDL_KMOD_GUI);
}
static int modifier_key(SDL_Keycode key) {
    return key == SDLK_LCTRL || key == SDLK_RCTRL || key == SDLK_LSHIFT || key == SDLK_RSHIFT || key == SDLK_LALT ||
           key == SDLK_RALT || key == SDLK_LGUI || key == SDLK_RGUI;
}
int snx_keybind_name(const SDL_KeyboardEvent* e, char* out, size_t size) {
    char name[64], text[128];
    const char* key_name;
    int at = 0;
    if (e->repeat) return 0;
    if (e->key == SDLK_ESCAPE) return -1;
    if (modifier_key(e->key)) return 0;
    key_name = SDL_GetKeyName(e->key);
    if (!key_name || !*key_name || !SDL_strcasecmp(key_name, "unknown") || strchr(key_name, '+')) return 0;
    snprintf(name, sizeof name, "%s", key_name);
    SDL_strlwr(name);
    if (e->mod & SDL_KMOD_CTRL) at += snprintf(text + at, sizeof text - at, "ctrl+");
    if (e->mod & SDL_KMOD_SHIFT) at += snprintf(text + at, sizeof text - at, "shift+");
    if (e->mod & SDL_KMOD_ALT) at += snprintf(text + at, sizeof text - at, "alt+");
    if (e->mod & SDL_KMOD_GUI) at += snprintf(text + at, sizeof text - at, "gui+");
    at += snprintf(text + at, sizeof text - at, "%s", name);
    if (at < 0 || (size_t)at >= sizeof text || (size_t)at >= size) return 0;
    snprintf(out, size, "%s", text);
    return 1;
}
