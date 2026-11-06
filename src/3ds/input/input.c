/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's input for the 3DS game. The keys are a Switch game's (the SDL backend's keyboard_buttons): X or Space
   A, Z B, C X, V Y, Q L, E R, Enter Start, Backspace Select, the arrows the D-pad and WASD the circle pad. A gamepad's
   buttons go by where they are, as on the console (east A, south B, north X, west Y), gamepad_swap_ab swapping A and
   B, its left stick the circle pad past gamepad_deadzone. The script (ctr_script) is unattended.c's for a Switch game:
   ms:hex presses, each held for press_len ms */
#include "3ds/input/input.h"
#include "common/utilities/common.h"
#include "common/utilities/script.h"
#include "snx_settings.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_PADS 4

static struct {
    SDL_Gamepad* pads[INPUT_PADS];
    SnxPress* script; /* ms after snx_ctr_input_init, SNX_CTR_PAD_* */
    size_t presses;
    double start; /* snx_seconds() at snx_ctr_input_init */
    bool gamepads;
    bool touch;
    uint32_t touch_x, touch_y;
} g_input;

void snx_ctr_input_init(void) {
    g_input.start = snx_seconds();
    g_input.presses = snx_script_parse("scarletnx-3ds", g_settings.ctr_script, SNX_CTR_PAD_BUTTONS, &g_input.script);
    /* SDL sends an added event for each gamepad already connected */
    g_input.gamepads = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!g_input.gamepads) fprintf(stderr, "scarletnx-3ds: no gamepads: %s\n", SDL_GetError());
}
void snx_ctr_input_event(const union SDL_Event* event) {
    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        for (unsigned i = 0; i < INPUT_PADS; ++i)
            if (g_input.pads[i] && SDL_GetGamepadID(g_input.pads[i]) == event->gdevice.which) return;
        for (unsigned i = 0; i < INPUT_PADS; ++i)
            if (!g_input.pads[i]) {
                g_input.pads[i] = SDL_OpenGamepad(event->gdevice.which);
                break;
            }
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED)
        for (unsigned i = 0; i < INPUT_PADS; ++i)
            if (g_input.pads[i] && SDL_GetGamepadID(g_input.pads[i]) == event->gdevice.which) {
                SDL_CloseGamepad(g_input.pads[i]);
                g_input.pads[i] = NULL;
            }
}
/* A release keeps the last position: the HID sample keeps it too, with its validity clear */
void snx_ctr_input_touch(bool pressed, uint32_t x, uint32_t y) {
    g_input.touch = pressed;
    if (pressed) {
        g_input.touch_x = x;
        g_input.touch_y = y;
    }
}
bool snx_ctr_input_touched(void) { return g_input.touch; }
/* a stick's axis, -1 to 1 */
static float axis(SDL_Gamepad* pad, SDL_GamepadAxis which) {
    float v = SDL_GetGamepadAxis(pad, which) / 32767.0f;
    return v < -1 ? -1 : v;
}
void snx_ctr_input_read(SnxCtrInput* input) {
    static const struct {
        SDL_Scancode key;
        uint32_t bit;
    } keys[] = {
        {SDL_SCANCODE_X, SNX_CTR_PAD_A},           {SDL_SCANCODE_SPACE, SNX_CTR_PAD_A},
        {SDL_SCANCODE_Z, SNX_CTR_PAD_B},           {SDL_SCANCODE_C, SNX_CTR_PAD_X},
        {SDL_SCANCODE_V, SNX_CTR_PAD_Y},           {SDL_SCANCODE_Q, SNX_CTR_PAD_L},
        {SDL_SCANCODE_E, SNX_CTR_PAD_R},           {SDL_SCANCODE_RETURN, SNX_CTR_PAD_START},
        {SDL_SCANCODE_BACKSPACE, SNX_CTR_PAD_SELECT}, {SDL_SCANCODE_LEFT, SNX_CTR_PAD_LEFT},
        {SDL_SCANCODE_UP, SNX_CTR_PAD_UP},         {SDL_SCANCODE_RIGHT, SNX_CTR_PAD_RIGHT},
        {SDL_SCANCODE_DOWN, SNX_CTR_PAD_DOWN},
    };
    static const struct {
        SDL_GamepadButton button;
        uint32_t bit;
    } buttons[] = {
        {SDL_GAMEPAD_BUTTON_EAST, SNX_CTR_PAD_A},           {SDL_GAMEPAD_BUTTON_SOUTH, SNX_CTR_PAD_B},
        {SDL_GAMEPAD_BUTTON_NORTH, SNX_CTR_PAD_X},          {SDL_GAMEPAD_BUTTON_WEST, SNX_CTR_PAD_Y},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SNX_CTR_PAD_L},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SNX_CTR_PAD_R},
        {SDL_GAMEPAD_BUTTON_START, SNX_CTR_PAD_START},      {SDL_GAMEPAD_BUTTON_BACK, SNX_CTR_PAD_SELECT},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, SNX_CTR_PAD_LEFT},   {SDL_GAMEPAD_BUTTON_DPAD_UP, SNX_CTR_PAD_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SNX_CTR_PAD_RIGHT}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, SNX_CTR_PAD_DOWN},
    };
    *input = (SnxCtrInput){0};
    input->touch = g_input.touch;
    input->touch_x = g_input.touch_x;
    input->touch_y = g_input.touch_y;
    /* the window's keys, which SDL keeps for a window with the focus: none without a window */
    const bool* held = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (held) {
        for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i)
            if (held[keys[i].key]) input->buttons |= keys[i].bit;
        input->circle[0] = (float)(held[SDL_SCANCODE_D] - held[SDL_SCANCODE_A]);
        input->circle[1] = (float)(held[SDL_SCANCODE_W] - held[SDL_SCANCODE_S]);
    }
    float dead = (float)g_settings.stick_dead_zone / 100.0f;
    for (unsigned p = 0; p < INPUT_PADS; ++p) {
        SDL_Gamepad* pad = g_input.pads[p];
        if (!pad) continue;
        for (size_t i = 0; i < sizeof buttons / sizeof *buttons; ++i)
            if (SDL_GetGamepadButton(pad, buttons[i].button)) {
                uint32_t bit = buttons[i].bit;
                if (g_settings.swap_a_b && (bit == SNX_CTR_PAD_A || bit == SNX_CTR_PAD_B))
                    bit ^= SNX_CTR_PAD_A | SNX_CTR_PAD_B;
                input->buttons |= bit;
            }
        float x = axis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = -axis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        float length = sqrtf(x * x + y * y);
        if (length > dead && length > sqrtf(input->circle[0] * input->circle[0] + input->circle[1] * input->circle[1])) {
            /* past the dead zone, scaled so its edge is the stick at rest and the full circle its rim */
            float scale = (fminf(length, 1) - dead) / (1 - dead) / length;
            input->circle[0] = x * scale;
            input->circle[1] = y * scale;
        }
    }
    if (g_input.presses) {
        double ms = (snx_seconds() - g_input.start) * 1000.0;
        input->buttons |= snx_script_held(g_input.script, g_input.presses, ms, (uint32_t)g_settings.press_length);
    }
}
void snx_ctr_input_free(void) {
    for (unsigned i = 0; i < INPUT_PADS; ++i)
        if (g_input.pads[i]) SDL_CloseGamepad(g_input.pads[i]);
    if (g_input.gamepads) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    free(g_input.script);
    memset(&g_input, 0, sizeof g_input);
}
