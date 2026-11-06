/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_INDEXED_HANDLES_H
#define SNX_VK_INDEXED_HANDLES_H
#include <stdint.h>

/* A table's descriptor IDs stay in first-seen order. Slots hold ID + 1, so handle zero is an ordinary key and
   entries of kind 255 (the renderer's null descriptor) need no slot. The caller supplies a zeroed array of
   2 * capacity slots; capacity is a power of two, at most 32768. A miss reserves the next ID's slot when there is
   capacity, then the caller stores handles[used] and kinds[used] before its next lookup. */
static inline uint32_t vk_indexed_handle_find(uint16_t* slots, uint32_t capacity, const uint32_t* handles,
                                              const uint8_t* kinds, uint32_t used, uint32_t handle, uint8_t kind,
                                              uint32_t linear_limit) {
    if (used <= linear_limit) {
        for (uint32_t i = 0; i < used; i++)
            if (handles[i] == handle && kinds[i] == kind) return i;
        if (used == capacity) return used;
    }
    uint64_t h = ((uint64_t)handle | (uint64_t)kind << 32) * 0x9E3779B97F4A7C15ull;
    uint32_t mask = 2 * capacity - 1, at = (uint32_t)(h ^ (h >> 32)) & mask;
    while (slots[at]) {
        uint32_t id = slots[at] - 1;
        if (handles[id] == handle && kinds[id] == kind) return id;
        at = (at + 1) & mask;
    }
    if (used < capacity) slots[at] = (uint16_t)(used + 1);
    return used;
}
#endif
