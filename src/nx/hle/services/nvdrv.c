/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/nvdrv/devices/nvmap.cpp, nvhost_as_gpu.cpp, nvhost_ctrl.cpp,
   nvhost_ctrl_gpu.cpp, nvhost_gpu.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
   SPDX-FileCopyrightText: 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: 2021 Skyline Team and Contributors */
/* nvdrv's devices: nvmap, the GPU address space, nvhost-ctrl's syncpoints and events, ctrl-gpu, the GPU channels, and
   the NVDEC and VIC fds that gpu/video/host1x.c serves */
#include "nx/hle/hle.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/kernel/ipc.h"
#include "hle/services/nvdrv.swipc.inc"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "common/utilities/endian.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/video/host1x.h"
#include "nx/runtime/clock.h"
#include <stdlib.h>
#include <string.h>
static void nv_events_check(void);

/* host1x (nvmap_get) and vi (nvmap_addr_by_id) read handles on the game's other threads while they create, alloc
   and free them, so under the lock, the address and the size in one hold: two would let them describe two handles */
int nvmap_get(u32 h, u64* addr, u32* size) {
    pm_lock(&g_hle.nvdrv.lock);
    int alive = h && h < 8192 && g_hle.nvdrv.nvh[h].alive && g_hle.nvdrv.nvh[h].allocated;
    *addr = alive ? g_hle.nvdrv.nvh[h].addr : 0;
    *size = alive ? g_hle.nvdrv.nvh[h].size : 0;
    pm_unlock(&g_hle.nvdrv.lock);
    return alive;
}
/* the live handle an id names, 0 for none. Called with the lock held */
/* CREATE gives a handle its own number as its id, so the id is the index (Eden's IocFromId: handles and ids are the
   same value); the check on id keeps that an assumption this function states rather than one it hides */
static u32 nvmap_from_id(u32 id) {
    const NvMapH* m;
    if (!id || id >= NVMAP_HANDLES) return 0;
    m = &g_hle.nvdrv.nvh[id];
    return m->alive && m->id == id ? id : 0;
}
u64 nvmap_addr_by_id(u32 id) {
    pm_lock(&g_hle.nvdrv.lock);
    u32 h = nvmap_from_id(id);
    u64 a = h && g_hle.nvdrv.nvh[h].allocated ? g_hle.nvdrv.nvh[h].addr : 0;
    pm_unlock(&g_hle.nvdrv.lock);
    return a;
}

/* the GPU's 40-bit address space */
/* two level table, gva -> guest address, 4K granularity. The GPU thread reads it while ioctl_as
   changes it on others: an L2 table is installed with a CAS and loaded with acquire, entries are relaxed atomics */
#define GPAGE_BITS 12 /* 4K GPU pages */
#define GL2_BITS   12 /* gmmu_l2's tables: 4096 entries */
/* first-level entries: 16 bits of page number above the 12 a second-level table covers, for 40 bits in all */
#define GL1        (1u << (40 - GPAGE_BITS - GL2_BITS))
static_assert(GL1 == NV_GMMU_L1, "nvdrv.h sizes the first level");
static u32 gmmu_l1_index(u64 page) { return (u32)(page >> GL2_BITS) & (GL1 - 1); }
/* the guest address of a GPU address, 0 when its page isn't mapped. On the GPU thread, while ioctl_as maps and unmaps
   on others. Bits past the 40 the table covers are dropped, as the GPU's MMU does */
u64 gmmu_translate(u64 gva) {
    u64 page = gva >> GPAGE_BITS;
    u64* t = __atomic_load_n(&g_hle.nvdrv.gmmu[gmmu_l1_index(page)], __ATOMIC_ACQUIRE);
    u64 e;
    if (!t) return 0;
    e = __atomic_load_n(&t[page & ((1u << GL2_BITS) - 1)], __ATOMIC_RELAXED);
    if (!(e & 1)) return 0;
    e = (e & ~(u64)0xFFF) | (gva & 0xFFF);
    return mem_mapped_run(e, 1) ? e : 0; /* a page the guest freed after mapping it here: as unmapped */
}
/* An entry's bits 1 to 11 count the pages after it, in its table, that are mapped and follow on in guest memory: a
   lower bound, so gmmu_run steps over a run of them in one load instead of one per page. A map writes its pages from the
   last, each counting those after it already written, and a change first shortens the counts before it that reach into
   it (gmmu_clip) */
