/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The map of guest memory the GPU holds, and the traps over it (docs/GPU_OWNERSHIP.md; the trap model is Skyline's,
   read for its design). Entries are byte exact and never overlap; a host page with any owned byte on it is armed
   (plat_trap_arm), so every access to it, the guest's or the runtime's, waits while own_fault lands the entries on
   it through the pool's mirror, and the page is let go. Owned memory is always pool pages mapped at one address
   only (mem_own_prepare): a second mapping of them lands them first, so a fault has one entry to find.

   Locks: the producer's (the renderer's vk_lock), then mem.c's, then g_trap_lk, then g_own_lk, which is never held
   across landing, a resource's release or a wait for the GPU; the producer's lock is never taken under g_trap_lk or
   g_own_lk. A landing takes the producer's lock before it marks its entries, and holds it until they are let go and
   released: every thread that waits on g_own_cv for a mark (own_begin, own_drop, a landing) holds that lock too, so
   the mark is its own, never one of a thread that waits for the lock in turn. The landing runs with no lock of ours
   held, so a thread holding the kernel's lock may fault on owned memory, as long as no holder of the producer's lock
   waits for the kernel's (nvdrv.c's syncpt_add_quiet). One holding mem.c's lock may not: holders of the producer's
   lock wait for it (own_begin, every mapping change), so mem.c lands what it touches first (own_acquire). Nor may one
   holding the producer's lock, on an owned page (its landing needs the lock): own_fault ends the process with the
   address, as that is a bug in the runtime. Its fault on a page no entry is on any more goes on */
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>
#include "nx/memory/own.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

typedef struct {
    u64 lo, hi;   /* guest bytes */
    u64 pool;     /* the pool offset of lo */
    void* res;    /* the producer's copy, lo at res_off */
    u64 res_off;
    int landing;  /* being landed by some thread: the others wait */
} Own;

int g_own_any;
static u64 g_own_lands; /* landings so far, for the statistics */
static OwnHooks g_hooks;
static int g_traps;    /* own_init found traps */
static u64 g_page;     /* the host's page, which traps are armed in */
static PMutex g_own_lk = PMUTEX_INIT;
/* arming pages and the check that lets them go, one at a time: a page let go just as an entry lands on it stays
   trapped (taken before g_own_lk) */
static PMutex g_trap_lk = PMUTEX_INIT;
static PCond g_own_cv;
static Own* g_own;     /* sorted by lo, under g_own_lk */
static u32 g_nown, g_capown;
/* The entries on each 4 MB region of guest memory, changed with them under g_own_lk and read without it: the GPU
   thread reads guest memory through copy_runs, which asked under the lock whether any entry was on each run it read,
   and with storage buffers kept on the GPU there almost always is one somewhere. Those lock round trips were about 2%
   of the GPU thread in one game's underground scene. A count of 0 answers for the region without the lock; a reader
   racing an entry being made finds it or not, as one taking the lock first or second would. NULL: the lock always */
#define OWN_REGION_SHIFT 22
static u32* g_own_region;
static u64 g_own_nregion;
static void cover_nl(const Own* o, u32 d) {
    if (!g_own_region || o->hi <= o->lo) return;
    for (u64 r = o->lo >> OWN_REGION_SHIFT, e = (o->hi - 1) >> OWN_REGION_SHIFT; r <= e && r < g_own_nregion; r++)
        __atomic_add_fetch(&g_own_region[r], d, __ATOMIC_RELEASE);
}
/* whether an entry may be on [lo, hi): 0 only when none is */
static int region_owned(u64 lo, u64 hi) {
    if (!g_own_region) return 1;
    if (hi <= lo) return 0;
    for (u64 r = lo >> OWN_REGION_SHIFT, e = (hi - 1) >> OWN_REGION_SHIFT; r <= e; r++)
        if (r >= g_own_nregion || __atomic_load_n(&g_own_region[r], __ATOMIC_ACQUIRE)) return 1;
    return 0;
}

