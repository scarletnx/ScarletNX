/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu in a console's window (common/utilities/menu_window.c), a GBA game's: F2 opens it over the picture
   and Escape closes it; it has the console's items alone (no gamepads' page, no TV mode, no recordings of input); its
   Save states page lists the game's slots, with a slot's thumbnail beside it in the window's rectangles, and a slot's
   Save is the window's save of that slot, between two frames; Exit ends the run. A plain window under SDL's offscreen
   driver */
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
/* slot 1's state in the folder: a header and its thumbnail, every texel R G B */
static void state_with_thumb(const char* dir, u8 r, u8 g, u8 b) {
    uint8_t header[SNX_STATE_HEADER] = {0};
    SnxStateHeader h = {.format = SNX_STATE_FORMAT, .thumb_w = SNX_STATE_THUMB_W, .thumb_h = SNX_STATE_THUMB_H};
    snx_state_header_write(header, &h, 0, 0);
    char *states = snx_path_join(dir, "states"),
         *path = states ? snx_format("%s/a game.1" SNX_STATE_EXT, states) : NULL;
    size_t n = (size_t)SNX_STATE_THUMB_W * SNX_STATE_THUMB_H * 4;
    uint8_t* file = malloc(sizeof header + n);
    CHECK(path && file && !snx_mkdirs(states));
    if (file) {
        memcpy(file, header, sizeof header);
        for (size_t i = 0; i < n; i += 4)
            file[sizeof header + i] = r, file[sizeof header + i + 1] = g, file[sizeof header + i + 2] = b,
                                 file[sizeof header + i + 3] = 0xFF;
    }
    CHECK(path && file && !snx_write_file(path, file, sizeof header + n));
    free(file), free(path), free(states);
}
/* the window's rectangles of colour rgba, as a quad keeps it */
static size_t rects_of(int w, int h, u32 rgba) {
    const SnxUiRects* r = snx_vpad_window_rects(w, h);
    size_t n = 0;
    for (size_t i = 0; r && i < r->count; ++i) n += r->rect[i].rgba == ui_rgba(rgba);
    return n;
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
    state_with_thumb(dir, 0x20, 0x80, 0xF8);
    const u32 thumb = 0xFFF88020u; /* its texels' colour, R in the low byte */
    CHECK(key(SDLK_RETURN, &action) == SNX_VPAD_TAKEN && overlay_states_shown());
    (void)snx_vpad_window_rects(w, h); /* the page's slots read from the folder */
    CHECK(rects_of(w, h, thumb) == 0); /* the auto slot in focus, which has no state: "Empty" */
    /* slot 1 (the auto slot is 0): its thumbnail beside the list; its page, Save first, the window's save asked */
    CHECK(key(SDLK_DOWN, &action) == SNX_VPAD_TAKEN);
    (void)snx_vpad_window_rects(w, h); /* the picture given the layer as the focus moved */
    size_t shown = rects_of(w, h, thumb);
    CHECK(shown > 0);
    if (!shown)
        printf("menu_window: slot 1's thumbnail is not among the %zu rectangles\n", snx_vpad_window_rects(w, h)->count);
    CHECK(key(SDLK_RETURN, &action) == SNX_VPAD_TAKEN);
    CHECK(key(SDLK_RETURN, &action) == SNX_VPAD_TAKEN); /* Save over a state: asked first, Cancel in focus */
    CHECK(key(SDLK_UP, &action) == SNX_VPAD_TAKEN);
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
