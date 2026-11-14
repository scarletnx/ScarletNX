/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/loader/hook_routes.h"
#include "common/utilities/ascii.h"
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
    for (size_t i = 0; i < HOOK_ROUTE_PATTERNS; ++i)
        if (snx_hook_glob(hook_route_names[hook_route_patterns[i]], name)) return 1;
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

int snx_hook_key_at(const char* name, const unsigned char build_id[32], SnxU64* offset) {
    size_t n = 0, i;
    SnxU64 at = 0;
    for (; snx_is_hex(name[n]); ++n)
        if (n == 64) return 0;
    if (!n || n % 2 || name[n] != ':') return 0;
    for (i = 0; i < 32; ++i) {
        int byte = i < n / 2 ? snx_hex_value(name[2 * i]) << 4 | snx_hex_value(name[2 * i + 1]) : 0;
        if (build_id[i] != byte) return 0;
    }
    for (i = n + 1; snx_is_hex(name[i]); ++i) {
        if (at >> 60) return 0;
        at = at << 4 | (SnxU64)snx_hex_value(name[i]);
    }
    if (i == n + 1 || name[i]) return 0;
    *offset = at;
    return 1;
}
