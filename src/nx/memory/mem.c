/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/kernel/k_address_space_info.cpp, k_page_table_base.cpp):
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project */
/* the guest address space: the host reservation every guest address is an offset into, its regions, and the list of
   memory blocks with the state, permission and attributes QueryMemory reports */
#include "nx/memory/mem.h"
#include "common/utilities/common.h"
#include "nx/memory/own.h"
#include "nx/gpu/gpu.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GIB  (1ull << 30)
#define PAGE 0x1000ull

/* The 39-bit layout of a 64-bit application without ASLR, as Eden's KPageTableBase::InitializeForProcess lays it out
   from k_address_space_info.cpp's sizes: code at 0x8000000, then the 64 GiB alias region, the 8 GiB heap region and
   the 2 GiB stack region. The console's address space is 512 GiB; the host reservation is AS_SIZE (128 GiB), so the
   address space (the ASLR region of GetInfo 12 and 13) ends there, and the kernel map region, 64 GiB on the console,
   is the 1 GiB the kernel's own pages (TLS) need. The alias region starts at 4 GiB, which leaves the code below it
   up to 3.9 GiB, as the end of the code isn't known yet when the layout is made */
MemLayout g_layout = {0x8000000ull,  AS_MAX,      0x1100000000ull, 8 * GIB, 0x100000000ull, 64 * GIB,
                      0x1300000000ull, 2 * GIB, 0x1380000000ull, 0x13C0000000ull};
u64 g_as_size = AS_MAX;
u8* g_mem;
u64 g_heap_size, g_phys_mapped, g_heap_max, g_code_memory, g_main_stack_size;
u64 g_pool_size = 64 * GIB; /* sparse: only the pages in use take memory */

#if defined(SNX_TRACY)
/* Guest mappings as Tracy events; g_events is kept under g_mem_lk. */
typedef struct {
    u64 addr, size;
} GuestEvent;
typedef struct {
    GuestEvent* v;
    u32 n, cap;
} GuestEvents;
static GuestEvents g_events;

static void ev_reserve(u32 n) {
    GuestEvent* v;
    u32 cap;
    if (n <= g_events.cap) return;
    cap = g_events.cap ? g_events.cap : 32;
    while (cap < n) cap *= 2;
    v = (GuestEvent*)realloc(g_events.v, sizeof *v * (size_t)cap);
    if (!v) fatal("Out of memory for the guest memory events");
    g_events.v = v;
    g_events.cap = cap;
}
static void ev_add(u64 addr, u64 size) {
    u32 i;
    if (!size) return;
    ev_reserve(g_events.n + 1);
    for (i = 0; i < g_events.n && g_events.v[i].addr < addr; i++) {}
    memmove(&g_events.v[i + 1], &g_events.v[i], sizeof *g_events.v * (size_t)(g_events.n - i));
    g_events.v[i] = (GuestEvent){addr, size};
    g_events.n++;
    SNX_MEM_ALLOC((const void*)(uintptr_t)addr, size, "guest");
}
static void ev_remove(u64 addr, u64 size) {
    GuestEvent* old = g_events.v;
    GuestEvent* out;
    u32 n = g_events.n, i, m = 0;
    u64 end = addr + size;
    if (!n || !size) return;
    /* n + 2: the range can split one allocation on each side. */
    out = (GuestEvent*)malloc(sizeof *out * (size_t)(n + 2));
    if (!out) fatal("Out of memory for the guest memory events");
    for (i = 0; i < n; i++) {
        u64 a = old[i].addr, e = a + old[i].size;
        if (e <= addr || a >= end) {
            out[m++] = old[i];
            continue;
        }
        SNX_MEM_FREE((const void*)(uintptr_t)a, "guest");
        if (a < addr) {
            out[m++] = (GuestEvent){a, addr - a};
            SNX_MEM_ALLOC((const void*)(uintptr_t)a, addr - a, "guest");
        }
        if (e > end) {
            out[m++] = (GuestEvent){end, e - end};
            SNX_MEM_ALLOC((const void*)(uintptr_t)end, e - end, "guest");
        }
    }
    free(old);
    g_events.v = out;
    g_events.n = m;
    g_events.cap = n + 2;
}
#else
#define ev_add(addr, size)   ((void)0)
#define ev_remove(addr, size) ((void)0)
#endif

/* The 32-bit layouts (Eden's 32-bit entries in k_address_space_info.cpp): the code region from 2 MiB to 1 GiB, which
   is the stack region too, then from 1 GiB the 1 GiB alias region and the 1 GiB heap region, or a 2 GiB heap region
   without an alias region. The kernel's pages go in the last GiB, above both */
void mem_layout32(unsigned aspace) {
    g_layout.base = AS32_CODE_BASE;
    g_layout.end = 4 * GIB;
    g_layout.stack = AS32_CODE_BASE;
    g_layout.stack_size = GIB - AS32_CODE_BASE;
    g_layout.alias = GIB;
    if (aspace == 2) {
        g_layout.alias_size = 0;
        g_layout.heap = GIB;
        g_layout.heap_size = 2 * GIB;
    } else {
        g_layout.alias_size = GIB;
        g_layout.heap = 2 * GIB;
        g_layout.heap_size = GIB;
    }
    g_layout.kern = 3 * GIB;
    g_layout.kern_end = 4 * GIB;
}

/* ============================================================ the block list */
/* The blocks cover [0, AS_END) in address order, each a run of pages with one state, permission and attribute set,
   and no two neighbours alike. Free pages have no host memory behind them (decommitted), every other block has, but
   the host commits whole host pages (g_host_page): one larger than the Switch's 4 KiB also commits the free guest
   pages beside a mapped one, which then read zero (commit_nl, decommit_nl).
   Everything here is under g_mem_lk; a change of host memory also holds the renderer's memory lock around it
   (g_gpu_mapping_begin, taken first), which may query the list itself */
static PMutex g_mem_lk = PMUTEX_INIT;
static u64 g_host_page = PAGE; /* plat_vm_page: set by mem_init, before other threads */
static MemBlock* g_blk;
static u32 g_nblk, g_capblk;
/* whether this thread holds g_mem_lk: a crash inside mem.c is reported by the thread that holds it, and the report's
   queries (mem_query) read the list without taking the lock again */
static __thread int t_mem_held;
static void mem_lock(void) {
    pm_lock(&g_mem_lk);
    t_mem_held = 1;
}
static void mem_unlock(void) {
    t_mem_held = 0;
    pm_unlock(&g_mem_lk);
}

/* bumped under g_mem_lk before guest pages lose their host memory or turn free or inaccessible, and by mem_init:
   until it moves, a run mem_mapped_run found mapped stays mapped, so each thread keeps the runs it found, one per
   megabyte granule of the address they were found for (direct mapped). The GPU thread asks this of every buffer and
   page run it touches, about two million times a second in one game, across more blocks than a few kept runs held:
   with one run kept, or a handful searched, it took g_mem_lk and searched the blocks for most of them */
static u64 g_unmap_gen = 1;
#define MEM_RUNS 64
#define MEM_RUN_SLOT(a) (((a) >> 20) & (MEM_RUNS - 1))
static __thread struct {
    u64 lo[MEM_RUNS], hi[MEM_RUNS], gen;
} t_runs;
static void runs_stale_nl(void) { __atomic_add_fetch(&g_unmap_gen, 1, __ATOMIC_RELEASE); }

static void (*g_code_invalidate)(u64, u64); /* the runtime's JIT: what was compiled in the range is stale now */
void mem_set_code_invalidate(void (*fn)(u64, u64)) { g_code_invalidate = fn; }
/* no memory lock held: the hook takes the JIT's */
static void code_invalidate(u64 addr, u64 size) {
    if (g_code_invalidate) g_code_invalidate(addr, size);
}

