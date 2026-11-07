/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A recompiled game's .ini as the driver, the launcher and the runtime share it: where it is, the keys of [Game] the
   driver and the launcher write into it (source=, addons=), and the add-ons addons= checks */
#ifndef SNX_GAME_INI_H
#define SNX_GAME_INI_H
#include "common/utilities/common.h"

/* the game's settings, <name>.ini, where the runtime reads them: in its folder, or in an installed build's config
   folder (snx_user_path). Owned */
char* snx_game_ini(const char* folder, const char* name);
/* key's value in the .ini at path, the last one's, malloc'd: NULL for none, an empty one or no such file */
char* snx_ini_value(const char* path, const char* key);
/* key=value in [Game] of the .ini at path, every older key= taken out and the file made when there is none, left as
   it is when it has the value already. value has no line break. 0, or nonzero with a line saying why */
int snx_ini_record(const char* path, const char* key, const char* value);
/* 1 when the .ini at path has a key= line, an empty value's too: a game's own choice, which "nothing" can be */
int snx_ini_has(const char* path, const char* key);
/* every key= line of the .ini at path taken out, the file left as it is when it has none. 0, or nonzero with a line
   saying why */
int snx_ini_drop(const char* path, const char* key);
/* the dump the game was made from, owned: what Extract to tmpfs extracts again, and what a game made from its dump
   without extracting it reads its ExeFS and RomFS from. source= in the game's .ini (snx_game_ini's, else the one in its
   folder, where the 3DS, Vita and GBA drivers keep theirs), else <folder>/<name>.source, the
   link an older version made (Windows makes links only with the right to); NULL when that path does not fit (said).
   recorded, when not NULL: 1 when either names a dump, there or not */
char* snx_game_source(const char* folder, const char* name, int* recorded);
/* dump, absolute, as source= in [Game] of the game's .ini, the file made when there is none and left as it is when it
   names dump already. 0, or nonzero with a line saying why */
int snx_record_source(const char* folder, const char* name, const char* dump);

/* the console a game's folder plays, as system= in [Game] of the .ini in it, <folder>/<name>.ini, says: the launcher
   picks the driver, the options and what R does by it, and cmake/deploy.cmake the runtime it copies there. A folder
   an older version made says none: a 3DS one's .ini has model=, a Vita one holds ux0/, any other is a Switch game's */
typedef enum { SNX_SYSTEM_SWITCH, SNX_SYSTEM_3DS, SNX_SYSTEM_VITA, SNX_SYSTEM_GBA } SnxSystem;
SnxSystem snx_game_system(const char* folder, const char* name);
/* system='s word for each: "switch", "3ds", "vita", "gba" */
const char* snx_system_word(SnxSystem system);
/* system= in [Game] of the .ini in the game's folder, <folder>/<name>.ini, as the 3DS and Vita drivers keep theirs: the
   file made when there is none. 0, or nonzero with a line saying why */
int snx_record_system(const char* folder, const char* name, SnxSystem system);
/* The game folder's copy of the runtime, which plays the game from then on: runtime copied to copy, through a
   temporary file and a rename (a copy that is running keeps its old file), when copy is missing or another build's;
   nothing when runtime is copy. 1 when it copied, 0 when nothing had to, -1 with error saying why */
int snx_place_runtime(const char* runtime, const char* copy, char* error, size_t error_size);
/* addons=: the game's updates and DLCs played with it, their entries (snx_addon_name: a title ID, an update's with
   its version), a comma between two. The most a list holds */
#define SNX_ADDONS_MAX 2048
typedef struct {
    char entry[32]; /* 0100ABCD12341001, 0100ABCD12340800v65536 */
    SnxU64 id;
} SnxAddon;
/* text's entries that are add-ons of the game named game, into list (max of them), each title ID once: an update at
   most, the last one listed, as one runs at a time. Their count; anything else in text is left out */
size_t snx_addons_parse(const char* text, const char* game, SnxAddon* list, size_t max);
/* list as addons= writes it, malloc'd */
char* snx_addons_text(const SnxAddon* list, size_t count);
/* the add-on of entry checked in the addons= of the game named name in folder, what else is checked kept: an update
   in place of the one checked, of whichever version (every version of it has the same title ID), so the update
   installed last is the one played, older or newer. 0, or nonzero with a line saying why */
int snx_addon_check(const char* folder, const char* name, const char* entry);

/* ---- A collection's programs (common.h's snx_program_index), each made beside its game in the game's folder: its
   code and its stamp, <game>_<its ID>_code.so and .abi, and its data folder, which lies over the game's; it runs on the
   game's copy of the runtime (--game=), with the game's settings and saves */
/* the code of the program name in folder, <name>_code.so (.dll on Windows, .snxobj on iOS), owned: what says the
   program is made */
char* snx_program_code(const char* folder, const char* name);
/* the update the game name in folder plays with, its entry in its addons= ("" for none), into update */
void snx_game_update(const char* folder, const char* name, char update[32]);
/* the program of index that plays for the game name in folder: its checked update's of that index when it is made, else
   its own, into out. 1 when that one is made, else 0 */
int snx_program_played(const char* folder, const char* name, int index, char* out, size_t size);
/* which programs the game name in folder has made, made[i] for each index (snx_program_played), and their count */
int snx_game_programs(const char* folder, const char* name, unsigned char made[SNX_PROGRAMS_MAX]);
/* the program that plays for index (snx_program_played) into out, and its index: index's when it is made, else
   program 0, the collection's menu, with why saying both into why ("program 5 is not one of this application's
   programs 0-3: it starts program 0"), "" when the one asked plays */
int snx_program_pick(const char* folder, const char* name, int index, char* out, size_t size, char* why,
                     size_t why_size);

/* What a program a collection's program starts (am's ExecuteProgram) takes from the one that ends: the program, by
   its name in the game's folder (snx_program_played) and index, the index of the one that started it (am's
   GetPreviousProgramIndex), whether that one stored its open user (acc's StoreOpenContext) and the storages it left in
   the user channel, the first the bottom one. The one that ends writes it in <game>.next in the game's folder; its
   launcher starts the program it names, which takes the file away as it starts. serial counts the hand-overs since the
   program the player started, from 1: a launcher that finds the one it started a program with still there after it
   (the same serial) knows it never started */
#define SNX_NEXT_STORAGES    8         /* am's user channel holds as many */
#define SNX_NEXT_STORAGE_MAX 0x1000000 /* the bytes of a storage at most: 16 MiB */
typedef struct {
    char program[256];
    int index, previous, context;
    unsigned long serial;
    size_t count;
    unsigned char* storage[SNX_NEXT_STORAGES]; /* owned, size[i] bytes each */
    size_t size[SNX_NEXT_STORAGES];
} SnxNext;
/* <folder>/<name>.next, the hand-over of the game name in folder, owned */
char* snx_next_path(const char* folder, const char* name);
/* next into path, through a temporary file: 0, or nonzero with a line saying why */
int snx_next_write(const char* path, const SnxNext* next);
/* path into next: 1, 0 when there is none, -1 with why (its line, the value) for one that is not what snx_next_write
   writes; snx_next_free either way */
int snx_next_read(const char* path, SnxNext* next, char* why, size_t why_size);
void snx_next_free(SnxNext* next);
#endif
