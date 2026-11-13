/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The flat window recompiled code runs on. The board's regions move into the window at their canonical
   addresses, with the console's mirrors mapped to the same pages, so generated code reaches every region
   with one pointer add and no access hooks. */
#ifndef SNX_AGB_MAP_H
#define SNX_AGB_MAP_H
#include "common/memory/host.h"
#include <stddef.h>
#include <stdint.h>

/* the largest host page a mirror can be a view at: the smallest unit a region repeats in (IWRAM, VRAM's last 32 KiB) */
#define AGB_MAP_VIEW_MAX 0x8000u

typedef struct AgbBoard AgbBoard;
/* The flat window: the 256 MiB the console decodes at the window's base, the guest's 0x00000000, in a reservation of
   every 32-bit address. Past those 28 bits it is plain memory no load or store reaches: the bus hook takes them. */
typedef struct {
    uint8_t* base;              /* AGB_MAP_SIZE bytes and the guard, the guest's 0x00000000 */
    SnxHostMemory backings[10]; /* one per region, mapped at each of its mirrors */
    int backing_count;
    size_t page;  /* the host's view size (snx_host_page) */
    int mirrored; /* 0: page is past AGB_MAP_VIEW_MAX, and each mirror is RAM of its own that diverges */
    int adopted;  /* the board's regions point into the window */
} AgbMap;

/* Builds the window and points the board's memory regions into it. Returns 0, or -1 with a message. */
int agb_map_init(AgbMap* map, AgbBoard* board, char* error, size_t error_size);
void agb_map_destroy(AgbMap* map, AgbBoard* board);
#endif
