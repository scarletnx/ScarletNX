/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <stdlib.h>
#include "nx/gpu/vk/pending_pages.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"

PendPages g_pend_any;
#define LEAF_PAGES  4096u
#define TABLE_PAGES ((u64)LEAF_PAGES << 16) /* the pages a table's leaves cover: 2^40 bytes, all of the GPU's */
struct PendLeaf {
    u32 count[LEAF_PAGES];      /* the results covering each page */
    u16 group[LEAF_PAGES / 64]; /* each group's pages with a count */
    u32 pages;                  /* the leaf's pages with a count */
};
/* the tables that count guest pages, each listed when it first gains one, for pend_remap to redo their GPU pages; and
   SNX_ALIAS_CHECK, which has pend_hit answer the old way too, translating the GPU range, and warn when only that way
   finds a result (-1 until read) */
static struct {
    PendPages* table[8];
    u32 n;
    int check;
} g_pending_pages = {{0}, 0, -1};

/* page p's count, 0 when no leaf holds it */
static u32 leaf_count(PendLeaf* const* leaf, u64 p) {
    const PendLeaf* l = leaf[p / LEAF_PAGES];
    return l ? l->count[p % LEAF_PAGES] : 0;
}
/* page p's count becomes v, and the leaf's counts of pages with one follow */
static void leaf_set(PendLeaf** leaf, u64 p, u32 v) {
    PendLeaf* l = leaf[p / LEAF_PAGES];
    u32 i = (u32)(p % LEAF_PAGES), before;
    if (!l) {
        if (!v) return;
        if (!(l = leaf[p / LEAF_PAGES] = calloc(1, sizeof *l))) fatal("vulkan: out of memory for pending result pages");
    }
    before = l->count[i];
    l->count[i] = v;
    if (!before != !v) {
        int d = v ? 1 : -1;
        l->group[i / 64] = (u16)(l->group[i / 64] + d);
        l->pages = (u32)((i64)l->pages + d);
    }
}
/* pages [first, last], below TABLE_PAGES, gain delta each */
static void leaves_add(PendLeaf** leaf, u64 first, u64 last, int delta) {
    for (u64 p = first; p <= last; p++) leaf_set(leaf, p, (u32)((i64)leaf_count(leaf, p) + delta));
}
/* whether a page of [first, last], below TABLE_PAGES, has a count, skipping leaves and groups that have none */
static int leaves_hit(PendLeaf* const* leaf, u64 first, u64 last) {
    for (u64 p = first; p <= last;) {
        u64 li = p / LEAF_PAGES, leaf_end = (li + 1) * LEAF_PAGES - 1, stop = last < leaf_end ? last : leaf_end;
        const PendLeaf* l = leaf[li];
        if (l && l->pages)
            while (p <= stop) {
                u32 i = (u32)(p % LEAF_PAGES);
                u64 gstop = p + (63u - (i & 63u)) < stop ? p + (63u - (i & 63u)) : stop;
                if (!l->group[i / 64]) {
                    p = gstop + 1;
                    continue;
                }
                for (; p <= gstop; p++)
                    if (l->count[p % LEAF_PAGES]) return 1;
            }
        p = stop + 1;
    }
    return 0;
}
/* pages [first, last] of one table gain delta each: those past TABLE_PAGES in its overflow */
static void count_pages(PendPages* t, u64 first, u64 last, int delta) {
    t->total += (last - first + 1) * (u64)(i64)delta;
    if (last >= TABLE_PAGES) {
        t->overflow += (last - (first > TABLE_PAGES ? first : TABLE_PAGES) + 1) * (u64)(i64)delta;
        if (first >= TABLE_PAGES) return;
        last = TABLE_PAGES - 1;
    }
    leaves_add(t->leaf, first, last, delta);
}
/* GPU pages that show guest pages a result gained or lost: they gain or lose it in its table and in the ones counting
   it as well */
typedef struct {
    PendPages* t;
    int delta;
} AliasAdd;
static void alias_add(void* ctx, u64 gva, u64 n) {
    const AliasAdd* a = ctx;
    for (PendPages* t = a->t; t; t = t->also) leaves_add(t->gpu, gva >> 12, (gva + n - 1) >> 12, a->delta);
}
/* the guest pages [first, last] gain or lose a result, and so does every GPU page showing one of them: the aliases are
   found here, once for a result, so that pend_hit, asked far more often, needs no translation */
static int add_pages(void* table, u64 first, u64 last, int delta) {
    AliasAdd a;
    for (PendPages* t = table; t; t = t->also) {
        count_pages(t, first, last, delta);
        if (!t->listed) {
            if (g_pending_pages.n == sizeof g_pending_pages.table / sizeof *g_pending_pages.table)
                fatal("vulkan: more tables of pending result pages than pend_remap has room for");
            g_pending_pages.table[g_pending_pages.n++] = t;
            t->listed = 1;
        }
    }
    if (first >= TABLE_PAGES) return 0; /* the overflow answers for them */
    if (last >= TABLE_PAGES) last = TABLE_PAGES - 1;
    a.t = table;
    a.delta = delta;
    gmmu_aliases(first << 12, (last - first + 1) << 12, alias_add, &a);
    return 0;
}
/* calls fn for each run of guest pages behind [gva, gva + size), skipping unmapped GPU pages, until it returns nonzero */
static int each_run(u64 gva, u64 size, int (*fn)(void* t, u64 first, u64 last, int delta), void* t, int delta) {
    u64 a = gva, left = gva && size > 0 - gva ? 0 - gva : size; /* up to the top of the address space */
    while (left) {
        u64 cpu, n = gmmu_run(a, left, &cpu);
        if (!n) { /* an unmapped page: nothing of guest memory behind it */
            n = 0x1000 - (a & 0xFFF);
            if (n >= left) break;
        } else if (fn(t, cpu >> 12, (cpu + n - 1) >> 12, delta))
            return 1;
        a += n;
        left -= n;
    }
    return 0;
}
/* a result added counts in the table it is added to and in the one counting it as well: g_pend_any.adds unchanged is
   then no result of any kind added (tex_resolve and stage_set in render.c keep what they read while it is) */
