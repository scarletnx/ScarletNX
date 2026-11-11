/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The caches' budget on a GPU sharing the system's memory (gpu/vk/uma_budget.h), with what a phone measured: 11122 MB
   of memory, the driver's budget 8897 MB, a game the low memory killer ended with 683 MB available and the caches
   never trimmed, as they trim past 90% of the budget (vk_mem_trim) */
#include <assert.h>
#include "nx/gpu/vk/uma_budget.h"
#define MB (1ull << 20)
static int trims(uint64_t usage, uint64_t budget) { return usage > budget / 100 * 90; }
/* a frame's rings (vk_uma_ring_cap): four allocations of 256 MiB at most, an eighth of the budget where that is less,
   and two chunks of 16 MiB at least. The phone that ran out at an 8192x8192 texture had 1037 MB available as the upload
   began and keeps 463 back: with 300 MB in the caches, say, a budget of 874 MB, and frames of 109 MiB rather than
   1024 */
static void ring_caps(void) {
    const uint64_t caps = 4 * 256 * MB, floor = 32 * MB,
                   phone = vk_uma_budget(1365 * MB, 300 * MB, 1037 * MB, 463 * MB);
    assert(phone == 874 * MB && vk_uma_ring_cap(caps, phone, floor) == phone / 8);
    /* a budget of 8 GB or more leaves the four allocations, and one squeezed to 100 MB keeps the two chunks */
    assert(vk_uma_ring_cap(caps, 8192 * MB, floor) == caps && vk_uma_ring_cap(caps, 31640 * MB, floor) == caps);
    assert(vk_uma_ring_cap(caps, 100 * MB, floor) == floor);
}
int main(void) {
    ring_caps();
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
    return 0;
}
