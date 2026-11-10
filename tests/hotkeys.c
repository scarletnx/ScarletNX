/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's keys (runtime/platform/hotkeys.c): settings.txt's Shortcuts a row an action, each default today's
   key; a key's text read as SDL names keys, its modifiers before it in any case, and said back in one way; the action
   a key pressed with modifiers does, Shift's loose match below an exact one, and a key two actions share; a setting
   read from the .ini, an empty one no key, and one that isn't a key refused with the value given and the one in
   effect; the quit key's two presses */
#include "nx/runtime/platform/hotkeys.h"
#include "common/utilities/quit_key.h"
#include "snx_settings.h"
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* the log's last line; a key's text as read (parse: 0 with key and mods, or -1 with why); the second action a key
   find looked for is set to (also) */
static struct {
    char logged[512];
    unsigned key, mods;
    char why[256];
    int also;
} g;
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g.logged, sizeof g.logged, fmt, ap);
    va_end(ap);
}

static int parse(const char* text) { return hotkey_parse(text, &g.key, &g.mods, g.why, sizeof g.why); }
/* the action key with mods does */
static int find(unsigned key, unsigned mods) { return hotkey_find(key, mods, &g.also); }
/* a setting read as the .ini's are */
static void set(int action, const char* v) { ini_read(hotkey_row(action), v); }

