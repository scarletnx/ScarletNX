/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The recent games: newest first, five at most, a file once, the name its run gave it, and the list in a file. */
#include "gui/recent.h"
#include "common/utilities/common.h"
#include "gui/model.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    GuiRecent r, again;
    char *dir, *file, *text, *games[7];
    char name[32];
    int i;
    dir = snx_temp_dir(NULL);
    assert(dir);
    for (i = 0; i < 7; ++i) {
        sprintf(name, "game %d.nsp", i);
        games[i] = snx_path_join(dir, name);
        assert(!snx_write_file(games[i], "x", 1));
    }
    memset(&r, 0, sizeof r);

    /* newest first, named after the file; the sixth pushes the first out */
    for (i = 0; i < 6; ++i) assert(!gui_recent_add(&r, games[i], GUI_SWITCH, 0));
    assert(r.count == 5 && !strcmp(r.game[0].name, "game 5") && !strcmp(r.game[4].name, "game 1"));
    assert(r.game[0].driver == GUI_SWITCH && !r.game[0].prepared && !strcmp(r.game[0].path, games[5]));

    /* the name a run gave it; an empty one, or a game not in the list, changes nothing */
    assert(gui_recent_rename(&r, games[3], "Game Three") && !gui_recent_rename(&r, games[3], "Game Three"));
    assert(!gui_recent_rename(&r, games[0], "gone already") && !gui_recent_rename(&r, games[3], ""));

    /* a file once: run again it moves up and keeps its name; with the other driver, its file's name again */
    assert(!gui_recent_add(&r, games[3], GUI_SWITCH, 1));
    assert(r.count == 5 && !strcmp(r.game[0].path, games[3]) && !strcmp(r.game[0].name, "Game Three"));
    assert(r.game[0].prepared && !strcmp(r.game[1].name, "game 5") && !strcmp(r.game[4].name, "game 1"));
    assert(!gui_recent_add(&r, games[3], GUI_LINUX, 0) && r.count == 5 && !strcmp(r.game[0].name, "game 3"));
    assert(r.game[0].driver == GUI_LINUX);

    /* what one line of the file cannot hold is left out */
    assert(gui_recent_add(&r, "/a\tb.nsp", GUI_SWITCH, 0) && gui_recent_rename(&r, games[3], "a\nb") == 0);

    /* kept across a restart, folder made */
    file = snx_path_join(dir, "gui/recent.txt");
    assert(!gui_recent_save(&r, file));
    gui_recent_load(&again, file);
    assert(again.count == r.count);
    for (i = 0; i < r.count; ++i)
        assert(!strcmp(again.game[i].path, r.game[i].path) && !strcmp(again.game[i].name, r.game[i].name) &&
               again.game[i].driver == r.game[i].driver && again.game[i].prepared == r.game[i].prepared);

    /* every console's records keep their driver across a restart: its package (a CIA, a VPK, a GBA ROM) as one, and the
       game its driver made of it, ready to run, as one too */
    GuiRecent before_ctr = r;
    assert(gui_recent_add(&r, games[6], GUI_KEYS, 0));
    for (int d = 0; d < 6; ++d) {
        int driver = d % 3 == 0 ? GUI_3DS : d % 3 == 1 ? GUI_VITA : GUI_GBA, made = d >= 3;
        assert(!gui_recent_add(&r, games[6], driver, made));
        assert(r.game[0].prepared == made && r.game[0].driver == driver);
        assert(!gui_recent_save(&r, file));
        gui_recent_load(&again, file);
        assert(again.count == r.count && again.game[0].driver == driver && again.game[0].prepared == made);
        r = before_ctr;
    }

    /* a file that is gone, a line it cannot read, a second copy: left out; CRLF read too */
    assert(!remove(games[1]));
    text = snx_format("switch\tfile\t%s\tTwo\r\nnonsense\nlinux\tgames\t%s\t\nswitch\tfile\t%s\tcopy\n"
                      "wii\tfile\t%s\tx\nswitch\tfile\t%s\tgone\nlinux\tfile\t%s\ta\textra\n",
                      games[2], games[4], games[2], games[6], games[1], games[6]);
    assert(!snx_write_file(file, text, strlen(text)));
    gui_recent_load(&again, file);
    assert(again.count == 2 && !strcmp(again.game[0].name, "Two") && again.game[0].driver == GUI_SWITCH);
    assert(!again.game[0].prepared && !strcmp(again.game[1].path, games[4]) && !strcmp(again.game[1].name, "game 4"));
    assert(again.game[1].driver == GUI_LINUX && again.game[1].prepared);
    free(text);

    /* no file yet: no games */
    gui_recent_load(&again, games[1]);
    assert(!again.count);
    gui_recent_remove(&r, 0);
    gui_recent_remove(&r, 9);
    assert(r.count == 4 && !strcmp(r.game[0].path, games[5]));

    /* a dump made since into its game: the entry runs the program, its place and name kept; an entry the program had
       already goes when it is the older, else the dump's does */
    memset(&r, 0, sizeof r);
    assert(!gui_recent_add(&r, games[2], GUI_SWITCH, 0) && !gui_recent_add(&r, games[3], GUI_SWITCH, 0));
    assert(gui_recent_rename(&r, games[2], "Two"));
    assert(gui_recent_made(&r, 1, games[5]) && r.count == 2 && !strcmp(r.game[0].path, games[3]));
    assert(!strcmp(r.game[1].path, games[5]) && r.game[1].prepared && !strcmp(r.game[1].name, "Two"));
    assert(r.game[1].driver == GUI_SWITCH && !r.game[0].prepared);
    assert(gui_recent_made(&r, 0, games[5]) && r.count == 1 && !strcmp(r.game[0].path, games[5]));
    assert(!strcmp(r.game[0].name, "game 3") && r.game[0].prepared); /* the newer, the dump's, kept */
    assert(!gui_recent_add(&r, games[6], GUI_SWITCH, 0) && !gui_recent_add(&r, games[5], GUI_SWITCH, 1));
    assert(r.count == 2 && !strcmp(r.game[1].path, games[6]) && gui_recent_made(&r, 1, games[5]));
    assert(r.count == 1 && !strcmp(r.game[0].path, games[5]) && !strcmp(r.game[0].name, "game 3")); /* the newer */
    assert(!gui_recent_made(&r, 1, games[6]) && !gui_recent_made(&r, -1, games[6]));
    assert(!gui_recent_made(&r, 0, "/a\tb") && r.count == 1 && !strcmp(r.game[0].path, games[5]));

    assert(!snx_remove_tree(dir));
    for (i = 0; i < 7; ++i) free(games[i]);
    free(file);
    free(dir);
    puts("GUI recent games: order, cap, a file once, names, the file and a dump made into its game passed");
    return 0;
}
