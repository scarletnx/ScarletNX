/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA runner's window (src/agb/runtime/host.c) under SDL's dummy drivers: it presents with vsync on, so a frame
   never goes on screen half drawn. Esc closes it on a second press, a first only warning, in a line over the game
   too, read back from the renderer: the band of common/utilities/notice.h in the window's own pixels, the frame
   around it; held with Ctrl, Alt or the system key it is no press, nor is a key's repeat */
#include "agb/runtime/host.h"
#include "agb/ppu/ppu.h"
#include "common/utilities/notice.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* the frame drawn under the warning, read back before it is shown: how many pixels differ from the band where the
   notice goes and from the frame's colour elsewhere; -1 when it cannot be read */
static long notice_read(AgbHost* host, SDL_Renderer* renderer, SDL_Window* window) {
    static uint32_t frame[AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT];
    static const unsigned char ground[3] = {0x10, 0x20, 0x30};
    SnxNotice n = {0};
    int w, h, rect[4], scale;
    long bad = 0;
    for (size_t i = 0; i < sizeof frame / sizeof *frame; ++i) frame[i] = 0xFF302010u; /* R 0x10, G 0x20, B 0x30 */
    agb_host_draw(host, frame);
    /* the whole window, as its pixels are: the notice is drawn in them, not in the screen's scaled 240x160 */
    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_Surface* read = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface* got = read ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA32) : NULL;
    SDL_DestroySurface(read);
    if (!got || !SDL_GetCurrentRenderOutputSize(renderer, &w, &h) || got->w != w || got->h != h) {
        fprintf(stderr, "agb_host: the window's pixels cannot be read: %s\n", SDL_GetError());
        SDL_DestroySurface(got);
        return -1;
    }
    snx_notice_show(&n, "Press Esc again within 3 seconds to quit", 0);
    scale = snx_notice_scale(w, h, snx_display_percent(SDL_GetWindowDisplayScale(window)));
    unsigned char* band =
        snx_notice_rect(&n, w, h, scale, rect) ? malloc((size_t)rect[2] * (size_t)rect[3] * 4u) : NULL;
    if (!band) {
        SDL_DestroySurface(got);
        return -1;
    }
    snx_notice_pixels(&n, scale, 0, rect, band, (size_t)rect[2] * 4u);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* p =
                (const unsigned char*)got->pixels + (size_t)y * (size_t)got->pitch + (size_t)x * 4u;
            if (x >= rect[0] && x < rect[0] + rect[2] && y >= rect[1] && y < rect[1] + rect[3])
                bad += memcmp(p, band + ((size_t)(y - rect[1]) * (size_t)rect[2] + (size_t)(x - rect[0])) * 4u, 4) != 0;
            else
                bad += memcmp(p, ground, 3) != 0;
        }
    printf("agb_host: the warning over a %dx%d window: a %dx%d band at %d,%d, %ld pixels wrong\n", w, h, rect[2],
           rect[3], rect[0], rect[1], bad);
    free(band);
    SDL_DestroySurface(got);
    return bad;
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
    SDL_Window* window = windows[0];
    SDL_free(windows);
    if (!esc(host, SDL_KMOD_LCTRL, false) || !esc(host, SDL_KMOD_LALT, false) || !esc(host, SDL_KMOD_LGUI, false) ||
        !esc(host, SDL_KMOD_NONE, false)) {
        fprintf(stderr, "agb_host: Esc's first press closed the window\n");
        agb_host_close(host);
        return 1;
    }
    if (notice_read(host, SDL_GetRenderer(window), window) != 0) {
        agb_host_close(host);
        return 1;
    }
    if (!esc(host, SDL_KMOD_NONE, true) || !esc(host, SDL_KMOD_LCTRL, false) || esc(host, SDL_KMOD_NONE, false)) {
        fprintf(stderr, "agb_host: Esc did not close the window on its second press, and only then\n");
        agb_host_close(host);
        return 1;
    }
    agb_host_close(host);
    puts("agb_host: passed");
    return 0;
}
