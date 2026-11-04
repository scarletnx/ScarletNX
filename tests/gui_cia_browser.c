/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/browser.h"
#include "common/utilities/common.h"
#include "common/utilities/game_ini.h"
#include <assert.h>
#include <errno.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* gui_games_group's made: b.nsp is the dump of a game recompiled already */
static bool made_b(const char* path, const void* user) {
    size_t n = strlen(path);
    (void)user;
    return n >= 5 && !strcmp(path + n - 5, "b.nsp");
}

int main(void) {
    GuiGames games = {0};
    char error[256];
    char* dir = snx_temp_dir(NULL);
    const char* files[] = {"Alpha.CIA",    "game with spaces.cia", ".hidden.cia",
                           "wrong.cia.so", "launch.exe",           "note.txt",
                           "card.3DS",     "card.cci",             "homebrew.3dsx"};
    assert(dir);
    for (size_t i = 0; i < sizeof files / sizeof *files; ++i) {
        char* path = snx_path_join(dir, files[i]);
        assert(!snx_write_file(path, "fixture", 7));
        free(path);
    }
    char* subdir = snx_path_join(dir, "directory.cia");
    assert(!snx_mkdirs(subdir));
    /* the space a folder takes: its files' bytes, its folders' too; a link is not followed; a stop ends the count */
    {
        int stop = 0;
        char* deep = snx_path_join(subdir, "deep.bin");
        char* elsewhere = snx_temp_dir(NULL);
        char* big = snx_path_join(elsewhere, "big.bin");
        assert(!snx_write_file(deep, "123", 3) && !snx_write_file(big, "0123456789", 10));
        assert(gui_tree_size(dir, &stop) == 9 * 7 + 3);
#ifndef _WIN32
        char* link = snx_path_join(dir, "link");
        assert(!symlink(elsewhere, link) && gui_tree_size(dir, &stop) == 9 * 7 + 3);
        assert(!remove(link));
        free(link);
#endif
        assert(gui_tree_size(elsewhere, &stop) == 10 && !gui_tree_size("/nonexistent/snx-tree", &stop));
        assert(!gui_tree_size("", &stop)); /* none, not the working folder (Windows lists that for "*") */
        stop = 1;
        assert(!gui_tree_size(dir, &stop));
        assert(!snx_remove_tree(deep) && !snx_remove_tree(elsewhere));
        free(big);
        free(elsewhere);
        free(deep);
    }
    assert(gui_games_scan(&games, dir, true, error, sizeof error) && !error[0]);
    assert(games.count == 4 && !strcmp(games.names[0], "Alpha.CIA") && !strcmp(games.names[1], "card.3DS") &&
           !strcmp(games.names[2], "card.cci") && !strcmp(games.names[3], "game with spaces.cia"));
    /* each with where it is */
    {
        char* alpha = snx_path_join(dir, "Alpha.CIA");
        assert(!strcmp(games.paths[0], alpha));
        free(alpha);
    }
    /* more folders' games go after the ones listed, each with its path; one that is not there is named */
    {
        char* more = snx_temp_dir(NULL);
        char* beta = snx_path_join(more, "beta.cia");
        assert(more && !snx_write_file(beta, "fixture", 7));
        assert(gui_games_add(&games, more, GUI_GAMES_CIA, error, sizeof error) && !error[0] && games.count == 5);
        assert(!strcmp(games.names[4], "beta.cia") && !strcmp(games.paths[4], beta));
        assert(gui_games_add(&games, more, GUI_GAMES_RUN, error, sizeof error) && games.count == 5); /* no CIA */
        assert(!gui_games_add(&games, "/nonexistent/snx-games", GUI_GAMES_CIA, error, sizeof error));
        assert(strstr(error, "/nonexistent/snx-games") && games.count == 5);
        assert(!snx_remove_tree(more));
        free(beta);
        free(more);
    }
    /* a game in a folder of its own is listed by the folder, run from inside it; an older version's game beside the
       others still is; a folder holding no game of its name (its _data/, another program) is not */
    {
#ifdef _WIN32
#define EXE ".exe"
#else
#define EXE ""
#endif
        static const char* const made[] = {"0100ABCD12345000/0100ABCD12345000" EXE, "Old" EXE, "Old_data/main",
                                           "Other/Else" EXE, "Plain/Plain.txt"};
        char *ready = snx_temp_dir(NULL), *path, *inside;
        size_t i;
        assert(ready);
        for (i = 0; i < sizeof made / sizeof *made; ++i) {
            char* folder;
            path = snx_path_join(ready, made[i]);
            folder = snx_dirname(path);
            assert(!snx_mkdirs(folder) && !snx_write_file(path, "#!/bin/sh\n", 10) && !snx_make_executable(path));
            free(folder);
            free(path);
        }
        assert(gui_games_scan(&games, ready, false, error, sizeof error) && games.count == 2);
        assert(!strcmp(games.names[0], "0100ABCD12345000") && !strcmp(games.names[1], "Old" EXE));
        inside = snx_path_join(ready, made[0]);
        path = snx_path_join(ready, made[1]);
        assert(!strcmp(games.paths[0], inside) && !strcmp(games.paths[1], path));
        assert(!strcmp(games.shown[0], games.names[0]) && !strcmp(games.shown[1], games.names[1])); /* no NACP */
        /* the one in a folder of its own goes through the driver once it has its data; the older version's, beside
           its data and the others, never */
        assert(!gui_game_folder(inside) && !gui_game_folder(path));
        /* shown by the name in the control.nacp its data holds, and sorted by it, the folder still its title ID */
        {
            static unsigned char nacp[0x300 * 2];
            char* file = snx_path_join(ready, "0100ABCD12345000/0100ABCD12345000_data/control.nacp");
            char* data = snx_dirname(file);
            assert(data && !snx_mkdirs(data));
            assert(gui_game_folder(inside) && !gui_game_folder(path) && !gui_game_folder(data));
            {
                char* other = snx_path_join(ready, "Other/Else" EXE);
                char* other_data = snx_path_join(ready, "Other/Else_data");
                assert(!snx_mkdirs(other_data) && !gui_game_folder(other)); /* not its folder's name */
                free(other_data);
                free(other);
            }
            memcpy(nacp + 0x300, "Zeta Quest", 10); /* the second language's: American English's is empty */
            assert(!snx_write_file(file, nacp, sizeof nacp));
            assert(gui_games_scan(&games, ready, false, error, sizeof error) && games.count == 2);
            assert(!strcmp(games.shown[0], "Old" EXE) && !strcmp(games.shown[1], "Zeta Quest"));
            assert(!strcmp(games.names[1], "0100ABCD12345000") && !strcmp(games.paths[1], inside));
            memcpy(nacp, "alpha quest", 11); /* before Old whatever the letters' case */
            assert(!snx_write_file(file, nacp, sizeof nacp));
            assert(gui_games_scan(&games, ready, false, error, sizeof error) && games.count == 2);
            assert(!strcmp(games.shown[0], "alpha quest") && !strcmp(games.names[0], "0100ABCD12345000"));
            assert(!strcmp(games.paths[0], inside) && !strcmp(games.paths[1], path));
            free(data);
            free(file);
        }
        free(path);
        free(inside);
        assert(!snx_remove_tree(ready));
        free(ready);
#undef EXE
    }
    /* updates and DLCs made beside their game, in its folder: not games of their own, its add-ons, listed by the
       names the player reads (an update's version from its control.nacp, a DLC's number past the base ID its game's
       control.nacp gives, else its whole title ID), the update first. A program named so without its data, an
       update's data without its program, or a name that is no title ID is none */
    {
#ifdef _WIN32
#define EXE ".exe"
#else
#define EXE ""
#endif
        static const char* const made[] = {"0100ABCD12340000/0100ABCD12340000" EXE,
                                           "0100ABCD12340000/0100ABCD12340000_0100ABCD12341002" EXE,
                                           "0100ABCD12340000/0100ABCD12340000_0100ABCD12340800" EXE,
                                           "0100ABCD12340000/0100ABCD12340000_0100ABCD12341003" EXE,
                                           "0100ABCD12340000/0100ABCD12340000_0100ABCD5555A001" EXE,
                                           "0100ABCD12340000/0100ABCD12340000_junk" EXE};
        static const char* const data[] = {"0100ABCD12340000_data", "0100ABCD12340000_0100ABCD12341002_data",
                                           "0100ABCD12340000_0100ABCD12340800_data",
                                           "0100ABCD12340000_0100ABCD12341005_data",
                                           "0100ABCD12340000_0100ABCD5555A001_data", "0100ABCD12340000_junk_data",
                                           /* a collection's program 3, its code beside it: no add-on */
                                           "0100ABCD12340000_0100ABCD12340003_data"};
        static unsigned char nacp[0x4000];
        char *ready = snx_temp_dir(NULL), *path, *folder, *game, *update;
        GuiGames addons = {0};
        size_t i;
        assert(ready);
        folder = snx_path_join(ready, "0100ABCD12340000");
        for (i = 0; i < sizeof made / sizeof *made; ++i) {
            path = snx_path_join(ready, made[i]);
            assert(!snx_mkdirs(folder) && !snx_write_file(path, "#!/bin/sh\n", 10) && !snx_make_executable(path));
            free(path);
        }
        for (i = 0; i < sizeof data / sizeof *data; ++i) {
            path = snx_path_join(folder, data[i]);
            assert(!snx_mkdirs(path));
            free(path);
        }
        memcpy(nacp + 0x3060, "1.2.0", 5); /* DisplayVersion */
        path = snx_path_join(folder, "0100ABCD12340000_0100ABCD12340800_data/control.nacp");
        assert(!snx_write_file(path, nacp, sizeof nacp));
        free(path);
        game = snx_path_join(ready, made[0]);
        update = snx_path_join(ready, made[2]);
        assert(gui_games_scan(&games, ready, false, error, sizeof error) && games.count == 1);
        /* a DLC is its data folder alone, its program an older version's left aside; an update needs its program */
        assert(!strcmp(games.names[0], "0100ABCD12340000") && games.addons[0] == 4);
        assert(gui_game_folder(update) && gui_game_addons(game, &addons) && addons.count == 4);
        assert(!strcmp(addons.names[0], "0100ABCD12340000_0100ABCD12340800") && !strcmp(addons.shown[0], "Update 1.2.0"));
        assert(!strcmp(addons.paths[0], update));
        assert(!strcmp(addons.names[1], "0100ABCD12340000_0100ABCD12341002") && !strcmp(addons.shown[1], "DLC 2"));
        assert(!strcmp(addons.names[2], "0100ABCD12340000_0100ABCD12341005") && !strcmp(addons.shown[2], "DLC 5"));
        path = snx_path_join(folder, "0100ABCD12340000_0100ABCD12341005_data");
        assert(!strcmp(addons.paths[2], path));
        free(path);
        /* a DLC numbered from no base its game's control.nacp names: by its whole title ID */
        assert(!strcmp(addons.names[3], "0100ABCD12340000_0100ABCD5555A001") &&
               !strcmp(addons.shown[3], "DLC 0100ABCD5555A001"));
        assert(!gui_game_addons(update, &addons) && !addons.count); /* an add-on has none of its own */
        { /* the collection's program 3, made: a program of the game's, which gui_game_programs lists */
            int programs[SNX_PROGRAMS_MAX];
            char* code = snx_program_code(folder, "0100ABCD12340000_0100ABCD12340003");
            assert(gui_game_programs(game, programs) == 0); /* its data folder alone: not made */
            assert(!snx_write_file(code, "code", 4));
            assert(gui_game_programs(game, programs) == 1 && programs[0] == 3);
            assert(gui_game_addons(game, &addons) && addons.count == 4);
            free(code);
        }
        gui_games_free(&addons);
        free(game);
        free(update);
        free(folder);
        assert(!snx_remove_tree(ready));
        free(ready);
#undef EXE
    }
    /* Repeated scans replace the old list; neither recursion nor execute bits. */
    assert(gui_games_scan(&games, subdir, true, error, sizeof error) && !games.count);
    assert(!snx_remove_tree(dir));
    assert(!gui_games_scan(&games, dir, true, error, sizeof error) && error[0] && !games.count);
    /* the folder gone is said with its path and the system's reason */
    assert(strstr(error, dir) != NULL);
#ifndef _WIN32
    assert(strstr(error, strerror(ENOENT)) != NULL);
#endif
    gui_games_free(&games);
    free(subdir);
    free(dir);
    puts("GUI CIA browser filtering, sorting and refresh passed");
    /* dumps listed with GUI_GAMES_DUMP, and the list in two groups: the ones ready to run, then the dumps and CIAs,
       under their headings when both have games; a dump made already is left out, and grouping again gives the same
       list */
    {
        GuiGames g = {0};
        char* d = snx_temp_dir(NULL);
        const char* dumps[] = {"b.nsp", "a.xci", "c.nro", "z.cia", "notes.txt"};
        size_t k, ready = 0;
        assert(d);
        for (k = 0; k < sizeof dumps / sizeof *dumps; ++k) {
            char* path = snx_path_join(d, dumps[k]);
            assert(!snx_write_file(path, "fixture", 7));
            free(path);
        }
        assert(gui_games_add(&g, d, GUI_GAMES_RUN, error, sizeof error) && g.count == 0); /* no dump unasked */
        gui_games_free(&g);
        assert(gui_games_add(&g, d, GUI_GAMES_DUMP | GUI_GAMES_CIA, error, sizeof error) && g.count == 4);
        assert(gui_games_group(&g, "Recompiled", "Dumps", NULL, NULL) && g.count == 4); /* dumps alone: no heading */
        for (k = 0; k < g.count; ++k) assert(!gui_games_heading(&g, k));
        gui_games_free(&g);
#ifndef _WIN32
        {
            char* folder = snx_path_join(d, "Ready");
            char* program = snx_path_join(folder, "Ready");
            assert(folder && program && !snx_mkdirs(folder) && !snx_write_file(program, "#!/bin/sh\n", 10));
            assert(!chmod(program, 0755));
            ready = 1;
            free(program);
            free(folder);
        }
#endif
        assert(gui_games_add(&g, d, GUI_GAMES_RUN | GUI_GAMES_CIA | GUI_GAMES_DUMP, error, sizeof error));
        assert(g.count == 4 + ready);
        assert(gui_games_group(&g, "Recompiled", "Dumps", made_b, NULL));
        for (int again = 0; again < 2; ++again) {
            /* [Recompiled, Ready, Dumps,] then a.xci, c.nro and z.cia in their order; b.nsp made already. The headings
               only with both groups: without Ready (Windows here) the dumps alone */
            size_t at = 0;
            assert(g.count == (ready ? 3 : 0) + 3);
            if (ready) {
                assert(gui_games_heading(&g, 0) && !strcmp(g.shown[0], "Recompiled"));
                assert(!gui_games_heading(&g, 1) && !strcmp(g.names[1], "Ready"));
                assert(gui_games_heading(&g, 2) && !strcmp(g.shown[2], "Dumps"));
                at = 3;
            }
            for (k = at; k < g.count; ++k) assert(!gui_games_heading(&g, k) && strcmp(g.names[k], "b.nsp"));
            assert(!gui_games_heading(&g, g.count));                          /* past the end: no heading */
            assert(gui_games_group(&g, "Recompiled", "Dumps", made_b, NULL)); /* again: the same */
        }
        gui_games_free(&g);
        /* no dumps' heading (Games' Dumps off): the ready games alone, no heading over them */
        assert(gui_games_add(&g, d, GUI_GAMES_RUN | GUI_GAMES_CIA | GUI_GAMES_DUMP, error, sizeof error));
        assert(g.count == 4 + ready && gui_games_group(&g, "Recompiled", NULL, made_b, NULL) && g.count == ready);
        for (k = 0; k < g.count; ++k) assert(!gui_games_heading(&g, k) && !strcmp(g.names[k], "Ready"));
        gui_games_free(&g);
        snx_remove_tree(d);
        free(d);
    }
    /* a deep library: folders of dumps, then its own dumps (snx_list_dir sorts: "folder" before "game"), every game
       listed however many each folder holds. A folder's scan once sized the lists for its own games alone, smaller
       than what its parent still had to add, and the parent wrote past their end */
    {
        GuiGames g = {0};
        char* d = snx_temp_dir(NULL);
        char name[32];
        size_t k;
        int f;
        assert(d);
        for (k = 0; k < 12; ++k) {
            char* path;
            snprintf(name, sizeof name, "game%02zu.nsp", k);
            path = snx_path_join(d, name);
            assert(path && !snx_write_file(path, "fixture", 7));
            free(path);
        }
        for (f = 0; f < 3; ++f) { /* a dump, twenty, a dump */
            char* folder;
            snprintf(name, sizeof name, "folder%d", f);
            folder = snx_path_join(d, name);
            assert(folder && !snx_mkdirs(folder));
            for (k = 0; k < (f == 1 ? 20u : 1u); ++k) {
                char* path;
                snprintf(name, sizeof name, "in%d-%02zu.xci", f, k);
                path = snx_path_join(folder, name);
                assert(path && !snx_write_file(path, "fixture", 7));
                free(path);
            }
            free(folder);
        }
        assert(gui_games_add_deep(&g, d, GUI_GAMES_DUMP, 1, error, sizeof error) && !error[0]);
        assert(g.count == 12 + 1 + 20 + 1);
        for (k = 0; k < g.count; ++k) assert(g.names[k] && g.shown[k] && g.paths[k] && snx_is_file(g.paths[k]));
        gui_games_free(&g);
        snx_remove_tree(d);
        free(d);
    }
    /* the log a run writes for a game (gui_game_log), beside it in a folder no data folder holds; and the games by when
       each last ran: the newest log first, two of a time in the order they had, one never run after them, each group
       apart under its heading, a dump not ordered by a log of its name */
    {
        GuiGames g = {0};
        char* d = snx_temp_dir(NULL);
        const char* names[] = {"alpha", "beta", "delta", "gamma"};
        const unsigned long long times[] = {1000, 3000, 1000, 0};
        const char* order[] = {"beta", "flat", "alpha", "delta", "gamma", "a.nsp", "b.nsp"};
        char *path, *log, *own;
        size_t k, at;
        assert(d);
        for (k = 0; k < 4; ++k) {
            char* folder = snx_path_join(d, names[k]);
            char* data = snx_format("%s/%s_data", folder, names[k]);
#ifdef _WIN32
            path = snx_format("%s/%s.exe", folder, names[k]);
#else
            path = snx_path_join(folder, names[k]);
#endif
            assert(!snx_mkdirs(data) && !snx_write_file(path, "game", 4) && !snx_make_executable(path));
            own = snx_format("%s/%s.log", folder, names[k]);
            log = gui_game_log(path);
            if (times[k]) { /* the log as the runtime writes it, where gui_game_log says it is */
                assert(!snx_write_file(own, "log", 3) && !snx_set_file_time(own, times[k]));
                assert(log && snx_same_file(log, own));
            }
            free(own);
            free(log);
            free(path);
            free(data);
            free(folder);
        }
#ifdef _WIN32
        path = snx_path_join(d, "flat.exe"); /* an older version's, laid out beside the others: its name's log */
#else
        path = snx_path_join(d, "flat");
#endif
        own = snx_path_join(d, "flat.log");
        assert(!snx_write_file(path, "game", 4) && !snx_make_executable(path));
        assert(!snx_write_file(own, "log", 3) && !snx_set_file_time(own, 2000));
        log = gui_game_log(path);
        assert(log && snx_same_file(log, own));
        free(log);
        free(own);
        free(path);
        for (k = 0; k < 2; ++k) { /* the dumps; b.nsp with a newer log of its name, which is no run's */
            path = snx_path_join(d, k ? "b.nsp" : "a.nsp");
            assert(!snx_write_file(path, "dump", 4));
            free(path);
        }
        own = snx_path_join(d, "b.nsp.log");
        assert(!snx_write_file(own, "log", 3) && !snx_set_file_time(own, 9000));
        free(own);
        { /* where the game's <name>.folders records another state folder, the runtime's log is there while it is */
            char *folder = snx_path_join(d, "gamma"), *record = snx_path_join(folder, "gamma.folders");
            char *state = snx_temp_dir(NULL), *text = snx_format("state=%s\n", state), *there;
#ifdef _WIN32
            path = snx_path_join(folder, "gamma.exe");
#else
            path = snx_path_join(folder, "gamma");
#endif
            assert(state && !snx_write_file(record, text, strlen(text)));
            log = gui_game_log(path);
            there = snx_format("%s/gamma.log", state);
            assert(!snx_write_file(there, "log", 3) && log && snx_same_file(log, there));
            free(log);
            snx_remove_tree(state); /* gone: beside the game again */
            log = gui_game_log(path);
            own = snx_format("%s/gamma.log", folder);
            assert(!snx_write_file(own, "log", 3) && log && snx_same_file(log, own) && !remove(own));
            assert(!remove(record));
            free(own);
            free(log);
            free(there);
            free(text);
            free(state);
            free(path);
            free(record);
            free(folder);
        }
        assert(gui_games_add(&g, d, GUI_GAMES_RUN | GUI_GAMES_DUMP, error, sizeof error));
        assert(gui_games_group(&g, "Recompiled", "Dumps", NULL, NULL) && g.count == 9);
        assert(!strcmp(g.names[1], "alpha") && !strcmp(g.names[5], "gamma")); /* by name first */
        assert(gui_games_by_last_opened(&g) && g.count == 9);
        assert(gui_games_heading(&g, 0) && !strcmp(g.shown[0], "Recompiled") && gui_games_heading(&g, 6) &&
               !strcmp(g.shown[6], "Dumps"));
        for (k = 0, at = 1; k < sizeof order / sizeof *order; ++k, ++at) {
            if (at == 6) ++at; /* the heading */
            assert(!strncmp(g.names[at], order[k], strlen(order[k])));
        }
        gui_games_free(&g);
        snx_remove_tree(d);
        free(d);
    }
    return 0;
}
