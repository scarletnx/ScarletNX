/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS window's runtime keys (gpu/present.c), as every console's (common/utilities/vpad_sdl.h) and as SDL sends
   them: the quit key's first press only warns, over the game too, and a second closes the window, held with Ctrl, Alt
   or the system key it is no press, nor is a key's repeat; F4 turns the clock's pace off and back (board/clock.h); F7
   asks the run for a save between two dispatches. A plain window under SDL's offscreen driver, no device */
#include "3ds/gpu/present.h"
#include "3ds/board/clock.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/pace.h"
#include "common/utilities/vpad_consoles.h"
#include "common/utilities/vpad_sdl.h"
#include "support/vpad_window.h"
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

static SDL_Window* g_window;
/* key down with mod, a repeat or not, in the window, pumped as the presenter's loop pumps it: whether it closed */
static bool press(SDL_Keycode key, SDL_Keymod mod, bool repeat) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = key;
    e.key.scancode = SDL_GetScancodeFromKey(key, NULL);
    e.key.windowID = SDL_GetWindowID(g_window);
    e.key.mod = mod;
    e.key.down = true;
    e.key.repeat = repeat;
    CHECK(SDL_PushEvent(&e));
    return snx_ctr_present_closed();
}
static bool esc(SDL_Keymod mod, bool repeat) { return press(SDLK_ESCAPE, mod, repeat); }

int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) || !(g_window = SDL_CreateWindow("3ds_quit_key", 400, 480, 0))) {
        printf("3ds_quit_key: no window under SDL's offscreen driver here (%s): skipped\n", SDL_GetError());
        return 77;
    }
    snx_ctr_present_keys(g_window, NULL);
    CHECK(!snx_ctr_present_closed() && !strcmp(snx_ctr_present_notice(), ""));
    /* F4: the clock at no pace, then back to the console's, each said over the game */
    CHECK(snx_ctr_clock_pace() == SNX_PACE_CONSOLE && !press(SDLK_F4, SDL_KMOD_NONE, false));
    CHECK(snx_ctr_clock_pace() == SNX_PACE_NONE && !strcmp(snx_ctr_present_notice(), "FPS limit: off"));
    CHECK(!press(SDLK_F4, SDL_KMOD_NONE, false) && snx_ctr_clock_pace() == SNX_PACE_CONSOLE);
    /* F7: a save asked of the run, once */
    CHECK(snx_ctr_present_state_ask() < 0 && !press(SDLK_F7, SDL_KMOD_NONE, false));
    CHECK(snx_ctr_present_state_ask() == HOTKEY_SAVE_STATE && snx_ctr_present_state_ask() < 0);
    /* the upscaler's and the anti-aliasing's keys: the next value for the run, said over the game */
    CHECK(!press(SDLK_U, SDL_KMOD_LCTRL, false));
    CHECK(!strncmp(snx_ctr_present_notice(), "Upscaler: ", 10));
    {
        char first[64];
        snprintf(first, sizeof first, "%s", snx_ctr_present_notice());
        CHECK(!press(SDLK_U, SDL_KMOD_LCTRL, false) && strcmp(snx_ctr_present_notice(), first) &&
              !strncmp(snx_ctr_present_notice(), "Upscaler: ", 10));
    }
    CHECK(!press(SDLK_A, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT, false) &&
          !strncmp(snx_ctr_present_notice(), "Anti-aliasing: ", 15));
    /* F5: the threads listed by the run, once */
    CHECK(!snx_ctr_present_dump_asked() && !press(SDLK_F5, SDL_KMOD_NONE, false));
    CHECK(snx_ctr_present_dump_asked() && !snx_ctr_present_dump_asked());
    { /* a finger on the controls' A in the window presses it; the same place in the lower screen's own window, none */
        SDL_Window* lower = SDL_CreateWindow("3ds_quit_key lower", 400, 480, 0);
        u64 held = 0;
        i32 sticks[4];
        int w = 0, h = 0;
        CHECK(lower && SDL_GetWindowSizeInPixels(g_window, &w, &h));
        vpad_console(&snx_vpad_3ds);
        g_settings.show_controls = 1;
        CHECK(snx_vpad_window_rects(w, h)); /* laid out for the window */
        float x = vpad_config()->at[SNX_VPAD_A].x * (float)w, y = vpad_config()->at[SNX_VPAD_A].y * (float)h;
        CHECK(vpad_test_finger(lower, SDL_EVENT_FINGER_DOWN, 77, 9, x, y));
        (void)snx_ctr_present_closed();
        snx_vpad_window_state(&held, sticks);
        CHECK(held == 0);
        CHECK(vpad_test_finger(lower, SDL_EVENT_FINGER_UP, 77, 9, x, y));
        CHECK(vpad_test_finger(g_window, SDL_EVENT_FINGER_DOWN, 77, 9, x, y));
        (void)snx_ctr_present_closed();
        snx_vpad_window_state(&held, sticks);
        CHECK(held == 1);
        CHECK(vpad_test_finger(g_window, SDL_EVENT_FINGER_UP, 77, 9, x, y));
        (void)snx_ctr_present_closed();
        SDL_DestroyWindow(lower);
    }
    snx_ctr_present_free();
    snx_ctr_present_keys(g_window, NULL); /* the notice gone with the keys' state */
    CHECK(!esc(SDL_KMOD_LCTRL, false) && !esc(SDL_KMOD_LALT, false) && !esc(SDL_KMOD_LGUI, false));
    CHECK(!strcmp(snx_ctr_present_notice(), "")); /* no press, no notice */
    CHECK(!esc(SDL_KMOD_NONE, false)); /* the first: a warning */
    CHECK(!strcmp(snx_ctr_present_notice(), "Press Escape again within 3 seconds to quit"));
    CHECK(!esc(SDL_KMOD_NONE, true) && !esc(SDL_KMOD_LCTRL, false));
    CHECK(esc(SDL_KMOD_NONE, false)); /* the second */
    snx_ctr_present_free();
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    puts("3ds_quit_key: passed");
    return 0;
}
