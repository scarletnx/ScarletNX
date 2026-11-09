/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_BROWSER_H
#define SNX_GUI_BROWSER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* What a Switch dump is, as the driver read it from its content meta and control.nacp: title.ini, which its -x --icon
   job for the launcher writes beside the dump's icon */
typedef struct {
    uint64_t title;     /* its own title ID */
    uint64_t game;      /* the game's it belongs to, its own for a game's dump */
    uint64_t version;   /* its content meta's version, SNX_NO_VERSION for none */
    char name[0x200];   /* its control.nacp's name, "" for none (a DLC has none) */
    char display[0x20]; /* its control.nacp's version ("1.0.1"), "" for none */
} GuiDumpTitle;
/* the title.ini at path read into title: true; false for none, or one that names no title and game */
bool gui_dump_title(const char* path, GuiDumpTitle* title);
/* The updates and DLCs of one game among the dumps, the item gui_games_group lists for them all */
typedef struct {
    uint64_t game; /* its title ID */
    char* name;    /* what it is called: its update's control.nacp's name, its dump's, its program's, else its ID */
    char* program; /* the game made already, which they go in: its program as Games lists it; NULL for none */
    char* dump;    /* else the game's dump among the dumps listed, which they are recompiled with; NULL for none */
    /* their dumps: count to put in, the newest update's first when update, then each DLC's by its title ID; then the
       left others, left out (an older update's, a DLC's second copy) */
    char** files;
    size_t count, left;
    bool update;        /* files[0] is an update's */
    char version[0x20]; /* the update's version as the player reads it: its control.nacp's, else v and its number */
} GuiAddonDumps;
typedef struct {
    char** names; /* basenames: a game's folder, or an older version's file */
    char** shown; /* what each is shown as, sorted in each folder: the name in its control.nacp, else names' */
    char** paths; /* where each is */
    size_t* addons; /* how many updates and DLCs each has beside it (gui_game_addons), and mods (gui_game_mods) */
    size_t count;
    size_t room; /* the entries the lists have room for, one past the last included */
    /* the Updates and DLCs group's items (gui_games_group), set_count of them: an item's path is its files[0] */
    GuiAddonDumps* sets;
    size_t set_count;
} GuiGames;
/* what a folder's games are: ready to run (executables; on Android a game's code or a dump), the other consoles'
   packages (3DS CIA and Vita VPK files, GBA ROMs, each opened with its driver), and dumps (NSP, XCI and NRO files),
   prepared when they run */
enum { GUI_GAMES_RUN = 1, GUI_GAMES_CIA = 2, GUI_GAMES_DUMP = 4 };
/* A folder's games of kinds added after those listed: its own files, and each folder of it that holds its game
   (<name>/<name>), named by the folder; nothing deeper. False, error saying why, when it cannot be read */
bool gui_games_add(GuiGames* games, const char* directory, int kinds, char* error, size_t size);
/* and with depth, the games of its other folders too, down that many levels; a hidden folder left alone. The folder's
   own error alone is said */
bool gui_games_add_deep(GuiGames* games, const char* directory, int kinds, int depth, char* error, size_t size);
/* how gui_games_group groups: each group's heading, and what it asks of the dumps */
typedef struct {
    const char *ready, *dumps, *addons;               /* the headings; dumps NULL leaves out every dump and CIA */
    bool (*made)(const char* path, const void* user); /* a dump made already, or put in its game: NULL for none */
    /* what the dump at path is (gui_dump_title), when it is known: NULL for none */
    bool (*titled)(const char* path, const void* user, GuiDumpTitle* title);
    const void* user;
} GuiGroups;
/* games in three groups: those ready to run, the dumps and CIAs, each in the order it had, then the Updates and DLCs,
   an item a game (GuiAddonDumps) for the update and DLC dumps of it, as titled says of each, by what it is shown as
   ("<name>: update 1.0.1 + 3 DLCs"); when two have games, each under a heading of its own (an entry whose path is NULL,
   its text shown). One group alone gets none here: Games heads it itself while Recent Games shows rows above it. A dump
   made already (made says so of its path) is left out, and with dumps NULL every dump and CIA, the ready games alone.
   False when out of memory, games as they were */
bool gui_games_group(GuiGames* games, const GuiGroups* groups);
/* the Updates and DLCs item at entry i of games, NULL for any other entry */
const GuiAddonDumps* gui_games_addon_set(const GuiGames* games, size_t i);
/* whether entry i of games is a group's heading, which nothing runs */
bool gui_games_heading(const GuiGames* games, size_t i);
/* whether entry i of games goes in gui_games_group's second group, the dumps': a dump, a CIA, a VPK or a GBA ROM */
bool gui_games_dumps_group(const GuiGames* games, size_t i);
/* The log the runtime writes for the game whose program is at path, again at each run: <name>.log in its state folder
   (gui_game_kind_dir), <name> the game's folder for one of its own, else the program's name without .exe. Owned, NULL
   when out of memory */
