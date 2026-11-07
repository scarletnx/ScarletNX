/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A collection's programs (common.h's snx_program_index): their IDs told from an update's and a DLC's, the names
   their game's folder gives them, the one a run plays (snx_program_pick, program 0 said for one that is not made),
   and the hand-over a program that ends leaves the one it starts (game_ini.h's SnxNext), written, read back and
   refused when it is not one */
#include "common/utilities/common.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/mods.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static void check(int good, const char* expression, int line) {
    ++checks;
    if (!good) {
        fprintf(stderr, "collection_programs.c:%d: check failed: %s\n", line, expression);
        exit(1);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)

#define GAME   0x0100ABCD12340000ull
#define UPDATE 0x0100ABCD12340800ull

/* each ID's kind and program index: the game's own and its update are program 0, its programs follow its ID and
   their patches the update's, a DLC is none of its programs (numbered from 0x1000 or from a base of its own) */
static void kinds(void) {
    CHECK(snx_addon_kind(GAME, GAME) == SNX_ADDON_NONE && snx_program_index(GAME, GAME) == 0);
    CHECK(snx_addon_kind(GAME, UPDATE) == SNX_ADDON_UPDATE && snx_program_index(GAME, UPDATE) == 0);
    CHECK(snx_addon_kind(GAME, GAME + 1) == SNX_ADDON_PROGRAM && snx_program_index(GAME, GAME + 1) == 1);
    CHECK(snx_addon_kind(GAME, GAME + 3) == SNX_ADDON_PROGRAM && snx_program_index(GAME, GAME + 3) == 3);
    CHECK(snx_addon_kind(GAME, GAME + 255) == SNX_ADDON_PROGRAM && snx_program_index(GAME, GAME + 255) == 255);
    CHECK(snx_addon_kind(GAME, UPDATE + 2) == SNX_ADDON_PROGRAM && snx_program_index(GAME, UPDATE + 2) == 2);
    /* DLCs: the usual numbering, past it, and one of a base of its own */
    CHECK(snx_addon_kind(GAME, GAME + 0x1001) == SNX_ADDON_DLC && snx_program_index(GAME, GAME + 0x1001) == -1);
    CHECK(snx_addon_kind(GAME, GAME + 0x17FF) == SNX_ADDON_DLC && snx_program_index(GAME, GAME + 0x17FF) == -1);
    CHECK(snx_addon_kind(GAME, 0x0100ABCD5555A001ull) == SNX_ADDON_DLC);
    /* past an index's byte: no program of it */
    CHECK(snx_addon_kind(GAME, GAME + 256) == SNX_ADDON_DLC && snx_program_index(GAME, GAME + 256) == -1);
    CHECK(snx_program_index(GAME, UPDATE + 256) == -1 && snx_program_index(GAME, GAME - 1) == -1);
    /* a 3DS game's: its update its low half under 0x0004000E, its DLC under 0x0004008C (the low half's 28 bits), no
       programs; another game's or a system title's none of its */
    CHECK(snx_addon_kind(0x0004000001234500ull, 0x0004000E01234500ull) == SNX_ADDON_UPDATE);
    CHECK(snx_addon_kind(0x0004000001234500ull, 0x0004008C01234500ull) == SNX_ADDON_DLC);
    CHECK(snx_addon_kind(0x0004000001234500ull, 0x0004000E01234501ull) == SNX_ADDON_NONE);
    CHECK(snx_addon_kind(0x0004000001234500ull, 0x0004008C09999900ull) == SNX_ADDON_NONE);
    CHECK(snx_addon_kind(0x0004000001234500ull, 0x0004000001234501ull) == SNX_ADDON_NONE);
    CHECK(snx_addon_kind(0x0004000001234500ull, 0x0004001001234500ull) == SNX_ADDON_NONE);
    /* the game a dump's NCA belongs to, without a content meta: a patch's and a program patch's, a DLC's */
    CHECK(snx_addon_guess(UPDATE) == GAME && snx_addon_guess(UPDATE + 3) == GAME);
    CHECK(snx_addon_guess(GAME + 0x1002) == GAME && snx_addon_guess(GAME + 2) == GAME);
}

/* entries and names: an update's programs carry its version as the update does, the game's own none; an entry names
   back its ID; the names of the game's folder for each program, with an update and without */
static void names(void) {
    char entry[32], name[SNX_PATH_SIZE];
    SnxU64 id = 0;
    snx_addon_entry(GAME, GAME + 2, 65536, entry);
    CHECK(!strcmp(entry, "0100ABCD12340002")); /* the game's program: its ID alone */
    snx_addon_entry(GAME, UPDATE + 2, 196608, entry);
    CHECK(!strcmp(entry, "0100ABCD12340802v196608"));
    CHECK(snx_addon_entry_id(GAME, "0100ABCD12340802v196608", &id) && id == UPDATE + 2);
    CHECK(snx_addon_entry_id(GAME, "0100ABCD12340001", &id) && id == GAME + 1);
    CHECK(!snx_addon_entry_id(GAME, "0100ABCD12340001v5", &id)); /* a program of the game's has no version */
    CHECK(snx_addon_name("0100ABCD12340000", "0100ABCD12340000_0100ABCD12340003", &id) && id == GAME + 3);
    CHECK(!snx_program_name("0100ABCD12340000", NULL, 0, name, sizeof name) && !strcmp(name, "0100ABCD12340000"));
    CHECK(!snx_program_name("0100ABCD12340000", NULL, 3, name, sizeof name) &&
          !strcmp(name, "0100ABCD12340000_0100ABCD12340003"));
    CHECK(!snx_program_name("0100ABCD12340000", "0100ABCD12340800v196608", 0, name, sizeof name) &&
          !strcmp(name, "0100ABCD12340000_0100ABCD12340800v196608"));
    CHECK(!snx_program_name("0100ABCD12340000", "0100ABCD12340800v196608", 2, name, sizeof name) &&
          !strcmp(name, "0100ABCD12340000_0100ABCD12340802v196608"));
    /* a DLC is no update to take a version from; past the index's byte, and a game named by no title ID, no name */
    CHECK(snx_program_name("0100ABCD12340000", "0100ABCD12341001", 2, name, sizeof name));
    CHECK(snx_program_name("0100ABCD12340000", NULL, SNX_PROGRAMS_MAX, name, sizeof name));
    CHECK(snx_program_name("Homebrew", NULL, 1, name, sizeof name));
}

/* a program's code made in folder, as the recompile makes it */
static void made(const char* folder, const char* name) {
    char* code = snx_program_code(folder, name);
    CHECK(!snx_write_file(code, "code", 4));
    free(code);
}

/* the program a run plays: the one asked when made, its update's when the update checked has it, else program 0 with
   both values said; the made ones listed by index */
static void picks(const char* root) {
    char *folder = snx_path_join(root, "0100ABCD12340000"), *ini, out[SNX_PATH_SIZE], why[512];
    unsigned char list[SNX_PROGRAMS_MAX];
    CHECK(!snx_mkdirs(folder));
    made(folder, "0100ABCD12340000");
    made(folder, "0100ABCD12340000_0100ABCD12340001");
    made(folder, "0100ABCD12340000_0100ABCD12340002");
    made(folder, "0100ABCD12340000_0100ABCD12340003");
    CHECK(snx_game_programs(folder, "0100ABCD12340000", list) == 4 && list[0] && list[3] && !list[4]);
    CHECK(snx_program_pick(folder, "0100ABCD12340000", 2, out, sizeof out, why, sizeof why) == 2);
    CHECK(!strcmp(out, "0100ABCD12340000_0100ABCD12340002") && !why[0]);
    CHECK(snx_program_pick(folder, "0100ABCD12340000", 5, out, sizeof out, why, sizeof why) == 0);
    CHECK(!strcmp(out, "0100ABCD12340000"));
    CHECK(!strcmp(why, "program 5 is not one of this application's programs 0-3: it starts program 0"));
    /* the update checked: its programs where it has them made, the game's own where it has not */
    ini = snx_game_ini(folder, "0100ABCD12340000");
    CHECK(!snx_ini_record(ini, "addons", "0100ABCD12340800v196608,0100ABCD12341001"));
    made(folder, "0100ABCD12340000_0100ABCD12340800v196608");
    made(folder, "0100ABCD12340000_0100ABCD12340802v196608");
    CHECK(snx_program_played(folder, "0100ABCD12340000", 0, out, sizeof out) &&
          !strcmp(out, "0100ABCD12340000_0100ABCD12340800v196608"));
    CHECK(snx_program_played(folder, "0100ABCD12340000", 2, out, sizeof out) &&
          !strcmp(out, "0100ABCD12340000_0100ABCD12340802v196608"));
    CHECK(snx_program_played(folder, "0100ABCD12340000", 1, out, sizeof out) &&
          !strcmp(out, "0100ABCD12340000_0100ABCD12340001"));
    CHECK(!snx_program_played(folder, "0100ABCD12340000", 9, out, sizeof out) &&
          !strcmp(out, "0100ABCD12340000_0100ABCD12340009"));
    CHECK(snx_program_pick(folder, "0100ABCD12340000", 9, out, sizeof out, why, sizeof why) == 0 &&
          !strcmp(out, "0100ABCD12340000_0100ABCD12340800v196608"));
    /* a gap in the made ones is listed one by one */
    {
        char* code = snx_program_code(folder, "0100ABCD12340000_0100ABCD12340001");
        CHECK(!remove(code));
        free(code);
    }
    snx_program_pick(folder, "0100ABCD12340000", 1, out, sizeof out, why, sizeof why);
    CHECK(!strcmp(why, "program 1 is not one of this application's programs 0, 2 and 3: it starts program 0"));
    free(ini);
    free(folder);
}

/* each program's own mods, as Eden lays load/<its ID>/ over load/<its application's>/: both folders' mods by name,
   its own after its application's of the same name, mods_off= taking either out */
static void mods(const char* root) {
    char *folder = snx_path_join(root, "mods"), *game = snx_path_join(folder, "game"), *own, **paths;
    static const char* const made_ones[] = {"game/a/romfs", "game/c/romfs", "own/b/romfs", "own/c/romfs",
                                            "own/d/romfs"};
    size_t i, count;
    own = snx_path_join(folder, "own");
    for (i = 0; i < sizeof made_ones / sizeof *made_ones; i++) {
        char* path = snx_path_join(folder, made_ones[i]);
        CHECK(!snx_mkdirs(path));
        free(path);
    }
    count = snx_mods_played_of(game, own, "d", "romfs", &paths);
    CHECK(count == 4);
    for (i = 0; i < count; i++) /* Windows' separators as / for the comparison */
        for (char* c = paths[i]; *c; c++)
            if (*c == '\\') *c = '/';
    CHECK(strstr(paths[0], "game/a") && strstr(paths[1], "own/b") && strstr(paths[2], "game/c") &&
          strstr(paths[3], "own/c"));
    snx_free_strings(paths, count);
    count = snx_mods_played_of(game, NULL, NULL, "romfs", &paths); /* the game's own program: its own alone */
    CHECK(count == 2);
    snx_free_strings(paths, count);
    {
        char *path = snx_program_own_mods("/games/0100ABCD12340000", "0100ABCD12340000", 2),
             *expected = snx_path_join("/games/0100ABCD12340000", "0100ABCD12340000_0100ABCD12340002_data/mods");
        CHECK(path && !strcmp(path, expected));
        free(path);
        free(expected);
        CHECK(!snx_program_own_mods("/games/0100ABCD12340000", "0100ABCD12340000", 0));
    }
    free(own);
    free(game);
    free(folder);
}

/* the hand-over written and read back as it was, storages of other sizes and an empty one among them; a file cut or
   with a line that is not one refused, the line and value said */
static void handover(const char* root) {
    char *path = snx_path_join(root, "0100ABCD12340000.next"), why[512];
    static const unsigned char first[] = {0x00, 0xff, 0x10, 0x7f};
    unsigned char big[3000];
    SnxNext next = {0}, back;
    size_t i;
    for (i = 0; i < sizeof big; i++) big[i] = (unsigned char)(i * 7);
    snprintf(next.program, sizeof next.program, "%s", "0100ABCD12340000_0100ABCD12340002");
    next.index = 2;
    next.previous = 0;
    next.context = 1;
    next.serial = 4;
    next.count = 3;
    next.storage[0] = (unsigned char*)first;
    next.size[0] = sizeof first;
    next.storage[1] = big;
    next.size[1] = sizeof big;
    next.storage[2] = big;
    next.size[2] = 0;
    CHECK(!snx_next_write(path, &next));
    CHECK(snx_next_read(path, &back, why, sizeof why) == 1 && !why[0]);
    CHECK(!strcmp(back.program, next.program) && back.index == 2 && back.previous == 0 && back.context == 1);
    CHECK(back.serial == 4 && back.count == 3 && back.size[0] == sizeof first && back.size[1] == sizeof big);
    CHECK(back.size[2] == 0 && !memcmp(back.storage[0], first, sizeof first) &&
          !memcmp(back.storage[1], big, sizeof big));
    snx_next_free(&back);
    /* the system started the one before: previous -1 */
    next.previous = -1;
    next.count = 0;
    CHECK(!snx_next_write(path, &next) && snx_next_read(path, &back, why, sizeof why) == 1 && back.previous == -1);
    snx_next_free(&back);
    /* none: 0, nothing said */
    CHECK(!remove(path) && snx_next_read(path, &back, why, sizeof why) == 0 && !why[0]);
    /* not one: a line without =, a key that is none, a storage of odd digits or past 8, a field missing */
    {
        static const char* const bad[][2] = {
            {"program=x\nindex=1\nprevious=0\ncontext=0\nserial=1\njunk\n", "line 6, junk, is no key=value"},
            {"program=x\nindex=1\nprevious=0\ncontext=0\nserial=1\ncolour=red\n", "colour=red is none of program="},
            {"program=x\nindex=300\nprevious=0\ncontext=0\nserial=1\n", "index=300 is none of program="},
            {"program=x\nindex=1\nprevious=0\ncontext=0\nserial=1\nstorage=abc\n", "a storage of 3 hex digits"},
            {"program=x\nindex=1\nprevious=0\ncontext=0\nserial=1\nstorage=zz\n", "storage byte 0, zz, is not hex"},
            {"program=x\nindex=1\nprevious=0\nserial=1\n", "lacks context="},
            {"program=../x\nindex=1\nprevious=0\ncontext=0\nserial=1\n", "program=../x is none of program="}};
        for (i = 0; i < sizeof bad / sizeof *bad; i++) {
            CHECK(!snx_write_file(path, bad[i][0], strlen(bad[i][0])));
            CHECK(snx_next_read(path, &back, why, sizeof why) == -1);
            if (!strstr(why, bad[i][1])) fprintf(stderr, "said \"%s\", not \"%s\"\n", why, bad[i][1]);
            CHECK(strstr(why, bad[i][1]) != NULL && back.count == 0);
        }
    }
    free(path);
}

int main(void) {
    char* root = snx_temp_dir(NULL);
    CHECK(root != NULL);
    kinds();
    names();
    picks(root);
    mods(root);
    handover(root);
    CHECK(!snx_remove_tree(root));
    free(root);
    printf("PASS a collection's programs: their IDs, names, the one played and the hand-over (%u checks)\n", checks);
    return 0;
}
