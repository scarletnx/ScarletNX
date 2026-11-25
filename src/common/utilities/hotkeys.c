/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/hotkeys.h"
#include "common/utilities/common.h"
#include "common/utilities/key_text.h"
#include "common/utilities/quit_key.h"
#include "snx_settings.h"
#include <SDL3/SDL_keycode.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

/* each action's setting, in HOTKEY_'s order */
static const char* const KEYS[HOTKEY_COUNT] = {
    "hotkey_help",     "hotkey_menu",       "hotkey_screenshot",   "hotkey_fps_limit", "hotkey_thread_dump",
    "hotkey_showcase", "hotkey_save_state", "hotkey_load_state",   "hotkey_next_slot", "hotkey_tracy",
    "hotkey_pause",    "hotkey_fullscreen", "hotkey_fullscreen_2", "hotkey_stats",     "hotkey_capture",
    "hotkey_mouse",    "hotkey_quit",       "hotkey_tv_mode",      "hotkey_upscaler",  "hotkey_anti_aliasing"};
/* each action's key and modifiers in effect: their defaults' until hotkey_set reads a setting (the settings' variables
   start as their defaults); started once the first call needs them */
static struct {
    unsigned key[HOTKEY_COUNT], mods[HOTKEY_COUNT];
    int started;
    uint64_t quit_by; /* a second press of the quit key before this quits */
    _Atomic uint32_t version; /* the changes to a key's text */
} g_hotkeys;

uint32_t hotkeys_version(void) { return atomic_load_explicit(&g_hotkeys.version, memory_order_acquire); }

const IniOpt* hotkey_row(int action) {
    const IniOpt* o = action >= 0 && action < HOTKEY_COUNT ? ini_find(g_hotkeys_ini, KEYS[action]) : NULL;
    return o && (o->flags & INI_NEEDS_BUILD) ? NULL : o;
}
const char* hotkey_text(int action) {
    const IniOpt* o = hotkey_row(action);
    return o ? (const char*)o->var : "";
}

unsigned hotkey_mods(unsigned sdl_mod) {
    return (sdl_mod & SDL_KMOD_CTRL ? HOTKEY_CTRL : 0u) | (sdl_mod & SDL_KMOD_ALT ? HOTKEY_ALT : 0u) |
           (sdl_mod & SDL_KMOD_SHIFT ? HOTKEY_SHIFT : 0u) | (sdl_mod & SDL_KMOD_GUI ? HOTKEY_GUI : 0u);
}
const IniOpt* hotkey_setting(const char* key) {
    for (int a = 0; a < HOTKEY_COUNT; ++a)
        if (!strcmp(key, KEYS[a])) return hotkey_row(a);
    return NULL;
}
int hotkey_modifier(unsigned key) { return snx_key_modifier(key); }
int hotkey_parse(const char* text, unsigned* key, unsigned* mods, char* why, size_t n) {
    return snx_key_parse(text, key, mods, why, n);
}
void hotkey_name(unsigned key, unsigned mods, char* out, size_t n) { snx_key_name(key, mods, out, n); }

static void start(void) {
    char why[160];
    int a;
    if (g_hotkeys.started) return;
    g_hotkeys.started = 1;
    for (a = 0; a < HOTKEY_COUNT; ++a) {
        const IniOpt* o = hotkey_row(a);
        if (o && hotkey_parse(o->def, &g_hotkeys.key[a], &g_hotkeys.mods[a], why, sizeof why))
            snx_error("settings: %s's default, %s: %s: it has no key", o->key, o->def, why);
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
        snx_error("settings: %s=%s: %s: it keeps %s", o->key, v, why, now[0] ? now : "\"\" (no key)");
    }
    hotkey_name(g_hotkeys.key[a], g_hotkeys.mods[a], (char*)o->var, o->size);
    atomic_fetch_add_explicit(&g_hotkeys.version, 1, memory_order_release);
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
    atomic_fetch_add_explicit(&g_hotkeys.version, 1, memory_order_release);
    return 0;
}

int hotkeys_ini(const char* k, const char* v) {
    const IniOpt* o = ini_find(g_hotkeys_ini, k);
    if (o) ini_read(o, v);
    return o != NULL;
}

int hotkey_quit_press(uint64_t now_ns) { return snx_quit_press(&g_hotkeys.quit_by, now_ns); }
