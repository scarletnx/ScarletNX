/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime hotkeys' key names (gpu/vk/upscale.c's live cycle, platform/backend/keybind.c): the modifiers, the key
   SDL names, the spaces a setting may hold, and the event that matches. */
#include "nx/runtime/platform/backend/keybind.h"
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
    assert(matches(&b, SDLK_U, SDL_KMOD_CTRL | SDL_KMOD_CAPS)); /* lock keys don't count */
    assert(!matches(&b, SDLK_U, SDL_KMOD_NONE));
    assert(!matches(&b, SDLK_U, SDL_KMOD_CTRL | SDL_KMOD_SHIFT));
    assert(!matches(&b, SDLK_Y, SDL_KMOD_CTRL));

    assert(snx_keybind_parse("f13", &b));
    assert(matches(&b, SDLK_F13, SDL_KMOD_NONE));
    assert(!matches(&b, SDLK_F13, SDL_KMOD_SHIFT));

    puts("keybind: ok");
    return 0;
}