static u64 blk_end(const MemBlock* b) { return b->addr + b->size; }
/* b follows a: they join when alike, the pool's pages continuing from a's when they are the pool's */
static int blk_same(const MemBlock* a, const MemBlock* b) {
    return a->state == b->state && a->perm == b->perm && a->attr == b->attr && a->ipc == b->ipc && a->dev == b->dev &&
           (a->pool ? b->pool == a->pool + a->size : !b->pool);
}
static void blk_reserve_nl(u32 n) {
    MemBlock* b;
    u32 cap;
    if (g_nblk + n <= g_capblk) return;
    cap = g_capblk ? g_capblk * 2 : 256;
    while (cap < g_nblk + n) cap *= 2;
    b = (MemBlock*)realloc(g_blk, sizeof *b * (size_t)cap);
    if (!b) fatal("Out of memory for the guest memory map");
    g_blk = b;
    g_capblk = cap;
}
/* the block holding a (below AS_END) */
static u32 blk_index_nl(u64 a) {
    u32 lo = 0, hi = g_nblk - 1;
    while (lo < hi) {
        u32 mid = lo + (hi - lo + 1) / 2;
        if (g_blk[mid].addr <= a)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}
/* a block boundary at a: the index of the block starting there, g_nblk at AS_END */
static u32 blk_split_nl(u64 a) {
    u32 i;
    if (a >= AS_END) return g_nblk;
    i = blk_index_nl(a);
    if (g_blk[i].addr == a) return i;
    blk_reserve_nl(1);
    memmove(&g_blk[i + 2], &g_blk[i + 1], sizeof *g_blk * (size_t)(g_nblk - i - 1));
    g_blk[i + 1] = g_blk[i];
    g_blk[i + 1].addr = a;
    if (g_blk[i].pool) g_blk[i + 1].pool += a - g_blk[i].addr;
    g_blk[i + 1].size = blk_end(&g_blk[i]) - a;
    g_blk[i].size = a - g_blk[i].addr;
    g_nblk++;
    return i + 1;
}
/* joins the equal neighbours among the blocks lo - 1 to hi */
static void blk_merge_nl(u32 lo, u32 hi) {
    u32 r, w;
    if (lo) lo--;
    if (hi >= g_nblk) hi = g_nblk - 1;
    w = lo;
    for (r = lo + 1; r <= hi; r++) {
        if (blk_same(&g_blk[w], &g_blk[r]))
            g_blk[w].size += g_blk[r].size;
        else
            g_blk[++w] = g_blk[r];
    }
    if (w == hi) return;
    memmove(&g_blk[w + 1], &g_blk[hi + 1], sizeof *g_blk * (size_t)(g_nblk - hi - 1));
    g_nblk -= hi - w;
}
/* what blk_change_nl does to each block of a range: its state and permission (when set_state), the attribute bits
   of attr_mask set to attr's, and (when set_pool) the pool pages from pool (plus 1, 0 for none) behind it */
typedef struct {
    int set_state, set_perm, set_pool;
    u32 state, perm, attr_mask, attr;
    u64 pool;
} Change;
static void blk_change_nl(u64 a, u64 size, const Change* c) {
    u32 i = blk_split_nl(a), j = blk_split_nl(a + size), k;
    if (c->set_state && (c->state == MS_FREE || c->state == MS_INACCESSIBLE)) runs_stale_nl();
    for (k = i; k < j; k++) {
        MemBlock* b = &g_blk[k];
        if (c->set_pool) b->pool = c->pool ? c->pool + (b->addr - a) : 0;
        if (c->set_state) {
            b->state = c->state;
            if (c->state == MS_FREE) b->ipc = b->dev = 0, b->pool = 0;
        }
        if (c->set_perm) b->perm = c->perm;
        b->attr = (b->attr & ~c->attr_mask) | (c->attr & c->attr_mask);
    }
    blk_merge_nl(i, j);
}
static void blk_set_nl(u64 a, u64 size, u32 state, u32 perm, u32 attr) {
    Change c;
    c.set_state = c.set_perm = c.set_pool = 1;
    c.pool = 0;
    c.state = state;
    c.perm = perm;
    c.attr_mask = ~0u;
    c.attr = attr;
    blk_change_nl(a, size, &c);
}
/* every block of [a, a + size) is in state with perm and attr (ANY: whatever it is) */
#define ANY 0xFFFFFFFFu
static int range_is_nl(u64 a, u64 size, u32 state, u32 perm, u32 attr) {
    u64 end = a + size;
    u32 i = blk_index_nl(a);
    for (; i < g_nblk && g_blk[i].addr < end; i++) {
        const MemBlock* b = &g_blk[i];
        if (b->state != state || (perm != ANY && b->perm != perm) || (attr != ANY && b->attr != attr)) return 0;
    }
    return 1;
}
/* host memory under [a, a + size): the host pages it touches committed, whole */
static int commit_nl(u64 a, u64 size) {
    u64 mask = g_host_page - 1, lo = a & ~mask, hi = (a + size + mask) & ~mask;
    return plat_vm_commit(GP(lo), hi - lo);
}
/* [a, a + size), free in the block list now, given back: the host pages left with no mapped guest page decommitted,
   and in a host page a neighbour still maps, the range's part zeroed, as fresh pages read when mapped again */
static void decommit_nl(u64 a, u64 size) {
    u64 mask = g_host_page - 1, end = a + size, first = a & ~mask, last = (end - 1) & ~mask, edge[2];
    int i, n = 0;
    runs_stale_nl();
    own_drop(a, size);
    if (!((a | end) & mask)) { /* whole host pages, all of them free */
        plat_vm_decommit(GP(a), size);
        return;
    }
    edge[n++] = first;
    if (last != first) edge[n++] = last;
    for (i = 0; i < n; i++) {
        u64 lo = edge[i] > a ? edge[i] : a, hi = edge[i] + g_host_page < end ? edge[i] + g_host_page : end;
        if (range_is_nl(edge[i], g_host_page, MS_FREE, ANY, ANY))
            plat_vm_decommit(GP(edge[i]), g_host_page);
        else {
            own_acquire(edge[i], g_host_page); /* a neighbour's bytes the GPU holds land before the page is written */
            memset(GP(lo), 0, (size_t)(hi - lo));
        }
    }
    if (last > first + g_host_page) plat_vm_decommit(GP(first + g_host_page), last - first - g_host_page);
}
/* [a, a + size) widened to whole pages, inside the address space [AS_BASE, AS_END) and not empty: 0 when it can't be */
static int page_range(u64* a, u64* size) {
    u64 lo = *a & ~(PAGE - 1), end;
    if (!*size || *a < AS_BASE || *a >= AS_END || *size > AS_END - *a) return 0;
    end = (*a + *size + PAGE - 1) & ~(PAGE - 1);
    *a = lo;
    *size = end - lo;
    return 1;
}
static void mapping_begin(void) {
    if (g_gpu_mapping_begin) g_gpu_mapping_begin(0, 0, 1);
}
static void mapping_end(void) {
    if (g_gpu_mapping_end) g_gpu_mapping_end();
}
/* the lowest address from lo, aligned, where size bytes fit below hi with guard free bytes on either side */
static u64 find_free_nl(u64 lo, u64 hi, u64 size, u64 align, u64 guard) {
    u32 i;
    if (!align) align = PAGE;
    if (hi > AS_END) hi = AS_END;
    if (lo >= hi || !size) return 0;
    for (i = blk_index_nl(lo); i < g_nblk && g_blk[i].addr < hi; i++) {
        const MemBlock* b = &g_blk[i];
        u64 a, end = blk_end(b);
        if (b->state != MS_FREE || b->size < size + 2 * guard) continue;
        a = b->addr + guard > lo ? b->addr + guard : lo;
        if (a > ~(align - 1)) break;
        a = (a + align - 1) & ~(align - 1);
        if (a < end && size + guard <= end - a && size <= hi - a) return a;
    }
    return 0;
}

/* ============================================================ the alias pool */
/* The pool's offsets in use (plat_alias_*), as runs in offset order with no two touching, under g_mem_lk. Shared
   memory owns its pages from its creation, a view's source from mem_alias_begin to mem_alias_end; a guest mapping of
   them (blocks with pool set) only borrows them, so unmapping one never frees pool pages */
typedef struct {
    u64 off, size;
} PoolRun;
static PoolRun* g_run;
static u32 g_nrun, g_caprun;
static int g_pool_on; /* plat_alias_init worked; set by mem_init, before other threads */

static void run_insert_nl(u32 i, u64 off, u64 size) {
    if (g_nrun == g_caprun) {
        u32 cap = g_caprun ? g_caprun * 2 : 64;
        PoolRun* r = (PoolRun*)realloc(g_run, sizeof *r * (size_t)cap);
        if (!r) fatal("Out of memory for the alias pool's map");
        g_run = r;
        g_caprun = cap;
    }
    memmove(&g_run[i + 1], &g_run[i], sizeof *g_run * (size_t)(g_nrun - i));
    g_run[i].off = off;
    g_run[i].size = size;
    g_nrun++;
}
static void run_remove_nl(u32 i) {
    memmove(&g_run[i], &g_run[i + 1], sizeof *g_run * (size_t)(g_nrun - i - 1));
    g_nrun--;
}
/* the lowest free offset with size bytes free: 0 with it in off, -1 when the pool is full */
static int pool_alloc_nl(u64 size, u64* off) {
    u64 at = 0;
    u32 i;
    for (i = 0; i < g_nrun && g_run[i].off - at < size; i++) at = g_run[i].off + g_run[i].size;
    if (at > g_pool_size || g_pool_size - at < size) return -1;
    *off = at;
    if (i && i < g_nrun && at + size == g_run[i].off) { /* fills the gap: one run with the next */
        g_run[i - 1].size += size + g_run[i].size;
        run_remove_nl(i);
    } else if (i) /* at is where the run before ends */
        g_run[i - 1].size += size;
    else if (i < g_nrun && at + size == g_run[i].off) {
        g_run[i].off = at;
        g_run[i].size += size;
    } else
        run_insert_nl(i, at, size);
    return 0;
}
/* gives back what pool_alloc_nl handed out (all or part of it); the pages read zero when handed out again */
static void pool_free_nl(u64 off, u64 size) {
    u64 end = off + size;
    u32 i;
    for (i = 0; i < g_nrun && g_run[i].off + g_run[i].size < end; i++) {
    }
    if (i == g_nrun || off < g_run[i].off) fatal("The alias pool freed pages it didn't hand out");
    if (off > g_run[i].off && end < g_run[i].off + g_run[i].size) {
        run_insert_nl(i + 1, end, g_run[i].off + g_run[i].size - end);
        g_run[i].size = off - g_run[i].off;
    } else if (off > g_run[i].off)
        g_run[i].size = off - g_run[i].off;
    else if (end < g_run[i].off + g_run[i].size) {
        g_run[i].size -= size;
        g_run[i].off = end;
    } else
        run_remove_nl(i);
    plat_alias_free(off, size);
}
u64 mem_pool_used(void) {
    u64 n = 0;
    u32 i;
    mem_lock();
    for (i = 0; i < g_nrun; i++) n += g_run[i].size;
    mem_unlock();
    return n;
}
/* the pool offset plus 1 of a when all of [a, a + size) is pool pages, in order; 0 when it isn't */
static u64 pool_run_nl(u64 a, u64 size) {
    u64 end = a + size, p;
    u32 i = blk_index_nl(a);
    if (!g_blk[i].pool) return 0;
    p = g_blk[i].pool + (a - g_blk[i].addr);
    for (i++; i < g_nblk && g_blk[i].addr < end; i++)
        if (g_blk[i].pool != p + (g_blk[i].addr - a)) return 0;
    return p;
}
static int any_pool_nl(u64 a, u64 size) {
    u64 end = a + size;
    u32 i;
    for (i = blk_index_nl(a); i < g_nblk && g_blk[i].addr < end; i++)
        if (g_blk[i].pool) return 1;
    return 0;
}
static void pool_set_nl(u64 a, u64 size, u64 pool) {
    Change c;
    c.set_state = c.set_perm = 0;
    c.set_pool = 1;
    c.state = c.perm = c.attr_mask = c.attr = 0;
    c.pool = pool;
    blk_change_nl(a, size, &c);
}
/* whatever is mapped in [a, a + size) goes: the pool's pages unmapped (they stay their owner's), the reservation's own
   decommitted, block by block with the guest's pages, else once the list is free there (decommit_nl) */
static void unmap_nl(u64 a, u64 size) {
    u64 end = a + size;
    u32 i;
    runs_stale_nl(); /* before the pages go, not with the state after them */
    own_drop(a, size); /* what the GPU held there goes with the mapping */
    for (i = blk_index_nl(a); i < g_nblk && g_blk[i].addr < end; i++) {
        const MemBlock* b = &g_blk[i];
        u64 lo = b->addr > a ? b->addr : a, hi = blk_end(b) < end ? blk_end(b) : end;
        if (b->state == MS_FREE) continue;
        if (b->pool) {
            if (plat_alias_unmap(GP(lo), hi - lo))
                fatal("Could not unmap %llu KiB of guest memory from the alias pool",
                      (unsigned long long)((hi - lo) >> 10));
        } else if (g_host_page == PAGE)
            plat_vm_decommit(GP(lo), hi - lo);
    }
    blk_set_nl(a, size, MS_FREE, PERM_NONE, 0);
    if (g_host_page != PAGE) decommit_nl(a, size); /* no pool with larger pages (mem_init) */
}
/* [a, a + size) the reservation's own pages again: committed with the pool's contents from *off, or decommitted
   without off. Also what a failed MAP_FIXED needs, as it may have taken the old pages with it; a host that can't do
   even this leaves nothing to run on */
static void unpool_nl(u64 a, u64 size, const u64* off) {
    runs_stale_nl();
    if (plat_alias_unmap(GP(a), size) || (off && (commit_nl(a, size) || plat_alias_read(*off, GP(a), size))))
        fatal("Could not put %llu KiB of guest memory back from the alias pool", (unsigned long long)(size >> 10));
}
/* Moves mapped memory into the pool: its contents are copied there and the pool's pages mapped over it, so views of
   it can map them too. Memory that is pool pages already (a view of a view) stays as it is. Pool pages mapped here
   only (what an owner of GPU memory moved there, mem_own_prepare) move again with the rest, into one run, and their
   old pages are given back */
static int pool_mapped_elsewhere_nl(u64 off, u64 size, u64 a);
static int host_view_nl(u64 off, u64 size);
static int promote_nl(u64 a, u64 size) {
    u64 end = a + size, off;
    u32 i;
    if (!g_pool_on || pool_run_nl(a, size)) return 0;
    for (i = blk_index_nl(a); i < g_nblk && g_blk[i].addr < end; i++) {
        const MemBlock* b = &g_blk[i];
        u64 lo, hi, boff;
        if (b->state == MS_FREE) return MEM_BADSTATE;
        if (!b->pool) continue;
        lo = b->addr > a ? b->addr : a;
        hi = blk_end(b) < end ? blk_end(b) : end;
        boff = b->pool - 1 + (lo - b->addr);
        if (pool_mapped_elsewhere_nl(boff, hi - lo, lo) || host_view_nl(boff, hi - lo)) return MEM_BADSTATE;
    }
    /* the copy goes through the kernel, which can't wait on trapped pages: what the GPU holds there lands first (the
       caller holds the renderer's lock, which landing takes on this thread) */
    own_acquire(a, size);
    if (pool_alloc_nl(size, &off)) return MEM_NOMEM;
    if (plat_alias_write(off, GP(a), size)) {
        pool_free_nl(off, size);
        return MEM_NOMEM;
    }
    if (plat_alias_map(GP(a), off, size)) {
        unpool_nl(a, size, &off);
        pool_free_nl(off, size);
        return MEM_NOMEM;
    }
    for (i = blk_index_nl(a); i < g_nblk && g_blk[i].addr < end; i++) { /* the old pool pages, not mapped any more */
        const MemBlock* b = &g_blk[i];
        if (b->pool) {
            u64 lo = b->addr > a ? b->addr : a, hi = blk_end(b) < end ? blk_end(b) : end;
            pool_free_nl(b->pool - 1 + (lo - b->addr), hi - lo);
        }
    }
    pool_set_nl(a, size, off + 1);
    return 0;
}
/* promote_nl undone, once no other guest mapping of the pages is left: fresh pages of the reservation's own at a
   with the pool's contents, and the pool's pages freed. Memory that isn't the pool's stays as it is */
static void demote_nl(u64 a, u64 size) {
    u64 p = pool_run_nl(a, size), lo, hi;
    u32 i;
    if (!p) return;
    own_acquire(a, size); /* the pool's contents are read below, past any trap */
    lo = p - 1;
    hi = lo + size;
    for (i = 0; i < g_nblk; i++) {
        const MemBlock* b = &g_blk[i];
        /* a's own pages, in a block merged with a neighbour that is the source of another view: two stacks mapped
           one after the other get neighbouring pool pages and become one block, and keeping them here left pool
           pages in the heap that a later stack's view then overlapped (svcMapMemory failed, and nn::os aborted) */
        if (b->pool && b->addr + lo == a + (b->pool - 1)) continue;
        if (b->pool && (b->addr < a || blk_end(b) > a + size) && b->pool - 1 < hi && lo < b->pool - 1 + b->size)
            return; /* another mapping of them, or a block reaching past a's range (a view of part of a view) */
    }
    unpool_nl(a, size, &lo);
    pool_set_nl(a, size, 0);
    pool_free_nl(lo, size);
}
/* a view of src at free pages dst: the same pages when src is the pool's, a copy when it isn't */
static int alias_map_nl(u64 dst, u64 src, u64 size, u32 state, u32 perm) {
    u64 p;
    Change c;
    if (!range_is_nl(dst, size, MS_FREE, ANY, ANY)) return MEM_BADSTATE;
    own_acquire(src, size); /* owned memory has one mapping: what the GPU holds of src lands before it has two */
    p = pool_run_nl(src, size);
    if (p && plat_alias_map(GP(dst), p - 1, size)) {
        unpool_nl(dst, size, NULL);
        return MEM_NOMEM;
    }
    if (!p) {
        if (commit_nl(dst, size)) return MEM_NOMEM;
        memcpy(GP(dst), GP(src), size);
    }
    c.set_state = c.set_perm = c.set_pool = 1;
    c.state = state;
    c.perm = perm;
    c.attr_mask = ~0u;
    c.attr = 0;
    c.pool = p;
    blk_change_nl(dst, size, &c);
    return 0;
}

#ifdef SNX_IOS
#include <mach/mach.h>
#include <mach/vm_map.h>
/* The layout in size bytes of address space, smaller than the console's: the alias and heap regions shrink, in the
   same order and below the stack, with the kernel's pages in the last GiB. The game finds the sizes it asks of
   GetInfo (the alias region 4 GiB to 32 GiB, the heap region 4 GiB to 8 GiB) */
static void layout_for(u64 size) {
    g_layout.end = size;
    if (size >= 16 * GIB) {
        u64 alias = size >= 64 * GIB ? 32 * GIB : size >= 32 * GIB ? 8 * GIB : 4 * GIB;
        u64 heap = size >= 24 * GIB ? 8 * GIB : 4 * GIB;
        g_layout.alias = 4 * GIB;
        g_layout.alias_size = alias;
        g_layout.heap = g_layout.alias + alias;
        g_layout.heap_size = heap;
        g_layout.stack = g_layout.heap + heap;
        g_layout.stack_size = 2 * GIB;
        g_layout.kern = size - GIB;
    } else if (size >= 12 * GIB) {
        g_layout.alias = 4 * GIB;
        g_layout.alias_size = 2 * GIB;
        g_layout.heap = 6 * GIB;
        g_layout.heap_size = 4 * GIB;
        g_layout.stack = 10 * GIB;
        g_layout.stack_size = GIB;
        g_layout.kern = 11 * GIB;
    } else {
        /* under 12 GiB: no alias region (as a 32-bit layout without one), the heap region from 1 GiB, which leaves
           the code that room, then a 256 MiB stack and the kernel's 256 MiB */
        g_layout.alias = GIB;
        g_layout.alias_size = 0;
        g_layout.heap = GIB;
        g_layout.heap_size = size - GIB - GIB / 2;
        g_layout.stack = g_layout.heap + g_layout.heap_size;
        g_layout.stack_size = GIB / 4;
        g_layout.kern = size - GIB / 4;
    }
    g_layout.kern_end = size;
}
/* The largest run of address space in the task that nothing maps, from its range (TASK_VM_INFO) and its regions
   (vm_region): what a reservation can be at most, whatever the system would say to a bigger one */
static u64 largest_hole(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    vm_address_t at = 0;
    u64 lo = 0, hi = 0, end, best = 0, best_at = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
        lo = info.min_address;
        hi = info.max_address;
    }
    end = lo;
    for (;;) {
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t region;
        mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object;
        if (vm_region_64(mach_task_self(), &at, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&region, &n,
                         &object) != KERN_SUCCESS)
            break;
        if (at > end && at - end > best) best = at - end, best_at = end;
        end = (u64)at + size;
        at += size;
        if (end >= hi) break;
    }
    if (hi > end && hi - end > best) best = hi - end, best_at = end;
    fprintf(stderr, "memory: the task's address space is %#llx to %#llx, its largest free run %llu MiB at %#llx\n",
            (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)(best >> 20),
            (unsigned long long)best_at);
    return best;
}
/* An app without the paid extended-virtual-addressing entitlement may reserve less than the console's layout wants:
   the largest of these that the system gives is the guest's address space */
