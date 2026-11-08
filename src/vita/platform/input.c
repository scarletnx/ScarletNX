/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's input for the Vita game, as the 3DS driver's (3ds/input/input.c) by where the buttons are: Z or Space
   Cross (south), X Circle (east), V Square (west), C Triangle (north), Q L, E R, Enter Start, Backspace Select, the
   arrows the D-pad, WASD the left stick and IJKL the right (pad_sdl.h's keys of a Switch game, Space added). The
   window's mouse is the touch screen: the left button touches the front panel, the right the rear one. A gamepad's
   controls are a Switch game's pad's, as on the 3DS: each sends the button where it is, gamepad_map over that, then
   gamepad_swap_ab swapping Cross and Circle; its sticks are the Vita's, each past gamepad_deadzone. The script
   (vita_script) is ms:hex presses, each held for vita_press_len ms, its bits 24 to 27 the left stick pushed right,
   left, up or down */
#include "vita/platform/input.h"
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
/* the Vita's bit for each of the Switch's buttons (pad_map.h's) by where it sits, 0 where the Vita has none */
static const unsigned VITA_BITS[SNX_PAD_BUTTONS] = {
    VITA_CTRL_CIRCLE,   /* A, east */
    VITA_CTRL_CROSS,    /* B, south */
    VITA_CTRL_TRIANGLE, /* X, north */
    VITA_CTRL_SQUARE,   /* Y, west */
    0,                  /* L3 */
    0,                  /* R3 */
    VITA_CTRL_LTRIGGER, /* L */
    VITA_CTRL_RTRIGGER, /* R */
    0,                  /* ZL */
    0,                  /* ZR */
    VITA_CTRL_START,    /* Plus */
    VITA_CTRL_SELECT,   /* Minus */
    VITA_CTRL_LEFT,     /* Left */
    VITA_CTRL_UP,       /* Up */
    VITA_CTRL_RIGHT,    /* Right */
    VITA_CTRL_DOWN,     /* Down */
};

static struct {
    SDL_Gamepad* pads[INPUT_PADS];
    SnxPress* script;
    size_t presses;
    double start;
    bool gamepads;
    SnxPadMap map; /* gamepad_map's: the controls it names, the others SNX_PAD_UNSET */
} g_input = {.map = {{UNSET4, UNSET4, UNSET4, UNSET4}}};

void vita_input_map(const char* text) { snx_pad_map_setting(&g_input.map, text, VITA_BITS, "Vita"); }

#define SCRIPT_BUTTONS     0x00ffffffu
#define SCRIPT_STICK_RIGHT 0x01000000u
#define SCRIPT_STICK_LEFT  0x02000000u
#define SCRIPT_STICK_UP    0x04000000u
#define SCRIPT_STICK_DOWN  0x08000000u
void vita_input_init(void) {
    g_input.start = snx_seconds();
    /* its buttons, and the left stick's directions in bits 24 to 27 */
    g_input.presses = snx_script_parse("scarletnx-vita", g_settings.vita_script,
                                       VITA_CTRL_BUTTONS | SCRIPT_STICK_RIGHT | SCRIPT_STICK_LEFT | SCRIPT_STICK_UP |
                                           SCRIPT_STICK_DOWN,
                                       &g_input.script);
    g_input.gamepads = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
}
void vita_input_event(const union SDL_Event* event) { snx_pads_event(g_input.pads, INPUT_PADS, event, NULL); }
/* an axis, -1 to 1, to the stick's 0 to 255 */
static uint8_t stick(float v) {
    if (v < -1) v = -1;
    if (v > 1) v = 1;
    return (uint8_t)lrintf(127.5f + v * 127.5f);
}
void vita_input_read(VitaInput* input) {
    float sticks[4] = {0, 0, 0, 0}; /* left x, y, right x, y; y down as the Vita has it */
    *input = (VitaInput){0};
    const bool* held = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (held) {
        unsigned keys = snx_pad_keys(held);
        for (int b = 0; b < SNX_PAD_BUTTONS; ++b)
            if (keys & 1u << b) input->buttons |= VITA_BITS[b];
        if (held[SDL_SCANCODE_SPACE]) input->buttons |= VITA_CTRL_CROSS;
        float left[2], right[2];
        snx_pad_key_sticks(held, left, right);
        sticks[0] = left[0], sticks[1] = -left[1], sticks[2] = right[0], sticks[3] = -right[1];
        /* the mouse over the window: its place scaled to the panel's */
        float x, y;
        SDL_MouseButtonFlags mouse = SDL_GetMouseState(&x, &y);
        SDL_Window* window = SDL_GetMouseFocus();
        int w = 0, h = 0;
        if (window && SDL_GetWindowSize(window, &w, &h) && w > 0 && h > 0 && (mouse & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK))) {
            float u = fminf(fmaxf(x / (float)w, 0), 1), v = fminf(fmaxf(y / (float)h, 0), 1);
            if (mouse & SDL_BUTTON_LMASK) {
                input->front = true;
                input->front_x = (uint16_t)lrintf(u * 1919), input->front_y = (uint16_t)lrintf(v * 1087);
            }
            if (mouse & SDL_BUTTON_RMASK) {
                input->back = true;
                input->back_x = (uint16_t)lrintf(u * 1919), input->back_y = (uint16_t)lrintf(v * 889);
            }
        }
    }
    double dead = g_settings.stick_dead_zone / 100.0;
    SnxPadMap map;
    snx_pad_map_sent(&map, &g_input.map, g_settings.swap_a_b);
    for (unsigned p = 0; p < INPUT_PADS; ++p) {
        SDL_Gamepad* pad = g_input.pads[p];
        if (!pad) continue;
        for (int c = 0; c < SNX_PAD_CONTROLS; ++c)
            if (map.bit[c] >= 0 && snx_pad_held(pad, c)) input->buttons |= VITA_BITS[map.bit[c]];
        SDL_GamepadAxis axes[4] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
                                   SDL_GAMEPAD_AXIS_RIGHTY};
        for (unsigned s = 0; s < 2; ++s) {
            double x = snx_pad_axis(pad, axes[s * 2]), y = snx_pad_axis(pad, axes[s * 2 + 1]);
            if (snx_pad_dead_zone(&x, &y, dead, 1)) sticks[s * 2] = (float)x, sticks[s * 2 + 1] = (float)y;
        }
    }
    /* the script's presses: buttons, and the left stick pushed all the way by bits 24 to 27 (right, left, up, down) */
    if (g_input.presses) {
        double ms = (snx_seconds() - g_input.start) * 1000.0;
        uint32_t mask = snx_script_held(g_input.script, g_input.presses, ms, (uint32_t)g_settings.vita_press_length);
        input->buttons |= mask & SCRIPT_BUTTONS;
        if (mask & SCRIPT_STICK_RIGHT) sticks[0] = 1;
        if (mask & SCRIPT_STICK_LEFT) sticks[0] = -1;
        if (mask & SCRIPT_STICK_UP) sticks[1] = -1;
        if (mask & SCRIPT_STICK_DOWN) sticks[1] = 1;
    }
    input->lx = stick(sticks[0]), input->ly = stick(sticks[1]);
    input->rx = stick(sticks[2]), input->ry = stick(sticks[3]);
}
void vita_input_free(void) {
    snx_pads_close(g_input.pads, INPUT_PADS);
    if (g_input.gamepads) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    free(g_input.script);
    SnxPadMap map = g_input.map; /* a setting, not the run's: it stays */
    memset(&g_input, 0, sizeof g_input);
    g_input.map = map;
}
