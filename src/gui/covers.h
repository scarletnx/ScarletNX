/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Settings > GBA > Covers' box art (gba_covers=boxart): a GBA ROM's No-Intro name, found by its CRC-32 in
   libretro-database's No-Intro list of the GBA's games, and the link of the box libretro-thumbnails keeps under that
   name, as RetroArch finds a game's thumbnail. No SDL, no network: the launcher fetches (gui/main.c) */
#ifndef SNX_GUI_COVERS_H
#define SNX_GUI_COVERS_H
#include <stddef.h>
#include <stdint.h>

/* libretro-database's No-Intro list of the GBA's games, a clrmamepro DAT */
#define GUI_COVERS_LIST_URL                                                                                            \
    "https://raw.githubusercontent.com/libretro/libretro-database/master/"                                             \
    "metadat/no-intro/Nintendo%20-%20Game%20Boy%20Advance.dat"
/* the folder of libretro-thumbnails' boxes of the GBA's games, a PNG each, named by the game's No-Intro name */
#define GUI_COVERS_BOX_URL                                                                                             \
    "https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Game_Boy_Advance/master/Named_Boxarts/"

/* the CRC-32 the list keys a ROM by: of its whole file */
uint32_t gui_cover_crc(const void* rom, size_t size);
/* the name the list (its text, n bytes) gives the game whose ROM has crc, into out: 1, else 0 with out "" (no game of
   the list has it, or the name does not fit) */
int gui_cover_name(const char* list, size_t n, uint32_t crc, char* out, size_t size);
/* the link of the box of the game named name: its file named as libretro-thumbnails names it (each of &*\/:`<>?\| an
   _), percent-encoded, under GUI_COVERS_BOX_URL. 1, else 0 when it does not fit in out */
int gui_cover_url(const char* name, char* out, size_t size);
/* 1 when bytes start as a PNG does */
int gui_cover_png(const void* bytes, size_t size);
#endif
