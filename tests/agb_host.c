/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA runner's window (src/agb/runtime/host.c) under SDL's dummy drivers: it presents with vsync on, so a frame
   never goes on screen half drawn. Esc closes it on a second press, a first only warning, in a line over the game
   too, read back from the renderer: the band of common/utilities/notice.h in the window's own pixels, the frame
   around it; held with Ctrl, Alt or the system key it is no press, nor is a key's repeat. The on-screen controls: each
   presses its key under a finger and under the mouse, alone and two at once, a finger off them none; drawn over the
   frame as vpad_rects gives them and nothing else; Auto shows them on a touch screen and hides them as a gamepad
   connects; F2 edits their layout, which the game's .ini keeps and gives back, and so does Android's Back, twice
   quitting */
#include "agb/runtime/host.h"
#include "agb/ppu/ppu.h"
#include "common/utilities/common.h"
#include "common/utilities/notice.h"
#include "common/utilities/vpad_consoles.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/quit_key.h"
#include "common/utilities/vpad_sdl.h"
#include "support/sdl_real_pads.h"
#include "support/vpad_window.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* key down with mod, a repeat or not, read by a poll: whether the window stays open */
static bool press(AgbHost* host, SDL_Keycode key, SDL_Keymod mod, bool repeat) {
    SDL_Event e = {0};
    uint16_t keys;
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = key;
    e.key.scancode = SDL_GetScancodeFromKey(key, NULL);
    e.key.mod = mod;
    e.key.down = true;
    e.key.repeat = repeat;
    return SDL_PushEvent(&e) && agb_host_poll(host, &keys);
}
/* the last line snx_error said */
static char g_said[256];
static void said(const char* line) { snprintf(g_said, sizeof g_said, "%s", line); }
/* the quit key, Esc by default: the runtime's hotkey_quit */
static bool esc(AgbHost* host, SDL_Keymod mod, bool repeat) { return press(host, SDLK_ESCAPE, mod, repeat); }

/* the frame drawn under the notice saying text, read back before it is shown: how many pixels differ from the band
   where the notice goes and from the frame's colour elsewhere; -1 when it cannot be read */
static long notice_read(AgbHost* host, SDL_Renderer* renderer, SDL_Window* window, const char* text) {
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
    snx_notice_show(&n, text, 0);
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

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "agb_host:%d: %s\n", __LINE__, #x);                                                        \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* the keys after the events waiting */
static uint16_t keys(AgbHost* host) {
    uint16_t held = 0;
    return agb_host_poll(host, &held) ? held : 0xFFFF;
}
/* the frame drawn, the on-screen controls over it, as it is read back: how many pixels are not the frame's colour nor
   the controls' rectangles; -1 when it cannot be read */
static long controls_read(AgbHost* host, SDL_Window* window, size_t* count) {
    static uint32_t frame[AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT];
    SDL_Renderer* renderer = SDL_GetRenderer(window);
    int w = 0, h = 0;
    long bad = 0;
    for (size_t i = 0; i < sizeof frame / sizeof *frame; ++i) frame[i] = 0xFF302010u; /* R 0x10, G 0x20, B 0x30 */
    agb_host_draw(host, frame);
    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_Surface* read = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface* got = read ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA32) : NULL;
    SDL_DestroySurface(read);
    uint32_t* want = got ? malloc((size_t)got->w * (size_t)got->h * 4) : NULL;
    if (!want || !SDL_GetCurrentRenderOutputSize(renderer, &w, &h) || got->w != w || got->h != h) {
        SDL_DestroySurface(got);
        return -1;
    }
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) want[i] = frame[0];
    const SnxUiRects* rects = snx_vpad_window_rects(w, h);
    *count = rects ? rects->count : 0;
    for (size_t i = 0; i < *count; ++i)
        for (int y = rects->rect[i].y; y < rects->rect[i].y + rects->rect[i].h; ++y)
            for (int x = rects->rect[i].x; x < rects->rect[i].x + rects->rect[i].w; ++x)
                want[(size_t)y * (size_t)w + (size_t)x] = rects->rect[i].rgba;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = (const unsigned char*)got->pixels + (size_t)y * (size_t)got->pitch + (size_t)x * 4;
            uint32_t pixel = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | 0xFF000000u;
            bad += pixel != want[(size_t)y * (size_t)w + (size_t)x];
        }
    free(want);
    SDL_DestroySurface(got);
    return bad;
}

