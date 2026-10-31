/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's input for the 3DS game (input.c): the keyboard of the presenter's window, SDL's gamepads and the
   scripted presses, as a Switch game reads them (the SDL backend and unattended.c), in hid:USER's terms */
#ifndef SNX_CTR_INPUT_H
#define SNX_CTR_INPUT_H
#include <stdbool.h>
#include <stdint.h>

/* hid:USER's pad bits (Azahar's PadState, 3dbrew's HID Shared Memory): the circle pad's direction bits are HID's to
   derive from its position, and ZL, ZR and the C-stick are the New 3DS's ir:rst, not HID's */
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
    SNX_CTR_PAD_BUTTONS = 0xfffu
};
#define SNX_CTR_TOUCH_WIDTH  320u /* the bottom screen's pixels, which the touch screen reports in */
#define SNX_CTR_TOUCH_HEIGHT 240u
typedef struct {
    uint32_t buttons; /* SNX_CTR_PAD_* held */
    float circle[2];  /* the circle pad, -1 to 1 on each axis, x to the right and y up */
    bool touched;     /* the touch screen is pressed, at the bottom screen's pixel touch: x from the left, y from the top */
    uint16_t touch[2];
} SnxCtrInput;
/* The window's picture as the presenter draws it (gpu/present.c): an image of image_w by image_h pixels with the top
   screen above the bottom one, both centred, the pair scaled as one to fit. The screens' sizes are the frames' */
typedef struct {
    unsigned image_w, image_h, top_w, top_h, bottom_w, bottom_h;
} SnxCtrWindowLayout;
/* Where the point (x, y) of the window's image is on the bottom screen, as its pixel in px and py, held inside the
   screen. True when the point is on the screen, false when it is not (px and py are the nearest pixel then, 0 and 0
   with no picture) */
bool snx_ctr_touch_locate(const SnxCtrWindowLayout* layout, double x, double y, unsigned* px, unsigned* py);
/* The touch screen pressed at the bottom screen's pixel (x, y), or let go. Any thread: the window's pump sets it and
   hid's update reads it */
void snx_ctr_input_touch(bool down, unsigned x, unsigned y);
union SDL_Event;
/* The gamepads and the script: from now on scripted presses count their time */
void snx_ctr_input_init(void);
/* An event of the presenter's window pump: a gamepad connected or gone */
void snx_ctr_input_event(const union SDL_Event* event);
/* What the player holds now */
void snx_ctr_input_read(SnxCtrInput* input);
void snx_ctr_input_free(void);
#endif
