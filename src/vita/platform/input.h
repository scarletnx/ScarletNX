/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's input for the Vita game (input.c): the window's keyboard and mouse, SDL's gamepads and the scripted
   presses, in SceCtrl's and SceTouch's terms */
#ifndef SNX_VITA_INPUT_H
#define SNX_VITA_INPUT_H
#include <stdbool.h>
#include <stdint.h>

/* SceCtrl's buttons (psp2common/ctrl.h) */
enum {
    VITA_CTRL_SELECT = 0x1u,
    VITA_CTRL_START = 0x8u,
    VITA_CTRL_UP = 0x10u,
    VITA_CTRL_RIGHT = 0x20u,
    VITA_CTRL_DOWN = 0x40u,
    VITA_CTRL_LEFT = 0x80u,
    VITA_CTRL_LTRIGGER = 0x100u,
    VITA_CTRL_RTRIGGER = 0x200u,
    VITA_CTRL_TRIANGLE = 0x1000u,
    VITA_CTRL_CIRCLE = 0x2000u,
    VITA_CTRL_CROSS = 0x4000u,
    VITA_CTRL_SQUARE = 0x8000u,
    VITA_CTRL_BUTTONS = 0xf3f9u,
};
typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry; /* the sticks, 0 to 255, 128 at rest, y down */
    /* the front panel (the window's left mouse button) and the rear one (its right), in the panels' coordinates:
       1920 by 1088 for the front, 1920 by 890 for the rear */
    bool front, back;
    uint16_t front_x, front_y, back_x, back_y;
} VitaInput;
union SDL_Event;
void vita_input_init(void);
/* the gamepad_map setting (pad_map.h's text), which the command line gives */
void vita_input_map(const char* text);
void vita_input_event(const union SDL_Event* event);
void vita_input_read(VitaInput* input);
void vita_input_free(void);
#endif
