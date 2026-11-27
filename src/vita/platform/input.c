/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's input for the Vita game, as the 3DS driver's (3ds/input/input.c) by where the buttons are: Z or Space
   Cross (south), X Circle (east), V Square (west), C Triangle (north), Q L, E R, Enter Start, Backspace Select, the
   arrows the D-pad, WASD the left stick and IJKL the right (pad_sdl.h's keys of a Switch game, Space added). The
   fingers on the picture, off the on-screen controls, touch the front panel, and so does the window's mouse's left
   button, as the touch setting lets each; its right button touches the rear one. A gamepad's
   controls are a Switch game's pad's, as on the 3DS: each sends the button where it is, gamepad_map over that, then
   gamepad_swap_ab swapping Cross and Circle; its sticks are the Vita's, each past gamepad_deadzone. The script
   (vita_script) is ms:hex presses, each held for vita_press_len ms, its bits 24 to 27 the left stick pushed right,
   left, up or down */
#include "vita/platform/input.h"
#include "common/utilities/common.h"
#include "common/utilities/pad_sdl.h"
#include "common/utilities/script.h"
#include "common/utilities/vpad_sdl.h"
#include "vita/gpu/vk.h"
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
static unsigned vita_key(int bit);
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
static unsigned vita_key(int bit) { return VITA_BITS[bit]; }

static struct {
    SDL_Gamepad* pads[INPUT_PADS];
    SnxPress* script;
    size_t presses;
    double start;
    bool gamepads;
    /* the front panel's touches, each in its slot (its id) while it stays down: which finger (the mouse's
       SNX_VPAD_MOUSE) and where, in the window's pixels */
    struct {
        bool down;
        u64 finger;
        float x, y;
    } front[VITA_FRONT_TOUCHES];
    u64 pressed; /* the on-screen controls', the Switch's bits, and their sticks */
    i32 sticks[4];
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
/* a finger's event, or the left button's, on the front panel, as the touch setting lets it: 0 none, 1 the fingers, 2
   the fingers and the mouse. A press off the picture starts nothing */
static void front_event(const SDL_Event* e) {
    int rect[4], slot = -1;
    float x, y;
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) /* this window may not get the lifts */
        for (int i = 0; i < VITA_FRONT_TOUCHES; ++i) g_input.front[i].down = false;
    bool finger = e->type >= SDL_EVENT_FINGER_DOWN && e->type <= SDL_EVENT_FINGER_CANCELED;
    if (!snx_vpad_window_point(e, &x, &y)) return;
    if ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        e->button.button != SDL_BUTTON_LEFT)
        return;
    u64 id = finger ? snx_vpad_sdl_finger(&e->tfinger) : SNX_VPAD_MOUSE;
    for (int i = 0; i < VITA_FRONT_TOUCHES && slot < 0; ++i)
        if (g_input.front[i].down && g_input.front[i].finger == id) slot = i;
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_FINGER_DOWN) {
        if (slot >= 0 || g_settings.touch_screen < (finger ? 1 : 2) || !vita_vk_picture(rect) || x < (float)rect[0] ||
            y < (float)rect[1] || x >= (float)(rect[0] + rect[2]) || y >= (float)(rect[1] + rect[3]))
            return;
        for (int i = VITA_FRONT_TOUCHES - 1; i >= 0; --i)
            if (!g_input.front[i].down) slot = i;
        if (slot < 0) return; /* the panel takes no more */
        g_input.front[slot].down = true;
        g_input.front[slot].finger = id;
    } else if (slot >= 0 && e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_MOUSE_MOTION)
        g_input.front[slot].down = false;
    if (slot >= 0) g_input.front[slot].x = x, g_input.front[slot].y = y;
}
void vita_input_event(const union SDL_Event* event) {
    snx_pads_event(g_input.pads, INPUT_PADS, event, NULL);
    front_event(event);
}
void vita_input_sample(void) { snx_vpad_window_state(&g_input.pressed, g_input.sticks); }
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
        /* the mouse over the window: its place scaled to the rear panel's */
        float x, y;
        SDL_MouseButtonFlags mouse = SDL_GetMouseState(&x, &y);
        SDL_Window* window = SDL_GetMouseFocus();
        int w = 0, h = 0;
        if (window && SDL_GetWindowSize(window, &w, &h) && w > 0 && h > 0 && (mouse & SDL_BUTTON_RMASK)) {
            float u = fminf(fmaxf(x / (float)w, 0), 1), v = fminf(fmaxf(y / (float)h, 0), 1);
            input->back = true;
            input->back_x = (uint16_t)lrintf(u * 1919), input->back_y = (uint16_t)lrintf(v * 889);
        }
    }
    /* the front panel's touches, on the picture's, kept to its edges as they leave it */
    int rect[4];
    for (unsigned i = 0; i < VITA_FRONT_TOUCHES && vita_vk_picture(rect); ++i) {
        if (!g_input.front[i].down) continue;
        float u = fminf(fmaxf((g_input.front[i].x - (float)rect[0]) / (float)rect[2], 0), 1);
        float v = fminf(fmaxf((g_input.front[i].y - (float)rect[1]) / (float)rect[3], 0), 1);
        input->front[input->fronts].id = (uint8_t)i;
        input->front[input->fronts].x = (uint16_t)lrintf(u * 1919);
        input->front[input->fronts].y = (uint16_t)lrintf(v * 1087);
        ++input->fronts;
    }
    /* the on-screen controls: their sticks over the keys' while held */
    for (int b = 0; b < SNX_PAD_BUTTONS; ++b)
        if (g_input.pressed >> b & 1) input->buttons |= VITA_BITS[b];
    for (unsigned s = 0; s < 4; s += 2)
        if (g_input.sticks[s] || g_input.sticks[s + 1]) {
            sticks[s] = (float)g_input.sticks[s] / 32767;
            sticks[s + 1] = -(float)g_input.sticks[s + 1] / 32767;
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
    /* the layout edited: the player's hands are on it, and the script's presses alone reach the game */
    if (vpad_editing()) {
        *input = (VitaInput){0};
        for (unsigned s = 0; s < 4; ++s) sticks[s] = 0;
    }
    /* the script's presses: buttons, and the left stick pushed all the way by bits 24 to 27 (right, left, up, down) */
    if (g_input.presses) {
        double ms = (snx_seconds() - g_input.start) * 1000.0;
        uint32_t mask =
            (uint32_t)snx_script_held(g_input.script, g_input.presses, ms, (uint32_t)g_settings.vita_press_length);
        input->buttons |= mask & SCRIPT_BUTTONS;
        if (mask & SCRIPT_STICK_RIGHT) sticks[0] = 1;
        if (mask & SCRIPT_STICK_LEFT) sticks[0] = -1;
        if (mask & SCRIPT_STICK_UP) sticks[1] = -1;
        if (mask & SCRIPT_STICK_DOWN) sticks[1] = 1;
    }
    input->lx = stick(sticks[0]), input->ly = stick(sticks[1]);
    input->rx = stick(sticks[2]), input->ry = stick(sticks[3]);
    {
        i32 shown[4]; /* the Switch's way, up positive */
        for (unsigned s = 0; s < 4; ++s)
            shown[s] = (i32)lrintf(fminf(fmaxf(sticks[s], -1), 1) * (s & 1 ? -32767 : 32767));
        snx_vpad_window_showcase(input->buttons, vita_key, shown);
    }
}
void vita_input_free(void) {
    snx_pads_close(g_input.pads, INPUT_PADS);
    if (g_input.gamepads) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    free(g_input.script);
    SnxPadMap map = g_input.map; /* a setting, not the run's: it stays */
    memset(&g_input, 0, sizeof g_input);
    g_input.map = map;
}