static int controls(AgbHost* host, SDL_Window* window, const char* dir, const char* ini) {
    static const struct {
        int control;
        uint16_t key;
    } buttons[] = {{SNX_VPAD_A, AGB_KEY_A}, {SNX_VPAD_B, AGB_KEY_B},        {SNX_VPAD_L, AGB_KEY_L},
                   {SNX_VPAD_R, AGB_KEY_R}, {SNX_VPAD_PLUS, AGB_KEY_START}, {SNX_VPAD_MINUS, AGB_KEY_SELECT}};
    static const struct {
        float dx, dy;
        uint16_t key;
    } arms[] = {{-0.7f, 0, AGB_KEY_LEFT},
                {0.7f, 0, AGB_KEY_RIGHT},
                {0, -0.7f, AGB_KEY_UP},
                {0, 0.7f, AGB_KEY_DOWN},
                {0.7f, 0.7f, AGB_KEY_RIGHT | AGB_KEY_DOWN}};
    float x, y, bx, by, half;
    size_t drawn = 0, pressed = 0;
    /* Auto on a device with no touch screen: none drawn, none pressed; a finger on a screen shows them, the next one
       presses; a gamepad connected hides them, and gone shows them again */
    g_settings.show_controls = 2;
    vpad_test_middle(SNX_VPAD_A, &x, &y);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 1, x, y) && !keys(host));
    CHECK(controls_read(host, window, &drawn) == 0 && !drawn);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 1, x, y));
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1"); /* SDL's own screen of fingers, SDL_MOUSE_TOUCHID */
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, SDL_MOUSE_TOUCHID, 2, x, y));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, SDL_MOUSE_TOUCHID, 2, x, y));
    (void)keys(host);
    CHECK(!keys(host) && controls_read(host, window, &drawn) == 0 && drawn > 100);
    SDL_JoystickID pad = vpad_test_gamepad();
    CHECK(pad && vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, SDL_MOUSE_TOUCHID, 3, x, y));
    SDL_UpdateJoysticks();
    (void)keys(host);
    CHECK(!keys(host) && controls_read(host, window, &drawn) == 0 && !drawn);
    CHECK(SDL_DetachVirtualJoystick(pad));
    SDL_UpdateJoysticks();
    (void)keys(host);
    CHECK(!keys(host) && controls_read(host, window, &drawn) == 0 && drawn > 100);
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");

    /* each button under a finger, held until it lifts, and drawn pressed */
    g_settings.show_controls = 1;
    for (size_t i = 0; i < sizeof buttons / sizeof *buttons; ++i) {
        vpad_test_middle(buttons[i].control, &x, &y);
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 10, x, y) && keys(host) == buttons[i].key);
        CHECK(keys(host) == buttons[i].key && controls_read(host, window, &pressed) == 0 && pressed != drawn);
        /* a finger dragged off it holds it still, as a thumb that rolls; one that lands off every control is none's */
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 10, x + 300, y) && keys(host) == buttons[i].key);
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 10, x + 300, y) && !keys(host));
        CHECK(controls_read(host, window, &pressed) == 0 && pressed == drawn);
        CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y) && keys(host) == buttons[i].key);
        CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, x, y) && !keys(host));
    }
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 11, 480, 320) && !keys(host));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 11, 480, 320) && !keys(host));
    /* a tap that came and went between two polls reaches one */
    vpad_test_middle(SNX_VPAD_PLUS, &x, &y);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 12, x, y) &&
          vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 12, x, y));
    CHECK(keys(host) == AGB_KEY_START && !keys(host));
    /* the D-pad's directions, slid through, and two fingers at once */
    vpad_test_middle(SNX_VPAD_DPAD, &x, &y);
    half = vpad_test_side(0.25f) / 2;
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 13, x, y) && !keys(host));
    for (size_t i = 0; i < sizeof arms / sizeof *arms; ++i) {
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 13, x + arms[i].dx * half, y + arms[i].dy * half));
        CHECK(keys(host) == arms[i].key);
    }
    vpad_test_middle(SNX_VPAD_B, &bx, &by);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 14, bx, by));
    CHECK(keys(host) == (AGB_KEY_RIGHT | AGB_KEY_DOWN | AGB_KEY_B));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 13, x, y) && keys(host) == AGB_KEY_B);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_CANCELED, 77, 14, bx, by) && !keys(host));
    /* the Switch's others are not there: no ZL, no stick, no Home */
    CHECK(!strcmp(vpad_name(SNX_VPAD_ZL), "?") && !strcmp(vpad_name(SNX_VPAD_PLUS), "Start"));

    /* F2 edits the layout: the game gets nothing, a control dragged moves and the .ini keeps it as a Switch game's; F2
       again ends it. Read back, it is where it was left */
    vpad_test_middle(SNX_VPAD_A, &x, &y);
    CHECK(vpad_test_key(window, SDLK_F2) && !keys(host) && vpad_editing());
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 20, x, y) && !keys(host));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 20, 480, 320) && !keys(host));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 20, 480, 320) && !keys(host));
    CHECK(vpad_test_in_file(ini, "[On-screen controls]\ncontrols_a=0.500 0.500 100\n") == 1);
    CHECK(vpad_test_key(window, SDLK_F2) && !keys(host) && !vpad_editing());
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 21, 480, 320) && keys(host) == AGB_KEY_A);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 21, 480, 320) && !keys(host));
    vpad_console(&snx_vpad_gba); /* the defaults, as a run starts */
    CHECK(vpad_config()->at[SNX_VPAD_A].x > 0.9f);
    vpad_settings(dir, "a game", 0, NULL);
    CHECK(vpad_config()->at[SNX_VPAD_A].x == 0.5f && vpad_config()->at[SNX_VPAD_A].y == 0.5f);
    /* Esc ends it too, and is no press of the quit key */
    CHECK(vpad_test_key(window, SDLK_F2) && !keys(host) && vpad_editing());
    CHECK(controls_read(host, window, &drawn) == 0); /* the controls and edit mode's bar */
    /* nor does the keyboard reach the game while it is edited */
    SDL_Event x_key = {.key = {.type = SDL_EVENT_KEY_DOWN, .scancode = SDL_SCANCODE_X, .key = SDLK_X, .down = true}};
    CHECK(SDL_PushEvent(&x_key) && !keys(host));
    CHECK(vpad_test_key(window, SDLK_ESCAPE) && keys(host) == AGB_KEY_A && !vpad_editing());
    CHECK(!agb_host_notice(host)[0]);
    x_key.key.type = SDL_EVENT_KEY_UP, x_key.key.down = false;
    CHECK(SDL_PushEvent(&x_key) && !keys(host));
    /* Android's Back edits it, saying over the picture that a second one quits */
    CHECK(vpad_test_key(window, SDLK_AC_BACK) && !keys(host) && vpad_editing());
    CHECK(controls_read(host, window, &pressed) == 0 && pressed > drawn + 100); /* the bar and the line */
    CHECK(vpad_test_key(window, SDLK_AC_BACK) && keys(host) == 0xFFFF);
    vpad_edit(0);
    g_settings.show_controls = 0;
    CHECK(controls_read(host, window, &drawn) == 0 && !drawn); /* off: none */
    return 0;
}

