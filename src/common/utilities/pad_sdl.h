/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* pad_map.h's controls and the motion sensors on SDL's gamepads, for the runtimes (the Switch's backend/gamepad.c, the
   3DS's input/input.c) and the launcher (cmake/pad_sdl.cmake) */
#ifndef SNX_PAD_SDL_H
#define SNX_PAD_SDL_H
#include <SDL3/SDL.h>
#include "common/utilities/pad_map.h"

#define SNX_PAD_TRIGGER 8000 /* how far a trigger goes before it is held */
/* the control an SDL button is, -1 for none (Home, the extra ones) */
int snx_pad_of_button(int button);
/* whether g holds control now */
int snx_pad_held(SDL_Gamepad* g, int control);
/* g's face labels (SNX_PAD_LABEL_ of south, east, west, north), and whether its sticks sit side by side */
void snx_pad_labels(SDL_Gamepad* g, int labels[4]);
int snx_pad_symmetric(SDL_Gamepad* g);
/* The player a pad takes as it is plugged in, as the games give them their pads: the first free one whose controller
   names it (its SDL GUID's text, "/2" after it for the second alike, "/3" the third), else the first free one that
   names none (NULL or ""); -1 for none. alike: the pads with its GUID that took a player before it; players: each
   player's controller (gamepad_p<N>_device), count of them; taken: a bit for each player that has a pad */
int snx_pad_slot(const char* guid, int alike, const char* const* players, int count, unsigned taken);
/* a pad's motion sensors, as SDL has them: its own accelerometer and gyroscope, a left Joy-Con's and a right one's */
enum { SNX_PAD_MOTION_MAIN, SNX_PAD_MOTION_LEFT, SNX_PAD_MOTION_RIGHT, SNX_PAD_MOTIONS };
/* Turns on every motion sensor g has, saying each through say (a line without its newline: warn set with SDL's error
   when one would not turn on), and that it has none */
void snx_pad_motion_enable(SDL_Gamepad* g, void (*say)(int warn, const char* line));
/* The accelerometer and gyroscope of g's pair which (SNX_PAD_MOTION_*) as SDL reads them: m/s^2 and rad/s, x to the
   right, y up and z toward the player (SDL_sensor.h). 0 when either is off or has nothing to read */
int snx_pad_motion_read(SDL_Gamepad* g, int which, float accel[3], float gyro[3]);
/* An added gamepad of event opened into the first free one of count pads (opened, when not NULL, told of it), a
   removed one closed; another event is left. snx_pads_close closes every pad open */
void snx_pads_event(SDL_Gamepad** pads, int count, const SDL_Event* event, void (*opened)(SDL_Gamepad* pad));
void snx_pads_close(SDL_Gamepad** pads, int count);
/* a stick's axis, -1 to 1 */
float snx_pad_axis(SDL_Gamepad* g, SDL_GamepadAxis which);
/* The console buttons (a bit for each of pad_map.h's) that the window's keys hold, as a Switch game's keyboard has
   them: X A, Z B, C X, V Y, Q L, E R, 1 ZL, 3 ZR, Enter Plus (not with Alt, which is the window's fullscreen),
   Backspace Minus and the arrows the D-pad. held is SDL_GetKeyboardState's */
unsigned snx_pad_keys(const bool* held);
/* WASD as the left stick and IJKL as the right one, -1 to 1 with y up */
void snx_pad_key_sticks(const bool* held, float left[2], float right[2]);
#endif