int main(void) {
    static const char* const DEFAULTS[HOTKEY_COUNT] = {"F1",     "F2",     "F3",         "F4",  "F5",
                                                       "F6",     "F7",     "F8",         "F9",  "F10",
                                                       "Pause",  "F11",    "Alt+Return", "F12", "Ctrl+Shift+F12",
                                                       "Ctrl+G", "Escape", "Ctrl+D"};
    char name[64];
    const IniOpt* o;
    int a, rows = 0;
    /* a row an action, each read by hotkey_set, its default today's key: Tracy's only in a build with it */
    for (o = g_hotkeys_ini; o->key; ++o) ++rows;
    for (a = 0; a < HOTKEY_COUNT; ++a) {
        o = hotkey_row(a);
        if (!o) {
            assert(a == HOTKEY_TRACY && !strcmp(hotkey_text(a), ""));
            continue;
        }
        assert(o->set == hotkey_set && o->type == INI_TEXT && o->size == 64 && o->name && *o->name);
        assert(!strcmp(o->def, DEFAULTS[a]) && !strcmp(hotkey_text(a), DEFAULTS[a]) && !parse(o->def) && g.key);
    }
    assert(rows == HOTKEY_COUNT - !hotkey_row(HOTKEY_TRACY) && !hotkey_row(-1) && !hotkey_row(HOTKEY_COUNT));

    /* a key's text: SDL's name after the modifiers, in any case, each modifier once in the text's order */
    assert(!parse("F1") && g.key == SDLK_F1 && g.mods == 0);
    assert(!parse("Ctrl+Shift+F12") && g.key == SDLK_F12 && g.mods == (HOTKEY_CTRL | HOTKEY_SHIFT));
    assert(!parse("ctrl+alt+delete") && g.key == SDLK_DELETE && g.mods == (HOTKEY_CTRL | HOTKEY_ALT));
    assert(!parse("GUI+a") && g.key == SDLK_A && g.mods == HOTKEY_GUI && !parse("A") && g.key == SDLK_A);
    /* the + after Ctrl's: the key SDL names +, the unshifted one it is on (= on a US layout); the keypad's own */
    assert(!parse("Ctrl++") && g.key == SDL_GetKeyFromName("+") && g.key && g.mods == HOTKEY_CTRL);
    assert(!parse("Ctrl+Keypad +") && g.key == SDLK_KP_PLUS && g.mods == HOTKEY_CTRL);
    assert(!parse("Alt+Return") && g.key == SDLK_RETURN && !parse("Keypad Enter") && g.key == SDLK_KP_ENTER);
    assert(!parse("Escape") && g.key == SDLK_ESCAPE && !parse("Pause") && g.key == SDLK_PAUSE);
    assert(!parse("") && g.key == 0 && g.mods == 0); /* no key */
    hotkey_name(SDLK_F12, HOTKEY_SHIFT | HOTKEY_CTRL, name, sizeof name);
    assert(!strcmp(name, "Ctrl+Shift+F12"));
    hotkey_name(SDLK_A, HOTKEY_GUI | HOTKEY_ALT, name, sizeof name);
    assert(!strcmp(name, "Alt+GUI+A"));
    hotkey_name(SDLK_RETURN, HOTKEY_ALT, name, sizeof name);
    assert(!strcmp(name, "Alt+Return"));
    hotkey_name(0, HOTKEY_CTRL, name, sizeof name);
    assert(!strcmp(name, ""));
    /* what isn't a key: a name SDL doesn't know (a modifier word it doesn't take, "Super", is one), nothing after the
       modifiers, a modifier where the key goes */
    assert(parse("Ctrl+Foo") == -1 && strstr(g.why, "no key is named \"Foo\"") && g.key == 0);
    assert(parse("Super+F1") == -1 && strstr(g.why, "no key is named \"Super+F1\""));
    assert(parse("Ctrl+") == -1 && !strcmp(g.why, "no key after Ctrl+"));
    assert(parse("Ctrl+Left Shift") == -1 &&
           !strcmp(g.why, "Left Shift is a modifier, which goes with a key (Ctrl+F1)"));
    assert(hotkey_modifier(SDLK_LCTRL) && hotkey_modifier(SDLK_RGUI) && hotkey_modifier(SDLK_MODE));
    assert(!hotkey_modifier(SDLK_F1) && !hotkey_modifier(SDLK_CAPSLOCK));

    /* the defaults' actions: exactly the key and its modifiers, else with Shift held the key without it */
    assert(find(SDLK_F1, 0) == HOTKEY_HELP && g.also == -1 && find(SDLK_F2, 0) == HOTKEY_MENU);
    assert(find(SDLK_F12, 0) == HOTKEY_STATS && find(SDLK_F12, HOTKEY_SHIFT) == HOTKEY_STATS);
    assert(find(SDLK_F12, HOTKEY_CTRL | HOTKEY_SHIFT) == HOTKEY_CAPTURE && find(SDLK_F12, HOTKEY_CTRL) == -1);
    assert(find(SDLK_F4, HOTKEY_ALT) == -1 && find(SDLK_F4, 0) == HOTKEY_FPS_LIMIT); /* Alt+F4 is the window's */
    assert(find(SDLK_RETURN, HOTKEY_ALT) == HOTKEY_FULLSCREEN_2 && find(SDLK_RETURN, 0) == -1);
    assert(find(SDLK_F11, 0) == HOTKEY_FULLSCREEN && find(SDLK_ESCAPE, HOTKEY_SHIFT) == HOTKEY_QUIT);
    assert(find(SDLK_G, HOTKEY_CTRL) == HOTKEY_MOUSE && find(SDLK_G, 0) == -1); /* Ctrl+G, hotkey_mouse's default */
    assert(find(SDLK_PAUSE, 0) == HOTKEY_PAUSE && find(0, 0) == -1 && find(SDLK_X, 0) == -1);
    assert(find(SDLK_D, HOTKEY_CTRL) == HOTKEY_TV_MODE && find(SDLK_D, 0) == -1); /* Ctrl+D, hotkey_tv_mode's */

    /* read from the .ini: said back as SDL names it, and the old key no more */
    set(HOTKEY_SCREENSHOT, "ctrl+s");
    assert(!strcmp(hotkey_text(HOTKEY_SCREENSHOT), "Ctrl+S") && !strcmp(g_settings.screenshot_shortcut, "Ctrl+S"));
    assert(find(SDLK_S, HOTKEY_CTRL) == HOTKEY_SCREENSHOT && find(SDLK_F3, 0) == -1);
    /* not a key: refused, with the value given and the one in effect, which differs from the default */
    g.logged[0] = 0;
    set(HOTKEY_SCREENSHOT, "Ctrl+Nope");
    assert(!strcmp(g.logged, "settings: hotkey_screenshot=Ctrl+Nope: no key is named \"Nope\" (a key as SDL names "
                             "it, after Ctrl+, Alt+, Shift+ or GUI+): it keeps Ctrl+S\n"));
    assert(!strcmp(hotkey_text(HOTKEY_SCREENSHOT), "Ctrl+S") && find(SDLK_S, HOTKEY_CTRL) == HOTKEY_SCREENSHOT);
    /* empty: no key, and a refusal then says so */
    set(HOTKEY_SCREENSHOT, "");
    assert(!strcmp(hotkey_text(HOTKEY_SCREENSHOT), "") && find(SDLK_S, HOTKEY_CTRL) == -1);
    set(HOTKEY_SCREENSHOT, "Shift+");
    assert(!strcmp(g.logged, "settings: hotkey_screenshot=Shift+: no key after Shift+: it keeps \"\" (no key)\n"));
    assert(!strcmp(hotkey_text(HOTKEY_SCREENSHOT), ""));
    /* a key another action has: the first in order does it, the other said */
    set(HOTKEY_HELP, "F5");
    assert(find(SDLK_F5, 0) == HOTKEY_HELP && g.also == HOTKEY_THREAD_DUMP && find(SDLK_F1, 0) == -1);
    /* a key set with Shift beats the key without it, which Shift alone no longer reaches */
    set(HOTKEY_CAPTURE, "Shift+F12");
    assert(find(SDLK_F12, HOTKEY_SHIFT) == HOTKEY_CAPTURE && g.also == -1 && find(SDLK_F12, 0) == HOTKEY_STATS);
    assert(find(SDLK_F12, HOTKEY_CTRL | HOTKEY_SHIFT) == -1);
    /* backend_ini's way in: the Shortcuts' keys, no other */
    assert(hotkeys_ini("hotkey_quit", "Q") == 1 && find(SDLK_Q, 0) == HOTKEY_QUIT && find(SDLK_ESCAPE, 0) == -1);
    assert(hotkeys_ini("hotkey_nope", "F1") == 0 && hotkeys_ini("stats", "1") == 0);
    /* the quit key asks twice: a first press warns, a second within 3 s quits, one 3 s or more after a first warns
       again; the warning names the key and the time */
    {
        char warning[64];
        const uint64_t again = SNX_QUIT_AGAIN_NS;
        assert(again == 3000000000ull);
        assert(!hotkey_quit_press(1000) && hotkey_quit_press(1000 + again - 1));
        assert(!hotkey_quit_press(10 * again) && !hotkey_quit_press(11 * again) && hotkey_quit_press(11 * again + 1));
        snx_quit_warning(warning, sizeof warning, "Escape");
        assert(!strcmp(warning, "Press Escape again within 3 seconds to quit"));
    }
    puts("hotkeys passed");
    return 0;
}