/* ---------------------------------------------------------------- the entries, under g_own_lk */
/* the first entry ending after a */
static u32 first_after_nl(u64 a) {
    u32 lo = 0, hi = g_nown;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_own[mid].hi <= a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
static void insert_nl(u32 i, const Own* o) {
    if (g_nown == g_capown) {
        u32 cap = g_capown ? g_capown * 2 : 64;
        Own* n = (Own*)realloc(g_own, sizeof *n * (size_t)cap);
        if (!n) fatal("Out of memory for the map of GPU-owned memory");
        g_own = n;
        g_capown = cap;
    }
    memmove(&g_own[i + 1], &g_own[i], sizeof *g_own * (size_t)(g_nown - i));
    g_own[i] = *o;
    g_nown++;
    cover_nl(o, 1);
    __atomic_store_n(&g_own_any, 1, __ATOMIC_RELEASE);
}
static void remove_nl(u32 i) {
    cover_nl(&g_own[i], (u32)-1);
    memmove(&g_own[i], &g_own[i + 1], sizeof *g_own * (size_t)(g_nown - i - 1));
    if (!--g_nown) __atomic_store_n(&g_own_any, 0, __ATOMIC_RELEASE);
}
static int page_owned_nl(u64 page) {
    u32 i = first_after_nl(page);
    return i < g_nown && g_own[i].lo < page + g_page;
}
/* whether an entry overlapping [lo, hi) is being landed */
static int landing_nl(u64 lo, u64 hi) {
    u32 i;
    for (i = first_after_nl(lo); i < g_nown && g_own[i].lo < hi; i++)
        if (g_own[i].landing) return 1;
    return 0;
}

/* resources to release once g_own_lk is let go (release may take the producer's lock) */
typedef struct {
    void** res;
    u32 n, cap;
} ResList;
static void res_push(ResList* l, void* res) {
    if (l->n == l->cap) {
        u32 cap = l->cap ? l->cap * 2 : 8;
        void** r = (void**)realloc(l->res, sizeof *r * (size_t)cap);
        if (!r) fatal("Out of memory for the map of GPU-owned memory");
        l->res = r;
        l->cap = cap;
    }
    l->res[l->n++] = res;
}
static void res_release(ResList* l) {
    u32 i;
    for (i = 0; i < l->n; i++) g_hooks.release(l->res[i]);
    free(l->res);
}

/* the bytes [lo, hi) owned by nobody any more: entries cut back or split, their resources to release in rel */
static void cut_nl(u64 lo, u64 hi, ResList* rel) {
    u32 i = first_after_nl(lo);
    while (i < g_nown && g_own[i].lo < hi) {
        Own* o = &g_own[i];
        if (o->lo < lo && o->hi > hi) { /* the middle goes: two entries */
            Own right = *o;
            right.res_off += hi - o->lo;
            right.pool += hi - o->lo;
            right.lo = hi;
            cover_nl(o, (u32)-1);
            o->hi = lo;
            cover_nl(o, 1);
            g_hooks.retain(right.res);
            insert_nl(i + 1, &right);
            return;
        }
        if (o->lo < lo) { /* its end goes */
            cover_nl(o, (u32)-1);
            o->hi = lo;
            cover_nl(o, 1);
            i++;
        } else if (o->hi > hi) { /* its start goes */
            o->res_off += hi - o->lo;
            o->pool += hi - o->lo;
            cover_nl(o, (u32)-1);
            o->lo = hi;
            cover_nl(o, 1);
            return;
        } else { /* all of it */
            res_push(rel, o->res);
            remove_nl(i);
        }
    }
}

/* the host pages [lo, hi) (page aligned): those no entry is on any more let go, and trapped no more */
static void let_go(u64 lo, u64 hi) {
    u64 a = lo;
    pm_lock(&g_trap_lk);
    while (a < hi) {
        u64 run;
        int free_page;
        pm_lock(&g_own_lk);
        free_page = !page_owned_nl(a);
        pm_unlock(&g_own_lk);
        if (!free_page) {
            a += g_page;
            continue;
        }
        for (run = a + g_page; run < hi; run += g_page) {
            int owned;
            pm_lock(&g_own_lk);
            owned = page_owned_nl(run);
            pm_unlock(&g_own_lk);
            if (owned) break;
        }
        /* maps the pages (waking a thread waiting on one) before the trap goes, which would leave it waiting */
        plat_trap_continue(GP(a), run - a);
        plat_trap_disarm(GP(a), run - a);
        a = run;
    }
    pm_unlock(&g_trap_lk);
}

/* whether an entry is on [lo, hi) */
static int owned_nl(u64 lo, u64 hi) {
    u32 i = first_after_nl(lo);
    return i < g_nown && g_own[i].lo < hi;
}
static void producer_lock(void) {
    if (g_hooks.lock) g_hooks.lock();
}
static void producer_unlock(void) {
    if (g_hooks.unlock) g_hooks.unlock();
}

/* every entry on the host pages [lo, hi) landed and let go, the pages with them. The producer's lock is taken only
   when entries are there (a fault with none, the trap thread's, never waits for it), before any is marked, then the
   entries are looked for again: they may have landed or changed while it waited */
static void land_pages(u64 lo, u64 hi) {
    for (;;) {
        OwnRange* r;
        u8** dst;
        u8* mirror = plat_alias_mirror();
        u64 elo = lo, ehi = hi;
        u32 i, first, n = 0;
        int any;
        ResList rel = {0};
        if (!region_owned(lo, hi)) return;
        pm_lock(&g_own_lk);
        any = owned_nl(lo, hi);
        pm_unlock(&g_own_lk);
        if (!any) return;
        producer_lock();
        pm_lock(&g_own_lk);
        while (landing_nl(lo, hi)) pc_wait(&g_own_cv, &g_own_lk, -1);
        first = first_after_nl(lo);
        for (i = first; i < g_nown && g_own[i].lo < hi; i++) n++;
        if (!n) {
            pm_unlock(&g_own_lk);
            producer_unlock();
            return;
        }
        /* what one publish made lands whole (it is split by the pool's runs): the entries beside these that continue
           them, of the same resource, are landed with them: one wait for the GPU rather than one a piece */
        while (first && g_own[first - 1].res == g_own[first].res && g_own[first - 1].hi == g_own[first].lo &&
               !g_own[first - 1].landing)
            first--, n++;
        while (first + n < g_nown && g_own[first + n].res == g_own[first + n - 1].res &&
               g_own[first + n].lo == g_own[first + n - 1].hi && !g_own[first + n].landing)
            n++;
        r = (OwnRange*)malloc(sizeof *r * n);
        dst = (u8**)malloc(sizeof *dst * n);
        if (!r || !dst) fatal("Out of memory landing GPU-owned memory");
        for (i = 0; i < n; i++) {
            Own* o = &g_own[first + i];
            o->landing = 1;
            r[i].addr = o->lo;
            r[i].size = o->hi - o->lo;
            r[i].res = o->res;
            r[i].res_off = o->res_off;
            dst[i] = mirror + o->pool;
            if (o->lo < elo) elo = o->lo;
            if (o->hi > ehi) ehi = o->hi;
        }
        pm_unlock(&g_own_lk);
        g_hooks.land(r, dst, n);
        __atomic_add_fetch(&g_own_lands, 1, __ATOMIC_RELAXED);
        /* the landed pages through the mirror, still trapped: no thread can write them before let_go below. An entry is
           a run of pool pages, so its pages lie in the mirror as they do in guest memory, the edges' CPU bytes too */
        if (g_hooks.landed)
            for (i = 0; i < n; i++) {
                u64 plo = r[i].addr & ~(g_page - 1), phi = (r[i].addr + r[i].size + g_page - 1) & ~(g_page - 1);
                g_hooks.landed(plo, phi - plo, dst[i] - (r[i].addr - plo));
            }
        pm_lock(&g_own_lk);
        for (i = 0; i < n; i++) cut_nl(r[i].addr, r[i].addr + r[i].size, &rel);
        pc_broadcast(&g_own_cv);
        pm_unlock(&g_own_lk);
        free(r);
        free(dst);
        /* the pages go before the resources: no page is left armed with no entry on it past this landing (own_stage
           reads such a page directly) */
        let_go(elo & ~(g_page - 1), (ehi + g_page - 1) & ~(g_page - 1));
        res_release(&rel);
        producer_unlock();
    }
}

/* ---------------------------------------------------------------- traps */
static void report_owned_fault(u64 a, int write, u32 tid, int fault_state) {
    Own owners[16];
    u64 page = a & ~(g_page - 1);
    u32 i, n = 0, total = 0;
    /* Snapshot metadata only: no guest reads, renderer lock, or logging under the map lock. */
    pm_lock(&g_own_lk);
    for (i = first_after_nl(page); i < g_nown && g_own[i].lo < page + g_page; i++) {
        if (n < sizeof owners / sizeof *owners) owners[n++] = g_own[i];
        total++;
    }
    pm_unlock(&g_own_lk);
    log_crash_mode();
    WARN("ownership-fault: thread=%u renderer-holder=%u access=%s guest-page=%llx page-size=%llu "
         "host-page=%p wait=%s\n",
         tid, tid, write ? "write" : "read", (unsigned long long)page, (unsigned long long)g_page, (void*)GP(page),
         fault_state == 1 ? "confirmed" : "unknown");
    for (i = 0; i < n; i++) {
        const Own* o = &owners[i];
        WARN("ownership-fault: owner guest=[%llx,%llx) pool=%llx resource=%p resource-offset=%llx landing=%d\n",
             (unsigned long long)o->lo, (unsigned long long)o->hi, (unsigned long long)o->pool, o->res,
             (unsigned long long)o->res_off, o->landing);
    }
    if (total > n) WARN("ownership-fault: %u additional owners omitted\n", total - n);
    plat_dump_fault_thread(tid);
}

static void own_fault(u8* p, int write, u32 tid) {
    u64 a = (u64)(p - g_mem), page = a & ~(g_page - 1);
    if (p < g_mem || a >= AS_SIZE) { /* not the guest's memory: nothing of ours, let it go */
        plat_trap_continue(p, g_page);
        return;
    }
    for (;;) {
        int owned;
        /* A page no entry is on goes on: landed meanwhile (the message is late), or cut and not let go yet. Checked
           and let go under g_trap_lk, so an owner can't arm it between the two: own_begin arms under it */
        pm_lock(&g_trap_lk);
        pm_lock(&g_own_lk);
        owned = page_owned_nl(page);
        pm_unlock(&g_own_lk);
        if (!owned) plat_trap_continue(p, g_page);
        pm_unlock(&g_trap_lk);
        if (!owned) return;
        /* An owned page faulted by the producer's lock holder: nothing can land it while that thread waits here. The
           holder is asked now, not when it faulted: a late message's thread may have taken the lock since, and
           republished the page, so it ends the process only when that thread is waiting in a fault (or it can't be
           told); otherwise the landing waits for the lock as any other */
        if (tid && g_hooks.busy_tid && tid == g_hooks.busy_tid()) {
            int fault_state = plat_thread_in_fault(tid);
            if (fault_state != 0) {
                report_owned_fault(a, write, tid, fault_state);
                fatal(
                    "Thread %u touched GPU-owned guest memory at %llx while it held the renderer: the runtime must call "
                    "own_acquire first",
                    tid, (unsigned long long)a);
            }
        }
        /* never under g_trap_lk: the landing takes the producer's lock. A page owned again by the time it is done (a
           republish) lands again */
        land_pages(page, page + g_page);
    }
}

int own_init(const OwnHooks* h) {
    g_hooks = *h;
    g_page = plat_vm_page();
    pc_init(&g_own_cv);
    /* SNX_NO_TRAPS: as a host without traps (Windows, macOS), to measure and test that path here */
    if (getenv("SNX_NO_TRAPS") || !plat_alias_mirror() || plat_trap_init(own_fault)) {
        LOG("memory: no traps on this host: GPU engines land their results in guest memory at their fence\n");
        return -1;
    }
    g_traps = 1;
    g_own_nregion = (AS_SIZE + (1ull << OWN_REGION_SHIFT) - 1) >> OWN_REGION_SHIFT;
    g_own_region = (u32*)calloc((size_t)g_own_nregion, sizeof *g_own_region);
    LOG("memory: GPU engines keep their results, guest memory is trapped until the CPU needs it\n");
    return 0;
}

int own_begin(u64 addr, u64 size, void* res, u64 res_off) {
    u64 lo = addr & ~(g_page - 1), hi, end = addr + size;
    MemPoolRun runs[16];
    ResList rel = {0};
    u32 i, n;
    if (!g_traps || !size || addr >= AS_SIZE || size > AS_SIZE - addr) return -1;
    hi = (end + g_page - 1) & ~(g_page - 1);
    /* the pages in the pool, mapped here only (the caller holds the renderer's lock, as host memory changes do) */
    if (mem_own_prepare(lo, hi - lo, runs, sizeof runs / sizeof *runs, &n)) return -1;
    pm_lock(&g_own_lk);
    while (landing_nl(addr, end)) pc_wait(&g_own_cv, &g_own_lk, -1);
    cut_nl(addr, end, &rel);
    for (i = 0; i < n; i++) { /* an entry a run of pool pages, so each has one place in the mirror */
        u64 elo = runs[i].addr > addr ? runs[i].addr : addr;
        u64 ehi = runs[i].addr + runs[i].size < end ? runs[i].addr + runs[i].size : end;
        Own o;
        o.lo = elo;
        o.hi = ehi;
        o.pool = runs[i].pool + (elo - runs[i].addr);
        o.res = res;
        o.res_off = res_off + (elo - addr);
        o.landing = 0;
        g_hooks.retain(res);
        insert_nl(first_after_nl(elo), &o);
    }
    pm_unlock(&g_own_lk);
    res_release(&rel);
    /* the pages trapped from now on: the CPU's bytes on them stay in the pool, the owned ones are stale there */
    pm_lock(&g_trap_lk);
    if (plat_trap_arm(GP(lo), hi - lo)) fatal("Could not trap guest memory at %llx", (unsigned long long)lo);
    pm_unlock(&g_trap_lk);
    return 0;
}

u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) {
    u64 end = addr + size;
    u32 i, n = 0;
    if (!__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE) || !region_owned(addr, end)) return 0;
    pm_lock(&g_own_lk);
    for (i = first_after_nl(addr); i < g_nown && g_own[i].lo < end; i++, n++)
        if (n < max) {
            const Own* o = &g_own[i];
            u64 lo = o->lo > addr ? o->lo : addr, hi = o->hi < end ? o->hi : end;
            out[n].addr = lo;
            out[n].size = hi - lo;
            out[n].res = o->res;
            out[n].res_off = o->res_off + (lo - o->lo);
        }
    pm_unlock(&g_own_lk);
    return n;
}

