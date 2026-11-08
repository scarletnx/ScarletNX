/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_UMA_BUDGET_H
#define SNX_VK_UMA_BUDGET_H
#include <stdint.h>

/* The budget of the caches on a GPU that shares the system's memory, where the system ends a process that runs it
   short instead of paging it out (Android's low memory killer). The driver's budget there is a share of the whole
   memory, blind to what the game and every other process hold: an Adreno 830 budgets 8897 of an 11 GB phone's MB, and
   a game whose render targets and textures grew past 3 GB in 35 s was killed with 683 MB left, the caches never
   trimmed. Here the caches may grow by what the system can still give above a reserve, and no further than the
   driver's budget: past it they trim (vk_mem_trim), and what the game itself takes is room they lose */
static inline uint64_t vk_uma_budget(uint64_t driver, uint64_t usage, uint64_t available, uint64_t reserve) {
    uint64_t budget = usage + (available > reserve ? available - reserve : 0);
    return budget < driver ? budget : driver;
}
/* the system's memory kept free of the caches: an eighth of it, 1390 of that phone's 11122 MB, where the low memory
   killer ended background apps from 1925 MB available and the game at 683 */
static inline uint64_t vk_uma_reserve(uint64_t system) { return system / 8; }

/* VMA's blocks: 256 MiB, and 32 MiB where the GPU's memory is the system's (uma). A block's room nothing uses, and the
   one VMA keeps empty of each memory type, are memory there that every other app loses: that phone's 256 MiB blocks
   held 1312 MB with 263 MB allocated in them in a game's world, where the low memory killer ended the game. A resource
   past half a block gets memory of its own, which goes back with it. Within cap, the allocations' */
#define SNX_VK_BLOCK     (256ull << 20)
#define SNX_VK_UMA_BLOCK (32ull << 20)
static inline uint64_t vk_mem_block_size(int uma, uint64_t cap) {
    uint64_t block = uma ? SNX_VK_UMA_BLOCK : SNX_VK_BLOCK;
    return cap < block ? cap : block;
}

/* What trims give the system back: only a block they empty, the rest staying in the blocks as room the next allocations
   take. Trims that freed freed bytes (16 MiB or more), with VMA's blocks at blocks_then before them and blocks_now once
   what they freed was released, and gave back less than a quarter of it only make the next frames load it again, the
   memory the budget asks for still not the system's: the caches stop trimming (vk_mem_trim) */
#define SNX_VK_GIVEN_LEAST (16ull << 20)
static inline int vk_mem_trim_held(uint64_t blocks_then, uint64_t freed, uint64_t blocks_now) {
    uint64_t back = blocks_then > blocks_now ? blocks_then - blocks_now : 0;
    return freed >= SNX_VK_GIVEN_LEAST && back < freed / 4;
}

#endif
