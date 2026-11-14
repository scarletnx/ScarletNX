/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS window's Esc (gpu/present.c), as SDL sends it: a first press only warns, over the game too, and a second
   closes the window; held with Ctrl, Alt or the system key it is no press, nor is a key's repeat. The upscaler's and
   the anti-aliasing's shortcuts (the Switch's keys, Ctrl+U and Ctrl+Shift+A) step them and say the mode over the game,
   with either Ctrl, and only on a press. The presenter pumps SDL's events with no device or window, so none is
   opened */
#include "3ds/gpu/present.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e)                                                                                                       \
    do {                                                                                                               \
        if (!(e)) {                                                                                                    \
            fprintf(stderr, "3ds_quit_key.c:%d: check failed: %s\n", __LINE__, #e);                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* Esc down with mod, a repeat or not, pumped as the presenter's loop pumps it: whether the window is closed */
static bool esc(SDL_Keymod mod, bool repeat) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = SDLK_ESCAPE;
    e.key.scancode = SDL_SCANCODE_ESCAPE;
    e.key.mod = mod;
    e.key.down = true;
    e.key.repeat = repeat;
    CHECK(SDL_PushEvent(&e));
    return snx_ctr_present_closed();
}

/* key down with mod, a repeat or not, pumped: the line over the game after it */
static const char* press(SDL_Keycode key, SDL_Keymod mod, bool repeat) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = key;
    e.key.mod = mod;
    e.key.down = true;
    e.key.repeat = repeat;
    CHECK(SDL_PushEvent(&e) && !snx_ctr_present_closed());
    return snx_ctr_present_notice();
}

int main(void) {
    CHECK(SDL_Init(SDL_INIT_EVENTS));
    CHECK(!snx_ctr_present_closed() && !strcmp(snx_ctr_present_notice(), ""));
    CHECK(!strcmp(press(SDLK_U, SDL_KMOD_NONE, false), "")); /* U alone is the game's */
    CHECK(!strcmp(press(SDLK_U, SDL_KMOD_LCTRL, false), "Upscaler: Bilinear"));
    CHECK(!strcmp(press(SDLK_U, SDL_KMOD_RCTRL, false), "Upscaler: AMD FSR 1"));
    CHECK(!strcmp(press(SDLK_U, SDL_KMOD_RCTRL, true), "Upscaler: AMD FSR 1")); /* a repeat steps nothing */
    CHECK(!strcmp(press(SDLK_A, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT, false), "Anti-aliasing: Auto"));
    CHECK(!strcmp(press(SDLK_A, SDL_KMOD_LCTRL, false), "Anti-aliasing: Auto")); /* Ctrl+A is no shortcut */
    CHECK(!esc(SDL_KMOD_LCTRL, false) && !esc(SDL_KMOD_LALT, false) && !esc(SDL_KMOD_LGUI, false));
    CHECK(!esc(SDL_KMOD_NONE, false)); /* the first: a warning */
    CHECK(!strcmp(snx_ctr_present_notice(), "Press Esc again within 3 seconds to quit"));
    CHECK(!esc(SDL_KMOD_NONE, true) && !esc(SDL_KMOD_LCTRL, false));
    CHECK(esc(SDL_KMOD_NONE, false)); /* the second */
    SDL_Quit();
    puts("3ds_quit_key: passed");
    return 0;
}