static u8* reserve_ios(void) {
    static const u64 tiers[] = {128 * GIB, 64 * GIB, 48 * GIB, 32 * GIB, 24 * GIB, 16 * GIB, 12 * GIB, 8 * GIB, 7 * GIB, 6 * GIB, 5 * GIB};
    const char* cap = getenv("SNX_ADDRESS_SPACE_GIB"); /* a smaller layout on purpose, to try one on a host with room */
    u64 hole = largest_hole(), most = cap ? (u64)strtoull(cap, NULL, 10) * GIB : AS_MAX;
    size_t i;
    for (i = 0; i < sizeof tiers / sizeof *tiers; ++i) {
        int skipped = tiers[i] > most || tiers[i] > hole;
        u8* p = skipped ? NULL : plat_vm_reserve(tiers[i]);
        fprintf(stderr, "memory: %llu GiB of address space: %s%s\n", (unsigned long long)(tiers[i] >> 30),
                tiers[i] > most ? "skipped (SNX_ADDRESS_SPACE_GIB)"
                : tiers[i] > hole ? "skipped (no free run that large)"
                : p ? "reserved" : "refused: ",
                !p && !skipped ? strerror(errno) : "");
        if (!p) continue;
        g_as_size = tiers[i];
        if (tiers[i] < AS_MAX) layout_for(tiers[i]);
        return p;
    }
    return NULL;
}
#endif

