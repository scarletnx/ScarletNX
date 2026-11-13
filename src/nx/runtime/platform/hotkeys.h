/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's keys (backend/sdl3.c): an action each, its key a setting of settings.txt's Shortcuts
   (hotkey_<action>), the key as SDL names it after Ctrl+, Alt+, Shift+ or GUI+ for the modifiers held with it, empty
   for none. F1's help (help.c) and the stats box's RenderDoc line show them, and the game's menu's Shortcuts page sets
   them (overlay.c). The main thread's, and the renderer's reading a key's text under ui_lock, which the settings' reads
   (backend_ini) and the menu hold too */
#ifndef SNX_HOTKEYS_H
#define SNX_HOTKEYS_H
#include "common/utilities/key_text.h"
#include "nx/runtime/ini.h"
#include <stddef.h>
#include <stdint.h>

/* the actions, in F1's order */
enum {
    HOTKEY_HELP,
    HOTKEY_MENU,
    HOTKEY_SCREENSHOT,
    HOTKEY_FPS_LIMIT,
    HOTKEY_THREAD_DUMP,
    HOTKEY_SHOWCASE,
    HOTKEY_SAVE_STATE,
    HOTKEY_LOAD_STATE,
    HOTKEY_NEXT_SLOT,
    HOTKEY_TRACY,
    HOTKEY_PAUSE,
    HOTKEY_FULLSCREEN,
    HOTKEY_FULLSCREEN_2,
    HOTKEY_STATS,
    HOTKEY_CAPTURE,
    HOTKEY_MOUSE,
    HOTKEY_QUIT,
    HOTKEY_TV_MODE,
    HOTKEY_UPSCALER,
    HOTKEY_ANTI_ALIASING,
    HOTKEY_COUNT
};
/* the modifiers held with a key, either side's, in the order a key's text names them (common/utilities/key_text.h,
   whose parsing and matching these keys' are) */
enum { HOTKEY_CTRL = SNX_KEY_CTRL, HOTKEY_ALT = SNX_KEY_ALT, HOTKEY_SHIFT = SNX_KEY_SHIFT, HOTKEY_GUI = SNX_KEY_GUI };

/* action's row of settings.txt's Shortcuts (its key, label and default), NULL where the build has none: Tracy's
   without SNX_TRACY */
const IniOpt* hotkey_row(int action);
/* action's key as its setting holds it, SDL's name after the modifiers' (Ctrl+Shift+F12); "" for none */
const char* hotkey_text(int action);
/* counts the changes to the keys (hotkey_set, hotkey_assign), for the menu's pages that name them */
uint32_t hotkeys_version(void);
/* text as a key (SDL_Keycode) and the modifiers held with it: 0, "" giving no key (0); else -1 with why into why (n
   bytes): a name SDL doesn't know, a modifier where the key goes, or nothing after the modifiers */
int hotkey_parse(const char* text, unsigned* key, unsigned* mods, char* why, size_t n);
/* key and mods as a setting holds them, into out (n bytes): "" for no key */
void hotkey_name(unsigned key, unsigned mods, char* out, size_t n);
/* whether key is a modifier (Ctrl, Shift, Alt, GUI, AltGr...), which only goes with another */
int hotkey_modifier(unsigned key);
/* the action a key pressed with mods does, -1 for none: the one set to that key with those modifiers, else, with Shift
   held, the one set to the key without Shift (Shift+F12 is F12's, as before the keys were settings). The first in
   HOTKEY_'s order; another set to the same key in *also, else -1 */
int hotkey_find(unsigned key, unsigned mods, int* also);
/* settings.txt's set of the Shortcuts' rows: a text that isn't a key refused in the log, with the key the setting
   keeps; the setting's text the key's, as hotkey_name says it */
void hotkey_set(const IniOpt* o, const char* v);
/* action set to key with mods (key 0: none), as the game's menu sets it, the setting's text with it: 0; else -1, with
   the action set to that key with those modifiers already in *owner, action's key as it was */
int hotkey_assign(int action, unsigned key, unsigned mods, int* owner);
/* backend_ini's: k a Shortcuts' row, v read into it: 1; else 0 */
int hotkeys_ini(const char* k, const char* v);
/* the quit key pressed at now_ns (plat_ns): 1 when it quits, a second press within SNX_QUIT_AGAIN_NS of a first
   (common/utilities/quit_key.h); 0 for a first, which only warns */
int hotkey_quit_press(uint64_t now_ns);
#endif
