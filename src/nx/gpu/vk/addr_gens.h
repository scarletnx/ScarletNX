/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Generations by address (docs/dev/draw-cost.md, item 3): a small hashed table of counts for each kind of address,
   one count for each slot. A write to a range adds one to the slot of each granule of ADDR_GENS_SHIFT bytes that the
   range touches. A reader adds up the slots of the granules of what it read: the sum moves after any write to them,
   and also after a write to a granule whose slot is the same (a collision, which only costs a check). A range wider
   than the table moves the count of every range (all). Counts only grow, so a sum that stays the same tells that no
   write came to the granules since the sum was read, as long as each write bumps after it is visible where the reader
   looks (pending_pages.c bumps after it counts a result, buffer.c after it stamps a page written).

   ADDR_GENS_GPU: by GPU address. pending_pages.c bumps it for the GPU pages that gain or lose a result of g_pend_any
   (and its tables), and for the pages whose mapping changes. Its writers hold vk_lock and pend_lock, so a bump there
   is a load and a store.
   ADDR_GENS_CPU: by guest address (the offset of a host pointer into guest memory). The write tracking (buffer.c)
   bumps it when it stamps pages written, on the scanner thread or on a check's thread.
   ADDR_GENS_WRITE: by GPU address, with tex_events_shadow on (render.c tex_events_on): each GPU-side write once the
   write record has published it (maxwell3d.c's write watch by GPU address), each draw to a render target, and each
   surface made or freed in the surface table, over their ranges. */
#ifndef SNX_VK_ADDR_GENS_H
#define SNX_VK_ADDR_GENS_H
#include "common/cpu/aarch64/cpu.h"

enum { ADDR_GENS_GPU, ADDR_GENS_CPU, ADDR_GENS_WRITE, ADDR_GENS_SPACES };
#define ADDR_GENS_BITS     40   /* the GMMU drops a GPU address's bits past these (nvdrv.c): so do the generations */
#define ADDR_GENS_SHIFT    18   /* 256 KiB granules */
#define ADDR_GENS_SLOTS    8192 /* a power of two */
#define ADDR_GENS_READ_MAX 512  /* granules a read adds up at most (128 MiB): past them it tells nothing */

/* [a, a + size) of the space written: one more in the slot of each of its granules, or in all where the range has more
   granules than the table has slots */
void addr_gens_bump(int space, u64 a, u64 size);
/* every range of the space written (a change that no range describes) */
void addr_gens_bump_all(int space);
/* the sum of the slots of the granules of [a, a + size) and of all into *gen: 1, or 0 where the range has more than
   ADDR_GENS_READ_MAX granules (or none): then the sum tells nothing */
int addr_gens_read(int space, u64 a, u64 size, u32* gen);
#endif