/* the CPU's bytes of [a, b) into dst at a - addr: page by page, a page an owner is on through the mirror (its CPU
   bytes are in the pool), the others as they are. A page landed meanwhile reads the same through either */
static void stage_cpu(u64 addr, u64 a, u64 b, u8* dst, const u8* mirror) {
    while (a < b) {
        u64 end = (a & ~(g_page - 1)) + g_page, off;
        int owned;
        if (end > b) end = b;
        pm_lock(&g_own_lk);
        owned = page_owned_nl(a & ~(g_page - 1));
        pm_unlock(&g_own_lk);
        if (owned && !mem_pool_offset(a, &off))
            memcpy(dst + (a - addr), mirror + off, (size_t)(end - a));
        else
            memcpy(dst + (a - addr), GP(a), (size_t)(end - a));
        a = end;
    }
}

u32 own_stage(u64 addr, u64 size, u8* dst, OwnRange* out, u32 max) {
    u64 lo = addr & ~(g_page - 1), hi = (addr + size + g_page - 1) & ~(g_page - 1), at = addr;
    u32 n = own_lookup(addr, size, out, max), i;
    int trapped;
    if (n > max) return n;
    trapped = region_owned(lo, hi);
    if (trapped) {
        pm_lock(&g_own_lk);
        trapped = __atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE) && first_after_nl(lo) < g_nown &&
                  g_own[first_after_nl(lo)].lo < hi;
        pm_unlock(&g_own_lk);
    }
    /* No owner on its pages: read as they are. One just landed may still be armed until its landing lets it go; the
       caller holds the producer's lock, which that landing holds until then, so none is armed here */
    if (!trapped) {
        memcpy(dst, GP(addr), (size_t)size);
        return n;
    }
    /* only the gaps between the owned parts (sorted, apart, clipped to the range): the caller copies those from
       their resources over dst. Copying them too made the CPU copy every target a publish staged (55 of the 58 GB
       the CPU staged in 40 s of one game) for the GPU to overwrite */
    for (i = 0; i <= n; i++) {
        u64 end = i < n ? out[i].addr : addr + size;
        if (end > at) stage_cpu(addr, at, end, dst, plat_alias_mirror());
        if (i < n) at = out[i].addr + out[i].size;
    }
    return n;
}

