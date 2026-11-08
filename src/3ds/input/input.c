/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's input for the 3DS game. The keys are a Switch game's (the SDL backend's keyboard_buttons): X or Space
   A, Z B, C X, V Y, Q L, E R, 1 ZL, 3 ZR, Enter Start, Backspace Select, the arrows the D-pad, WASD the circle pad and
   IJKL the C-stick (ZL, ZR and the C-stick are the New 3DS's, which ir:rst gives the game). A gamepad's
   controls are a Switch game's pad's (backend/gamepad.c): each sends the button where it is, as on the console (east
   A, south B, north X, west Y), gamepad_map over that in the Switch's button names (Plus is Start and Minus Select),
   then gamepad_swap_ab swapping A and B; its left stick is the circle pad and its right one the C-stick, each past
   gamepad_deadzone. The first pad with motion sensors is the console's accelerometer and gyroscope. The script
   (ctr_script) is unattended.c's for a Switch game: ms:hex presses, each held for press_len ms */
#include "3ds/input/input.h"
#include "3ds/board/clock.h"
#include "common/utilities/common.h"
#include "common/utilities/pad_sdl.h"
#include "common/utilities/script.h"
#include "snx_settings.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_PADS 4
#define UNSET4     SNX_PAD_UNSET, SNX_PAD_UNSET, SNX_PAD_UNSET, SNX_PAD_UNSET
typedef char input_map_controls[SNX_PAD_CONTROLS == 16 ? 1 : -1]; /* the four UNSET4 of g_input.map */
/* the 3DS's bit for each of the Switch's buttons a gamepad sends (pad_map.h's), 0 where the 3DS has none */
static const unsigned CTR_BITS[SNX_PAD_BUTTONS] = {
    SNX_CTR_PAD_A,      /* A */
    SNX_CTR_PAD_B,      /* B */
    SNX_CTR_PAD_X,      /* X */
    SNX_CTR_PAD_Y,      /* Y */
    0,                  /* L3 */
    0,                  /* R3 */
    SNX_CTR_PAD_L,      /* L */
    SNX_CTR_PAD_R,      /* R */
    SNX_CTR_PAD_ZL,     /* ZL */
    SNX_CTR_PAD_ZR,     /* ZR */
    SNX_CTR_PAD_START,  /* Plus */
    SNX_CTR_PAD_SELECT, /* Minus */
    SNX_CTR_PAD_LEFT,   /* Left */
    SNX_CTR_PAD_UP,     /* Up */
    SNX_CTR_PAD_RIGHT,  /* Right */
    SNX_CTR_PAD_DOWN,   /* Down */
};

static struct {
    SDL_Gamepad* pads[INPUT_PADS];
    SnxPress* script; /* ms after snx_ctr_input_init, SNX_CTR_PAD_* */
    size_t presses;
    double start; /* the clock (board/clock.h) at snx_ctr_input_init: the script is on the game's time */
    bool gamepads;
    bool touch;
    uint32_t touch_x, touch_y;
    SnxPadMap map; /* gamepad_map's: the controls it names, the others SNX_PAD_UNSET */
} g_input = {.map = {{UNSET4, UNSET4, UNSET4, UNSET4}}};

void snx_ctr_input_map(const char* text) { snx_pad_map_setting(&g_input.map, text, CTR_BITS, "3DS"); }

void snx_ctr_input_init(void) {
    g_input.start = snx_ctr_clock_seconds();
    g_input.presses = snx_script_parse("scarletnx-3ds", g_settings.ctr_script, SNX_CTR_PAD_BUTTONS, &g_input.script);
    /* SDL sends an added event for each gamepad already connected */
    g_input.gamepads = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!g_input.gamepads) fprintf(stderr, "scarletnx-3ds: no gamepads: %s\n", SDL_GetError());
}
/* the lines of snx_pad_motion_enable (pad_sdl.c), which turns a pad's motion sensors on: each says whether its sensor
   turned on, SDL's error when it did not */
static void say_motion(int warn, const char* line) {
    (void)warn;
    fprintf(stderr, "3ds input: %s\n", line);
}
static void opened(SDL_Gamepad* pad) { snx_pad_motion_enable(pad, say_motion); }
void snx_ctr_input_event(const union SDL_Event* event) { snx_pads_event(g_input.pads, INPUT_PADS, event, opened); }
/* A release keeps the last position: the HID sample keeps it too, with its validity clear */
void snx_ctr_input_touch(bool pressed, uint32_t x, uint32_t y) {
    g_input.touch = pressed;
    if (pressed) {
        g_input.touch_x = x;
        g_input.touch_y = y;
    }
}
bool snx_ctr_input_touched(void) { return g_input.touch; }
/* A stick at x and y (-1 to 1, y up) as held, past the dead zone; it replaces held when it is pushed further, so of
   several pads and the keys the furthest wins */
static void stick(float x, float y, float dead, float held[2]) {
    double sx = x, sy = y;
    if (sqrtf(x * x + y * y) > sqrtf(held[0] * held[0] + held[1] * held[1]) && snx_pad_dead_zone(&sx, &sy, dead, 1)) {
        held[0] = (float)sx;
        held[1] = (float)sy;
    }
}
void snx_ctr_input_read(SnxCtrInput* input) {
    *input = (SnxCtrInput){0};
    input->touch = g_input.touch;
    input->touch_x = g_input.touch_x;
    input->touch_y = g_input.touch_y;
    /* the window's keys, which SDL keeps for a window with the focus: none without a window */
    const bool* held = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (held) {
        /* the Switch game's keys (pad_sdl.h), and Space A too */
        unsigned keys = snx_pad_keys(held);
        for (int b = 0; b < SNX_PAD_BUTTONS; ++b)
            if (keys & 1u << b) input->buttons |= CTR_BITS[b];
        if (held[SDL_SCANCODE_SPACE]) input->buttons |= SNX_CTR_PAD_A;
        snx_pad_key_sticks(held, input->circle, input->c_stick);
    }
    float dead = (float)g_settings.stick_dead_zone / 100.0f;
    /* what each control of a pad sends, as a Switch game's pad's (backend/gamepad.c's map_of) */
    SnxPadMap map;
    snx_pad_map_sent(&map, &g_input.map, g_settings.swap_a_b);
    for (unsigned p = 0; p < INPUT_PADS; ++p) {
        SDL_Gamepad* pad = g_input.pads[p];
        if (!pad) continue;
        for (int c = 0; c < SNX_PAD_CONTROLS; ++c)
            if (map.bit[c] >= 0 && snx_pad_held(pad, c)) input->buttons |= CTR_BITS[map.bit[c]];
        stick(snx_pad_axis(pad, SDL_GAMEPAD_AXIS_LEFTX), -snx_pad_axis(pad, SDL_GAMEPAD_AXIS_LEFTY), dead,
              input->circle);
        stick(snx_pad_axis(pad, SDL_GAMEPAD_AXIS_RIGHTX), -snx_pad_axis(pad, SDL_GAMEPAD_AXIS_RIGHTY), dead,
              input->c_stick);
    }
    if (g_input.presses) {
        double ms = (snx_ctr_clock_seconds() - g_input.start) * 1000.0;
        input->buttons |= snx_script_held(g_input.script, g_input.presses, ms, (uint32_t)g_settings.press_length);
    }
}
bool snx_ctr_input_motion(float accel[3], float gyro[3]) {
    const float degrees = 57.29577951f; /* a radian's */
    for (unsigned p = 0; p < INPUT_PADS; ++p)
        for (int which = 0; g_input.pads[p] && which < SNX_PAD_MOTIONS; ++which) {
            float a[3], w[3];
            if (!snx_pad_motion_read(g_input.pads[p], which, a, w)) continue;
            /* Azahar's SDL motion (input_common/sdl/sdl_impl.cpp at azahar-emu/azahar 6e42de8): y of the acceleration
               and x and z of the rotation negated */
            accel[0] = a[0] / SDL_STANDARD_GRAVITY;
            accel[1] = -a[1] / SDL_STANDARD_GRAVITY;
            accel[2] = a[2] / SDL_STANDARD_GRAVITY;
            gyro[0] = -w[0] * degrees;
            gyro[1] = w[1] * degrees;
            gyro[2] = -w[2] * degrees;
            return true;
        }
    return false;
}
void snx_ctr_input_free(void) {
    snx_pads_close(g_input.pads, INPUT_PADS);
    if (g_input.gamepads) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    free(g_input.script);
    SnxPadMap map = g_input.map; /* a setting, not the run's: it stays */
    memset(&g_input, 0, sizeof g_input);
    g_input.map = map;
}
