/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/hotkeys.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/quit_key.h"
#include "snx_settings.h"
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_stdinc.h>
#include <stdio.h>
#include <string.h>

/* each action's setting, in HOTKEY_'s order */
static const char* const KEYS[HOTKEY_COUNT] = {
    "hotkey_help",     "hotkey_menu",       "hotkey_screenshot",   "hotkey_fps_limit", "hotkey_thread_dump",
    "hotkey_showcase", "hotkey_save_state", "hotkey_load_state",   "hotkey_next_slot", "hotkey_tracy",
    "hotkey_pause",    "hotkey_fullscreen", "hotkey_fullscreen_2", "hotkey_stats",     "hotkey_capture",
    "hotkey_mouse",    "hotkey_quit",       "hotkey_tv_mode"};
/* the modifiers' words, in a text's order */
static const struct {
    const char* word;
    unsigned bit;
} MODS[] = {{"Ctrl", HOTKEY_CTRL}, {"Alt", HOTKEY_ALT}, {"Shift", HOTKEY_SHIFT}, {"GUI", HOTKEY_GUI}};
#define MOD_COUNT (int)(sizeof MODS / sizeof *MODS)

/* each action's key and modifiers in effect: their defaults' until hotkey_set reads a setting (the settings' variables
   start as their defaults); started once the first call needs them */
static struct {
    unsigned key[HOTKEY_COUNT], mods[HOTKEY_COUNT];
    int started;
    uint64_t quit_by; /* a second press of the quit key before this quits */
    uint32_t version; /* the changes to a key's text */
} g_hotkeys;

uint32_t hotkeys_version(void) { return __atomic_load_n(&g_hotkeys.version, __ATOMIC_ACQUIRE); }

const IniOpt* hotkey_row(int action) {
    return action >= 0 && action < HOTKEY_COUNT ? ini_find(g_hotkeys_ini, KEYS[action]) : NULL;
}
const char* hotkey_text(int action) {
    const IniOpt* o = hotkey_row(action);
    return o ? (const char*)o->var : "";
}

int hotkey_modifier(unsigned key) {
    return (key >= SDLK_LCTRL && key <= SDLK_RGUI) || key == SDLK_MODE || key == SDLK_LEVEL5_SHIFT ||
           (key >= SDLK_LMETA && key <= SDLK_RHYPER);
}

int hotkey_parse(const char* text, unsigned* key, unsigned* mods, char* why, size_t n) {
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
    if (hotkey_modifier(k)) {
        snprintf(why, n, "%s is a modifier, which goes with a key (Ctrl+F1)", p);
        return -1;
    }
    *key = k;
    return 0;
}

void hotkey_name(unsigned key, unsigned mods, char* out, size_t n) {
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

/* the defaults' keys, the first time one is needed */
static void start(void) {
    char why[160];
    int a;
    if (g_hotkeys.started) return;
    g_hotkeys.started = 1;
    for (a = 0; a < HOTKEY_COUNT; ++a) {
        const IniOpt* o = hotkey_row(a);
        if (o && hotkey_parse(o->def, &g_hotkeys.key[a], &g_hotkeys.mods[a], why, sizeof why))
            WARN("settings: %s's default, %s: %s: it has no key\n", o->key, o->def, why);
    }
}

int hotkey_find(unsigned key, unsigned mods, int* also) {
    int found = -1, a;
    unsigned m = mods;
    *also = -1;
    start();
    if (!key) return -1;
    for (;;) {
        for (a = 0; a < HOTKEY_COUNT; ++a) {
            if (g_hotkeys.key[a] != key || g_hotkeys.mods[a] != m) continue;
            if (found < 0)
                found = a;
            else if (*also < 0)
                *also = a;
        }
        if (found >= 0 || !(m & HOTKEY_SHIFT)) return found;
        m &= ~(unsigned)HOTKEY_SHIFT; /* the key without Shift's */
    }
}

void hotkey_set(const IniOpt* o, const char* v) {
    char why[192], now[64];
    unsigned key, mods;
    int a;
    for (a = 0; a < HOTKEY_COUNT && strcmp(o->key, KEYS[a]); ++a) continue;
    if (a == HOTKEY_COUNT) return;
    start();
    if (!hotkey_parse(v, &key, &mods, why, sizeof why)) {
        g_hotkeys.key[a] = key;
        g_hotkeys.mods[a] = mods;
    } else {
        hotkey_name(g_hotkeys.key[a], g_hotkeys.mods[a], now, sizeof now);
        WARN("settings: %s=%s: %s: it keeps %s\n", o->key, v, why, now[0] ? now : "\"\" (no key)");
    }
    hotkey_name(g_hotkeys.key[a], g_hotkeys.mods[a], (char*)o->var, o->size);
    __atomic_add_fetch(&g_hotkeys.version, 1, __ATOMIC_RELEASE);
}

int hotkey_assign(int action, unsigned key, unsigned mods, int* owner) {
    const IniOpt* o = hotkey_row(action);
    int a;
    *owner = -1;
    if (!o) return -1;
    start();
    for (a = 0; a < HOTKEY_COUNT && key; ++a)
        if (a != action && g_hotkeys.key[a] == key && g_hotkeys.mods[a] == mods) {
            *owner = a;
            return -1;
        }
    g_hotkeys.key[action] = key;
    g_hotkeys.mods[action] = mods;
    hotkey_name(key, mods, (char*)o->var, o->size);
    __atomic_add_fetch(&g_hotkeys.version, 1, __ATOMIC_RELEASE);
    return 0;
}

int hotkeys_ini(const char* k, const char* v) {
    const IniOpt* o = ini_find(g_hotkeys_ini, k);
    if (o) ini_read(o, v);
    return o != NULL;
}

int hotkey_quit_press(uint64_t now_ns) { return snx_quit_press(&g_hotkeys.quit_by, now_ns); }
