/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA runner's window (src/agb/runtime/host.c) under SDL's dummy drivers: it presents with vsync on, so a frame
   never goes on screen half drawn. */
#include "agb/runtime/host.h"
#include "snx_settings.h"
#include <SDL3/SDL.h>
#include <stdio.h>

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
    {
        /* the runtime's keys are the runtime's: Ctrl+Shift+A cycles the anti aliasing and does not reach the game as
           an A press, while a plain A does */
        SDL_Event e = {0};
        uint16_t keys = 0;
        /* X is the A button here, so a hotkey on Ctrl+Shift+X shows the event being taken before the game's map */
        snprintf(g_settings.anti_aliasing_key, sizeof g_settings.anti_aliasing_key, "ctrl+shift+x");
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = SDLK_X;
        e.key.scancode = SDL_SCANCODE_X;
        e.key.mod = SDL_KMOD_CTRL | SDL_KMOD_SHIFT;
        SDL_PushEvent(&e);
        agb_host_poll(host, &keys);
        if (keys & AGB_KEY_A) {
            fprintf(stderr, "agb_host: Ctrl+Shift+X reached the game as A\n");
            agb_host_close(host);
            return 1;
        }
        e.key.mod = 0;
        SDL_PushEvent(&e);
        agb_host_poll(host, &keys);
        if (!(keys & AGB_KEY_A)) {
            fprintf(stderr, "agb_host: a plain X did not reach the game\n");
            agb_host_close(host);
            return 1;
        }
    }
    agb_host_close(host);
    puts("agb_host: passed");
    return 0;
}
