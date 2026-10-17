/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/kernel/k_address_space_info.cpp, k_page_table_base.cpp):
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project */
/* the guest address space: the host reservation every guest address is an offset into, its regions, and the list of
   memory blocks with the state, permission and attributes QueryMemory reports */
#include "nx/memory/mem.h"
#include "nx/gpu/gpu.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>

#define GIB  (1ull << 30)
#define PAGE 0x1000ull

/* The 39-bit layout of a 64-bit application without ASLR, as Eden's KPageTableBase::InitializeForProcess lays it out
   from k_address_space_info.cpp's sizes: code at 0x8000000, then the 64 GiB alias region, the 8 GiB heap region and
   the 2 GiB stack region. The console's address space is 512 GiB; the host reservation is AS_SIZE (128 GiB), so the
   address space (the ASLR region of GetInfo 12 and 13) ends there, and the kernel map region, 64 GiB on the console,
   is the 1 GiB the kernel's own pages (TLS) need. The alias region starts at 4 GiB, which leaves the code below it
   up to 3.9 GiB, as the end of the code isn't known yet when the layout is made */
MemLayout g_layout = {0x8000000ull,  AS_SIZE,     0x1100000000ull, 8 * GIB, 0x100000000ull, 64 * GIB,
                      0x1300000000ull, 2 * GIB, 0x1380000000ull, 0x13C0000000ull};
u8* g_mem;
u64 g_heap_size, g_phys_mapped, g_heap_max, g_code_memory, g_main_stack_size;

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
   and no two neighbours alike. Free pages have no host memory behind them (decommitted), every other block has.
   Everything here is under g_mem_lk; a change of host memory also holds the renderer's memory lock around it
   (g_gpu_mapping_begin, taken first), which may query the list itself */
static PMutex g_mem_lk = PMUTEX_INIT;
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

