/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The DS runner's window (src/nds/runtime/host.c) under SDL's offscreen driver: its on-screen controls each press their
   key under a finger and under the mouse, alone and two at once, drawn over frames that go on screen; the pen is one
   finger or the mouse on the lower screen, the first that pressed it, by the touch setting, kept to its edge when it
   leaves, and never a finger on a control; Auto hides the controls as a gamepad connects; F2 edits their layout, kept in
   the game's .ini, and Android's Back twice quits. Skipped (77) where no Vulkan window opens */
#include "common/utilities/common.h"
#include "common/utilities/vpad_consoles.h"
#include "nds/gpu/vk/vk.h"
#include "nds/gpu/vk/vk_present.h"
#include "nds/runtime/host.h"
#include "support/sdl_real_pads.h"
#include "support/vpad_window.h"
#include <stdlib.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "nds_host:%d: %s\n", __LINE__, #x);                                                        \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static NdsInput g_input;
/* the keys after the events waiting, 0xFFFF once the window closed; the pen in g_input */
static uint16_t keys(NdsHost* host) {
    g_input = (NdsInput){0};
    return nds_host_poll(host, &g_input) ? g_input.keys : 0xFFFF;
}
static bool pen_at(unsigned x, unsigned y) {
    return g_input.touching && abs((int)g_input.touch_x - (int)x) <= 1 && abs((int)g_input.touch_y - (int)y) <= 1;
}

