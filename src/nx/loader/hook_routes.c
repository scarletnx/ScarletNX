/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/loader/hook_routes.h"
#include "common/utilities/bits.h"
#include <string.h>
#include "nx/loader/hook_routes.inc"

int snx_hook_routed(const char* name) {
    size_t lo = 0, hi = HOOK_ROUTE_COUNT;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        int order = strcmp(hook_route_names[middle], name);
        if (!order) return 1;
        if (order < 0)
            lo = middle + 1;
        else
            hi = middle;
    }
    return 0;
}

size_t snx_hook_route_count(void) { return HOOK_ROUTE_COUNT; }

const char* snx_hook_route_name(size_t index) { return index < HOOK_ROUTE_COUNT ? hook_route_names[index] : NULL; }

SnxU64 snx_hook_route_hash(const char* name) { return snx_fnv1a(SNX_FNV_BASIS, name, strlen(name)); }

SnxU64 snx_hook_route_list_hash(void) {
    SnxU64 hash = SNX_FNV_BASIS;
    for (size_t i = 0; i < HOOK_ROUTE_COUNT; ++i) hash = snx_fnv_step(hash, snx_hook_route_hash(hook_route_names[i]));
    return hash;
}
