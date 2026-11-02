/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the player's keys and gamepad controls send the 3DS game (the ctr_keyboard_map and ctr_gamepad_map settings),
   without SDL's devices: the keyboard map names a key (SDL's name for it, an _ for a space) for each button and each
   direction of the sticks, and the gamepad map is the Switch games' (common/utilities/pad_map.h) read in the 3DS's
   terms, Plus being Start and Minus Select */
#ifndef SNX_CTR_INPUT_MAP_H
#define SNX_CTR_INPUT_MAP_H
#include "common/utilities/pad_map.h"
#include <stddef.h>
#include <stdint.h>

/* what a key can send: the 14 buttons, then the four directions of the circle pad and of the C-stick */
enum {
    SNX_CTR_BIND_A,
    SNX_CTR_BIND_B,
    SNX_CTR_BIND_X,
    SNX_CTR_BIND_Y,
    SNX_CTR_BIND_L,
    SNX_CTR_BIND_R,
    SNX_CTR_BIND_ZL,
    SNX_CTR_BIND_ZR,
    SNX_CTR_BIND_START,
    SNX_CTR_BIND_SELECT,
    SNX_CTR_BIND_UP,
    SNX_CTR_BIND_DOWN,
    SNX_CTR_BIND_LEFT,
    SNX_CTR_BIND_RIGHT,
    SNX_CTR_BIND_CPAD_UP,
    SNX_CTR_BIND_CPAD_DOWN,
    SNX_CTR_BIND_CPAD_LEFT,
    SNX_CTR_BIND_CPAD_RIGHT,
    SNX_CTR_BIND_CSTICK_UP,
    SNX_CTR_BIND_CSTICK_DOWN,
    SNX_CTR_BIND_CSTICK_LEFT,
    SNX_CTR_BIND_CSTICK_RIGHT,
    SNX_CTR_BINDS
};
#define SNX_CTR_BIND_KEYS 4 /* the keys one binding holds */
typedef struct {
    uint16_t key[SNX_CTR_BINDS][SNX_CTR_BIND_KEYS]; /* SDL scancodes, 0 for none */
} SnxCtrKeyMap;

/* the keys of the default layout */
void snx_ctr_keys_default(SnxCtrKeyMap* map);
/* text, "a:X,Space zl:1 cpad_up:W", over map: a binding named has exactly the keys listed (none after the colon
   unbinds it), the others stay. 0, else -1 with word naming what could not be read, and map as it was */
int snx_ctr_keys_read(SnxCtrKeyMap* map, const char* text, char* word, size_t size);
/* the pad bit (SNX_CTR_PAD_*) binding sends, 0 for a direction of a stick */
uint32_t snx_ctr_bind_bit(unsigned binding);
/* a Switch pad map's button bit (HidNpadButton) as the 3DS's pad bit, 0 for none or one it has no counterpart for */
uint32_t snx_ctr_pad_bit(int button);
/* the gamepad's map: the console's layout by where the controls sit, text over it, A and B swapped when swap. 0, else
   -1 with word naming what could not be read, and the map without text */
int snx_ctr_pad_map(SnxPadMap* map, const char* text, int swap, char* word, size_t size);
#endif
