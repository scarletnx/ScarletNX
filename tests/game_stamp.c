/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* common/utilities/mods.c's game stamp, a recompiled game's <name>.abi: what it is compared by, the made line after it
   (how the code was made, which no comparison reads), and the save states' copy of it (state_header.c) */
#include "common/utilities/mods.h"
#include "common/utilities/state_header.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* g_base; /* <root>/Game/Game, whose stamp is <root>/Game/Game.abi */

static void stamp(const char* text) {
    char* path = snx_format("%s.abi", g_base);
    assert(path && !snx_write_file(path, text, strlen(text)));
    free(path);
}

int main(void) {
    char *root = snx_temp_dir(NULL), *folder, *line, kept[64];
    SnxGameMade made, full = {'3', 1, 812345, 1003210, 0}, jit = {'0', 0, 0, 4096, 0},
                      switch2 = {'2', 1, 100, 200, 1};
    assert(root);
    folder = snx_path_join(root, "Game");
    assert(folder && !snx_mkdirs(folder));
    g_base = snx_path_join(folder, "Game");

    /* the line as the recompile writes it after the stamp */
    line = snx_game_made_line(&full);
    assert(!strcmp(line, "\nmade opt=3 aot=1 found=812345 code=1003210\n"));
    free(line);

    /* no stamp: not made, nothing known */
    assert(!snx_game_stamp_is(g_base, "14"));
    assert(!snx_game_made_read(g_base, &made) && !made.opt && made.aot == -1 && !made.found && !made.code);

    /* a stamp from before the made line: made as it says, how unknown */
    stamp("14 mods 0123456789abcdef");
    assert(snx_game_stamp_is(g_base, "14 mods 0123456789abcdef") && !snx_game_stamp_is(g_base, "14"));
    assert(!snx_game_made_read(g_base, &made) && !made.opt && made.aot == -1 && !made.code);
    snx_state_stamp(g_base, kept, sizeof kept);
    assert(!strcmp(kept, "14 mods 0123456789abcdef"));

    /* with it: compared without it, read back whole, and out of the save states' copy */
    line = snx_game_made_line(&full);
    {
        char* text = snx_format("14 mods 0123456789abcdef%s", line);
        stamp(text);
        free(text);
    }
    free(line);
    assert(snx_game_stamp_is(g_base, "14 mods 0123456789abcdef") && !snx_game_stamp_is(g_base, "14"));
    assert(snx_game_made_read(g_base, &made) && made.opt == '3' && made.aot == 1 && made.found == 812345 &&
           made.code == 1003210 && !made.switch2);
    snx_state_stamp(g_base, kept, sizeof kept);
    assert(!strcmp(kept, "14 mods 0123456789abcdef"));

    /* a Switch 2 game's: the made line ends with it, and the stamp the comparisons read stays the same */
    line = snx_game_made_line(&switch2);
    assert(!strcmp(line, "\nmade opt=2 aot=1 found=100 code=200 switch2\n"));
    {
        char* text = snx_format("14 mods 0123456789abcdef%s", line);
        stamp(text);
        free(text);
    }
    free(line);
    assert(snx_game_stamp_is(g_base, "14 mods 0123456789abcdef"));
    assert(snx_game_made_read(g_base, &made) && made.opt == '2' && made.code == 200 && made.switch2);
    snx_state_stamp(g_base, kept, sizeof kept);
    assert(!strcmp(kept, "14 mods 0123456789abcdef"));

    /* --jit-only's: no code ahead of time */
    line = snx_game_made_line(&jit);
    {
        char* text = snx_format("14%s", line);
        stamp(text);
        free(text);
    }
    free(line);
    assert(snx_game_stamp_is(g_base, "14"));
    assert(snx_game_made_read(g_base, &made) && made.opt == '0' && made.aot == 0 && !made.found && made.code == 4096);

    /* a stamp of lines of its own (the driver test's) keeps them: only the last "\nmade " line is left out */
    stamp("4\nabc1234\n\nmade opt=2 aot=1 found=1 code=2\n");
    assert(snx_game_stamp_is(g_base, "4\nabc1234\n"));
    stamp("4\nabc1234\n\n"); /* more after it, no made line: no completed build */
    assert(!snx_game_stamp_is(g_base, "4\nabc1234\n"));

    /* anything else after the stamp is no completed build; a made line that does not read is unknown */
    stamp("14\n");
    assert(!snx_game_stamp_is(g_base, "14"));
    stamp("14\nanother line, no made one\n");
    assert(!snx_game_stamp_is(g_base, "14"));
    stamp("14garbage\nmade opt=2 aot=1 found=1 code=2\n");
    assert(!snx_game_stamp_is(g_base, "14"));
    stamp("14\nmade opt=7 aot=1 found=1 code=2\n");
    assert(snx_game_stamp_is(g_base, "14") && !snx_game_made_read(g_base, &made) && !made.opt && made.aot == -1);
    stamp("14\nmade opt=2 aot=1 found=3 code=2\n"); /* more found than there is */
    assert(!snx_game_made_read(g_base, &made) && !made.code);
    stamp("14\nmade opt=2 aot=1");
    assert(snx_game_stamp_is(g_base, "14") && !snx_game_made_read(g_base, &made));

    assert(!snx_remove_tree(root));
    free(g_base);
    free(folder);
    free(root);
    puts("game_stamp: ok");
    return 0;
}
