/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SDL gamepad discovery and per-player input (backend/gamepad.c). */
#ifndef SNX_GAMEPAD_H
#define SNX_GAMEPAD_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/ini.h"
#include "snx_settings.h"
#include "common/utilities/pad_map.h"
#include "nx/hle/services/hid.h"

/* a gamepad the system added or removed (its SDL joystick id): opened, closed */
void gamepad_open(u32 id);
void gamepad_close(u32 id);
void gamepad_open_waiting(void); /* the connected gamepads no player took yet, opened where one is free now */
/* gamepad_assign=1 (connect.h): a player kept for its controller that left, which the game still sees, and the type
   (SDL_GamepadType) that controller was */
int gamepad_slot_kept(int slot);
int gamepad_slot_kept_type(int slot);
/* each poll, on the main thread, after the input: gamepad_assign=1's pause while a player's controller is gone, its
   notice, and its end when the controller returns or + (plus, held on any controller) is let go. now_ns:
   plat_ns's, the clock the gamepads' arrivals and leavings and the overlay's notices are told by */
void gamepad_console(int plus, u64 now_ns);
/* whether that pause holds the game now. And its notice left to the controller applet's dialog, which shows instead */
int gamepad_console_paused(void);
void gamepad_console_quiet(int on);
/* enumerate connected SDL joysticks at startup, or inspect one newly attached joystick */
void gamepad_scan(void);
void gamepad_detect(u32 id);
/* all open gamepads together, for the game's controller overlay and legacy callers */
void gamepad_state(u64* buttons, i32 sticks[4]);
void gamepad_state_slot(int slot, u64* buttons, i32 sticks[4]);
int gamepad_slot_type(int slot); /* SDL_GamepadType, or SDL_GAMEPAD_TYPE_UNKNOWN */
/* a Joy-Con pair's SL and SR as the game's button bits (24 to 27), which its pad map never sends: 0 for any other */
u64 gamepad_slot_sides(int slot);
int gamepad_slot_connected(int slot);
u32 gamepad_slot_id(int slot); /* SDL joystick instance ID, 0 for an empty slot */
void gamepad_motion(int slot, PadMotion out[2]);
/* the HidNpadButton bit one of a gamepad's buttons (an SDL_GamepadButton) is, as the game reads it; 0 for none */
u64 gamepad_button(u32 id, int button);
/* the name gamepad id's face gives the control that sends the console's button bit (0 A, 1 B...), its player's map
   and A/B swap applied: "B" for A on an Xbox pad, "Circle" on a PlayStation one; a Joy-Con alone, whose buttons turn
   with it, the console's name. Into out (size bytes); 0 with out empty when the pad is gone or no control sends it */
int gamepad_sends_name(u32 id, int bit, char* out, size_t size);
/* legacy combined rumble, played on every open gamepad that can vibrate */
void gamepad_rumble(u32 left, u32 right);
/* one guest player's vibration (hid.h's PadState.rumble), played on that slot's gamepad each poll at now_ns
   (plat_ns). hz: the frequencies the game asked for, in Hz (left low, left high, right low, right high). A Switch
   controller that SDL's HIDAPI driver owns plays them as HD rumble: a Pro Controller each side on its own actuator, a
   single Joy-Con each band of the stronger side on its one. Others play the amplitudes */
void gamepad_rumble_slot(int slot, u32 left, u32 right, const float hz[4], u64 now_ns);
/* the settings are g_settings' (settings.txt: swap_a_b, stick_dead_zone, vibration, background_input, pair_joy_cons,
   button_map), kept in the game's .ini; the game's menu changes them under ui_lock. What gamepad_map reads into: */
typedef struct {
    SnxPadMap own; /* the player's map, SNX_PAD_UNSET where a control sends the button in its place */
} SnxGamepadConfig;
SnxGamepadConfig* gamepad_config(void);
void gamepad_store(const int* setting);    /* one of g_settings' (&g_settings.vibration), to the .ini, and applied */
void gamepad_store_map(void);              /* the custom map, to the .ini */
void gamepad_store_player_map(int player); /* a player's own (0 to SNX_GAMEPADS - 1), to the .ini */
/* the i-th open gamepad's face labels and whether its sticks sit side by side: 1, 0 past the last. Under ui_lock */
int gamepad_layout(int i, int labels[4], int* symmetric);
/* the controls (pad_map.h's) the i-th open gamepad held at the last gamepad_state, a bit each; every one's for -1.
   Under ui_lock */
unsigned gamepad_held(int i);
/* the open gamepads' names, from 0 on: NULL past the last. Under ui_lock */
const char* gamepad_name(int i);
/* counts the changes to those names (a gamepad joined or left), for the menu's page that names them */
u32 gamepad_names_version(void);
/* the i that slot's gamepad (its player's) is in the three above, -1 for none. Under ui_lock */
int gamepad_of_player(int slot);
/* their tables (settings.h): g_gamepad_ini, keys gamepad_*; g_gamepad_map_ini, gamepad_map, the player's map that
   SNX_GAMEPAD_MAP's screen sets; g_gamepad_players_ini, gamepad_p<N>_*, their page in the menus; g_joycon2_ini,
   joycon2*, the Joy-Con 2's (Bluetooth) */
/* each player's own, as Eden has them, in the game's .ini as gamepad_p<N>_* (N from 1); one left at its default takes
   the setting every player shares. Their values are g_settings' arrays by player: player_controller ("" the next to
   connect, else its SDL GUID, "/2" for the second alike), player_connected (0: the game never sees this player; player
   1 it always does), player_type (0: as the controller is; else 1 + the PadKind the game sees), player_swap_a_b (-1:
   gamepad_swap_ab's), player_vibration (-10: gamepad_rumble's), player_button_map. What its map reads into: */
typedef struct {
    int has_map;     /* 0: gamepad_map's */
    SnxPadMap own;   /* its map, SNX_PAD_UNSET where a control sends the button in its place */
} SnxPlayerConfig;
SnxPlayerConfig* gamepad_player(int player); /* 0 to SNX_GAMEPADS - 1 */
int gamepad_player_seen(int slot);           /* whether the game sees the player whose pad is in slot */
int gamepad_player_kind(int slot, int kind); /* the PadKind it sees for that player: its type's, else kind */
/* the controller applet's dialog (ctrl_applet.h): the PadKind the game sees for slot's player whatever its settings
   say (-1: none, as they say), a controller moved from one player to another (the two swapped), and a player's
   controller identified with a short pulse and its player lights */
void gamepad_kind_override(int slot, int kind);
void gamepad_move(int from, int to);
void gamepad_identify(int slot);
/* the slot's controller is about to go because the dialog disconnects it: when it goes, its player is not kept for it
   (gamepad_assign=1), so no pause and no notice waits for it */
void gamepad_drop_quietly(int slot);
int gamepad_ini(const char* key, const char* value); /* a key of the game's .ini: applied, 1; not one of these, 0 */

#endif
