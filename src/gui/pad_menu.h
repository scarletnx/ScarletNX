/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the menu's gamepads: each press the key the menu takes for it, sent as a key down from GUI_PAD_KEYBOARD, the
   arrows and the pages again while held. The menu goes on taking the keyboard, the mouse and the fingers alike */
#ifndef SNX_GUI_PAD_MENU_H
#define SNX_GUI_PAD_MENU_H
#include "common/utilities/pad_map.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

#define GUI_PAD_KEYBOARD ((SDL_KeyboardID)0xFFFFFFFFu) /* the keyboard a pad's key comes from, no real one's */
#define GUI_PAD_DELAY_MS 400                           /* a held arrow's first repeat */
#define GUI_PAD_RATE_MS  80                            /* and the next ones' */
#define GUI_PAD_STICK    16384                         /* how far a stick goes before it presses */
#define GUI_PADS_MAX     8

/* the right stick pushed past GUI_PAD_STICK, any way: a control of the menu's alone, past pad_map.h's (a game's right
   stick is no button) */
#define GUI_PAD_RIGHT_STICK SNX_PAD_CONTROLS

typedef struct {
    SDL_Gamepad* pads[GUI_PADS_MAX]; /* every pad plugged in, open, count of them */
    int count;
    bool started;    /* SDL's gamepads started for the menu */
    unsigned axes;   /* the controls the sticks (the left one the D-pad's) and the triggers hold, a bit each */
    int held;        /* the control held that repeats, a button's or an axis', -1 for none */
    SDL_Keycode key; /* and its key */
    Uint64 next_ms;  /* when it repeats */
    int labels[4];   /* the face labels of the pad pressed last (snx_pad_labels'), which the foot names */
} GuiPads;

/* the key and its modifiers a control presses (pad_map.h's SNX_PAD_), its face's labels (snx_pad_labels', of south,
   east, west and north) saying which button selects: the one labelled A or Cross, B or Circle going back, X or Square
   Del and Y or Triangle R, L3 M and R3 B, Back F1 (ESC on Android, as its back), the right stick L. Whether it presses
   one; *repeats, whether it is pressed again while held */
bool gui_pad_key(int control, const int labels[4], SDL_Keycode* key, SDL_Keymod* mod, bool* repeats);
/* a foot's sentences (main.c's "Keys: what. Keys: what.") in out, their keys A, B, X and Y named as labels (the pad
   pressed last's) name them: Cross, Circle, Square and Triangle on a PlayStation pad */
void gui_pad_keys_named(const char* keys, const int labels[4], char* out, size_t size);
/* SDL's gamepads started, every pad there opened; whether they could be */
bool gui_pads_start(GuiPads* p);
/* every pad closed and SDL's gamepads ended for the menu */
void gui_pads_stop(GuiPads* p);
/* a pad plugged in opened, one taken out closed, a press (a button, the left stick or a trigger past its way) the key
   down it is in *key, for the menu to take as the keyboard's. Whether there is one */
bool gui_pads_event(GuiPads* p, const SDL_Event* e, Uint64 now_ms, SDL_Event* key);
/* the arrow or the page held, pressed again once its time has come, in *key: whether it is */
bool gui_pads_repeat(GuiPads* p, Uint64 now_ms, SDL_Event* key);
#endif