void mem_init(void) {
    pm_lock_announce(&g_mem_lk, "guest memory"); /* every thread's reads and writes of the game's memory */
    g_host_page = plat_vm_page();
#ifdef SNX_IOS
    g_mem = reserve_ios();
    if (!g_mem) fatal("Could not reserve even 5 GiB of address space for the game's memory");
    LOG("memory: %llu GiB of address space reserved%s\n", (unsigned long long)(g_as_size >> 30),
        g_as_size < AS_MAX ? " (a smaller layout: this app has no extended virtual addressing)" : "");
#else
    g_mem = plat_vm_reserve(AS_SIZE);
    if (!g_mem) fatal("Could not reserve %llu GiB of address space for the game's memory",
                      (unsigned long long)(AS_SIZE >> 30));
#endif
    mem_lock();
    blk_reserve_nl(1);
    memset(&g_blk[0], 0, sizeof *g_blk);
    g_blk[0].size = AS_END;
    g_nblk = 1;
    runs_stale_nl();
    mem_unlock();
    /* a view shares pages only at the same place in a host page as its source, which a game picking addresses in
       4 KiB steps leaves to chance with larger pages: they copy, as hosts without a pool do */
    g_pool_on = g_host_page == PAGE && !plat_alias_init(g_pool_size);
    LOG("memory: %llu KiB host pages, %s\n", (unsigned long long)(g_host_page >> 10),
        g_pool_on ? "views share their source's pages (alias pool)" : "views are copies (no alias pool)");
}