static u64 blk_end(const MemBlock* b) { return b->addr + b->size; }
static int blk_same(const MemBlock* a, const MemBlock* b) {
    return a->state == b->state && a->perm == b->perm && a->attr == b->attr && a->ipc == b->ipc && a->dev == b->dev;
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
/* what blk_change_nl does to each block of a range: its state and permission (when set_state), and the attribute
   bits of attr_mask set to attr's */
typedef struct {
    int set_state, set_perm;
    u32 state, perm, attr_mask, attr;
} Change;
static void blk_change_nl(u64 a, u64 size, const Change* c) {
    u32 i = blk_split_nl(a), j = blk_split_nl(a + size), k;
    for (k = i; k < j; k++) {
        MemBlock* b = &g_blk[k];
        if (c->set_state) {
            b->state = c->state;
            if (c->state == MS_FREE) b->ipc = b->dev = 0;
        }
        if (c->set_perm) b->perm = c->perm;
        b->attr = (b->attr & ~c->attr_mask) | (c->attr & c->attr_mask);
    }
    blk_merge_nl(i, j);
}
static void blk_set_nl(u64 a, u64 size, u32 state, u32 perm, u32 attr) {
    Change c;
    c.set_state = c.set_perm = 1;
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

void mem_init(void) {
    g_mem = plat_vm_reserve(AS_SIZE);
    if (!g_mem) fatal("Could not reserve %llu GiB of address space for the game's memory", AS_SIZE >> 30);
    mem_lock();
    blk_reserve_nl(1);
    memset(&g_blk[0], 0, sizeof *g_blk);
    g_blk[0].size = AS_END;
    g_nblk = 1;
    mem_unlock();
}

/* ============================================================ mapping */
int mem_map(u64 addr, u64 size, u32 state, u32 perm) {
    int r = 0;
    if (!page_range(&addr, &size)) return -1;
    mapping_begin();
    mem_lock();
    if (!range_is_nl(addr, size, MS_FREE, ANY, ANY))
        r = -1;
    else if (plat_vm_commit(GP(addr), size))
        r = -2;
    else
        blk_set_nl(addr, size, state, perm, 0);
    mem_unlock();
    mapping_end();
    return r;
}
/* whatever is mapped in the range goes, and reads as zero when mapped again */
int mem_unmap(u64 addr, u64 size) {
    u64 end;
    u32 i;
    if (!page_range(&addr, &size)) return -1;
    end = addr + size;
    mapping_begin();
    mem_lock();
    for (i = blk_index_nl(addr); i < g_nblk && g_blk[i].addr < end; i++) {
        const MemBlock* b = &g_blk[i];
        u64 lo = b->addr > addr ? b->addr : addr, hi = blk_end(b) < end ? blk_end(b) : end;
        if (b->state != MS_FREE) plat_vm_decommit(GP(lo), hi - lo);
    }
    blk_set_nl(addr, size, MS_FREE, PERM_NONE, 0);
    mem_unlock();
    mapping_end();
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
/* the state and permission of mapped memory (the loader's sections, ldr's code views, the memory SVCs) */
int mem_set_state(u64 addr, u64 size, u32 state, u32 perm) {
    int r = 0;
    Change c;
    if (!page_range(&addr, &size)) return -1;
    c.set_state = c.set_perm = 1;
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
    return r;
}
int mem_set_attr(u64 addr, u64 size, u32 mask, u32 attr) {
    Change c;
    if (!page_range(&addr, &size)) return -1;
    c.set_state = c.set_perm = 0;
    c.state = c.perm = 0;
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
    if (a && !plat_vm_commit(GP(a), size))
        blk_set_nl(a, size, state, perm, 0);
    else
        a = 0;
    mem_unlock();
    if (!a) fatal("Out of guest memory for the kernel (%llu KiB)", (unsigned long long)(size >> 10));
    return a;
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
        else if (plat_vm_commit(GP(a), grow))
            r = MEM_NOMEM;
        else
            blk_set_nl(a, grow, MS_NORMAL, PERM_RW, 0);
    } else if (size < cur) {
        lo = HEAP_BASE + size;
        n = cur - size;
        if (!range_is_nl(lo, n, MS_NORMAL, PERM_RW, 0))
            r = MEM_BADSTATE;
        else {
            plat_vm_decommit(GP(lo), n);
            blk_set_nl(lo, n, MS_FREE, PERM_NONE, 0);
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
            if (g_blk[k].state == MS_FREE && plat_vm_commit(GP(a), hi - a)) r = MEM_NOMEM;
            if (!r) done = hi;
            a = hi;
        }
        for (a = addr; a < done;) {
            u32 k = blk_index_nl(a);
            u64 hi = blk_end(&g_blk[k]) < done ? blk_end(&g_blk[k]) : done;
            if (g_blk[k].state == MS_FREE) {
                if (r)
                    plat_vm_decommit(GP(a), hi - a);
                else
                    blk_set_nl(a, hi - a, MS_NORMAL, PERM_RW, 0);
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
    for (i = blk_index_nl(addr); i < g_nblk && g_blk[i].addr < end; i++)
        if (g_blk[i].state != MS_FREE && (g_blk[i].state != MS_NORMAL || g_blk[i].attr)) r = MEM_BADSTATE;
    if (!r) {
        for (i = blk_index_nl(addr); i < g_nblk && g_blk[i].addr < end; i++) {
            const MemBlock* b = &g_blk[i];
            u64 lo = b->addr > addr ? b->addr : addr, hi = blk_end(b) < end ? blk_end(b) : end;
            if (b->state == MS_FREE) continue;
            plat_vm_decommit(GP(lo), hi - lo);
            freed += hi - lo;
        }
        blk_set_nl(addr, size, MS_FREE, PERM_NONE, 0);
        __atomic_store_n(&g_phys_mapped, freed < g_phys_mapped ? g_phys_mapped - freed : 0, __ATOMIC_RELEASE);
    }
    mem_unlock();
    mapping_end();
    if (!r && freed) gpu_mem_changed(addr, size);
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
/* The image (source) and its BSS, each at a page, are copied into a new view and locked (no permission, the locked
   attribute) until mem_unmap_code copies the view back: the copy stands in for the console's second mapping of the
   same pages, which the host reservation can't make. The view isn't charged: its pages are the heap's on the
   console */
int mem_map_code(MemCodeMap* m) {
    u64 total = m->size + m->bss_size, base;
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
    else if (plat_vm_commit(GP(base), total))
        r = MEM_NOMEM;
    else {
        Change lock;
        memcpy(GP(base), GP(m->source), m->size);
        if (m->bss_size) memcpy(GP(base + m->size), GP(m->bss), m->bss_size);
        blk_set_nl(base, total, MS_ALIASCODEDATA, PERM_RW, 0);
        lock.set_state = 0;
        lock.set_perm = 1;
        lock.state = 0;
        lock.perm = PERM_NONE;
        lock.attr_mask = lock.attr = 1;
        blk_change_nl(m->source, m->size, &lock);
        if (m->bss_size) blk_change_nl(m->bss, m->bss_size, &lock);
        m->base = base;
    }
    mem_unlock();
    mapping_end();
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
        Change unlock;
        memcpy(GP(m->source), GP(m->base), m->size);
        if (m->bss_size) memcpy(GP(m->bss), GP(m->base + m->size), m->bss_size);
        plat_vm_decommit(GP(m->base), total);
        blk_set_nl(m->base, total, MS_FREE, PERM_NONE, 0);
        unlock.set_state = 0;
        unlock.set_perm = 1;
        unlock.state = 0;
        unlock.perm = PERM_RW;
        unlock.attr_mask = 1;
        unlock.attr = 0;
        blk_change_nl(m->source, m->size, &unlock);
        if (m->bss_size) blk_change_nl(m->bss, m->bss_size, &unlock);
    }
    mem_unlock();
    mapping_end();
    if (!r) gpu_mem_changed(m->base, total);
    return r;
}
