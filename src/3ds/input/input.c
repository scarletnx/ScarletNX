/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's input for the 3DS game. The keys are a Switch game's (the SDL backend's keyboard_buttons) unless
   ctr_keyboard_map names others: X or Space A, Z B, C X, V Y, Q L, E R, Enter Start, Backspace Select, the arrows the
   D-pad and WASD the circle pad. A gamepad's buttons go by where they are, as on the console (east A, south B, north
   X, west Y) unless ctr_gamepad_map says otherwise, ctr_gamepad_swap_ab swapping A and B, its left stick the circle
   pad past ctr_gamepad_deadzone. The New 3DS's ZL and ZR are 1 and 3 or a pad's triggers, and its C-stick IJKL or a
   pad's right stick. The script (ctr_script) is unattended.c's for a Switch game:
   ms:hex presses, each held for press_len ms */
#include "3ds/input/input.h"
#include "3ds/gpu/screens.h"
#include "3ds/input/map.h"
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
    SnxCtrKeyMap keys; /* ctr_keyboard_map over the default layout */
    SnxPadMap pad;     /* ctr_gamepad_map over the console's, A and B swapped as ctr_gamepad_swap_ab says */
} g_input;

void snx_ctr_input_init(void) {
    g_input.start = snx_seconds();
    char word[64];
    snx_ctr_keys_default(&g_input.keys);
    if (snx_ctr_keys_read(&g_input.keys, g_settings.ctr_keyboard_map, word, sizeof word))
        fprintf(stderr, "scarletnx-3ds: ctr_keyboard_map's \"%s\" is not a button and its keys (a:X,Space)\n", word);
    if (snx_ctr_pad_map(&g_input.pad, g_settings.ctr_gamepad_map, g_settings.ctr_swap_a_b, word, sizeof word))
        fprintf(stderr, "scarletnx-3ds: ctr_gamepad_map's \"%s\" is not a control and the button it sends (south:B)\n",
                word);
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
    SnxCtrScreenRect rects[2];
    snx_ctr_screens_place(l, rects);
    const SnxCtrScreenRect* b = &rects[1];
    if (!b->w) return false; /* no picture, or the bottom screen is not shown */
    double u = (x - b->x) / b->w * l->bottom_w, v = (y - b->y) / b->h * l->bottom_h;
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
    /* a control's SDL button, by where it sits (pad_map.h's order); the triggers are axes */
    static const SDL_GamepadButton buttons[SNX_PAD_CONTROLS] = {
        SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,           SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_NORTH,          SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_INVALID,        SDL_GAMEPAD_BUTTON_INVALID,        SDL_GAMEPAD_BUTTON_LEFT_STICK,
        SDL_GAMEPAD_BUTTON_RIGHT_STICK,    SDL_GAMEPAD_BUTTON_BACK,           SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_DPAD_UP,        SDL_GAMEPAD_BUTTON_DPAD_DOWN,      SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
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
        bool bound[SNX_CTR_BINDS] = {false};
        for (unsigned b = 0; b < SNX_CTR_BINDS; ++b)
            for (unsigned k = 0; k < SNX_CTR_BIND_KEYS; ++k)
                bound[b] |= g_input.keys.key[b][k] && held[g_input.keys.key[b][k]];
        for (unsigned b = 0; b < SNX_CTR_BIND_CPAD_UP; ++b)
            if (bound[b]) input->buttons |= snx_ctr_bind_bit(b);
        input->circle[0] = (float)(bound[SNX_CTR_BIND_CPAD_RIGHT] - bound[SNX_CTR_BIND_CPAD_LEFT]);
        input->circle[1] = (float)(bound[SNX_CTR_BIND_CPAD_UP] - bound[SNX_CTR_BIND_CPAD_DOWN]);
        input->cstick[0] = (float)(bound[SNX_CTR_BIND_CSTICK_RIGHT] - bound[SNX_CTR_BIND_CSTICK_LEFT]);
        input->cstick[1] = (float)(bound[SNX_CTR_BIND_CSTICK_UP] - bound[SNX_CTR_BIND_CSTICK_DOWN]);
    }
    float dead = (float)g_settings.ctr_stick_dead_zone / 100.0f;
    for (unsigned p = 0; p < INPUT_PADS; ++p) {
        SDL_Gamepad* pad = g_input.pads[p];
        if (!pad) continue;
        for (unsigned c = 0; c < SNX_PAD_CONTROLS; ++c) {
            bool down = buttons[c] == SDL_GAMEPAD_BUTTON_INVALID
                            ? SDL_GetGamepadAxis(pad, c == SNX_PAD_LT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                                      : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_PRESSED
                            : SDL_GetGamepadButton(pad, buttons[c]);
            if (down) input->buttons |= snx_ctr_pad_bit(g_input.pad.bit[c]);
        }
        stick(axis(pad, SDL_GAMEPAD_AXIS_LEFTX), -axis(pad, SDL_GAMEPAD_AXIS_LEFTY), dead, input->circle);
        stick(axis(pad, SDL_GAMEPAD_AXIS_RIGHTX), -axis(pad, SDL_GAMEPAD_AXIS_RIGHTY), dead, input->cstick);
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
