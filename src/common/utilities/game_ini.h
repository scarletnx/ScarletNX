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
#endif
