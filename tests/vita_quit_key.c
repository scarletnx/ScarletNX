/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vita window's Esc (gpu/present.c), as SDL sends it: a first press only warns, over the game too, and a second
   ends the game at the vblank that reads it; held with Ctrl, Alt or the system key it is no press, nor is a key's
   repeat. The upscaler's and the anti-aliasing's shortcuts (the Switch's keys, Ctrl+U and Ctrl+Shift+A) step them and
   say the mode over the game. A vblank with no framebuffer draws nothing, so no device or window is opened */
#include "vita/gpu/present.h"
#include "vita/gpu/vk.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e)                                                                                                       \
    do {                                                                                                               \
        if (!(e)) {                                                                                                    \
            fprintf(stderr, "vita_quit_key.c:%d: check failed: %s\n", __LINE__, #e);                                   \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static VitaProcess g_process; /* no framebuffer: the vblank reads the events only */

/* Esc down with mod, a repeat or not, read at a vblank: whether the game goes on */
static bool esc(SDL_Keymod mod, bool repeat) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = SDLK_ESCAPE;
    e.key.scancode = SDL_SCANCODE_ESCAPE;
    e.key.mod = mod;
    e.key.down = true;
    e.key.repeat = repeat;
    CHECK(SDL_PushEvent(&e));
    return vita_present_vblank(&g_process);
}

/* key down with mod, read at a vblank: the line over the game after it */
static const char* press(SDL_Keycode key, SDL_Keymod mod) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = key;
    e.key.mod = mod;
    e.key.down = true;
    CHECK(SDL_PushEvent(&e) && vita_present_vblank(&g_process));
    return vita_vk_notice_text();
}

int main(void) {
    CHECK(SDL_Init(SDL_INIT_EVENTS));
    CHECK(vita_present_vblank(&g_process) && !strcmp(vita_vk_notice_text(), ""));
    CHECK(!strcmp(press(SDLK_U, SDL_KMOD_NONE), ""));
    CHECK(!strcmp(press(SDLK_U, SDL_KMOD_LCTRL), "Upscaler: Bilinear"));
    CHECK(!strcmp(press(SDLK_A, SDL_KMOD_RCTRL | SDL_KMOD_RSHIFT), "Anti-aliasing: Auto"));
    CHECK(esc(SDL_KMOD_LCTRL, false) && esc(SDL_KMOD_LALT, false) && esc(SDL_KMOD_LGUI, false));
    CHECK(esc(SDL_KMOD_NONE, false)); /* the first: a warning */
    CHECK(!strcmp(vita_vk_notice_text(), "Press Esc again within 3 seconds to quit"));
    CHECK(esc(SDL_KMOD_NONE, true) && esc(SDL_KMOD_LCTRL, false));
    CHECK(!esc(SDL_KMOD_NONE, false)); /* the second */
    SDL_Quit();
    puts("vita_quit_key: passed");
    return 0;
}
