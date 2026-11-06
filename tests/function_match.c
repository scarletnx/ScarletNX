/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The function matcher (src/nx/loader/match.c) on a synthetic program: an sdk module that exports six functions and a
   main that exports one, whose other functions are copies of the sdk's at other addresses. A copy whose calls and page
   addresses differ is named, under the shortest of its aliases; a copy of code two sdk functions share, a copy of a
   function shorter than the minimum, of a constructor, two copies of one function in main (generic code), a near copy
   (one constant differs) and main's own export are not */
#include "nx/loader/match.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FN_WORDS 40 /* over SNX_MATCH_MIN_WORDS */
#define SHORT    20 /* under it */

static void put32(unsigned char* p, SnxU32 w) {
    p[0] = (unsigned char)w;
    p[1] = (unsigned char)(w >> 8);
    p[2] = (unsigned char)(w >> 16);
    p[3] = (unsigned char)(w >> 24);
}

/* a function of n words: ADRP x8 of page, ADD x8, x8 of lo, BL by call words, then MOVZ w0 of seed + k, and RET */
static void function(unsigned char* at, SnxU32 n, SnxU32 page, SnxU32 lo, SnxU32 call, SnxU32 seed) {
    SnxU32 k;
    put32(at, 0x90000008u | (page & 3) << 29 | (page >> 2 & 0x7FFFF) << 5);
    put32(at + 4, 0x91000108u | (lo & 0xFFF) << 10);
    put32(at + 8, 0x94000000u | (call & 0x3FFFFFF));
    for (k = 3; k + 1 < n; ++k) put32(at + 4 * k, 0x52800000u | ((seed + k) & 0xFFFF) << 5);
    put32(at + 4 * (n - 1), 0xD65F03C0u);
}

int main(void) {
    unsigned char sdk_image[0x800] = {0}, main_image[0x800] = {0};
    SnxSymbol sdk_syms[] = {{0x000, 4 * FN_WORDS, (char*)"found_fn_alias", 2},
                            {0x000, 4 * FN_WORDS, (char*)"found_fn", 2},
                            {0x100, 4 * FN_WORDS, (char*)"twin_a", 2},
                            {0x200, 4 * FN_WORDS, (char*)"twin_b", 2},
                            {0x300, 4 * SHORT, (char*)"short_fn", 2},
                            {0x400, 4 * FN_WORDS, (char*)"generic_fn", 2},
                            {0x500, 4, (char*)"a_variable", 1},
                            {0x600, 4 * FN_WORDS, (char*)"_ZN5ClassC2Ev", 2}};
    SnxSymbol main_syms[] = {{0x440, 4 * FN_WORDS, (char*)"own_export", 2}};
    SnxU64 main_functions[] = {0x40, 0x140, 0x240, 0x340, 0x440, 0x540, 0x640, 0x740};
    SnxModuleImage mods[2] = {{0}};
    SnxProgram program = {0};
    SnxMatch* found;
    SnxMatchStats st;
    size_t n;
    function(sdk_image + 0x000, FN_WORDS, 1, 0x10, 0x100, 100);
    function(sdk_image + 0x100, FN_WORDS, 1, 0x20, 0x200, 200); /* the twins differ in what a link moves only */
    function(sdk_image + 0x200, FN_WORDS, 9, 0x30, 0x300, 200);
    function(sdk_image + 0x300, SHORT, 1, 0x40, 0x400, 300);
    function(sdk_image + 0x400, FN_WORDS, 1, 0x50, 0x500, 400);
    function(sdk_image + 0x600, FN_WORDS, 1, 0x60, 0x600, 600);     /* a constructor */
    function(main_image + 0x040, FN_WORDS, 77, 0x123, 0x5000, 100); /* found_fn elsewhere */
    function(main_image + 0x140, FN_WORDS, 77, 0x123, 0x5000, 101); /* every constant one off: another function */
    function(main_image + 0x240, FN_WORDS, 55, 0x321, 0x6000, 200); /* the twins' code */
    function(main_image + 0x340, SHORT, 66, 0x222, 0x7000, 300);    /* short_fn's */
    function(main_image + 0x440, FN_WORDS, 88, 0x111, 0x8000, 100); /* found_fn's, exported by main */
    function(main_image + 0x540, FN_WORDS, 12, 0x333, 0x9000, 400); /* generic_fn's, twice */
    function(main_image + 0x640, FN_WORDS, 13, 0x444, 0xA000, 400);
    function(main_image + 0x740, FN_WORDS, 14, 0x555, 0xB000, 600); /* its code: no name, whatever its length */
    mods[0].name = (char*)"main";
    mods[0].image = main_image;
    mods[0].image_size = sizeof main_image;
    mods[0].text_size = sizeof main_image;
    mods[0].base = 0x8005000;
    mods[0].symbols = main_syms;
    mods[0].symbol_count = 1;
    mods[0].functions = main_functions;
    mods[0].function_count = sizeof main_functions / sizeof *main_functions;
    mods[1].name = (char*)"sdk";
    mods[1].image = sdk_image;
    mods[1].image_size = sizeof sdk_image;
    mods[1].text_size = sizeof sdk_image;
    mods[1].base = 0x9000000;
    mods[1].symbols = sdk_syms;
    mods[1].symbol_count = sizeof sdk_syms / sizeof *sdk_syms;
    program.modules = mods;
    program.module_count = 2;
    program.npdm.is64 = 1;

    n = snx_match_program(&program, &found, &st);
    printf("matches %zu, signatures %zu, skipped %zu, ambiguous %zu, repeated %zu\n", n, st.signatures, st.skipped,
           st.ambiguous, st.repeated);
    assert(st.sdk == 1);
    assert(st.signatures == 4); /* found_fn once for its two names, the twins, generic_fn; short_fn too short, the
                                   variable no function */
    assert(st.skipped == 2);    /* short_fn, and the constructor */
    assert(st.ambiguous == 1);  /* the twins' code in main */
    assert(st.repeated == 2);   /* generic_fn's two copies */
    assert(n == 1);
    assert(found[0].addr == 0x8005000 + 0x40 && found[0].words == FN_WORDS);
    assert(!strcmp(sdk_syms[found[0].symbol].name, "found_fn"));
    free(found);
    puts("PASS a copy named under its shortest alias; shared code, a short function, a constructor, a repeated one, a "
         "near copy and an export not");

    program.npdm.is64 = 0; /* a 32-bit program's code is no A64 */
    n = snx_match_program(&program, &found, &st);
    assert(n == 0 && !found && st.signatures == 0);
    program.npdm.is64 = 1;
    mods[1].name = (char*)"subsdk0"; /* no sdk module: nothing to sign */
    n = snx_match_program(&program, &found, &st);
    assert(n == 0 && !found && st.signatures == 0);
    puts("PASS nothing for a 32-bit program or one without an sdk module");
    return 0;
}
