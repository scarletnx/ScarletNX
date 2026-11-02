/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS game's keyboard and gamepad maps (ctr_keyboard_map, ctr_gamepad_map): the default layout, a map over it,
   what a map that cannot be read leaves alone */
#include "3ds/input/input.h"
#include "3ds/input/map.h"
#include <SDL3/SDL_keyboard.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int has(const SnxCtrKeyMap* map, unsigned binding, SDL_Scancode key) {
    for (unsigned k = 0; k < SNX_CTR_BIND_KEYS; ++k)
        if (map->key[binding][k] == key) return 1;
    return 0;
}
int main(void) {
    SnxCtrKeyMap map;
    char word[64];
    snx_ctr_keys_default(&map);
    assert(has(&map, SNX_CTR_BIND_A, SDL_SCANCODE_X) && has(&map, SNX_CTR_BIND_A, SDL_SCANCODE_SPACE));
    assert(has(&map, SNX_CTR_BIND_CPAD_UP, SDL_SCANCODE_W) && has(&map, SNX_CTR_BIND_CSTICK_LEFT, SDL_SCANCODE_J));

    /* a binding named has exactly its keys, the others stay, a space in a name is an underscore */
    assert(!snx_ctr_keys_read(&map, "a:K,Left_Shift zl: cpad_up:Up", word, sizeof word));
    assert(has(&map, SNX_CTR_BIND_A, SDL_SCANCODE_K) && has(&map, SNX_CTR_BIND_A, SDL_SCANCODE_LSHIFT));
    assert(!has(&map, SNX_CTR_BIND_A, SDL_SCANCODE_X));
    assert(!map.key[SNX_CTR_BIND_ZL][0]); /* unbound */
    assert(has(&map, SNX_CTR_BIND_CPAD_UP, SDL_SCANCODE_UP) && !has(&map, SNX_CTR_BIND_CPAD_UP, SDL_SCANCODE_W));
    assert(has(&map, SNX_CTR_BIND_B, SDL_SCANCODE_Z)); /* not named */

    /* a word that cannot be read changes nothing and is named */
    SnxCtrKeyMap before = map;
    assert(snx_ctr_keys_read(&map, "b:Q turbo:X", word, sizeof word) == -1 && !strcmp(word, "turbo:X"));
    assert(!memcmp(&map, &before, sizeof map));
    assert(snx_ctr_keys_read(&map, "b:NoSuchKey", word, sizeof word) == -1 && !strcmp(word, "b:NoSuchKey"));
    assert(snx_ctr_keys_read(&map, "b", word, sizeof word) == -1);
    assert(!memcmp(&map, &before, sizeof map));
    assert(snx_ctr_keys_read(&map, "a:A,B,C,D,E", word, sizeof word) == -1); /* a binding holds four keys */

    /* the pad bits: the buttons have one, the stick directions none */
    assert(snx_ctr_bind_bit(SNX_CTR_BIND_START) == SNX_CTR_PAD_START && snx_ctr_bind_bit(SNX_CTR_BIND_CPAD_UP) == 0);

    /* the gamepad's console layout: east is A, south B, the shoulders L and R, the triggers ZL and ZR, back Select */
    SnxPadMap pad;
    assert(!snx_ctr_pad_map(&pad, "", 0, word, sizeof word));
    assert(snx_ctr_pad_bit(pad.bit[SNX_PAD_EAST]) == SNX_CTR_PAD_A && snx_ctr_pad_bit(pad.bit[SNX_PAD_SOUTH]) == SNX_CTR_PAD_B);
    assert(snx_ctr_pad_bit(pad.bit[SNX_PAD_LB]) == SNX_CTR_PAD_L && snx_ctr_pad_bit(pad.bit[SNX_PAD_RT]) == SNX_CTR_PAD_ZR);
    assert(snx_ctr_pad_bit(pad.bit[SNX_PAD_BACK]) == SNX_CTR_PAD_SELECT && snx_ctr_pad_bit(pad.bit[SNX_PAD_START]) == SNX_CTR_PAD_START);
    assert(snx_ctr_pad_bit(pad.bit[SNX_PAD_LS]) == 0); /* the stick clicks have no 3DS button */
    assert(snx_ctr_pad_bit(pad.bit[SNX_PAD_UP]) == SNX_CTR_PAD_UP && snx_ctr_pad_bit(pad.bit[SNX_PAD_RIGHT]) == SNX_CTR_PAD_RIGHT);

    /* swapped, and a map over it: the control named sends what it says, the others keep the layout */
    assert(!snx_ctr_pad_map(&pad, "", 1, word, sizeof word));
    assert(snx_ctr_pad_bit(pad.bit[SNX_PAD_EAST]) == SNX_CTR_PAD_B && snx_ctr_pad_bit(pad.bit[SNX_PAD_SOUTH]) == SNX_CTR_PAD_A);
    assert(!snx_ctr_pad_map(&pad, "rs:Plus lb:None", 0, word, sizeof word));
    assert(snx_ctr_pad_bit(pad.bit[SNX_PAD_RS]) == SNX_CTR_PAD_START && snx_ctr_pad_bit(pad.bit[SNX_PAD_LB]) == 0);
    assert(snx_ctr_pad_bit(pad.bit[SNX_PAD_EAST]) == SNX_CTR_PAD_A);
    /* a map that cannot be read leaves the layout and is named */
    assert(snx_ctr_pad_map(&pad, "south:Turbo", 0, word, sizeof word) == -1 && word[0]);
    assert(snx_ctr_pad_bit(pad.bit[SNX_PAD_SOUTH]) == SNX_CTR_PAD_B);
    puts("ok");
    return 0;
}