/* ============================================================ mapping */
int mem_map(u64 addr, u64 size, u32 state, u32 perm) {
    int r = 0;
    if (!page_range(&addr, &size)) return -1;
    mapping_begin();
    mem_lock();
    if (!range_is_nl(addr, size, MS_FREE, ANY, ANY))
        r = -1;
    else if (commit_nl(addr, size))
        r = -2;
    else {
        blk_set_nl(addr, size, state, perm, 0);
        ev_add(addr, size);
    }
    mem_unlock();
    mapping_end();
    return r;
}
/* whatever is mapped in the range goes, and reads as zero when mapped again */
int mem_unmap(u64 addr, u64 size) {
    if (!page_range(&addr, &size)) return -1;
    mapping_begin();
    mem_lock();
    unmap_nl(addr, size);
    ev_remove(addr, size);
    mem_unlock();
    mapping_end();
    code_invalidate(addr, size);
    gpu_mem_changed(addr, size);
    return 0;
}
/* the console reports everything from the end of the address space up as one inaccessible block */
void mem_query(u64 addr, MemBlock* out) {
    if (addr >= AS_END || !g_nblk) {
        memset(out, 0, sizeof *out);
        out->addr = AS_END;
        out->size = 0 - AS_END;
        out->state = MS_INACCESSIBLE;
        return;
    }
    if (t_mem_held) {
        *out = g_blk[blk_index_nl(addr)];
        return;
    }
    mem_lock();
    *out = g_blk[blk_index_nl(addr)];
    mem_unlock();
}
u64 mem_mapped_run(u64 addr, u64 size) {
    u64 end, lo, lim = addr, run, gen;
    u32 i;
    int mine = !t_mem_held; /* a crash report's queries come from the thread that holds the lock */
    if (!size) return 0;
    i = MEM_RUN_SLOT(addr);
    if (__atomic_load_n(&g_unmap_gen, __ATOMIC_ACQUIRE) == t_runs.gen && addr >= t_runs.lo[i] &&
        addr < t_runs.hi[i] && size <= t_runs.hi[i] - addr)
        return size;
    if (mine) mem_lock();
    gen = __atomic_load_n(&g_unmap_gen, __ATOMIC_RELAXED);
    if (!g_nblk)
        run = size;
    else if (addr >= AS_END)
        run = 0;
    else {
        end = size < AS_END - addr ? addr + size : AS_END;
        /* the run kept goes on past the request over the mapped blocks after it (up to 16 MB more), where the next
           lookups of this granule usually land */
        u64 reach = end < AS_END - (16u << 20) ? end + (16u << 20) : AS_END;
        u32 b = blk_index_nl(addr);
        lo = g_blk[b].addr;
        for (; b < g_nblk && lim < reach; b++) {
            if (g_blk[b].state == MS_FREE || g_blk[b].state == MS_INACCESSIBLE) break;
            lim = blk_end(&g_blk[b]);
        }
        run = (lim < end ? lim : end) - addr;
        if (lim > addr) {
            if (t_runs.gen != gen) { /* the runs kept are of an older generation */
                memset(&t_runs, 0, sizeof t_runs);
                t_runs.gen = gen;
            }
            t_runs.lo[i] = lo;
            t_runs.hi[i] = lim;
        }
    }
    if (mine) mem_unlock();
    return run;
}
/* the state and permission of mapped memory (the loader's sections, ldr's code views, the memory SVCs) */
int mem_set_state(u64 addr, u64 size, u32 state, u32 perm) {
    int r = 0;
    Change c;
    if (!page_range(&addr, &size)) return -1;
    c.set_state = c.set_perm = 1;
    c.set_pool = 0;
    c.pool = 0;
    c.state = state;
    c.perm = perm;
    c.attr_mask = c.attr = 0;
    mem_lock();
    {
        u64 end = addr + size;
        u32 i;
        for (i = blk_index_nl(addr); i < g_nblk && g_blk[i].addr < end; i++)
            if (g_blk[i].state == MS_FREE) r = -1;
    }
    if (!r) blk_change_nl(addr, size, &c);
    mem_unlock();
    if (!r) code_invalidate(addr, size); /* the guest may write the range now and run it after */
    return r;
}
int mem_set_attr(u64 addr, u64 size, u32 mask, u32 attr) {
    Change c;
    if (!page_range(&addr, &size)) return -1;
    c.set_state = c.set_perm = c.set_pool = 0;
    c.state = c.perm = 0;
    c.pool = 0;
    c.attr_mask = mask;
    c.attr = attr;
    mem_lock();
    blk_change_nl(addr, size, &c);
    mem_unlock();
    return 0;
}
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align) {
    u64 a;
    mem_lock();
    a = find_free_nl(lo, hi, size, align, 0);
    mem_unlock();
    return a;
}
/* The kernel's own pages (TLS), in the kernel map region, found and mapped in one hold of the lock with a free guard
   page on either side, so two allocations never share a page. Called with g_klock held, so it doesn't take the
   renderer's lock: nothing the renderer sees goes away, and no guest memory the GPU uses is ever here */
u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    u64 a;
    size = (size + PAGE - 1) & ~(PAGE - 1);
    mem_lock();
    a = find_free_nl(KERN_BASE, KERN_END, size, PAGE, PAGE);
    if (a && !commit_nl(a, size)) {
        blk_set_nl(a, size, state, perm, 0);
        ev_add(a, size);
    } else {
        a = 0;
    }
    mem_unlock();
    if (!a) fatal("Out of guest memory for the kernel (%llu KiB)", (unsigned long long)(size >> 10));
    return a;
}

/* ============================================================ views and the pool's own pages */
/* All under the renderer's lock and g_mem_lk, as every change of host memory. A promotion or demotion copies the
   source while it is mapped again: a guest thread writing it meanwhile loses the write, as it would race the lock the
   caller is about to take on the console. The renderer is told of every range whose pages changed underneath */
int mem_alias_begin(u64 src, u64 size) {
    int r;
    if (!page_range(&src, &size)) return MEM_BADSTATE;
    mapping_begin();
    own_acquire(src, size); /* owned memory has one mapping: what the GPU holds of src lands before views come */
    mem_lock();
    r = promote_nl(src, size);
    mem_unlock();
    mapping_end();
    if (!r) gpu_mem_changed(src, size);
    return r;
}
void mem_alias_end(u64 src, u64 size) {
    if (!page_range(&src, &size)) return;
    mapping_begin();
    mem_lock();
    demote_nl(src, size);
    mem_unlock();
    mapping_end();
    gpu_mem_changed(src, size);
}
int mem_alias_map(u64 dst, u64 src, u64 size, u32 state, u32 perm) {
    int r;
    u64 n = size;
    if (!page_range(&dst, &size) || !page_range(&src, &n) || n != size) return MEM_BADSTATE;
    mapping_begin();
    mem_lock();
    r = alias_map_nl(dst, src, size, state, perm);
    mem_unlock();
    mapping_end();
    if (!r) gpu_mem_changed(dst, size);
    return r;
}
int mem_alias_unmap(u64 dst, u64 size, u64 back) {
    if (!page_range(&dst, &size) || (back && (back >= AS_END || size > AS_END - back))) return MEM_BADSTATE;
    mapping_begin();
    /* what the GPU holds of back lands before the copy writes it (an alias that is a copy leaves back mapped alone,
       so an engine may own it), with the producer's lock held but not mem.c's */
    if (back) own_acquire(back, size);
    mem_lock();
    if (back && !any_pool_nl(dst, size)) memcpy(GP(back), GP(dst), size);
    unmap_nl(dst, size);
    mem_unlock();
    mapping_end();
    code_invalidate(dst, size);
    if (back) code_invalidate(back, size); /* the write back may have changed the pages it went to */
    gpu_mem_changed(dst, size);
    return 0;
}
/* the pool's pages a service also maps outside the reservation (mem_pool_host): never GPU-owned, as the service's
   view of them can't be trapped. Runs of offsets as g_run's, under g_mem_lk */
static PoolRun* g_hostrun;
static u32 g_nhostrun, g_caphostrun;
static int host_view_nl(u64 off, u64 size) {
    u32 i;
    for (i = 0; i < g_nhostrun; i++)
        if (g_hostrun[i].off < off + size && off < g_hostrun[i].off + g_hostrun[i].size) return 1;
    return 0;
}
static void host_view_add_nl(u64 off, u64 size) {
    if (g_nhostrun == g_caphostrun) {
        u32 cap = g_caphostrun ? g_caphostrun * 2 : 16;
        PoolRun* r = (PoolRun*)realloc(g_hostrun, sizeof *r * (size_t)cap);
        if (!r) fatal("Out of memory for the alias pool's map");
        g_hostrun = r;
        g_caphostrun = cap;
    }
    g_hostrun[g_nhostrun].off = off;
    g_hostrun[g_nhostrun++].size = size;
}
static void host_view_remove_nl(u64 off) {
    u32 i;
    for (i = 0; i < g_nhostrun; i++)
        if (g_hostrun[i].off == off) {
            g_hostrun[i] = g_hostrun[--g_nhostrun];
            return;
        }
}
/* whether a guest mapping other than the one at a has any of the pool pages [off, off + size): a block mapping them
   with another translation from guest addresses to pool offsets than a's (off + x - a at x) */
