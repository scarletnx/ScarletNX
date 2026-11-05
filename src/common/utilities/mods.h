/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's mods, as the driver, the recompiler, the launcher and the runtime share them. Each is a folder of the
   game's mods folder, <game>_data/mods/<mod>/, named as the player likes, with an exefs/ and a romfs/ laid over the
   game's (and its update's) the way an update's are: a file of exefs/ replaces the program's file of its name, which
   the game is recompiled with, and a file of romfs/ the RomFS file at its path, or is added to the RomFS. Every mod is
   played unless the game's mods_off= names it. Mods are laid in their names' order, a later one over those before */
#ifndef SNX_MODS_H
#define SNX_MODS_H
#include "common/utilities/common.h"

/* mods_off=: the mods not played, their folders' names with a / between two, which no folder's name has */
#define SNX_MODS_SEPARATOR '/'
/* the mods folder of the game whose data folder is data, owned */
char* snx_mods_folder(const char* data);
/* the mods folder of the game name in its folder (games/<name>), and its mods_off= in *off (NULL for none), owned */
char* snx_game_mods(const char* folder, const char* name, char** off);
/* A collection's other programs (common.h's snx_program_index) play their application's mods and mods of their own, as
   Eden lays a program's (patch_manager.cpp's GetModificationLoadRoot: load/<the program's ID>/ over
   load/<its application's>/): the mods of both folders by their names, a program's own after its application's of the
   same name, so its files win. Its own are in the data folder named by its own ID, <game>_<game + index>_data/mods,
   whichever update it plays with; mods_off= is the game's for both. The program of index in the folder of the game
   name: its own mods folder, owned; NULL for program 0 or a game named by no title ID */
char* snx_program_own_mods(const char* folder, const char* name, int index);
/* the mods in folder (snx_mods_folder), sorted by name, into *names (snx_free_strings): their count, 0 for none or no
   folder. A file or a hidden folder (.name) is none */
size_t snx_mods_list(const char* folder, char*** names);
/* 1 when off (mods_off=, NULL for none) names the mod name */
int snx_mod_off(const char* off, const char* name);
/* 1 for a file of a mod's exefs/ that is part of the program: a module (rtld, main, subsdk<n>, sdk) or main.npdm */
int snx_mods_exefs_file(const char* name);
/* the part (exefs, romfs) of the mods played (off as mods_off= has it) that have it as a folder, in the order they are
   laid, into *paths (snx_free_strings): their count */
size_t snx_mods_played(const char* folder, const char* off, const char* part, char*** paths);
/* the same for a program with mods of its own in own (NULL for none), laid after the application's in folder */
size_t snx_mods_played_of(const char* folder, const char* own, const char* off, const char* part, char*** paths);
/* the files of the played mods' exefs/ that are part of the program (snx_mods_exefs_file), each from the last mod laid
   that has it: their names into *names, sorted, and their paths into *paths (each snx_free_strings). Their count */
size_t snx_mods_exefs(const char* folder, const char* off, char*** names, char*** paths);
size_t snx_mods_exefs_of(const char* folder, const char* own, const char* off, char*** names, char*** paths);
/* what the program of a game with mods in folder is built from beyond its own ExeFS, the files snx_mods_exefs gives
   and then the patches snx_mods_patches gives (common/utilities/patches.h), by their names and bytes, as 16 hex digits,
   owned. "" when no mod played has any */
char* snx_mods_stamp(const char* folder, const char* off);
char* snx_mods_stamp_of(const char* folder, const char* own, const char* off);
/* what a game's <name>.abi holds when the recompile made it for abi with mods, its mods' stamp (snx_mods_stamp): the
   ABI, and " mods " and that stamp unless it is "", owned */
char* snx_game_stamp_for(unsigned abi, const char* mods);
/* the stamp of the mods the game at base (<folder>/<name>, a game's, its update's or another program's) plays now, its
   game's (the folder's name) and a program's own (snx_program_own_mods), owned */
char* snx_game_mods_stamp(const char* base);
/* 1 when the game at base was made as expected says (snx_game_stamp_for): its <base>.abi holds it, then its made line
   or nothing, a partial write or anything else after it being no completed build. The driver recompiles a game that
   was not before it plays it, and the launcher compiles it through its queue */
int snx_game_stamp_is(const char* base, const char* expected);
/* how the recompile made a game, the made line after the stamp in its <name>.abi ("\nmade opt=2 aot=1 found=N
   code=N"), which the stamp is not compared by: the -O, whether it made code ahead of time (0: --jit-only, the JIT
   compiles it all), and of the instructions in the modules' code those its analysis found and made ahead of time.
   Unknown, from a stamp an older version wrote: opt 0, aot -1, code 0 */
typedef struct {
    char opt;
    int aot;
    unsigned long long found, code;
} SnxGameMade;
/* made as the line the stamp ends with, its "\n" first, owned */
char* snx_game_made_line(const SnxGameMade* made);
/* the size of the stamp proper in a <name>.abi's text: what comes before its made line (the last "\nmade "), else all */
size_t snx_game_stamp_size(const char* text, size_t size);
/* the made line of the game at base's <base>.abi in made: 1, else 0 with made unknown (no stamp, or one without it) */
int snx_game_made_read(const char* base, SnxGameMade* made);
#endif
