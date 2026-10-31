/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's input for the 3DS game. The keys are a Switch game's (the SDL backend's keyboard_buttons): X or Space
   A, Z B, C X, V Y, Q L, E R, Enter Start, Backspace Select, the arrows the D-pad and WASD the circle pad. A gamepad's
   buttons go by where they are, as on the console (east A, south B, north X, west Y), gamepad_swap_ab swapping A and
   B, its left stick the circle pad past gamepad_deadzone. The New 3DS's ZL and ZR are 1 and 3 or a pad's triggers, and
   its C-stick IJKL or a pad's right stick. The script (ctr_script) is unattended.c's for a Switch game:
   ms:hex presses, each held for press_len ms */
#include "3ds/input/input.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/script.h"
#include "snx_settings.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_PADS 4
/* a trigger's axis (0 to 32767) past which ZL or ZR is held: half way */
#define TRIGGER_PRESSED 16384

/* The touch screen: bit 31 pressed, the pixel's y in bits 16 to 30 and x in bits 0 to 15, in one word so that a read
   gets both of them (the window's pump writes it and hid's update reads it, on other threads) */
static uint32_t g_touch;

static struct {
    SDL_Gamepad* pads[INPUT_PADS];
    SnxPress* script; /* ms after snx_ctr_input_init, SNX_CTR_PAD_* */
    size_t presses;
    double start; /* snx_seconds() at snx_ctr_input_init */
    bool gamepads;
} g_input;

void snx_ctr_input_init(void) {
    g_input.start = snx_seconds();
    g_input.presses = snx_script_parse("scarletnx-3ds", g_settings.ctr_script,
                                      SNX_CTR_PAD_BUTTONS | SNX_CTR_PAD_NEW, &g_input.script);
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
                /* the motion sensors, for a pad that has both: they report nothing until enabled */
                SDL_Gamepad* pad = g_input.pads[i];
                if (pad && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
                    (void)SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
                    (void)SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
                }
                break;
            }
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED)
        for (unsigned i = 0; i < INPUT_PADS; ++i)
            if (g_input.pads[i] && SDL_GetGamepadID(g_input.pads[i]) == event->gdevice.which) {
                SDL_CloseGamepad(g_input.pads[i]);
                g_input.pads[i] = NULL;
            }
}
bool snx_ctr_touch_locate(const SnxCtrWindowLayout* l, double x, double y, unsigned* px, unsigned* py) {
    *px = *py = 0;
    unsigned layout_w = snx_max_u32(l->top_w, l->bottom_w), layout_h = l->top_h + l->bottom_h;
    if (!l->image_w || !l->image_h || !layout_w || !l->bottom_w || !l->bottom_h) return false;
    double scale = fmin((double)l->image_w / layout_w, (double)l->image_h / layout_h);
    /* the bottom screen's corner, where show_screens blits it: under the top screen, centred across the pair */
    double left = (l->image_w - layout_w * scale) / 2 + (layout_w - l->bottom_w) * scale / 2;
    double top = (l->image_h - layout_h * scale) / 2 + l->top_h * scale;
    double u = (x - left) / scale, v = (y - top) / scale;
    *px = (unsigned)snx_clamp_double(floor(u), 0, l->bottom_w - 1);
    *py = (unsigned)snx_clamp_double(floor(v), 0, l->bottom_h - 1);
    return u >= 0 && v >= 0 && u < l->bottom_w && v < l->bottom_h;
}
void snx_ctr_input_touch(bool down, unsigned x, unsigned y) {
    uint32_t state = down ? 1u << 31 | (y & 0x7fffu) << 16 | (x & 0xffffu) : 0;
    __atomic_store_n(&g_touch, state, __ATOMIC_RELAXED);
}
void snx_ctr_input_motion(const float sdl_accel[3], const float sdl_gyro[3], SnxCtrInput* input) {
    const float degrees = 180.0f / 3.14159265358979f;
    input->motion = true;
    input->accel[0] = sdl_accel[0] / SDL_STANDARD_GRAVITY;
    input->accel[1] = -sdl_accel[1] / SDL_STANDARD_GRAVITY;
    input->accel[2] = sdl_accel[2] / SDL_STANDARD_GRAVITY;
    input->gyro[0] = -sdl_gyro[0] * degrees;
    input->gyro[1] = sdl_gyro[1] * degrees;
    input->gyro[2] = -sdl_gyro[2] * degrees;
}
/* A stick's position (x to the right and y up) as held: past the dead zone, scaled so that its edge is the stick at rest
   and the full circle its rim. It replaces held when it is the longer, so that of several pads (and the keys) the
   furthest wins */
static void stick(float x, float y, float dead, float held[2]) {
    float length = sqrtf(x * x + y * y);
    if (length > dead && length > sqrtf(held[0] * held[0] + held[1] * held[1])) {
        float scale = (fminf(length, 1) - dead) / (1 - dead) / length;
        held[0] = x * scale;
        held[1] = y * scale;
    }
}
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
    uint32_t touch = __atomic_load_n(&g_touch, __ATOMIC_RELAXED);
    if (touch >> 31) {
        input->touched = true;
        input->touch[0] = (uint16_t)(touch & 0xffffu);
        input->touch[1] = (uint16_t)(touch >> 16 & 0x7fffu);
    }
    /* the window's keys, which SDL keeps for a window with the focus: none without a window */
    const bool* held = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (held) {
        for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i)
            if (held[keys[i].key]) input->buttons |= keys[i].bit;
        input->circle[0] = (float)(held[SDL_SCANCODE_D] - held[SDL_SCANCODE_A]);
        input->circle[1] = (float)(held[SDL_SCANCODE_W] - held[SDL_SCANCODE_S]);
        input->cstick[0] = (float)(held[SDL_SCANCODE_L] - held[SDL_SCANCODE_J]);
        input->cstick[1] = (float)(held[SDL_SCANCODE_I] - held[SDL_SCANCODE_K]);
        if (held[SDL_SCANCODE_1]) input->buttons |= SNX_CTR_PAD_ZL;
        if (held[SDL_SCANCODE_3]) input->buttons |= SNX_CTR_PAD_ZR;
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
        stick(axis(pad, SDL_GAMEPAD_AXIS_LEFTX), -axis(pad, SDL_GAMEPAD_AXIS_LEFTY), dead, input->circle);
        stick(axis(pad, SDL_GAMEPAD_AXIS_RIGHTX), -axis(pad, SDL_GAMEPAD_AXIS_RIGHTY), dead, input->cstick);
        /* the triggers are the New 3DS's ZL and ZR, as far as they are held */
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_PRESSED) input->buttons |= SNX_CTR_PAD_ZL;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_PRESSED) input->buttons |= SNX_CTR_PAD_ZR;
        /* the first pad with a motion sensor is the console's */
        float accel[3], gyro[3];
        if (!input->motion && SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, accel, 3) &&
            SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, gyro, 3))
            snx_ctr_input_motion(accel, gyro, input);
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
    snx_ctr_input_touch(false, 0, 0);
}
