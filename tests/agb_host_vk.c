/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA runner's Vulkan window (src/agb/runtime/vk.c) under SDL's offscreen driver, whose Vulkan surface has no
   display: the shortcuts step the upscaler round its five modes and the anti-aliasing round its six, each said over the
   game as a Switch game says it, and a few frames go through every one. Skipped (77) where no Vulkan window opens */
#include "agb/runtime/host.h"
#include "agb/ppu/ppu.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* key down with mod, read by a poll */
static bool press(AgbHost* host, SDL_Keycode key, SDL_Keymod mod) {
    SDL_Event e = {0};
    uint16_t keys;
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = key;
    e.key.mod = mod;
    e.key.down = true;
    return SDL_PushEvent(&e) && agb_host_poll(host, &keys);
}

int main(void) {
    static const char* const upscalers[5] = {"Upscaler: Bilinear", "Upscaler: AMD FSR 1", "Upscaler: NVIDIA NIS",
                                             "Upscaler: Qualcomm SGSR", "Upscaler: Off"};
    static const char* const aas[6] = {"Anti-aliasing: Auto",    "Anti-aliasing: FXAA",    "Anti-aliasing: SMAA",
                                       "Anti-aliasing: SSAA 2x", "Anti-aliasing: SSAA 4x", "Anti-aliasing: Off"};
    static uint32_t frame[AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT];
    static const int16_t silence[2] = {0};
    char error[256] = {0};
    AgbHost* host;
    int bad = 0;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    host = agb_host_open("agb_host_vk", false, error, sizeof error);
    if (!host) {
        fprintf(stderr, "agb_host_vk: opening the window: %s\n", error);
        return 1;
    }
    if (!press(host, SDLK_U, SDL_KMOD_LCTRL) || strcmp(agb_host_notice(host), upscalers[0])) {
        printf("agb_host_vk: no Vulkan window here (%s): skipped\n", agb_host_notice(host));
        agb_host_close(host);
        return 77;
    }
    for (size_t i = 0; i < sizeof frame / sizeof *frame; ++i) frame[i] = 0xFF000000u | (uint32_t)(i * 2654435761u >> 8);
    for (int k = 0; k < 11; ++k) {
        const char* want = k < 5 ? upscalers[k] : aas[k - 5];
        if (k && !press(host, k < 5 ? SDLK_U : SDLK_A, k < 5 ? SDL_KMOD_RCTRL : SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT))
            return 1;
        if (strcmp(agb_host_notice(host), want)) {
            fprintf(stderr, "agb_host_vk: step %d says \"%s\", not \"%s\"\n", k, agb_host_notice(host), want);
            ++bad;
        }
        for (int f = 0; f < 3; ++f) agb_host_frame(host, frame, silence, 0);
    }
    agb_host_close(host);
    if (bad) return 1;
    puts("agb_host_vk: passed");
    return 0;
}
