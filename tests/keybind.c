/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime hotkeys' key names (gpu/vk/upscale.c's live cycle, platform/backend/keybind.c): the modifiers, the key
   SDL names, the spaces a setting may hold, and the event that matches. */
#include "common/utilities/keybind.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void parses(const char* spec, SDL_Keycode key, SDL_Keymod mods) {
    SnxKeybind b;
    assert(snx_keybind_parse(spec, &b));
    assert(b.key == key);
    assert(b.mods == mods);
}
static void refuses(const char* spec) {
    SnxKeybind b;
    assert(!snx_keybind_parse(spec, &b));
}
static int matches(const SnxKeybind* b, SDL_Keycode key, SDL_Keymod mods) {
    SDL_KeyboardEvent e = {.type = SDL_EVENT_KEY_DOWN, .key = key, .mod = mods};
    return snx_keybind_match(b, &e);
}
static void names(SDL_Keycode key, SDL_Keymod mods, int want, const char* text) {
    SDL_KeyboardEvent e = {.type = SDL_EVENT_KEY_DOWN, .key = key, .mod = mods};
    char out[128];
    int r = snx_keybind_name(&e, out, sizeof out);
    assert(r == want);
    if (r == 1) assert(!strcmp(out, text));
}

int main(void) {
    SnxKeybind b;
    parses("ctrl+u", SDLK_U, SDL_KMOD_CTRL);
    parses("Ctrl + U", SDLK_U, SDL_KMOD_CTRL);
    parses("control+u", SDLK_U, SDL_KMOD_CTRL);
    parses("shift+f13", SDLK_F13, SDL_KMOD_SHIFT);
    parses("ctrl+shift+u", SDLK_U, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
    parses("alt+u", SDLK_U, SDL_KMOD_ALT);
    parses("gui+u", SDLK_U, SDL_KMOD_GUI);
    parses("f13", SDLK_F13, SDL_KMOD_NONE);
    parses("u", SDLK_U, SDL_KMOD_NONE);
    refuses("");
    refuses("bogus+u");
    refuses("ctrl+");
    refuses("ctrl++");
    refuses("ctrl+nonsense");

    assert(snx_keybind_parse("ctrl+u", &b));
    assert(matches(&b, SDLK_U, SDL_KMOD_CTRL));
    assert(matches(&b, SDLK_U, SDL_KMOD_LCTRL)); /* an event has the side that is down */
    assert(matches(&b, SDLK_U, SDL_KMOD_RCTRL));
    assert(matches(&b, SDLK_U, SDL_KMOD_LCTRL | SDL_KMOD_CAPS)); /* lock keys don't count */
    assert(!matches(&b, SDLK_U, SDL_KMOD_NONE));
    assert(!matches(&b, SDLK_U, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT));
    assert(!matches(&b, SDLK_Y, SDL_KMOD_LCTRL));

    assert(snx_keybind_parse("f13", &b));
    assert(matches(&b, SDLK_F13, SDL_KMOD_NONE));
    assert(!matches(&b, SDLK_F13, SDL_KMOD_LSHIFT));
    assert(snx_keybind_parse("shift+f13", &b));
    assert(matches(&b, SDLK_F13, SDL_KMOD_LSHIFT));
    assert(matches(&b, SDLK_F13, SDL_KMOD_RSHIFT));

    /* the launcher's capture: the event's text, and that it parses back */
    names(SDLK_U, SDL_KMOD_CTRL, 1, "ctrl+u");
    names(SDLK_U, SDL_KMOD_CTRL | SDL_KMOD_SHIFT, 1, "ctrl+shift+u");
    names(SDLK_F13, SDL_KMOD_SHIFT, 1, "shift+f13");
    names(SDLK_U, SDL_KMOD_NONE, 1, "u");
    names(SDLK_U, SDL_KMOD_CAPS, 1, "u");
    names(SDLK_LCTRL, SDL_KMOD_CTRL, 0, NULL);
    names(SDLK_ESCAPE, SDL_KMOD_NONE, -1, NULL);
    {
        SDL_KeyboardEvent repeat = {.type = SDL_EVENT_KEY_DOWN, .key = SDLK_U, .repeat = 1};
        char out[128];
        assert(snx_keybind_name(&repeat, out, sizeof out) == 0);
    }
    assert(snx_keybind_parse("ctrl+shift+u", &b) &&
           matches(&b, SDLK_U, SDL_KMOD_LCTRL | SDL_KMOD_RSHIFT));
    { /* what the launcher captured is what the runtime matches, with the left Ctrl a real event has */
        SDL_KeyboardEvent e = {.type = SDL_EVENT_KEY_DOWN, .key = SDLK_T, .mod = SDL_KMOD_LCTRL};
        char out[128];
        assert(snx_keybind_name(&e, out, sizeof out) == 1);
        assert(!strcmp(out, "ctrl+t"));
        assert(snx_keybind_parse(out, &b));
        assert(snx_keybind_match(&b, &e));
    }

    puts("keybind: ok");
    return 0;
}
