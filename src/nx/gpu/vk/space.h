/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The free space of one block of memory, in units: the runs of free units in address order, where no two runs touch.
   A request takes the shortest run that holds it at its alignment (the lowest of equals), and leaves the rest of the
   run free. A range given back joins the free runs on both sides of it, whatever their sizes.

   A buddy system was here first. It joins two free pieces only when they are the two halves of one piece, so the
   64 KiB blocks of one game's sparse textures, freed here and there, left the image blocks with 613 MB of 3322 MB
   free and no free piece over 1 MiB, and the textures that needed more went to system memory.

   The runs are one array: a block of device memory holds a few hundred to a few thousand of them, and a request
   reads them once. It knows nothing of Vulkan: devmem.c keeps one for each block of device memory, and dma.c one for
   each of its snapshot buffers. Its caller holds the lock */
#ifndef SNX_VK_SPACE_H
#define SNX_VK_SPACE_H
#include "common/cpu/aarch64/cpu.h"

typedef struct {
    u32 at, n;
} SpaceRun;
typedef struct {
    SpaceRun* run; /* the free runs by address */
    u32 nrun, cap;
    u32 units;   /* the block's size */
    u32 used;    /* units handed out */
    u32 largest; /* the longest free run */
} Space;

/* a block of units (1 or more), all of it free. 0 without memory */
int space_init(Space* s, u32 units);
void space_destroy(Space* s);
/* n units (1 or more) at a multiple of align units (a power of two): 1 and their first unit in *at, or 0 when no free
   run holds them there, or without memory for the runs */
int space_take(Space* s, u32 n, u32 align, u32* at);
/* the n units at `at`, which space_take handed out, or a part of them: they are free again */
void space_give(Space* s, u32 at, u32 n);

#endif