int main(void) {
    char error[256] = {0};
    AgbHost* host;
    SDL_Window** windows;
    int count = 0, vsync = 0;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    sdl_real_pads_hide();
    vpad_console(&snx_vpad_gba);
    snx_vpad_sdl_settings(); /* the runtime's keys and fps_limit read with the controls', as main.c reads them */
    char *root = snx_temp_root(), *dir = root ? snx_temp_dir(root) : NULL;
    char* ini = dir ? snx_path_join(dir, "a game.ini") : NULL;
    if (!ini || snx_write_file(ini, "[Game]\nsystem=gba\n", strlen("[Game]\nsystem=gba\n"))) {
        fprintf(stderr, "agb_host: no temporary folder for the game's .ini\n");
        return 1;
    }
    host = agb_host_open("agb_host", ini, false, error, sizeof error);
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
    int failed = controls(host, window, dir, ini); /* before a notice is over the frame */
    /* the runtime's keys: a key the GBA has nothing for said with why; F11 and Alt+Return switch fullscreen */
    if (!press(host, SDLK_D, SDL_KMOD_LCTRL, false) ||
        notice_read(host, SDL_GetRenderer(window), window,
                    "Ctrl+D (TV mode) does nothing in a GBA game: the TV mode is the Switch's console docked") != 0) {
        fprintf(stderr, "agb_host: Ctrl+D was not said to do nothing in a GBA game\n");
        agb_host_close(host);
        return 1;
    }
    /* the upscaler's key: none, said why */
    CHECK(press(host, SDLK_U, SDL_KMOD_LCTRL, false) &&
          notice_read(host, SDL_GetRenderer(window), window,
                      "Ctrl+U (Upscaler) does nothing in a GBA game: its screen is drawn in sharp pixels, with no "
                      "upscaler nor anti-aliasing") == 0);
    /* the screenshot key: the next frame, as it shows, in the game's folder's captures/, said over it */
    {
        static uint32_t frame[AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT];
        char* captures = snx_path_join(dir, "captures");
        size_t count = 0, size = 0;
        char** names = NULL;
        unsigned char* got = NULL;
        for (size_t i = 0; i < sizeof frame / sizeof *frame; ++i) frame[i] = 0xFF000000u | (uint32_t)i;
        if (!press(host, SDLK_F3, SDL_KMOD_NONE, false)) return 1;
        agb_host_frame(host, frame, NULL, 0);
        if (!snx_list_dir(captures, &names, &count) && count == 1 && !strncmp(names[0], "screenshot_", 11)) {
            char* path = snx_path_join(captures, names[0]);
            got = (unsigned char*)snx_read_file(path, &size);
            free(path);
        }
        const char head[] = "P6\n240 160\n255\n";
        if (!got || size != sizeof head - 1 + sizeof frame / 4 * 3 || memcmp(got, head, sizeof head - 1) ||
            got[sizeof head - 1 + 3 * 300] != (300 & 0xFF) || got[sizeof head - 1 + 3 * 300 + 1] != (300 >> 8) ||
            notice_read(host, SDL_GetRenderer(window), window, "Screenshot saved in captures") != 0) {
            fprintf(stderr, "agb_host: F3 saved %zu files in %s, not the frame as one PPM\n", count, captures);
            agb_host_close(host);
            return 1;
        }
        free(got);
        snx_free_strings(names, count);
        free(captures);
    }
    /* F4: no limit, SDL's renderer then without vsync, and back to the game's pace, said as a Switch game's; fps_limit
       from the command line, a value not taken as given held to what is */
    {
        static const uint32_t frame[AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT];
        char* limits[] = {"scarletnx-agb", "--fps-limit=30", "--fps-limit=5", NULL};
        int vsync = -1;
        static const struct {
            const char* said;
            uint64_t frame_ns;
            int vsync;
        } steps[2] = {{"FPS limit: off", 0, 0}, {"FPS limit: the game's pace", 16743000, 1}};
        for (int i = 0; i < 2; ++i) {
            if (!press(host, SDLK_F4, SDL_KMOD_NONE, false)) return 1;
            agb_host_frame(host, frame, NULL, 0);
            if (snx_vpad_window_frame_ns(16743000) != steps[i].frame_ns ||
                !SDL_GetRenderVSync(SDL_GetRenderer(window), &vsync) || vsync != steps[i].vsync ||
                notice_read(host, SDL_GetRenderer(window), window, steps[i].said) != 0) {
                fprintf(stderr, "agb_host: F4's press %d did not give %s, vsync %d\n", i + 1, steps[i].said,
                        steps[i].vsync);
                agb_host_close(host);
                return 1;
            }
        }
        vpad_settings(dir, "a game", 2, limits);
        if (snx_vpad_window_frame_ns(16743000) != 1000000000ull / 30) {
            fprintf(stderr, "agb_host: --fps-limit=30 is not 30 frames a second\n");
            return 1;
        }
        snx_error_sink(said);
        vpad_settings(dir, "a game", 3, limits);
        snx_error_sink(NULL);
        if (snx_vpad_window_frame_ns(16743000) != 1000000000ull / 10 ||
            strcmp(g_said, "settings: fps_limit=5 is not game, 0 or a whole number of frames a second from 10 to 1000: "
                           "it takes 10")) {
            fprintf(stderr, "agb_host: --fps-limit=5 was not held to 10 frames a second, saying so (%s)\n", g_said);
            return 1;
        }
        char* back[] = {"scarletnx-agb", "--fps-limit=game", NULL};
        vpad_settings(dir, "a game", 2, back);
    }
    /* F12 and F6: the stats and the showcase over the game, on and off for the run, said as a Switch game's; the
       showcase draws what the game gets, the keyboard's A (Space) here, and both leave nothing drawn once off */
    {
        static const uint32_t frame[AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT];
        static const SDL_Keycode key[2] = {SDLK_F12, SDLK_F6};
        int w = 0, h = 0;
        CHECK(SDL_GetCurrentRenderOutputSize(SDL_GetRenderer(window), &w, &h));
        const SnxUiRects* r = snx_vpad_window_rects(w, h);
        size_t bare = r ? r->count : 0;
        for (int i = 0; i < 2; ++i) {
            int* setting = i ? &g_settings.showcase : &g_settings.stats;
            CHECK(press(host, key[i], SDL_KMOD_NONE, false) && *setting == 1);
            agb_host_frame(host, frame, NULL, 0);
            r = snx_vpad_window_rects(w, h);
            size_t on = r ? r->count : 0;
            CHECK(on > bare);
            if (i) { /* the A the game gets, drawn pressed in the showcase */
                SnxUiRect* was = malloc(on * sizeof *was);
                CHECK(was);
                memcpy(was, r->rect, on * sizeof *was);
                SDL_Event e = {0};
                e.type = SDL_EVENT_KEY_DOWN;
                e.key.key = SDLK_SPACE;
                e.key.scancode = SDL_SCANCODE_SPACE;
                e.key.down = true;
                CHECK(SDL_PushEvent(&e) && keys(host) == AGB_KEY_A);
                r = snx_vpad_window_rects(w, h);
                int same = r && r->count == on && !memcmp(r->rect, was, on * sizeof *was);
                free(was);
                e.type = SDL_EVENT_KEY_UP;
                e.key.down = false;
                CHECK(SDL_PushEvent(&e) && keys(host) == 0);
                CHECK(!same);
            }
            CHECK(press(host, key[i], SDL_KMOD_NONE, false) && *setting == 0);
            CHECK(notice_read(host, SDL_GetRenderer(window), window, i ? "Showcase: off" : "Stats: off") == 0);
        }
    }
    /* the pause key holds the game and says so, as a Switch game's */
    static const uint32_t held[AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT];
    for (int i = 0; i < 2; ++i)
        if (!press(host, SDLK_PAUSE, SDL_KMOD_NONE, false) || snx_vpad_window_paused() != !i ||
            agb_host_held(host, held) != !i ||
            notice_read(host, SDL_GetRenderer(window), window, i ? "Pause: off" : "Pause: on") != 0) {
            fprintf(stderr, "agb_host: the pause key did not hold the game and say so (press %d)\n", i + 1);
            agb_host_close(host);
            return 1;
        }
    for (int i = 0; i < 4; ++i) {
        bool alt = i & 1;
        if (!press(host, alt ? SDLK_RETURN : SDLK_F11, alt ? SDL_KMOD_LALT : SDL_KMOD_NONE, false) ||
            !SDL_SyncWindow(window) || !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != (i == 1 || i == 3)) {
            fprintf(stderr, "agb_host: %s did not switch the window's fullscreen\n", alt ? "Alt+Return" : "F11");
            agb_host_close(host);
            return 1;
        }
    }
    /* the quit key as a Switch game's command line sets it: Q quits on its second press, Esc no more; a key SDL has no
       name for keeps the one in effect, as a Switch game's does (hotkey_set says so) */
    {
        char why[256] = {0}, *quit_q[] = {"scarletnx-agb", "--hotkey-quit=Q", NULL},
             *quit_esc[] = {"scarletnx-agb", "--hotkey-quit=Escape", NULL},
             *quit_nope[] = {"scarletnx-agb", "--hotkey-quit=Nope", NULL};
        if (vpad_arg(quit_nope[1], why, sizeof why) != 1 || vpad_arg(quit_q[1], why, sizeof why) != 1) {
            fprintf(stderr, "agb_host: --hotkey-quit= was not taken as the runtime takes it (%s)\n", why);
            agb_host_close(host);
            return 1;
        }
        vpad_settings(dir, "a game", 2, quit_nope);
        if (strcmp(hotkey_text(HOTKEY_QUIT), "Escape")) {
            fprintf(stderr, "agb_host: hotkey_quit=Nope left %s, not Escape\n", hotkey_text(HOTKEY_QUIT));
            agb_host_close(host);
            return 1;
        }
        vpad_settings(dir, "a game", 2, quit_q);
        if (!esc(host, SDL_KMOD_NONE, false) || !esc(host, SDL_KMOD_NONE, false) ||
            !press(host, SDLK_Q, SDL_KMOD_NONE, false) || press(host, SDLK_Q, SDL_KMOD_NONE, false)) {
            fprintf(stderr, "agb_host: with hotkey_quit=Q, Esc quit or Q did not on its second press\n");
            agb_host_close(host);
            return 1;
        }
        vpad_settings(dir, "a game", 2, quit_esc);
        SDL_Delay(SNX_QUIT_AGAIN_NS / 1000000 + 100); /* Q's presses spent: Esc's first is a first again */
    }
    if (!esc(host, SDL_KMOD_LCTRL, false) || !esc(host, SDL_KMOD_LALT, false) || !esc(host, SDL_KMOD_LGUI, false) ||
        !esc(host, SDL_KMOD_NONE, false)) {
        fprintf(stderr, "agb_host: Esc's first press closed the window\n");
        agb_host_close(host);
        return 1;
    }
    if (notice_read(host, SDL_GetRenderer(window), window, "Press Escape again within 3 seconds to quit") != 0) {
        agb_host_close(host);
        return 1;
    }
    if (!esc(host, SDL_KMOD_NONE, true) || !esc(host, SDL_KMOD_LCTRL, false) || esc(host, SDL_KMOD_NONE, false)) {
        fprintf(stderr, "agb_host: Esc did not close the window on its second press, and only then\n");
        agb_host_close(host);
        return 1;
    }
    agb_host_close(host);
    if (snx_remove_tree(dir)) fprintf(stderr, "agb_host: %s could not be removed\n", dir);
    free(ini);
    free(dir);
    free(root);
    if (failed) return 1;
    puts("agb_host: passed");
    return 0;
}
