/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A console's window to fullscreen and back (common/utilities/window_fullscreen.h) under SDL's dummy video: the
   window's flag follows, asking for what it is already is no change, and a refusal says the system's reason */
#include "common/utilities/window_fullscreen.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "window_fullscreen: %s:%d: %s\n", __FILE__, __LINE__, #c);                                 \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static bool full(SDL_Window* w) { return SDL_SyncWindow(w), (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0; }

int main(void) {
    char why[256] = "";
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window* w = SDL_CreateWindow("fullscreen", 320, 240, 0);
    CHECK(w && !full(w));
    CHECK(snx_window_fullscreen(w, false, why, sizeof why) && !full(w) && !why[0]); /* windowed already */
    CHECK(snx_window_fullscreen(w, true, why, sizeof why) && full(w));
    CHECK(snx_window_fullscreen(w, true, why, sizeof why) && full(w) && !why[0]);
    CHECK(snx_window_fullscreen(w, false, why, sizeof why) && !full(w));
    /* no window: SDL refuses, and its reason is said after what was asked */
    SDL_ClearError();
    CHECK(!snx_window_fullscreen(NULL, true, why, sizeof why));
    char want[256];
    snprintf(want, sizeof want, "the window cannot go fullscreen: %s", SDL_GetError());
    CHECK(SDL_GetError()[0] && !strcmp(why, want));
    static const char back[] = "the window cannot go back from fullscreen: ";
    CHECK(!snx_window_fullscreen(NULL, false, why, sizeof why) && !strncmp(why, back, sizeof back - 1));
    SDL_DestroyWindow(w);
    SDL_Quit();
    printf("window_fullscreen: passed (%s)\n", why);
    return 0;
}
