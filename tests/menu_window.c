/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu in a console's window (common/utilities/menu_window.c), a GBA game's: F2 opens it over the picture
   and Escape closes it; it has the console's items alone (no gamepads' page, no TV mode, no recordings of input); its
   Save states page lists the game's slots and a slot's Save is the window's save of that slot, between two frames; a
   setting changed is kept in the game's picks; Exit ends the run. A plain window under SDL's offscreen driver */
#include "common/utilities/menu_window.h"
#include "common/utilities/common.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/overlay.h"
#include "common/utilities/state_header.h"
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
            fprintf(stderr, "menu_window.c:%d: check failed: %s\n", __LINE__, #e);                                     \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static SDL_Window* g_window;
static char g_saved[1024]; /* the path the window's save wrote */
static void say(const char* line) { (void)line; }
static int save(void* game, const char* path, const uint8_t* thumb, char* why, size_t whysize) {
    (void)game, (void)thumb, (void)why, (void)whysize;
    snprintf(g_saved, sizeof g_saved, "%s", path);
    return 0;
}
static int load(void* game, const char* path, char* why, size_t whysize) {
    (void)game, (void)path;
    snprintf(why, whysize, "no game");
    return 1;
}
/* the events waiting, as a console's loop takes them: the last one's answer, its action into *action */
static SnxVpadEvent pump(int* action) {
    SDL_Event e;
    SnxVpadEvent last = SNX_VPAD_PASS;
    *action = -1;
    while (SDL_PollEvent(&e)) {
        int a = -1;
        SnxVpadEvent took = snx_vpad_window_event(&e, &a);
        if (took != SNX_VPAD_PASS) last = took;
        if (took == SNX_VPAD_ACTION) *action = a;
    }
    return last;
}
static SnxVpadEvent key(SDL_Keycode k, int* action) {
    CHECK(vpad_test_key(g_window, k));
    return pump(action);
}
int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) || !(g_window = SDL_CreateWindow("menu_window", 960, 640, 0))) {
        printf("menu_window: no window under SDL's offscreen driver here (%s): skipped\n", SDL_GetError());
        return 77;
    }
    char *root = snx_temp_root(), *dir = root ? snx_temp_dir(root) : NULL;
    char *ini = dir ? snx_path_join(dir, "a game.ini") : NULL, *prefix = dir ? snx_format("%s/", dir) : NULL;
    CHECK(ini && prefix && !snx_write_file(ini, "[Game]\nsystem=gba\n", 18));
    vpad_console(&snx_vpad_gba);
    snx_vpad_window_open(g_window, ini, say, say);
    snx_menu_window_open(SNX_SYSTEM_GBA, dir, "a game");
    int action, w = 0, h = 0;
    CHECK(SDL_GetWindowSizeInPixels(g_window, &w, &h));
    const SnxUiRects* r = snx_vpad_window_rects(w, h);
    size_t closed = r ? r->count : 0;
    /* F2 opens it over the picture, Escape closes it */
    CHECK(key(SDLK_F2, &action) == SNX_VPAD_TAKEN && overlay_shown() && !vpad_editing());
    r = snx_vpad_window_rects(w, h);
    CHECK(r && r->count > closed);
    CHECK(key(SDLK_ESCAPE, &action) == SNX_VPAD_TAKEN && !overlay_shown());
    /* its items: Controls, Save states, Shortcuts, Exit (no gamepads' page, no TV mode, no capture, no settings) */
    CHECK(key(SDLK_F2, &action) == SNX_VPAD_TAKEN && overlay_shown());
    CHECK(key(SDLK_DOWN, &action) == SNX_VPAD_TAKEN); /* Save states, second */
    CHECK(key(SDLK_RETURN, &action) == SNX_VPAD_TAKEN && overlay_states_shown());
    (void)snx_vpad_window_rects(w, h); /* the page's slots read from the folder */
    /* slot 1 (the auto slot is 0): its page, Save first, the window's save of that slot asked */
    CHECK(key(SDLK_DOWN, &action) == SNX_VPAD_TAKEN);
    CHECK(key(SDLK_RETURN, &action) == SNX_VPAD_TAKEN);
    CHECK(key(SDLK_RETURN, &action) == SNX_VPAD_ACTION && action == HOTKEY_SAVE_STATE);
    SnxVpadStates states = {prefix, "a game", NULL, save, load};
    snx_vpad_window_states(action, &states, NULL);
    CHECK(strstr(g_saved, "states/a game.1" SNX_STATE_EXT));
    /* Exit, the main page's last: the run ends */
    CHECK(key(SDLK_ESCAPE, &action) != SNX_VPAD_QUIT);
    for (int i = 0; i < 4 && overlay_shown(); ++i) (void)key(SDLK_ESCAPE, &action);
    CHECK(key(SDLK_F2, &action) == SNX_VPAD_TAKEN && overlay_shown());
    CHECK(key(SDLK_UP, &action) == SNX_VPAD_TAKEN); /* round to the last */
    CHECK(key(SDLK_RETURN, &action) == SNX_VPAD_TAKEN && overlay_shown()); /* asked first, Cancel in focus */
    CHECK(key(SDLK_UP, &action) == SNX_VPAD_TAKEN);
    CHECK(key(SDLK_RETURN, &action) == SNX_VPAD_QUIT);
    snx_menu_window_close();
    snx_vpad_window_close();
    CHECK(!snx_remove_tree(dir));
    free(prefix), free(ini), free(dir), free(root);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    puts("menu_window: a GBA game's menu opens, saves a slot and exits");
    return 0;
}
