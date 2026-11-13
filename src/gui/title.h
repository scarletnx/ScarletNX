/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a game's file is, from its own header, with no key: the title Locate... compares with the game's before the
   game plays from that file (gui/main.c's missing_located). A Switch dump's comes from its driver instead (title.ini) */
#ifndef SNX_GUI_TITLE_H
#define SNX_GUI_TITLE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A 3DS game's title ID, and its product code ("CTR-P-ABCD", "" where it is not in the clear) into code, as its
   container says them, as the 3DS driver reads it: a CIA's TMD (the code from its first content's NCCH), a cartridge
   image's game partition (0) or a bare NCCH, alone, in a bundle (a ustar archive, its first entry named as a game) or
   in a z3ds file. True; false, why saying so, for a file that is none of them */
bool gui_ctr_title(const char* path, uint64_t* id, char* code, size_t code_size, char* why, size_t why_size);
/* A dumped Vita game's title ID, folder/sce_sys/param.sfo's TITLE_ID, into id: false for none */
bool gui_vita_title(const char* folder, char* id, size_t size);
/* A GBA ROM's header, its game code (0xAC, four characters) and its title (0xA0, up to twelve), and the ROM's FNV-1a, as
   the GBA driver's map of a game names the ROM it was made from. True; false, why saying so, for a file too short to be
   one or that does not read */
bool gui_gba_title(const char* rom, char code[5], char title[13], uint64_t* hash, char* why, size_t why_size);
/* The hash of the ROM the GBA game named name in folder was made from, as its map's first line names it
   (<name>_code.map): false for none */
bool gui_gba_made_from(const char* folder, const char* name, uint64_t* hash);
#endif
