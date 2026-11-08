/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A run's scripted presses as the drivers take them (--script=ms:hex,...): when, in ms from the start of the run, and
   the buttons in hex in the console's own bits, comma between, each held for the driver's press length. A Switch
   game's script (nx/runtime/platform/unattended.c) can also give a press at a present, Nf:hex, and its buttons take
   64 bits */
#ifndef SNX_SCRIPT_H
#define SNX_SCRIPT_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t mask; /* the console's buttons */
    uint32_t at;   /* ms from the start, or with frame set the presents since it */
    uint8_t frame;
} SnxPress;

/* text's presses into *presses (malloc'd, NULL for none), their masks within buttons (the bits the console takes), Nf:
   ones only when frames is set: their count. A press it cannot read ends the script, and *stop is the text from it
   ("" when it read all). NULL presses and 0 when out of memory */
size_t snx_script_read(const char* text, uint64_t buttons, int frames, SnxPress** presses, const char** stop);
/* snx_script_read's ms:hex presses, with a line on stderr after program's name when it stops early: how many it read
   and the text it stopped at */
size_t snx_script_parse(const char* program, const char* text, uint64_t buttons, SnxPress** presses);
/* the buttons the ms presses hold at ms, each for length ms */
uint64_t snx_script_held(const SnxPress* presses, size_t count, double ms, uint32_t length);
#endif
