/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A 3DS game's gamepads (input/input.c) on SDL's virtual ones, under its dummy video driver: the buttons where the
   console's are, A and B swapped by gamepad_swap_ab, and the circle pad past gamepad_deadzone, scaled to its rim */
#include "3ds/input/input.h"
#include "snx_settings.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int checks;
#define CHECK(e)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(e)) {                                                                                                    \
            fprintf(stderr, "3ds_input.c:%d: check failed: %s\n", __LINE__, #e);                                       \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* the virtual pad, by the vendor and product SDL tells an Xbox pad's layout from: only it is read, a real pad plugged
   in stays out */
#define PAD_VENDOR  0x045e
#define PAD_PRODUCT 0x02ea
#define PAD_IDS     "0x045e/0x02ea"

/* the window's loop (gpu/present.c's pump), for the gamepads */
static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) snx_ctr_input_event(&e);
}
static SnxCtrInput read_input(void) {
    SnxCtrInput input;
    pump();
    snx_ctr_input_read(&input);
    return input;
}
/* a pad with every gamepad button and axis (button i is SDL_GamepadButton i), at rest */
static SDL_Joystick* attach(void) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.vendor_id = PAD_VENDOR;
    d.product_id = PAD_PRODUCT;
    SDL_Joystick* j = SDL_OpenJoystick(SDL_AttachVirtualJoystick(&d));
    CHECK(j != NULL);
    /* a real pad reports its axes at rest as it opens, a virtual one when they are first set; a trigger rests at the
       bottom of the range */
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++)
        SDL_SetJoystickVirtualAxis(j, a, a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? SDL_JOYSTICK_AXIS_MIN : 0);
    pump();
    return j;
}
/* the pad's bits with one button held, nothing held after */
static uint32_t with(SDL_Joystick* j, SDL_GamepadButton button) {
    SDL_SetJoystickVirtualButton(j, button, true);
    uint32_t bits = read_input().buttons;
    SDL_SetJoystickVirtualButton(j, button, false);
    CHECK(read_input().buttons == 0);
    return bits;
}
/* the circle pad with the left stick at x and y of its travel (y up), as the game gets it */
static void circle(SDL_Joystick* j, float x, float y, float out[2]) {
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, (Sint16)lroundf(x * 32767));
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, (Sint16)lroundf(-y * 32767));
    SnxCtrInput input = read_input();
    out[0] = input.circle[0];
    out[1] = input.circle[1];
}
static bool near(float a, float b) { return fabsf(a - b) < 0.002f; }

int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, PAD_IDS);
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
    SDL_Window* window = SDL_CreateWindow("3ds input", 64, 64, 0); /* the dummy driver's, with the focus */
    CHECK(window != NULL);
    snx_ctr_input_init();
    SDL_Joystick* pad = attach();
    float at[2];

    /* the face buttons where the console's are: A right, B bottom, X top, Y left; Start and Back the 3DS's Start and
       Select */
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_EAST) == SNX_CTR_PAD_A && with(pad, SDL_GAMEPAD_BUTTON_SOUTH) == SNX_CTR_PAD_B);
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_NORTH) == SNX_CTR_PAD_X && with(pad, SDL_GAMEPAD_BUTTON_WEST) == SNX_CTR_PAD_Y);
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) == SNX_CTR_PAD_L &&
          with(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) == SNX_CTR_PAD_R);
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_START) == SNX_CTR_PAD_START &&
          with(pad, SDL_GAMEPAD_BUTTON_BACK) == SNX_CTR_PAD_SELECT);
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) == SNX_CTR_PAD_LEFT &&
          with(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) == SNX_CTR_PAD_DOWN);
    /* gamepad_swap_ab: A bottom and B right, X and Y where they were */
    g_settings.swap_a_b = 1;
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_SOUTH) == SNX_CTR_PAD_A && with(pad, SDL_GAMEPAD_BUTTON_EAST) == SNX_CTR_PAD_B);
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_NORTH) == SNX_CTR_PAD_X);
    g_settings.swap_a_b = 0;

    /* gamepad_deadzone's default 20: a stick within a fifth of its travel is at rest, and the rest of the travel spans
       the whole circle, so 60% along x is half way and all of it the rim */
    CHECK(g_settings.stick_dead_zone == 20);
    circle(pad, 0.15f, 0, at);
    CHECK(at[0] == 0 && at[1] == 0);
    circle(pad, 0.6f, 0, at);
    CHECK(near(at[0], 0.5f) && at[1] == 0);
    circle(pad, 0, 1, at);
    CHECK(at[0] == 0 && near(at[1], 1));
    /* a dead zone of 50: 60% is a fifth of the way, 40% at rest */
    g_settings.stick_dead_zone = 50;
    circle(pad, 0.6f, 0, at);
    CHECK(near(at[0], 0.2f));
    circle(pad, -0.4f, 0, at);
    CHECK(at[0] == 0 && at[1] == 0);
    circle(pad, 0, 0, at);
    g_settings.stick_dead_zone = 20;

    SDL_DetachVirtualJoystick(SDL_GetJoystickID(pad));
    pump();
    snx_ctr_input_free();
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("3ds input: %d checks\n", checks);
    return 0;
}
