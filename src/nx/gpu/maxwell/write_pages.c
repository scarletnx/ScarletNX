/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/maxwell/write_pages.h"
#include <stdlib.h>

#define LEAF_PAGES 4096u
#define GROUP_PAGES 64u
struct WritePageLeaf {
    u64 max;
    u64 group_max[LEAF_PAGES / GROUP_PAGES];
    u64 gen[LEAF_PAGES];
};
/* Stamps are taken by one writer at a time (the caller's lock) and read without one: a leaf is published with a
   release store once made, and a page's stamp is stored before its group's and the leaf's newest, so a reader that
   finds the leaf's or the group's newest after since finds the page's too */
void write_pages_stamp(WritePages* t, u64 a, u64 size, u64 gen) {
    u64 p, e;
    if (!size) return;
    if (a + size - 1 < a) {
        __atomic_store_n(&t->overflow, gen, __ATOMIC_RELEASE);
        return;
    }
    for (p = a >> 12, e = (a + size - 1) >> 12; p <= e;) {
        WritePageLeaf** slot = &t->leaf[(p >> 12) & 0xFFFF];
        WritePageLeaf* l = p >> 28 ? NULL : __atomic_load_n(slot, __ATOMIC_ACQUIRE);
        if (!l && !(p >> 28) && (l = calloc(1, sizeof(WritePageLeaf)))) __atomic_store_n(slot, l, __ATOMIC_RELEASE);
        if (!l) {
            __atomic_store_n(&t->overflow, gen, __ATOMIC_RELEASE);
            return;
        }
        if (p == e) {
            __atomic_store_n(&l->gen[p & (LEAF_PAGES - 1)], gen, __ATOMIC_RELAXED);
            __atomic_store_n(&l->group_max[(p & (LEAF_PAGES - 1)) / GROUP_PAGES], gen, __ATOMIC_RELEASE);
            __atomic_store_n(&l->max, gen, __ATOMIC_RELEASE);
            return;
        }
        /* A range often spans many pages in the same leaf. Keep the leaf once, write each page, then publish each
           group's newest once and the leaf's newest once. Readers still acquire the summaries before the pages;
           the caller publishes the completed generation only after all its ranges have been stamped. */
        u64 end = (p | (LEAF_PAGES - 1)) < e ? p | (LEAF_PAGES - 1) : e;
        while (p <= end) {
            u64 group_end = (p | (GROUP_PAGES - 1)) < end ? p | (GROUP_PAGES - 1) : end;
            u64 group = (p & (LEAF_PAGES - 1)) / GROUP_PAGES;
            for (; p <= group_end; p++)
                __atomic_store_n(&l->gen[p & (LEAF_PAGES - 1)], gen, __ATOMIC_RELAXED);
            __atomic_store_n(&l->group_max[group], gen, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&l->max, gen, __ATOMIC_RELEASE);
    }
}
int write_pages_since(const WritePages* t, u64 since, u64 a, u64 size) {
    u64 p, e;
    if (__atomic_load_n(&t->overflow, __ATOMIC_ACQUIRE) > since) return 1;
    if (!size) return 0;
    e = a + size - 1 < a ? ~0ull >> 12 : (a + size - 1) >> 12; /* pages past 2^40 were only ever stamped as overflow */
    for (p = a >> 12; p <= e && !(p >> 28);) {
        u64 end = (p | (LEAF_PAGES - 1)) < e ? p | (LEAF_PAGES - 1) : e;
        const WritePageLeaf* l = __atomic_load_n(&t->leaf[p >> 12], __ATOMIC_ACQUIRE);
        if (l && __atomic_load_n(&l->max, __ATOMIC_ACQUIRE) > since)
            for (u64 group = p; group <= end;) {
                u64 group_end = (group | (GROUP_PAGES - 1)) < end ? group | (GROUP_PAGES - 1) : end;
                if (__atomic_load_n(&l->group_max[(group & (LEAF_PAGES - 1)) / GROUP_PAGES], __ATOMIC_ACQUIRE) > since)
                    for (u64 q = group; q <= group_end; q++)
                        if (__atomic_load_n(&l->gen[q & (LEAF_PAGES - 1)], __ATOMIC_RELAXED) > since) return 1;
                group = group_end + 1;
            }
        p = end + 1;
    }
    return 0;
}