int own_hash(u64 addr, u64 size, u64* h) {
    u64 lo = addr & ~(g_page - 1), hi = (addr + size + g_page - 1) & ~(g_page - 1);
    u32 i;
    int any = 0;
    if (!size || !__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE) || !region_owned(lo, hi)) return 0;
    pm_lock(&g_own_lk);
    for (i = first_after_nl(lo); i < g_nown && g_own[i].lo < hi; i++) {
        const Own* o = &g_own[i];
        u64 v[4];
        v[0] = o->lo;
        v[1] = o->hi;
        v[2] = (u64)(uintptr_t)o->res;
        v[3] = o->res_off;
        for (u32 k = 0; k < 4; k++) *h = snx_fnv_step(*h, v[k]);
        any = 1;
    }
    pm_unlock(&g_own_lk);
    return any;
}

void own_acquire(u64 addr, u64 size) {
    if (!size || !__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE)) return;
    land_pages(addr & ~(g_page - 1), (addr + size + g_page - 1) & ~(g_page - 1));
}

void own_drop(u64 addr, u64 size) {
    ResList rel = {0};
    if (!size || !__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE)) return;
    pm_lock(&g_own_lk);
    while (landing_nl(addr, addr + size)) pc_wait(&g_own_cv, &g_own_lk, -1);
    u32 first = first_after_nl(addr);
    if (first == g_nown || g_own[first].lo >= addr + size) {
        pm_unlock(&g_own_lk);
        return;
    }
    /* The caller holds the producer's lock, so a landing cannot leave an entry removed but its trap still armed.
       Only pages of entries cut here can need opening. Continuing an unrelated unmap faults its whole range into
       userfaultfd's mappings just before mem.c releases those pages again. */
    u64 lo = g_own[first].lo > addr ? g_own[first].lo : addr, hi = g_own[first].hi;
    for (u32 i = first + 1; i < g_nown && g_own[i].lo < addr + size; i++) hi = g_own[i].hi;
    if (hi > addr + size) hi = addr + size;
    cut_nl(addr, addr + size, &rel);
    pm_unlock(&g_own_lk);
    let_go(lo & ~(g_page - 1), (hi + g_page - 1) & ~(g_page - 1)); /* before the release, as land_pages */
    res_release(&rel);
}
u64 own_landings(void) { return __atomic_load_n(&g_own_lands, __ATOMIC_RELAXED); }
int own_traps(void) { return g_traps; }