static int pool_mapped_elsewhere_nl(u64 off, u64 size, u64 a) {
    u32 i;
    for (i = 0; i < g_nblk; i++) {
        const MemBlock* b = &g_blk[i];
        if (b->pool && b->pool - 1 < off + size && off < b->pool - 1 + b->size && b->pool - 1 - b->addr != off - a)
            return 1;
    }
    return 0;
}
int mem_own_prepare(u64 a, u64 size, MemPoolRun* runs, u32 max, u32* n) {
    u64 end = a + size, at;
    u32 i;
    int r = -1;
    *n = 0;
    if (!g_pool_on || (a | size) & (PAGE - 1) || !size || a < AS_BASE || end > AS_END) return -1;
    mem_lock();
    for (i = blk_index_nl(a); i < g_nblk && g_blk[i].addr < end; i++) {
        const MemBlock* b = &g_blk[i];
        u32 st = b->state;
        /* data only: code and its aliases are the code's, I/O and the kernel's aren't the guest's */
        if (st == MS_FREE || st == MS_CODE || st == MS_ALIASCODE || st == MS_SHAREDCODE || st == MS_GENCODE ||
            st == MS_CODEOUT || st == MS_IO || st == MS_KERNEL || st == MS_INACCESSIBLE || st == MS_TLS)
            goto out;
        if (b->pool) { /* pool pages already: only when mapped here alone */
            u64 lo = b->addr > a ? b->addr : a, hi = blk_end(b) < end ? blk_end(b) : end;
            u64 boff = b->pool - 1 + (lo - b->addr);
            if (pool_mapped_elsewhere_nl(boff, hi - lo, lo) || host_view_nl(boff, hi - lo)) goto out;
        }
    }
    /* the parts that aren't pool pages yet moved there, each a run of its own: parts that are may be another owner's,
       trapped, and keep their pages. Writes to a part wait while it is copied (a neighbour's, on a page it shares) */
    for (at = a; at < end;) {
        u32 j = blk_index_nl(at);
        u64 lo = at, hi = at;
        if (g_blk[j].pool) {
            at = blk_end(&g_blk[j]) < end ? blk_end(&g_blk[j]) : end;
            continue;
        }
        while (hi < end && !g_blk[blk_index_nl(hi)].pool) {
            const MemBlock* b = &g_blk[blk_index_nl(hi)];
            hi = blk_end(b) < end ? blk_end(b) : end;
        }
        plat_freeze(GP(lo), hi - lo);
        r = promote_nl(lo, hi - lo);
        plat_thaw();
        if (r) {
            r = -1;
            goto out;
        }
        at = hi;
    }
    /* the runs of pool pages, in order */
    for (at = a; at < end;) {
        const MemBlock* b = &g_blk[blk_index_nl(at)];
        u64 hi = blk_end(b) < end ? blk_end(b) : end, off = b->pool - 1 + (at - b->addr);
        if (*n && runs[*n - 1].addr + runs[*n - 1].size == at && runs[*n - 1].pool + runs[*n - 1].size == off)
            runs[*n - 1].size += hi - at;
        else if (*n == max) {
            r = -1;
            goto out;
        } else {
            runs[*n].addr = at;
            runs[*n].size = hi - at;
            runs[*n].pool = off;
            ++*n;
        }
        at = hi;
    }
    r = 0;
out:
    mem_unlock();
    return r;
}
int mem_pool_offset(u64 a, u64* off) {
    int r = -1;
    u32 i;
    if (a >= AS_END) return -1;
    mem_lock();
    i = blk_index_nl(a);
    if (g_blk[i].pool) {
        *off = g_blk[i].pool - 1 + (a - g_blk[i].addr);
        r = 0;
    }
    mem_unlock();
    return r;
}
u8* mem_pool_host(u64 size, u64* off) {
    u8* p;
    int r;
    if (!g_pool_on || !size) return NULL;
    size = (size + PAGE - 1) & ~(PAGE - 1);
    mem_lock();
    r = pool_alloc_nl(size, off);
    mem_unlock();
    if (r) return NULL;
    p = plat_alias_host(*off, size);
    mem_lock();
    if (!p)
        pool_free_nl(*off, size);
    else
        host_view_add_nl(*off, size);
    mem_unlock();
    return p;
}
void mem_pool_host_free(u8* p, u64 off, u64 size) {
    size = (size + PAGE - 1) & ~(PAGE - 1);
    plat_alias_host_free(p, size);
    mem_lock();
    host_view_remove_nl(off);
    pool_free_nl(off, size);
    mem_unlock();
}
int mem_map_pool(u64 addr, u64 off, u64 size, u32 state, u32 perm) {
    int r = 0;
    if (!page_range(&addr, &size)) return MEM_BADSTATE;
    mapping_begin();
    mem_lock();
    if (!range_is_nl(addr, size, MS_FREE, ANY, ANY))
        r = MEM_BADSTATE;
    else if (plat_alias_map(GP(addr), off, size)) {
        unpool_nl(addr, size, NULL);
        r = MEM_NOMEM;
    } else {
        Change c;
        c.set_state = c.set_perm = c.set_pool = 1;
        c.state = state;
        c.perm = perm;
        c.attr_mask = ~0u;
        c.attr = 0;
        c.pool = off + 1;
        blk_change_nl(addr, size, &c);
        ev_add(addr, size);
    }
    mem_unlock();
    mapping_end();
    if (!r) gpu_mem_changed(addr, size);
    return r;
}
int mem_unmap_pool(u64 addr, u64 off, u64 size, u32 state) {
    int r = MEM_BADSTATE;
    if (!page_range(&addr, &size)) return r;
    mapping_begin();
    mem_lock();
    if (range_is_nl(addr, size, state, ANY, ANY) && pool_run_nl(addr, size) == off + 1) {
        unmap_nl(addr, size);
        ev_remove(addr, size);
        r = 0;
    }
    mem_unlock();
    mapping_end();
    if (!r) {
        code_invalidate(addr, size);
        gpu_mem_changed(addr, size);
    }
    return r;
}

/* ============================================================ the heap and physical memory */
/* SetHeapSize's mapping (the size checks are svc_mem.c's): growth charges only the new pages, against the budget the
   heap shares with physical memory, and fails without a change; shrinking needs every page that goes to be heap
   memory as SetHeapSize left it (normal, read-write, no attribute: not locked by ldr, a transfer memory or MapMemory) */
int mem_heap_set(u64 size, u64 budget) {
    u64 cur, lo = 0, n = 0;
    int r = 0;
    if (size == __atomic_load_n(&g_heap_size, __ATOMIC_ACQUIRE)) return 0;
    if (size > HEAP_SIZE) return MEM_NOMEM;
    mapping_begin();
    mem_lock();
    cur = g_heap_size;
    if (size > cur) {
        u64 a = HEAP_BASE + cur, grow = size - cur;
        if (size > budget || g_phys_mapped > budget - size)
            r = MEM_LIMIT;
        else if (!range_is_nl(a, grow, MS_FREE, ANY, ANY))
            r = MEM_BADSTATE;
        else if (commit_nl(a, grow))
            r = MEM_NOMEM;
        else {
            blk_set_nl(a, grow, MS_NORMAL, PERM_RW, 0);
            ev_add(a, grow);
        }
    } else if (size < cur) {
        lo = HEAP_BASE + size;
        n = cur - size;
        if (!range_is_nl(lo, n, MS_NORMAL, PERM_RW, 0))
            r = MEM_BADSTATE;
        else {
            unmap_nl(lo, n);
            ev_remove(lo, n);
        }
    }
    if (!r) __atomic_store_n(&g_heap_size, size, __ATOMIC_RELEASE);
    mem_unlock();
    mapping_end();
    if (!r && n) gpu_mem_changed(lo, n);
    return r;
}
/* MapPhysicalMemory's mapping: the free pages of the range become normal memory, charged once against the budget; a
   page already mapped stays as it is. A host commit that fails undoes this call's earlier commits */