static void count_add(PendPages* t, int delta) {
    if (delta <= 0) return;
    t->adds++;
    if (t->also) t->also->adds++;
}
void pend_add(PendPages* t, u64 gva, u64 size, int delta) {
    count_add(t, delta);
    each_run(gva, size, add_pages, t, delta);
}
/* whether a guest page of [first, last] has a count: pend_hit's answer before the GPU pages, for SNX_ALIAS_CHECK */
static int hit_pages(void* table, u64 first, u64 last, int delta) {
    const PendPages* t = table;
    (void)delta;
    if (first >= TABLE_PAGES) return 0;
    return leaves_hit(t->leaf, first, last < TABLE_PAGES ? last : TABLE_PAGES - 1);
}
/* the GPU pages of [gva, gva + size), up to the top of the address space, as the GMMU numbers them, dropping the bits
   past its 40 (nvdrv.c): one range, or two where they wrap. Returns how many */
static int gpu_ranges(u64 gva, u64 size, u64 r[4]) {
    u64 first = gva >> 12, last = (gva + size - 1 < gva ? ~0ull : gva + size - 1) >> 12;
    if (last - first >= TABLE_PAGES - 1) {
        r[0] = 0;
        r[1] = TABLE_PAGES - 1;
        return 1;
    }
    first &= TABLE_PAGES - 1;
    last &= TABLE_PAGES - 1;
    r[0] = first;
    r[1] = first <= last ? last : TABLE_PAGES - 1;
    r[2] = 0;
    r[3] = last;
    return first <= last ? 1 : 2;
}
static int alias_check_on(void) {
    if (g_pending_pages.check < 0) {
        const char* e = getenv("SNX_ALIAS_CHECK");
        g_pending_pages.check = e && *e && *e != '0';
        if (g_pending_pages.check) LOG("vulkan: SNX_ALIAS_CHECK: pending results are checked through the GMMU too\n");
    }
    return g_pending_pages.check;
}
int pend_hit(const PendPages* t, u64 gva, u64 size) {
    u64 r[4];
    int n, i;
    if (!t->total || !size) return 0;
    if (t->overflow) return 1;
    for (i = 0, n = gpu_ranges(gva, size, r); i < n; i++)
        if (leaves_hit(t->gpu, r[2 * i], r[2 * i + 1])) return 1;
    if (alias_check_on() && each_run(gva, size, hit_pages, (void*)t, 0)) {
        WARN_LIMIT(16, "vulkan: a result is pending behind GPU range %llx+%llx, but not counted at its GPU pages\n",
                   (unsigned long long)gva, (unsigned long long)size);
        return 1;
    }
    return 0;
}
/* GPU pages from gva on show guest pages from cpu on now: each takes their count */
static void remap_piece(void* ctx, u64 gva, u64 cpu, u64 n) {
    PendPages* t = ctx;
    for (u64 k = 0; k < n >> 12; k++) {
        u64 c = (cpu >> 12) + k;
        u32 v = c < TABLE_PAGES ? leaf_count(t->leaf, c) : 0;
        if (v) leaf_set(t->gpu, (gva >> 12) + k, v);
    }
}
void pend_remap(u64 gva, u64 size) {
    u64 r[4];
    int n, i;
    if (!size) return;
    n = gpu_ranges(gva, size, r);
    for (u32 k = 0; k < g_pending_pages.n; k++) {
        PendPages* t = g_pending_pages.table[k];
        if (!t->total) continue; /* no guest page counted, so no GPU page either */
        for (i = 0; i < n; i++) {
            u64 first = r[2 * i], last = r[2 * i + 1];
            for (u64 p = first; p <= last; p++) { /* what the pages showed before is gone */
                const PendLeaf* l = t->gpu[p / LEAF_PAGES];
                if (!l || !l->pages)
                    p |= LEAF_PAGES - 1;
                else
                    leaf_set(t->gpu, p, 0);
            }
            gmmu_pieces(first << 12, (last - first + 1) << 12, remap_piece, t);
        }
    }
}
void pend_add_pages(PendPages* t, u64 first, u64 last, int delta) {
    count_add(t, delta);
    add_pages(t, first, last, delta);
}
void pend_add_va(PendPages* t, u64 gva, u64 size, int delta) {
    count_add(t, delta);
    if (!size) return;
    for (PendPages* u = t; u; u = u->also)
        count_pages(u, gva >> 12, (gva + size - 1 < gva ? ~0ull : gva + size - 1) >> 12, delta);
}
int pend_hit_va(const PendPages* t, u64 gva, u64 size) {
    u64 first = gva >> 12, last = (gva + size - 1 < gva ? ~0ull : gva + size - 1) >> 12;
    if (!t->total) return 0;
    if (t->overflow) return 1;
    return size && first < TABLE_PAGES && leaves_hit(t->leaf, first, last < TABLE_PAGES ? last : TABLE_PAGES - 1);
}
int pend_empty(const PendPages* t) { return !t->total; }
int pend_hit_pages(const PendPages* t, u64 first, u64 last) {
    if (!t->total) return 0;
    if (t->overflow) return 1;
    return first < TABLE_PAGES && leaves_hit(t->leaf, first, last < TABLE_PAGES ? last : TABLE_PAGES - 1);
}
