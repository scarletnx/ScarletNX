/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen controls (vpad.h) in an SDL window: its fingers and its mouse's left button on them, what Auto goes by
   (SDL's touch screens and gamepads) and Android's vibrator for their buzz. For the consoles whose window has no menu
   (snx_vpad_window_*), all of it: the keys that edit their layout, the .ini that keeps it, and their rectangles over
   the picture. Beside SDL's own events, on the thread that reads them */
#ifndef SNX_VPAD_SDL_H
#define SNX_VPAD_SDL_H
#include <SDL3/SDL.h>
#include "common/utilities/ui_rects.h"
#include "common/utilities/vpad.h"

#define SNX_VPAD_MOUSE (~(u64)0) /* the mouse's id among the fingers' */

/* what they ask of SDL before its video starts: a finger and the mouse each reach a window once, not as the other too,
   and Android's Back is a key and leaves nothing */
void snx_vpad_sdl_hints(void);
/* a finger's id, the pen's apart: SDL gives a pen's touch the id 1 on every platform, the id Android and Wayland give a
   first finger and any finger may have, and no real finger's id sets bit 63 */
u64 snx_vpad_sdl_finger(const SDL_TouchFingerEvent* f);
/* a finger's event, or the mouse's left button's or motion, at x, y in the window's pixels: 1 for one that landed on
   the controls (and a lift of the button off them), which the game's touch screen then does not get */
int snx_vpad_sdl_pointer(const SDL_Event* e, float x, float y, const int safe[4]);
/* vpad_auto with the touch screens SDL knows, or a finger seen on one, and the gamepads connected */
void snx_vpad_sdl_auto(int finger_seen);
/* Android's vibrator, which SDL lists as VIBRATOR_SERVICE, for the controls' buzz: NULL for none, and elsewhere (a
   gamepad's motors are no buzz for a finger) */
SDL_Haptic* snx_vpad_sdl_buzzer(void);
void snx_vpad_sdl_buzz(SDL_Haptic* buzzer);

/* the runtime's keys (hotkeys.h: hotkey_quit...) and the Switch's fps_limit among the settings vpad_arg and
   vpad_settings take: before the first vpad_arg. fps_limit's text is read as a Switch game reads it (pace.h), one not
   taken as given said with the pace in effect */
void snx_vpad_sdl_settings(void);
/* a frame's time at the pace fps_limit and F4 give, console_ns the console's own: 0 for no limit */
uint64_t snx_vpad_window_frame_ns(uint64_t console_ns);
/* window's controls, their layout kept in [On-screen controls] of the game's .ini at ini as it is edited, what they say
   on say (the log), and what the runtime's keys say on show too (a line over the picture). After vpad_console and
   vpad_settings */
void snx_vpad_window_open(SDL_Window* window, const char* ini, void (*say)(const char* line),
                          void (*show)(const char* line));
void snx_vpad_window_close(void);
/* window's clear part (its safe area, whole when fullscreen) in its pixels: x, y, w, h */
void snx_vpad_window_safe(int safe[4]);
/* where a finger's event, or the mouse's motion or button, is in the window's pixels: false for the mouse's that SDL made
   of a finger, for one in another window, and where the window has no size */
bool snx_vpad_window_point(const SDL_Event* e, float* x, float* y);
/* an event of the window's: PASS for one that is not the controls' nor a runtime's key, TAKEN for one that is (the game
   gets none of it), QUIT for the quit key's second press within quit_key.h's time (hotkey_quit, Esc), whose first says
   so on say and show, ACTION for another runtime's key, its HOTKEY_ in *action, for the console to do or say it has
   none of (snx_vpad_window_none). The two fullscreen keys switch the window; the menu's key (F2) edits the layout, and
   so does Android's Back, a second one within that time quitting, which the first says over the picture; later, or
   with Esc or the menu's key, it ends the editing */
typedef enum { SNX_VPAD_PASS, SNX_VPAD_TAKEN, SNX_VPAD_QUIT, SNX_VPAD_ACTION } SnxVpadEvent;
SnxVpadEvent snx_vpad_window_event(const SDL_Event* e, int* action);
/* the pause key's (hotkey_pause): the game held while it is on, which the window still shows and takes keys in. Each
   press said on say and show, as a Switch game's ("Pause: on") */
bool snx_vpad_window_paused(void);
/* A console's save states, as its window's keys ask for them: the game's folder with its separator (prefix) and its
   name, whose states are states/<name>.<slot>.snxstate there as a Switch game's, and the console's pair that saves and
   loads game (common/utilities/state_header.h's layout) */
typedef struct {
    const char *prefix, *name;
    void* game;
    int (*save)(void* game, const char* path, const uint8_t* thumb, char* why, size_t whysize);
    int (*load)(void* game, const char* path, char* why, size_t whysize);
} SnxVpadStates;
/* the save states' key action (HOTKEY_SAVE_STATE, F7; HOTKEY_LOAD_STATE, F8; HOTKEY_NEXT_SLOT, F9) done on the game,
   between two of its frames, as a Switch game's: the slot F9 picks, 1 to state_slots, saved (thumb its picture) or
   loaded, the game saved to the auto slot first with state_auto_backup on; each said on say and show */
void snx_vpad_window_states(int action, const SnxVpadStates* states, const uint8_t* thumb);
/* an unattended run's saves and loads, the Switch's state_save and state_load settings ("at:slot,...": at ms of the
   run's clock, a slot's number, auto, or a state's path, as the launcher's Play from a state gives "0:path"), each due
   by ms done on the game between two frames, once, and said as the keys' are */
void snx_vpad_window_states_due(uint64_t ms, const SnxVpadStates* states, const uint8_t* thumb);
/* whether one of them is yet to be done: the picture a save takes is then worth making */
bool snx_vpad_window_states_waiting(void);
/* whether a save of them is due by ms: the picture a save takes is worth making only then */
bool snx_vpad_window_save_due(uint64_t ms);
/* the upscaler's or the anti-aliasing's key (HOTKEY_UPSCALER, HOTKEY_ANTI_ALIASING) stepped to label, for the run (the
   .ini keeps its own), said on say and show as a Switch game's: "Upscaler: FSR 1" */
void snx_vpad_window_effect(int action, const char* label);
/* the runtime's key action said on say and show to do nothing in this window, with why: "F3 (Screenshot) does nothing
   in a GBA game: why" */
void snx_vpad_window_none(int action, const char* why);
/* the window the last key was pressed in (a console's with two windows says over that one), 0 for none */
SDL_WindowID snx_vpad_window_key_window(void);
/* fps_limit's pace as the keys left it (common/utilities/pace.h's SNX_PACE_), for a console that keeps its own clock */
int snx_vpad_window_pace(void);
/* that pace set to the console's clock's, the one F4 then turns off and back to */
void snx_vpad_window_set_pace(int pace);
/* vpad_state, with the buzz they ask for: nothing held while their layout is edited */
void snx_vpad_window_state(u64* buttons, i32 sticks[4]);
/* what the game got of player 1 this frame, from every keyboard, pad and the controls, for the showcase (F6): the
   buttons as the Switch's bits for the console's own (key, each bit's in the console's keys, 0 for none), whole: a bit
   whose console key is held; sticks as the Switch's (-32767 to 32767, up positive), NULL for none */
void snx_vpad_window_showcase(u64 keys, unsigned (*key)(int bit), const i32 sticks[4]);
/* their rectangles over a picture w by h pixels, the window's, a frame presented: with the showcase (F6) and the stats
   (F12) while they are on, as a Switch game's, the GPU's numbers said as unknown. NULL while there is nothing to draw */
const SnxUiRects* snx_vpad_window_rects(int w, int h);
/* those drawn by SDL's renderer, over what it drew */
void snx_vpad_window_render(SDL_Renderer* renderer);
#endif
