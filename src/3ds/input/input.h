/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's input for the 3DS game (input.c): the keyboard of the presenter's window, SDL's gamepads and the
   scripted presses, as a Switch game reads them (the SDL backend and unattended.c), in hid:USER's terms */
#ifndef SNX_CTR_INPUT_H
#define SNX_CTR_INPUT_H
#include <stdbool.h>
#include <stdint.h>

/* hid:USER's pad bits (Azahar's PadState, 3dbrew's HID Shared Memory): the circle pad's direction bits are HID's to
   derive from its position. ZL, ZR and the C-stick are the New 3DS's ir:rst, not HID's: its pad state has ZL and ZR in
   bits 14 and 15 (Azahar's ir_rst.h), which HID's never sets */
enum {
    SNX_CTR_PAD_A = 1u << 0,
    SNX_CTR_PAD_B = 1u << 1,
    SNX_CTR_PAD_SELECT = 1u << 2,
    SNX_CTR_PAD_START = 1u << 3,
    SNX_CTR_PAD_RIGHT = 1u << 4,
    SNX_CTR_PAD_LEFT = 1u << 5,
    SNX_CTR_PAD_UP = 1u << 6,
    SNX_CTR_PAD_DOWN = 1u << 7,
    SNX_CTR_PAD_R = 1u << 8,
    SNX_CTR_PAD_L = 1u << 9,
    SNX_CTR_PAD_X = 1u << 10,
    SNX_CTR_PAD_Y = 1u << 11,
    SNX_CTR_PAD_BUTTONS = 0xfffu,
    SNX_CTR_PAD_ZL = 1u << 14,
    SNX_CTR_PAD_ZR = 1u << 15
};
typedef struct {
    uint32_t buttons; /* SNX_CTR_PAD_* held */
    float circle[2];  /* the circle pad, -1 to 1 on each axis, x to the right and y up */
    float c_stick[2]; /* the New 3DS's C-stick, the same */
    bool touch;       /* the touch screen pressed */
    uint32_t touch_x, touch_y; /* the last touch position in the screen's 320x240 pixels, kept after a release */
} SnxCtrInput;
union SDL_Event;
/* The Switch's gamepad_map for the gamepads, as its text ("south:B east:A ..."): the controls it names send those
   buttons, the others the button in their place. A map that cannot be read is said with the map kept, and changes
   nothing; a control sent to a button the 3DS has none of (L3, R3) sends nothing, and is said. It lasts past
   snx_ctr_input_free, as a setting does */
void snx_ctr_input_map(const char* text);
/* The gamepads and the script: from now on scripted presses count their time */
void snx_ctr_input_init(void);
/* An event of the presenter's window pump: a gamepad connected or gone */
void snx_ctr_input_event(const union SDL_Event* event);
/* The presenter's pointer: a press on the touch screen at x and y in its pixels, or a release */
void snx_ctr_input_touch(bool pressed, uint32_t x, uint32_t y);
/* Whether the touch screen is pressed */
bool snx_ctr_input_touched(void);
/* What the player holds now */
void snx_ctr_input_read(SnxCtrInput* input);
/* The console's motion: the first pad whose accelerometer and gyroscope SDL reads (its own, else a left Joy-Con's,
   else a right one's), in the axes and units of Azahar's motion device: accel in g, a pad held up in front of the player
   at rest reading (0, -1, 0), and gyro in degrees a second. False, with both left as they were, when no pad has them */
bool snx_ctr_input_motion(float accel[3], float gyro[3]);
void snx_ctr_input_free(void);
#endif
