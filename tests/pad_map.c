/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a gamepad's controls send (src/common/utilities/pad_map.c): by position, A and B swapped, and the player's own
   map as gamepad_map writes it, read back word by word, a bad word naming itself and leaving the map */
#include "common/utilities/pad_map.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { A, B, X, Y, L3, R3, L, R, ZL, ZR, PLUS, MINUS, LEFT, UP, RIGHT, DOWN };
/* a console with A, B and L alone, a bit each */
static unsigned two_buttons(int bit) { return bit == A ? 1u : bit == B ? 2u : bit == L ? 4u : 0u; }

int main(void) {
    static const int xbox[4] = {SNX_PAD_LABEL_A, SNX_PAD_LABEL_B, SNX_PAD_LABEL_X, SNX_PAD_LABEL_Y};
    static const int sony[4] = {SNX_PAD_LABEL_CROSS, SNX_PAD_LABEL_CIRCLE, SNX_PAD_LABEL_SQUARE,
                                SNX_PAD_LABEL_TRIANGLE};
    SnxPadMap map, own, placed;
    char text[SNX_PAD_MAP_TEXT], error[64];
    int c;

    /* by position, as the console's controller has them: A right, B bottom, X top, Y left; the rest by their place */
    snx_pad_map_position(&map);
    assert(map.bit[SNX_PAD_SOUTH] == B && map.bit[SNX_PAD_EAST] == A && map.bit[SNX_PAD_NORTH] == X &&
           map.bit[SNX_PAD_WEST] == Y);
    assert(map.bit[SNX_PAD_LT] == ZL && map.bit[SNX_PAD_RB] == R && map.bit[SNX_PAD_START] == PLUS &&
           map.bit[SNX_PAD_BACK] == MINUS && map.bit[SNX_PAD_UP] == UP && map.bit[SNX_PAD_LS] == L3);
    /* A and B swapped: the bottom one sends A, the right one B, wherever they are; X, Y and the rest stay; twice is
       none */
    snx_pad_map_swap_ab(&map);
    assert(map.bit[SNX_PAD_SOUTH] == A && map.bit[SNX_PAD_EAST] == B && map.bit[SNX_PAD_NORTH] == X &&
           map.bit[SNX_PAD_WEST] == Y && map.bit[SNX_PAD_LT] == ZL);
    snx_pad_map_swap_ab(&map);
    snx_pad_map_position(&placed);
    assert(!memcmp(&map, &placed, sizeof map));
    map.bit[SNX_PAD_START] = A; /* a control the player gave A sends B */
    map.bit[SNX_PAD_LB] = SNX_PAD_NONE;
    snx_pad_map_swap_ab(&map);
    assert(map.bit[SNX_PAD_START] == B && map.bit[SNX_PAD_LB] == SNX_PAD_NONE);

    /* the player's own: what it names, any case, and None; what it leaves out goes by its place */
    snx_pad_map_unset(&own);
    assert(!snx_pad_map_read(&own, "south:B  east:a\tLT:R lb:none", error, sizeof error));
    assert(own.bit[SNX_PAD_SOUTH] == B && own.bit[SNX_PAD_EAST] == A && own.bit[SNX_PAD_LT] == R &&
           own.bit[SNX_PAD_LB] == SNX_PAD_NONE && own.bit[SNX_PAD_WEST] == SNX_PAD_UNSET);
    map = own;
    snx_pad_map_fill(&map, &placed);
    assert(map.bit[SNX_PAD_SOUTH] == B && map.bit[SNX_PAD_WEST] == Y && map.bit[SNX_PAD_RT] == ZR);
    assert(snx_pad_map_find(&map, B) == SNX_PAD_SOUTH && snx_pad_map_find(&map, L) == -1);
    /* written back as it reads: the set ones, in the controls' order */
    snx_pad_map_write(&own, text, sizeof text);
    assert(!strcmp(text, "south:B east:A lb:None lt:R"));
    snx_pad_map_unset(&map);
    assert(!snx_pad_map_read(&map, text, error, sizeof error) && !memcmp(&map, &own, sizeof map));
    /* every control set fits its room */
    snx_pad_map_position(&map);
    snx_pad_map_write(&map, text, sizeof text);
    assert(strlen(text) < SNX_PAD_MAP_TEXT && !strncmp(text, "south:B east:A west:Y north:X", 29));
    /* a word it cannot read is named, and the map stays as it was: a button no console has, a control no pad has, no
       colon, a control named twice */
    map = own;
    assert(snx_pad_map_read(&map, "east:B south:Q", error, sizeof error) && !strcmp(error, "south:Q"));
    assert(snx_pad_map_read(&map, "paddle:A", error, sizeof error) && !strcmp(error, "paddle:A"));
    assert(snx_pad_map_read(&map, "south", error, sizeof error) && !strcmp(error, "south"));
    assert(snx_pad_map_read(&map, "south:A south:B", error, sizeof error) && !strcmp(error, "south:B"));
    assert(!memcmp(&map, &own, sizeof map));
    assert(!snx_pad_map_read(&map, "", error, sizeof error) && !memcmp(&map, &own, sizeof map));

    /* names: a face button as its pad shows it, the D-pad's in the font's arrows; the console's buttons; None */
    assert(!strcmp(snx_pad_control_name(SNX_PAD_SOUTH, sony), "Cross") &&
           !strcmp(snx_pad_control_name(SNX_PAD_SOUTH, NULL), "A"));
    assert(!strcmp(snx_pad_control_name(SNX_PAD_UP, xbox), "\x18") &&
           !strcmp(snx_pad_control_name(SNX_PAD_LT, sony), "LT"));
    assert(!strcmp(snx_pad_button_name(ZL), "ZL") && !strcmp(snx_pad_button_name(SNX_PAD_NONE), "None"));
    for (c = 0; c < SNX_PAD_CONTROLS; ++c) assert(snx_pad_control_key(c)[0]);
    assert(!snx_pad_control_key(SNX_PAD_CONTROLS)[0] && !snx_pad_control_name(-1, NULL)[0]);
    puts("pad_map passed");
    /* a console's: each button in its place, the map's text over it, then A and B swapped; a button the console has
       none of sends nothing there */
    {
        SnxPadMap console;
        snx_pad_map_console(&console, two_buttons, "test console", "", 0);
        assert(console.bit[SNX_PAD_SOUTH] == B && console.bit[SNX_PAD_EAST] == A);
        snx_pad_map_console(&console, two_buttons, "test console", "", 1);
        assert(console.bit[SNX_PAD_SOUTH] == A && console.bit[SNX_PAD_EAST] == B);
        snx_pad_map_console(&console, two_buttons, "test console", "south:L east:B", 1);
        assert(console.bit[SNX_PAD_SOUTH] == L && console.bit[SNX_PAD_EAST] == A);
        snx_pad_map_console(&console, two_buttons, "test console", "south:Q", 0); /* not read: the place's, said */
        assert(console.bit[SNX_PAD_SOUTH] == B);
    }
    return 0;
}
