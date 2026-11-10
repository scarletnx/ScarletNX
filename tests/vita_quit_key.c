/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vita window's Esc (gpu/present.c), as SDL sends it: a first press only warns and a second ends the game at the
   vblank that reads it; held with Ctrl, Alt or the system key it is no press, nor is a key's repeat. A vblank with no
   framebuffer draws nothing, so no device or window is opened */
#include "vita/gpu/present.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(void) {
    CHECK(SDL_Init(SDL_INIT_EVENTS));
    CHECK(vita_present_vblank(&g_process));
    CHECK(esc(SDL_KMOD_LCTRL, false) && esc(SDL_KMOD_LALT, false) && esc(SDL_KMOD_LGUI, false));
    CHECK(esc(SDL_KMOD_NONE, false)); /* the first: a warning */
    CHECK(esc(SDL_KMOD_NONE, true) && esc(SDL_KMOD_LCTRL, false));
    CHECK(!esc(SDL_KMOD_NONE, false)); /* the second */
    SDL_Quit();
    puts("vita_quit_key: passed");
    return 0;
}