static int controls(NdsHost* host, SDL_Window* window, const char* dir, const char* ini) {
    static const struct {
        int control;
        uint16_t key;
    } buttons[] = {{SNX_VPAD_A, 0x001}, {SNX_VPAD_B, 0x002}, {SNX_VPAD_X, 0x400},    {SNX_VPAD_Y, 0x800},
                   {SNX_VPAD_L, 0x200}, {SNX_VPAD_R, 0x100}, {SNX_VPAD_PLUS, 0x008}, {SNX_VPAD_MINUS, 0x004}};
    static const struct {
        float dx, dy;
        uint16_t key;
    } arms[] = {{-0.7f, 0, 0x020}, {0.7f, 0, 0x010}, {0, -0.7f, 0x040}, {0, 0.7f, 0x080}, {-0.7f, -0.7f, 0x060}};
    float x, y, half, sx = 0, sy = 0;
    int w = 0, h = 0, fx = 0, fy = 0;
    CHECK(SDL_GetWindowSizeInPixels(window, &w, &h));
    /* Auto on a device with no touch screen shows none; on one, until a gamepad connects */
    CHECK(!snx_vpad_window_rects(w, h));
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1"); /* SDL's own screen of fingers, SDL_MOUSE_TOUCHID */
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, SDL_MOUSE_TOUCHID, 2, 1, 1));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, SDL_MOUSE_TOUCHID, 2, 1, 1) && !keys(host));
    CHECK(snx_vpad_window_rects(w, h) && snx_vpad_window_rects(w, h)->count > 100);
    SDL_JoystickID gamepad = vpad_test_gamepad();
    SDL_UpdateJoysticks();
    (void)keys(host);
    CHECK(gamepad && !keys(host) && !snx_vpad_window_rects(w, h));
    CHECK(SDL_DetachVirtualJoystick(gamepad));
    SDL_UpdateJoysticks();
    (void)keys(host);
    CHECK(snx_vpad_window_rects(w, h));
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");

    g_settings.show_controls = 1;
    for (int frame = 0; frame < 3; ++frame) nds_vk_present(); /* the picture's place, and the controls over it */
    for (size_t i = 0; i < sizeof buttons / sizeof *buttons; ++i) {
        vpad_test_middle(buttons[i].control, &x, &y);
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 10, x, y) && keys(host) == buttons[i].key);
        CHECK(!g_input.touching); /* the control's, never the pen's */
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 10, x, y - 200) && keys(host) == buttons[i].key);
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 10, x, y - 200) && !keys(host));
        CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y) && keys(host) == buttons[i].key);
        CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, x, y) && !keys(host));
    }
    vpad_test_middle(SNX_VPAD_DPAD, &x, &y);
    half = vpad_test_side(0.25f) / 2;
    /* whole in the window, so its middle is not its place where it starts at the window's foot */
    y = y > (float)h - half ? (float)h - half : y;
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 13, x, y) && !keys(host));
    for (size_t i = 0; i < sizeof arms / sizeof *arms; ++i) {
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 13, x + arms[i].dx * half, y + arms[i].dy * half));
        CHECK(keys(host) == arms[i].key);
    }
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 13, x, y) && !keys(host));

    /* the lower screen's middle, in the window's pixels */
    for (sy = 0; sy < (float)h && !(nds_vk_present_point((float)w / 2, sy, &fx, &fy) && fy >= 192 + 96); ++sy) continue;
    sx = (float)w / 2;
    CHECK(sy < (float)h);
    /* a finger there is the pen, beside a control under another; a second finger there is not; dragged off the screen it
       keeps to the edge, and lifted it is gone */
    vpad_test_middle(SNX_VPAD_A, &x, &y);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 20, sx, sy) && !keys(host) && pen_at(128, 96));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 21, x, y) && keys(host) == 0x001 && pen_at(128, 96));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 22, sx + 30, sy) && keys(host) == 0x001 &&
          pen_at(128, 96));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 20, sx, 2) && keys(host) == 0x001 && pen_at(128, 0));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 20, -50, sy) && keys(host) == 0x001 && pen_at(0, 96));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 22, sx + 30, sy) && keys(host) == 0x001 && pen_at(0, 96));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 20, sx, sy) && keys(host) == 0x001 && !g_input.touching);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 21, x, y) && !keys(host));
    /* a press that starts above it, on the upper screen, is none, wherever it goes */
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 23, sx, sy / 3) && !keys(host) && !g_input.touching);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 23, sx, sy) && !keys(host) && !g_input.touching);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 23, sx, sy) && !keys(host));
    /* the touch setting: 2 the mouse too, 1 the fingers alone, 0 none */
    CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy) && !keys(host) && pen_at(128, 96));
    CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy) && !keys(host) && !g_input.touching);
    g_settings.touch_screen = 1;
    CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy) && !keys(host) && !g_input.touching);
    CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy) && !keys(host));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 24, sx, sy) && !keys(host) && pen_at(128, 96));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_CANCELED, 77, 24, sx, sy) && !keys(host) && !g_input.touching);
    g_settings.touch_screen = 0;
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 25, sx, sy) && !keys(host) && !g_input.touching);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 25, sx, sy) && !keys(host));
    g_settings.touch_screen = 2;

    /* the layout edited: no key and no pen, a control dragged kept in the .ini and read back */
    SDL_Event x_key = {.key = {.type = SDL_EVENT_KEY_DOWN, .scancode = SDL_SCANCODE_X, .key = SDLK_X, .down = true}};
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 29, sx, sy) && SDL_PushEvent(&x_key));
    CHECK(keys(host) == 0x001 && pen_at(128, 96)); /* held as the editing starts, and let go of by it */
    CHECK(vpad_test_key(window, SDLK_F2) && !keys(host) && vpad_editing() && !g_input.touching);
    x_key.key.type = SDL_EVENT_KEY_UP, x_key.key.down = false;
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 29, sx, sy) && SDL_PushEvent(&x_key) && !keys(host));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 30, sx, sy) && !keys(host) && !g_input.touching);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 30, sx, sy) && !keys(host));
    vpad_test_middle(SNX_VPAD_X, &x, &y);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 31, x, y) && !keys(host));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 31, (float)w / 2, (float)h / 2) && !keys(host));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 31, (float)w / 2, (float)h / 2) && !keys(host));
    CHECK(vpad_test_in_file(ini, "[On-screen controls]\ncontrols_x=0.500 0.500 100\n") == 1);
    CHECK(vpad_test_key(window, SDLK_F2) && !keys(host) && !vpad_editing());
    /* the pause key: the game held (main.c's loop shows the last frame again, nds_host_held), then let go */
    CHECK(!strcmp(nds_vk_notice_text(), ""));
    CHECK(vpad_test_key(window, SDLK_PAUSE) && !keys(host) && snx_vpad_window_paused());
    CHECK(!strcmp(nds_vk_notice_text(), "Pause: on")); /* over the game, as a Switch game's */
    /* the screenshot key, paused too: both screens in the game's folder's captures/, a PPM */
    {
        char *captures = snx_path_join(dir, "captures"), **names = NULL;
        size_t count = 0, size = 0;
        unsigned char* got = NULL;
        CHECK(vpad_test_key(window, SDLK_F3) && !keys(host));
        CHECK(nds_host_held(host));
        CHECK(!snx_list_dir(captures, &names, &count) && count == 1 && !strncmp(names[0], "screenshot_", 11));
        char* path = snx_path_join(captures, names[0]);
        CHECK((got = snx_read_file(path, &size)) && !memcmp(got, "P6\n256 384\n255\n", 15));
        free(got);
        free(path);
        snx_free_strings(names, count);
        free(captures);
    }
    CHECK(nds_host_held(host));
    CHECK(vpad_test_key(window, SDLK_PAUSE) && !keys(host) && !snx_vpad_window_paused() && !nds_host_held(host));
    CHECK(!strcmp(nds_vk_notice_text(), "Pause: off"));
    vpad_console(&snx_vpad_nds);
    vpad_settings(dir, "a game", 0, NULL);
    CHECK(vpad_config()->at[SNX_VPAD_X].x == 0.5f && vpad_config()->at[SNX_VPAD_X].y == 0.5f);
    CHECK(vpad_test_key(window, SDLK_AC_BACK) && !keys(host) && vpad_editing());
    nds_vk_present();
    CHECK(vpad_test_key(window, SDLK_AC_BACK) && keys(host) == 0xFFFF);
    return 0;
}

int main(void) {
    char error[256] = {0};
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    sdl_real_pads_hide();
    vpad_console(&snx_vpad_nds);
    g_settings.touch_screen = 2;
    char *root = snx_temp_root(), *dir = root ? snx_temp_dir(root) : NULL;
    char* ini = dir ? snx_path_join(dir, "a game.ini") : NULL;
    CHECK(ini && !snx_write_file(ini, "[Game]\nsystem=nds\n", strlen("[Game]\nsystem=nds\n")));
    NdsHost* host = nds_host_open(NULL, "nds_host", ini, NULL, 0, 0, error, sizeof error);
    int failed = 0, opened = host && nds_vk() && nds_vk()->host.window;
    if (opened)
        failed = controls(host, nds_vk()->host.window, dir, ini);
    else
        printf("nds_host: no Vulkan window here (%s): skipped\n", error);
    nds_host_close(host);
    if (snx_remove_tree(dir)) fprintf(stderr, "nds_host: %s could not be removed\n", dir);
    free(ini);
    free(dir);
    free(root);
    if (failed) return 1;
    if (!opened) return 77;
    puts("nds_host: passed");
    return 0;
}
