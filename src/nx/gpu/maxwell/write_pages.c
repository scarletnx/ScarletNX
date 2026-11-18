/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/maxwell/write_pages.h"
#include <stdlib.h>
#include <stdatomic.h>

#define LEAF_PAGES 4096u
#define GROUP_PAGES 64u
/* A range that covers a whole group of pages stamps the group once (group_all), not each of its pages: a page's stamp
   is the newer of its own and its group's. One game's programs bind 16 MiB windows hundreds of times a present, and a
   store a page for each made the stamps a seventh of its GPU thread */
struct WritePageLeaf {
    _Atomic u64 max;
    _Atomic u64 group_max[LEAF_PAGES / GROUP_PAGES];
    _Atomic u64 group_all[LEAF_PAGES / GROUP_PAGES]; /* the newest stamp given to all of the group's pages at once */
    _Atomic u64 gen[LEAF_PAGES];
};
/* Stamps are taken by one writer at a time (the caller's lock) and read without one: a leaf is published with a
   release store once made, and a page's stamp, or the one its whole group takes for it, is stored before the group's
   and the leaf's newest, so a reader that finds the leaf's or the group's newest after since finds the page's too */
void write_pages_stamp(WritePages* t, u64 a, u64 size, u64 gen) {
    u64 p, e;
    if (!size) return;
    if (a + size - 1 < a) {
        atomic_store_explicit(&t->overflow, gen, memory_order_release);
        return;
    }
    for (p = a >> 12, e = (a + size - 1) >> 12; p <= e;) {
        WritePageLeaf* _Atomic* slot = &t->leaf[(p >> 12) & 0xFFFF];
        WritePageLeaf* l = p >> 28 ? NULL : atomic_load_explicit(slot, memory_order_acquire);
        if (!l && !(p >> 28) && (l = calloc(1, sizeof(WritePageLeaf))))
            atomic_store_explicit(slot, l, memory_order_release);
        if (!l) {
            atomic_store_explicit(&t->overflow, gen, memory_order_release);
            return;
        }
        if (p == e) {
            atomic_store_explicit(&l->gen[p & (LEAF_PAGES - 1)], gen, memory_order_relaxed);
            atomic_store_explicit(&l->group_max[(p & (LEAF_PAGES - 1)) / GROUP_PAGES], gen, memory_order_release);
            atomic_store_explicit(&l->max, gen, memory_order_release);
            return;
        }
        /* A range often spans many pages in the same leaf. Keep the leaf once, write each page, then publish each
           group's newest once and the leaf's newest once. Readers still acquire the summaries before the pages;
           the caller publishes the completed generation only after all its ranges have been stamped. */
        u64 end = (p | (LEAF_PAGES - 1)) < e ? p | (LEAF_PAGES - 1) : e;
        while (p <= end) {
            u64 group_end = (p | (GROUP_PAGES - 1)) < end ? p | (GROUP_PAGES - 1) : end;
            u64 group = (p & (LEAF_PAGES - 1)) / GROUP_PAGES;
            if (!(p & (GROUP_PAGES - 1)) && group_end - p == GROUP_PAGES - 1) { /* the whole group: one stamp */
                atomic_store_explicit(&l->group_all[group], gen, memory_order_relaxed);
                p = group_end + 1;
            } else
                for (; p <= group_end; p++)
                    atomic_store_explicit(&l->gen[p & (LEAF_PAGES - 1)], gen, memory_order_relaxed);
            atomic_store_explicit(&l->group_max[group], gen, memory_order_release);
        }
        atomic_store_explicit(&l->max, gen, memory_order_release);
    }
}
int write_pages_since(const WritePages* t, u64 since, u64 a, u64 size) {
    u64 p, e;
    if (atomic_load_explicit(&t->overflow, memory_order_acquire) > since) return 1;
    if (!size) return 0;
    e = a + size - 1 < a ? ~0ull >> 12 : (a + size - 1) >> 12; /* pages past 2^40 were only ever stamped as overflow */
    for (p = a >> 12; p <= e && !(p >> 28);) {
        u64 end = (p | (LEAF_PAGES - 1)) < e ? p | (LEAF_PAGES - 1) : e;
        const WritePageLeaf* l = atomic_load_explicit(&t->leaf[p >> 12], memory_order_acquire);
        if (l && atomic_load_explicit(&l->max, memory_order_acquire) > since)
            for (u64 group = p; group <= end;) {
                u64 group_end = (group | (GROUP_PAGES - 1)) < end ? group | (GROUP_PAGES - 1) : end;
                const u64 g = (group & (LEAF_PAGES - 1)) / GROUP_PAGES;
                if (atomic_load_explicit(&l->group_max[g], memory_order_acquire) > since) {
                    if (atomic_load_explicit(&l->group_all[g], memory_order_relaxed) > since) return 1;
                    for (u64 q = group; q <= group_end; q++)
                        if (atomic_load_explicit(&l->gen[q & (LEAF_PAGES - 1)], memory_order_relaxed) > since) return 1;
                }
                group = group_end + 1;
            }
        p = end + 1;
    }
    return 0;
}
/* the pages [first, past) of a run, as bytes of [a, a + size) */
static void run_out(u64 a, u64 size, u64 first, u64 past, void (*fn)(void*, u64, u64), void* ctx) {
    u64 lo = first << 12 > a ? first << 12 : a, hi = past << 12 < a + size ? past << 12 : a + size;
    fn(ctx, lo, hi - lo);
}
void write_pages_runs(const WritePages* t, u64 since, u64 a, u64 size, void (*fn)(void* ctx, u64 at, u64 n),
                      void* ctx) {
    u64 p, e, first = 0;
    int in = 0;
    if (!size) return;
    if (atomic_load_explicit(&t->overflow, memory_order_acquire) > since || a + size - 1 < a) {
        fn(ctx, a, size);
        return;
    }
    e = (a + size - 1) >> 12;
    /* the leaves and groups nothing wrote since end a run where they start, as a clean page would */
    for (p = a >> 12; p <= e && !(p >> 28);) {
        u64 end = (p | (LEAF_PAGES - 1)) < e ? p | (LEAF_PAGES - 1) : e;
        const WritePageLeaf* l = atomic_load_explicit(&t->leaf[p >> 12], memory_order_acquire);
        if (!l || atomic_load_explicit(&l->max, memory_order_acquire) <= since) {
            if (in) run_out(a, size, first, p, fn, ctx);
            in = 0;
            p = end + 1;
            continue;
        }
        for (u64 group = p; group <= end;) {
            u64 group_end = (group | (GROUP_PAGES - 1)) < end ? group | (GROUP_PAGES - 1) : end;
            if (atomic_load_explicit(&l->group_max[(group & (LEAF_PAGES - 1)) / GROUP_PAGES], memory_order_acquire) <=
                since) {
                if (in) run_out(a, size, first, group, fn, ctx);
                in = 0;
            } else {
                const int all = atomic_load_explicit(&l->group_all[(group & (LEAF_PAGES - 1)) / GROUP_PAGES],
                                                     memory_order_relaxed) > since;
                for (u64 q = group; q <= group_end; q++) {
                    int w = all || atomic_load_explicit(&l->gen[q & (LEAF_PAGES - 1)], memory_order_relaxed) > since;
                    if (w && !in) first = q;
                    if (!w && in) run_out(a, size, first, q, fn, ctx);
                    in = w;
                }
            }
            group = group_end + 1;
        }
        p = end + 1;
    }
    if (in) run_out(a, size, first, p, fn, ctx);
}
u64 write_pages_newest(const WritePages* t, u64 a, u64 size) {
    u64 newest = atomic_load_explicit(&t->overflow, memory_order_acquire);
    if (!size || a + size - 1 < a) return newest;
    for (u64 p = a >> 12, e = (a + size - 1) >> 12; p <= e && !(p >> 28); p++) {
        const WritePageLeaf* l = atomic_load_explicit(&t->leaf[p >> 12], memory_order_acquire);
        if (!l || atomic_load_explicit(&l->max, memory_order_acquire) <= newest) {
            p |= LEAF_PAGES - 1; /* nothing newer in this leaf: the next one */
            continue;
        }
        u64 g = atomic_load_explicit(&l->gen[p & (LEAF_PAGES - 1)], memory_order_relaxed);
        if (g > newest) newest = g;
    }
    return newest;
}
