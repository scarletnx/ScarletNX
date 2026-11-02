/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/nvdrv/devices/nvmap.cpp, nvhost_as_gpu.cpp, nvhost_ctrl.cpp,
   nvhost_ctrl_gpu.cpp, nvhost_gpu.cpp, nvhost_tsg_gpu.cpp, native_gpu_context.h,
   src/core/hle/service/nvdrv/core/syncpoint_manager.h,
   src/video_core/memory_manager.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
   SPDX-FileCopyrightText: 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: 2021 Skyline Team and Contributors
   SPDX-FileCopyrightText: 2022 yuzu Emulator Project
   SPDX-FileCopyrightText: 2022 Skyline Team and Contributors */
/* nvdrv's devices, as nvservices serves them to the game's NVIDIA libraries (NVN, NvRm): nvmap's handles to guest
   memory, the GPU's address space and its page table (read by the GPU thread and the renderer through
   gmmu_translate and gmmu_run), nvhost-ctrl's syncpoints and event slots, ctrl-gpu's description of the GM20B, the GPU
   channels (whose GPFIFO submissions go to gpu.c's queue, never run in the ioctl), TSG channel groups, and the NVDEC
   and VIC fds that gpu/video/host1x.c serves. Every syncpoint user, here and in host1x, takes its ids from
   syncpt_alloc. The ioctl numbers and argument layouts are in the device headers beside this file (nvdrv_*.h) */
#include "nx/hle/hle.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/hle/services/nvdrv_ioctl.h"
#include "nx/hle/services/nvdrv_nvmap.h"
#include "nx/hle/services/nvdrv_as_gpu.h"
#include "nx/hle/services/nvdrv_ctrl.h"
#include "nx/hle/services/nvdrv_ctrl_gpu.h"
#include "nx/hle/services/nvdrv_gpu.h"
#include "nx/hle/services/nvdrv_tsg.h"
#include <stddef.h>
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
static void nv_cache_allocation(u32 id);

/* host1x (nvmap_get) and vi (nvmap_addr_by_id) read handles on the game's other threads while they create, alloc
   and free them, so under the lock, the address and the size in one hold: two would let them describe two handles */
int nvmap_get(u32 h, u64* addr, u32* size) {
    pm_lock(&g_host.nvdrv.lock);
    int alive = h && h < NVMAP_HANDLES && g_hle.nvdrv.nvh[h].alive && g_hle.nvdrv.nvh[h].allocated;
    *addr = alive ? g_hle.nvdrv.nvh[h].addr : 0;
    *size = alive ? g_hle.nvdrv.nvh[h].size : 0;
    pm_unlock(&g_host.nvdrv.lock);
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
    pm_lock(&g_host.nvdrv.lock);
    u32 h = nvmap_from_id(id);
    u64 a = h && g_hle.nvdrv.nvh[h].allocated ? g_hle.nvdrv.nvh[h].addr : 0;
    pm_unlock(&g_host.nvdrv.lock);
    return a;
}

/* The GPU's 40-bit address space: a two-level table from GPU page to guest page (the geometry is nvdrv.h's GMMU_*).
   The GPU thread reads it while ioctl_as changes it on others: a second-level table is installed with a CAS and loaded
   with acquire, entries are relaxed atomics */
/* the first-level index of a GPU page. Bits past the 40 the table covers are dropped, as the GPU's MMU does */
static u32 gmmu_l1_index(u64 page) { return (u32)(page >> GMMU_L2_BITS) & (NV_GMMU_L1 - 1); }
/* a GPU page's entry within its second-level table */
static u32 gmmu_l2_index(u64 page) { return (u32)(page & (GMMU_L2_ENTRIES - 1)); }
/* A page entry: the guest address of the page (bits 12 up, page aligned), a run hint in bits 1 to 11, and bit 0 set
   when the page is mapped; 0 when it isn't. The hint counts the pages after the entry, in its table, that are mapped
   and follow on in guest memory: a lower bound, so gmmu_run steps over a run of them in one load instead of one per
   page. A map writes its pages from the last, each counting those after it already written, and a change first
   shortens the counts before it that reach into it (gmmu_clip). The hint has the bits between the valid bit and the
   page address, so it saturates at 2047 */
#define GMMU_PTE_VALID      1ull
#define GMMU_PTE_HINT_SHIFT 1
#define GMMU_PTE_ADDR_MASK  (~GMMU_PAGE_MASK)
#define GHINT_MAX           ((1u << (GMMU_PAGE_BITS - GMMU_PTE_HINT_SHIFT)) - 1)
#define GHINT_MASK          ((u64)GHINT_MAX << GMMU_PTE_HINT_SHIFT)
static u64 gmmu_pte_hint(u64 e) { return (e & GHINT_MASK) >> GMMU_PTE_HINT_SHIFT; }
/* the guest address of a GPU address, 0 when its page isn't mapped. On the GPU thread, while ioctl_as maps and unmaps
   on others */
u64 gmmu_translate(u64 gva) {
    u64 page = gva >> GMMU_PAGE_BITS;
    u64* t = __atomic_load_n(&g_hle.nvdrv.gmmu[gmmu_l1_index(page)], __ATOMIC_ACQUIRE);
    u64 e;
    if (!t) return 0;
    e = __atomic_load_n(&t[gmmu_l2_index(page)], __ATOMIC_RELAXED);
    if (!(e & GMMU_PTE_VALID)) return 0;
    e = (e & GMMU_PTE_ADDR_MASK) | (gva & GMMU_PAGE_MASK);
    return mem_mapped_run(e, 1) ? e : 0; /* a page the guest freed after mapping it here: as unmapped */
}
/* the bytes from gva on, at most max, whose pages map to consecutive guest pages, and in *cpu the guest address of gva:
   0 when gva's page isn't mapped. One walk of the table for a range, where gmmu_translate per page walks it again */
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    u64 page = gva >> GMMU_PAGE_BITS, n, e, want;
    u64* t = __atomic_load_n(&g_hle.nvdrv.gmmu[gmmu_l1_index(page)], __ATOMIC_ACQUIRE);
    if (!max || !t) return 0;
    e = __atomic_load_n(&t[gmmu_l2_index(page)], __ATOMIC_RELAXED);
    if (!(e & GMMU_PTE_VALID)) return 0;
    *cpu = (e & GMMU_PTE_ADDR_MASK) | (gva & GMMU_PAGE_MASK);
    n = GMMU_PAGE_SIZE - (gva & GMMU_PAGE_MASK);
    want = (e & GMMU_PTE_ADDR_MASK) + GMMU_PAGE_SIZE;
    while (n < max) {
        u64 h = gmmu_pte_hint(e);
        if (h) { /* the next h pages follow on: to the last of them, whose entry tells what follows it */
            page += h;
            n += h << GMMU_PAGE_BITS;
            want += h << GMMU_PAGE_BITS;
            if (n >= max) break;
            e = __atomic_load_n(&t[gmmu_l2_index(page)], __ATOMIC_RELAXED);
            continue;
        }
        page++;
        if (!gmmu_l2_index(page)) /* into the next second-level table */
            t = __atomic_load_n(&g_hle.nvdrv.gmmu[gmmu_l1_index(page)], __ATOMIC_ACQUIRE);
        if (!t) break;
        e = __atomic_load_n(&t[gmmu_l2_index(page)], __ATOMIC_RELAXED);
        if (!(e & GMMU_PTE_VALID) || (e & GMMU_PTE_ADDR_MASK) != want) break;
        n += GMMU_PAGE_SIZE;
        want += GMMU_PAGE_SIZE;
    }
    /* a game's GPU mappings can outlive the guest memory behind them, which has no host memory left: the run stops
       there, as at a page with no entry */
    return mem_mapped_run(*cpu, n < max ? n : max);
}
static u64* gmmu_l2(u32 i) {
    u64* t = __atomic_load_n(&g_hle.nvdrv.gmmu[i], __ATOMIC_ACQUIRE);
    if (t) return t;
    u64* n = calloc(GMMU_L2_ENTRIES, sizeof *n);
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
        if ((q ^ first) >> GMMU_L2_BITS) break; /* counts stay in their table */
        t = __atomic_load_n(&g_hle.nvdrv.gmmu[gmmu_l1_index(q)], __ATOMIC_ACQUIRE);
        if (!t) break;
        e = __atomic_load_n(&t[gmmu_l2_index(q)], __ATOMIC_RELAXED);
        if (gmmu_pte_hint(e) >= k)
            __atomic_store_n(&t[gmmu_l2_index(q)], (e & ~GHINT_MASK) | ((k - 1) << GMMU_PTE_HINT_SHIFT),
                             __ATOMIC_RELAXED);
    }
}
/* ---- the page table again as runs, and their index by guest page, for gmmu_aliases. Mappings change a few times a
   frame at most, while the renderer asks, hundreds of thousands of times a frame, about GPU ranges where results it
   keeps per page may have been written through another GPU address: the index lets it record those once, at every
   alias, when a result is made (vk/pending_pages.c, maxwell/maxwell3d.c) instead of translating each range it asks
   about. The runs follow the table's page numbers, which wrap at GMMU_PAGES as gmmu_l1_index drops the bits past
   them */
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
    pm_lock(&g_host.nvdrv.alias_lock);
    for (;;) {
        u64 g = first & (GMMU_PAGES - 1), n = last - first + 1;
        if (n > GMMU_PAGES - g) n = GMMU_PAGES - g; /* the rest wraps to page 0 */
        if (map) {
            u32 k;
            grun_cut(g, g + n, 0, 0);
            grun_room(1);
            k = grun_find(g);
            memmove(&g_hle.nvdrv.gruns[k + 1], &g_hle.nvdrv.gruns[k],
                    (g_hle.nvdrv.ngruns - k) * sizeof *g_hle.nvdrv.gruns);
            g_hle.nvdrv.gruns[k].gpage = g;
            g_hle.nvdrv.gruns[k].pages = n;
            g_hle.nvdrv.gruns[k].cpage = cpu >> GMMU_PAGE_BITS;
            g_hle.nvdrv.ngruns++;
        } else if (!own || !(cpu & GHINT_MASK)) /* an own unmap compares its address's hint bits too: set, none match */
            grun_cut(g, g + n, own, cpu >> GMMU_PAGE_BITS);
        if (n >= last - first + 1) break;
        first += n;
        cpu += n << GMMU_PAGE_BITS;
    }
    g_hle.nvdrv.alias.stale = 1;
    pm_unlock(&g_host.nvdrv.alias_lock);
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
    u64 end = cpu + size < cpu ? ~(u64)0 : cpu + size, c0 = cpu >> GMMU_PAGE_BITS, c1 = (end - 1) >> GMMU_PAGE_BITS;
    u32 lo = 0, hi, i, k;
    if (!size) return;
    pm_lock(&g_host.nvdrv.alias_lock);
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
            u64 rlo = r->cpage << GMMU_PAGE_BITS, rhi = (r->cpage + r->pages) << GMMU_PAGE_BITS, a, b;
            if (x->seen[ri] == x->query) continue;
            x->seen[ri] = x->query;
            a = rlo > cpu ? rlo : cpu;
            b = rhi < end ? rhi : end;
            if (a < b) fn(ctx, (r->gpage << GMMU_PAGE_BITS) + (a - rlo), b - a);
        }
    pm_unlock(&g_host.nvdrv.alias_lock);
}
void gmmu_pieces(u64 gva, u64 size, void (*fn)(void* ctx, u64 gva, u64 cpu, u64 n), void* ctx) {
    u64 g0 = gva >> GMMU_PAGE_BITS, g1;
    if (!size) return;
    g1 = ((gva + size - 1) >> GMMU_PAGE_BITS) + 1;
    pm_lock(&g_host.nvdrv.alias_lock);
    for (u32 k = grun_find(g0); k < g_hle.nvdrv.ngruns && g_hle.nvdrv.gruns[k].gpage < g1; k++) {
        const GRun* r = &g_hle.nvdrv.gruns[k];
        u64 a = r->gpage > g0 ? r->gpage : g0, b = r->gpage + r->pages < g1 ? r->gpage + r->pages : g1;
        fn(ctx, a << GMMU_PAGE_BITS, (r->cpage + (a - r->gpage)) << GMMU_PAGE_BITS, (b - a) << GMMU_PAGE_BITS);
    }
    pm_unlock(&g_host.nvdrv.alias_lock);
}
void gmmu_map(u64 gva, u64 cpu, u64 size) {
    u64 first, last, p;
    if (!size) return;
    gmmu_change_begin(gva, size);
    first = gva >> GMMU_PAGE_BITS;
    last = (gva + size - 1) >> GMMU_PAGE_BITS;
    gmmu_clip(first);
    for (p = last;; p--) { /* from the last, so a page's count covers only pages already written */
        u64* t = gmmu_l2(gmmu_l1_index(p));
        u64 h = last - p, room = (GMMU_L2_ENTRIES - 1) - gmmu_l2_index(p); /* counts stay in their table */
        u64 e;
        if (h > room) h = room;
        if (h > GHINT_MAX) h = GHINT_MAX;
        e = (cpu + ((p - first) << GMMU_PAGE_BITS)) | (h << GMMU_PTE_HINT_SHIFT) | GMMU_PTE_VALID;
        __atomic_store_n(&t[gmmu_l2_index(p)], e, __ATOMIC_RELAXED);
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
    first = gva >> GMMU_PAGE_BITS;
    last = (gva + size - 1) >> GMMU_PAGE_BITS;
    /* no count may reach a page about to be cleared: the ones before the range stop short of it, and the pages in it
       that an own unmap keeps lose theirs */
    gmmu_clip(first);
    for (p = first; p <= last; p++) {
        u64* t = __atomic_load_n(&g_hle.nvdrv.gmmu[gmmu_l1_index(p)], __ATOMIC_ACQUIRE);
        u64 e;
        if (!t) {
            p |= GMMU_L2_ENTRIES - 1;
            continue;
        }
        e = __atomic_load_n(&t[gmmu_l2_index(p)], __ATOMIC_RELAXED);
        if (e & GHINT_MASK) __atomic_store_n(&t[gmmu_l2_index(p)], e & ~GHINT_MASK, __ATOMIC_RELAXED);
    }
    for (p = first; p <= last; p++) {
        u64* t = __atomic_load_n(&g_hle.nvdrv.gmmu[gmmu_l1_index(p)], __ATOMIC_ACQUIRE);
        u64* e;
        if (!t) { /* nothing was ever mapped in this table's range: skip to the next table */
            p |= GMMU_L2_ENTRIES - 1;
            continue;
        }
        e = &t[gmmu_l2_index(p)];
        if (own && (__atomic_load_n(e, __ATOMIC_RELAXED) & ~GHINT_MASK) !=
                       ((cpu + ((p - first) << GMMU_PAGE_BITS)) | GMMU_PTE_VALID))
            continue;
        __atomic_store_n(e, 0, __ATOMIC_RELAXED);
    }
    grun_change(first, last, 0, own, cpu);
    gmmu_change_end(gva, size);
}
/* A GPU mapping of a handle's memory pins it, as the console's nvmap does: the handle object lives until the mapping
   goes, even when FREE has given back the game's reference, and FREE reports no memory meanwhile. A pin is one piece
   of a mapping, since a split keeps a piece of its own; every pin's piece unpins it in turn */
static void nvmap_retire(u32 h);

static void nvmap_pin(u32 h) {
    if (!h || h >= NVMAP_HANDLES) return;
    pm_lock(&g_host.nvdrv.lock);
    if (g_hle.nvdrv.nvh[h].alive) g_hle.nvdrv.nvh[h].pins++;
    pm_unlock(&g_host.nvdrv.lock);
}
static void nvmap_unpin(u32 h) {
    int retire = 0;
    if (!h || h >= NVMAP_HANDLES) return;
    pm_lock(&g_host.nvdrv.lock);
    if (g_hle.nvdrv.nvh[h].alive && g_hle.nvdrv.nvh[h].pins) {
        if (!--g_hle.nvdrv.nvh[h].pins && !g_hle.nvdrv.nvh[h].refs) {
            g_hle.nvdrv.nvh[h].alive = g_hle.nvdrv.nvh[h].allocated = 0;
            retire = 1;
        }
    }
    pm_unlock(&g_host.nvdrv.lock);
    if (retire) nvmap_retire(h);
}
static GMap* gmap_add(void) {
    if (g_hle.nvdrv.ngmaps == g_hle.nvdrv.capgmaps) {
        u32 nc = g_hle.nvdrv.capgmaps ? g_hle.nvdrv.capgmaps * 2 : 1024;
        GMap* g = realloc(g_hle.nvdrv.gmaps, nc * sizeof *g);
        if (!g) fatal("nvdrv: out of memory for GPU mappings\n");
        g_hle.nvdrv.gmaps = g;
        g_hle.nvdrv.capgmaps = nc;
    }
    GMap* g = &g_hle.nvdrv.gmaps[g_hle.nvdrv.ngmaps++];
    *g = (GMap){0};
    return g;
}
/* a piece of a mapping of handle h, pinned */
static GMap* gmap_new(u32 h) {
    GMap* g = gmap_add();
    g->h = h;
    nvmap_pin(h);
    return g;
}
/* takes [gva, gva+size) out of the tracked pieces, splitting a piece that straddles it: nvhost-as-gpu's with sparse 0,
   else the pieces of that sparse region */
static void gmap_cut(u64 gva, u64 size, u32 sparse) {
    u64 e = gva + size;
    for (u32 k = 0; k < g_hle.nvdrv.ngmaps;) {
        GMap* g = &g_hle.nvdrv.gmaps[k];
        u64 ge = g->gva + g->size;
        if ((sparse ? g->sparse != sparse : g->gsv != 0) || ge <= gva || g->gva >= e) {
            k++;
        } else if (g->gva < gva && ge > e) { /* the cut is inside: keep the head, add the tail */
            GMap tail = *g;
            tail.cpu += e - g->gva;
            tail.gva = e;
            tail.size = ge - e;
            g->size = gva - g->gva;
            *gmap_new(tail.h) = tail; /* two pieces now, each with its own pin */
            k++;
        } else if (g->gva < gva) {
            g->size = gva - g->gva;
            k++;
        } else if (ge > e) {
            g->cpu += e - g->gva;
            g->size = ge - e;
            g->gva = e;
            k++;
        } else {
            u32 h = g->h;
            g_hle.nvdrv.gmaps[k] = g_hle.nvdrv.gmaps[--g_hle.nvdrv.ngmaps];
            nvmap_unpin(h);
        }
    }
}
/* GPU VA for ALLOC_SPACE and MAP_BUFFER_EX: first fit from where the big-page region starts (Eden's
   DEFAULT_VA_SPLIT, 0x04_0000_0000, MemoryManager's split_address) to the end of the 40-bit space the gmmu covers, in
   units of the smaller big page (64 KiB). Not bounded to the big page region GET_VA_REGIONS reports: a game maps more
   than its 0x1BFFF big pages (its NVN init maps 0x20000000 bytes past it) and checks nothing against the regions.
   Free ranges are kept sorted and merged */
#define VA_BASE    AS_DEFAULT_VA_SPLIT
#define VA_END     (1ull << GMMU_ADDRESS_SPACE_BITS)
#define VA_UNIT    (1ull << AS_BIG_PAGE_BITS_64K) /* the allocator's unit and least alignment */
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
    /* One page table serves both front ends. The native SDK owns its whole partition, including VA it has not
       mapped yet. Keep legacy allocations above it and below the shim's 64 KiB allocator granule. */
    if (g_hle.kernel.hw == SNX_HW_SWITCH2)
        va_insert(0, NV_GSV_USER_END, NV_SYNCPT_SHIM_GVA & ~(VA_UNIT - 1));
    else
        va_insert(0, VA_BASE, VA_END);
}
static u64 va_round(u64 size) { return (size + VA_UNIT - 1) & ~(VA_UNIT - 1); }
/* a GPU range the game names: inside the 40 bit space the page table covers. One past it would wrap the table's
   index onto low addresses */
