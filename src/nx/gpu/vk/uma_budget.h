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

#endif
