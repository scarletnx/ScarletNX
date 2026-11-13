/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/space.h"

int space_init(Space* s, u32 units) {
    *s = (Space){0};
    if (!units) return 0;
    s->cap = 16;
    s->run = malloc(s->cap * sizeof *s->run);
    if (!s->run) return 0;
    s->run[0] = (SpaceRun){0, units};
    s->nrun = 1;
    s->units = s->largest = units;
    return 1;
}
void space_destroy(Space* s) {
    free(s->run);
    *s = (Space){0};
}

static void largest_find(Space* s) {
    u32 most = 0;
    for (u32 i = 0; i < s->nrun; i++)
        if (s->run[i].n > most) most = s->run[i].n;
    s->largest = most;
}
/* room for one more run */
static int runs_room(Space* s) {
    if (s->nrun < s->cap) return 1;
    u32 cap = s->cap * 2;
    SpaceRun* grown = cap > s->cap ? realloc(s->run, (size_t)cap * sizeof *grown) : NULL;
    if (!grown) return 0;
    s->run = grown;
    s->cap = cap;
    return 1;
}
static void run_remove(Space* s, u32 i) {
    memmove(s->run + i, s->run + i + 1, (size_t)(s->nrun - i - 1) * sizeof *s->run);
    s->nrun--;
}
static void run_insert(Space* s, u32 i, SpaceRun r) { /* the room is there */
    memmove(s->run + i + 1, s->run + i, (size_t)(s->nrun - i) * sizeof *s->run);
    s->run[i] = r;
    s->nrun++;
}

int space_take(Space* s, u32 n, u32 align, u32* at) {
    if (!n || n > s->largest || !align || (align & (align - 1))) return 0;
    u32 best = ~0u, best_n = ~0u, best_at = 0;
    for (u32 i = 0; i < s->nrun; i++) {
        SpaceRun r = s->run[i];
        if (r.n < n || r.n >= best_n) continue;
        u32 a = (r.at + align - 1) & ~(align - 1);
        if (a < r.at || a - r.at > r.n - n) continue; /* past the end of the units, or of the run */
        best = i;
        best_n = r.n;
        best_at = a;
        if (r.n == n) break;
    }
    if (best == ~0u) return 0;
    SpaceRun r = s->run[best];
    u32 before = best_at - r.at, after = r.n - before - n;
    if (before && after) { /* the run becomes two */
        if (!runs_room(s)) return 0;
        s->run[best].n = before;
        run_insert(s, best + 1, (SpaceRun){best_at + n, after});
    } else if (before)
        s->run[best].n = before;
    else if (after)
        s->run[best] = (SpaceRun){best_at + n, after};
    else
        run_remove(s, best);
    s->used += n;
    if (r.n == s->largest) largest_find(s);
    *at = best_at;
    return 1;
}

void space_give(Space* s, u32 at, u32 n) {
    if (!n) return;
    /* the first run after the range */
    u32 lo = 0, hi = s->nrun;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (s->run[mid].at < at)
            lo = mid + 1;
        else
            hi = mid;
    }
    int joins_before = lo && s->run[lo - 1].at + s->run[lo - 1].n == at;
    int joins_after = lo < s->nrun && s->run[lo].at == at + n;
    u32 len;
    if (joins_before && joins_after) {
        len = s->run[lo - 1].n += n + s->run[lo].n;
        run_remove(s, lo);
    } else if (joins_before)
        len = s->run[lo - 1].n += n;
    else if (joins_after) {
        s->run[lo].at = at;
        len = s->run[lo].n += n;
    } else {
        /* without memory for one more run the units stay out of use: they are lost to this block, not handed out
           twice */
        if (!runs_room(s)) return;
        run_insert(s, lo, (SpaceRun){at, n});
        len = n;
    }
    s->used -= n;
    if (len > s->largest) s->largest = len;
}
