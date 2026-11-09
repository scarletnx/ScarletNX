/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A 3DS game's gamepads (input/input.c) on SDL's virtual ones, under its dummy video driver: the buttons where the
   console's are, the triggers the New 3DS's ZL and ZR, the Switch's gamepad_map over them, A and B swapped by
   gamepad_swap_ab, and the circle pad and the C-stick past gamepad_deadzone, scaled to their rims */
#include "3ds/input/input.h"
#include "snx_settings.h"
#include "support/sdl_real_pads.h"
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

/* the virtual pad, by the vendor and product SDL tells an Xbox pad's layout from */
#define PAD_VENDOR  0x045e
#define PAD_PRODUCT 0x02ea

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
    sdl_real_pads_hide();
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
    /* the triggers, past a quarter of their travel, are the New 3DS's ZL and ZR (ir:rst's) */
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    CHECK(read_input().buttons == SNX_CTR_PAD_ZL);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    CHECK(read_input().buttons == SNX_CTR_PAD_ZR);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    CHECK(read_input().buttons == 0);
    /* gamepad_swap_ab: A bottom and B right, X and Y where they were */
    g_settings.swap_a_b = 1;
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_SOUTH) == SNX_CTR_PAD_A && with(pad, SDL_GAMEPAD_BUTTON_EAST) == SNX_CTR_PAD_B);
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_NORTH) == SNX_CTR_PAD_X);
    g_settings.swap_a_b = 0;

    /* gamepad_map in the Switch's names: what it names, the rest in place; Minus is Select and Plus Start */
    snx_ctr_input_map("south:X start:Minus back:Plus");
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_SOUTH) == SNX_CTR_PAD_X && with(pad, SDL_GAMEPAD_BUTTON_EAST) == SNX_CTR_PAD_A);
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_START) == SNX_CTR_PAD_SELECT &&
          with(pad, SDL_GAMEPAD_BUTTON_BACK) == SNX_CTR_PAD_START);
    /* the swap after the map: the control that sends A sends B */
    g_settings.swap_a_b = 1;
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_EAST) == SNX_CTR_PAD_B && with(pad, SDL_GAMEPAD_BUTTON_SOUTH) == SNX_CTR_PAD_X);
    g_settings.swap_a_b = 0;
    /* a map that cannot be read changes nothing */
    snx_ctr_input_map("south:Q");
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_SOUTH) == SNX_CTR_PAD_X);
    /* a trigger sends what the map says once past a quarter of its travel; L3, which the 3DS has none of, and None
       send nothing */
    snx_ctr_input_map("lt:A ls:L3 south:None");
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    CHECK(read_input().buttons == SNX_CTR_PAD_A);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK) == 0 && with(pad, SDL_GAMEPAD_BUTTON_SOUTH) == 0);
    /* a map may send ZL from a shoulder */
    snx_ctr_input_map("lb:ZL");
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) == SNX_CTR_PAD_ZL);
    /* an empty map: every control in its place again, and so it stays past the run's end */
    snx_ctr_input_map("");
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_SOUTH) == SNX_CTR_PAD_B);
    snx_ctr_input_map("north:Y");
    snx_ctr_input_free();
    snx_ctr_input_init();
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(pad)); /* a pad connected anew, which the input opens */
    pad = attach();
    CHECK(with(pad, SDL_GAMEPAD_BUTTON_NORTH) == SNX_CTR_PAD_Y);
    snx_ctr_input_map("");

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
    /* the right stick is the C-stick, past the same dead zone */
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX, (Sint16)lroundf(0.6f * 32767));
    SnxCtrInput input = read_input();
    CHECK(near(input.c_stick[0], 0.5f) && input.c_stick[1] == 0 && input.circle[0] == 0 && input.circle[1] == 0);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY, -32767);
    input = read_input();
    CHECK(input.c_stick[0] == 0 && near(input.c_stick[1], 1));
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX, (Sint16)lroundf(0.15f * 32767));
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY, 0);
    input = read_input();
    CHECK(input.c_stick[0] == 0 && input.c_stick[1] == 0);

    SDL_DetachVirtualJoystick(SDL_GetJoystickID(pad));
    pump();
    snx_ctr_input_free();
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("3ds input: %d checks\n", checks);
    return 0;
}