int mem_map_physical(u64 addr, u64 size, u64 budget) {
    u64 end = addr + size, add = 0, rolled = 0;
    u32 i;
    int r = 0;
    if (!size || addr >= AS_END || size > AS_END - addr) return MEM_BADSTATE;
    mapping_begin();
    mem_lock();
    for (i = blk_index_nl(addr); i < g_nblk && g_blk[i].addr < end; i++) {
        const MemBlock* b = &g_blk[i];
        u64 lo = b->addr > addr ? b->addr : addr, hi = blk_end(b) < end ? blk_end(b) : end;
        if (b->state == MS_FREE)
            add += hi - lo;
        else if (b->state != MS_NORMAL)
            r = MEM_BADSTATE;
    }
    if (!r && add && (add > budget || g_heap_size > budget - add || g_phys_mapped > budget - add - g_heap_size))
        r = MEM_LIMIT;
    if (!r && add) {
        u64 a = addr, done = addr;
        /* commit hole by hole, from the first; on a failure decommit the holes before it */
        while (a < end && !r) {
            u32 k = blk_index_nl(a);
            u64 hi = blk_end(&g_blk[k]) < end ? blk_end(&g_blk[k]) : end;
            if (g_blk[k].state == MS_FREE && commit_nl(a, hi - a)) r = MEM_NOMEM;
            if (!r) done = hi;
            a = hi;
        }
        for (a = addr; a < done;) {
            u32 k = blk_index_nl(a);
            u64 hi = blk_end(&g_blk[k]) < done ? blk_end(&g_blk[k]) : done;
            if (g_blk[k].state == MS_FREE) {
                if (r) {
                    decommit_nl(a, hi - a);
                } else {
                    blk_set_nl(a, hi - a, MS_NORMAL, PERM_RW, 0);
                    ev_add(a, hi - a);
                }
            }
            a = hi;
        }
        if (!r) __atomic_store_n(&g_phys_mapped, g_phys_mapped + add, __ATOMIC_RELEASE);
        if (r && done > addr) rolled = done;
    }
    mem_unlock();
    mapping_end();
    if (rolled) gpu_mem_changed(addr, rolled - addr); /* the decommitted pages read as zero */
    return r;
}
/* UnmapPhysicalMemory's: every mapped page of the range must be unlocked normal memory (its permission may have
   changed); free pages in it are allowed. Nothing changes when one isn't */
int mem_unmap_physical(u64 addr, u64 size) {
    u64 end = addr + size, freed = 0;
    u32 i;
    int r = 0;
    if (!size || addr >= AS_END || size > AS_END - addr) return MEM_BADSTATE;
    mapping_begin();
    mem_lock();
    for (i = blk_index_nl(addr); i < g_nblk && g_blk[i].addr < end; i++) {
        const MemBlock* b = &g_blk[i];
        u64 lo = b->addr > addr ? b->addr : addr, hi = blk_end(b) < end ? blk_end(b) : end;
        u64 off = b->pool - 1 + lo - b->addr; /* lo's offset in the pool, read only for a block in it */
        if (b->state == MS_FREE) continue;
        /* GPU ownership promotes ordinary memory into the pool too. Only borrowed pages are aliases. */
        if (b->state != MS_NORMAL || b->attr ||
            (b->pool && (pool_mapped_elsewhere_nl(off, hi - lo, lo) || host_view_nl(off, hi - lo)))) {
            r = MEM_BADSTATE;
            break;
        }
    }
    if (!r) {
        own_drop(addr, size); /* Cancel pending GPU writes before releasing their backing pages. */
        for (i = blk_index_nl(addr); i < g_nblk && g_blk[i].addr < end; i++) {
            const MemBlock* b = &g_blk[i];
            u64 lo = b->addr > addr ? b->addr : addr, hi = blk_end(b) < end ? blk_end(b) : end;
            if (b->state != MS_FREE) freed += hi - lo;
            if (b->pool) pool_free_nl(b->pool - 1 + lo - b->addr, hi - lo);
        }
        unmap_nl(addr, size);
        ev_remove(addr, size);
        __atomic_store_n(&g_phys_mapped, freed < g_phys_mapped ? g_phys_mapped - freed : 0, __ATOMIC_RELEASE);
    }
    mem_unlock();
    mapping_end();
    if (!r && freed) {
        code_invalidate(addr, size);
        gpu_mem_changed(addr, size); /* the decommitted pages read as zero */
    }
    return r;
}

/* ============================================================ ldr:ro's code views */
/* a free range for a code view, with a free guard page on either side: anywhere in the address space outside the
   heap and alias regions (the console's CanContain for alias code) and the kernel's pages, lowest first */
static u64 code_find_nl(u64 size) {
    u64 lo[3], hi[3], from = AS_BASE, a;
    int i, j, n = 0;
    lo[n] = HEAP_BASE, hi[n] = HEAP_BASE + HEAP_SIZE, n++;
    if (ALIAS_SIZE) lo[n] = ALIAS_BASE, hi[n] = ALIAS_BASE + ALIAS_SIZE, n++;
    lo[n] = KERN_BASE, hi[n] = KERN_END, n++;
    for (i = 1; i < n; i++) /* in address order */
        for (j = i; j > 0 && lo[j] < lo[j - 1]; j--) {
            u64 l = lo[j], h = hi[j];
            lo[j] = lo[j - 1], hi[j] = hi[j - 1];
            lo[j - 1] = l, hi[j - 1] = h;
        }
    for (i = 0; i <= n; i++) {
        u64 to = i < n ? lo[i] : AS_END;
        if (to > from && (a = find_free_nl(from, to, size, PAGE, PAGE)) != 0) return a;
        if (i < n && hi[i] > from) from = hi[i];
    }
    return 0;
}
/* heap memory a code view may copy: normal, read-write, no attribute, and neither IPC- nor device-mapped */
static int code_source_nl(u64 a, u64 size) {
    u64 end = a + size;
    u32 i;
    if (!range_is_nl(a, size, MS_NORMAL, PERM_RW, 0)) return 0;
    for (i = blk_index_nl(a); i < g_nblk && g_blk[i].addr < end; i++)
        if (g_blk[i].ipc || g_blk[i].dev) return 0;
    return 1;
}
/* the locked attribute and no permission on a code view's source, or read-write and unlocked again */
static void code_lock_nl(u64 a, u64 size, int lock) {
    Change c;
    if (!size) return;
    c.set_state = c.set_pool = 0;
    c.set_perm = 1;
    c.state = 0;
    c.perm = lock ? PERM_NONE : PERM_RW;
    c.attr_mask = 1;
    c.attr = lock ? 1 : 0;
    c.pool = 0;
    blk_change_nl(a, size, &c);
}
/* the view of the image and its BSS with the pool: both sources moved into it and mapped one after the other */
static int code_alias_nl(const MemCodeMap* m, u64 base) {
    int r = promote_nl(m->source, m->size);
    if (!r && m->bss_size && (r = promote_nl(m->bss, m->bss_size)) != 0) demote_nl(m->source, m->size);
    if (r) return r;
    r = alias_map_nl(base, m->source, m->size, MS_ALIASCODEDATA, PERM_RW);
    if (!r && m->bss_size && (r = alias_map_nl(base + m->size, m->bss, m->bss_size, MS_ALIASCODEDATA, PERM_RW)) != 0)
        unmap_nl(base, m->size);
    if (r) {
        demote_nl(m->source, m->size);
        if (m->bss_size) demote_nl(m->bss, m->bss_size);
    }
    return r;
}
/* The image (source) and its BSS, each at a page, are mapped one after the other at a new view, and locked (no
   permission, the locked attribute) until mem_unmap_code: the same pages with the alias pool, as the console maps
   them, else a copy that mem_unmap_code copies back. The view isn't charged: its pages are the heap's */
