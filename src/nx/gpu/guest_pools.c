/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <stdlib.h>
#include "nx/gpu/guest_pools.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

static struct {
    PMutex lock;
    GpuPool* v;
    u32 n, cap;
} g_guest_pools = {.lock = PMUTEX_INIT};

void gpu_pool_add(u64 gva, u64 size, int virtual) {
    if (!gva || !size) return;
    pm_lock(&g_guest_pools.lock);
    if (g_guest_pools.n == g_guest_pools.cap) {
        u32 cap = g_guest_pools.cap ? g_guest_pools.cap * 2 : 256;
        GpuPool* grown = cap > g_guest_pools.cap ? realloc(g_guest_pools.v, (size_t)cap * sizeof *grown) : NULL;
        if (!grown) fatal("Out of memory for the list of %u memory pools", cap);
        g_guest_pools.v = grown;
        g_guest_pools.cap = cap;
    }
    g_guest_pools.v[g_guest_pools.n++] = (GpuPool){gva, size, virtual};
    pm_unlock(&g_guest_pools.lock);
}

void gpu_pool_remove(u64 gva) {
    pm_lock(&g_guest_pools.lock);
    for (u32 i = 0; i < g_guest_pools.n; i++)
        if (g_guest_pools.v[i].gva == gva) {
            g_guest_pools.v[i] = g_guest_pools.v[--g_guest_pools.n];
            break;
        }
    pm_unlock(&g_guest_pools.lock);
}

u32 gpu_pools(GpuPool* out, u32 cap) {
    pm_lock(&g_guest_pools.lock);
    u32 n = g_guest_pools.n;
    for (u32 i = 0; i < n && i < cap; i++) out[i] = g_guest_pools.v[i];
    pm_unlock(&g_guest_pools.lock);
    return n;
}
