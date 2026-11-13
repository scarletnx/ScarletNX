/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/key_text.h"
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_stdinc.h>
#include <stdio.h>
#include <string.h>

/* the modifiers' words, in a text's order */
static const struct {
    const char* word;
    unsigned bit;
} MODS[] = {{"Ctrl", SNX_KEY_CTRL}, {"Alt", SNX_KEY_ALT}, {"Shift", SNX_KEY_SHIFT}, {"GUI", SNX_KEY_GUI}};
#define MOD_COUNT (int)(sizeof MODS / sizeof *MODS)

unsigned snx_key_mods(unsigned sdl_mod) {
    return (sdl_mod & SDL_KMOD_CTRL ? SNX_KEY_CTRL : 0u) | (sdl_mod & SDL_KMOD_ALT ? SNX_KEY_ALT : 0u) |
           (sdl_mod & SDL_KMOD_SHIFT ? SNX_KEY_SHIFT : 0u) | (sdl_mod & SDL_KMOD_GUI ? SNX_KEY_GUI : 0u);
}

int snx_key_modifier(unsigned key) {
    return (key >= SDLK_LCTRL && key <= SDLK_RGUI) || key == SDLK_MODE || key == SDLK_LEVEL5_SHIFT ||
           (key >= SDLK_LMETA && key <= SDLK_RHYPER);
}

int snx_key_parse(const char* text, unsigned* key, unsigned* mods, char* why, size_t n) {
    const char* p = text;
    SDL_Keycode k;
    int i;
    *key = *mods = 0;
    if (!*text) return 0;
    /* the modifiers, each with its +: "Ctrl++" is Ctrl and the + key */
    for (;;) {
        size_t len = 0;
        for (i = 0; i < MOD_COUNT; ++i) {
            len = strlen(MODS[i].word);
            if (!SDL_strncasecmp(p, MODS[i].word, len) && p[len] == '+') break;
        }
        if (i == MOD_COUNT) break;
        *mods |= MODS[i].bit;
        p += len + 1;
    }
    if (!*p) {
        snprintf(why, n, "no key after %s", text);
        return -1;
    }
    k = SDL_GetKeyFromName(p);
    if (k == SDLK_UNKNOWN) {
        snprintf(why, n, "no key is named \"%s\" (a key as SDL names it, after Ctrl+, Alt+, Shift+ or GUI+)", p);
        return -1;
    }
    if (snx_key_modifier(k)) {
        snprintf(why, n, "%s is a modifier, which goes with a key (Ctrl+F1)", p);
        return -1;
    }
    *key = k;
    return 0;
}

void snx_key_name(unsigned key, unsigned mods, char* out, size_t n) {
    size_t at = 0;
    int i;
    if (n) out[0] = 0;
    if (!key) return;
    for (i = 0; i < MOD_COUNT; ++i)
        if (mods & MODS[i].bit) {
            snprintf(out + at, n - at, "%s+", MODS[i].word);
            at += strlen(out + at);
        }
    snprintf(out + at, n - at, "%s", SDL_GetKeyName(key));
}

int snx_key_set(const char* key, const char* value, char* var, size_t size, char* said, size_t n) {
    unsigned k, m;
    char why[192];
    if (snx_key_parse(value, &k, &m, why, sizeof why)) {
        snprintf(said, n, "%s=%s: %s: it keeps %s", key, value, why, var[0] ? var : "\"\" (no key)");
        return -1;
    }
    snx_key_name(k, m, var, size);
    return 0;
}

int snx_key_find(const unsigned* keys, const unsigned* mods, int count, unsigned key, unsigned m, int* also) {
    int found = -1, a;
    *also = -1;
    if (!key) return -1;
    for (;;) {
        for (a = 0; a < count; ++a) {
            if (keys[a] != key || mods[a] != m) continue;
            if (found < 0)
                found = a;
            else if (*also < 0)
                *also = a;
        }
        if (found >= 0 || !(m & SNX_KEY_SHIFT)) return found;
        m &= ~(unsigned)SNX_KEY_SHIFT; /* the key without Shift's */
    }
}
