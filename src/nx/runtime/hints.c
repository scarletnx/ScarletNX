/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Code-address hints: a deduplicated ordered set, saved without keeping guest threads behind file I/O. */
#include "nx/runtime/rtapi.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- hints: a set of addresses, and the order they were found in for the file ---- */

/* One block: cap slots of the open-addressing set (0 is an empty slot, and never a hint), then the found order, which
   holds at most cap / 2 since the set is kept at most half full. Under g_hints.lock */
typedef struct {
    u64* slot;
    u64 cap, n;
    int dirty; /* added to since the file was last written */
    PMutex lock, save_lock; /* lookup state, then separately serialized file publication */
    char path[1400];
} Hints;
static Hints g_hints = {.lock = PMUTEX_INIT, .save_lock = PMUTEX_INIT};

static u64* hints_order(void) { return g_hints.slot + g_hints.cap; }
static u64 hint_hash(u64 pc, u64 cap) {
    unsigned bits = 0;
    while (((u64)1 << bits) < cap) bits++;
    return ((pc >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - bits); /* Fibonacci hashing: cap is a power of two */
}
/* 1 when pc was put in, 0 when it was there already */
static int set_put_nl(u64* slot, u64 cap, u64 pc) {
    u64 i = hint_hash(pc, cap);
    while (slot[i]) {
        if (slot[i] == pc) return 0;
        i = (i + 1) & (cap - 1);
    }
    slot[i] = pc;
    return 1;
}
static int grow_nl(void) {
    u64 cap = g_hints.cap ? g_hints.cap * 2 : 1024, i;
    u64 *block, *order;
    if (cap > SIZE_MAX / sizeof(u64) / 2) return -1;
    block = (u64*)calloc((size_t)(cap + cap / 2), sizeof(u64));
    if (!block) return -1;
    order = block + cap;
    for (i = 0; i < g_hints.n; i++) {
        order[i] = hints_order()[i];
        set_put_nl(block, cap, order[i]);
    }
    free(g_hints.slot);
    g_hints.slot = block;
    g_hints.cap = cap;
    return 0;
}
/* 1 when pc is new */
static int hint_add_nl(u64 pc) {
    if (!pc) return 0;
    if ((g_hints.n + 1) * 2 > g_hints.cap && grow_nl()) {
        WARN_LIMIT(1, "hints: out of memory, the hints file misses some addresses\n");
        return 0;
    }
    if (!set_put_nl(g_hints.slot, g_hints.cap, pc)) return 0;
    hints_order()[g_hints.n++] = pc;
    return 1;
}

void hints_init(void) { snprintf(g_hints.path, sizeof g_hints.path, "%s%s.hints", g_exedir, g_exename); }

void load_hints(void) {
    FILE* f = fopen(g_hints.path, "r");
    char line[64];
    if (!f) return;
    pm_lock(&g_hints.lock);
    while (fgets(line, sizeof line, f)) {
        char* end;
        unsigned long long pc = strtoull(line, &end, 16);
        if (end != line) hint_add_nl((u64)pc);
    }
    pm_unlock(&g_hints.lock);
    fclose(f);
}

/* the JIT's warm start (guest_jit.c) */
u64* hints_copy(size_t* count) {
    u64* copy;
    pm_lock(&g_hints.lock);
    copy = g_hints.n ? malloc(8 * (size_t)g_hints.n) : NULL;
    *count = copy ? (size_t)g_hints.n : 0;
    if (copy) memcpy(copy, hints_order(), 8 * (size_t)g_hints.n);
    pm_unlock(&g_hints.lock);
    return copy;
}

/* Serialize savers, but hold the lookup lock only while copying a snapshot. A slow file write must not stop
   guest threads recording or resolving misses. The set only grows, so its count identifies the saved snapshot. */
void save_hints(void) {
    char tmp[sizeof g_hints.path + 8];
    if (!g_hints.path[0]) return;
    pm_lock(&g_hints.save_lock);
    pm_lock(&g_hints.lock);
    size_t count = (size_t)g_hints.n;
    if (!g_hints.dirty) {
        pm_unlock(&g_hints.lock);
        pm_unlock(&g_hints.save_lock);
        return;
    }
    u64* snapshot = count ? malloc(count * sizeof(*snapshot)) : NULL;
    if (count && !snapshot) {
        pm_unlock(&g_hints.lock);
        pm_unlock(&g_hints.save_lock);
        WARN_LIMIT(1, "hints: no memory for a save snapshot\n");
        return;
    }
    if (count) memcpy(snapshot, hints_order(), count * sizeof(*snapshot));
    pm_unlock(&g_hints.lock);
    snprintf(tmp, sizeof tmp, "%s.tmp", g_hints.path);
    FILE* f = fopen(tmp, "w");
    int failed = !f;
    for (size_t i = 0; f && i < count; i++)
        if (fprintf(f, "%llx\n", (unsigned long long)snapshot[i]) < 0) failed = 1;
    if (f && fclose(f)) failed = 1;
    if (!failed && plat_replace_file(tmp, g_hints.path)) failed = 1;
    free(snapshot);
    if (failed) {
        remove(tmp);
        WARN_LIMIT(1, "hints: could not write %s\n", g_hints.path);
    } else {
        pm_lock(&g_hints.lock);
        /* A hint added during I/O still needs a later save. Never clear its dirty bit with an older snapshot. */
        if (g_hints.n == count) g_hints.dirty = 0;
        pm_unlock(&g_hints.lock);
    }
    pm_unlock(&g_hints.save_lock);
}

/* 1 when pc is a new hint */
int hints_add(u64 pc) {
    int added;
    pm_lock(&g_hints.lock);
    added = hint_add_nl(pc);
    if (added) g_hints.dirty = 1;
    pm_unlock(&g_hints.lock);
    return added;
}
