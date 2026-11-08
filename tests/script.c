/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A run's scripted presses (common/utilities/script.c), as the 3DS, Vita and GBA drivers take them: ms:hex, comma
   between, read to the first press that is not one, each held for the press length from its time; and as a Switch
   game's script takes them, Nf: presses and 64-bit masks too */
#include "common/utilities/script.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    SnxPress* presses;
    size_t count = snx_script_parse("script", "100:8,250:1,250:200", 0x3FF, &presses);
    assert(count == 3 && presses[0].at == 100 && presses[0].mask == 8 && presses[2].mask == 0x200);
    assert(snx_script_held(presses, count, 99, 300) == 0);
    assert(snx_script_held(presses, count, 100, 300) == 8);
    assert(snx_script_held(presses, count, 260, 300) == 0x209); /* the three at once */
    assert(snx_script_held(presses, count, 399.9, 300) == 0x209 && snx_script_held(presses, count, 400, 300) == 0x201);
    assert(snx_script_held(presses, count, 550, 300) == 0);
    free(presses);
    /* the first press it cannot read ends the script: a word, a mask past the console's buttons, no hex */
    count = snx_script_parse("script", "100:8,soon:1,300:2", 0x3FF, &presses);
    assert(count == 1 && presses[0].mask == 8);
    free(presses);
    count = snx_script_parse("script", "100:8,200:1000000", 0x3FF, &presses);
    assert(count == 1);
    free(presses);
    count = snx_script_parse("script", "100:", 0x3FF, &presses);
    assert(count == 0);
    free(presses);
    count = snx_script_parse("script", "", 0x3FF, &presses);
    assert(count == 0 && !presses);
    /* a Switch game's: Nf: presses at a present, and 64-bit masks; the drivers' parse refuses an Nf: one */
    const char* stop;
    count = snx_script_read("30f:1,500:8000000000000000,x", ~0ull, 1, &presses, &stop);
    assert(count == 2 && presses[0].frame && presses[0].at == 30 && presses[1].mask == 0x8000000000000000ull);
    assert(!presses[1].frame && !strcmp(stop, "x"));
    assert(snx_script_held(presses, count, 31, 100) == 0); /* a present's press is the caller's to time */
    free(presses);
    count = snx_script_parse("script", "30f:1", 0x3FF, &presses);
    assert(count == 0);
    free(presses);
    count = snx_script_read(" 1:1", ~0ull, 1, &presses, &stop); /* a digit first, a hex digit after the colon */
    assert(count == 0 && !strcmp(stop, " 1:1"));
    free(presses);
    puts("script: ok");
    return 0;
}
