/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The caches' budget on a GPU sharing the system's memory (gpu/vk/uma_budget.h), with what a phone measured: 11122 MB
   of memory, the driver's budget 8897 MB, a game the low memory killer ended with 683 MB available and the caches
   never trimmed, as they trim past 90% of the budget (vk_mem_trim) */
#include <assert.h>
#include "nx/gpu/vk/uma_budget.h"
#define MB (1ull << 20)
static int trims(uint64_t usage, uint64_t budget) { return usage > budget / 100 * 90; }
int main(void) {
    const uint64_t driver = 8897 * MB, reserve = vk_uma_reserve(11388864ull << 10);
    assert(reserve == 1423608ull << 10); /* an eighth: 1390 MB */
    /* at the start, the caches may take what the system can give above the reserve */
    assert(vk_uma_budget(driver, 0, 5946 * MB, reserve) == 5946 * MB - reserve);
    /* 14 s in, 1326 MB in the caches and 2877 MB available: room left, no trim */
    assert(!trims(1326 * MB, vk_uma_budget(driver, 1326 * MB, 2877 * MB, reserve)));
    /* 21 s later, 683 MB available, below the reserve: the budget is what the caches hold, and they trim, where the
       driver's 8897 MB let them grow until the game was killed */
    assert(vk_uma_budget(driver, 3000 * MB, 683 * MB, reserve) == 3000 * MB);
    assert(trims(3000 * MB, vk_uma_budget(driver, 3000 * MB, 683 * MB, reserve)));
    assert(!trims(3000 * MB, driver));
    /* within 10% of the reserve's edge the caches trim too: 1700 MB in them, 1500 available */
    assert(trims(1700 * MB, vk_uma_budget(driver, 1700 * MB, 1500 * MB, reserve)));
    /* plenty left (a 16 GB device with 14 GB free): the driver's budget stands */
    assert(vk_uma_budget(driver, 1000 * MB, 14000 * MB, vk_uma_reserve(16384 * MB)) == driver);
    /* VMA's blocks: 32 MiB where the GPU's memory is the system's, 256 MiB on a GPU of its own (a strong PC's is as it
       was), within a smaller allocation cap either way */
    assert(vk_mem_block_size(1, 256 * MB) == 32 * MB && vk_mem_block_size(0, 256 * MB) == 256 * MB);
    assert(vk_mem_block_size(1, 16 * MB) == 16 * MB && vk_mem_block_size(0, 64 * MB) == 64 * MB);
    /* what trims gave back, with that phone's numbers: 1312 MB of blocks, and trims that freed 120 MB of textures
       and targets left them at 1312: held. A quarter back or more goes on; under 16 MB freed says nothing */
    assert(vk_mem_trim_held(1312 * MB, 120 * MB, 1312 * MB));
    assert(vk_mem_trim_held(1312 * MB, 120 * MB, 1290 * MB));  /* 22 MB back of 120 */
    assert(!vk_mem_trim_held(1312 * MB, 120 * MB, 1280 * MB)); /* 32 back, a quarter and more */
    assert(!vk_mem_trim_held(1312 * MB, 120 * MB, 1100 * MB));
    assert(!vk_mem_trim_held(1312 * MB, 15 * MB, 1312 * MB));
    assert(vk_mem_trim_held(1312 * MB, 120 * MB, 1400 * MB)); /* the blocks grew meanwhile: none back, held */
    return 0;
}