#define GHINT_MAX  2047u
#define GHINT_MASK ((u64)GHINT_MAX << 1)
/* the bytes from gva on, at most max, whose pages map to consecutive guest pages, and in *cpu the guest address of gva:
   0 when gva's page isn't mapped. One walk of the table for a range, where gmmu_translate per page walks it again */
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    u64 page = gva >> GPAGE_BITS, n, e, want;
    u64* t = __atomic_load_n(&g_hle.nvdrv.gmmu[gmmu_l1_index(page)], __ATOMIC_ACQUIRE);
    if (!max || !t) return 0;
    e = __atomic_load_n(&t[page & ((1u << GL2_BITS) - 1)], __ATOMIC_RELAXED);
    if (!(e & 1)) return 0;
    *cpu = (e & ~(u64)0xFFF) | (gva & 0xFFF);
    n = 0x1000 - (gva & 0xFFF);
    want = (e & ~(u64)0xFFF) + 0x1000;
    while (n < max) {
        u64 h = (e & GHINT_MASK) >> 1;
        if (h) { /* the next h pages follow on: to the last of them, whose entry tells what follows it */
            page += h;
            n += h << GPAGE_BITS;
            want += h << GPAGE_BITS;
            if (n >= max) break;
            e = __atomic_load_n(&t[page & ((1u << GL2_BITS) - 1)], __ATOMIC_RELAXED);
            continue;
        }
        page++;
        if (!(page & ((1u << GL2_BITS) - 1)))
            t = __atomic_load_n(&g_hle.nvdrv.gmmu[gmmu_l1_index(page)], __ATOMIC_ACQUIRE);
        if (!t) break;
        e = __atomic_load_n(&t[page & ((1u << GL2_BITS) - 1)], __ATOMIC_RELAXED);
        if (!(e & 1) || (e & ~(u64)0xFFF) != want) break;
        n += 0x1000;
        want += 0x1000;
    }
    /* a game's GPU mappings can outlive the guest memory behind them, which has no host memory left: the run stops
       there, as at a page with no entry */
    return mem_mapped_run(*cpu, n < max ? n : max);
}
static u64* gmmu_l2(u32 i) {
    u64* t = __atomic_load_n(&g_hle.nvdrv.gmmu[i], __ATOMIC_ACQUIRE);
    if (t) return t;
    u64* n = calloc(4096, 8);
    if (!n) fatal("nvdrv: out of memory for the GPU page table\n");
    if (__atomic_compare_exchange_n(&g_hle.nvdrv.gmmu[i], &t, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return n;
    free(n);
    return t;
}
/* the renderer's caches may describe what was mapped at [gva, gva+size) before: they hear of every change */
static void gmmu_change_begin(u64 gva, u64 size) {
    if (g_gpu_mapping_begin) g_gpu_mapping_begin(gva, size, 0);
}
static void gmmu_change_end(u64 gva, u64 size) {
    gpu_mem_written(gva, size);
    if (g_gpu_mapping_end) g_gpu_mapping_end();
}
/* the counts of the entries before page first stop short of it, before it changes */
static void gmmu_clip(u64 first) {
    for (u64 k = 1; k <= GHINT_MAX && k <= first; k++) {
        u64 q = first - k, e;
        u64* t;
        if ((q ^ first) >> GL2_BITS) break; /* counts stay in their table */
        t = __atomic_load_n(&g_hle.nvdrv.gmmu[gmmu_l1_index(q)], __ATOMIC_ACQUIRE);
        if (!t) break;
        e = __atomic_load_n(&t[q & ((1u << GL2_BITS) - 1)], __ATOMIC_RELAXED);
        if ((e & GHINT_MASK) >> 1 >= k)
            __atomic_store_n(&t[q & ((1u << GL2_BITS) - 1)], (e & ~GHINT_MASK) | ((k - 1) << 1), __ATOMIC_RELAXED);
    }
}
/* ---- the page table again as runs, and their index by guest page, for gmmu_aliases. Mappings change a few times a
   frame at most, while the renderer asks, hundreds of thousands of times a frame, about GPU ranges where results it
   keeps per page may have been written through another GPU address: the index lets it record those once, at every
   alias, when a result is made (vk/pending_pages.c, maxwell/maxwell3d.c) instead of translating each range it asks
   about. The runs follow the table's page numbers, which wrap at GPAGES as gmmu_l1_index drops the bits past them */
#define GPAGES ((u64)1 << (40 - GPAGE_BITS))
static void grun_room(u32 more) {
    u32 nc = g_hle.nvdrv.capgruns ? g_hle.nvdrv.capgruns : 1024;
    GRun* r;
    if (g_hle.nvdrv.ngruns + more <= g_hle.nvdrv.capgruns) return;
    while (nc < g_hle.nvdrv.ngruns + more) nc *= 2;
    r = realloc(g_hle.nvdrv.gruns, (size_t)nc * sizeof *r);
    if (!r) fatal("nvdrv: out of memory for the GPU page table's runs\n");
    g_hle.nvdrv.gruns = r;
    g_hle.nvdrv.capgruns = nc;
}
/* the first run that ends past page g */
static u32 grun_find(u64 g) {
    const GRun* r = g_hle.nvdrv.gruns;
    u32 lo = 0, hi = g_hle.nvdrv.ngruns;
    while (lo < hi) {
        u32 m = lo + (hi - lo) / 2;
        if (r[m].gpage + r[m].pages <= g)
            lo = m + 1;
        else
            hi = m;
    }
    return lo;
}
/* takes pages [g0, g1) out of the runs. With own, only from the runs still showing guest page c0 + (p - g0) at each
   page p, which are the pages gmmu_unmap's own clears: a run shows them at all its pages or at none */
static void grun_cut(u64 g0, u64 g1, int own, u64 c0) {
    GRun* r = g_hle.nvdrv.gruns;
    u32 n = g_hle.nvdrv.ngruns, k = grun_find(g0), w;
    if (k < n && r[k].gpage < g0 && r[k].gpage + r[k].pages > g1) { /* inside one run: its head and tail stay */
        GRun tail;
        if (own && r[k].cpage - r[k].gpage != c0 - g0) return;
        tail.gpage = g1;
        tail.pages = r[k].gpage + r[k].pages - g1;
        tail.cpage = r[k].cpage + (g1 - r[k].gpage);
        r[k].pages = g0 - r[k].gpage;
        grun_room(1);
        r = g_hle.nvdrv.gruns;
        memmove(&r[k + 2], &r[k + 1], (n - k - 1) * sizeof *r);
        r[k + 1] = tail;
        g_hle.nvdrv.ngruns++;
        return;
    }
    for (w = k; k < n && r[k].gpage < g1; k++) {
        GRun e = r[k];
        u64 end = e.gpage + e.pages;
        if (!own || e.cpage - e.gpage == c0 - g0) {
            if (e.gpage < g0) { /* its head stays */
                e.pages = g0 - e.gpage;
            } else if (end > g1) { /* its tail stays */
                e.cpage += g1 - e.gpage;
                e.gpage = g1;
                e.pages = end - g1;
            } else /* all of it goes */
                continue;
        }
        r[w++] = e;
    }
    if (w == k) return;
    memmove(&r[w], &r[k], (n - k) * sizeof *r);
    g_hle.nvdrv.ngruns = n - (k - w);
}
/* the table's pages first to last (gmmu_map's and gmmu_unmap's numbers) changed: mapped to guest memory from cpu on,
   or cleared (with own, only those still mapped where a mapping of cpu put them) */
static void grun_change(u64 first, u64 last, int map, int own, u64 cpu) {
    pm_lock(&g_hle.nvdrv.alias_lock);
    for (;;) {
        u64 g = first & (GPAGES - 1), n = last - first + 1;
        if (n > GPAGES - g) n = GPAGES - g; /* the rest wraps to page 0 */
        if (map) {
            u32 k;
            grun_cut(g, g + n, 0, 0);
            grun_room(1);
            k = grun_find(g);
            memmove(&g_hle.nvdrv.gruns[k + 1], &g_hle.nvdrv.gruns[k],
                    (g_hle.nvdrv.ngruns - k) * sizeof *g_hle.nvdrv.gruns);
            g_hle.nvdrv.gruns[k].gpage = g;
            g_hle.nvdrv.gruns[k].pages = n;
            g_hle.nvdrv.gruns[k].cpage = cpu >> GPAGE_BITS;
            g_hle.nvdrv.ngruns++;
        } else if (!own || !(cpu & 0xFFE)) /* an own unmap compares its address's bits 1 to 11 too: set, none match */
            grun_cut(g, g + n, own, cpu >> GPAGE_BITS);
        if (n >= last - first + 1) break;
        first += n;
        cpu += n << GPAGE_BITS;
    }
    g_hle.nvdrv.alias.stale = 1;
    pm_unlock(&g_hle.nvdrv.alias_lock);
}
/* where a run starts (bit 0 set) or ends in guest memory, by page, ends first at a page */
typedef struct {
    u64 key;
    u32 run;
} GEdge;
static int gedge_cmp(const void* a, const void* b) {
    u64 x = ((const GEdge*)a)->key, y = ((const GEdge*)b)->key;
    return (x > y) - (x < y);
}
/* the index from the runs: a sweep over their edges in guest order, keeping the runs over the current interval in
   act (pos[r] is run r's place there). The first pass counts what the second writes */
static void alias_build(void) {
    GAlias* x = &g_hle.nvdrv.alias;
    const GRun* r = g_hle.nvdrv.gruns;
    u32 n = g_hle.nvdrv.ngruns, ne = 2 * n, i, nb = 0, nact, pass;
    u64 ncover = 0;
    GEdge* e;
    u32 *act, *pos;
    x->stale = 0;
    x->nbound = 0;
    x->query = 0;
    if (!n) return;
    if (n > 0x40000000u) fatal("nvdrv: too many GPU mappings for their index\n");
    e = malloc((size_t)ne * sizeof *e + 2 * (size_t)n * sizeof *act);
    if (!e) fatal("nvdrv: out of memory for the GPU mappings' index\n");
    act = (u32*)(e + ne);
    pos = act + n;
    for (i = 0; i < n; i++) {
        e[2 * i].key = r[i].cpage << 1 | 1;
        e[2 * i].run = i;
        e[2 * i + 1].key = (r[i].cpage + r[i].pages) << 1;
        e[2 * i + 1].run = i;
    }
    qsort(e, ne, sizeof *e, gedge_cmp);
    for (pass = 0; pass < 2; pass++) {
        nb = 0;
        ncover = 0;
        nact = 0;
        for (i = 0; i < ne;) {
            u64 page = e[i].key >> 1;
            for (; i < ne && e[i].key >> 1 == page; i++) {
                u32 k = e[i].run, moved;
                if (e[i].key & 1) {
                    pos[k] = nact;
                    act[nact++] = k;
                    continue;
                }
                moved = act[--nact];
                act[pos[k]] = moved;
                pos[moved] = pos[k];
            }
            if (pass) {
                x->bound[nb] = page;
                x->first[nb] = (u32)ncover;
                memcpy(x->cover + ncover, act, nact * sizeof *act);
            }
            nb++;
            ncover += nact;
        }
        if (!pass) {
            size_t need;
            if (ncover > 0xFFFFFFFFu - nb) fatal("nvdrv: the GPU mappings' index is too large\n");
            need = (size_t)nb * sizeof *x->bound + ((size_t)nb + 1 + n + (size_t)ncover) * sizeof *x->first;
            if (need > x->cap) {
                free(x->bound);
                x->bound = malloc(need);
                if (!x->bound) fatal("nvdrv: out of memory for the GPU mappings' index\n");
                x->cap = need;
            }
            x->first = (u32*)(x->bound + nb);
            x->seen = x->first + nb + 1;
            x->cover = x->seen + n;
            memset(x->seen, 0, n * sizeof *x->seen);
        }
    }
    x->first[nb] = (u32)ncover;
    x->nbound = nb;
    free(e);
}
void gmmu_aliases(u64 cpu, u64 size, void (*fn)(void* ctx, u64 gva, u64 n), void* ctx) {
    GAlias* x = &g_hle.nvdrv.alias;
    u64 end = cpu + size < cpu ? ~(u64)0 : cpu + size, c0 = cpu >> GPAGE_BITS, c1 = (end - 1) >> GPAGE_BITS;
    u32 lo = 0, hi, i, k;
    if (!size) return;
    pm_lock(&g_hle.nvdrv.alias_lock);
    if (x->stale) alias_build();
    if (!++x->query && x->nbound) { /* the stamps wrapped: none may look current */
        memset(x->seen, 0, g_hle.nvdrv.ngruns * sizeof *x->seen);
        x->query = 1;
    }
    for (hi = x->nbound; lo < hi;) { /* past the last bound at or before c0 */
        u32 m = lo + (hi - lo) / 2;
        if (x->bound[m] <= c0)
            lo = m + 1;
        else
            hi = m;
    }
    for (i = lo ? lo - 1 : 0; i + 1 < x->nbound && x->bound[i] <= c1; i++)
        for (k = x->first[i]; k < x->first[i + 1]; k++) {
            u32 ri = x->cover[k];
            const GRun* r = &g_hle.nvdrv.gruns[ri];
            u64 rlo = r->cpage << GPAGE_BITS, rhi = (r->cpage + r->pages) << GPAGE_BITS, a, b;
            if (x->seen[ri] == x->query) continue;
            x->seen[ri] = x->query;
            a = rlo > cpu ? rlo : cpu;
            b = rhi < end ? rhi : end;
            if (a < b) fn(ctx, (r->gpage << GPAGE_BITS) + (a - rlo), b - a);
        }
    pm_unlock(&g_hle.nvdrv.alias_lock);
}
void gmmu_pieces(u64 gva, u64 size, void (*fn)(void* ctx, u64 gva, u64 cpu, u64 n), void* ctx) {
    u64 g0 = gva >> GPAGE_BITS, g1;
    if (!size) return;
    g1 = ((gva + size - 1) >> GPAGE_BITS) + 1;
    pm_lock(&g_hle.nvdrv.alias_lock);
    for (u32 k = grun_find(g0); k < g_hle.nvdrv.ngruns && g_hle.nvdrv.gruns[k].gpage < g1; k++) {
        const GRun* r = &g_hle.nvdrv.gruns[k];
        u64 a = r->gpage > g0 ? r->gpage : g0, b = r->gpage + r->pages < g1 ? r->gpage + r->pages : g1;
        fn(ctx, a << GPAGE_BITS, (r->cpage + (a - r->gpage)) << GPAGE_BITS, (b - a) << GPAGE_BITS);
    }
    pm_unlock(&g_hle.nvdrv.alias_lock);
}
void gmmu_map(u64 gva, u64 cpu, u64 size) {
    u64 first, last, p;
    if (!size) return;
    gmmu_change_begin(gva, size);
    first = gva >> GPAGE_BITS;
    last = (gva + size - 1) >> GPAGE_BITS;
    gmmu_clip(first);
    for (p = last;; p--) { /* from the last, so a page's count covers only pages already written */
        u64* t = gmmu_l2(gmmu_l1_index(p));
        u64 h = last - p, room = ((1u << GL2_BITS) - 1) - (p & ((1u << GL2_BITS) - 1));
        u64 e;
        if (h > room) h = room;
        if (h > GHINT_MAX) h = GHINT_MAX;
        e = (cpu + ((p - first) << GPAGE_BITS)) | (h << 1) | 1;
        __atomic_store_n(&t[p & ((1u << GL2_BITS) - 1)], e, __ATOMIC_RELAXED);
        if (p == first) break;
    }
    grun_change(first, last, 1, 0, cpu);
    gmmu_change_end(gva, size);
}
/* clears the pages of [gva, gva+size). With own set, only the pages still pointing where a mapping of cpu put them:
   unmapping a buffer leaves alone what was mapped over it since */
static void gmmu_unmap(u64 gva, u64 size, int own, u64 cpu) {
    u64 first, last, p;
    if (!size) return;
    gmmu_change_begin(gva, size);
    first = gva >> GPAGE_BITS;
    last = (gva + size - 1) >> GPAGE_BITS;
    /* no count may reach a page about to be cleared: the ones before the range stop short of it, and the pages in it
       that an own unmap keeps lose theirs */
    gmmu_clip(first);
    for (p = first; p <= last; p++) {
        u64* t = __atomic_load_n(&g_hle.nvdrv.gmmu[gmmu_l1_index(p)], __ATOMIC_ACQUIRE);
        u64 e;
        if (!t) {
            p |= (1u << GL2_BITS) - 1;
            continue;
        }
        e = __atomic_load_n(&t[p & ((1u << GL2_BITS) - 1)], __ATOMIC_RELAXED);
        if (e & GHINT_MASK) __atomic_store_n(&t[p & ((1u << GL2_BITS) - 1)], e & ~GHINT_MASK, __ATOMIC_RELAXED);
    }
    for (p = first; p <= last; p++) {
        u64* t = __atomic_load_n(&g_hle.nvdrv.gmmu[gmmu_l1_index(p)], __ATOMIC_ACQUIRE);
        u64* e;
        if (!t) { /* nothing was ever mapped in this table's range: skip to the next table */
            p |= (1u << GL2_BITS) - 1;
            continue;
        }
        e = &t[p & ((1u << GL2_BITS) - 1)];
        if (own && (__atomic_load_n(e, __ATOMIC_RELAXED) & ~GHINT_MASK) != ((cpu + ((p - first) << GPAGE_BITS)) | 1))
            continue;
        __atomic_store_n(e, 0, __ATOMIC_RELAXED);
    }
    grun_change(first, last, 0, own, cpu);
    gmmu_change_end(gva, size);
}
static GMap* gmap_add(void) {
    if (g_hle.nvdrv.ngmaps == g_hle.nvdrv.capgmaps) {
        u32 nc = g_hle.nvdrv.capgmaps ? g_hle.nvdrv.capgmaps * 2 : 1024;
        GMap* g = realloc(g_hle.nvdrv.gmaps, nc * sizeof *g);
        if (!g) fatal("nvdrv: out of memory for GPU mappings\n");
        g_hle.nvdrv.gmaps = g;
        g_hle.nvdrv.capgmaps = nc;
    }
    return &g_hle.nvdrv.gmaps[g_hle.nvdrv.ngmaps++];
}
/* takes [gva, gva+size) out of the tracked pieces, splitting a piece that straddles it */
static void gmap_cut(u64 gva, u64 size) {
    u64 e = gva + size;
    for (u32 k = 0; k < g_hle.nvdrv.ngmaps;) {
        GMap* g = &g_hle.nvdrv.gmaps[k];
        u64 ge = g->gva + g->size;
        if (ge <= gva || g->gva >= e) {
            k++;
        } else if (g->gva < gva && ge > e) { /* the cut is inside: keep the head, add the tail */
            GMap tail = *g;
            tail.cpu += e - g->gva;
            tail.gva = e;
            tail.size = ge - e;
            g->size = gva - g->gva;
            *gmap_add() = tail;
            k++;
        } else if (g->gva < gva) {
            g->size = gva - g->gva;
            k++;
        } else if (ge > e) {
            g->cpu += e - g->gva;
            g->size = ge - e;
            g->gva = e;
            k++;
        } else
            g_hle.nvdrv.gmaps[k] = g_hle.nvdrv.gmaps[--g_hle.nvdrv.ngmaps];
    }
}
/* GPU VA for ALLOC_SPACE and MAP_BUFFER_EX: first fit from 0x04_0000_0000 to the end of the 40-bit space the gmmu
   covers, in 64K units. Not bounded to the big page region GET_VA_REGIONS reports: a game maps more than its
   0x1BFFF big pages (its NVN init maps 0x20000000 bytes past it) and checks nothing against the regions. Free ranges
   are kept sorted and merged */
#define VA_BASE 0x0400000000ull
#define VA_END  (1ull << 40)
static void va_insert(u32 k, u64 a, u64 e) {
    if (g_hle.nvdrv.nvafree == g_hle.nvdrv.capvafree) {
        u32 nc = g_hle.nvdrv.capvafree ? g_hle.nvdrv.capvafree * 2 : 64;
        VaRange* v = realloc(g_hle.nvdrv.vafree, nc * sizeof *v);
        if (!v) fatal("nvdrv: out of memory for the GPU address space\n");
        g_hle.nvdrv.vafree = v;
        g_hle.nvdrv.capvafree = nc;
    }
    memmove(&g_hle.nvdrv.vafree[k + 1], &g_hle.nvdrv.vafree[k], (g_hle.nvdrv.nvafree - k) * sizeof *g_hle.nvdrv.vafree);
    g_hle.nvdrv.vafree[k].a = a;
    g_hle.nvdrv.vafree[k].e = e;
    g_hle.nvdrv.nvafree++;
}
static void va_remove(u32 k) {
    memmove(&g_hle.nvdrv.vafree[k], &g_hle.nvdrv.vafree[k + 1],
            (g_hle.nvdrv.nvafree - k - 1) * sizeof *g_hle.nvdrv.vafree);
    g_hle.nvdrv.nvafree--;
}
static void va_init(void) {
    if (g_hle.nvdrv.va_ready) return;
    g_hle.nvdrv.va_ready = 1;
    va_insert(0, VA_BASE, VA_END);
}
static u64 va_round(u64 size) { return (size + 0xFFFF) & ~0xFFFFull; }
/* a GPU range the game names: inside the 40 bit space the page table covers. One past it would wrap the table's
   index onto low addresses */
static int gva_ok(u64 gva, u64 size) { return size <= VA_END && gva <= VA_END - size; }
/* guest memory a mapping points at: inside the guest's address space (GP() of an address past it is host memory) */
static int cpu_ok(u64 cpu, u64 size) { return size <= AS_SIZE && cpu <= AS_SIZE - size; }
/* takes [a, e) out of the free ranges */
static void va_take(u64 a, u64 e) {
    va_init();
    for (u32 k = 0; k < g_hle.nvdrv.nvafree;) {
        VaRange r = g_hle.nvdrv.vafree[k];
        if (r.e <= a || r.a >= e) {
            k++;
            continue;
        }
        va_remove(k);
        if (r.a < a) va_insert(k++, r.a, a);
        if (e < r.e) va_insert(k++, e, r.e);
    }
}
static void va_reserve(u64 a, u64 size) { va_take(a, a + va_round(size)); }
/* gives [a, a+size) back; only the part inside the region counts */
static void va_free(u64 a, u64 size) {
    va_init();
    u64 e = a + va_round(size), lo = VA_BASE, hi = VA_END;
    if (a < lo) a = lo;
    if (e > hi) e = hi;
    if (a >= e) return;
    va_take(a, e); /* what is already free isn't added twice */
    u32 k = 0;
    while (k < g_hle.nvdrv.nvafree && g_hle.nvdrv.vafree[k].a < a) k++;
    if (k > 0 && g_hle.nvdrv.vafree[k - 1].e == a) {
        a = g_hle.nvdrv.vafree[k - 1].a;
        va_remove(--k);
    }
    if (k < g_hle.nvdrv.nvafree && g_hle.nvdrv.vafree[k].a == e) {
        e = g_hle.nvdrv.vafree[k].e;
        va_remove(k);
    }
    va_insert(k, a, e);
}
/* 0 when the region has no room */
static u64 va_alloc(u64 size, u64 align) {
    va_init();
    align = align > 0x10000 ? align : 0x10000; /* the allocator's unit */
    for (u32 k = 0; k < g_hle.nvdrv.nvafree; k++) {
        u64 a = (g_hle.nvdrv.vafree[k].a + align - 1) & ~(align - 1);
        if (a < g_hle.nvdrv.vafree[k].a || a >= g_hle.nvdrv.vafree[k].e ||
            va_round(size) > g_hle.nvdrv.vafree[k].e - a)
            continue;
        va_reserve(a, size);
        return a;
    }
    WARN("nvdrv: no room left in the GPU address space for %llx bytes\n", (unsigned long long)size);
    return 0;
}

/* syncpoints */
u32 syncpt_read(u32 id) { return id < SYNCPT_IDS ? __atomic_load_n(&g_hle.nvdrv.syncpt[id], __ATOMIC_SEQ_CST) : 0; }
/* the value moved and its waiters woken: 0 for no increment */
static int syncpt_bump(u32 id, u32 n) {
    if (id >= SYNCPT_IDS || !n) return 0;
    pm_lock(&g_hle.nvdrv.sync_lk);
    u32 v = __atomic_add_fetch(&g_hle.nvdrv.syncpt[id], n, __ATOMIC_SEQ_CST);
    if ((i32)(v - g_hle.nvdrv.syncpt_max[id]) > 0) g_hle.nvdrv.syncpt_max[id] = v; /* the CPU promises nothing first */
    pc_broadcast(&g_hle.nvdrv.sync_cv);
    pm_unlock(&g_hle.nvdrv.sync_lk);
    return 1;
}
void syncpt_add(u32 id, u32 n) {
    if (!syncpt_bump(id, n)) return;
    nv_events_check();
    gpu_kick(); /* a GPU channel's stream may wait for it */
}
/* this thread's syncpt_add_quiet moved a syncpoint: its syncpt_events checks the event slots where syncpt_add would
   have, so the same thread takes the kernel's lock, after vk_lock goes */
static _Thread_local int t_events_due;
void syncpt_add_quiet(u32 id, u32 n) {
    if (!syncpt_bump(id, n)) return;
    t_events_due = 1;
    gpu_kick();
}
void syncpt_events(void) {
    if (!t_events_due) return;
    t_events_due = 0;
    nv_events_check();
}
void syncpt_incr(u32 id) { syncpt_add(id, 1); }
u32 syncpt_promise(u32 id, u32 n) {
    u32 cur, v;
    if (id >= SYNCPT_IDS) return 0;
    pm_lock(&g_hle.nvdrv.sync_lk);
    cur = syncpt_read(id);
    if ((i32)(cur - g_hle.nvdrv.syncpt_max[id]) > 0) g_hle.nvdrv.syncpt_max[id] = cur;
    g_hle.nvdrv.syncpt_max[id] += n;
    v = g_hle.nvdrv.syncpt_max[id];
    pm_unlock(&g_hle.nvdrv.sync_lk);
    return v;
}
/* whether syncpoint id has reached thresh, counting across the 32-bit wrap */
static int syncpt_reached(u32 id, u32 thresh) { return (i32)(syncpt_read(id) - thresh) >= 0; }
/* 1 once id reaches thresh, 0 when the time runs out first. timeout_ns 0 looks once, negative waits for good. The
   deadline is absolute, so a spurious wakeup doesn't extend it, and in guest time, which a game pause stops */
int syncpt_wait(u32 id, u32 thresh, i64 timeout_ns) {
    u64 deadline;
    int reached;
    if (id >= SYNCPT_IDS) return 0;
    if (syncpt_reached(id, thresh)) return 1;
    if (!timeout_ns) return 0;
    deadline = timeout_ns > 0 ? guest_ns() + (u64)timeout_ns : 0;
    /* syncpt_bump broadcasts under it, so no increment slips between the check and the wait */
    pm_lock(&g_hle.nvdrv.sync_lk);
    for (;;) {
        i64 left = -1;
        reached = syncpt_reached(id, thresh);
        if (reached) break;
        if (timeout_ns > 0) {
            u64 now = guest_ns();
            if (now >= deadline) break;
            left = (i64)(deadline - now);
        }
        kthread_wait(&g_hle.nvdrv.sync_cv, &g_hle.nvdrv.sync_lk, left);
    }
    pm_unlock(&g_hle.nvdrv.sync_lk);
    return reached;
}
/* every syncpoint user (GPU channels here, host1x channels in host1x.c) allocates here, so ids stay unique and low.
   NVIDIA's stream library sizes its per-syncpoint tables at 128 (NvRmChannelNumSyncPoints is a stub that fails), and an
   id past them writes into the game's heap: running out is fatal, and a closed channel (GPU or host1x) gives its ids
   back */
u32 syncpt_alloc(void) {
    pm_lock(&g_hle.nvdrv.sync_lk);
    for (u32 id = 20; id < SYNCPT_N; id++)
        if (!g_hle.nvdrv.syncpt_used[id]) {
            g_hle.nvdrv.syncpt_used[id] = 1;
            /* an id used before starts with nothing promised past its value */
            g_hle.nvdrv.syncpt_max[id] = g_hle.nvdrv.syncpt[id];
            pm_unlock(&g_hle.nvdrv.sync_lk);
            return id;
        }
    pm_unlock(&g_hle.nvdrv.sync_lk);
    fatal("nvdrv: all %u syncpoints are in use (NVIDIA's stream library has room for %u ids)\n", SYNCPT_N - 20,
          SYNCPT_N);
}
void syncpt_free(u32 id) {
    if (id >= SYNCPT_N) return;
    pm_lock(&g_hle.nvdrv.sync_lk);
    g_hle.nvdrv.syncpt_used[id] = 0;
    g_hle.nvdrv.syncpt_gpu[id] = 0;
    pm_unlock(&g_hle.nvdrv.sync_lk);
}
/* whether a present can leave a fence to the GPU thread: a GPU channel's value that submissions already promised is
   reached by that channel's jobs queued ahead of the present (a promise and its job are queued in one hold of
   submit_lk, which the caller takes after this). Any other fence (a host1x engine's, one the CPU increments, or one
   nothing promised yet) is waited for here, on the calling thread: a call on the GPU thread can't stand aside to wait
   as a stream can (gpu_park) */
static int fence_on_gpu_thread(u32 id, u32 val) {
    if (id == ~0u) return 0;
    pm_lock(&g_hle.nvdrv.sync_lk);
    int gpu = id < SYNCPT_N && g_hle.nvdrv.syncpt_gpu[id] && (i32)(val - g_hle.nvdrv.syncpt_max[id]) <= 0;
    pm_unlock(&g_hle.nvdrv.sync_lk);
    if (gpu) return 1;
    if (!syncpt_wait(id, val, 1000000000))
        WARN_LIMIT(
            8,
            "gpu: a present waited 1000 ms on its own thread for syncpoint %u to reach %u (it is at %u), and goes "
            "on\n",
            id, val, syncpt_read(id));
    return 0;
}
void nv_gpu_call_after(u32 n, u32* id, const u32* val, void (*fn)(void*), void* arg) {
    u32 chans[4], nchans = 0;
    for (u32 i = 0; i < n && i < 4; i++)
        if (fence_on_gpu_thread(id[i], val[i]))
            chans[nchans++] = id[i]; /* a GPU channel's syncpoint id is the channel */
        else
            id[i] = ~0u;    /* done with here */
    pm_lock(&g_hle.nvdrv.submit_lk); /* after the submissions promising the fences left to the GPU thread */
    gpu_call(fn, arg, chans, nchans);
    pm_unlock(&g_hle.nvdrv.submit_lk);
}
/* a GPU channel's syncpoint, allocated on first use. Two ioctls on the fd at once may both allocate: the one that
   stores it first under nv.lock wins, the other gives its id back (there are 108 to hand out) */
static u32 gpu_channel_syncpt(NvFile* f) {
    pm_lock(&g_hle.nvdrv.lock);
    u32 sp = f->syncpt;
    pm_unlock(&g_hle.nvdrv.lock);
    if (sp) return sp;
    u32 mine = syncpt_alloc();
    pm_lock(&g_hle.nvdrv.sync_lk); /* a GPU channel's before any ioctl can submit on it */
    g_hle.nvdrv.syncpt_gpu[mine] = 1;
    pm_unlock(&g_hle.nvdrv.sync_lk);
    pm_lock(&g_hle.nvdrv.lock);
    if (!f->syncpt) f->syncpt = mine;
    sp = f->syncpt;
    pm_unlock(&g_hle.nvdrv.lock);
    if (sp != mine) syncpt_free(mine); /* clears its syncpt_gpu too */
    return sp;
}
/* on the GPU thread, after the jobs queued on the channel before its fd closed */
static void gpu_channel_retire(void* p) {
    u32 id = (u32)(uintptr_t)p;
    gpu_release_retire(id);
    gpu_channel_close(id);
    syncpt_free(id);
}

static void nvev_lock(void) { pm_lock(&g_hle.nvdrv.nvev_lk); }
static void nvev_unlock(void) { pm_unlock(&g_hle.nvdrv.nvev_lk); }
/* ctrl fd fd's event slot, its table made if make is set (NULL without memory, or when it isn't made). Called with the
   lock held, for an fd that stays open meanwhile */
static NvEvent* nvev_slot(u32 fd, u32 slot, int make) {
    if (!g_hle.nvdrv.ctrl_ev[fd] && make) {
        NvEventTable* t = POOL_TAKE(g_hle.nvdrv.ctrl_evt, g_hle.nvdrv.ctrl_evt_next, "nvhost-ctrl fds with events");
        t->fd = fd + 1;
        g_hle.nvdrv.ctrl_ev[fd] = POOL_INDEX(g_hle.nvdrv.ctrl_evt, t) + 1;
    }
    return g_hle.nvdrv.ctrl_ev[fd] ? &g_hle.nvdrv.ctrl_evt[g_hle.nvdrv.ctrl_ev[fd] - 1].slot[slot & 63] : NULL;
}
/* the KEvent of an event slot, made on first use (NULL without memory). nvdrv keeps a reference of its own, so the
   game closing its handle doesn't free an event a later syncpoint increment signals. Called with the lock held */
static KEvent* nvev_get(u32 fd, u32 slot) {
    NvEvent* e = nvev_slot(fd, slot, 1);
    if (!e) return NULL;
    if (!e->ev) {
        KEvent* k = kevent_new("nvhost event");
        kobj_ref(&k->o);
        e->ev = kobj_id(&k->o);
    }
    return kev(e->ev);
}
/* the fd's slots, copied into out[64] and their table given back when its entry is cleared (with nv.lock held, so a
   new fd with the number doesn't meet them): out, or NULL when the fd had none. nv_release drops them */
static NvEvent* nvev_detach(u32 fd, NvEvent* out) {
    NvEventTable* t;
    nvev_lock();
    t = g_hle.nvdrv.ctrl_ev[fd] ? &g_hle.nvdrv.ctrl_evt[g_hle.nvdrv.ctrl_ev[fd] - 1] : NULL;
    if (t) {
        memcpy(out, t->slot, sizeof t->slot);
        POOL_PUT(t);
    }
    g_hle.nvdrv.ctrl_ev[fd] = 0;
    nvev_unlock();
    return t ? out : NULL;
}
/* signals the armed event slots whose syncpoint got where they wait for, and disarms them */
static void nv_events_check(void) {
    u32 t, i;
    nvev_lock();
    for (t = 0; t < NV_CTRL_TABLES; t++)
        for (i = 0; g_hle.nvdrv.ctrl_evt[t].fd && i < 64; i++) {
            NvEvent* e = &g_hle.nvdrv.ctrl_evt[t].slot[i];
            if (!e->armed || !syncpt_reached(e->id, e->thresh)) continue;
            e->armed = 0;
            if (e->ev) kevent_signal(kev(e->ev));
        }
    nvev_unlock();
}

/* the devices. Each handler gets the ioctl's argument in d (nv_ioctl's scratch buffer, so reads and writes stay inside
   it whatever n says), n the bytes the device may use, and answers an NvError: 0 Success, 1 BadValue, 4 BadParameter,
   5 Timeout, 6 InsufficientMemory (8, a bad fd, is nv_ioctl's). Layouts are Switchbrew's ("NV services") */
enum { DEV_NONE, DEV_NVMAP, DEV_AS, DEV_CTRL, DEV_CTRLGPU, DEV_GPU, DEV_NVDEC, DEV_VIC, DEV_OTHER };
/* an ioctl's argument block is a copy of the guest's buffer, so its words are little-endian */
static u32 rd32(const u8* d, u32 off) { return snx_ld_le32(d + off); }
static u64 rd64(const u8* d, u32 off) { return snx_ld_le64(d + off); }
static void wr32(u8* d, u32 off, u32 v) { snx_st_le32(d + off, v); }
static void wr64(u8* d, u32 off, u64 v) { snx_st_le64(d + off, v); }
/* an ioctl's second output (Ioctl3), up to its size */
static void copy_extra(u8* extra, u32 nextra, const void* src, u32 size) {
    if (extra) memcpy(extra, src, size < nextra ? size : nextra);
}

/* /dev/nvmap: handles to guest memory, numbered 1..8191. Every access to a handle is under nv.lock: host1x and vi
   read them on other threads */
static NvMapH* nvmap_live(u32 h) {
    return h && h < NVMAP_HANDLES && g_hle.nvdrv.nvh[h].alive ? &g_hle.nvdrv.nvh[h] : NULL;
}
static u32 ioctl_nvmap(u32 cmd, u8* d, u32 n) {
    u32 h, r = 0;
    NvMapH* m;
    (void)n;
    switch (cmd & 0xFF) {
        case 0x01: /* CREATE {u32 size; out u32 handle} */
            pm_lock(&g_hle.nvdrv.lock);
            h = g_hle.nvdrv.nvh_nfree                    ? g_hle.nvdrv.nvh_free[--g_hle.nvdrv.nvh_nfree]
                : g_hle.nvdrv.nvh_next < NVMAP_HANDLES ? g_hle.nvdrv.nvh_next++
                                                       : 0;
            if (h) {
                m = &g_hle.nvdrv.nvh[h];
                memset(m, 0, sizeof *m);
                m->size = rd32(d, 0);
                m->id = h;
                m->refs = 1;
                m->alive = 1;
            }
            pm_unlock(&g_hle.nvdrv.lock);
            wr32(d, 4, h);
            if (!h) {
                WARN_LIMIT(4, "nvdrv: nvmap has no handle left for %#x bytes\n", rd32(d, 0));
                return 6;
            }
            LOGV("nvdrv: nvmap create %#x bytes -> %u\n", rd32(d, 0), h);
            return 0;
        case 0x03: /* FROM_ID {u32 id; out u32 handle} */
            pm_lock(&g_hle.nvdrv.lock);
            h = nvmap_from_id(rd32(d, 0));
            if (h) g_hle.nvdrv.nvh[h].refs++;
            pm_unlock(&g_hle.nvdrv.lock);
            if (!h) return 1;
            wr32(d, 4, h);
            return 0;
        case 0x04: { /* ALLOC {u32 handle, heap_mask, flags, align; u8 kind; u64 addr at 24} */
            u64 addr = rd64(d, 24);
            h = rd32(d, 0);
            if (!h || h >= NVMAP_HANDLES) return 1;
            pm_lock(&g_hle.nvdrv.lock);
            m = nvmap_live(h);
            if (!m)
                r = 1;
            else if (!m->size || (addr & 0xFFF) || !cpu_ok(addr, m->size))
                r = 4; /* the handle stays as it was */
            else {
                m->heap = rd32(d, 4);
                m->flags = rd32(d, 8);
                m->align = rd32(d, 12);
                m->kind = d[16];
                m->addr = addr;
                m->allocated = 1;
            }
            pm_unlock(&g_hle.nvdrv.lock);
            if (!r) LOGV("nvdrv: nvmap alloc %u at %llx\n", h, (unsigned long long)addr);
            return r;
        }
        case 0x05: { /* FREE {u32 handle; out u64 addr at 8, u32 size at 16, u32 flags at 20} */
            int last = 0;
            h = rd32(d, 0);
            if (!h || h >= NVMAP_HANDLES) return 1;
            pm_lock(&g_hle.nvdrv.lock);
            m = nvmap_live(h);
            if (m) {
                wr64(d, 8, m->addr);
                wr32(d, 16, m->size);
                wr32(d, 20, m->flags);
                last = !--m->refs;
                if (last) m->alive = m->allocated = 0;
            }
            pm_unlock(&g_hle.nvdrv.lock);
            if (!m) return 1;
            if (last) {
                /* NVDEC and VIC drop what they mapped of it before the number can name another buffer */
                host1x_forget_handle(h);
                pm_lock(&g_hle.nvdrv.lock);
                g_hle.nvdrv.nvh_free[g_hle.nvdrv.nvh_nfree++] = h;
                pm_unlock(&g_hle.nvdrv.lock);
            }
            return 0;
        }
        case 0x09: { /* PARAM {u32 handle, u32 param; out u32 value} */
            u32 v = 0;
            pm_lock(&g_hle.nvdrv.lock);
            m = nvmap_live(rd32(d, 0));
            if (!m)
                r = 1;
            else
                switch (rd32(d, 4)) {
                    case 1: v = m->size; break;
                    case 2: v = m->align ? m->align : 0x1000; break;
                    case 3: v = (u32)m->addr; break;                   /* as Ryujinx answers Base */
                    case 4: v = m->allocated ? 0x40000000u : 0; break; /* Eden's heap */
                    case 5: v = m->kind; break;
                    case 6: v = 0; break; /* not compressed */
                    default: r = 1; break;
                }
            pm_unlock(&g_hle.nvdrv.lock);
            if (!r) wr32(d, 8, v);
            return r;
        }
        case 0x0E: /* GET_ID {out u32 id; u32 handle at 4} */
            pm_lock(&g_hle.nvdrv.lock);
            m = nvmap_live(rd32(d, 4));
            if (m) wr32(d, 0, m->id);
            pm_unlock(&g_hle.nvdrv.lock);
            return m ? 0 : 1;
        default: WARN_LIMIT(16, "nvdrv: nvmap ioctl %08x not implemented\n", cmd); return 0;
    }
}

/* /dev/nvhost-as-gpu. The caller holds nv.aslock, which covers nv.gmaps and the VA allocator */
static u32 as_alloc_space(u8* d) { /* {u32 pages, u32 page_size, u32 flags; u64 offset or align at 16, out offset} */
    u64 size = (u64)rd32(d, 0) * rd32(d, 4), field = rd64(d, 16), gva;
    if (rd32(d, 8) & 1) { /* fixed: the field is the address */
        gva = field;
        if (!gva_ok(gva, size)) {
            WARN_LIMIT(8, "nvdrv: ALLOC_SPACE at %llx of %llx bytes is outside the GPU address space\n",
                       (unsigned long long)gva, (unsigned long long)size);
            return 4;
        }
        va_reserve(gva, size);
    } else {
        u64 align = field > rd32(d, 4) ? field : rd32(d, 4);
        if (!align || (align & (align - 1)) || !gva_ok(0, size)) {
            WARN_LIMIT(8, "nvdrv: ALLOC_SPACE of %llx bytes aligned to %llx refused\n", (unsigned long long)size,
                       (unsigned long long)align);
            return 4;
        }
        gva = va_alloc(size, align);
        if (!gva) return 6;
        wr64(d, 16, gva);
    }
    LOGV("nvdrv: ALLOC_SPACE %llx bytes at %llx\n", (unsigned long long)size, (unsigned long long)gva);
    return 0;
}
static u32 as_unmap_buffer(u64 base) {
    u64 va = 0;
    u32 k = 0;
    while (k < g_hle.nvdrv.ngmaps) {
        GMap g = g_hle.nvdrv.gmaps[k];
        if (g.base != base) {
            k++;
            continue;
        }
        gmmu_unmap(g.gva, g.size, 1, g.cpu);
        if (g.va) va = g.va;
        g_hle.nvdrv.gmaps[k] = g_hle.nvdrv.gmaps[--g_hle.nvdrv.ngmaps];
    }
    if (va) va_free(base, va);
    return 0;
}
/* {u32 flags, u32 kind, u32 handle, u32 page_size; i64 buffer_offset at 16, u64 mapping_size at 24, u64 offset at 32
   (in and out)}. Flags: 1 fixed address, 0x100 modify part of an earlier mapping */
static u32 as_map_buffer(u8* d) {
    u32 flags = rd32(d, 0), h = rd32(d, 8), page = rd32(d, 12), hsize;
    i64 boff = (i64)rd64(d, 16);
    u64 msize = rd64(d, 24), gva = rd64(d, 32), haddr, size, span, cpu, base, va = 0;
    GMap* g;
    if ((flags & 0x100) && !h) return 0; /* only the kind changes, which nothing here keeps */
    if (!nvmap_get(h, &haddr, &hsize)) return 1;
    if (!haddr) WARN_LIMIT(4, "nvdrv: MAP_BUFFER_EX of nvmap handle %u, which has no address\n", h);
    size = msize ? msize : hsize;
    if (boff < 0 || size > ~(u64)0 - 0xFFF) return 4;
    span = (size + 0xFFF) & ~(u64)0xFFF; /* a page table entry exposes a whole page */
    if (!span || (u64)boff > AS_SIZE) return 4;
    cpu = haddr + (u64)boff;
    if ((cpu & 0xFFF) || !cpu_ok(cpu, span)) return 4;
    if (flags & 0x101) {
        if ((gva & 0xFFF) || !gva_ok(gva, span)) return 4;
    } else if (page & (page - 1))
        return 4;
    if (flags & 0x100) { /* the pieces it replaces keep naming the mapping UNMAP_BUFFER will end */
        u32 k;
        base = gva;
        for (k = 0; k < g_hle.nvdrv.ngmaps; k++)
            if (gva >= g_hle.nvdrv.gmaps[k].gva && gva - g_hle.nvdrv.gmaps[k].gva < g_hle.nvdrv.gmaps[k].size) {
                base = g_hle.nvdrv.gmaps[k].base;
                va = g_hle.nvdrv.gmaps[k].va;
                break;
            }
        gmap_cut(gva, span);
    } else if (flags & 1) {
        base = gva;
    } else {
        gva = va_alloc(span, page ? page : 0x10000);
        if (!gva) return 6;
        base = gva;
        va = span;
    }
    gmmu_map(gva, cpu, span);
    g = gmap_add();
    g->gva = gva;
    g->size = span;
    g->cpu = cpu;
    g->base = base;
    g->va = va;
    wr64(d, 32, gva);
    LOGV("nvdrv: MAP_BUFFER_EX handle %u +%llx: %llx bytes at %llx -> %llx\n", h, (unsigned long long)boff,
         (unsigned long long)span, (unsigned long long)gva, (unsigned long long)cpu);
    return 0;
}
/* GET_VA_REGIONS: the small-page region below the big-page one, which starts where the allocator does. The sizes are
   Eden's defaults (a 2^37 byte space, small pages from big page size << 10) */
static void as_va_regions(u8* d, u8* extra, u32 nextra) {
    u32 bits = g_hle.nvdrv.big_page_bits;
    u64 big = (u64)1 << bits, start = big << 10, end = (u64)1 << 37;
    u8 r[48];
    memset(r, 0, sizeof r);
    wr64(r, 0, start);
    wr32(r, 8, 0x1000);
    wr64(r, 16, (VA_BASE - start) >> 12);
    wr64(r, 24, VA_BASE);
    wr32(r, 32, (u32)big);
    wr64(r, 40, ((end - VA_BASE) >> bits) - (VA_BASE >> bits));
    wr32(d, 8, sizeof r);
    memcpy(d + 16, r, sizeof r);
    copy_extra(extra, nextra, r, sizeof r);
}
/* REMAP: entries of {u16 flags, u16 kind, u32 handle, u32 mem_offset, u32 gpu_offset, u32 pages}, in big pages. A
   bad entry is skipped and the rest go on: what the SDK makes of an error here is unknown */
static u32 as_remap(const u8* d, u32 n) {
    u32 bits = g_hle.nvdrv.big_page_bits, i;
    for (i = 0; i < n / 20; i++) {
        const u8* e = d + i * 20;
        u32 h = rd32(e, 4), hsize;
        u64 gva = (u64)rd32(e, 12) << bits, size = (u64)rd32(e, 16) << bits, haddr, cpu;
        if (!gva_ok(gva, size)) {
            WARN_LIMIT(8, "nvdrv: REMAP at %llx of %llx bytes is outside the GPU address space\n",
                       (unsigned long long)gva, (unsigned long long)size);
            continue;
        }
        if (!h) {
            gmmu_unmap(gva, size, 0, 0);
            LOG_LIMIT(16, "nvdrv: REMAP unmaps %llx bytes at %llx\n", (unsigned long long)size,
                      (unsigned long long)gva);
            continue;
        }
        if (!nvmap_get(h, &haddr, &hsize)) {
            WARN_LIMIT(8, "nvdrv: REMAP of nvmap handle %u, which isn't allocated\n", h);
            continue;
        }
        if (!haddr) WARN_LIMIT(4, "nvdrv: REMAP of nvmap handle %u, which has no address\n", h);
        cpu = haddr + ((u64)rd32(e, 8) << bits);
        if (!cpu_ok(cpu, size)) {
            WARN_LIMIT(8, "nvdrv: REMAP of handle %u past guest memory\n", h);
            continue;
        }
        gmmu_map(gva, cpu, size);
        LOG_LIMIT(16, "nvdrv: REMAP handle %u: %llx bytes at %llx -> %llx\n", h, (unsigned long long)size,
                  (unsigned long long)gva, (unsigned long long)cpu);
    }
    return 0;
}
static u32 ioctl_as(u32 cmd, u8* d, u32 n, u8* extra, u32 nextra) {
    switch (cmd & 0xFF) {
        case 0x01: return 0; /* BIND_CHANNEL */
        case 0x02: return as_alloc_space(d);
        case 0x03: { /* FREE_SPACE {u64 offset, u32 pages, u32 page_size} */
            u64 gva = rd64(d, 0), size = (u64)rd32(d, 8) * rd32(d, 12);
            if (!gva_ok(gva, size)) return 4;
            gmmu_unmap(gva, size, 0, 0);
            gmap_cut(gva, size);
            va_free(gva, size);
            return 0;
        }
        case 0x05: return as_unmap_buffer(rd64(d, 0)); /* UNMAP_BUFFER {u64 offset} */
        case 0x06: return as_map_buffer(d);
        case 0x08: as_va_regions(d, extra, nextra); return 0;
        case 0x09: { /* ALLOC_AS_EX {u32 flags, i32 as_fd, u32 big_page_size, ...} */
            u32 big = rd32(d, 8) ? rd32(d, 8) : 0x20000;
            if (big == 0x10000) g_hle.nvdrv.big_page_bits = 16;
            if (big == 0x20000) g_hle.nvdrv.big_page_bits = 17;
            LOG("nvdrv: GPU address space with %u KB big pages\n", (1u << g_hle.nvdrv.big_page_bits) >> 10);
            return 0;
        }
        case 0x14: return as_remap(d, n);
        default: WARN_LIMIT(16, "nvdrv: nvhost-as-gpu ioctl %08x not implemented\n", cmd); return 0;
    }
}

/* /dev/nvhost-ctrl: syncpoints and the event slots NvRm waits on */
static void nvev_disarm(u32 fd, u32 slot) { /* under nvev_lk; the event stays unsignaled: the wait was canceled */
    NvEvent* e = nvev_slot(fd, slot, 0);
    if (!e || !e->ev) return;
    e->armed = 0;
    kevent_clear(kev(e->ev));
}
/* EVENT_WAIT and EVENT_WAIT_ASYNC {u32 id, u32 thresh, i32 timeout_ms; u32 value}. The asynchronous one arms the slot
   of fd's that value names and answers Timeout with value naming that slot, which the SDK then waits on as an event.
   A timeout of 0 only asks: Timeout without arming the slot or clearing its event, which another wait on the slot may
   hold signaled, as Ryujinx's EventWait answers it */
static u32 ctrl_event_wait(u32 fd, u8* d, int async) {
    u32 id = rd32(d, 0), thresh = rd32(d, 4), slot = rd32(d, 12) & 63;
    i32 timeout = (i32)rd32(d, 8);
    NvEvent* e;
    if (syncpt_reached(id, thresh)) {
        wr32(d, 12, syncpt_read(id));
        return 0;
    }
    if (!timeout) return 5;
    if (!async) {
        if (syncpt_wait(id, thresh, timeout < 0 ? -1 : (i64)timeout * 1000000)) {
            wr32(d, 12, syncpt_read(id));
            return 0;
        }
        return 5;
    }
    nvev_lock();
    e = nvev_slot(fd, slot, 0);
    if (e && e->ev) {
        kevent_clear(kev(e->ev)); /* before arming: see nvev_lk */
        e->id = id;
        e->thresh = thresh;
        e->armed = 1;
    }
    nvev_unlock();
    nv_events_check(); /* it may have got there meanwhile */
    wr32(d, 12, 0x10000000u | ((id & 0xFFF) << 16) | slot);
    return 5;
}
static u32 ioctl_ctrl(u32 fd, u32 cmd, u8* d, u32 n) {
    u32 c = cmd & 0xFF, id = rd32(d, 0), i;
    if (((c >= 0x14 && c <= 0x1A) && c != 0x17 && c != 0x18) || c == 0x1D || c == 0x1E) {
        if (id >= SYNCPT_IDS) {
            WARN_LIMIT(8, "nvdrv: nvhost-ctrl ioctl %08x on syncpoint %u, past the table\n", cmd, id);
            return 4;
        }
    }
    switch (c) {
        case 0x14: wr32(d, 4, syncpt_read(id)); return 0; /* SYNCPT_READ {u32 id; out u32 value} */
        case 0x15: syncpt_incr(id); return 0;             /* SYNCPT_INCR {u32 id} */
        case 0x16:                                        /* SYNCPT_WAIT {u32 id, u32 thresh, i32 timeout_ms} */
        case 0x19: {                                      /* SYNCPT_WAITEX, with the value back at 12 */
            i32 timeout = (i32)rd32(d, 8);
            int ok = syncpt_wait(id, rd32(d, 4), timeout < 0 ? -1 : (i64)timeout * 1000000);
            if (n >= 16) wr32(d, 12, syncpt_read(id));
            return ok ? 0 : 5;
        }
        case 0x1A: /* SYNCPT_READ_MAX {u32 id; out u32 max}: submissions raise it under sync_lk */
            pm_lock(&g_hle.nvdrv.sync_lk);
            wr32(d, 4, g_hle.nvdrv.syncpt_max[id]);
            pm_unlock(&g_hle.nvdrv.sync_lk);
            return 0;
        case 0x1B: /* GET_CONFIG {char domain[0x41], param[0x41], value[0x101]}: no setting has a value */
            memset(d + 0x82, 0, 0x101);
            return 0;
        case 0x1C: /* CLEAR_EVENT_WAIT {u32 slot}: cancels without signaling */
            nvev_lock();
            nvev_disarm(fd, id & 63);
            nvev_unlock();
            LOG_LIMIT(8, "nvdrv: event slot %u wait canceled\n", id & 63);
            return 0;
        case 0x1D: return ctrl_event_wait(fd, d, 0);
        case 0x1E: return ctrl_event_wait(fd, d, 1);
        case 0x1F: { /* EVENT_REGISTER {u32 slot} */
            KEvent* k;
            nvev_lock();
            k = nvev_get(fd, id & 63);
            nvev_unlock();
            return k ? 0 : 6;
        }
        case 0x20: /* EVENT_UNREGISTER {u32 slot} */
            nvev_lock();
            nvev_disarm(fd, id & 63);
            nvev_unlock();
            return 0;
        case 0x21: { /* EVENT_KILL {u64 mask}, a bit per slot */
            u64 mask = rd64(d, 0);
            nvev_lock();
            for (i = 0; i < 64; i++)
                if (mask >> i & 1) nvev_disarm(fd, i);
            nvev_unlock();
            return 0;
        }
        default: WARN_LIMIT(16, "nvdrv: nvhost-ctrl ioctl %08x not implemented\n", cmd); return 0;
    }
}

/* /dev/nvhost-ctrl-gpu: what a Tegra X1's GM20B reports, values from Eden's nvhost_ctrl_gpu */
static void gpu_characteristics(u8* c) { /* 0xA0 bytes */
    static const u32 words[] = {
        0x120,      0xB,     0xA1,    1,       /* arch GM200, impl GM20B, rev A1, GPCs */
        0x40000,    0,       0,       0,       /* L2 cache size, on-board video memory (u64s) */
        2,          0x20,    0x20000, 0x20000, /* TPCs per GPC, bus AXI, big page size, compression page size */
        0x1B,       0x30000, 1,       0x503,   /* PDE coverage bits, available big page sizes, GPC mask, SM version */
        0x503,      0x80,    0x28,    0,       /* SPA version, warp count, GPU VA bits, reserved */
        0x55,       0,                         /* flags (u64) */
        0x902D,     0xB197,  0xB1C0,  0xB06F,  0xA140, 0xB0B5, /* 2D, 3D, compute, GPFIFO, I2M and DMA classes */
        1,          0,       2,       1,                       /* FBPs, FBP enable mask, LTCs per FBP, LTSs per LTC */
        0,          1,       0x21D70, 0,                       /* textures per TPC, GPCs, ROP L2 enable masks */
        0x30326D67, 0x62,                                      /* the chip's name, "gm20b" (u64) */
        0,          0                                          /* compression bit store base (u64) */
    };
    for (u32 i = 0; i < 0xA0 / 4; i++) wr32(c, i * 4, words[i]); /* a u64's halves are low then high, as stored */
}
static u32 ioctl_ctrlgpu(u32 cmd, u8* d, u32 n, u8* extra, u32 nextra) {
    static const u8 sms[4] = {0, 0, 0, 1}; /* {gpc, tpc} of each of the two SMs */
    (void)n;
    switch (cmd & 0xFF) {
        case 0x01: wr32(d, 0, 1); return 0; /* ZCULL_GET_CTX_SIZE */
        case 0x02: {                        /* ZCULL_GET_INFO */
            static const u32 info[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
            for (u32 i = 0; i < 10; i++) wr32(d, i * 4, info[i]);
            return 0;
        }
        case 0x03: /* ZBC_SET_TABLE */
        case 0x04: /* ZBC_QUERY_TABLE */ return 0;
        case 0x05: { /* GET_CHARACTERISTICS {u64 buf_size, u64 buf_addr (left alone); 0xA0 bytes at 16} */
            u8 c[0xA0];
            gpu_characteristics(c);
            wr64(d, 0, sizeof c);
            memcpy(d + 16, c, sizeof c);
            copy_extra(extra, nextra, c, sizeof c);
            return 0;
        }
        case 0x06: { /* GET_TPC_MASKS {u32 mask_buf_size, pad, u64 mask_buf_addr; u32 mask at 16} */
            wr32(d, 16, 3);
            if (nextra >= 4) copy_extra(extra, nextra, d + 16, 4);
            return 0;
        }
        case 0x13: /* NUM_VSMS {u32 num_vsms, u32 reserved} */
            wr32(d, 0, 2);
            wr32(d, 4, 0);
            copy_extra(extra, nextra, sms, sizeof sms);
            return 0;
        case 0x14:
            if (cmd >> 30 == 3) { /* VSMS_MAPPING (0xC0044714): in the ioctl's own buffer */
                memcpy(d, sms, sizeof sms);
                copy_extra(extra, nextra, sms, sizeof sms);
            } else { /* ZBC_GET_ACTIVE_SLOT_MASK {u32 slot, u32 mask} */
                wr32(d, 0, 7);
                wr32(d, 4, 1);
            }
            return 0;
        case 0x15: wr32(d, 0, 100); return 0;        /* PMU_GET_GPU_LOAD, as Eden answers */
        case 0x1C: wr64(d, 0, guest_ns()); return 0; /* GET_GPU_TIME, the system tick's timebase */
        default: WARN_LIMIT(16, "nvdrv: nvhost-ctrl-gpu ioctl %08x not implemented\n", cmd); return 0;
    }
}

/* /dev/nvhost-gpu: a GPU channel. SUBMIT_GPFIFO {u64 gpfifo, u32 num_entries, u32 flags, NvFence {u32 id, value}},
   its entries after the header (0x08) or in the second input (0x1B, Ioctl2). The GPU thread runs them: the ioctl
   never waits. Flags: 1 wait for the fence first, 2 the host increments twice after them, 0x100 the entries
   themselves increment value times */
static u32 gpu_submit_gpfifo(NvFile* f, u32 cmd, u8* d, u32 n, const u8* extra, u32 nextra) {
    u32 num = rd32(d, 8), flags = rd32(d, 12), fid = rd32(d, 16), fval = rd32(d, 20);
    u32 room = (cmd & 0xFF) == 0x1B ? (extra ? nextra / 8 : 0) : (n > 24 ? (n - 24) / 8 : 0);
    const u8* entries = (cmd & 0xFF) == 0x1B ? extra : d + 24;
    u32 sp, host = 0, incs, wait_id = ~0u, promised;
    if (num > room) {
        WARN_LIMIT(8, "nvdrv: SUBMIT_GPFIFO of %u entries in a buffer of %u\n", num, room);
        num = room;
    }
    if ((flags & 1) && fid != ~0u) wait_id = fid;
    if (flags & 2) host = 2;
    /* the fence promises every increment: with 0x100 the entries make fval of them themselves, and the job adds only
       the host's after them, as Ryujinx's SubmitGpfifo does. Adding fval there too ran the syncpoint ahead of every
       later fence, so NVN's waits returned at once and a game's main thread spun on the semaphores behind them */
    incs = host + ((flags & 0x100) ? fval : 0);
    sp = gpu_channel_syncpt(f);
    /* the promise and the queueing in one hold, so promises reach the GPU thread in order */
    pm_lock(&g_hle.nvdrv.submit_lk);
    promised = syncpt_promise(sp, incs);
    gpu_submit(entries, num, sp, host, wait_id, fval, promised);
    pm_unlock(&g_hle.nvdrv.submit_lk);
    wr32(d, 16, sp);
    wr32(d, 20, promised);
    return 0;
}
static u32 ioctl_gpu(NvFile* f, u32 cmd, u8* d, u32 n, u8* extra, u32 nextra) {
    switch (cmd & 0xFF) {
        case 0x01: /* SET_NVMAP_FD */
        case 0x03: /* SET_TIMEOUT */
        case 0x0A: /* SET_PRIORITY */
        case 0x0B: /* SET_TIMESLICE */
        case 0x0C: /* GET_ERROR_INFO (nothing went wrong) */
        case 0x0D: /* ZCULL_BIND */
        case 0x17: /* SET_ERROR_NOTIFIER */
        case 0x1D: /* SET_TIMEOUT_EX */ return 0;
        case 0x08:
        case 0x1B: return gpu_submit_gpfifo(f, cmd, d, n, extra, nextra);
        case 0x09: /* ALLOC_OBJ_CTX {u32 class_num, u32 flags; out u64 obj_id}: the class names the object */
            wr64(d, 8, rd32(d, 0));
            return 0;
        case 0x14: /* SET_USER_DATA {u64} */
            pm_lock(&g_hle.nvdrv.lock);
            f->user_data = rd64(d, 0);
            pm_unlock(&g_hle.nvdrv.lock);
            return 0;
        case 0x15: /* GET_USER_DATA {out u64} */
            pm_lock(&g_hle.nvdrv.lock);
            wr64(d, 0, f->user_data);
            pm_unlock(&g_hle.nvdrv.lock);
            return 0;
        case 0x16: memset(d, 0, n); return 0; /* GET_ERROR_NOTIFICATION: none */
        case 0x1A: { /* ALLOC_GPFIFO_EX2 {u32 num_entries, num_jobs, flags; NvFence fence at 12; u32 reserved[3]} */
            u32 sp = gpu_channel_syncpt(f);
            wr32(d, 12, sp);
            wr32(d, 16, syncpt_read(sp));
            return 0;
        }
        default: WARN_LIMIT(16, "nvdrv: nvhost-gpu ioctl %08x not implemented\n", cmd); return 0;
    }
}

static int nns_nvdrv_INvDrvServices_Open(SvcObj* self, IpcBuf path, u32* fd, u32* error) {
    static const struct {
        const char* path;
        int dev;
    } devices[] = {{"/dev/nvmap", DEV_NVMAP},      {"/dev/nvhost-as-gpu", DEV_AS},
                   {"/dev/nvhost-ctrl", DEV_CTRL}, {"/dev/nvhost-ctrl-gpu", DEV_CTRLGPU},
                   {"/dev/nvhost-gpu", DEV_GPU},   {"/dev/nvhost-nvdec", DEV_NVDEC},
                   {"/dev/nvhost-vic", DEV_VIC}};
    char name[64];
    int dev = DEV_OTHER; /* opens all the same; its ioctls warn */
    size_t k;
    memset(name, 0, sizeof name);
    if (path.addr) memcpy(name, GP(path.addr), path.size < sizeof name - 1 ? (size_t)path.size : sizeof name - 1);
    for (k = 0; k < sizeof devices / sizeof devices[0]; k++)
        if (!strcmp(name, devices[k].path)) dev = devices[k].dev;
    /* an NVDEC or VIC fd is a host1x channel of its own, with its registers, syncpoints and decoder */
    int h1x_dev = dev == DEV_NVDEC || dev == DEV_VIC;
    H1Ch* h1x = h1x_dev ? host1x_open(dev == DEV_VIC) : NULL;
    int slot = -1;
    pm_lock(&g_hle.nvdrv.lock);
    for (int i = 1; i < 256 && (h1x || !h1x_dev); i++)
        if (!g_hle.nvdrv.nvf[i].dev) {
            slot = i;
            memset(&g_hle.nvdrv.nvf[i], 0, sizeof g_hle.nvdrv.nvf[i]);
            g_hle.nvdrv.nvf[i].dev = dev;
            g_hle.nvdrv.nvf[i].h1x = h1x;
            break;
        }
    pm_unlock(&g_hle.nvdrv.lock);
    if (slot < 0) host1x_close(h1x);
    LOG("nvdrv: open %s -> %d\n", name, slot);
    *fd = (u32)slot;
    *error = slot < 0 ? 0x30003u : 0; /* FileOperationFailed when every fd is taken (or no memory) */
    R_SUCCEED();
}
/* what goes with a closed fd: the channel and its syncpoint once the GPU thread has run the jobs queued on it (freed
   now, the id could go to a new channel those jobs would then increment), an NVDEC or VIC channel with its decoder
   and syncpoints, and the fd's references to its events */
static void nv_release(NvFile* old, NvEvent* slots) {
    if (old->dev == DEV_GPU && old->syncpt)
        gpu_call(gpu_channel_retire, (void*)(uintptr_t)old->syncpt, &old->syncpt, 1);
    host1x_close(old->h1x);
    for (int k = 0; k < 4; k++)
        if (old->ev[k]) kobj_unref(kobj_at(old->ev[k]));
    for (int k = 0; slots && k < 64; k++) /* a ctrl fd's events: the game's handles keep them as long as they need */
        if (slots[k].ev) kobj_unref(kobj_at(slots[k].ev));
}
/* Ioctl, Ioctl2 (extra is a second input) and Ioctl3 (extra is a second output): the ioctl's in and out buffers go
   through tmp, the extra one straight to the device. A GPU channel takes Ioctl2's and Ioctl3's, as-gpu and ctrl-gpu
   only Ioctl3's. ipc is the command, for the log */
#define NV_SCRATCH 0x4000
/* the (device, ioctl) pairs logged already: each is logged on its first use. The size field (bits 16-29) is left out
   of the pair: a channel's submit carries its entry count there, and every count was logged as a new ioctl. With the
   table full, nothing new is logged: it said "first use" for every pair it couldn't hold, which logged nearly every
   submit (a lock and a log write a frame in one game) */
static int nv_first_use(int dev, u32 cmd) {
    HleNvdrv* nv = &g_hle.nvdrv;
    u32 i;
    int first = 1;
    cmd &= ~(0x3FFFu << 16);
    pm_lock(&nv->seen_lock);
    for (i = 0; i < nv->nseen && first; i++)
        if (nv->seen[i].dev == dev && nv->seen[i].cmd == cmd) first = 0;
    if (first) {
        if (nv->nseen < sizeof nv->seen / sizeof nv->seen[0]) {
            nv->seen[nv->nseen].dev = dev;
            nv->seen[nv->nseen].cmd = cmd;
            nv->nseen++;
        } else
            first = 0;
    }
    pm_unlock(&nv->seen_lock);
    return first;
}
/* a guest buffer, or none when it lies outside guest memory */
static u8* nv_buffer(IpcBuf b) { return b.addr && b.size <= AS_SIZE && b.addr <= AS_SIZE - b.size ? GP(b.addr) : NULL; }
static u32 nv_ioctl(u32 fd, u32 cmd, IpcBuf in, IpcBuf out, IpcBuf extra, int extra_out, u32 ipc) {
    u8 d[NV_SCRATCH];
    u8 *src = nv_buffer(in), *dst = nv_buffer(out), *x = NULL;
    u64 nin = src ? in.size : 0, nout = dst ? out.size : 0, n;
    u32 nx = 0, r;
    NvFile* f;
    NvFile old;
    NvEvent slot_copy[64], *slots = NULL;
    int dev, release = 0;
    pm_lock(&g_hle.nvdrv.lock);
    if (fd >= 256 || !g_hle.nvdrv.nvf[fd].dev || g_hle.nvdrv.nvf[fd].closed) {
        pm_unlock(&g_hle.nvdrv.lock);
        return 8;
    }
    f = &g_hle.nvdrv.nvf[fd];
    f->users++; /* the entry and its channel stay until this ioctl leaves (Close) */
    dev = f->dev;
    pm_unlock(&g_hle.nvdrv.lock);
    if (nv_first_use(dev, cmd))
        LOG("nvdrv: ioctl %08x on device %d (command %u), buffers %llx/%llx/%llx\n", cmd, dev, ipc,
            (unsigned long long)in.size, (unsigned long long)out.size, (unsigned long long)extra.size);
    memset(d, 0, sizeof d);
    if (nin > sizeof d) nin = sizeof d;
    if (nout > sizeof d) nout = sizeof d;
    if (nin) memcpy(d, src, (size_t)nin);
    /* the size the command names, or the larger buffer's without one. as, gpu and the host1x channels read past it,
       up to the input */
    n = (cmd >> 16) & 0x3FFF;
    if (!n) n = in.size > out.size ? in.size : out.size;
    if (dev == DEV_AS || dev == DEV_GPU || dev == DEV_NVDEC || dev == DEV_VIC) n = n > in.size ? n : in.size;
    if (n > sizeof d) n = sizeof d;
    /* Ioctl2's second buffer is an input, Ioctl3's an output: a GPU channel takes both, as-gpu and ctrl-gpu Ioctl3's */
    if (dev == DEV_GPU || (extra_out && (dev == DEV_AS || dev == DEV_CTRLGPU))) {
        x = nv_buffer(extra);
        nx = x ? (extra.size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)extra.size) : 0;
    }
    switch (dev) {
        case DEV_NVMAP: r = ioctl_nvmap(cmd, d, (u32)n); break;
        case DEV_AS:
            pm_lock(&g_hle.nvdrv.aslock);
            r = ioctl_as(cmd, d, (u32)n, x, nx);
            pm_unlock(&g_hle.nvdrv.aslock);
            break;
        case DEV_CTRL: r = ioctl_ctrl(fd, cmd, d, (u32)n); break;
        case DEV_CTRLGPU: r = ioctl_ctrlgpu(cmd, d, (u32)n, x, nx); break;
        case DEV_GPU: r = ioctl_gpu(f, cmd, d, (u32)n, x, nx); break;
        case DEV_NVDEC:
        case DEV_VIC: r = host1x_ioctl(f->h1x, cmd, d, (u32)n); break;
        default:
            WARN_LIMIT(16, "nvdrv: ioctl %08x on a device that has none\n", cmd);
            r = 0;
            break;
    }
    if (nout) memcpy(dst, d, (size_t)nout);
    pm_lock(&g_hle.nvdrv.lock);
    if (!--f->users && f->closed) { /* closed meanwhile: this was the last one using it */
        old = *f;
        memset(f, 0, sizeof *f);
        slots = nvev_detach(fd, slot_copy);
        release = 1;
    }
    pm_unlock(&g_hle.nvdrv.lock);
    if (release) nv_release(&old, slots);
    return r;
}
static int nns_nvdrv_INvDrvServices_Ioctl(SvcObj* self, u32 fd, u32 request, IpcBuf in, IpcBuf out, u32* error) {
    IpcBuf none = {0, 0, 0};
    *error = nv_ioctl(fd, request, in, out, none, 0, 1);
    R_SUCCEED();
}
static int nns_nvdrv_INvDrvServices_Ioctl2(SvcObj* self, u32 fd, u32 request, IpcBuf in, IpcBuf inline_in, IpcBuf out,
                                           u32* error) {
    *error = nv_ioctl(fd, request, in, out, inline_in, 0, 11);
    R_SUCCEED();
}
static int nns_nvdrv_INvDrvServices_Ioctl3(SvcObj* self, u32 fd, u32 request, IpcBuf in, IpcBuf out, IpcBuf inline_out,
                                           u32* error) {
    *error = nv_ioctl(fd, request, in, out, inline_out, 1, 12);
    R_SUCCEED();
}
static int nns_nvdrv_INvDrvServices_Close(SvcObj* self, u32 fd, u32* error) {
    NvFile old;
    NvEvent slot_copy[64], *slots = NULL;
    int release = 0, open = 0;
    pm_lock(&g_hle.nvdrv.lock);
    if (fd < 256 && g_hle.nvdrv.nvf[fd].dev && !g_hle.nvdrv.nvf[fd].closed) {
        open = 1;
        if (g_hle.nvdrv.nvf[fd].users) { /* the last ioctl releases it */
            g_hle.nvdrv.nvf[fd].closed = 1;
        } else {
            old = g_hle.nvdrv.nvf[fd];
            memset(&g_hle.nvdrv.nvf[fd], 0, sizeof g_hle.nvdrv.nvf[fd]);
            slots = nvev_detach(fd, slot_copy);
            release = 1;
        }
    }
    pm_unlock(&g_hle.nvdrv.lock);
    if (release) nv_release(&old, slots);
    *error = open ? 0 : 8; /* an fd that isn't open, or closed already: the NvError the ioctls answer for it */
    R_SUCCEED();
}
/* the ctrl fd's ids name event slots, a channel's ids its own events */
static int nns_nvdrv_INvDrvServices_QueryEvent(SvcObj* self, u32 fd, u32 event_id, u32* error, u32* event) {
    KEvent* e = NULL;
    int held = 0;
    pm_lock(&g_hle.nvdrv.lock);
    int dev = fd < 256 && !g_hle.nvdrv.nvf[fd].closed ? g_hle.nvdrv.nvf[fd].dev : DEV_NONE;
    if (dev != DEV_NONE && dev != DEV_CTRL && event_id >= 1 && event_id <= 3) {
        if (!g_hle.nvdrv.nvf[fd].ev[event_id]) {
            KEvent* k = kevent_new("nv channel event");
            kobj_ref(&k->o); /* the fd's reference, dropped on close */
            g_hle.nvdrv.nvf[fd].ev[event_id] = kobj_id(&k->o);
        }
        e = kev(g_hle.nvdrv.nvf[fd].ev[event_id]);
        kobj_ref(&e->o); /* a Close before h_new would drop the last reference */
        held = 1;
    }
    if (!e && dev == DEV_CTRL && (((event_id >> 28) & 0xF) == 1 || event_id < 64)) {
        nvev_lock(); /* with nv.lock held: a Close meanwhile would leave the fd's new table behind */
        e = nvev_get(fd, event_id & 63);
        if (e) {
            kobj_ref(&e->o); /* the fd's reference goes when it closes, which can come before h_new */
            held = 1;
        }
        nvev_unlock();
    }
    pm_unlock(&g_hle.nvdrv.lock);
    LOG_LIMIT(8, "nvdrv: QueryEvent fd %u (device %d) id %x\n", fd, dev, event_id);
    if (dev == DEV_NONE) { /* an fd that isn't open: the NvError the ioctls answer for it, and no handle to leak */
        *event = 0;
        *error = 8;
        R_SUCCEED();
    }
    if (!e) e = kevent_new("nv query");
    *event = h_new(&e->o);
    if (held) kobj_unref(&e->o);
    *error = 0;
    R_SUCCEED();
}
static SvcObj* new_nvdrv(void) { return nns_nvdrv_INvDrvServices_new(); }

void nv_init(void) {
    g_hle.nvdrv.nvh_next = 1;
    g_hle.nvdrv.big_page_bits = 16;
    pm_init(&g_hle.nvdrv.lock);
    pm_init(&g_hle.nvdrv.aslock);
    pm_init(&g_hle.nvdrv.alias_lock);
    pm_init(&g_hle.nvdrv.nvev_lk);
    pm_init(&g_hle.nvdrv.sync_lk);
    pm_init(&g_hle.nvdrv.submit_lk);
    pc_init(&g_hle.nvdrv.sync_cv);
    nns_nvdrv_INvDrvServices_register(new_nvdrv);
}
