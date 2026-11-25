/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Generations by address (addr_gens.h). A bump is a release and a read an acquire: a reader that sees a bump sees
   what the writer did before it (a result counted, a page stamped) */
#include <stdatomic.h>
#include "nx/gpu/vk/addr_gens.h"

static struct {
    _Atomic u32 slot[ADDR_GENS_SLOTS];
    _Atomic u32 all;
} g_addr_gens[ADDR_GENS_SPACES];

/* a granule's slot, by the bits of its address that the GMMU translates (ADDR_GENS_BITS) */
static u32 slot_of(u64 granule) {
    granule &= (1ull << (ADDR_GENS_BITS - ADDR_GENS_SHIFT)) - 1;
    return (u32)((granule * 0x9E3779B97F4A7C15ull) >> 51) & (ADDR_GENS_SLOTS - 1);
}
/* the granules of [a, a + size), first and last, up to the top of the address space: 0 for none */
static u64 granules(u64 a, u64 size, u64* first) {
    if (!size) return 0;
    const u64 end = a + size - 1 < a ? ~0ull : a + size - 1;
    *first = a >> ADDR_GENS_SHIFT;
    return (end >> ADDR_GENS_SHIFT) - *first + 1;
}
void addr_gens_bump_all(int space) { atomic_fetch_add_explicit(&g_addr_gens[space].all, 1, memory_order_release); }
void addr_gens_bump(int space, u64 a, u64 size) {
    u64 first, n = granules(a, size, &first);
    if (n > ADDR_GENS_SLOTS) {
        addr_gens_bump_all(space);
        return;
    }
    _Atomic u32* slot = g_addr_gens[space].slot;
    if (space == ADDR_GENS_GPU) /* its writers hold pend_lock: a load and a store, with no locked add */
        for (u64 g = first; n--; g++) {
            _Atomic u32* s = &slot[slot_of(g)];
            atomic_store_explicit(s, atomic_load_explicit(s, memory_order_relaxed) + 1, memory_order_release);
        }
    else
        for (u64 g = first; n--; g++) atomic_fetch_add_explicit(&slot[slot_of(g)], 1, memory_order_release);
}
int addr_gens_read(int space, u64 a, u64 size, u32* gen) {
    u64 first, n = granules(a, size, &first);
    if (!n || n > ADDR_GENS_READ_MAX) return 0;
    u32 sum = atomic_load_explicit(&g_addr_gens[space].all, memory_order_acquire);
    for (u64 g = first; n--; g++) sum += atomic_load_explicit(&g_addr_gens[space].slot[slot_of(g)], memory_order_acquire);
    *gen = sum;
    return 1;
}
