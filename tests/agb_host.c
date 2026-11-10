/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA runner's window (src/agb/runtime/host.c) under SDL's dummy drivers: it presents with vsync on, so a frame
   never goes on screen half drawn. Esc closes it on a second press, a first only warning; held with Ctrl, Alt or the
   system key it is no press, nor is a key's repeat */
#include "agb/runtime/host.h"
#include <SDL3/SDL.h>
#include <stdio.h>

/* Esc down with mod, a repeat or not, read by a poll: whether the window stays open */
static bool esc(AgbHost* host, SDL_Keymod mod, bool repeat) {
    SDL_Event e = {0};
    uint16_t keys;
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = SDLK_ESCAPE;
    e.key.scancode = SDL_SCANCODE_ESCAPE;
    e.key.mod = mod;
    e.key.down = true;
    e.key.repeat = repeat;
    return SDL_PushEvent(&e) && agb_host_poll(host, &keys);
}

int main(void) {
    char error[256] = {0};
    AgbHost* host;
    SDL_Window** windows;
    int count = 0, vsync = 0;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    host = agb_host_open("agb_host", false, error, sizeof error);
    if (!host) {
        fprintf(stderr, "agb_host: opening the window: %s\n", error);
        return 1;
    }
    windows = SDL_GetWindows(&count);
    if (count != 1 || !SDL_GetRenderVSync(SDL_GetRenderer(windows[0]), &vsync) || vsync != 1) {
        fprintf(stderr, "agb_host: %d windows, the renderer's vsync %d, not one window with vsync 1\n", count, vsync);
        SDL_free(windows);
        agb_host_close(host);
        return 1;
    }
    SDL_free(windows);
    if (!esc(host, SDL_KMOD_LCTRL, false) || !esc(host, SDL_KMOD_LALT, false) || !esc(host, SDL_KMOD_LGUI, false) ||
        !esc(host, SDL_KMOD_NONE, false) || !esc(host, SDL_KMOD_NONE, true) || !esc(host, SDL_KMOD_LCTRL, false) ||
        esc(host, SDL_KMOD_NONE, false)) {
        fprintf(stderr, "agb_host: Esc did not close the window on its second press, and only then\n");
        agb_host_close(host);
        return 1;
    }
    agb_host_close(host);
    puts("agb_host: passed");
    return 0;
}
