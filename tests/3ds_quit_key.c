/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS window's Esc and its shortcuts (gpu/present.c), as SDL sends them: a first press only warns, over the game
   too, and a second closes the window; held with Ctrl, Alt or the system key it is no press, nor is a key's repeat. The
   upscaler's and the anti-aliasing's shortcuts (the Switch's keys, Ctrl+U and Ctrl+Shift+A) step them and say the mode
   over the game, with either Ctrl, and only on a press; Ctrl+F3 steps the one window's screen layout round its four and
   Ctrl+Shift+F3 turns the screens' swap on and off, said with them, in the same line; with Separate screens neither
   moves, and the line says so. The presenter pumps SDL's events with no device or window, except for that check, where
   the host gives the two screens a window each */
#include "3ds/gpu/present.h"
#include "3ds/gpu/vk/vk.h"
#include "snx_settings.h"
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

/* Ctrl+F3 and Ctrl+Shift+F3 with the screens in separate windows: neither setting moves, and the line over the game
   says why. The windows come up as tests/3ds_windows.c's Separate screens do; a host with no memory for the presenter,
   whose Vulkan device does not come up or that gives the second screen no window has nothing to check here, and the
   message says which */
static void separate_layout(void) {
    SnxCtrMemory* memory = snx_ctr_memory_new(SNX_CTR_OLD);
    if (!memory) {
        printf("no memory for the presenter: the layout keys with separate screens cannot be checked here\n");
        return;
    }
    g_settings.ctr_separate_screens = 1;
    bool up = snx_ctr_present_init(memory, NULL);
    bool apart = up && snx_ctr_present_separate();
    if (apart) {
        int layout = g_settings.ctr_screen_layout, swap = g_settings.ctr_screen_swap;
        const char* line = "Separate screens: no layout to change";
        CHECK(!strcmp(press(SDLK_F3, SDL_KMOD_LCTRL, false), line));
        CHECK(g_settings.ctr_screen_layout == layout && g_settings.ctr_screen_swap == swap);
        CHECK(!strcmp(press(SDLK_F3, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT, false), line));
        CHECK(g_settings.ctr_screen_layout == layout && g_settings.ctr_screen_swap == swap);
    } else if (!up)
        printf("no Vulkan device for the presenter: the layout keys with separate screens cannot be checked here\n");
    else
        printf("no second window on this host: the layout keys with separate screens cannot be checked here\n");
    if (up)
        snx_ctr_present_free();
    else
        snx_ctr_vk_free();
    g_settings.ctr_separate_screens = 0;
    snx_ctr_memory_free(memory);
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
    /* with the screens in a window each the layout keys leave them alone, the line saying so */
    separate_layout();
    /* the one window's layout (Ctrl+F3) round its four, the swap (Ctrl+Shift+F3) on and off, the settings and the line
       over the game each said */
    CHECK(!strcmp(press(SDLK_F3, SDL_KMOD_LCTRL, false), "Screen layout: Single screen"));
    CHECK(g_settings.ctr_screen_layout == SNX_CTR_LAYOUT_SINGLE);
    CHECK(!strcmp(press(SDLK_F3, SDL_KMOD_RCTRL, false), "Screen layout: Large screen"));
    CHECK(!strcmp(press(SDLK_F3, SDL_KMOD_LCTRL, false), "Screen layout: Side by side"));
    CHECK(!strcmp(press(SDLK_F3, SDL_KMOD_LCTRL, false), "Screen layout: Stacked")); /* round, to the first */
    CHECK(g_settings.ctr_screen_layout == SNX_CTR_LAYOUT_STACKED);
    CHECK(!strcmp(press(SDLK_F3, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT, false), "Screen swap: On"));
    CHECK(g_settings.ctr_screen_swap == 1);
    CHECK(!strcmp(press(SDLK_F3, SDL_KMOD_LCTRL, false), "Screen layout: Single screen (swapped)"));
    CHECK(!strcmp(press(SDLK_F3, SDL_KMOD_LCTRL | SDL_KMOD_RSHIFT, false), "Screen swap: Off"));
    CHECK(g_settings.ctr_screen_swap == 0);
    /* F3 alone is the game's, and a repeat, Alt or the system key with Ctrl is no step: the settings say it, and not
       the line over the game, whose three seconds may be over by the time it is read */
    (void)press(SDLK_F3, SDL_KMOD_NONE, false);
    (void)press(SDLK_F3, SDL_KMOD_LCTRL, true);
    (void)press(SDLK_F3, SDL_KMOD_LCTRL | SDL_KMOD_LALT, false);
    (void)press(SDLK_F3, SDL_KMOD_LCTRL | SDL_KMOD_LGUI, false);
    CHECK(g_settings.ctr_screen_layout == SNX_CTR_LAYOUT_SINGLE && g_settings.ctr_screen_swap == 0);
    CHECK(!esc(SDL_KMOD_LCTRL, false) && !esc(SDL_KMOD_LALT, false) && !esc(SDL_KMOD_LGUI, false));
    CHECK(!esc(SDL_KMOD_NONE, false)); /* the first: a warning */
    CHECK(!strcmp(snx_ctr_present_notice(), "Press Esc again within 3 seconds to quit"));
    CHECK(!esc(SDL_KMOD_NONE, true) && !esc(SDL_KMOD_LCTRL, false));
    CHECK(esc(SDL_KMOD_NONE, false)); /* the second */
    SDL_Quit();
    puts("3ds_quit_key: passed");
    return 0;
}