int mem_map_code(MemCodeMap* m) {
    u64 total = m->size + m->bss_size, base = 0;
    int r = 0;
    if (!m->size || m->size > AS_END || m->source > AS_END - m->size || m->bss_size > AS_END ||
        m->bss > AS_END - m->bss_size || m->bss_size > ~m->size || (m->source & (PAGE - 1)) ||
        (m->bss_size && (m->bss & (PAGE - 1))))
        return MEM_BADSTATE;
    mapping_begin();
    mem_lock();
    if (!code_source_nl(m->source, m->size) ||
        (m->bss_size && (!code_source_nl(m->bss, m->bss_size) ||
                         (m->bss < m->source + m->size && m->source < m->bss + m->bss_size))))
        r = MEM_BADSTATE;
    else if ((base = code_find_nl(total)) == 0)
        r = MEM_LIMIT;
    else if (g_pool_on)
        r = code_alias_nl(m, base);
    else if (commit_nl(base, total))
        r = MEM_NOMEM;
    else {
        memcpy(GP(base), GP(m->source), m->size);
        if (m->bss_size) memcpy(GP(base + m->size), GP(m->bss), m->bss_size);
        blk_set_nl(base, total, MS_ALIASCODEDATA, PERM_RW, 0);
    }
    if (!r) {
        code_lock_nl(m->source, m->size, 1);
        code_lock_nl(m->bss, m->bss_size, 1);
        m->base = base;
    }
    mem_unlock();
    mapping_end();
    if (!r) gpu_mem_changed(base, total);
    return r;
}
int mem_unmap_code(const MemCodeMap* m) {
    u64 total = m->size + m->bss_size, end = m->base + total;
    u32 i;
    int r = 0;
    if (!m->base || m->base >= AS_END || total > AS_END - m->base) return MEM_BADSTATE;
    mapping_begin();
    mem_lock();
    for (i = blk_index_nl(m->base); i < g_nblk && g_blk[i].addr < end; i++)
        if (g_blk[i].state != MS_ALIASCODE && g_blk[i].state != MS_ALIASCODEDATA) r = MEM_BADSTATE;
    if (!r && (!range_is_nl(m->source, m->size, MS_NORMAL, PERM_NONE, 1) ||
               (m->bss_size && !range_is_nl(m->bss, m->bss_size, MS_NORMAL, PERM_NONE, 1))))
        r = MEM_BADSTATE;
    if (!r) {
        if (!any_pool_nl(m->base, total)) {
            memcpy(GP(m->source), GP(m->base), m->size);
            if (m->bss_size) memcpy(GP(m->bss), GP(m->base + m->size), m->bss_size);
        }
        unmap_nl(m->base, total);
        demote_nl(m->source, m->size);
        if (m->bss_size) demote_nl(m->bss, m->bss_size);
        code_lock_nl(m->source, m->size, 0);
        code_lock_nl(m->bss, m->bss_size, 0);
    }
    mem_unlock();
    mapping_end();
    if (!r) {
        code_invalidate(m->base, total); /* the view goes: what ran there is stale */
        code_invalidate(m->source, m->size);
        if (m->bss_size) code_invalidate(m->bss, m->bss_size);
        gpu_mem_changed(m->base, total);
        gpu_mem_changed(m->source, m->size);
        if (m->bss_size) gpu_mem_changed(m->bss, m->bss_size);
    }
    return r;
}

/* ============================================================ save states */
u32 mem_blocks(MemBlock* out, u32 max) {
    u32 n;
    mem_lock();
    n = g_nblk;
    if (out) memcpy(out, g_blk, sizeof *out * (size_t)(n < max ? n : max));
    mem_unlock();
    return n;
}
/* each run of g_run cut where a host view begins or ends inside it, under g_mem_lk: the count, out filled up to max */
static u32 spans_nl(MemSpan* out, u32 max) {
    u32 i, k, n = 0;
    for (i = 0; i < g_nrun; i++) {
        u64 at = g_run[i].off, end = at + g_run[i].size;
        while (at < end) {
            u64 cut = end;
            int host = 0;
            for (k = 0; k < g_nhostrun; k++) {
                u64 lo = g_hostrun[k].off, hi = lo + g_hostrun[k].size;
                if (lo <= at && at < hi) {
                    host = 1;
                    cut = hi < cut ? hi : cut;
                } else if (lo > at && lo < cut)
                    cut = lo;
            }
            if (out && n < max) {
                out[n].off = at;
                out[n].size = cut - at;
                out[n].host = host;
            }
            n++;
            at = cut;
        }
    }
    return n;
}
u32 mem_spans(MemSpan* out, u32 max) {
    u32 n;
    mem_lock();
    n = spans_nl(out, max);
    mem_unlock();
    return n;
}
int mem_pool_read(u64 off, void* dst, u64 size) { return g_pool_on ? plat_alias_read(off, dst, size) : -1; }

int mem_restore_begin(void) {
    u32 i, n;
    MemSpan* s;
    OwnRange own[64];
    mapping_begin();
    /* without landing: the state's bytes replace it all. Range by range: own_drop over the whole address space would
       let go of the traps on all of it, page by page */
    while ((n = own_lookup(AS_BASE, AS_END - AS_BASE, own, 64)) != 0)
        for (i = 0; i < n && i < 64; i++) own_drop(own[i].addr, own[i].size);
    mem_lock();
    if (!g_pool_on) return -1;
    /* everything mapped goes but the modules' code, from the top so the indices below stay put */
    for (i = g_nblk; i-- > 0;)
        if (g_blk[i].state != MS_FREE && g_blk[i].state != MS_CODE) unmap_nl(g_blk[i].addr, g_blk[i].size);
    /* and the pool's pages but the services' host views */
    n = spans_nl(NULL, 0);
    s = (MemSpan*)malloc(sizeof *s * (size_t)(n ? n : 1));
    if (!s) return -1;
    spans_nl(s, n);
    for (i = 0; i < n; i++)
        if (!s[i].host) pool_free_nl(s[i].off, s[i].size);
    free(s);
    return 0;
}
u64 mem_restore_alloc(u64 size) {
    u64 off;
    return pool_alloc_nl(size, &off) ? ~0ull : off;
}
int mem_restore_write(u64 off, const void* p, u64 size) { return plat_alias_write(off, p, size) ? -1 : 0; }
/* the state's list covers the address space in order, and its code is the code mapped now: 0, or -1. Under the lock */
static int restore_check_nl(const MemBlock* blk, u32 n) {
    u32 i, k;
    u64 at = 0;
    for (i = 0; i < n; i++) {
        if (blk[i].addr != at || !blk[i].size || (blk[i].addr & (PAGE - 1)) || (blk[i].size & (PAGE - 1)) ||
            blk[i].size > AS_END - at)
            return -1;
        at += blk[i].size;
    }
    if (at != AS_END) return -1;
    for (i = 0, k = 0;;) {
        while (i < g_nblk && g_blk[i].state != MS_CODE) i++;
        while (k < n && blk[k].state != MS_CODE) k++;
        if ((i < g_nblk) != (k < n)) return -1;
        if (i == g_nblk) break;
        if (g_blk[i].addr != blk[k].addr || g_blk[i].size != blk[k].size || g_blk[i].perm != blk[k].perm) return -1;
        i++;
        k++;
    }
    return 0;
}
int mem_restore_check(const MemBlock* blk, u32 n) {
    int r;
    mem_lock();
    r = restore_check_nl(blk, n);
    mem_unlock();
    return r;
}
int mem_restore_blocks(const MemBlock* blk, u32 n) {
    u32 i;
    if (restore_check_nl(blk, n)) return -1;
    for (i = 0; i < n; i++) {
        const MemBlock* b = &blk[i];
        if (b->state == MS_FREE || b->state == MS_CODE) continue;
        if (b->pool ? plat_alias_map(GP(b->addr), b->pool - 1, b->size) != 0 : commit_nl(b->addr, b->size) != 0)
            return -1;
    }
    if (n > g_nblk) blk_reserve_nl(n - g_nblk);
    memcpy(g_blk, blk, sizeof *blk * (size_t)n);
    g_nblk = n;
    return 0;
}
void mem_restore_end(u64 heap_size, u64 phys_mapped) {
    MemBlock* b;
    u32 i, n;
    __atomic_store_n(&g_heap_size, heap_size, __ATOMIC_RELEASE);
    __atomic_store_n(&g_phys_mapped, phys_mapped, __ATOMIC_RELEASE);
    runs_stale_nl();
    n = g_nblk;
    b = (MemBlock*)malloc(sizeof *b * (size_t)n);
    if (b) memcpy(b, g_blk, sizeof *b * (size_t)n);
    mem_unlock();
    mapping_end();
    /* what the JIT compiled and the renderer keeps of every block is stale: not the modules' code, whose entries the
       game module filled in once (dispatch_init) */
    for (i = 0; b && i < n; i++)
        if (b[i].state != MS_FREE && b[i].state != MS_CODE) code_invalidate(b[i].addr, b[i].size);
    free(b);
    gpu_mem_changed(AS_BASE, AS_END - AS_BASE);
}
