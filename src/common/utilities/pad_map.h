/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What each control of a gamepad sends the game, as the console's buttons: by where they sit on the console's
   controller, A and B swapped or not (gamepad_swap_ab), over which a map the player made (the .ini's gamepad_map) sets
   what it names. The runtime's gamepads
   (src/nx/runtime/platform/backend/gamepad.c) and the button map screens of the launcher and the game's menu
   (pad_screen.h) read it; no SDL here, each of them knows its pad */
#ifndef SNX_PAD_MAP_H
#define SNX_PAD_MAP_H
#include <stddef.h>

/* a gamepad's controls by where they sit, SDL's way: south is the bottom face button */
enum {
    SNX_PAD_SOUTH,
    SNX_PAD_EAST,
    SNX_PAD_WEST,
    SNX_PAD_NORTH,
    SNX_PAD_LB,
    SNX_PAD_RB,
    SNX_PAD_LT,
    SNX_PAD_RT,
    SNX_PAD_LS,
    SNX_PAD_RS,
    SNX_PAD_BACK,
    SNX_PAD_START,
    SNX_PAD_UP,
    SNX_PAD_DOWN,
    SNX_PAD_LEFT,
    SNX_PAD_RIGHT,
    SNX_PAD_CONTROLS
};
#define SNX_PAD_BUTTONS 16 /* the console's buttons, HidNpadButton bits 0 (A) to 15 (Down) */
#define SNX_PAD_NONE    (-1)
#define SNX_PAD_UNSET   (-2) /* a custom map's control it does not name: as the pad's labels say */
/* the labels a pad's face buttons show (SDL_GamepadButtonLabel's) */
enum {
    SNX_PAD_LABEL_NONE,
    SNX_PAD_LABEL_A,
    SNX_PAD_LABEL_B,
    SNX_PAD_LABEL_X,
    SNX_PAD_LABEL_Y,
    SNX_PAD_LABEL_CROSS,
    SNX_PAD_LABEL_CIRCLE,
    SNX_PAD_LABEL_SQUARE,
    SNX_PAD_LABEL_TRIANGLE
};
typedef struct {
    signed char bit[SNX_PAD_CONTROLS]; /* the console's button each control sends, SNX_PAD_NONE for none */
} SnxPadMap;

/* a control's name in the .ini ("south", "lt"), and as a pad shows it with face labels (SNX_PAD_LABEL_ of south,
   east, west and north; NULL for an Xbox pad's): "A", "LT"... */
const char* snx_pad_control_key(int control);
const char* snx_pad_control_name(int control, const int* labels);
/* a console button's name ("A", "ZL", "Plus"), "None" for SNX_PAD_NONE */
const char* snx_pad_button_name(int bit);
/* the console's layout: each control the button in its place */
void snx_pad_map_position(SnxPadMap* map);
/* A sent where B was and B where A was, whichever controls send them */
void snx_pad_map_swap_ab(SnxPadMap* map);
/* every control SNX_PAD_UNSET; the unset ones of map taken from from */
void snx_pad_map_unset(SnxPadMap* map);
void snx_pad_map_fill(SnxPadMap* map, const SnxPadMap* from);
/* text, "south:B east:A ...", over map: each control named once at most, the others left. 0, else -1 with error
   naming the word it could not read, and map as it was */
int snx_pad_map_read(SnxPadMap* map, const char* text, char* error, size_t size);
/* map as text for snx_pad_map_read: every control it sets */
void snx_pad_map_write(const SnxPadMap* map, char* out, size_t size);
/* the control sending bit, -1 for none */
int snx_pad_map_find(const SnxPadMap* map, int bit);
#define SNX_PAD_MAP_TEXT 192 /* room for snx_pad_map_write's text: 165 at most */
/* what each control sends: the button in its place, own (a map whose unset controls take that) over it, then A and B
   swapped when swap_ab is set */
void snx_pad_map_sent(SnxPadMap* map, const SnxPadMap* own, int swap_ab);
/* The gamepad_map setting's text into *own, for a console whose button for each of the console buttons above is
   bits[] (0 where it has none, console naming it). A text that does not read keeps *own and says why on stderr, as
   does each control that sends a button the console lacks */
void snx_pad_map_setting(SnxPadMap* own, const char* text, const unsigned bits[SNX_PAD_BUTTONS], const char* console);
/* A stick at x and y past a round dead zone of radius zone, its rim at rim (1, or 32767 for SDL's axes): scaled so the
   zone's edge is the stick at rest and the rim stays the rim, so a stick pushed along one axis keeps the small part of
   the other. 0 with x and y zeroed when it is inside the zone, else 1 */
int snx_pad_dead_zone(double* x, double* y, double zone, double rim);
#endif
