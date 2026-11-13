/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <stdlib.h>
#include "nx/gpu/guest_textures.h"
#include "common/platform/thread.h"
#include "nx/runtime/platform/log.h"

/* The game's threads make and end textures, and the GPU thread reads them: the lock puts them in order */
static struct {
    PMutex lock;
    GpuTexture* v; /* open addressed by gva, a power of two of slots: one with no refs is free */
    u32 n, cap;    /* the slots taken, and all of them */
    u64 added;
    /* the addresses whose last object ended, until gpu_textures_ended takes them */
    u64 ended[1024];
    u32 nended;
} g_guest_textures = {.lock = PMUTEX_INIT};

/* the slot the search for gva starts at, in a table of cap slots */
static u32 slot_home(u64 gva, u32 cap) { return (u32)((gva * 0x9e3779b97f4a7c15ull) >> 32) & (cap - 1); }

/* gva's slot in v, else the free slot its search ends at: a table always has a free slot */
static u32 slot_find(const GpuTexture* v, u32 cap, u64 gva) {
    u32 i = slot_home(gva, cap);
    while (v[i].refs && v[i].gva != gva) i = (i + 1) & (cap - 1);
    return i;
}

/* slot i of v freed. The entries after it up to the next free slot move back where their search would have passed
   i, so each one stays in reach from the slot its search starts at */
static void slot_free(GpuTexture* v, u32 cap, u32 i) {
    const u32 mask = cap - 1;
    for (u32 j = (i + 1) & mask; v[j].refs; j = (j + 1) & mask) {
        const u32 home = slot_home(v[j].gva, cap);
        if (((j - home) & mask) < ((j - i) & mask)) continue; /* its search starts after i: it stays */
        v[i] = v[j];
        i = j;
    }
    v[i] = (GpuTexture){0};
}

/* twice the slots, each entry in its place among them */
static void table_grow(void) {
    const u32 cap = g_guest_textures.cap ? g_guest_textures.cap * 2 : 1024;
    GpuTexture* v = cap > g_guest_textures.cap ? calloc(cap, sizeof *v) : NULL;
    if (!v) fatal("Out of memory for the table of %u texture objects", cap);
    for (u32 i = 0; i < g_guest_textures.cap; i++) {
        const GpuTexture* t = &g_guest_textures.v[i];
        if (t->refs) v[slot_find(v, cap, t->gva)] = *t;
    }
    free(g_guest_textures.v);
    g_guest_textures.v = v;
    g_guest_textures.cap = cap;
}

void gpu_texture_add(u64 gva, u64 size) {
    if (!gva) return;
    pm_lock(&g_guest_textures.lock);
    g_guest_textures.added++;
    /* three slots in four at most, so a search is short */
    if (((u64)g_guest_textures.n + 1) * 4 > (u64)g_guest_textures.cap * 3) table_grow();
    GpuTexture* t = &g_guest_textures.v[slot_find(g_guest_textures.v, g_guest_textures.cap, gva)];
    if (!t->refs) {
        *t = (GpuTexture){.gva = gva};
        g_guest_textures.n++;
    }
    if (size > t->size) t->size = size;
    t->refs++;
    pm_unlock(&g_guest_textures.lock);
}

void gpu_texture_remove(u64 gva) {
    if (!gva) return;
    pm_lock(&g_guest_textures.lock);
    if (g_guest_textures.cap) {
        const u32 i = slot_find(g_guest_textures.v, g_guest_textures.cap, gva);
        if (g_guest_textures.v[i].refs && !--g_guest_textures.v[i].refs) {
            slot_free(g_guest_textures.v, g_guest_textures.cap, i);
            g_guest_textures.n--;
            if (g_guest_textures.nended < sizeof g_guest_textures.ended / sizeof *g_guest_textures.ended)
                g_guest_textures.ended[g_guest_textures.nended++] = gva;
        }
    }
    pm_unlock(&g_guest_textures.lock);
}

u32 gpu_textures(GpuTexture* out, u32 cap) {
    pm_lock(&g_guest_textures.lock);
    const u32 n = g_guest_textures.n;
    for (u32 i = 0, k = 0; i < g_guest_textures.cap && k < cap; i++)
        if (g_guest_textures.v[i].refs) out[k++] = g_guest_textures.v[i];
    pm_unlock(&g_guest_textures.lock);
    return n;
}

int gpu_texture_live(u64 gva) {
    int live = 0;
    pm_lock(&g_guest_textures.lock);
    if (g_guest_textures.cap)
        live = g_guest_textures.v[slot_find(g_guest_textures.v, g_guest_textures.cap, gva)].refs != 0;
    pm_unlock(&g_guest_textures.lock);
    return live;
}

u32 gpu_textures_ended(u64* out, u32 cap) {
    pm_lock(&g_guest_textures.lock);
    const u32 n = g_guest_textures.nended < cap ? g_guest_textures.nended : cap;
    for (u32 i = 0; i < n; i++) out[i] = g_guest_textures.ended[g_guest_textures.nended - n + i];
    g_guest_textures.nended -= n;
    pm_unlock(&g_guest_textures.lock);
    return n;
}

u64 gpu_textures_added(void) {
    pm_lock(&g_guest_textures.lock);
    const u64 added = g_guest_textures.added;
    pm_unlock(&g_guest_textures.lock);
    return added;
}