char* gui_game_log(const char* path);
/* games in each group (from a heading to the next) by when each last ran, its log's time (gui_game_log), the newest
   first; one never run (no log), a dump, a CIA and a VPK after them, each in the order it had, as the headings stay.
   False when out of memory, games as they were */
bool gui_games_by_last_opened(GuiGames* games);
/* whether directory holds a folder that is no game and not hidden: one whose games a deep add would list */
bool gui_has_folders(const char* directory, int kinds);
/* games/'s alone: its executable files and game folders, or with cia CIA and VPK files (case-insensitive extension, no execute
   bit needed). Never recurse into data/save directories. */
bool gui_games_scan(GuiGames* games, const char* directory, bool cia, char* error, size_t size);
void gui_games_free(GuiGames* games);
/* The bytes of the files under path, in its folders too, links not followed: 0 for no such folder, or "". It stops,
   with what it counted, once *stop is set by another thread */
uint64_t gui_tree_size(const char* path, const int* stop);
/* An NRO, XCI or NSP (or a decrypted one) by its name: a dump the driver prepares, not a game ready to run. So is an
   extracted game's folder (snx_extracted_game), which a library lists by its folder's name and a / after it */
bool gui_is_dump(const char* name);
/* A game's program in a folder of its own beside its data (<name>/<name>, .exe on Windows, and <name>/<name>_data/),
   or an update's or a DLC's beside it (<name>/<name>_<its title ID>), as the recompile lays games out: the driver
   plays it, and recompiles it first when another build made it. A game an older version laid out beside the others
   is not one */
bool gui_game_folder(const char* path);
/* The program of the game in directory/name, a folder of its own as gui_game_folder has it (<name>/<name>, .exe on
   Windows, <name>_code.so on Android), as Games lists it: owned, else NULL */
char* gui_folder_game(const char* directory, const char* name);
/* whether path is a game's program in the folder named for it, <folder>/<name> (.exe cut): a 3DS game's shape */
bool gui_game_program(const char* path);
/* What Games shows the game whose program is at path as, a game of a folder of its own: the name in its
   <name>_data/control.nacp, or a Vita game's TITLE in its sce_sys/param.sfo, else its folder's name. Owned */
char* gui_game_shown(const char* path);
/* The icon the extraction kept for the game whose program is at path, a game of a folder of its own: its
   <name>_data/icon.jpg, or a Vita game's sce_sys/icon0.png (patch/ before app/), a PNG; owned, else NULL (none
   there, or no such game) */
char* gui_game_icon(const char* path);
/* The updates and DLCs of the game at path, made beside it in its folder (<name>/<name>_<add-on's title ID>_data/, an
   update's program beside it), into addons (emptied first): names <name>_<its title ID>, shown what the player reads
   ("Update 1.2.0" from its control.nacp, "DLC 1"), paths an update's program or a DLC's data folder, an update
   first. True when it has any; false, addons empty, when it has none or memory ran out */
bool gui_game_addons(const char* path, GuiGames* addons);
/* The mods folder of the game at path, a program in a folder of its own (<name>/<name>_data/mods/, snx_mods_folder),
   into *folder (owned, NULL for any other game), and its mods (snx_mods_list) into *names when names is not NULL:
   their count */
size_t gui_game_mods(const char* path, char** folder, char*** names);
/* The other programs of a collection's application the game at path (its program, in a folder of its own) has made
   beside it, each one's index into programs (room for SNX_PROGRAMS_MAX), the game's own (0) left out: their count. A
   program plays with the update the game's addons= checks when that update has it made (game_ini.h's
   snx_program_played) */
int gui_game_programs(const char* path, int* programs);
/* The game a run of the driver made ready, which a phone's game activity plays: name, as the driver's "Ready NAME"
   gives it, beside file, the game made already that the run was given (the program the driver plays for it: its
   checked update's, or a collection's program that --program asks for), else for a dump (file NULL) games/NAME/NAME,
   where the driver made it. Owned */
char* gui_ready_game(const char* file, const char* games, const char* name);
/* The cheats folder of the game at path, a program in a folder of its own (<name>/<name>_data/cheats/,
   snx_cheats_folder), into *folder (owned, NULL for any other game), and its cheats' names (snx_cheats_list) into
   *names when names is not NULL: their count */
size_t gui_game_cheats(const char* path, char** folder, char*** names);
/* The folder from copied whole to the folder to, which is not there yet: first into a hidden .<its name>.adding beside
   it (one a stopped copy left is made again), renamed to to once every file is in, so the mods list never shows a mod
   copied in part, and taken away when a file fails. from is left as it is. True; false with why naming the file that
   failed, or the link a mod cannot hold, and the system's reason */
bool gui_copy_mod(const char* from, const char* to, char* why, size_t size);
/* A 3DS CIA, a Vita VPK, a GBA ROM, an Android APK, XAPK or APKS, by its name */
bool gui_is_cia(const char* name);
bool gui_is_vpk(const char* name);
bool gui_is_gba(const char* name);
bool gui_is_apk(const char* name);
#endif
