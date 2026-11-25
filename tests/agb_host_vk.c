/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA runner's Vulkan window (src/agb/runtime/vk.c). Where it puts the screen in windows of many sizes, held to the
   pixels SDL's renderer draws with the integer scale the runner keeps without Vulkan (host.c): a frame drawn by SDL's
   software renderer is, pixel for pixel, the frame sampled at the window's whole step inside agb_vk_place's rectangle
   and black outside it, as the Vulkan window's blit draws it. Then, under SDL's offscreen driver, whose Vulkan surface
   has no display: frames go on screen with and without a notice over them, before and after the window is resized,
   and the runner opens through Vulkan (no SDL renderer on its window), says Esc's first press over the game and closes
   on the second. Skipped (77) where no Vulkan window opens, after the placement's checks */
#include "agb/ppu/ppu.h"
#include "agb/runtime/host.h"
#include "agb/runtime/vk.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* the screen's frame: a pixel of its own colour each, so a pixel taken from the wrong place shows */
static uint32_t g_frame[AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT];

/* the frame in a w by h window as SDL's software renderer draws it, against the Vulkan window's rectangle: the pixels
   that differ, -1 when SDL could not draw it */
static long placed(int w, int h) {
    int32_t at[4];
    long bad = 0;
    SDL_Surface* target = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* r = target ? SDL_CreateSoftwareRenderer(target) : NULL;
    SDL_Texture* t = r ? SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, AGB_SCREEN_WIDTH,
                                           AGB_SCREEN_HEIGHT)
                       : NULL;
    if (!t || !SDL_UpdateTexture(t, NULL, g_frame, AGB_SCREEN_WIDTH * 4) ||
        !SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST) ||
        !SDL_SetRenderLogicalPresentation(r, AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT,
                                          SDL_LOGICAL_PRESENTATION_INTEGER_SCALE) ||
        !SDL_SetRenderDrawColor(r, 0, 0, 0, 255) || !SDL_RenderClear(r) || !SDL_RenderTexture(r, t, NULL, NULL) ||
        !SDL_RenderPresent(r)) {
        fprintf(stderr, "agb_host_vk: SDL's renderer on a %dx%d surface: %s\n", w, h, SDL_GetError());
        bad = -1;
    }
    agb_vk_place((uint32_t)w, (uint32_t)h, at);
    for (int y = 0; y < h && !bad; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = (const unsigned char*)target->pixels + (size_t)y * (size_t)target->pitch + x * 4;
            uint32_t want = 0xFF000000u; /* black, opaque */
            if (x >= at[0] && x < at[0] + at[2] && y >= at[1] && y < at[1] + at[3])
                want = g_frame[(size_t)((y - at[1]) * AGB_SCREEN_HEIGHT / at[3]) * AGB_SCREEN_WIDTH +
                               (size_t)((x - at[0]) * AGB_SCREEN_WIDTH / at[2])];
            uint32_t got = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
            bad += got != want;
        }
    printf("agb_host_vk: a %dx%d window: the screen at %d,%d %dx%d, %ld pixels unlike SDL's renderer\n", w, h, at[0],
           at[1], at[2], at[3], bad);
    SDL_DestroyTexture(t);
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(target);
    return bad;
}

/* a key down with mod, read by a poll: whether the window stays open */
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
    /* whole steps from 1 to 13 in windows of the screen's aspect and others, odd margins on either side */
    static const int sizes[][2] = {{240, 160},   {241, 161},   {479, 320},  {480, 320},   {481, 323},
                                   {960, 640},   {1001, 641},  {1280, 720}, {1920, 1080}, {1080, 1920},
                                   {2560, 1440}, {3840, 2160}, {300, 1000}, {1000, 300},  {725, 483}};
    static const int16_t silence[2] = {0};
    char error[256] = {0};
    long bad = 0;
    for (size_t i = 0; i < sizeof g_frame / sizeof *g_frame; ++i)
        g_frame[i] = 0xFF000000u | (uint32_t)(i * 2654435761u >> 8);
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        long b = placed(sizes[i][0], sizes[i][1]);
        bad += b < 0 ? 1 : b;
    }
    if (bad) {
        fprintf(stderr, "agb_host_vk: the Vulkan window would not put the screen where SDL's renderer does\n");
        return 1;
    }

    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "agb_host_vk: SDL's offscreen video: %s\n", SDL_GetError());
        return 1;
    }
    AgbVk* vk = agb_vk_open("agb_host_vk", AGB_SCREEN_WIDTH * 4, AGB_SCREEN_HEIGHT * 4, false, error, sizeof error);
    if (!vk) {
        printf("agb_host_vk: the placement passed; no Vulkan window here (%s): the rest skipped\n", error);
        SDL_Quit();
        return 77;
    }
    /* three frames bare, three with a notice; then the window resized, which the next frame's swapchain follows */
    for (int k = 0; k < 12; ++k) {
        SnxNotice n = {0};
        if (k == 6 && (!SDL_SetWindowSize(agb_vk_window(vk), 725, 483) || !SDL_SyncWindow(agb_vk_window(vk))))
            fprintf(stderr, "agb_host_vk: the window could not be resized: %s\n", SDL_GetError());
        if (k % 6 >= 3) snx_notice_show(&n, "Press Escape again within 3 seconds to quit", SDL_GetTicksNS());
        if (!agb_vk_frame(vk, g_frame, &n)) {
            fprintf(stderr, "agb_host_vk: frame %d did not go on screen\n", k);
            ++bad;
        }
    }
    agb_vk_close(vk);
    SDL_Quit(); /* which drops the hints too */

    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    AgbHost* host = agb_host_open("agb_host_vk", NULL, false, error, sizeof error);
    if (!host) {
        fprintf(stderr, "agb_host_vk: opening the runner's window: %s\n", error);
        return 1;
    }
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    if (count != 1 || SDL_GetRenderer(windows[0])) {
        fprintf(stderr, "agb_host_vk: %d windows, %s: not the one Vulkan window\n", count,
                count && SDL_GetRenderer(windows[0]) ? "with SDL's renderer" : "none with a renderer");
        ++bad;
    }
    SDL_free(windows);
    if (!press(host, SDLK_ESCAPE, SDL_KMOD_NONE) ||
        strcmp(agb_host_notice(host), "Press Escape again within 3 seconds to quit")) {
        fprintf(stderr, "agb_host_vk: Esc's first press says \"%s\"\n", agb_host_notice(host));
        ++bad;
    }
    for (int f = 0; f < 3; ++f) agb_host_frame(host, g_frame, silence, 0);
    if (press(host, SDLK_ESCAPE, SDL_KMOD_NONE)) {
        fprintf(stderr, "agb_host_vk: Esc's second press left the window open\n");
        ++bad;
    }
    agb_host_close(host);
    if (bad) return 1;
    puts("agb_host_vk: passed");
    return 0;
}
