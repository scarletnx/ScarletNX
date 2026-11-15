/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A Joy-Con 2 (or a left and right one together) as an SDL virtual gamepad, so that the gamepads' code
   (backend/gamepad.c) gives it a player, a map, its motion and a place in the menus as it does any controller's. SDL
   names a virtual pad's type by its vendor and product ids: a left Joy-Con 2 (0x2067), a right one (0x2066), and the
   pair as Switch Joy-Cons together (0x2008). A single Joy-Con is fed held sideways, as SDL's own driver has one (the
   face buttons gamepad.c's joycon_button_bit reads, the stick hid.c's stick_upright turns back).

   What SDL can't carry goes beside it, by the pad's SDL joystick id: the game's vibration with its frequencies
   (joycon2_rumble, before gamepad.c's own), and the mouse sensors' motion (joycon2_mouse, for hid's Jcms). The buttons
   the Npad has no bit for (C, GL, GR) go nowhere yet. The main thread's alone, no lock */
#ifndef SNX_JOYCON2_SDL_H
#define SNX_JOYCON2_SDL_H
#include "nx/runtime/platform/joycon2_session.h"
#include <SDL3/SDL.h>

#define JC2_PADS 8
/* a single Joy-Con 2, a pair, or a Pro Controller 2 (a full controller to SDL and to the game, fed as the left) */
enum { JC2_PAD_LEFT, JC2_PAD_RIGHT, JC2_PAD_PAIR, JC2_PAD_PRO };

/* SDL's buttons (a bit each, by SDL_GamepadButton) and axes (by SDL_GamepadAxis) for a pad of kind: a single's from
   the one of left and right it is (the other NULL), a pair's from both */
void jc2_sdl_map(int kind, const Jc2Pad* left, const Jc2Pad* right, u32* buttons, i16 axes[SDL_GAMEPAD_AXIS_COUNT]);
/* a motion sample in the Switch's frame (g, rad/s) in SDL's (m/s^2, rad/s): gamepad.c's sample_sensor turned around */
void jc2_sdl_motion(const float accel[3], const float gyro[3], float sdl_accel[3], float sdl_gyro[3]);

/* ---- which controllers make which gamepad (the host's choice, no SDL). A lone controller waits JC2_PAIR_WAIT_NS for a
   partner while pairing is on, so that a left and a right that connect together become one pair at once; one that
   comes later joins a single that is there, as SDL's own driver joins two Joy-Cons (the single's gamepad goes) */
typedef struct {
    int ready;    /* set up and connected */
    int side;     /* JC2_SIDE_* */
    u64 ready_ns; /* when it became ready */
    int had;      /* it had a gamepad before: it waits for no partner again */
    int gamepad;  /* its gamepad now (jc2_pad_attach's), 0 for none */
    int kind;     /* that gamepad's JC2_PAD_* */
} Jc2Join;
#define JC2_PAIR_WAIT_NS 3000000000ull
enum { JC2_JOIN_NONE, JC2_JOIN_SINGLE, JC2_JOIN_PAIR };
/* the next gamepad to make for the n controllers d: JC2_JOIN_PAIR of *a (a left one) and *b (a right one), each out of
   the single it may have, when pairing is on and a left and a right are ready outside a pair, one of them without a
   gamepad; else JC2_JOIN_SINGLE of *a, ready and without a gamepad, when pairing is off, it had one before, it is a
   Pro Controller 2 (which waits for no partner), or it waited JC2_PAIR_WAIT_NS; else JC2_JOIN_NONE */
int jc2_join_next(const Jc2Join* d, int n, int pair, u64 now_ns, int* a, int* b);

/* a virtual gamepad of kind: its index (1 up), 0 when SDL makes none. SDL then says it was added, as for any pad */
int jc2_pad_attach(int kind);
void jc2_pad_detach(int pad);
u32 jc2_pad_id(int pad); /* its SDL joystick id */
/* the state now: the left and right Joy-Con's pads (one NULL for a single), and their sensors' motion since the last
   feed (X, Y, in sensor counts; NULL for none). The motion samples go to SDL when a pad has new reports */
void jc2_pad_feed(int pad, const Jc2Pad* left, const Jc2Pad* right, const i32* mouse_left, const i32* mouse_right);
/* the vibration the game wants of its left (or only) Joy-Con and its right one */
void jc2_pad_rumble(int pad, Jc2Vibration out[2]);
int jc2_pad_player(int pad); /* the player its SDL gamepad is (0 to 7), -1 for none */

/* gamepad.c's, for the pad of SDL joystick id: the game's vibration (PadState.rumble's amplitudes as the player's
   strength left them, the four frequencies as gamepad_rumble_slot has them). 1 when it is a Joy-Con 2's, which then
   plays it */
int joycon2_rumble(u32 id, u32 left, u32 right, const float hz[4]);
/* sdl3.c's: the mouse sensors' motion since the last take, by sensor (PadMotion's order: the left or only one, then
   the right) and X, Y, scaled by joycon2_mouse_scale. 0 when id is no Joy-Con 2's */
int joycon2_mouse(u32 id, float out[2][2]);
/* who pad is for connect.h's players: the controller identity a pair or a single carries over from the gamepad it
   replaces, so it takes that one's player back at once instead of joining as a new controller */
void jc2_pad_who(int pad, const char* who);
/* sdl3.c's: which of a Joy-Con 2 gamepad's mouse sensors see a surface, a bit each in PadMotion's order; -1 when id is
   no Joy-Con 2's */
int joycon2_surface(u32 id);
/* sdl3.c's: the reports per second the right (or only) Joy-Con of a Joy-Con 2 gamepad sent over the last second, 0
   when id is no Joy-Con 2's or before a second has gone by */
int joycon2_rate(u32 id);
/* gamepad.c's who_of: 1 and out when id is a Joy-Con 2's gamepad with an identity */
int joycon2_who(u32 id, char* out, size_t n);
#endif
