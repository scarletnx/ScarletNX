/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/loader/hook_routes.h"
#include "common/utilities/ascii.h"
#include "common/utilities/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/loader/hook_routes.inc"

/* snx_hook_route_add's keys, after the generated list */
static struct {
    char** keys;
    size_t count;
} g_added;

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

size_t snx_hook_route_count(void) { return HOOK_ROUTE_COUNT + g_added.count; }

const char* snx_hook_route_name(size_t index) {
    if (index < HOOK_ROUTE_COUNT) return hook_route_names[index];
    index -= HOOK_ROUTE_COUNT;
    return index < g_added.count ? g_added.keys[index] : NULL;
}

void snx_hook_route_add(const char* key) {
    if (snx_hook_route_listed(key)) return;
    g_added.keys = (char**)snx_realloc(g_added.keys, (g_added.count + 1) * sizeof(*g_added.keys));
    g_added.keys[g_added.count++] = snx_strdup(key);
}

/* snx_hook_route_add_build's pairs */
static struct {
    char** hex;
    SnxU64* offset;
    size_t count;
} g_builds;

int snx_hook_build_hex(const char* hex) {
    size_t n = 0;
    for (; snx_is_hex(hex[n]); ++n)
        if (n == 64) return 0;
    return n && !(n % 2) && !hex[n];
}

void snx_hook_route_add_build(const char* build_id, SnxU64 offset) {
    size_t i;
    if (!snx_hook_build_hex(build_id)) return;
    for (i = 0; i < g_builds.count; ++i)
        if (g_builds.offset[i] == offset && !strcmp(g_builds.hex[i], build_id)) return;
    g_builds.hex = (char**)snx_realloc(g_builds.hex, (g_builds.count + 1) * sizeof(*g_builds.hex));
    g_builds.offset = (SnxU64*)snx_realloc(g_builds.offset, (g_builds.count + 1) * sizeof(*g_builds.offset));
    g_builds.hex[g_builds.count] = snx_strdup(build_id);
    g_builds.offset[g_builds.count++] = offset;
}

size_t snx_hook_route_build_count(void) { return g_builds.count; }

const char* snx_hook_route_build_entry(size_t i, SnxU64* offset) {
    if (i >= g_builds.count) return NULL;
    *offset = g_builds.offset[i];
    return g_builds.hex[i];
}

int snx_hook_route_listed(const char* key) {
    size_t i;
    for (i = 0; i < snx_hook_route_count(); ++i)
        if (!strcmp(snx_hook_route_name(i), key)) return 1;
    return 0;
}

int snx_hook_route_build_at(size_t i, const unsigned char build_id[32], SnxU64* offset) {
    const char* hex;
    size_t k;
    if (i >= g_builds.count) return 0;
    hex = g_builds.hex[i];
    for (k = 0; hex[2 * k]; ++k)
        if (build_id[k] != (snx_hex_value(hex[2 * k]) << 4 | snx_hex_value(hex[2 * k + 1]))) return 0;
    *offset = g_builds.offset[i];
    return 1;
}

int snx_hook_build_key(const unsigned char build_id[32], SnxU64 offset, char* out, size_t size) {
    static const char digits[] = "0123456789abcdef";
    char hex[65];
    size_t n = 32, i;
    while (n > 1 && !build_id[n - 1]) n--; /* the ID's ending zero bytes */
    for (i = 0; i < n; ++i) hex[2 * i] = digits[build_id[i] >> 4], hex[2 * i + 1] = digits[build_id[i] & 15];
    hex[2 * n] = 0;
    return snprintf(out, size, "%s:%llx", hex, (unsigned long long)offset) < (int)size;
}

SnxU64 snx_hook_route_hash(const char* name) { return snx_fnv1a(SNX_FNV_BASIS, name, strlen(name)); }

SnxU64 snx_hook_route_list_hash(void) {
    SnxU64 hash = SNX_FNV_BASIS;
    for (size_t i = 0; i < snx_hook_route_count(); ++i)
        hash = snx_fnv_step(hash, snx_hook_route_hash(snx_hook_route_name(i)));
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