static int gva_ok(u64 gva, u64 size) { return size <= VA_END && gva <= VA_END - size; }
/* Fixed legacy requests must not modify the native partition or the read-only syncpoint page. */
static int legacy_gva_ok(u64 gva, u64 size) {
    if (!gva_ok(gva, size)) return 0;
    if (g_hle.kernel.hw != SNX_HW_SWITCH2) return 1;
    return !(gva < NV_GSV_USER_END && gva + size > NV_GSV_USER_FIRST) &&
           !(gva < VA_END && gva + size > NV_SYNCPT_SHIM_GVA);
}
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
    u64 e = a + va_round(size);
    u64 lo = g_hle.kernel.hw == SNX_HW_SWITCH2 ? NV_GSV_USER_END : VA_BASE;
    u64 hi = g_hle.kernel.hw == SNX_HW_SWITCH2 ? NV_SYNCPT_SHIM_GVA & ~(VA_UNIT - 1) : VA_END;
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
    align = align > VA_UNIT ? align : VA_UNIT;
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

/* ---- syncpoints: host1x's counters, as Eden's two syncpoint managers split them. syncpt is the value the engines
   have reached (Eden's counter_min, host1x's guest value), syncpt_max what submissions have promised it will reach
   (counter_max): a fence is a value, reached once the counter gets there across the 32-bit wrap. syncpt_alloc and
   syncpt_free are Eden's AllocateSyncpoint and FreeSyncpoint, syncpt_promise its IncrementSyncpointMaxExt, syncpt_add
   host1x's IncrementGuest, syncpt_wait its WaitGuest */
/* the syncpoint's value (ReadSyncpointMinValue); 0 for an id past the table */
u32 syncpt_read(u32 id) { return id < SYNCPT_IDS ? __atomic_load_n(&g_hle.nvdrv.syncpt[id], __ATOMIC_SEQ_CST) : 0; }
/* the value moved and its waiters woken: 0 for no increment */
static int syncpt_bump(u32 id, u32 n) {
    if (id >= SYNCPT_IDS || !n) return 0;
    pm_lock(&g_host.nvdrv.sync_lk);
    u32 v = __atomic_add_fetch(&g_hle.nvdrv.syncpt[id], n, __ATOMIC_SEQ_CST);
    if ((i32)(v - g_hle.nvdrv.syncpt_max[id]) > 0) g_hle.nvdrv.syncpt_max[id] = v; /* the CPU promises nothing first */
    pc_broadcast(&g_host.nvdrv.sync_cv);
    pm_unlock(&g_host.nvdrv.sync_lk);
    return 1;
}
void syncpt_add(u32 id, u32 n) {
    if (!syncpt_bump(id, n)) return;
    nv_events_check();
    gpu_kick(); /* a GPU channel's stream may wait for it */
}
void syncpt_add_quiet(u32 id, u32 n) {
    if (!syncpt_bump(id, n)) return;
    t_hle.nvdrv.events_due = 1;
    gpu_kick();
}
void syncpt_events(void) {
    if (!t_hle.nvdrv.events_due) return;
    t_hle.nvdrv.events_due = 0;
    nv_events_check();
}
void syncpt_incr(u32 id) { syncpt_add(id, 1); }
/* raises the promised maximum by n, from at least the current value: what a fence handed back now will reach */
u32 syncpt_promise(u32 id, u32 n) {
    u32 cur, v;
    if (id >= SYNCPT_IDS) return 0;
    pm_lock(&g_host.nvdrv.sync_lk);
    cur = syncpt_read(id);
    if ((i32)(cur - g_hle.nvdrv.syncpt_max[id]) > 0) g_hle.nvdrv.syncpt_max[id] = cur;
    g_hle.nvdrv.syncpt_max[id] += n;
    v = g_hle.nvdrv.syncpt_max[id];
    pm_unlock(&g_host.nvdrv.sync_lk);
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
    pm_lock(&g_host.nvdrv.sync_lk);
    for (;;) {
        i64 left = -1;
        reached = syncpt_reached(id, thresh);
        if (reached) break;
        if (timeout_ns > 0) {
            u64 now = guest_ns();
            if (now >= deadline) break;
            left = (i64)(deadline - now);
        }
        kthread_wait(&g_host.nvdrv.sync_cv, &g_host.nvdrv.sync_lk, left);
    }
    pm_unlock(&g_host.nvdrv.sync_lk);
    return reached;
}
/* every syncpoint user (GPU channels here, host1x channels in host1x.c) allocates here, lowest free id first, so ids
   stay unique and below SYNCPT_N (nvdrv.h: NVIDIA's stream library sizes its per-syncpoint tables at 128, and an id
   past them writes into the game's heap). Running out is fatal, and a closed channel (GPU or host1x) gives its ids
   back */
u32 syncpt_alloc(void) {
    pm_lock(&g_host.nvdrv.sync_lk);
    for (u32 id = SYNCPT_FIRST; id < SYNCPT_N; id++)
        if (!g_hle.nvdrv.syncpt_used[id]) {
            g_hle.nvdrv.syncpt_used[id] = 1;
            /* an id used before starts with nothing promised past its value */
            g_hle.nvdrv.syncpt_max[id] = g_hle.nvdrv.syncpt[id];
            pm_unlock(&g_host.nvdrv.sync_lk);
            nv_cache_allocation(id);
            return id;
        }
    pm_unlock(&g_host.nvdrv.sync_lk);
    fatal("nvdrv: all %u syncpoints are in use (NVIDIA's stream library has room for %u ids)\n",
          SYNCPT_N - SYNCPT_FIRST, SYNCPT_N);
}
void syncpt_free(u32 id) {
    if (id >= SYNCPT_N) return;
    pm_lock(&g_host.nvdrv.sync_lk);
    g_hle.nvdrv.syncpt_used[id] = 0;
    g_hle.nvdrv.syncpt_gpu[id] = 0;
    pm_unlock(&g_host.nvdrv.sync_lk);
    nv_cache_allocation(id);
}
/* whether a present can leave a fence to the GPU thread: a GPU channel's value that submissions already promised is
   reached by that channel's jobs queued ahead of the present (a promise and its job are queued in one hold of
   submit_lk, which the caller takes after this). Any other fence (a host1x engine's, one the CPU increments, or one
   nothing promised yet) is waited for here, on the calling thread: a call on the GPU thread can't stand aside to wait
   as a stream can (gpu_park) */
#define NV_CALL_FENCES 4u             /* the fences a present carries at most (nv_gpu_call_after) */
#define NS_PER_MS      ((i64)1000000)
#define FENCE_WAIT_NS  (1000 * NS_PER_MS) /* how long a present waits for a fence on its own thread: 1 s */
static int fence_on_gpu_thread(u32 id, u32 val) {
    if (id == NV_FENCE_NO_SYNCPT) return 0;
    pm_lock(&g_host.nvdrv.sync_lk);
    int gpu = id < SYNCPT_N && g_hle.nvdrv.syncpt_gpu[id] && (i32)(val - g_hle.nvdrv.syncpt_max[id]) <= 0;
    pm_unlock(&g_host.nvdrv.sync_lk);
    if (gpu) return 1;
    if (!syncpt_wait(id, val, FENCE_WAIT_NS))
        WARN_LIMIT(
            8,
            "gpu: a present waited %u ms on its own thread for syncpoint %u to reach %u (it is at %u), and goes "
            "on\n",
            (unsigned)(FENCE_WAIT_NS / NS_PER_MS), id, val, syncpt_read(id));
    return 0;
}
void nv_gpu_call_after(u32 n, u32* id, const u32* val, void (*fn)(void*), void* arg) {
    u32 chans[NV_CALL_FENCES], nchans = 0;
    for (u32 i = 0; i < n && i < NV_CALL_FENCES; i++)
        if (fence_on_gpu_thread(id[i], val[i]))
            chans[nchans++] = id[i]; /* a GPU channel's syncpoint id is the channel */
        else
            id[i] = NV_FENCE_NO_SYNCPT; /* done with here */
    pm_lock(&g_host.nvdrv.submit_lk); /* after the submissions promising the fences left to the GPU thread */
    gpu_call(fn, arg, chans, nchans);
    pm_unlock(&g_host.nvdrv.submit_lk);
}
/* a GPU channel's syncpoint, allocated on first use. Two ioctls on the fd at once may both allocate: the one that
   stores it first under nv.lock wins, the other gives its id back (there are SYNCPT_N - SYNCPT_FIRST to hand out) */
static u32 gpu_channel_syncpt(NvFile* f) {
    pm_lock(&g_host.nvdrv.lock);
    u32 sp = f->syncpt;
    pm_unlock(&g_host.nvdrv.lock);
    if (sp) return sp;
    u32 mine = syncpt_alloc();
    pm_lock(&g_host.nvdrv.sync_lk); /* a GPU channel's before any ioctl can submit on it */
    g_hle.nvdrv.syncpt_gpu[mine] = 1;
    pm_unlock(&g_host.nvdrv.sync_lk);
    pm_lock(&g_host.nvdrv.lock);
    if (!f->syncpt) f->syncpt = mine;
    sp = f->syncpt;
    pm_unlock(&g_host.nvdrv.lock);
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

static void nvev_lock(void) { pm_lock(&g_host.nvdrv.nvev_lk); }
static void nvev_unlock(void) { pm_unlock(&g_host.nvdrv.nvev_lk); }
/* ctrl fd fd's event slot, its table made if make is set (NULL without memory, or when it isn't made). Called with the
   lock held, for an fd that stays open meanwhile */
static NvEvent* nvev_slot(u32 fd, u32 slot, int make) {
    if (!g_hle.nvdrv.ctrl_ev[fd] && make) {
        NvEventTable* t = POOL_TAKE(g_hle.nvdrv.ctrl_evt, g_hle.nvdrv.ctrl_evt_next, "nvhost-ctrl fds with events");
        t->fd = fd + 1;
        g_hle.nvdrv.ctrl_ev[fd] = POOL_INDEX(g_hle.nvdrv.ctrl_evt, t) + 1;
    }
    return g_hle.nvdrv.ctrl_ev[fd] ? &g_hle.nvdrv.ctrl_evt[g_hle.nvdrv.ctrl_ev[fd] - 1].slot[nv_ctrl_event_slot(slot)]
                                   : NULL;
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
/* the fd's slots, copied into out[NV_CTRL_EVENT_SLOTS] and their table given back when its entry is cleared (with
   nv.lock held, so a new fd with the number doesn't meet them): out, or NULL when the fd had none. nv_release drops
   them */
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
        for (i = 0; g_hle.nvdrv.ctrl_evt[t].fd && i < NV_CTRL_EVENT_SLOTS; i++) {
            NvEvent* e = &g_hle.nvdrv.ctrl_evt[t].slot[i];
            if (!e->armed || !syncpt_reached(e->id, e->thresh)) continue;
            e->armed = 0;
            if (e->ev) kevent_signal(kev(e->ev));
        }
    for (u32 mask = g_hle.nvdrv.gsv_sync_active; mask; mask &= mask - 1) {
        NvGsvSync* s = &g_hle.nvdrv.gsv_sync[snx_ctz32(mask)];
        for (i = 0; i < s->count; ++i) {
            NvEvent* e = &s->events[i];
            if (e->armed && syncpt_reached(e->id, e->thresh)) {
                e->armed = 0;
                kevent_signal(kev(e->ev));
            }
        }
    }
    nvev_unlock();
}

/* ---- the devices. Each handler gets the ioctl's argument in d (nv_ioctl's scratch buffer), and answers an
   NvResult (nvdrv_ioctl.h). nv_ioctl checks that the fd is open. Layouts are Switchbrew's ("NV services") and
   eden-ns2's, by field offset in the device headers. TSG compares full ioctl numbers after nv_ioctl checks the buffer
   sizes. The other handlers get n, the bytes they may use, and switch on the command byte alone: each device has
   one group but for a GPU channel's user data ('G' beside 'H'), and none of its commands collide */
enum { DEV_NONE, DEV_NVMAP, DEV_AS, DEV_CTRL, DEV_CTRLGPU, DEV_GPU, DEV_NVDEC, DEV_VIC, DEV_OTHER, DEV_TSG };
/* an ioctl's argument block is a copy of the guest's buffer, so its words are little-endian */
static u32 rd32(const u8* d, u32 off) { return snx_ld_le32(d + off); }
static u64 rd64(const u8* d, u32 off) { return snx_ld_le64(d + off); }
static void wr32(u8* d, u32 off, u32 v) { snx_st_le32(d + off, v); }
static void wr64(u8* d, u32 off, u64 v) { snx_st_le64(d + off, v); }
/* an ioctl's second output (Ioctl3), up to its size */
static void copy_extra(u8* extra, u32 nextra, const void* src, u32 size) {
    if (extra) memcpy(extra, src, size < nextra ? size : nextra);
}
/* a guest page's alignment, for the addresses mappings and allocations take */
static int page_aligned(u64 a) { return !(a & GMMU_PAGE_MASK); }

/* /dev/nvmap: handles to guest memory, numbered 1..NVMAP_HANDLES - 1. Every access to a handle is under nv.lock:
   host1x and vi read them on other threads */
static NvMapH* nvmap_live(u32 h) {
    return h && h < NVMAP_HANDLES && g_hle.nvdrv.nvh[h].alive ? &g_hle.nvdrv.nvh[h] : NULL;
}
/* a new handle of the size, its number its id, with one reference, 0 when none is left. A freed number goes out
   again first */
static u32 nvmap_handle_make(u32 size) {
    u32 h;
    pm_lock(&g_host.nvdrv.lock);
    h = g_hle.nvdrv.nvh_nfree                    ? g_hle.nvdrv.nvh_free[--g_hle.nvdrv.nvh_nfree]
        : g_hle.nvdrv.nvh_next < NVMAP_HANDLES ? g_hle.nvdrv.nvh_next++
                                               : 0;
    if (h) {
        NvMapH* m = &g_hle.nvdrv.nvh[h];
        memset(m, 0, sizeof *m);
        m->size = size;
        m->id = h;
        m->refs = 1;
        m->alive = 1;
    }
    pm_unlock(&g_host.nvdrv.lock);
    if (!h) WARN_LIMIT(4, "nvdrv: nvmap has no handle left for %#x bytes\n", size);
    else LOGV("nvdrv: nvmap create %#x bytes -> %u\n", size, h);
    return h;
}
static u32 nvmap_ioc_create(u8* d) {
    u32 h = nvmap_handle_make(rd32(d, NVMAP_CREATE_SIZE));
    wr32(d, NVMAP_CREATE_HANDLE, h);
    return h ? NV_RESULT_SUCCESS : NV_RESULT_INSUFFICIENT_MEMORY;
}
/* FROM_ID: another reference on the live handle with the id */
static u32 nvmap_ioc_from_id(u8* d) {
    u32 h;
    pm_lock(&g_host.nvdrv.lock);
    h = nvmap_from_id(rd32(d, NVMAP_FROM_ID_ID));
    if (h) g_hle.nvdrv.nvh[h].refs++;
    pm_unlock(&g_host.nvdrv.lock);
    if (!h) return NV_RESULT_NOT_IMPLEMENTED;
    wr32(d, NVMAP_FROM_ID_HANDLE, h);
    return NV_RESULT_SUCCESS;
}
/* ALLOC: backs the handle with the guest memory at the address, when it is page aligned and inside guest memory */
static u32 nvmap_ioc_alloc(const u8* d) {
    u64 addr = rd64(d, NVMAP_ALLOC_ADDRESS);
    u32 h = rd32(d, NVMAP_ALLOC_HANDLE), r = NV_RESULT_SUCCESS;
    NvMapH* m;
    if (!h || h >= NVMAP_HANDLES) return NV_RESULT_NOT_IMPLEMENTED;
    pm_lock(&g_host.nvdrv.lock);
    m = nvmap_live(h);
    if (!m)
        r = NV_RESULT_NOT_IMPLEMENTED;
    else if (!m->size || !page_aligned(addr) || !cpu_ok(addr, m->size))
        r = NV_RESULT_BAD_PARAMETER; /* the handle stays as it was */
    else {
        m->heap = rd32(d, NVMAP_ALLOC_HEAP_MASK);
        m->flags = rd32(d, NVMAP_ALLOC_FLAGS);
        m->align = rd32(d, NVMAP_ALLOC_ALIGN);
        m->kind = d[NVMAP_ALLOC_KIND];
        m->addr = addr;
        m->allocated = 1;
    }
    pm_unlock(&g_host.nvdrv.lock);
    if (!r) LOGV("nvdrv: nvmap alloc %u at %llx\n", h, (unsigned long long)addr);
    return r;
}
/* the last reference and pin gone: NVDEC and VIC drop what they mapped of it before the number can name another buffer */
static void nvmap_retire(u32 h) {
    host1x_forget_handle(h);
    pm_lock(&g_host.nvdrv.lock);
    g_hle.nvdrv.nvh_free[g_hle.nvdrv.nvh_nfree++] = h;
    pm_unlock(&g_host.nvdrv.lock);
}
/* FREE: drops a reference and, only when that was the last one, reports the handle's memory: a zero address says the
   handle lives on (a GPU mapping pins it, another reference, or a mapping of the memory itself), which the SDK relies
   on to know whether it may change the memory's attributes. The last reference ends the handle, and its number goes
   back once NVDEC and VIC have dropped what they mapped of it */
static u32 nvmap_ioc_free(u8* d) {
    int last = 0, retire = 0;
    u32 h = rd32(d, NVMAP_FREE_HANDLE);
    NvMapH* m;
    if (!h || h >= NVMAP_HANDLES) return NV_RESULT_NOT_IMPLEMENTED;
    pm_lock(&g_host.nvdrv.lock);
    m = nvmap_live(h);
    wr64(d, NVMAP_FREE_ADDRESS, 0);
    wr32(d, NVMAP_FREE_SIZE, 0);
    wr32(d, NVMAP_FREE_FLAGS, 0);
    if (m) {
        if (m->refs) m->refs--;
        last = !m->refs && !m->pins;
        if (last) {
            if (!svc_mem_range_locked(m->addr, m->size)) {
                wr64(d, NVMAP_FREE_ADDRESS, m->addr);
                wr32(d, NVMAP_FREE_SIZE, m->size);
                wr32(d, NVMAP_FREE_FLAGS, (m->flags & NVMAP_ALLOC_UNCACHED) ? NVMAP_FREE_WAS_UNCACHED : 0);
            }
            m->alive = m->allocated = 0;
            retire = 1;
        }
    }
    pm_unlock(&g_host.nvdrv.lock);
    if (!m) return NV_RESULT_NOT_IMPLEMENTED;
    if (retire) nvmap_retire(h);
    return NV_RESULT_SUCCESS;
}
u32 nvmap_handle_new(u64 addr, u64 size) {
    u8 d[NVMAP_ALLOC_ADDRESS + 8]; /* the largest of the arguments */
    u32 h;
    if (!size || size > 0xffffffffull || !page_aligned(addr)) return 0;
    memset(d, 0, sizeof d);
    wr32(d, NVMAP_CREATE_SIZE, (u32)size);
    if (nvmap_ioc_create(d)) return 0;
    h = rd32(d, NVMAP_CREATE_HANDLE);
    memset(d, 0, sizeof d);
    wr32(d, NVMAP_ALLOC_HANDLE, h);
    wr64(d, NVMAP_ALLOC_ADDRESS, addr);
    if (!nvmap_ioc_alloc(d)) return h;
    nvmap_handle_drop(h);
    return 0;
}
void nvmap_handle_drop(u32 h) {
    u8 d[NVMAP_FREE_FLAGS + 4];
    if (!h) return;
    memset(d, 0, sizeof d);
    wr32(d, NVMAP_FREE_HANDLE, h);
    nvmap_ioc_free(d);
}
/* PARAM: one of the handle's properties (Eden's IocParam) */
static u32 nvmap_ioc_param(u8* d) {
    u32 v = 0, r = NV_RESULT_SUCCESS;
    const NvMapH* m;
    pm_lock(&g_host.nvdrv.lock);
    m = nvmap_live(rd32(d, NVMAP_PARAM_HANDLE));
    if (!m)
        r = NV_RESULT_NOT_IMPLEMENTED;
    else
        switch (rd32(d, NVMAP_PARAM_PARAM)) {
            case NVMAP_PARAM_SIZE: v = m->size; break;
            case NVMAP_PARAM_ALIGNMENT: v = m->align ? m->align : NVMAP_DEFAULT_ALIGNMENT; break;
            case NVMAP_PARAM_BASE: v = (u32)m->addr; break; /* as Ryujinx answers it (Eden answers EINVAL) */
            case NVMAP_PARAM_HEAP: v = m->allocated ? NVMAP_HEAP_ALLOCATED : 0; break;
            case NVMAP_PARAM_KIND: v = m->kind; break;
            case NVMAP_PARAM_IS_SHARED_MEM_MAPPED: v = 0; break;
            default: r = NV_RESULT_NOT_IMPLEMENTED; break;
        }
    pm_unlock(&g_host.nvdrv.lock);
    if (!r) wr32(d, NVMAP_PARAM_RESULT, v);
    return r;
}
static u32 nvmap_ioc_get_id(u8* d) {
    const NvMapH* m;
    pm_lock(&g_host.nvdrv.lock);
    m = nvmap_live(rd32(d, NVMAP_GET_ID_HANDLE));
    if (m) wr32(d, NVMAP_GET_ID_ID, m->id);
    pm_unlock(&g_host.nvdrv.lock);
    return m ? NV_RESULT_SUCCESS : NV_RESULT_NOT_IMPLEMENTED;
}
static u32 ioctl_nvmap(u32 cmd, u8* d, u32 n) {
    (void)n;
    switch (nv_ioctl_cmd(cmd)) {
        case NVMAP_IOC_CREATE: return nvmap_ioc_create(d);
        case NVMAP_IOC_FROM_ID: return nvmap_ioc_from_id(d);
        case NVMAP_IOC_ALLOC: return nvmap_ioc_alloc(d);
        case NVMAP_IOC_FREE: return nvmap_ioc_free(d);
        case NVMAP_IOC_PARAM: return nvmap_ioc_param(d);
        case NVMAP_IOC_GET_ID: return nvmap_ioc_get_id(d);
        default: WARN_LIMIT(16, "nvdrv: nvmap ioctl %08x not implemented\n", cmd); return NV_RESULT_SUCCESS;
    }
}

/* /dev/nvhost-as-gpu. The caller holds nv.aslock, which covers nv.gmaps and the VA allocator */
static int pow2(u64 v) { return !(v & (v - 1)); }
/* ALLOC_SPACE: reserves GPU VA, at the address with NVHOST_AS_MAP_FIXED, else wherever the allocator finds room at
   the alignment (at least the page size), which goes back in the offset field */
static u32 as_alloc_space(u8* d) {
    u32 page_size = rd32(d, AS_ALLOC_SPACE_PAGE_SIZE);
    u64 size = (u64)rd32(d, AS_ALLOC_SPACE_PAGES) * page_size, field = rd64(d, AS_ALLOC_SPACE_OFFSET), gva;
    if (rd32(d, AS_ALLOC_SPACE_FLAGS) & NVHOST_AS_MAP_FIXED) { /* the field is the address */
        gva = field;
        if (!legacy_gva_ok(gva, size)) {
            WARN_LIMIT(8, "nvdrv: ALLOC_SPACE at %llx of %llx bytes is outside the GPU address space\n",
                       (unsigned long long)gva, (unsigned long long)size);
            return NV_RESULT_BAD_PARAMETER;
        }
        va_reserve(gva, size);
    } else {
        u64 align = field > page_size ? field : page_size;
        if (!align || !pow2(align) || !gva_ok(0, size)) {
            WARN_LIMIT(8, "nvdrv: ALLOC_SPACE of %llx bytes aligned to %llx refused\n", (unsigned long long)size,
                       (unsigned long long)align);
            return NV_RESULT_BAD_PARAMETER;
        }
        gva = va_alloc(size, align);
        if (!gva) return NV_RESULT_INSUFFICIENT_MEMORY;
        wr64(d, AS_ALLOC_SPACE_OFFSET, gva);
    }
    LOGV("nvdrv: ALLOC_SPACE %llx bytes at %llx\n", (unsigned long long)size, (unsigned long long)gva);
    return NV_RESULT_SUCCESS;
}
/* FREE_SPACE: unmaps the range, forgets the mappings in it, and gives its VA back */
static u32 as_free_space(const u8* d) {
    u64 gva = rd64(d, AS_FREE_SPACE_OFFSET);
    u64 size = (u64)rd32(d, AS_FREE_SPACE_PAGES) * rd32(d, AS_FREE_SPACE_PAGE_SIZE);
    if (!legacy_gva_ok(gva, size)) return NV_RESULT_BAD_PARAMETER;
    gmmu_unmap(gva, size, 0, 0);
    gmap_cut(gva, size, 0);
    va_free(gva, size);
    return NV_RESULT_SUCCESS;
}
/* GPU address space for a front end that maps guest memory itself, without nvhost-as-gpu's ioctls (nvn's
   memory pools): the same allocator and page table, under the same lock */
u64 gmmu_space_alloc(u64 size, u64 align) {
    u64 gva;
    if (!size || !gva_ok(0, size)) return 0;
    pm_lock(&g_host.nvdrv.aslock);
    gva = va_alloc(size, align && pow2(align) ? align : VA_UNIT);
    pm_unlock(&g_host.nvdrv.aslock);
    return gva;
}
void gmmu_space_map(u64 gva, u64 cpu, u64 size) {
    if (!size || !gva_ok(gva, size) || !cpu_ok(cpu, size) || !page_aligned(gva) || !page_aligned(cpu)) {
        WARN_LIMIT(8, "nvdrv: a mapping of %llx bytes of %llx at %llx is outside the address spaces or unaligned\n",
                   (unsigned long long)size, (unsigned long long)cpu, (unsigned long long)gva);
        return;
    }
    pm_lock(&g_host.nvdrv.aslock);
    gmmu_map(gva, cpu, (size + GMMU_PAGE_MASK) & ~GMMU_PAGE_MASK);
    pm_unlock(&g_host.nvdrv.aslock);
}
void gmmu_space_unmap(u64 gva, u64 size) {
    if (!size || !gva_ok(gva, size)) return;
    pm_lock(&g_host.nvdrv.aslock);
    gmmu_unmap(gva, (size + GMMU_PAGE_MASK) & ~GMMU_PAGE_MASK, 0, 0);
    pm_unlock(&g_host.nvdrv.aslock);
}
void gmmu_space_free(u64 gva, u64 size) {
    if (!size || !gva_ok(gva, size)) return;
    pm_lock(&g_host.nvdrv.aslock);
    gmmu_unmap(gva, (size + GMMU_PAGE_MASK) & ~GMMU_PAGE_MASK, 0, 0);
    va_free(gva, size);
    pm_unlock(&g_host.nvdrv.aslock);
}
/* UNMAP_BUFFER: every piece of the mapping MAP_BUFFER_EX made at base, and its allocator VA */
static u32 as_unmap_buffer(u64 base) {
    u64 va = 0;
    u32 k = 0;
    while (k < g_hle.nvdrv.ngmaps) {
        GMap g = g_hle.nvdrv.gmaps[k];
        if (g.gsv || g.base != base) {
            k++;
            continue;
        }
        gmmu_unmap(g.gva, g.size, 1, g.cpu);
        if (g.va) va = g.va;
        g_hle.nvdrv.gmaps[k] = g_hle.nvdrv.gmaps[--g_hle.nvdrv.ngmaps];
        nvmap_unpin(g.h);
    }
    if (va) va_free(base, va);
    return NV_RESULT_SUCCESS;
}
/* MAP_BUFFER_EX: maps an nvmap handle's memory from buffer_offset at a GPU address, the game's (NVHOST_AS_MAP_FIXED)
   or the allocator's (handed back in the offset field). With NVHOST_AS_MAP_REMAP it replaces part of an earlier
   mapping, which keeps its base */
static u32 as_map_buffer(u8* d) {
    u32 flags = rd32(d, AS_MAP_BUFFER_FLAGS), h = rd32(d, AS_MAP_BUFFER_HANDLE);
    u32 page = rd32(d, AS_MAP_BUFFER_PAGE_SIZE), hsize;
    i64 boff = (i64)rd64(d, AS_MAP_BUFFER_BUFFER_OFFSET);
    u64 msize = rd64(d, AS_MAP_BUFFER_MAPPING_SIZE), gva = rd64(d, AS_MAP_BUFFER_OFFSET), haddr, size, span, cpu, base,
        va = 0;
    GMap* g;
    if ((flags & NVHOST_AS_MAP_REMAP) && !h) return NV_RESULT_SUCCESS; /* only the kind changes, which nothing keeps */
    if (!nvmap_get(h, &haddr, &hsize)) return NV_RESULT_NOT_IMPLEMENTED;
    if (!haddr) WARN_LIMIT(4, "nvdrv: MAP_BUFFER_EX of nvmap handle %u, which has no address\n", h);
    size = msize ? msize : hsize;
    if (boff < 0 || size > ~(u64)0 - GMMU_PAGE_MASK) return NV_RESULT_BAD_PARAMETER;
    span = (size + GMMU_PAGE_MASK) & ~GMMU_PAGE_MASK; /* a page table entry exposes a whole page */
    if (!span || (u64)boff > AS_SIZE) return NV_RESULT_BAD_PARAMETER;
    cpu = haddr + (u64)boff;
    if (!page_aligned(cpu) || !cpu_ok(cpu, span)) return NV_RESULT_BAD_PARAMETER;
    if (flags & (NVHOST_AS_MAP_FIXED | NVHOST_AS_MAP_REMAP)) {
        if (!page_aligned(gva) || !legacy_gva_ok(gva, span)) return NV_RESULT_BAD_PARAMETER;
    } else if (!pow2(page))
        return NV_RESULT_BAD_PARAMETER;
    if (flags & NVHOST_AS_MAP_REMAP) { /* the pieces it replaces keep naming the mapping UNMAP_BUFFER will end */
        u32 k;
        base = gva;
        for (k = 0; k < g_hle.nvdrv.ngmaps; k++)
            if (gva >= g_hle.nvdrv.gmaps[k].gva && gva - g_hle.nvdrv.gmaps[k].gva < g_hle.nvdrv.gmaps[k].size) {
                base = g_hle.nvdrv.gmaps[k].base;
                va = g_hle.nvdrv.gmaps[k].va;
                break;
            }
        gmap_cut(gva, span, 0);
    } else if (flags & NVHOST_AS_MAP_FIXED) {
        base = gva;
    } else {
        gva = va_alloc(span, page ? page : VA_UNIT);
        if (!gva) return NV_RESULT_INSUFFICIENT_MEMORY;
        base = gva;
        va = span;
    }
    gmmu_map(gva, cpu, span);
    g = gmap_new(h);
    g->gva = gva;
    g->size = span;
    g->cpu = cpu;
    g->base = base;
    g->va = va;
    wr64(d, AS_MAP_BUFFER_OFFSET, gva);
    LOGV("nvdrv: MAP_BUFFER_EX handle %u +%llx: %llx bytes at %llx -> %llx\n", h, (unsigned long long)boff,
         (unsigned long long)span, (unsigned long long)gva, (unsigned long long)cpu);
    return NV_RESULT_SUCCESS;
}
/* GET_VA_REGIONS: the small-page region below the big-page one, which starts where the allocator does. The layout is
   Eden's default VM (nvdrv_as_gpu.h): small pages from big page size << VA_START_SHIFT to DEFAULT_VA_SPLIT, big pages
   from there to DEFAULT_VA_RANGE (2^37). The big-page count is Eden's GetVARegions' too, its allocator's limit
   ((range - split) >> big page bits) less its start (split >> big page bits). A Switch 2 view has small pages only,
   one region, as eden-ns2's extended ABI answers */
static void as_va_regions(const NvFile* f, u8* d, u8* extra, u32 nextra) {
    u32 bits = g_hle.nvdrv.big_page_bits;
    u64 big = (u64)1 << bits, start = big << AS_VA_START_SHIFT, end = AS_DEFAULT_VA_RANGE;
    u8 r[AS_VA_REGIONS * AS_VA_REGION_BYTES];
    u8 *small_pages = r, *big_pages = r + AS_VA_REGION_BYTES;
    memset(r, 0, sizeof r);
    if (f->view_end) {
        wr64(small_pages, AS_VA_REGION_OFFSET, f->view_first);
        wr32(small_pages, AS_VA_REGION_PAGE_SIZE, AS_SMALL_PAGE_SIZE);
        wr64(small_pages, AS_VA_REGION_PAGES, (f->view_end - f->view_first) >> GMMU_PAGE_BITS);
        wr32(d, AS_GET_VA_REGIONS_BUF_SIZE, AS_VA_REGION_BYTES);
        memcpy(d + AS_GET_VA_REGIONS_REGIONS, r, sizeof r);
        copy_extra(extra, nextra, r, AS_VA_REGION_BYTES);
        return;
    }
    wr64(small_pages, AS_VA_REGION_OFFSET, start);
    wr32(small_pages, AS_VA_REGION_PAGE_SIZE, AS_SMALL_PAGE_SIZE);
    wr64(small_pages, AS_VA_REGION_PAGES, (VA_BASE - start) >> GMMU_PAGE_BITS);
    wr64(big_pages, AS_VA_REGION_OFFSET, VA_BASE);
    wr32(big_pages, AS_VA_REGION_PAGE_SIZE, (u32)big);
    wr64(big_pages, AS_VA_REGION_PAGES, ((end - VA_BASE) >> bits) - (VA_BASE >> bits));
    wr32(d, AS_GET_VA_REGIONS_BUF_SIZE, sizeof r);
    memcpy(d + AS_GET_VA_REGIONS_REGIONS, r, sizeof r);
    copy_extra(extra, nextra, r, sizeof r);
}
/* ALLOC_AS_EX: the big page size, 64 KiB or 128 KiB, 0 meaning Eden's default; another size leaves it as it was */
static u32 as_alloc_as_ex(const u8* d) {
    u32 big = rd32(d, AS_ALLOC_AS_EX_BIG_PAGE_SIZE) ? rd32(d, AS_ALLOC_AS_EX_BIG_PAGE_SIZE) : AS_DEFAULT_BIG_PAGE_SIZE;
    if (big == 1u << AS_BIG_PAGE_BITS_64K) g_hle.nvdrv.big_page_bits = AS_BIG_PAGE_BITS_64K;
    if (big == 1u << AS_BIG_PAGE_BITS_128K) g_hle.nvdrv.big_page_bits = AS_BIG_PAGE_BITS_128K;
    LOG("nvdrv: GPU address space with %u KB big pages\n", (1u << g_hle.nvdrv.big_page_bits) >> 10);
    return NV_RESULT_SUCCESS;
}
/* devices 0 looks up an AS for a legacy view or supervisor registration, without a worker's owner constraint. */
static NvGsvAs* nv_gsv_as(u32 devices, u32 id) {
    for (u32 i = 0; i < NV_GSV_ADDRESS_SPACES; ++i) {
        NvGsvAs* as = &g_hle.nvdrv.gsv_as[i];
        if (as->devices && as->id == id && (!devices || as->devices == devices)) return as;
    }
    return NULL;
}
/* ALLOC_AS_EX of a Switch 2 driver, with the id of a GSV address space (GPU session 33) as as_fd: a view of it in
   4 KiB pages, flags 5 with no split or flags 0 with the split at the end, inside the user partition (eden-ns2's
   IsSupportedNativeAddressSpaceView). The one GPU page table holds every view. The fd keeps its range for
   GET_VA_REGIONS, and makes it once */
static u32 as_alloc_view(NvFile* f, const u8* d) {
    u32 flags = rd32(d, AS_ALLOC_AS_EX_FLAGS), id = rd32(d, AS_ALLOC_AS_EX_AS_FD);
    u64 first = rd64(d, AS_ALLOC_AS_EX_VA_START), end = rd64(d, AS_ALLOC_AS_EX_VA_END);
    u64 split = rd64(d, AS_ALLOC_AS_EX_VA_SPLIT);
    int small = (flags == 5 && !split) || (flags == 0 && split == end);
    LOG("nvdrv: GPU address space view of %u: flags %x, %llx to %llx\n", id, flags, (unsigned long long)first,
        (unsigned long long)end);
    if (f->view_end) return NV_RESULT_INVALID_STATE;
    if (!nv_gsv_as(0, id) || !small || rd32(d, AS_ALLOC_AS_EX_BIG_PAGE_SIZE) != AS_SMALL_PAGE_SIZE ||
        rd32(d, AS_ALLOC_AS_EX_RESERVED) || first < NV_GSV_USER_FIRST || first >= end || end > NV_GSV_USER_END ||
        ((first | end) & GMMU_PAGE_MASK))
        return NV_RESULT_BAD_PARAMETER;
    f->view_first = first;
    f->view_end = end;
    f->view_as = id;
    return NV_RESULT_SUCCESS;
}
/* GetSyncpointShimInfo: a Switch 2 driver's GPU waits and increments read syncpoint id's value at shim + id * 4 */
static u32 as_syncpoint_shim(const NvFile* f, u8* d) {
    if (!f->view_end) return NV_RESULT_INVALID_STATE;
    wr64(d, AS_SYNCPOINT_SHIM_ADDRESS, NV_SYNCPT_SHIM_GVA);
    wr32(d, AS_SYNCPOINT_SHIM_STRIDE, 4);
    wr32(d, AS_SYNCPOINT_SHIM_COUNT, NV_SYNCPT_SHIM_COUNT);
    return NV_RESULT_SUCCESS;
}
/* This page is MMIO, not guest RAM. Return live counter bytes, including unaligned reads, and zeros past the last
   counter. Writes have no RAM mapping and are discarded. Both reference emulators expose 192 counters here. */
int nv_syncpoint_shim_read(u64 gva, void* dst, u64 size) {
    if (g_hle.kernel.hw != SNX_HW_SWITCH2 || gva < NV_SYNCPT_SHIM_GVA || size > GMMU_PAGE_SIZE ||
        gva - NV_SYNCPT_SHIM_GVA > GMMU_PAGE_SIZE - size)
        return 0;
    u8* out = dst;
    u64 offset = gva - NV_SYNCPT_SHIM_GVA;
    while (size) {
        u32 index = (u32)(offset / 4), byte = (u32)(offset & 3);
        u32 count = 4 - byte;
        if (count > size) count = (u32)size;
        u8 value[4];
        snx_st_le32(value, index < NV_SYNCPT_SHIM_COUNT ? syncpt_read(index) : 0);
        memcpy(out, value + byte, count);
        offset += count;
        out += count;
        size -= count;
    }
    return 1;
}
/* REMAP: entries of IoctlRemapEntry filling the argument, offsets and sizes in big pages. A bad entry is skipped and
   the rest go on: what the SDK makes of an error here is unknown */
static u32 as_remap(const u8* d, u32 n) {
    u32 bits = g_hle.nvdrv.big_page_bits, i;
    for (i = 0; i < n / AS_REMAP_ENTRY_BYTES; i++) {
        const u8* e = d + i * AS_REMAP_ENTRY_BYTES;
        u32 h = rd32(e, AS_REMAP_ENTRY_HANDLE), hsize;
        u64 gva = (u64)rd32(e, AS_REMAP_ENTRY_AS_OFFSET) << bits, size = (u64)rd32(e, AS_REMAP_ENTRY_BIG_PAGES) << bits;
        u64 haddr, cpu;
        if (!legacy_gva_ok(gva, size)) {
            WARN_LIMIT(8, "nvdrv: REMAP at %llx of %llx bytes is outside the GPU address space\n",
                       (unsigned long long)gva, (unsigned long long)size);
            if (g_hle.kernel.hw == SNX_HW_SWITCH2) return NV_RESULT_BAD_PARAMETER;
            continue;
        }
        if (!h) {
            gmmu_unmap(gva, size, 0, 0);
            gmap_cut(gva, size, 0);
            LOG_LIMIT(16, "nvdrv: REMAP unmaps %llx bytes at %llx\n", (unsigned long long)size,
                      (unsigned long long)gva);
            continue;
        }
        if (!nvmap_get(h, &haddr, &hsize)) {
            WARN_LIMIT(8, "nvdrv: REMAP of nvmap handle %u, which isn't allocated\n", h);
            continue;
        }
        if (!haddr) WARN_LIMIT(4, "nvdrv: REMAP of nvmap handle %u, which has no address\n", h);
        cpu = haddr + ((u64)rd32(e, AS_REMAP_ENTRY_HANDLE_OFFSET) << bits);
        if (!cpu_ok(cpu, size)) {
            WARN_LIMIT(8, "nvdrv: REMAP of handle %u past guest memory\n", h);
            continue;
        }
        gmmu_map(gva, cpu, size);
        gmap_cut(gva, size, 0);
        {
            GMap* g = gmap_new(h);
            g->gva = gva;
            g->size = size;
            g->cpu = cpu;
            g->base = gva;
            g->va = 0;
        }
        LOG_LIMIT(16, "nvdrv: REMAP handle %u: %llx bytes at %llx -> %llx\n", h, (unsigned long long)size,
                  (unsigned long long)gva, (unsigned long long)cpu);
    }
    return NV_RESULT_SUCCESS;
}
static u32 ioctl_as(NvFile* f, u32 cmd, u8* d, u32 n, u8* extra, u32 nextra) {
    int switch2 = g_hle.kernel.hw == SNX_HW_SWITCH2;
    if (f->view_as) {
        if (!switch2 || !nv_gsv_as(0, f->view_as)) return NV_RESULT_INVALID_STATE;
        /* Native views expose the worker's page table, not a second legacy allocator over the same VA. */
        switch (nv_ioctl_cmd(cmd)) {
            case NVHOST_AS_GPU_ALLOC_AS_EX: return NV_RESULT_INVALID_STATE;
            case NVHOST_AS_GPU_ALLOC_SPACE:
            case NVHOST_AS_GPU_FREE_SPACE:
            case NVHOST_AS_GPU_UNMAP_BUFFER:
            case NVHOST_AS_GPU_MAP_BUFFER_EX:
            case NVHOST_AS_GPU_REMAP: return NV_RESULT_NOT_SUPPORTED;
            default: break;
        }
    }
    switch (nv_ioctl_cmd(cmd)) {
        case NVHOST_AS_GPU_BIND_CHANNEL: return NV_RESULT_SUCCESS;
        case NVHOST_AS_GPU_ALLOC_SPACE: return as_alloc_space(d);
        case NVHOST_AS_GPU_FREE_SPACE: return as_free_space(d);
        case NVHOST_AS_GPU_UNMAP_BUFFER: return as_unmap_buffer(rd64(d, AS_UNMAP_BUFFER_OFFSET));
        case NVHOST_AS_GPU_MAP_BUFFER_EX: return as_map_buffer(d);
        case NVHOST_AS_GPU_GET_VA_REGIONS: as_va_regions(f, d, extra, nextra); return NV_RESULT_SUCCESS;
        case NVHOST_AS_GPU_ALLOC_AS_EX:
            return switch2 && rd32(d, AS_ALLOC_AS_EX_AS_FD) ? as_alloc_view(f, d) : as_alloc_as_ex(d);
        case NVHOST_AS_GPU_SYNCPOINT_SHIM:
            if (switch2) return as_syncpoint_shim(f, d);
            break;
        case NVHOST_AS_GPU_REMAP: return as_remap(d, n);
        default: break;
    }
    WARN_LIMIT(16, "nvdrv: nvhost-as-gpu ioctl %08x not implemented\n", cmd);
    /* the Switch 1 path answers success for these, as it did. A Switch 2 driver gets the error */
    return switch2 ? NV_RESULT_NOT_IMPLEMENTED : NV_RESULT_SUCCESS;
}

/* /dev/nvhost-ctrl: syncpoints and the event slots NvRm waits on */
static void nvev_disarm(u32 fd, u32 slot) { /* under nvev_lk; the event stays unsignaled: the wait was canceled */
    NvEvent* e = nvev_slot(fd, slot, 0);
    if (!e || !e->ev) return;
    e->armed = 0;
    kevent_clear(kev(e->ev));
}
/* a ctrl wait's timeout in ms as syncpt_wait's ns: negative waits for good */
static i64 ctrl_timeout_ns(i32 timeout_ms) { return timeout_ms < 0 ? -1 : (i64)timeout_ms * NS_PER_MS; }
/* EVENT_WAIT and EVENT_WAIT_ASYNC. The asynchronous one arms the slot of fd's that value names and answers Timeout
   with value naming that slot (an allocated SyncpointEventValue), which the SDK then waits on as an event. A timeout
   of 0 only asks: Timeout without arming the slot or clearing its event, which another wait on the slot may hold
   signaled, as Ryujinx's EventWait answers it */
static u32 ctrl_event_wait(u32 fd, u8* d, int async) {
    u32 id = rd32(d, CTRL_SYNCPT_ID), thresh = rd32(d, CTRL_SYNCPT_THRESH);
    u32 slot = nv_ctrl_event_slot(rd32(d, CTRL_EVENT_WAIT_VALUE));
    i32 timeout = (i32)rd32(d, CTRL_SYNCPT_TIMEOUT);
    NvEvent* e;
    if (syncpt_reached(id, thresh)) {
        wr32(d, CTRL_EVENT_WAIT_VALUE, syncpt_read(id));
        return NV_RESULT_SUCCESS;
    }
    if (!timeout) return NV_RESULT_TIMEOUT;
    if (!async) {
        if (syncpt_wait(id, thresh, ctrl_timeout_ns(timeout))) {
            wr32(d, CTRL_EVENT_WAIT_VALUE, syncpt_read(id));
            return NV_RESULT_SUCCESS;
        }
        return NV_RESULT_TIMEOUT;
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
    wr32(d, CTRL_EVENT_WAIT_VALUE, nv_event_value(id, slot));
    return NV_RESULT_TIMEOUT;
}
/* the ctrl commands whose first word is a syncpoint id, refused past the table (MODULE_MUTEX and MODULE_REGRDWR name
   a module instead) */
static int ctrl_takes_syncpt(u32 c) {
    return (c >= NVHOST_CTRL_SYNCPT_READ && c <= NVHOST_CTRL_SYNCPT_READ_MAX && c != NVHOST_CTRL_MODULE_MUTEX &&
            c != NVHOST_CTRL_MODULE_REGRDWR) ||
           c == NVHOST_CTRL_EVENT_WAIT || c == NVHOST_CTRL_EVENT_WAIT_ASYNC;
}
static u32 ioctl_ctrl(u32 fd, u32 cmd, u8* d, u32 n) {
    u32 c = nv_ioctl_cmd(cmd), id = rd32(d, CTRL_SYNCPT_ID), i;
    if (ctrl_takes_syncpt(c) && id >= SYNCPT_IDS) {
        WARN_LIMIT(8, "nvdrv: nvhost-ctrl ioctl %08x on syncpoint %u, past the table\n", cmd, id);
        return NV_RESULT_BAD_PARAMETER;
    }
    switch (c) {
        case NVHOST_CTRL_SYNCPT_READ: wr32(d, CTRL_SYNCPT_VALUE, syncpt_read(id)); return NV_RESULT_SUCCESS;
        case NVHOST_CTRL_SYNCPT_INCR: syncpt_incr(id); return NV_RESULT_SUCCESS;
        case NVHOST_CTRL_SYNCPT_WAIT:
        case NVHOST_CTRL_SYNCPT_WAITEX: { /* WAITEX has the value back at its end */
            int ok = syncpt_wait(id, rd32(d, CTRL_SYNCPT_THRESH), ctrl_timeout_ns((i32)rd32(d, CTRL_SYNCPT_TIMEOUT)));
            if (n >= CTRL_SYNCPT_WAITEX_BYTES) wr32(d, CTRL_SYNCPT_WAITEX_VALUE, syncpt_read(id));
            return ok ? NV_RESULT_SUCCESS : NV_RESULT_TIMEOUT;
        }
        case NVHOST_CTRL_SYNCPT_READ_MAX: /* submissions raise it under sync_lk */
            pm_lock(&g_host.nvdrv.sync_lk);
            wr32(d, CTRL_SYNCPT_VALUE, g_hle.nvdrv.syncpt_max[id]);
            pm_unlock(&g_host.nvdrv.sync_lk);
            return NV_RESULT_SUCCESS;
        /* the SDK's debug switches (NvOsGetConfigU32, NvOsGetConfigString), none of which a production console has: the
           SDK takes one found as set, and an empty NVWSI_FILL had the window clear each buffer it hands out, on the CPU */
        case NVHOST_CTRL_GET_CONFIG:
            if (n >= CTRL_GET_CONFIG_PARAM_STR + CTRL_GET_CONFIG_NAME_BYTES)
                LOGV("nvdrv: GET_CONFIG %.*s!%.*s: not found, as on a production console\n", CTRL_GET_CONFIG_NAME_BYTES,
                     (const char*)d + CTRL_GET_CONFIG_DOMAIN_STR, CTRL_GET_CONFIG_NAME_BYTES,
                     (const char*)d + CTRL_GET_CONFIG_PARAM_STR);
            return NV_RESULT_CONFIG_VAR_NOT_FOUND;
        case NVHOST_CTRL_EVENT_CLEAR_WAIT: /* cancels without signaling */
            nvev_lock();
            nvev_disarm(fd, nv_ctrl_event_slot(id));
            nvev_unlock();
            LOG_LIMIT(8, "nvdrv: event slot %u wait canceled\n", nv_ctrl_event_slot(id));
            return NV_RESULT_SUCCESS;
        case NVHOST_CTRL_EVENT_WAIT: return ctrl_event_wait(fd, d, 0);
        case NVHOST_CTRL_EVENT_WAIT_ASYNC: return ctrl_event_wait(fd, d, 1);
        case NVHOST_CTRL_EVENT_REGISTER: {
            KEvent* k;
            nvev_lock();
            k = nvev_get(fd, nv_ctrl_event_slot(rd32(d, CTRL_EVENT_SLOT)));
            nvev_unlock();
            return k ? NV_RESULT_SUCCESS : NV_RESULT_INSUFFICIENT_MEMORY;
        }
        case NVHOST_CTRL_EVENT_UNREGISTER:
            nvev_lock();
            nvev_disarm(fd, nv_ctrl_event_slot(rd32(d, CTRL_EVENT_SLOT)));
            nvev_unlock();
            return NV_RESULT_SUCCESS;
        case NVHOST_CTRL_EVENT_KILL: {
            u64 mask = rd64(d, CTRL_EVENT_KILL_MASK);
            nvev_lock();
            for (i = 0; i < NV_CTRL_EVENT_SLOTS; i++)
                if (mask >> i & 1) nvev_disarm(fd, i);
            nvev_unlock();
            return NV_RESULT_SUCCESS;
        }
        default: WARN_LIMIT(16, "nvdrv: nvhost-ctrl ioctl %08x not implemented\n", cmd); return NV_RESULT_SUCCESS;
    }
}

/* /dev/nvhost-ctrl-gpu: what a Tegra X1's GM20B reports, values from Eden's nvhost_ctrl_gpu.h, whose comments name
   each (the NVGPU_GPU_* constants of Linux nvgpu) */
static const NvgpuGpuCharacteristics gm20b_characteristics = {
    .arch = 0x120,                          /* NVGPU_GPU_ARCH_GM200 */
    .impl = 0xB,                            /* NVGPU_GPU_IMPL_GM20B */
    .rev = 0xA1,                            /* revision A1 */
    .num_gpc = NVGPU_NUM_GPCS,
    .l2_cache_size = 0x40000,               /* 256 KiB */
    .on_board_video_memory_size = 0,        /* none: the GPU uses system memory */
    .num_tpc_per_gpc = NVGPU_NUM_SMS / NVGPU_NUM_GPCS,
    .bus_type = 0x20,                       /* NVGPU_GPU_BUS_TYPE_AXI */
    .big_page_size = 0x20000,
    .compression_page_size = 0x20000,
    .pde_coverage_bit_count = 0x1B,         /* a page directory entry covers 2^27 bytes */
    .available_big_page_sizes = 0x30000,    /* 64 KiB | 128 KiB */
    .gpc_mask = 1,
    .sm_arch_sm_version = 0x503,            /* SM 5.3 */
    .sm_arch_spa_version = 0x503,
    .sm_arch_warp_count = NVGPU_SM_WARPS,
    .gpu_va_bit_count = GMMU_ADDRESS_SPACE_BITS,
    .reserved = 0,
    .flags = 0x55,
    .twod_class = 0x902D,                   /* FERMI_TWOD_A */
    .threed_class = 0xB197,                 /* MAXWELL_B */
    .compute_class = 0xB1C0,                /* MAXWELL_COMPUTE_B */
    .gpfifo_class = 0xB06F,                 /* MAXWELL_CHANNEL_GPFIFO_A */
    .inline_to_memory_class = 0xA140,       /* KEPLER_INLINE_TO_MEMORY_B */
    .dma_copy_class = 0xB0B5,               /* MAXWELL_DMA_COPY_A */
    .max_fbps_count = 1,
    .fbp_en_mask = 0,                       /* disabled */
    .max_ltc_per_fbp = 2,
    .max_lts_per_ltc = 1,
    .max_tex_per_tpc = 0,                   /* not supported */
    .max_gpc_count = 1,
    .rop_l2_en_mask_0 = 0x21D70,            /* fuse_status_opt_rop_l2_fbp_r */
    .rop_l2_en_mask_1 = 0,
    .chipname = 0x6230326D67ull,            /* "gm20b", little-endian */
    .gr_compbit_store_base_hw = 0           /* not supported */
};
/* the characteristics as the guest reads them, NVGPU_CHARACTERISTICS_BYTES little-endian bytes */
static void gpu_characteristics(u8* c) {
    const NvgpuGpuCharacteristics* g = &gm20b_characteristics;
#define PUT32(f) wr32(c, (u32)offsetof(NvgpuGpuCharacteristics, f), g->f)
#define PUT64(f) wr64(c, (u32)offsetof(NvgpuGpuCharacteristics, f), g->f)
    PUT32(arch); PUT32(impl); PUT32(rev); PUT32(num_gpc);
    PUT64(l2_cache_size); PUT64(on_board_video_memory_size);
    PUT32(num_tpc_per_gpc); PUT32(bus_type); PUT32(big_page_size); PUT32(compression_page_size);
    PUT32(pde_coverage_bit_count); PUT32(available_big_page_sizes); PUT32(gpc_mask); PUT32(sm_arch_sm_version);
    PUT32(sm_arch_spa_version); PUT32(sm_arch_warp_count); PUT32(gpu_va_bit_count); PUT32(reserved);
    PUT64(flags);
    PUT32(twod_class); PUT32(threed_class); PUT32(compute_class); PUT32(gpfifo_class);
    PUT32(inline_to_memory_class); PUT32(dma_copy_class);
    PUT32(max_fbps_count); PUT32(fbp_en_mask); PUT32(max_ltc_per_fbp); PUT32(max_lts_per_ltc);
    PUT32(max_tex_per_tpc); PUT32(max_gpc_count); PUT32(rop_l2_en_mask_0); PUT32(rop_l2_en_mask_1);
    PUT64(chipname); PUT64(gr_compbit_store_base_hw);
#undef PUT32
#undef PUT64
}
static_assert(sizeof(NvgpuGpuCharacteristics) == NVGPU_CHARACTERISTICS_BYTES, "the guest's IoctlGpuCharacteristics");
static_assert(offsetof(NvgpuGpuCharacteristics, chipname) == 0x90, "the guest's IoctlGpuCharacteristics");
/* The Switch 2 profile's answer, in the layout of NVGPU_NATIVE_CHARACTERISTICS_BYTES, with eden-ns2's values
   (native_gpu_characteristics.h) but SM 8.8. These are not measured on a console. The first 0x70 bytes are the Tegra
   X1's, with these changes: the Ampere chip id (arch 0x170, impl 0xE, which the driver's own chip table holds), SM 8.8,
   4 KiB pages beside 64 KiB, and more capability flags. The driver needs flags 8 and 22 to take the subcontext count.
   The interface levels are eden-ns2's too. The driver's device setup fails with 2 when the address space level is below 6
   (0xc2904f0 in one title), and the GM20B fields at that place (max_ltc_per_fbp, 2) did just that.

   SM 8.8, not eden-ns2's 8.7: one launch title's NVN driver has its built-in GPU programs only for 0x808 (its lookup at
   0xd5b0180 returns none for any other version), so its nvnDeviceInitialize fails on 8.7. A retail console runs that
   title, so it reports 8.8. The later SDKs' drivers handle both */
static void gpu_characteristics_switch2(u8* c) {
    /* ctrl-gpu, TSG, debugger (none), channel, address space */
    /* NvRmGpuChannelSetPreemptionModes needs channel level 0x1F. At 0x1D an SDK tears down the first queue,
       then a queue sharing its TSG reads through null. */
    static const i16 levels[] = {0x1C, 0xB, -1, 0x1F, 0x14};
    u8 legacy[NVGPU_CHARACTERISTICS_BYTES];
    gpu_characteristics(legacy);
    memcpy(c, legacy, NVGPU_NATIVE_LEVELS);
    wr32(c, (u32)offsetof(NvgpuGpuCharacteristics, arch), 0x170);
    wr32(c, (u32)offsetof(NvgpuGpuCharacteristics, impl), 0xE);
    wr32(c, (u32)offsetof(NvgpuGpuCharacteristics, sm_arch_sm_version), 0x808);
    wr32(c, (u32)offsetof(NvgpuGpuCharacteristics, sm_arch_spa_version), 0x808);
    wr32(c, (u32)offsetof(NvgpuGpuCharacteristics, big_page_size), 0x1000);
    wr32(c, (u32)offsetof(NvgpuGpuCharacteristics, available_big_page_sizes),
         (gm20b_characteristics.available_big_page_sizes & 0x10000) | 0x1000);
    wr64(c, (u32)offsetof(NvgpuGpuCharacteristics, flags),
         gm20b_characteristics.flags | 1ull << 8 | 1ull << 22 | 1ull << 26 | 1ull << 34 | 1ull << 35 | 0xFull << 40);
    for (u32 i = 0; i < sizeof levels / sizeof *levels; ++i)
        snx_st_le16(c + NVGPU_NATIVE_LEVELS + 2 * i, (u16)levels[i]);
    wr32(c, NVGPU_NATIVE_SUBCONTEXTS, NV_TSG_SUBCONTEXTS);
}
/* ZCULL_GET_INFO's answer, Eden's ZCullGetInfo values: the GM20B's ZCULL alignment and region geometry */
static const NvgpuGpuZcullGetInfoArgs gm20b_zcull_info = {
    .width_align_pixels = 0x20,
    .height_align_pixels = 0x20,
    .pixel_squares_by_aliquots = 0x400,
    .aliquot_total = 0x800,
    .region_byte_multiplier = 0x20,
    .region_header_size = 0x20,
    .subregion_header_size = 0xC0,
    .subregion_width_align_pixels = 0x20,
    .subregion_height_align_pixels = 0x40,
    .subregion_count = 0x10
};
static void ctrlgpu_zcull_info(u8* d) {
    const NvgpuGpuZcullGetInfoArgs* z = &gm20b_zcull_info;
#define PUT32(f) wr32(d, (u32)offsetof(NvgpuGpuZcullGetInfoArgs, f), z->f)
    PUT32(width_align_pixels); PUT32(height_align_pixels); PUT32(pixel_squares_by_aliquots); PUT32(aliquot_total);
    PUT32(region_byte_multiplier); PUT32(region_header_size); PUT32(subregion_header_size);
    PUT32(subregion_width_align_pixels); PUT32(subregion_height_align_pixels); PUT32(subregion_count);
#undef PUT32
}
static_assert(sizeof(NvgpuGpuZcullGetInfoArgs) == 40, "the guest's IoctlNvgpuGpuZcullGetInfoArgs");
static u32 ioctl_ctrlgpu(u32 cmd, u8* d, u32 n, u8* extra, u32 nextra) {
    static const u8 sms[NVGPU_NUM_SMS * 2] = {0, 0, 0, 1}; /* {gpc, tpc} of each SM */
    switch (nv_ioctl_cmd(cmd)) {
        case NVGPU_GPU_ZCULL_GET_CTX_SIZE:
            wr32(d, CTRL_GPU_ZCULL_CTX_SIZE, NVGPU_ZCULL_CTX_SIZE);
            return NV_RESULT_SUCCESS;
        case NVGPU_GPU_ZCULL_GET_INFO: ctrlgpu_zcull_info(d); return NV_RESULT_SUCCESS;
        case NVGPU_GPU_ZBC_SET_TABLE:
        case NVGPU_GPU_ZBC_QUERY_TABLE: return NV_RESULT_SUCCESS;
        case NVGPU_GPU_GET_CHARACTERISTICS: { /* the buffer address is left as it came */
            u8 c[NVGPU_NATIVE_CHARACTERISTICS_BYTES] = {0};
            u32 size = NVGPU_CHARACTERISTICS_BYTES;
            if (g_hle.kernel.hw == SNX_HW_SWITCH2) {
                gpu_characteristics_switch2(c);
                size = NVGPU_NATIVE_CHARACTERISTICS_BYTES;
            } else {
                gpu_characteristics(c);
            }
            u32 inline_size = n > CTRL_GPU_CHARACTERISTICS_GC ? n - CTRL_GPU_CHARACTERISTICS_GC : 0;
            wr64(d, CTRL_GPU_CHARACTERISTICS_BUF_SIZE, size);
            memcpy(d + CTRL_GPU_CHARACTERISTICS_GC, c, size < inline_size ? size : inline_size);
            copy_extra(extra, nextra, c, size);
            return NV_RESULT_SUCCESS;
        }
        case NVGPU_GPU_GET_TPC_MASKS:
            wr32(d, CTRL_GPU_TPC_MASKS_MASK, NVGPU_TPC_MASK);
            if (nextra >= 4) copy_extra(extra, nextra, d + CTRL_GPU_TPC_MASKS_MASK, 4);
            return NV_RESULT_SUCCESS;
        case NVGPU_GPU_NUM_VSMS:
            if (g_hle.kernel.hw == SNX_HW_SWITCH2) {
                /* A Switch 2 driver sends 0xC0084713 as VSMS_MAPPING, as Linux nvgpu numbers it: an 8-byte argument
                   it leaves alone, and the {gpc, tpc} byte pair of each SM in the second output. It fills that
                   buffer with 0xC0 first and fails (0xF) on a last pair left so (0xc29116c in one title). The
                   Ampere chip's SMs are two to a TPC, so the GPU of the characteristics (one GPC of two TPCs) has
                   four, as eden-ns2 answers */
                R_UNLESS(extra && nextra >= NVGPU_SWITCH2_SMS * 2, NV_RESULT_BAD_PARAMETER);
                for (u32 i = 0; i < NVGPU_SWITCH2_SMS; ++i) {
                    extra[2 * i] = 0;
                    extra[2 * i + 1] = (u8)(i / 2);
                }
                return NV_RESULT_SUCCESS;
            }
            wr32(d, CTRL_GPU_NUM_VSMS, NVGPU_NUM_SMS);
            wr32(d, CTRL_GPU_NUM_VSMS_RESERVED, 0);
            copy_extra(extra, nextra, sms, sizeof sms);
            return NV_RESULT_SUCCESS;
        case NVGPU_GPU_VSMS_MAPPING: /* and NVGPU_GPU_ZBC_GET_ACTIVE_SLOT_MASK: the direction tells them apart */
            if (nv_ioctl_dir(cmd) == NV_IOCTL_DIR_RW) { /* VSMS_MAPPING (0xC0044714): in the ioctl's own buffer */
                memcpy(d, sms, sizeof sms);
                copy_extra(extra, nextra, sms, sizeof sms);
            } else {
                wr32(d, CTRL_GPU_ACTIVE_SLOT, NVGPU_ACTIVE_SLOT);
                wr32(d, CTRL_GPU_ACTIVE_SLOT_MASK, NVGPU_ACTIVE_SLOT_MASK);
            }
            return NV_RESULT_SUCCESS;
        case NVGPU_GPU_PMU_GET_GPU_LOAD: wr32(d, CTRL_GPU_PMU_GPU_LOAD, NVGPU_PMU_GPU_LOAD); return NV_RESULT_SUCCESS;
        case NVGPU_GPU_GET_GPU_TIME: /* the system tick's timebase */
            wr64(d, CTRL_GPU_GPU_TIME, guest_ns());
            return NV_RESULT_SUCCESS;
        default: WARN_LIMIT(16, "nvdrv: nvhost-ctrl-gpu ioctl %08x not implemented\n", cmd); return NV_RESULT_SUCCESS;
    }
}

/* /dev/nvhost-gpu: a GPU channel. SUBMIT_GPFIFO has its entries after the header, SUBMIT_GPFIFO_EX (Ioctl2) in the
   second input. The GPU thread runs them: the ioctl never waits */
static u32 gpu_submit_gpfifo(NvFile* f, u32 cmd, u8* d, u32 n, const u8* extra, u32 nextra) {
    u32 num = rd32(d, GPU_SUBMIT_NUM_ENTRIES), flags = rd32(d, GPU_SUBMIT_FLAGS);
    u32 fid = rd32(d, GPU_SUBMIT_FENCE_ID), fval = rd32(d, GPU_SUBMIT_FENCE_VALUE);
    int ex = nv_ioctl_cmd(cmd) == NVHOST_GPU_SUBMIT_GPFIFO_EX;
    u32 room = ex ? (extra ? nextra / GPFIFO_ENTRY_BYTES : 0)
                  : (n > GPU_SUBMIT_HEADER_BYTES ? (n - GPU_SUBMIT_HEADER_BYTES) / GPFIFO_ENTRY_BYTES : 0);
    const u8* entries = ex ? extra : d + GPU_SUBMIT_HEADER_BYTES;
    u32 sp, host = 0, incs, wait_id = NV_FENCE_NO_SYNCPT, promised;
    if (num > room) {
        WARN_LIMIT(8, "nvdrv: SUBMIT_GPFIFO of %u entries in a buffer of %u\n", num, room);
        num = room;
    }
    if ((flags & GPU_SUBMIT_FENCE_WAIT) && fid != NV_FENCE_NO_SYNCPT) wait_id = fid;
    if (flags & GPU_SUBMIT_FENCE_INCREMENT) host = GPU_SUBMIT_HOST_INCREMENTS;
    /* Maxwell streams make increment_value's fval increments themselves. Native streams use semaphore and QMD
       releases instead, so their promised syncpoint increments belong to the driver's completion command. Keep
       Maxwell's split: adding its stream increments again advances later fences before their work completes. */
    incs = host + ((flags & GPU_SUBMIT_INCREMENT_VALUE) ? fval : 0);
    if (g_hle.kernel.hw == SNX_HW_SWITCH2) host = incs;
    sp = gpu_channel_syncpt(f);
    /* the promise and the queueing in one hold, so promises reach the GPU thread in order */
    pm_lock(&g_host.nvdrv.submit_lk);
    pm_lock(&g_host.nvdrv.lock);
    u64 tsg = f->tsg ? g_hle.nvdrv.nvf[f->tsg].tsg_id : 0;
    pm_unlock(&g_host.nvdrv.lock);
    promised = syncpt_promise(sp, incs);
    gpu_submit(entries, num, sp, host, wait_id, fval, promised, tsg);
    pm_unlock(&g_host.nvdrv.submit_lk);
    wr32(d, GPU_SUBMIT_FENCE_ID, sp);
    wr32(d, GPU_SUBMIT_FENCE_VALUE, promised);
    return NV_RESULT_SUCCESS;
}
static u32 ioctl_gpu(NvFile* f, u32 cmd, u8* d, u32 n, u8* extra, u32 nextra) {
    switch (nv_ioctl_cmd(cmd)) {
        /* channel setup nothing here depends on */
        case NVHOST_GPU_SET_NVMAP_FD:
        case NVHOST_GPU_SET_TIMEOUT:
        case NVHOST_GPU_FREE_OBJ_CTX:
        case NVHOST_GPU_ZCULL_BIND:
        case NVHOST_GPU_SET_ERROR_NOTIFIER:
        case NVHOST_GPU_SET_PRIORITY:
        case NVHOST_GPU_UNNAMED_17:
        case NVHOST_GPU_SET_TIMESLICE: return NV_RESULT_SUCCESS;
        case NVHOST_GPU_SET_PREEMPTION_MODES: {
            R_UNLESS(g_hle.kernel.hw == SNX_HW_SWITCH2, NV_RESULT_NOT_SUPPORTED);
            R_UNLESS(n >= 8, NV_RESULT_BAD_PARAMETER);
            pm_lock(&g_host.nvdrv.lock);
            int initialized = f->syncpt != 0;
            pm_unlock(&g_host.nvdrv.lock);
            R_UNLESS(initialized, NV_RESULT_INVALID_STATE);
            /* Completed commands are both WFI and CTA boundaries. No finer preemption is exposed. */
            return rd32(d, GPU_PREEMPTION_GRAPHICS) <= 1 && rd32(d, GPU_PREEMPTION_COMPUTE) <= 2
                       ? NV_RESULT_SUCCESS
                       : NV_RESULT_NOT_SUPPORTED;
        }
        case NVHOST_GPU_SUBMIT_GPFIFO:
        case NVHOST_GPU_SUBMIT_GPFIFO_EX: return gpu_submit_gpfifo(f, cmd, d, n, extra, nextra);
        case NVHOST_GPU_ALLOC_OBJ_CTX: /* the class names the object */
            wr64(d, GPU_ALLOC_OBJ_CTX_OBJ_ID, rd32(d, GPU_ALLOC_OBJ_CTX_CLASS_NUM));
            return NV_RESULT_SUCCESS;
        case NVHOST_GPU_SET_USER_DATA:
            pm_lock(&g_host.nvdrv.lock);
            f->user_data = rd64(d, GPU_USER_DATA);
            pm_unlock(&g_host.nvdrv.lock);
            return NV_RESULT_SUCCESS;
        case NVHOST_GPU_GET_USER_DATA:
            pm_lock(&g_host.nvdrv.lock);
            wr64(d, GPU_USER_DATA, f->user_data);
            pm_unlock(&g_host.nvdrv.lock);
            return NV_RESULT_SUCCESS;
        case NVHOST_GPU_GET_ERROR_NOTIFICATION: memset(d, 0, n); return NV_RESULT_SUCCESS; /* none */
        case NVHOST_GPU_ALLOC_GPFIFO_EX2: { /* the channel's fence as it stands */
            u32 sp = gpu_channel_syncpt(f);
            wr32(d, GPU_ALLOC_GPFIFO_FENCE_ID, sp);
            wr32(d, GPU_ALLOC_GPFIFO_FENCE_VALUE, syncpt_read(sp));
            return NV_RESULT_SUCCESS;
        }
        default: WARN_LIMIT(16, "nvdrv: nvhost-gpu ioctl %08x not implemented\n", cmd); return NV_RESULT_SUCCESS;
    }
}

/* All membership changes, including close, hold nv.lock. A channel belongs to at most one TSG. */
static u32 ioctl_tsg(u32 fd, u32 cmd, const u8* d) {
    if (cmd != NV_TSG_BIND_CHANNEL && cmd != NV_TSG_UNBIND_CHANNEL &&
        cmd != NV_TSG_BIND_CHANNEL_EX && cmd != NV_TSG_SET_TIMESLICE)
        return NV_RESULT_NOT_IMPLEMENTED;
    if (cmd == NV_TSG_SET_TIMESLICE) /* The GPU thread switches at command boundaries, not timed slices. */
        return rd32(d, 0) ? NV_RESULT_SUCCESS : NV_RESULT_BAD_PARAMETER;
    u32 channel = rd32(d, TSG_CHANNEL_FD);
    u32 subcontext = cmd == NV_TSG_BIND_CHANNEL_EX ? rd32(d, TSG_SUBCONTEXT_ID) : 0;
    if (subcontext >= NV_TSG_SUBCONTEXTS ||
        (cmd == NV_TSG_BIND_CHANNEL_EX && (rd64(d, TSG_RESERVED) || rd64(d, TSG_RESERVED + 8))))
        return NV_RESULT_BAD_PARAMETER;
    pm_lock(&g_host.nvdrv.lock);
    NvFile* f = channel < NV_FDS ? &g_hle.nvdrv.nvf[channel] : NULL;
    u32 r = NV_RESULT_SUCCESS;
    if (g_hle.nvdrv.nvf[fd].closed)
        r = NV_RESULT_INVALID_STATE;
    else if (!f || f->dev != DEV_GPU || f->closed)
        r = NV_RESULT_BAD_PARAMETER;
    else if (cmd == NV_TSG_UNBIND_CHANNEL) {
        if (f->tsg != fd)
            r = NV_RESULT_BAD_PARAMETER;
        else
            f->tsg = f->subcontext = 0;
    } else if (f->tsg)
        r = NV_RESULT_BUSY;
    else {
        u32 count = 0;
        for (u32 i = 1; i < NV_FDS; ++i)
            if (g_hle.nvdrv.nvf[i].tsg == fd) ++count;
        if (count == NV_TSG_CHANNELS)
            r = NV_RESULT_INSUFFICIENT_MEMORY;
        else {
            f->tsg = fd;
            f->subcontext = subcontext;
        }
    }
    pm_unlock(&g_host.nvdrv.lock);
    return r;
}

static int nns_nvdrv_INvDrvServices_Open(SvcObj* self, IpcBuf path, u32* fd, u32* error) {
    static const struct {
        const char* path;
        int dev;
    } devices[] = {{"/dev/nvmap", DEV_NVMAP},      {"/dev/nvhost-as-gpu", DEV_AS},
                   {"/dev/nvhost-ctrl", DEV_CTRL}, {"/dev/nvhost-ctrl-gpu", DEV_CTRLGPU},
                   {"/dev/nvhost-gpu", DEV_GPU},   {"/dev/nvhost-nvdec", DEV_NVDEC},
                   {"/dev/nvhost-vic", DEV_VIC},   {"/dev/nvhost-tsg-gpu", DEV_TSG}};
    char name[64]; /* longer than any device's path */
    int dev = DEV_OTHER; /* opens all the same; its ioctls warn */
    size_t k;
    memset(name, 0, sizeof name);
    if (path.addr) memcpy(name, GP(path.addr), path.size < sizeof name - 1 ? (size_t)path.size : sizeof name - 1);
    /* A production console has no profiler: fd 0 and NotSupported, as eden-ns2 and Eglantine answer */
    if (g_hle.kernel.hw == SNX_HW_SWITCH2 && !strcmp(name, "/dev/nvhost-prof-gpu")) {
        LOG("nvdrv: open %s refused, as on a production console\n", name);
        *fd = 0;
        *error = NV_RESULT_NOT_SUPPORTED;
        R_SUCCEED();
    }
    for (k = 0; k < sizeof devices / sizeof devices[0]; k++)
        if (!strcmp(name, devices[k].path)) dev = devices[k].dev;
    /* an NVDEC or VIC fd is a host1x channel of its own, with its registers, syncpoints and decoder */
    int h1x_dev = dev == DEV_NVDEC || dev == DEV_VIC;
    H1Ch* h1x = h1x_dev ? host1x_open(dev == DEV_VIC) : NULL;
    int slot = -1;
    pm_lock(&g_host.nvdrv.lock);
    for (int i = 1; i < NV_FDS && (h1x || !h1x_dev); i++)
        if (!g_hle.nvdrv.nvf[i].dev) {
            if (dev == DEV_TSG && g_hle.nvdrv.tsg_last == ~0ull) break;
            slot = i;
            memset(&g_hle.nvdrv.nvf[i], 0, sizeof g_hle.nvdrv.nvf[i]);
            g_hle.nvdrv.nvf[i].dev = dev;
            g_hle.nvdrv.nvf[i].h1x = h1x;
            if (dev == DEV_TSG) g_hle.nvdrv.nvf[i].tsg_id = ++g_hle.nvdrv.tsg_last;
            break;
        }
    pm_unlock(&g_host.nvdrv.lock);
    if (slot < 0) host1x_close(h1x);
    LOG("nvdrv: open %s -> %d\n", name, slot);
    *fd = (u32)slot;
    *error = slot < 0 ? NV_RESULT_FILE_OPERATION_FAILED : NV_RESULT_SUCCESS; /* every fd taken (or no memory) */
    R_SUCCEED();
}
static void gpu_tsg_retire(void* arg) { mme_tu104_tsg_close((u64)(uintptr_t)arg); }
/* what goes with a closed fd: the channel and its syncpoint once the GPU thread has run the jobs queued on it (freed
   now, the id could go to a new channel those jobs would then increment), an NVDEC or VIC channel with its decoder
   and syncpoints, and the fd's references to its events */
static void nv_release(NvFile* old, NvEvent* slots) {
    if (old->dev == DEV_GPU && old->syncpt)
        gpu_call(gpu_channel_retire, (void*)(uintptr_t)old->syncpt, &old->syncpt, 1);
    if (old->dev == DEV_TSG) {
        /* Submission captures the token under submit_lk. No old-token job can queue after this notice. */
        pm_lock(&g_host.nvdrv.submit_lk);
        gpu_call_tsg(gpu_tsg_retire, (void*)(uintptr_t)old->tsg_id, old->tsg_id);
        pm_unlock(&g_host.nvdrv.submit_lk);
    }
    host1x_close(old->h1x);
    for (u32 k = 0; k < NV_CHANNEL_EVENTS; k++)
        if (old->ev[k]) kobj_unref(kobj_at(old->ev[k]));
    for (u32 k = 0; slots && k < NV_CTRL_EVENT_SLOTS; k++) /* a ctrl fd's events: the game's handles keep them */
        if (slots[k].ev) kobj_unref(kobj_at(slots[k].ev));
}
/* the scratch buffer an ioctl's in and out data go through, 16 KiB: the sizes the devices see are clamped to it */
#define NV_SCRATCH 0x4000
/* the (device, ioctl) pairs logged already: each is logged on its first use. The size field is left out of the pair:
   a channel's submit carries its entry count there, and every count was logged as a new ioctl. With the table full,
   nothing new is logged: it said "first use" for every pair it couldn't hold, which logged nearly every submit (a
   lock and a log write a frame in one game) */
static int nv_first_use(int dev, u32 cmd) {
    HleNvdrv* nv = &g_hle.nvdrv;
    u32 i;
    int first = 1;
    cmd = nv_ioctl_without_length(cmd);
    pm_lock(&g_host.nvdrv.seen_lock);
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
    pm_unlock(&g_host.nvdrv.seen_lock);
    return first;
}
/* a guest buffer, or none when it lies outside guest memory */
static u8* nv_buffer(IpcBuf b) { return b.addr && b.size <= AS_SIZE && b.addr <= AS_SIZE - b.size ? GP(b.addr) : NULL; }
void nv_ioctl_abandon(u32 fd) {
    pm_lock(&g_host.nvdrv.lock);
    if (fd < NV_FDS && g_hle.nvdrv.nvf[fd].users) g_hle.nvdrv.nvf[fd].users--;
    pm_unlock(&g_host.nvdrv.lock);
}
/* Ioctl, Ioctl2 (extra is a second input) and Ioctl3 (extra is a second output): the ioctl's in and out buffers go
   through the scratch buffer, the extra one straight to the device. A GPU channel takes Ioctl2's and Ioctl3's, as-gpu
   and ctrl-gpu only Ioctl3's. ipc is the command, for the log */
static u32 nv_ioctl(u32 fd, u32 cmd, IpcBuf in, IpcBuf out, IpcBuf extra, int extra_out, u32 ipc) {
    u8 d[NV_SCRATCH];
    u8 *src = nv_buffer(in), *dst = nv_buffer(out), *x = NULL;
    u64 nin = src ? in.size : 0, nout = dst ? out.size : 0, n;
    u32 nx = 0, r;
    NvFile* f;
    NvFile old;
    NvEvent slot_copy[NV_CTRL_EVENT_SLOTS], *slots = NULL;
    int dev, release = 0;
    pm_lock(&g_host.nvdrv.lock);
    if (fd >= NV_FDS || !g_hle.nvdrv.nvf[fd].dev || g_hle.nvdrv.nvf[fd].closed) {
        pm_unlock(&g_host.nvdrv.lock);
        return NV_RESULT_INVALID_STATE;
    }
    f = &g_hle.nvdrv.nvf[fd];
    f->users++; /* the entry and its channel stay until this ioctl leaves (Close) */
    dev = f->dev;
    if (kthread_cur()) kthread_cur()->req_nvfd = fd + 1;
    pm_unlock(&g_host.nvdrv.lock);
    if (nv_first_use(dev, cmd))
        LOG("nvdrv: ioctl %08x on device %d (command %u), buffers %llx/%llx/%llx\n", cmd, dev, ipc,
            (unsigned long long)in.size, (unsigned long long)out.size, (unsigned long long)extra.size);
    memset(d, 0, sizeof d);
    if (nin > sizeof d) nin = sizeof d;
    if (nout > sizeof d) nout = sizeof d;
    if (nin) memcpy(d, src, (size_t)nin);
    /* the size the command names, or the larger buffer's without one. as, gpu and the host1x channels read past it,
       up to the input */
    n = nv_ioctl_length(cmd);
    if (!n) n = in.size > out.size ? in.size : out.size;
    if (dev == DEV_AS || dev == DEV_GPU || dev == DEV_NVDEC || dev == DEV_VIC) n = n > in.size ? n : in.size;
    if (n > sizeof d) n = sizeof d;
    /* Ioctl2's second buffer is an input, Ioctl3's an output: a GPU channel takes both, as-gpu and ctrl-gpu Ioctl3's */
    if (dev == DEV_GPU || (extra_out && (dev == DEV_AS || dev == DEV_CTRLGPU))) {
        x = nv_buffer(extra);
        nx = x ? (extra.size > UINT32_MAX ? UINT32_MAX : (u32)extra.size) : 0;
    }
    switch (dev) {
        case DEV_NVMAP: r = ioctl_nvmap(cmd, d, (u32)n); break;
        case DEV_AS:
            pm_lock(&g_host.nvdrv.aslock);
            r = ioctl_as(f, cmd, d, (u32)n, x, nx);
            pm_unlock(&g_host.nvdrv.aslock);
            break;
        case DEV_CTRL: r = ioctl_ctrl(fd, cmd, d, (u32)n); break;
        case DEV_CTRLGPU: r = ioctl_ctrlgpu(cmd, d, (u32)n, x, nx); break;
        case DEV_GPU: r = ioctl_gpu(f, cmd, d, (u32)n, x, nx); break;
        case DEV_TSG:
            r = ipc != 1 ? NV_RESULT_NOT_IMPLEMENTED
                : nin != nv_ioctl_length(cmd) || nout != nv_ioctl_length(cmd) ? NV_RESULT_BAD_PARAMETER
                : ioctl_tsg(fd, cmd, d);
            break;
        case DEV_NVDEC:
        case DEV_VIC: r = host1x_ioctl(f->h1x, cmd, d, (u32)n); break;
        default:
            WARN_LIMIT(16, "nvdrv: ioctl %08x on a device that has none\n", cmd);
            r = NV_RESULT_SUCCESS;
            break;
    }
    if (nout) memcpy(dst, d, (size_t)nout);
    pm_lock(&g_host.nvdrv.lock);
    if (kthread_cur()) kthread_cur()->req_nvfd = 0;
    if (!--f->users && f->closed) { /* closed meanwhile: this was the last one using it */
        old = *f;
        memset(f, 0, sizeof *f);
        slots = nvev_detach(fd, slot_copy);
        release = 1;
    }
    pm_unlock(&g_host.nvdrv.lock);
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
    NvEvent slot_copy[NV_CTRL_EVENT_SLOTS], *slots = NULL;
    int release = 0, open = 0;
    pm_lock(&g_host.nvdrv.lock);
    if (fd < NV_FDS && g_hle.nvdrv.nvf[fd].dev && !g_hle.nvdrv.nvf[fd].closed) {
        open = 1;
        if (g_hle.nvdrv.nvf[fd].dev == DEV_TSG)
            for (u32 i = 1; i < NV_FDS; ++i)
                if (g_hle.nvdrv.nvf[i].tsg == fd)
                    g_hle.nvdrv.nvf[i].tsg = g_hle.nvdrv.nvf[i].subcontext = 0;
        g_hle.nvdrv.nvf[fd].tsg = g_hle.nvdrv.nvf[fd].subcontext = 0;
        if (g_hle.nvdrv.nvf[fd].users) { /* the last ioctl releases it */
            g_hle.nvdrv.nvf[fd].closed = 1;
        } else {
            old = g_hle.nvdrv.nvf[fd];
            memset(&g_hle.nvdrv.nvf[fd], 0, sizeof g_hle.nvdrv.nvf[fd]);
            slots = nvev_detach(fd, slot_copy);
            release = 1;
        }
    }
    pm_unlock(&g_host.nvdrv.lock);
    if (release) nv_release(&old, slots);
    /* an fd that isn't open, or closed already: the NvError the ioctls answer for it */
    *error = open ? NV_RESULT_SUCCESS : NV_RESULT_INVALID_STATE;
    R_SUCCEED();
}
/* the ctrl fd's ids name event slots, a channel's ids its own events */
static int nns_nvdrv_INvDrvServices_QueryEvent(SvcObj* self, u32 fd, u32 event_id, u32* error, u32* event) {
    KEvent* e = NULL;
    int held = 0;
    pm_lock(&g_host.nvdrv.lock);
    int dev = fd < NV_FDS && !g_hle.nvdrv.nvf[fd].closed ? g_hle.nvdrv.nvf[fd].dev : DEV_NONE;
    if (dev != DEV_NONE && dev != DEV_CTRL && event_id >= 1 && event_id < NV_CHANNEL_EVENTS) {
        if (!g_hle.nvdrv.nvf[fd].ev[event_id]) {
            KEvent* k = kevent_new("nv channel event");
            kobj_ref(&k->o); /* the fd's reference, dropped on close */
            g_hle.nvdrv.nvf[fd].ev[event_id] = kobj_id(&k->o);
        }
        e = kev(g_hle.nvdrv.nvf[fd].ev[event_id]);
        kobj_ref(&e->o); /* a Close before h_new would drop the last reference */
        held = 1;
    }
    if (!e && dev == DEV_CTRL && nv_event_value_names_slot(event_id)) {
        nvev_lock(); /* with nv.lock held: a Close meanwhile would leave the fd's new table behind */
        e = nvev_get(fd, nv_ctrl_event_slot(event_id));
        if (e) {
            kobj_ref(&e->o); /* the fd's reference goes when it closes, which can come before h_new */
            held = 1;
        }
        nvev_unlock();
    }
    pm_unlock(&g_host.nvdrv.lock);
    LOG_LIMIT(8, "nvdrv: QueryEvent fd %u (device %d) id %x\n", fd, dev, event_id);
    if (dev == DEV_NONE) { /* an fd that isn't open: the NvError the ioctls answer for it, and no handle to leak */
        *event = 0;
        *error = NV_RESULT_INVALID_STATE;
        R_SUCCEED();
    }
    if (!e) e = kevent_new("nv query");
    *event = h_new(&e->o);
    if (held) kobj_unref(&e->o);
    *error = NV_RESULT_SUCCESS;
    R_SUCCEED();
}
static SvcObj* new_nvdrv(void) { return nns_nvdrv_INvDrvServices_new(); }
/* GSV counter-cache protocol from eden-ns2's graphics_syncpoint_cache.h and graphics_supervisor.cpp
   (copyright 2026 Eden Emulator Project, GPL-3.0-or-later). Requests refresh only the toggled groups and slots.
   Allocation changes are published immediately, since an unallocated counter is treated as already complete. */
static void nv_cache_observe(u32 id) {
    NvGsvCache* c = &g_hle.nvdrv.gsv_cache;
    pm_lock(&g_host.nvdrv.sync_lk);
    int allocated = id < SYNCPT_N && g_hle.nvdrv.syncpt_used[id];
    u32 current = syncpt_read(id);
    pm_unlock(&g_host.nvdrv.sync_lk);
    u64 value = ~0ull;
    if (allocated) {
        u64 previous = c->previous[id];
        value = previous == ~0ull ? current :
            ((previous & 0xFFFFFFFF00000000ull) + ((u64)(current < (u32)previous) << 32)) | current;
        c->previous[id] = value;
    }
    KTmem* ro = (KTmem*)kobj_at(c->ro);
    gw64(ro->addr + 0x40 + id * 8, value);
}
static void nv_cache_allocation(u32 id) {
    if (g_hle.kernel.hw != SNX_HW_SWITCH2 || id >= NV_GSV_COUNTERS) return;
    nvev_lock();
    if (g_hle.nvdrv.gsv_cache.rw) nv_cache_observe(id);
    nvev_unlock();
}
int nv_syncpoint_cache_bind(u32 rw_handle, u32 ro_handle, u32* request, u32* completed) {
    KTmem* rw = (KTmem*)h_get_ref(rw_handle, KO_TMEM);
    KTmem* ro = (KTmem*)h_get_ref(ro_handle, KO_TMEM);
    int rc = 0;
    if (!rw || !ro || rw == ro)
        rc = RC(1, 114);
    else if (rw->size != 0x1000 || ro->size != 0x2000)
        rc = RC(1, 101);
    if (rc) goto drop;
    nvev_lock();
    NvGsvCache* c = &g_hle.nvdrv.gsv_cache;
    if (c->rw) {
        rc = RC(1, 125);
    } else {
        KEvent* req = kevent_new("gsv cache request");
        KEvent* done = kevent_new("gsv cache completed");
        kobj_ref(&req->o);
        kobj_ref(&done->o);
        *request = kevent_write_handle(req);
        *completed = h_new(&done->o);
        if (!*request || !*completed) {
            if (*request) h_close(*request);
            if (*completed) h_close(*completed);
            *request = *completed = 0;
            kobj_unref(&req->o);
            kobj_unref(&done->o);
            rc = RC(1, 105);
        } else {
            c->rw = kobj_id(&rw->o);
            c->ro = kobj_id(&ro->o);
            c->request = kobj_id(&req->o);
            c->completed = kobj_id(&done->o);
            memset(GP(rw->addr), 0, 0x1000);
            memset(GP(ro->addr), 0, 0x2000);
            const u32 header[] = {1, 0, NV_GSV_COUNTERS - 1, 0x40, 0, 0x640, 8, 0x648};
            for (u32 i = 0; i < sizeof header / sizeof *header; ++i) gw32(ro->addr + i * 4, header[i]);
            for (u32 id = 0; id < NV_GSV_COUNTERS; ++id) {
                c->previous[id] = ~0ull;
                nv_cache_observe(id);
            }
        }
    }
    nvev_unlock();
    if (!rc) return 0; /* the application retains both h_get_ref references */
drop:
    kobj_unref(rw ? &rw->o : NULL);
    kobj_unref(ro ? &ro->o : NULL);
    return rc;
}
void nv_syncpoint_cache_request(u32 event) {
    nvev_lock();
    NvGsvCache* c = &g_hle.nvdrv.gsv_cache;
    if (c->rw && event == c->request) {
        KTmem* rw = (KTmem*)kobj_at(c->rw);
        KTmem* ro = (KTmem*)kobj_at(c->ro);
        u64 requests = gr64(rw->addr);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        u64 groups = (requests ^ gr64(ro->addr + 0x640)) & 7;
        for (; groups; groups &= groups - 1) {
            u32 group = snx_ctz64(groups);
            u64 requested = gr64(rw->addr + 8 + group * 8);
            u64 slots = requested ^ gr64(ro->addr + 0x648 + group * 8);
            for (; slots; slots &= slots - 1) nv_cache_observe(group * 64 + snx_ctz64(slots));
            __atomic_thread_fence(__ATOMIC_RELEASE);
            gw64(ro->addr + 0x648 + group * 8, requested);
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        gw64(ro->addr + 0x640, requests);
        kevent_clear(kev(c->request));
        kevent_signal(kev(c->completed));
    }
    nvev_unlock();
}

/* GSV's 32-bit counter capability and event waits, from eden-ns2's graphics_supervisor.cpp
   (copyright 2026 Eden Emulator Project, GPL-3.0-or-later). Counter increments deliver events, without polling. */
static KEvent* nv_gsv_event(NvGsvSync* s, u32 slot) {
    if (!s->events[slot].ev) {
        KEvent* e = kevent_new("gsv syncpoint");
        kobj_ref(&e->o);
        s->events[slot].ev = kobj_id(&e->o);
    }
    return kev(s->events[slot].ev);
}
/* Older SDKs verify a per-launch challenge. From Eglantine's nx2_gsv.h at bb353609e6,
   copyright 2026 Eden Emulator Project, GPL-3.0-or-later. */
static int nn_gsv_ISyncpointSupervisor_Handshake(SvcObj* self, u64 token, u64* echoed_token) {
    (void)self;
    *echoed_token = token;
    R_SUCCEED();
}
static int nn_gsv_ISyncpointSupervisor_GetMaximumEventCount(SvcObj* self, u32* count) {
    (void)self;
    *count = NV_GSV_EVENTS;
    R_SUCCEED();
}
static int nn_gsv_ISyncpointSupervisor_SetEventCount(SvcObj* self, u32 count) {
    NvGsvSync* s = self->data;
    R_UNLESS(count <= NV_GSV_EVENTS, RC(1, 14));
    for (u32 i = count; i < s->count; ++i) {
        s->events[i].armed = 0;
        if (s->events[i].ev) kevent_clear(kev(s->events[i].ev));
    }
    s->count = count;
    R_SUCCEED();
}
static int nn_gsv_ISyncpointSupervisor_GetEvent(SvcObj* self, u32 slot, u32* event) {
    NvGsvSync* s = self->data;
    R_UNLESS(slot < s->count, RC(1, 14));
    *event = h_new(&nv_gsv_event(s, slot)->o);
    return *event ? 0 : RC(1, 105);
}
static int nn_gsv_ISyncpointSupervisor_QueryCounter(SvcObj* self, u32 encoded_id, u64* value, u8* wide) {
    (void)self;
    u32 id = ~encoded_id;
    R_UNLESS(id < SYNCPT_IDS, RC(1, 121));
    *value = syncpt_read(id);
    *wide = 0; /* this backend exposes 32-bit modular counters */
    R_SUCCEED();
}
static int nn_gsv_ISyncpointSupervisor_ArmWait(SvcObj* self, u32 slot, u32 encoded_id, u64 threshold, u8 wide) {
    NvGsvSync* s = self->data;
    u32 id = ~encoded_id;
    R_UNLESS(slot < s->count && wide <= 1, RC(1, 14));
    R_UNLESS(id < SYNCPT_IDS, RC(1, 121));
    R_UNLESS(!wide, RC(1, 33));
    KEvent* event = nv_gsv_event(s, slot);
    kevent_clear(event);
    s->events[slot].id = id;
    s->events[slot].thresh = (u32)threshold;
    s->events[slot].armed = !syncpt_reached(id, (u32)threshold);
    if (!s->events[slot].armed) kevent_signal(event);
    R_SUCCEED();
}
static int nn_gsv_ISyncpointSupervisor_CancelWait(SvcObj* self, u32 slot) {
    NvGsvSync* s = self->data;
    R_UNLESS(slot < s->count, RC(1, 14));
    s->events[slot].armed = 0;
    if (s->events[slot].ev) kevent_signal(kev(s->events[slot].ev));
    R_SUCCEED();
}
static int nv_gsv_sync_dispatch(SvcObj* self, IpcCtx* c) {
    R_UNLESS(c->tipc, RC(1, 33));
    nvev_lock();
    int rc = nn_gsv_ISyncpointSupervisor_dispatch(self, c);
    nvev_unlock();
    return rc;
}
static void nv_gsv_sync_destroy(SvcObj* self) {
    NvGsvSync* s = self->data;
    nvev_lock();
    for (u32 i = 0; i < NV_GSV_EVENTS; ++i)
        if (s->events[i].ev) kobj_unref(kobj_at(s->events[i].ev));
    /* Stop delivery before IPC releases the pool slot. Its allocation marker stays set until that release. */
    g_hle.nvdrv.gsv_sync_active &= ~(1u << (u32)(s - g_hle.nvdrv.gsv_sync));
    nvev_unlock();
}
SvcObj* nv_syncpoint_new(void) {
    SvcObj* s = SVCOBJ_NEW_DATA("nn::gsv::ISyncpointSupervisor", nv_gsv_sync_dispatch,
                              g_hle.nvdrv.gsv_sync, g_hle.nvdrv.gsv_sync_next, "GSV syncpoint sessions");
    s->destroy = nv_gsv_sync_destroy;
    nvev_lock();
    g_hle.nvdrv.gsv_sync_active |= 1u << (u32)((NvGsvSync*)s->data - g_hle.nvdrv.gsv_sync);
    nvev_unlock();
    return s;
}
/* GPU control-session notification slots, from eden-ns2's gpu_session.cpp (copyright 2026 Eden Emulator Project,
   GPL-3.0-or-later). Events remain unsignalled until an operation actually completes. */
static int nn_gsv_IGpuSession_GetNotificationRange(SvcObj* self, u32* first, u32* last) {
    (void)self;
    *first = 0;
    *last = NV_GSV_GPU_EVENTS - 1;
    R_SUCCEED();
}
static int nn_gsv_IGpuSession_GetNotificationEvent(SvcObj* self, u32 index, u32* event) {
    NvGsvGpu* gpu = self->data;
    NvGsvDevices* s = &g_hle.nvdrv.gsv_devices[gpu->devices - 1];
    R_UNLESS(index < NV_GSV_GPU_EVENTS, RC(1, 116));
    if (!s->events[index]) {
        KEvent* e = kevent_new("gsv GPU notification");
        kobj_ref(&e->o);
        s->events[index] = kobj_id(&e->o);
    }
    *event = h_new(kobj_at(s->events[index]));
    return *event ? 0 : RC(1, 105);
}
/* Called under aslock and nvev_lk. A worker may retain the device state after its control session closes. The
   address spaces go with the last of them */
static void nv_gsv_devices_unref(u32 id) {
    NvGsvDevices* s = &g_hle.nvdrv.gsv_devices[id - 1];
    if (--s->refs) return;
    for (u32 i = 0; i < NV_GSV_GPU_EVENTS; ++i)
        if (s->events[i]) kobj_unref(kobj_at(s->events[i]));
    for (u32 i = 0; i < NV_GSV_ADDRESS_SPACES; ++i)
        if (g_hle.nvdrv.gsv_as[i].devices == id) POOL_PUT(&g_hle.nvdrv.gsv_as[i]);
    pool_put(s, sizeof *s);
}
/* The GPU worker: GPU-accessible guest memory (101, 102) and its maps into the GSV address space (211 to 213), from
   the SDK's nn::gpu::sdk client and eden-ns2's gpu_session.cpp (copyright 2026 Eden Emulator Project, GPL-3.0-or-later).
   Map/modify/unmap requests signal their notification after the operation, including an operation error. Destruction
   signals only a successful release, not NotFound or Busy. Under aslock (nv_gsv_worker_dispatch), without nvev_lk:
   the page table's renderer bracket may take that lock. */
static int nv_gsv_graphics_live(const NvGsvDevices* s, u32 device) {
    device &= 0xff;
    return device && (s->graphics[device / 32] >> (device % 32) & 1);
}
static int nv_gsv_notification_ok(const NvGsvDevices* s, u32 index) {
    return index < NV_GSV_GPU_EVENTS && s->events[index];
}
static void nv_gsv_notify(const NvGsvDevices* s, u32 index) {
    nvev_lock();
    kevent_signal(kev(s->events[index]));
    nvev_unlock();
}
static NvGsvMemory* nv_gsv_memory(u32 devices, u32 id) {
    NvGsvMemory* m = id - 1 < NV_GSV_MEMORIES ? &g_hle.nvdrv.gsv_mem[id - 1] : NULL;
    return m && m->devices == devices ? m : NULL;
}
/* every page of [addr, addr + size) mapped with the GpuAccessible attribute (svcSetGpuMemoryAttribute) */
static int nv_gpu_accessible(u64 addr, u64 size) {
    u64 end = addr + size;
    while (addr < end) {
        MemBlock b;
        mem_query(addr, &b);
        if (b.state == MS_FREE || !(b.attr & MA_GPU_ACCESSIBLE) || b.addr + b.size <= addr) return 0;
        addr = b.addr + b.size;
    }
    return 1;
}
/* The SDK asks for a kind and its uncompressed fallback. The GPU here keeps no compression, so a map or a modify
   selects the fallback, and the SDK drops its compressed preference (eden-ns2's SelectNativeMemoryKind answers so for the
   pairs it knows). Not measured on a console */
static u32 nv_gsv_kind(u32 fallback) { return fallback & 0xff; }
static int nn_gsv_IGpuWorker_CreateGpuAccessibleMemory(SvcObj* self, u64 first, u64 last, u32 flags, u32 device,
                                                       u32* memory) {
    NvGsvWorker* w = self->data;
    const NvGsvDevices* s = &g_hle.nvdrv.gsv_devices[w->devices - 1];
    u64 size = last - first + 1;
    if (flags & 0xff) {
        WARN_LIMIT(4, "nvdrv: GPU-accessible memory with flags %x, which this runtime doesn't know\n", flags & 0xff);
        R_THROW(RC(1, 33)); /* NotImplemented */
    }
    R_UNLESS(nv_gsv_graphics_live(s, device), RC(1, 121));                       /* NotFound */
    R_UNLESS(last >= first && last != ~(u64)0, RC(1, 101));                      /* InvalidSize */
    R_UNLESS(!(first & GMMU_PAGE_MASK), RC(1, 102));                             /* InvalidAddress */
    R_UNLESS(!(size & GMMU_PAGE_MASK), RC(1, 101));                              /* InvalidSize */
    R_UNLESS(cpu_ok(first, size) && nv_gpu_accessible(first, size), RC(1, 106)); /* InvalidCurrentMemory */
    /* from where the last one was taken: a game holds thousands, and frees few */
    for (u32 k = 0, i = g_hle.nvdrv.gsv_mem_next % NV_GSV_MEMORIES; k < NV_GSV_MEMORIES;
         ++k, i = (i + 1) % NV_GSV_MEMORIES)
        if (!g_hle.nvdrv.gsv_mem[i].devices) {
            g_hle.nvdrv.gsv_mem[i] = (NvGsvMemory){first, size, w->devices};
            g_hle.nvdrv.gsv_mem_next = i + 1;
            *memory = i + 1;
            LOGV("nvdrv: GPU-accessible memory %u: %llx bytes at %llx\n", i + 1, (unsigned long long)size,
                 (unsigned long long)first);
            R_SUCCEED();
        }
    WARN_LIMIT(4, "nvdrv: more than %d GPU-accessible memories at once\n", NV_GSV_MEMORIES);
    return RC(1, 103); /* OutOfResource */
}
/* Do not publish completion while a mapping can still use this registration. The SDK unmaps before destruction. */
static int nn_gsv_IGpuWorker_DestroyGpuAccessibleMemory(SvcObj* self, u32 memory, u32 notification) {
    NvGsvWorker* w = self->data;
    const NvGsvDevices* s = &g_hle.nvdrv.gsv_devices[w->devices - 1];
    NvGsvMemory* m = nv_gsv_memory(w->devices, memory);
    R_UNLESS(nv_gsv_notification_ok(s, notification), RC(1, 116)); /* InvalidCombination */
    R_UNLESS(m, RC(1, 121));
    for (u32 i = 0; i < g_hle.nvdrv.ngmaps; ++i) R_UNLESS(g_hle.nvdrv.gmaps[i].memory != memory, RC(1, 122)); /* Busy */
    POOL_PUT(m);
    nv_gsv_notify(s, notification);
    R_SUCCEED();
}
/* whether a mapping of anyone's or a sparse region covers a page of [gva, gva + size) */
static int gsv_va_taken(u64 gva, u64 size) {
    for (u32 k = 0; k < g_hle.nvdrv.ngmaps; k++) {
        const GMap* g = &g_hle.nvdrv.gmaps[k];
        if (g->gva < gva + size && gva < g->gva + g->size) return 1;
    }
    for (u32 i = 0; i < NV_GSV_SPARSE_REGIONS; ++i) {
        const NvGsvSparse* r = &g_hle.nvdrv.gsv_sparse[i];
        if (r->devices && r->first < gva + size && gva < r->first + r->size) return 1;
    }
    return 0;
}
static int nv_gsv_map(u32 devices, const NvGsvDevices* s, const nn_gsv_GpuMapRequest* r, u32* kind) {
    u64 first = snx_ld_le64(r->first), last = snx_ld_le64(r->last), offset = snx_ld_le64(r->offset);
    u64 size = last - first + 1;
    const NvGsvAs* as = nv_gsv_as(devices, r->address_space);
    const NvGsvMemory* m = nv_gsv_memory(devices, r->memory);
    R_UNLESS(as && as->registered && m && nv_gsv_graphics_live(s, r->device), RC(1, 121));
    /* write access, uncached or cached, the 4 KiB mode, and synchronous or not, as eden-ns2 takes them */
    R_UNLESS(!(r->read_only & 0xff) && (r->cache & 0xff) <= 1 && !(r->mode & 0xff) && (r->sync & 0xff) <= 1, RC(1, 33));
    R_UNLESS(last >= first && size && !((first | size | offset) & GMMU_PAGE_MASK), RC(1, 101));
    R_UNLESS(offset <= m->size && size <= m->size - offset, RC(1, 101));
    R_UNLESS(first >= NV_GSV_USER_FIRST && last < NV_GSV_USER_END && !gsv_va_taken(first, size), RC(1, 106));
    gmmu_map(first, m->addr + offset, size);
    GMap* g = gmap_add();
    g->gva = g->base = first;
    g->size = size;
    g->cpu = m->addr + offset;
    g->gsv = devices;
    g->memory = r->memory;
    *kind = nv_gsv_kind(r->fallback_kind);
    LOGV("nvdrv: GPU worker maps memory %u +%llx: %llx bytes at %llx\n", r->memory, (unsigned long long)offset,
         (unsigned long long)size, (unsigned long long)first);
    R_SUCCEED();
}
static int nn_gsv_IGpuWorker_NormalMapGpuMemory4K(SvcObj* self, const nn_gsv_GpuMapRequest* request, u32* kind) {
    NvGsvWorker* w = self->data;
    const NvGsvDevices* s = &g_hle.nvdrv.gsv_devices[w->devices - 1];
    R_UNLESS(nv_gsv_notification_ok(s, request->notification), RC(1, 116));
    int rc = nv_gsv_map(w->devices, s, request, kind);
    nv_gsv_notify(s, request->notification);
    return rc;
}
/* A kind change inside one of the worker's maps: the page table keeps no kinds */
static int nn_gsv_IGpuWorker_NormalModifyGpuMemory4K(SvcObj* self, const nn_gsv_GpuModifyRequest* request, u32* kind) {
    NvGsvWorker* w = self->data;
    const NvGsvDevices* s = &g_hle.nvdrv.gsv_devices[w->devices - 1];
    u64 first = snx_ld_le64(request->first), last = snx_ld_le64(request->last);
    int rc = RC(1, 121);
    R_UNLESS(nv_gsv_notification_ok(s, request->notification) && (request->sync & 0xff) <= 1, RC(1, 116));
    const NvGsvAs* as = nv_gsv_as(w->devices, request->address_space);
    for (u32 k = 0; as && as->registered && k < g_hle.nvdrv.ngmaps && rc; k++) {
        const GMap* g = &g_hle.nvdrv.gmaps[k];
        if (g->gsv == w->devices && last >= first && last != ~(u64)0 && !((first | (last + 1)) & GMMU_PAGE_MASK) &&
            first >= g->gva && last - g->gva < g->size)
            rc = 0;
    }
    if (!rc) *kind = nv_gsv_kind(request->fallback_kind);
    nv_gsv_notify(s, request->notification);
    return rc;
}
/* base 0 removes all of a worker's maps, otherwise only the pieces of the map with that base. GSV bases are nonzero. */
static void nv_gsv_drop_maps(u32 devices, u64 base) {
    for (u32 k = 0; k < g_hle.nvdrv.ngmaps;) {
        GMap g = g_hle.nvdrv.gmaps[k];
        if (g.gsv != devices || (base && g.base != base)) { /* a sparse piece's base is 0: only base 0 takes it */
            k++;
            continue;
        }
        gmmu_unmap(g.gva, g.size, 1, g.cpu);
        g_hle.nvdrv.gmaps[k] = g_hle.nvdrv.gmaps[--g_hle.nvdrv.ngmaps];
    }
}
/* the pieces of the worker's map at first, as many bytes as it made: NotFound when that isn't one of its maps */
static int nv_gsv_unmap(u32 devices, u64 first, u64 size) {
    u64 mapped = 0;
    for (u32 k = 0; k < g_hle.nvdrv.ngmaps; k++)
        if (g_hle.nvdrv.gmaps[k].gsv == devices && g_hle.nvdrv.gmaps[k].base == first)
            mapped += g_hle.nvdrv.gmaps[k].size;
    R_UNLESS(mapped && mapped == size, RC(1, 121));
    nv_gsv_drop_maps(devices, first);
    R_SUCCEED();
}
static int nn_gsv_IGpuWorker_NormalUnmapGpuMemory4K(SvcObj* self, const nn_gsv_GpuUnmapRequest* request, u64* size) {
    NvGsvWorker* w = self->data;
    const NvGsvDevices* s = &g_hle.nvdrv.gsv_devices[w->devices - 1];
    u64 first = snx_ld_le64(request->first), last = snx_ld_le64(request->last);
    R_UNLESS(nv_gsv_notification_ok(s, request->notification) && (request->sync & 0xff) <= 1, RC(1, 116));
    const NvGsvAs* as = nv_gsv_as(w->devices, request->address_space);
    int rc = as && as->registered && last >= first && last != ~(u64)0 && !((first | (last + 1)) & GMMU_PAGE_MASK)
                 ? nv_gsv_unmap(w->devices, first, last - first + 1)
                 : RC(1, 121);
    if (!rc) *size = last - first + 1;
    nv_gsv_notify(s, request->notification);
    return rc;
}
/* Sparse regions (214 to 216), from the SDK's nn::gpu::sdk::CreateSparseRegion, DestroySparseRegion and
   ExecuteSparseMapRequests, and eden-ns2's gpu_session.cpp (copyright 2026 Eden Emulator Project, GPL-3.0-or-later).
   NVN's NvRmGpuAddressSpaceAllocationCreate makes its sparse allocations with 214, and NvRm's remap of them comes as
   216. The SDK waits for the notification after each, also after an error. A page of a region without memory maps
   nothing, as an unmapped page */
static NvGsvSparse* nv_gsv_sparse_at(u32 devices, u64 gva, u64 size) {
    for (u32 i = 0; i < NV_GSV_SPARSE_REGIONS; ++i) {
        NvGsvSparse* r = &g_hle.nvdrv.gsv_sparse[i];
        if (r->devices == devices && gva >= r->first && gva - r->first < r->size && size <= r->size - (gva - r->first))
            return r;
    }
    return NULL;
}
static int nv_gsv_sparse_create(u32 devices, const NvGsvDevices* s, const nn_gsv_GpuSparseRegionRequest* r) {
    u64 first = snx_ld_le64(r->first), last = snx_ld_le64(r->last);
    u32 page_bits = r->page_shift & 0xff;
    const NvGsvAs* as = nv_gsv_as(devices, r->address_space);
    R_UNLESS(as && as->registered && nv_gsv_graphics_live(s, r->device), RC(1, 121));
    R_UNLESS((page_bits == 12 || page_bits == 16) && (r->mode & 0xff) <= 1 && (r->sync & 0xff) <= 1, RC(1, 33));
    u64 page_mask = ((u64)1 << page_bits) - 1;
    R_UNLESS(last >= first && last != ~(u64)0 && !(first & page_mask) && !((last + 1) & page_mask), RC(1, 101));
    u64 size = last - first + 1;
    R_UNLESS(first >= NV_GSV_USER_FIRST && last < NV_GSV_USER_END && !gsv_va_taken(first, size), RC(1, 106));
    for (u32 i = 0; i < NV_GSV_SPARSE_REGIONS; ++i)
        if (!g_hle.nvdrv.gsv_sparse[i].devices) {
            g_hle.nvdrv.gsv_sparse[i] = (NvGsvSparse){first, size, devices};
            LOGV("nvdrv: GPU worker reserves sparse region %u: %llx bytes at %llx\n", i + 1, (unsigned long long)size,
                 (unsigned long long)first);
            R_SUCCEED();
        }
    WARN_LIMIT(4, "nvdrv: more than %d sparse GPU regions at once\n", NV_GSV_SPARSE_REGIONS);
    return RC(1, 103); /* OutOfResource */
}
static int nn_gsv_IGpuWorker_CreateSparseRegion(SvcObj* self, const nn_gsv_GpuSparseRegionRequest* request) {
    NvGsvWorker* w = self->data;
    const NvGsvDevices* s = &g_hle.nvdrv.gsv_devices[w->devices - 1];
    R_UNLESS(nv_gsv_notification_ok(s, request->notification), RC(1, 116));
    int rc = nv_gsv_sparse_create(w->devices, s, request);
    nv_gsv_notify(s, request->notification);
    return rc;
}
/* the region's maps, then the region */
static void nv_gsv_sparse_free(NvGsvSparse* r) {
    u32 id = (u32)(r - g_hle.nvdrv.gsv_sparse) + 1;
    for (u32 k = 0; k < g_hle.nvdrv.ngmaps;) {
        GMap g = g_hle.nvdrv.gmaps[k];
        if (g.sparse != id) {
            k++;
            continue;
        }
        gmmu_unmap(g.gva, g.size, 1, g.cpu);
        g_hle.nvdrv.gmaps[k] = g_hle.nvdrv.gmaps[--g_hle.nvdrv.ngmaps];
    }
    POOL_PUT(r);
}
static int nn_gsv_IGpuWorker_DestroySparseRegion(SvcObj* self, const nn_gsv_GpuUnmapRequest* request) {
    NvGsvWorker* w = self->data;
    const NvGsvDevices* s = &g_hle.nvdrv.gsv_devices[w->devices - 1];
    u64 first = snx_ld_le64(request->first), last = snx_ld_le64(request->last);
    R_UNLESS(nv_gsv_notification_ok(s, request->notification) && (request->sync & 0xff) <= 1, RC(1, 116));
    const NvGsvAs* as = nv_gsv_as(w->devices, request->address_space);
    NvGsvSparse* r = as && as->registered && last >= first && last != ~(u64)0
                         ? nv_gsv_sparse_at(w->devices, first, last - first + 1)
                         : NULL;
    int rc = r && r->first == first ? 0 : RC(1, 121);
    if (!rc) nv_gsv_sparse_free(r);
    nv_gsv_notify(s, request->notification);
    return rc;
}
/* A sparse map request in 216's buffer: the SDK's util::ExecuteVirtualMachineRemap makes one from each
   nvgpu_as_remap_op, its pages in bytes, NVGPU_AS_REMAP_OP_FLAGS bit 7 (no write) as read_only, bit 2 as cache and bit
   15 in its own byte. It zeroes the bytes between the fields, and memory 0 unmaps */
#define GSV_SPARSE_REQUEST_BYTES 0x28
#define GSV_SPARSE_ADDRESS       0x00
#define GSV_SPARSE_SIZE          0x08
#define GSV_SPARSE_READ_ONLY     0x10
#define GSV_SPARSE_CACHE         0x11
#define GSV_SPARSE_FALLBACK_KIND 0x14
#define GSV_SPARSE_FLAG15        0x15
#define GSV_SPARSE_MEMORY        0x18
#define GSV_SPARSE_OFFSET        0x20
#define GSV_SPARSE_REQUESTS_MAX  256 /* eden-ns2's bound. The SDK sends 64 at most */
typedef struct {
    u64 gva, size, cpu;
    u32 sparse, memory, kind;
} GsvSparseMap;
/* Checks every request before the first changes the page table, so a request that fails changes nothing */
static int nv_gsv_sparse_map(u32 devices, IpcBuf requests, IpcBuf kinds, const nn_gsv_GpuSparseMapArguments* args,
                             u64* processed) {
    GsvSparseMap maps[GSV_SPARSE_REQUESTS_MAX];
    const NvGsvAs* as = nv_gsv_as(devices, args->address_space);
    R_UNLESS(as && as->registered, RC(1, 121));
    R_UNLESS(snx_ld_le64(args->one) == 1 && snx_ld_le64(args->request_size) == GSV_SPARSE_REQUEST_BYTES &&
                 (args->sync & 0xff) <= 1,
             RC(1, 33));
    u64 n = requests.size / GSV_SPARSE_REQUEST_BYTES;
    R_UNLESS(n && n <= GSV_SPARSE_REQUESTS_MAX && requests.size % GSV_SPARSE_REQUEST_BYTES == 0 && kinds.size >= n,
             RC(1, 101));
    const u8* in = nv_buffer(requests);
    u8* out = nv_buffer(kinds);
    R_UNLESS(in && out, RC(1, 106));
    for (u64 i = 0; i < n; ++i) {
        const u8* q = in + i * GSV_SPARSE_REQUEST_BYTES;
        u64 gva = snx_ld_le64(q + GSV_SPARSE_ADDRESS), size = snx_ld_le64(q + GSV_SPARSE_SIZE);
        u64 offset = snx_ld_le64(q + GSV_SPARSE_OFFSET);
        u32 memory = snx_ld_le32(q + GSV_SPARSE_MEMORY);
        /* the page table has no write protection, and what bit 15 asks for is unknown */
        R_UNLESS(!q[GSV_SPARSE_READ_ONLY] && q[GSV_SPARSE_CACHE] <= 1 && !q[GSV_SPARSE_FLAG15] && !q[0x12] &&
                     !snx_ld_le16(q + 0x16) && !snx_ld_le32(q + 0x1c) && (memory || !offset),
                 RC(1, 33));
        R_UNLESS(size && !((gva | size | offset) & GMMU_PAGE_MASK), RC(1, 101));
        const NvGsvSparse* r = nv_gsv_sparse_at(devices, gva, size);
        R_UNLESS(r, RC(1, 106));
        u64 cpu = 0;
        if (memory) {
            const NvGsvMemory* m = nv_gsv_memory(devices, memory);
            R_UNLESS(m, RC(1, 121));
            R_UNLESS(offset <= m->size && size <= m->size - offset, RC(1, 101));
            cpu = m->addr + offset;
        }
        maps[i] = (GsvSparseMap){gva, size, cpu, (u32)(r - g_hle.nvdrv.gsv_sparse) + 1, memory,
                                 nv_gsv_kind(q[GSV_SPARSE_FALLBACK_KIND])};
    }
    for (u64 i = 0; i < n; ++i) {
        const GsvSparseMap* p = &maps[i];
        gmmu_unmap(p->gva, p->size, 0, 0);
        gmap_cut(p->gva, p->size, p->sparse);
        if (p->memory) {
            gmmu_map(p->gva, p->cpu, p->size);
            GMap* g = gmap_add();
            g->gva = p->gva;
            g->size = p->size;
            g->cpu = p->cpu;
            g->gsv = devices;
            g->memory = p->memory;
            g->sparse = p->sparse;
        }
        out[i] = (u8)p->kind;
    }
    LOGV("nvdrv: GPU worker remaps %llu pieces of sparse regions\n", (unsigned long long)n);
    *processed = n;
    R_SUCCEED();
}
static int nn_gsv_IGpuWorker_ExecuteSparseMapRequests(SvcObj* self, IpcBuf requests, IpcBuf kinds,
                                                      const nn_gsv_GpuSparseMapArguments* args, u64* processed) {
    NvGsvWorker* w = self->data;
    const NvGsvDevices* s = &g_hle.nvdrv.gsv_devices[w->devices - 1];
    R_UNLESS(nv_gsv_notification_ok(s, args->notification), RC(1, 116));
    int rc = nv_gsv_sparse_map(w->devices, requests, kinds, args, processed);
    nv_gsv_notify(s, args->notification);
    return rc;
}
static int nv_gsv_worker_dispatch(SvcObj* self, IpcCtx* c) {
    R_UNLESS(c->tipc, RC(1, 33));
    pm_lock(&g_host.nvdrv.aslock);
    int rc = nn_gsv_IGpuWorker_dispatch(self, c);
    pm_unlock(&g_host.nvdrv.aslock);
    return rc;
}
/* The worker's maps, sparse regions and memories go with it, as eden-ns2's IGpuWorker drops them, before nvev_lk */
static void nv_gsv_worker_destroy(SvcObj* self) {
    NvGsvWorker* s = self->data;
    /* session_new can fail before the worker takes a share of the device state. */
    if (!s->devices) return;
    pm_lock(&g_host.nvdrv.aslock);
    nv_gsv_drop_maps(s->devices, 0);
    for (u32 i = 0; i < NV_GSV_SPARSE_REGIONS; ++i)
        if (g_hle.nvdrv.gsv_sparse[i].devices == s->devices) POOL_PUT(&g_hle.nvdrv.gsv_sparse[i]);
    for (u32 i = 0; i < NV_GSV_MEMORIES; ++i)
        if (g_hle.nvdrv.gsv_mem[i].devices == s->devices) POOL_PUT(&g_hle.nvdrv.gsv_mem[i]);
    nvev_lock();
    nv_gsv_devices_unref(s->devices);
    nvev_unlock();
    pm_unlock(&g_host.nvdrv.aslock);
}
static int nn_gsv_IGpuSession_RequestWorker(SvcObj* self, u32 index) {
    NvGsvGpu* s = self->data;
    NvGsvDevices* devices = &g_hle.nvdrv.gsv_devices[s->devices - 1];
    R_UNLESS(index < NV_GSV_GPU_EVENTS, RC(1, 116));
    R_UNLESS(devices->events[index], RC(1, 125));
    if (!s->worker) {
        SvcObj* worker = SVCOBJ_NEW_DATA("nn::gsv::IGpuWorker", nv_gsv_worker_dispatch, g_hle.nvdrv.gsv_workers,
                                         g_hle.nvdrv.gsv_worker_next, "GSV GPU workers");
        worker->destroy = nv_gsv_worker_destroy;
        u32 handle = session_new(worker);
        R_UNLESS(handle, RC(1, 105));
        ((NvGsvWorker*)worker->data)->devices = s->devices;
        ++devices->refs;
        s->worker = kobj_id(h_get_ref(handle, KO_SESSION));
        h_close(handle);
    }
    kevent_signal(kev(devices->events[index]));
    R_SUCCEED();
}
static int nn_gsv_IGpuSession_AcquireWorker(SvcObj* self, u32* worker) {
    NvGsvGpu* s = self->data;
    R_UNLESS(s->worker, RC(1, 125));
    *worker = h_new(kobj_at(s->worker));
    return *worker ? 0 : RC(1, 105);
}
/* The graphics devices and address spaces of nn::gpu::sdk, from its client (CreateGraphicsDevice,
   CreateGpuAddressSpace, Get{User,Gsv}PartitionGpuAddress{Min,Max}) and eden-ns2's gpu_session.cpp (copyright 2026 Eden
   Emulator Project, GPL-3.0-or-later). The SDK names a device by a hash of its name, and keeps the id in a byte */
static int nn_gsv_IGpuSession_CreateGraphicsDevice(SvcObj* self, u64 name, u64 flags, u64 debug_label, u32* id) {
    NvGsvGpu* gpu = self->data;
    NvGsvDevices* s = &g_hle.nvdrv.gsv_devices[gpu->devices - 1];
    (void)name;
    (void)debug_label;
    R_UNLESS(!flags, RC(1, 33)); /* NotImplemented: the SDK's callers pass 0 */
    for (u32 i = 1; i < NV_GSV_GRAPHICS_DEVICES; ++i)
        if (!(s->graphics[i / 32] >> (i % 32) & 1)) {
            s->graphics[i / 32] |= 1u << (i % 32);
            *id = i;
            R_SUCCEED();
        }
    return RC(1, 103); /* OutOfResource */
}
static int nn_gsv_IGpuSession_DestroyGraphicsDevice(SvcObj* self, u8 id) {
    NvGsvGpu* gpu = self->data;
    NvGsvDevices* s = &g_hle.nvdrv.gsv_devices[gpu->devices - 1];
    R_UNLESS(s->graphics[id / 32] >> (id % 32) & 1, RC(1, 121)); /* NotFound */
    s->graphics[id / 32] &= ~(1u << (id % 32));
    R_SUCCEED();
}
/* The SDK keeps an address space for each flags value it asks for. Its GPU driver asks for flags 0 */
static int nn_gsv_IGpuSession_CreateAddressSpace(SvcObj* self, u32 flags, u32* id) {
    NvGsvGpu* gpu = self->data;
    R_UNLESS(!flags, RC(1, 116)); /* InvalidCombination, as eden-ns2 answers */
    for (u32 i = 0; i < NV_GSV_ADDRESS_SPACES; ++i)
        if (!g_hle.nvdrv.gsv_as[i].devices) {
            R_UNLESS(g_hle.nvdrv.gsv_as_last < UINT32_MAX, RC(1, 103));
            NvGsvAs* as = &g_hle.nvdrv.gsv_as[i];
            as->devices = gpu->devices;
            as->id = ++g_hle.nvdrv.gsv_as_last;
            *id = as->id;
            R_SUCCEED();
        }
    WARN_LIMIT(4, "nvdrv: a second GPU address space, which the one GPU page table can't give\n");
    return RC(1, 103); /* OutOfResource */
}
/* The user partition, the GPU addresses the SDK allocates from (nvdrv.h). The GSV partition, which the supervisor
   keeps for itself, is empty: first past last, as eden-ns2 answers */
static int nn_gsv_IGpuSession_GetUserPartition(SvcObj* self, u32 id, u64* first, u64* last) {
    NvGsvGpu* gpu = self->data;
    R_UNLESS(nv_gsv_as(gpu->devices, id), RC(1, 121));
    *first = NV_GSV_USER_FIRST;
    *last = NV_GSV_USER_END - 1;
    R_SUCCEED();
}
static int nn_gsv_IGpuSession_GetGsvPartition(SvcObj* self, u32 id, u64* first, u64* last) {
    NvGsvGpu* gpu = self->data;
    R_UNLESS(nv_gsv_as(gpu->devices, id), RC(1, 121));
    *first = ~(u64)0;
    *last = 0;
    R_SUCCEED();
}
/* The newer SDK aborts on more than four ranges, and takes only the first */
static int nn_gsv_IGpuSession_GetUserPartitions(SvcObj* self, u32 id, u64* count, nn_gsv_GpuPartitionRanges* ranges) {
    NvGsvGpu* gpu = self->data;
    R_UNLESS(nv_gsv_as(gpu->devices, id), RC(1, 121));
    *count = 1;
    *ranges = (nn_gsv_GpuPartitionRanges){{NV_GSV_USER_FIRST, NV_GSV_USER_END - 1}};
    R_SUCCEED();
}
int nv_gsv_register_as(u32 id) {
    pm_lock(&g_host.nvdrv.aslock);
    NvGsvAs* as = nv_gsv_as(0, id);
    int rc = as ? 0 : RC(1, 121);
    if (!rc) as->registered = 1;
    pm_unlock(&g_host.nvdrv.aslock);
    return rc;
}
/* GSV native memory, after eden-ns2's graphics_supervisor.cpp (copyright 2026 Eden Emulator Project, GPL-3.0-or-later):
   an nvmap handle, as the SDK reads its NvRm memory id. The handle size is 32 bits here */
int nv_native_memory_create(u64 size, u32* memory) {
    R_UNLESS(size && size <= UINT32_MAX, RC(1, 101)); /* InvalidSize */
    u32 h = nvmap_handle_make((u32)size);
    R_UNLESS(h, RC(1, 103)); /* OutOfResource */
    pm_lock(&g_host.nvdrv.lock);
    g_hle.nvdrv.nvh[h].native = 1;
    pm_unlock(&g_host.nvdrv.lock);
    *memory = h;
    R_SUCCEED();
}
/* The SDK's CreateNativeMemoryWithGpukMemHandle names the guest address of its offset in the parent */
int nv_native_memory_bind(u32 memory, u64 addr, u64 size, u32 parent, u64 offset) {
    pm_lock(&g_host.nvdrv.aslock);
    const NvGsvMemory* p = parent - 1 < NV_GSV_MEMORIES && g_hle.nvdrv.gsv_mem[parent - 1].devices
                               ? &g_hle.nvdrv.gsv_mem[parent - 1]
                               : NULL;
    pm_lock(&g_host.nvdrv.lock);
    NvMapH* m = nvmap_live(memory);
    int rc = !m || !m->native || !p           ? RC(1, 121) /* NotFound */
             : m->allocated                   ? RC(1, 125) /* InvalidState */
             : size != m->size                ? RC(1, 101)
             : offset > p->size || size > p->size - offset || addr != p->addr + offset ? RC(1, 106) /* InvalidCurrentMemory */
             : !page_aligned(addr)            ? RC(1, 102) /* InvalidAddress */
                                              : 0;
    if (!rc) {
        m->addr = addr;
        m->allocated = 1;
    }
    pm_unlock(&g_host.nvdrv.lock);
    pm_unlock(&g_host.nvdrv.aslock);
    return rc;
}
/* Busy while an image holds the memory */
int nv_native_memory_destroy(u32 memory) {
    int retire = 0;
    pm_lock(&g_host.nvdrv.lock);
    NvMapH* m = nvmap_live(memory);
    int rc = !m || !m->native ? RC(1, 121) : m->refs != 1 ? RC(1, 122) /* Busy */ : 0;
    if (!rc && !--m->refs && !m->pins) {
        m->alive = m->allocated = 0;
        retire = 1;
    }
    pm_unlock(&g_host.nvdrv.lock);
    if (retire) nvmap_retire(memory);
    return rc;
}
int nv_native_image_create(u32 memory, u64 size, u32 count, const NvGsvImageShape* shape, u32* image) {
    pm_lock(&g_host.nvdrv.lock);
    NvMapH* m = nvmap_live(memory);
    NvGsvImage* slot = NULL;
    for (u32 i = 0; i < NV_GSV_IMAGES && !slot; ++i)
        if (!g_hle.nvdrv.gsv_images[i].handle) slot = &g_hle.nvdrv.gsv_images[i];
    int rc = !m || !m->native || !m->allocated               ? RC(1, 121)
             : size > m->size                                ? RC(1, 101)
             : !slot || count > UINT32_MAX - g_hle.nvdrv.gsv_image_last ? RC(1, 103) /* handles never wrap */
                                                             : 0;
    if (!rc) {
        m->refs++;
        *slot = (NvGsvImage){.handle = g_hle.nvdrv.gsv_image_last + 1, .count = count, .memory = memory,
                             .shape = *shape};
        g_hle.nvdrv.gsv_image_last += count;
        *image = slot->handle;
    }
    pm_unlock(&g_host.nvdrv.lock);
    return rc;
}
/* the image whose first handle is image, under nv.lock */
static NvGsvImage* nv_native_image(u32 image) {
    for (u32 i = 0; i < NV_GSV_IMAGES && image; ++i)
        if (g_hle.nvdrv.gsv_images[i].handle == image) return &g_hle.nvdrv.gsv_images[i];
    return NULL;
}
/* Busy while a native window's alias image pool */
int nv_native_image_destroy(u32 image) {
    pm_lock(&g_host.nvdrv.lock);
    NvGsvImage* slot = nv_native_image(image);
    int rc = !slot ? RC(1, 121) : slot->window_pool ? RC(1, 122) : 0;
    if (!rc) {
        g_hle.nvdrv.nvh[slot->memory].refs--; /* the memory's own reference stays until 112 */
        *slot = (NvGsvImage){0};
    }
    pm_unlock(&g_host.nvdrv.lock);
    return rc;
}
/* The SDK's native window makes an alias pool of count 1 for each image it inserts, which it made with count 1. A
   count other than the image's is refused */
int nv_native_image_window_pool(u32 image, u32 count, int create) {
    pm_lock(&g_host.nvdrv.lock);
    NvGsvImage* slot = nv_native_image(image);
    int rc = !slot                                ? RC(1, 121)
             : create && count != slot->count     ? RC(1, 33)
             : slot->window_pool == create        ? RC(1, 125)
                                                  : 0;
    if (!rc) slot->window_pool = create;
    pm_unlock(&g_host.nvdrv.lock);
    return rc;
}
int nv_native_window_image(u32 image, u32* memory, NvGsvImageShape* shape) {
    pm_lock(&g_host.nvdrv.lock);
    NvGsvImage* slot = nv_native_image(image);
    int rc = slot && slot->window_pool && slot->count == 1 ? 0 : RC(1, 121);
    if (!rc) {
        *memory = slot->memory;
        *shape = slot->shape;
    }
    pm_unlock(&g_host.nvdrv.lock);
    return rc;
}
/* The address spaces and graphics devices are under aslock, the events and shares under nvev_lk */
static int nv_gsv_gpu_dispatch(SvcObj* self, IpcCtx* c) {
    R_UNLESS(c->tipc, RC(1, 33));
    c->tipc_error_moves = c->cmd == 12 || c->cmd == 22 ? 1 : 0;
    pm_lock(&g_host.nvdrv.aslock);
    nvev_lock();
    int rc = nn_gsv_IGpuSession_dispatch(self, c);
    nvev_unlock();
    pm_unlock(&g_host.nvdrv.aslock);
    return rc;
}
static void nv_gsv_gpu_destroy(SvcObj* self) {
    NvGsvGpu* s = self->data;
    pm_lock(&g_host.nvdrv.aslock);
    nvev_lock();
    nv_gsv_devices_unref(s->devices);
    nvev_unlock();
    pm_unlock(&g_host.nvdrv.aslock);
    /* Dropping the retained session may destroy its worker, which takes both locks itself. */
    kobj_unref(kobj_at(s->worker));
}
SvcObj* nv_gpu_new(void) {
    SvcObj* s = SVCOBJ_NEW_DATA("nn::gsv::IGpuSession", nv_gsv_gpu_dispatch,
                              g_hle.nvdrv.gsv_gpu, g_hle.nvdrv.gsv_gpu_next, "GSV GPU sessions");
    s->destroy = nv_gsv_gpu_destroy;
    nvev_lock();
    NvGsvDevices* devices = POOL_TAKE(g_hle.nvdrv.gsv_devices, g_hle.nvdrv.gsv_devices_next, "GSV GPU device states");
    devices->refs = 1;
    ((NvGsvGpu*)s->data)->devices = 1 + (u32)(devices - g_hle.nvdrv.gsv_devices);
    nvev_unlock();
    return s;
}

void nv_init(void) {
    g_hle.nvdrv.nvh_next = 1;
    g_hle.nvdrv.big_page_bits = AS_BIG_PAGE_BITS_64K; /* MemoryManager's default big_page_bits */
    pm_init(&g_host.nvdrv.lock);
    pm_init(&g_host.nvdrv.aslock);
    pm_init(&g_host.nvdrv.alias_lock);
    pm_init(&g_host.nvdrv.nvev_lk);
    pm_init(&g_host.nvdrv.sync_lk);
    pm_init(&g_host.nvdrv.submit_lk);
    pc_init(&g_host.nvdrv.sync_cv);
    nns_nvdrv_INvDrvServices_register(new_nvdrv);
}
/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_nvdrv_state_fns,
               STATE_FN("nvdrv", gpu_channel_retire),
               STATE_FN("nvdrv", gpu_tsg_retire),
               STATE_FN("nvdrv", nv_gsv_sync_dispatch),
               STATE_FN("nvdrv", nv_gsv_sync_destroy),
               STATE_FN("nvdrv", nv_gsv_gpu_dispatch),
               STATE_FN("nvdrv", nv_gsv_gpu_destroy),
               STATE_FN("nvdrv", nv_gsv_worker_dispatch),
               STATE_FN("nvdrv", nv_gsv_worker_destroy));
