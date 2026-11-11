/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The map of guest memory the GPU holds, and the traps over it (docs/layers/gpu/ownership.md; the trap model is Skyline's,
   read for its design). Entries are byte exact and never overlap; a host page with any owned byte on it is armed
   (plat_trap_arm), so every access to it, the guest's or the runtime's, waits while own_fault lands the entries on
   it through the pool's mirror, and the page is let go. Owned memory is always pool pages mapped at one address
   only (mem_own_prepare): a second mapping of them lands them first, so a fault has one entry to find.

   Locks: the producer's (the renderer's vk_lock), then the renderer's pend_lock, then mem.c's, then g_trap_lk, then
   g_own_lk, which is never held across landing, a resource's release or a wait for the GPU; the producer's lock is
   never taken under g_trap_lk or g_own_lk. A change of guest memory the renderer lets go on without its lock
   (GPU_MAPPING_GUEST_ONLY) has no entry on its pages, and none comes while it runs (own_begin's caller holds pend_lock
   too), so its own_drop and own_acquire find nothing to wait for. Before a landing takes the producer's lock it waits
   for the GPU's work on its entries with no lock held (OwnHooks' wait). A landing takes the producer's lock before it
   marks its entries, and holds it until they are let go and released: every thread that waits on g_own_cv for a mark
   (own_begin, own_drop, a landing) holds that lock too, so the mark is its own, never one of a thread that waits for
   the lock in turn. The landing runs with no lock of ours held, so a thread holding the kernel's lock may fault on
   owned memory, as long as no holder of the producer's lock waits for the kernel's (nvdrv.c's syncpt_add_quiet). One
   holding mem.c's lock may not: holders of the producer's lock wait for it (own_begin, every mapping change), so mem.c
   lands what it touches first (own_acquire). Nor may one holding the producer's lock, on an owned page (its landing
   needs the lock): own_fault ends the process with the address, as that is a bug in the runtime. Its fault on a page no
   entry is on any more goes on.

   A host with no pool (Windows: SNX_PLAT_TRAPS_IN_PLACE) has no mirror. Owned memory is the reservation's own pages,
   which nothing else maps (views are copies there), and a landing writes them in place: the GPU's bytes go to a staging
   copy while the pages stay trapped, then plat_trap_fill opens and fills them, and they are let go. Every entry on a
   page it opens lands with it (close_pages_nl), as a page opened with an entry left on it would show that entry's stale
   bytes. The CPU's bytes on a trapped page can't be read through a mirror either: an edge page's are copied when it is
   first armed (its shadow, which own_stage reads), and a page with none (bytes own_drop uncovered on a page that stays
   trapped) lands before own_stage reads it.

   A watch (own_watch) is an entry with no resource (res NULL): its pages are trapped, but it holds no bytes. A landing
   that finds watches on its pages asks the producer to publish its result there first (OwnHooks' resolve), with the
   producer's lock held and no mark taken. Then the watches left there go, and the owners the publish made land as any
   other. The thread that held the producer's lock before cannot wait for that publish: it drops the watches and reads
   the CPU's bytes, as it would with no watch. In place, a watch's pages each get a shadow when it arms them */
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>
#include "nx/memory/own.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "snx_settings.h"

typedef struct {
    u64 lo, hi;   /* guest bytes */
    u64 pool;     /* the pool offset of lo */
    void* res;    /* the producer's copy, lo at res_off: NULL for a watch (own_watch) */
    u64 res_off;
    int landing;  /* being landed by some thread: the others wait */
} Own;

static u64 g_own_lands; /* landings so far, for the statistics */
static OwnHooks g_hooks;
static int g_traps;    /* own_init found traps */
static u64 g_page;     /* the host's page, which traps are armed in */
static u8* g_mirror;   /* the pool's mirror, which landings write through; NULL where they land in place */
static PMutex g_own_lk = PMUTEX_INIT;
/* arming pages and the check that lets them go, one at a time: a page let go just as an entry lands on it stays
   trapped (taken before g_own_lk) */
static PMutex g_trap_lk = PMUTEX_INIT;
static PCond g_own_cv;
static Own* g_own;     /* sorted by lo, under g_own_lk */
u32 g_nown;            /* own.h's own_any reads it without the lock */
static u32 g_capown;
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

/* ---------------------------------------------------------------- in place (no mirror) */
/* The CPU's bytes of a trapped page, where there is no mirror to read them through: its bytes as they were when it was
   first armed with CPU bytes on it (an owner's edge page, or a watch's page), sorted by page, under g_trap_lk. While the
   page is armed no CPU write can change them: a write lands it first. They go when it is let go.

   The bytes are slots of one slab, a page each, and an entry names its slot: a watch arms tens of pages at once, and
   an allocation for each page took a heap call per page and a page fault on Windows for each new one. A slot let go
   goes on the free list for the next. The slab moves when it grows, so its bytes are read under g_trap_lk only, and it
   goes when no shadow is left */
typedef struct {
    u64 page;
    u32 slot;
} Shadow;
static Shadow* g_shadow;
static u32 g_nshadow, g_capshadow;
static u8* g_slab;        /* g_nslot slots of g_page bytes */
static u32 g_nslot;
static u32 g_slot_top;    /* slots [0, g_slot_top) were handed out at least once */
static u32* g_slot_free;  /* the free ones of those, g_nslot of room */
static u32 g_nfree;
static u32 shadow_at_nl(u64 page) {
    u32 lo = 0, hi = g_nshadow;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_shadow[mid].page < page)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
static u8* slot_bytes(u32 slot) { return g_slab + (size_t)slot * g_page; }
static const u8* shadow_get_nl(u64 page) {
    u32 i = shadow_at_nl(page);
    return i < g_nshadow && g_shadow[i].page == page ? slot_bytes(g_shadow[i].slot) : NULL;
}
static u32 slot_take_nl(void) {
    if (g_nfree) return g_slot_free[--g_nfree];
    if (g_slot_top == g_nslot) {
        u32 cap = g_nslot ? g_nslot * 2 : 64;
        u8* slab = (u8*)realloc(g_slab, (size_t)cap * g_page);
        if (!slab) fatal("Out of memory for the map of GPU-owned memory");
        g_slab = slab;
        u32* free_list = (u32*)realloc(g_slot_free, sizeof *free_list * (size_t)cap);
        if (!free_list) fatal("Out of memory for the map of GPU-owned memory");
        g_slot_free = free_list;
        g_nslot = cap;
    }
    return g_slot_top++;
}
/* the n pages page[] (sorted, apart), with bytes[i * g_page] as page[i]'s, as their shadows: a page with one keeps it,
   from when it was first armed. One pass over the table, as a watch adds its pages at once */
static void shadow_add_nl(const u64* page, const u8* bytes, u32 n) {
    u32 fresh = 0, i = 0;
    for (u32 j = 0; j < n; j++) {
        while (i < g_nshadow && g_shadow[i].page < page[j]) i++;
        if (i == g_nshadow || g_shadow[i].page != page[j]) fresh++;
    }
    if (!fresh) return;
    if (g_nshadow + fresh > g_capshadow) {
        u32 cap = g_capshadow ? g_capshadow : 64;
        while (cap < g_nshadow + fresh) cap *= 2;
        Shadow* t = (Shadow*)realloc(g_shadow, sizeof *t * (size_t)cap);
        if (!t) fatal("Out of memory for the map of GPU-owned memory");
        g_shadow = t;
        g_capshadow = cap;
    }
    /* merged from the end, so no entry moves twice */
    u32 k = g_nshadow + fresh;
    i = g_nshadow;
    for (u32 j = n; j-- > 0;) {
        while (i && g_shadow[i - 1].page > page[j]) g_shadow[--k] = g_shadow[--i];
        if (i && g_shadow[i - 1].page == page[j]) continue;
        u32 slot = slot_take_nl();
        memcpy(slot_bytes(slot), bytes + (size_t)j * g_page, (size_t)g_page);
        g_shadow[--k] = (Shadow){.page = page[j], .slot = slot};
    }
    g_nshadow += fresh;
}
/* the pages [lo, hi) let go: their shadows go, and their slots are free */
static void shadow_drop_nl(u64 lo, u64 hi) {
    u32 i = shadow_at_nl(lo), j = i;
    while (j < g_nshadow && g_shadow[j].page < hi) g_slot_free[g_nfree++] = g_shadow[j++].slot;
    memmove(&g_shadow[i], &g_shadow[j], sizeof *g_shadow * (size_t)(g_nshadow - j));
    g_nshadow -= j - i;
    if (g_nshadow) return;
    free(g_slab); /* none left: the slab goes, as a burst of watches made it big */
    free(g_slot_free);
    g_slab = NULL;
    g_slot_free = NULL;
    g_nslot = g_slot_top = g_nfree = 0;
}
/* A buffer kept for the next use up to keep bytes: a fresh allocation of megabytes takes a page fault per page on
   Windows. Its users run one at a time under the producer's lock, but a test's may not: a second one at once gets an
   allocation of its own */
typedef struct {
    u8* p;
    u64 cap;
    int busy; /* atomic */
} Scratch;
static u8* scratch_get(Scratch* s, u64 size, u64 keep) {
    int none = 0;
    if (size <= keep && __atomic_compare_exchange_n(&s->busy, &none, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        if (size <= s->cap) return s->p;
        free(s->p);
        s->p = (u8*)malloc((size_t)size);
        s->cap = s->p ? size : 0;
        if (s->p) return s->p;
        __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
    }
    u8* p = (u8*)malloc((size_t)(size ? size : 1));
    if (!p) fatal("Out of memory for GPU-owned memory (%llu KiB)", (unsigned long long)(size >> 10));
    return p;
}
static void scratch_put(Scratch* s, u8* p) {
    if (p && p == s->p && __atomic_load_n(&s->busy, __ATOMIC_RELAXED))
        __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
    else
        free(p);
}
#define STAGE_KEEP ((u64)64 << 20)
static Scratch g_stage; /* a landing's staging copy, in place */
#define SNAP_KEEP ((u64)8 << 20)
static Scratch g_snap; /* the pages own_watch and own_begin copy into shadows */

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
    cover_nl(o, 1);
    __atomic_store_n(&g_nown, g_nown + 1, __ATOMIC_RELEASE);
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
    if (!res) return; /* a watch's */
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
            if (right.res) g_hooks.retain(right.res);
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
            /* Entries are sorted and apart, so all fully covered entries form one run. Move the suffix only once:
               dropping or replacing a range over many owners must not shift that suffix once for every owner. */
            u32 past = i;
            do {
                res_push(rel, g_own[past].res);
                cover_nl(&g_own[past], (u32)-1);
                past++;
            } while (past < g_nown && g_own[past].lo < hi && g_own[past].hi <= hi);
            memmove(&g_own[i], &g_own[past], sizeof *g_own * (size_t)(g_nown - past));
            __atomic_store_n(&g_nown, g_nown - (past - i), __ATOMIC_RELEASE);
        }
    }
}

/* own_begin's taking of [lo, hi) in one move: the entries over it go (their resources to release in rel), and the n
   entries nw (sorted by lo, apart, within [lo, hi)) take their place. cut_nl then one insert_nl per entry would move
   the suffix once for the cut and once for each insert, though a range re-owned with the same count ends where it
   began. Here it moves once, and not at all when n is the count it replaces */
static void own_replace(u64 lo, u64 hi, const Own* nw, u32 n, ResList* rel) {
    u32 i = first_after_nl(lo), past = i;
    while (past < g_nown && g_own[past].lo < hi) past++;
    int left = i < past && g_own[i].lo < lo;         /* the first sticks out below lo */
    int right = past > i && g_own[past - 1].hi > hi; /* the last sticks out above hi */
    u32 from = i, to = past, added = n;
    Own up;                                          /* the upper part of an entry split by [lo, hi) */
    int split = left && right && i + 1 == past;      /* one entry spans [lo, hi) wholly */
    if (split) {
        Own* o = &g_own[i];
        up = *o;
        up.res_off += hi - o->lo;
        up.pool += hi - o->lo;
        up.lo = hi;
        cover_nl(o, (u32)-1);
        o->hi = lo;
        cover_nl(o, 1);
        if (up.res) g_hooks.retain(up.res); /* the upper part keeps the resource beside the lower one */
        from = to = i + 1;      /* nothing removed: the new entries follow the lower part */
        added = n + 1;
    } else {
        if (left) {
            cover_nl(&g_own[i], (u32)-1);
            g_own[i].hi = lo;
            cover_nl(&g_own[i], 1);
            from = i + 1;
        }
        if (right) {
            Own* o = &g_own[past - 1];
            o->res_off += hi - o->lo;
            o->pool += hi - o->lo;
            cover_nl(o, (u32)-1);
            o->lo = hi;
            cover_nl(o, 1);
            to = past - 1;
        }
    }
    u32 removed = to - from;
    u32 total = g_nown - removed + added;
    if (total > g_capown) {
        u32 cap = g_capown ? g_capown : 64;
        while (cap < total) cap *= 2;
        Own* grown = (Own*)realloc(g_own, sizeof *grown * (size_t)cap);
        if (!grown) fatal("Out of memory for the map of GPU-owned memory");
        g_own = grown;
        g_capown = cap;
    }
    for (u32 k = from; k < to; k++) {
        res_push(rel, g_own[k].res);
        cover_nl(&g_own[k], (u32)-1);
    }
    /* one move for the net change (none when the count is replaced), then the new entries fill the gap */
    if (added != removed && g_nown > to)
        memmove(&g_own[from + added], &g_own[to], sizeof *g_own * (size_t)(g_nown - to));
    __atomic_store_n(&g_nown, g_nown - removed + added, __ATOMIC_RELEASE);
    for (u32 k = 0; k < n; k++) {
        g_own[from + k] = nw[k];
        cover_nl(&nw[k], 1);
    }
    if (split) {
        g_own[from + n] = up;
        cover_nl(&up, 1);
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
        if (!g_mirror) shadow_drop_nl(a, run);
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

/* ---------------------------------------------------------------- watches (res NULL) */
/* the first entry at or after i that is a real owner */
static u32 next_real_nl(u32 i) {
    while (i < g_nown && !g_own[i].res) i++;
    return i;
}
/* whether a watch is on [lo, hi) */
static int watched_nl(u64 lo, u64 hi) {
    for (u32 i = first_after_nl(lo); i < g_nown && g_own[i].lo < hi; i++)
        if (!g_own[i].res) return 1;
    return 0;
}
/* the watches on [lo, hi) go, one at a time: whole when whole is set, else only their bytes in [lo, hi). Their pages
   open where no entry is left, now, or with keep the caller's landing's let_go opens them: the pages widen [*keep,
   keep[1]). In place, opening a page wakes every thread that waits in a trap, and one on a page the landing fills
   then can read it half written (plat_trap_fill). Never under a lock of own.c's */
static void unwatch(u64 lo, u64 hi, int whole, u64* keep) {
    for (u64 c = lo; c < hi;) {
        ResList rel = {0};
        pm_lock(&g_own_lk);
        u32 i = first_after_nl(c);
        while (i < g_nown && g_own[i].lo < hi && g_own[i].res) i++;
        if (i >= g_nown || g_own[i].lo >= hi) {
            pm_unlock(&g_own_lk);
            return;
        }
        u64 a = g_own[i].lo, b = g_own[i].hi;
        if (!whole) {
            if (a < c) a = c;
            if (b > hi) b = hi;
        }
        cut_nl(a, b, &rel); /* the watch's bytes alone: no other entry is on them */
        pm_unlock(&g_own_lk);
        u64 plo = a & ~(g_page - 1), phi = (b + g_page - 1) & ~(g_page - 1);
        if (!keep)
            let_go(plo, phi);
        else {
            if (plo < keep[0]) keep[0] = plo;
            if (phi > keep[1]) keep[1] = phi;
        }
        res_release(&rel);
        c = b;
    }
}
/* the watches on [lo, hi), found by a landing with the producer's lock held and no mark taken: the producer publishes
   its result over them (unless this thread held its lock before, busy), then every watch on [lo, hi) goes whole. Its
   pages widen keep, which the landing lets go of when it is done (unwatch) */
static void watch_hit(u64 lo, u64 hi, int busy, u64* keep) {
    if (!busy && g_hooks.resolve) g_hooks.resolve(lo, hi - lo);
    unwatch(lo, hi, 1, keep);
}

/* In place (no mirror): every entry on the pages of the entries [*first, *first + *n) joins them, until no page they
   are on holds another, so the pages the landing opens hold no entry it leaves behind. 0 when one of those is being
   landed (by a thread without the producer's lock: a test's), to be waited for */
static int close_pages_nl(u32* first, u32* n) {
    for (;;) {
        u64 plo = g_own[*first].lo & ~(g_page - 1), phi = (g_own[*first + *n - 1].hi + g_page - 1) & ~(g_page - 1);
        u32 f = first_after_nl(plo), e = f, i;
        while (e < g_nown && g_own[e].lo < phi) e++;
        for (i = f; i < e; i++)
            if ((i < *first || i >= *first + *n) && g_own[i].landing) return 0;
        if (f == *first && e == *first + *n) return 1;
        *first = f;
        *n = e - f;
    }
}

/* every entry on the host pages [lo, hi) landed and let go, the pages with them, once. The producer's lock is taken
   only when entries are there (a fault with none, the trap thread's, never waits for it), before any is marked, then
   the entries are looked for again: they may have landed or changed while it waited. No owner can come while it is
   held (own_begin's callers hold it), so every page lands and is let go; an owner that publishes over them after the
   lock is let go stays until something touches its bytes, as any other */
static void land_pages(u64 lo, u64 hi) {
    OwnRange* r;
    u8** dst;
    u8* mirror = g_mirror;
    u8* stage = NULL;
    u64 elo = lo, ehi = hi, writer = 0, total = 0, at;
    u32 i, first, n = 0;
    int any, real = 0;
    ResList rel = {0};
    u64 kept[2] = {~0ull, 0}; /* the pages of the watches it dropped, let go of at its end */
    if (!region_owned(lo, hi)) return;
    /* asked before the producer's lock is taken: after it, every thread holds it */
    int busy = g_hooks.busy_tid && g_hooks.busy_tid() == plat_tid();
    pm_lock(&g_own_lk);
    any = owned_nl(lo, hi);
    for (i = first_after_nl(lo); any && i < g_nown && g_own[i].lo < hi; i++) {
        if (!g_own[i].res) continue; /* a watch: no GPU work of its own */
        real = 1;
        if (!g_hooks.writer) break;
        u64 w = g_hooks.writer(g_own[i].res);
        if (w > writer) writer = w;
    }
    pm_unlock(&g_own_lk);
    if (!any) return;
    /* the GPU's work first, with no lock held: under the producer's lock, every thread that takes it waited too
       (in one game's menus a landing held it 7.9 s, waiting for a draw's pipeline, and the GPU thread and the
       game's memory thread with it). Once: an entry published again meanwhile lands under the lock, as before */
    if (real && g_hooks.wait) {
        g_hooks.wait(writer);
        if (!region_owned(lo, hi)) return;
        pm_lock(&g_own_lk);
        any = owned_nl(lo, hi);
        pm_unlock(&g_own_lk);
        if (!any) return;
    }
    producer_lock();
    pm_lock(&g_own_lk);
    for (;;) {
        while (landing_nl(lo, hi)) pc_wait(&g_own_cv, &g_own_lk, -1);
        /* watches first, before any mark: the producer's publish over them may wait for marks (own_begin) */
        if (watched_nl(lo, hi)) {
            pm_unlock(&g_own_lk);
            watch_hit(lo, hi, busy, kept);
            pm_lock(&g_own_lk);
            continue;
        }
        first = first_after_nl(lo);
        for (n = 0, i = first; i < g_nown && g_own[i].lo < hi; i++) n++;
        if (!n) {
            pm_unlock(&g_own_lk);
            if (kept[1]) let_go(kept[0], kept[1]);
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
        /* no mirror (Windows): every entry on the pages being opened lands with them */
        if (mirror) break;
        if (close_pages_nl(&first, &n)) {
            /* a watch among them, on a page that an owner here shares: answered as one on [lo, hi) */
            u32 w = first;
            while (w < first + n && g_own[w].res) w++;
            if (w == first + n) break;
            u64 wlo = g_own[w].lo & ~(g_page - 1), whi = (g_own[w].hi + g_page - 1) & ~(g_page - 1);
            pm_unlock(&g_own_lk);
            watch_hit(wlo, whi, busy, kept);
            pm_lock(&g_own_lk);
            continue;
        }
        pc_wait(&g_own_cv, &g_own_lk, -1);
    }
    r = (OwnRange*)malloc(sizeof *r * n);
    dst = (u8**)malloc(sizeof *dst * n);
    if (!r || !dst) fatal("Out of memory landing GPU-owned memory");
    for (i = 0; i < n; i++) total += g_own[first + i].hi - g_own[first + i].lo;
    /* in place: the GPU's bytes wait there while the pages stay trapped */
    if (!mirror) stage = scratch_get(&g_stage, total, STAGE_KEEP);
    for (i = 0, at = 0; i < n; i++) {
        Own* o = &g_own[first + i];
        o->landing = 1;
        r[i].addr = o->lo;
        r[i].size = o->hi - o->lo;
        r[i].res = o->res;
        r[i].res_off = o->res_off;
        dst[i] = mirror ? mirror + o->pool : stage + at;
        at += r[i].size;
        if (o->lo < elo) elo = o->lo;
        if (o->hi > ehi) ehi = o->hi;
    }
    pm_unlock(&g_own_lk);
    g_hooks.land(r, dst, n);
    __atomic_add_fetch(&g_own_lands, 1, __ATOMIC_RELAXED);
    if (!mirror)
        for (i = 0; i < n; i++)
            if (plat_trap_fill(GP(r[i].addr), dst[i], r[i].size))
                fatal("Could not land GPU-owned memory at %llx (%llu bytes)", (unsigned long long)r[i].addr,
                      (unsigned long long)r[i].size);
    /* the landed pages through the mirror, still trapped: no thread can write them before let_go below. An entry is
       a run of pool pages, so its pages lie in the mirror as they do in guest memory, the edges' CPU bytes too.
       Without a mirror, the pages that plat_trap_fill filled */
    if (g_hooks.landed)
        for (i = 0; i < n; i++) {
            u64 plo = r[i].addr & ~(g_page - 1), phi = (r[i].addr + r[i].size + g_page - 1) & ~(g_page - 1);
            g_hooks.landed(plo, phi - plo, mirror ? dst[i] - (r[i].addr - plo) : GP(plo));
        }
    pm_lock(&g_own_lk);
    /* The snapshot is a consecutive run of entries, and the producer's lock prevents another owner coming between
       them while they land. Remove the run at once, including empty gaps, instead of moving the suffix n times. */
    cut_nl(r[0].addr, r[n - 1].addr + r[n - 1].size, &rel);
    pc_broadcast(&g_own_cv);
    pm_unlock(&g_own_lk);
    free(r);
    free(dst);
    scratch_put(&g_stage, stage);
    /* the pages go before the resources: no page is left armed with no entry on it past this landing (own_stage
       reads such a page directly) */
    let_go(elo & ~(g_page - 1), (ehi + g_page - 1) & ~(g_page - 1));
    if (kept[1]) let_go(kept[0], kept[1]);
    res_release(&rel);
    producer_unlock();
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
    int owned;
    if (p < g_mem || a >= AS_SIZE) { /* not the guest's memory: nothing of ours, let it go */
        plat_trap_continue(p, g_page);
        return;
    }
    /* A page no entry is on goes on: landed meanwhile (the message is late), or cut and not let go yet. Checked and
       let go under g_trap_lk, so an owner can't arm it between the two: own_begin arms under it */
    pm_lock(&g_trap_lk);
    pm_lock(&g_own_lk);
    owned = page_owned_nl(page);
    pm_unlock(&g_own_lk);
    if (!owned) {
        plat_trap_continue(p, g_page);
        if (!g_mirror) shadow_drop_nl(page, page + g_page);
    }
    pm_unlock(&g_trap_lk);
    if (!owned) return;
    /* An owned page faulted by the producer's lock holder: nothing can land it while that thread waits here. The holder
       is asked now, not when it faulted: a late message's thread may have taken the lock since, and republished the
       page, so it ends the process only when that thread is waiting in a fault (or it can't be told); otherwise the
       landing waits for the lock as any other */
    if (tid && g_hooks.busy_tid && tid == g_hooks.busy_tid()) {
        int fault_state = plat_thread_in_fault(tid);
        if (fault_state != 0) {
            /* only watches there: they go, and the thread reads the CPU's bytes, as with no watch (own_watch) */
            pm_lock(&g_own_lk);
            u32 i = next_real_nl(first_after_nl(page));
            int real = i < g_nown && g_own[i].lo < page + g_page;
            pm_unlock(&g_own_lk);
            if (!real) {
                unwatch(page, page + g_page, 1, NULL);
                return;
            }
            report_owned_fault(a, write, tid, fault_state);
            fatal("Thread %u touched GPU-owned guest memory at %llx while it held the renderer: the runtime must call "
                  "own_acquire first",
                  tid, (unsigned long long)a);
        }
    }
    /* never under g_trap_lk: the landing takes the producer's lock. Its let_go wakes the faulting thread, which may
       publish over the page again before this returns: that owner lands when an access faults on it, not here (a VIC
       job's picture, written over the one just read, stays on the GPU for the display) */
    land_pages(page, page + g_page);
}

int own_init(const OwnHooks* h) {
    g_hooks = *h;
    g_page = plat_vm_page();
    pc_init(&g_own_cv);
    /* memory_traps=2: as a host without traps (macOS, FreeBSD), to measure and test that path here. Without a mirror
       only a host whose traps land in place traps (Windows) */
    g_mirror = plat_alias_mirror();
    if (g_settings.memory_traps == 2 || (!g_mirror && !SNX_PLAT_TRAPS_IN_PLACE) || plat_trap_init(own_fault)) {
        LOG("memory: no traps (%s): GPU engines land their results in guest memory at their fence\n",
            g_settings.memory_traps == 2   ? "memory_traps=2"
            : g_settings.memory_traps == 1 ? "memory_traps=1, page protection, has none on this host"
                                           : "none on this host");
        g_mirror = NULL;
        return -1;
    }
    g_traps = 1;
    g_own_nregion = (AS_SIZE + (1ull << OWN_REGION_SHIFT) - 1) >> OWN_REGION_SHIFT;
    g_own_region = (u32*)calloc((size_t)g_own_nregion, sizeof *g_own_region);
    LOG("memory: GPU engines keep their results, guest memory is trapped until the CPU needs it%s\n",
        g_mirror ? "" : " (page protection and an exception handler, landing in place)");
    return 0;
}

/* In place: the pages of [lo, hi) that no entry arms yet (so readable), appended to page[] from *n on, with page[i]'s
   bytes at bytes + i * g_page. Read without faulting: the caller holds the producer's lock, which a landing the trap's
   thread may be in needs. Each run of such pages is one read. A page that can't be read was armed since (by an
   own_begin with no producer's lock: a test's) and keeps what it has */
static void snap_pages(u64 lo, u64 hi, u64* page, u8* bytes, u32* n) {
    u32 from = *n, w = from;
    pm_lock(&g_own_lk);
    for (u64 p = lo; p < hi; p += g_page)
        if (!page_owned_nl(p)) page[(*n)++] = p;
    pm_unlock(&g_own_lk);
    for (u32 i = from; i < *n;) {
        u32 e = i + 1;
        while (e < *n && page[e] == page[e - 1] + g_page) e++;
        if (!plat_vm_read(GP(page[i]), bytes + (size_t)w * g_page, (u64)(e - i) * g_page))
            for (u32 k = i; k < e; k++) page[w++] = page[k];
        else /* one of them armed meanwhile: page by page, the readable ones */
            for (u32 k = i; k < e; k++)
                if (!plat_vm_read(GP(page[k]), bytes + (size_t)w * g_page, g_page)) page[w++] = page[k];
        i = e;
    }
    *n = w;
}

int own_begin(u64 addr, u64 size, void* res, u64 res_off) {
    u64 lo = addr & ~(g_page - 1), hi, end = addr + size, edge[2];
    MemPoolRun runs[16];
    ResList rel = {0};
    u8* snap = NULL;
    u32 i, n, nedge = 0;
    if (!g_traps || !size || addr >= AS_SIZE || size > AS_SIZE - addr) return -1;
    hi = (end + g_page - 1) & ~(g_page - 1);
    /* the pages in the pool, mapped here only (the caller holds the renderer's lock, as host memory changes do) */
    if (mem_own_prepare(lo, hi - lo, runs, sizeof runs / sizeof *runs, &n)) return -1;
    /* in place: an edge page with CPU bytes on it, armed by no owner yet (so readable), keeps a copy of them for
       own_stage. One armed already has its copy from then, or none to keep (the older owners' bytes are the GPU's) */
    if (!g_mirror) {
        u64 last = hi - g_page;
        int first_edge = addr > lo || (last == lo && end < hi), last_edge = last != lo && end < hi;
        if (first_edge || last_edge) snap = scratch_get(&g_snap, 2 * g_page, SNAP_KEEP);
        if (first_edge) snap_pages(lo, lo + g_page, edge, snap, &nedge);
        if (last_edge) snap_pages(last, hi, edge, snap, &nedge);
    }
    pm_lock(&g_own_lk);
    while (landing_nl(addr, end)) pc_wait(&g_own_cv, &g_own_lk, -1);
    Own nw[16]; /* an entry a run of pool pages, so each has one place in the mirror */
    for (i = 0; i < n; i++) {
        u64 elo = runs[i].addr > addr ? runs[i].addr : addr;
        u64 ehi = runs[i].addr + runs[i].size < end ? runs[i].addr + runs[i].size : end;
        nw[i] = (Own){.lo = elo,
                      .hi = ehi,
                      .pool = runs[i].pool + (elo - runs[i].addr),
                      .res = res,
                      .res_off = res_off + (elo - addr),
                      .landing = 0};
        g_hooks.retain(res);
    }
    own_replace(addr, end, nw, n, &rel);
    pm_unlock(&g_own_lk);
    res_release(&rel);
    /* the pages trapped from now on: the CPU's bytes on them stay in the pool, the owned ones are stale there */
    pm_lock(&g_trap_lk);
    if (nedge) shadow_add_nl(edge, snap, nedge);
    if (plat_trap_arm(GP(lo), hi - lo)) fatal("Could not trap guest memory at %llx", (unsigned long long)lo);
    pm_unlock(&g_trap_lk);
    if (snap) scratch_put(&g_snap, snap);
    return 0;
}

u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) {
    u64 end = addr + size;
    u32 i, n = 0;
    if (!own_any() || !region_owned(addr, end)) return 0;
    pm_lock(&g_own_lk);
    for (i = next_real_nl(first_after_nl(addr)); i < g_nown && g_own[i].lo < end; i = next_real_nl(i + 1), n++)
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
        u64 page = a & ~(g_page - 1), end = page + g_page, off;
        int owned;
        if (end > b) end = b;
        pm_lock(&g_own_lk);
        owned = page_owned_nl(page);
        pm_unlock(&g_own_lk);
        if (owned && !mirror) { /* in place: the page's shadow (own_stage landed a page with none first) */
            const u8* sh = NULL;
            /* through a buffer of this thread's: dst is written with no lock of ours held, as a write to it that
               faulted (dst in guest memory) would wait for a trap that needs g_trap_lk */
            for (u64 at = a; at < end;) {
                u8 part[1024];
                u64 n = end - at < sizeof part ? end - at : sizeof part;
                pm_lock(&g_trap_lk);
                sh = shadow_get_nl(page);
                if (sh) memcpy(part, sh + (at - page), (size_t)n);
                pm_unlock(&g_trap_lk);
                if (!sh) break;
                memcpy(dst + (at - addr), part, (size_t)n);
                at += n;
            }
            if (!sh) {
                static int said;
                memset(dst + (a - addr), 0, (size_t)(end - a));
                if (!__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED))
                    WARN("memory: the CPU's bytes of trapped page %llx have no copy: read as zero\n",
                         (unsigned long long)page);
            }
        } else if (owned && !mem_pool_offset(a, &off))
            memcpy(dst + (a - addr), mirror + off, (size_t)(end - a));
        else
            memcpy(dst + (a - addr), GP(a), (size_t)(end - a));
        a = end;
    }
}
/* In place: the first page of [addr, end) whose CPU bytes the range reads and no shadow holds (bytes own_drop
   uncovered on a page an owner keeps trapped), in *at. Only a gap between the entries can be on such a page, and only
   on its first and last pages: an entry on a page within it would be in it. Under g_trap_lk and g_own_lk */
static int unshadowed_nl(u64 addr, u64 end, u64* at) {
    u64 c = addr;
    u32 i = next_real_nl(first_after_nl(addr)); /* a watch's bytes are the CPU's: a gap's */
    for (;;) {
        u64 next = i < g_nown && g_own[i].lo < end ? (g_own[i].lo > addr ? g_own[i].lo : addr) : end;
        if (next > c) {
            u64 p0 = c & ~(g_page - 1), p1 = (next - 1) & ~(g_page - 1);
            if (page_owned_nl(p0) && !shadow_get_nl(p0)) {
                *at = p0;
                return 1;
            }
            if (p1 != p0 && page_owned_nl(p1) && !shadow_get_nl(p1)) {
                *at = p1;
                return 1;
            }
        }
        if (next >= end || g_own[i].hi >= end) return 0;
        c = g_own[i].hi;
        i = next_real_nl(i + 1);
    }
}

u32 own_stage(u64 addr, u64 size, u8* dst, OwnRange* out, u32 max) {
    u64 lo = addr & ~(g_page - 1), hi = (addr + size + g_page - 1) & ~(g_page - 1), at = addr;
    u32 n, i;
    int trapped;
    /* in place, a trapped page whose CPU bytes it reads and no shadow holds lands first: before the lookup, whose
       resources a landing would release */
    if (!g_mirror && g_traps && own_any() && region_owned(lo, hi)) {
        if ((n = own_lookup(addr, size, NULL, 0)) > max) return n;
        for (;;) {
            u64 page;
            int need;
            pm_lock(&g_trap_lk);
            pm_lock(&g_own_lk);
            need = unshadowed_nl(addr, addr + size, &page);
            pm_unlock(&g_own_lk);
            pm_unlock(&g_trap_lk);
            if (!need) break;
            land_pages(page, page + g_page);
        }
    }
    n = own_lookup(addr, size, out, max);
    if (n > max) return n;
    trapped = region_owned(lo, hi);
    if (trapped) {
        pm_lock(&g_own_lk);
        trapped = first_after_nl(lo) < g_nown && g_own[first_after_nl(lo)].lo < hi;
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
        if (end > at) stage_cpu(addr, at, end, dst, g_mirror);
        if (i < n) at = out[i].addr + out[i].size;
    }
    return n;
}

int own_hash(u64 addr, u64 size, u64* h) {
    u64 lo = addr & ~(g_page - 1), hi = (addr + size + g_page - 1) & ~(g_page - 1);
    u32 i;
    int any = 0;
    if (!size || !own_any() || !region_owned(lo, hi)) return 0;
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

int own_watch(u64 addr, u64 size) {
    u64 lo = addr & ~(g_page - 1), hi, end = addr + size;
    MemPoolRun runs[16];
    ResList rel = {0};
    u64* snap_page = NULL;
    u8* snap = NULL;
    u32 i, n, nsnap = 0;
    if (!g_traps || !size || addr >= AS_SIZE || size > AS_SIZE - addr) return -1;
    hi = (end + g_page - 1) & ~(g_page - 1);
    if (mem_own_prepare(lo, hi - lo, runs, sizeof runs / sizeof *runs, &n)) return -1;
    /* in place: each page armed by no entry yet keeps a copy of its bytes, all of them the CPU's, for own_stage. Read
       without faulting, as own_begin's edges: a page that can't be read is armed already and keeps what it has */
    if (!g_mirror) { /* the pages' bytes, then the list of them, in one buffer kept for the next */
        u64 pages = (hi - lo) / g_page;
        snap = scratch_get(&g_snap, pages * (g_page + sizeof *snap_page), SNAP_KEEP);
        snap_page = (u64*)(void*)(snap + pages * g_page);
        snap_pages(lo, hi, snap_page, snap, &nsnap);
    }
    pm_lock(&g_own_lk);
    while (landing_nl(addr, end)) pc_wait(&g_own_cv, &g_own_lk, -1);
    /* each run of pool pages, in the gaps between the owners on it: an older watch there is replaced */
    for (i = 0; i < n; i++) {
        u64 rlo = runs[i].addr > addr ? runs[i].addr : addr;
        u64 rhi = runs[i].addr + runs[i].size < end ? runs[i].addr + runs[i].size : end;
        for (u64 c = rlo; c < rhi;) {
            u32 j = next_real_nl(first_after_nl(c));
            u64 gap = j < g_nown && g_own[j].lo < rhi ? g_own[j].lo : rhi;
            u64 next = gap < rhi ? g_own[j].hi : rhi; /* before the replace moves the entries */
            if (gap > c) {
                Own w = {.lo = c, .hi = gap, .pool = runs[i].pool + (c - runs[i].addr), .res = NULL};
                own_replace(c, gap, &w, 1, &rel);
            }
            c = next;
        }
    }
    pm_unlock(&g_own_lk);
    res_release(&rel);
    pm_lock(&g_trap_lk);
    if (nsnap) shadow_add_nl(snap_page, snap, nsnap);
    if (plat_trap_arm(GP(lo), hi - lo)) fatal("Could not trap guest memory at %llx", (unsigned long long)lo);
    pm_unlock(&g_trap_lk);
    if (snap) scratch_put(&g_snap, snap);
    return 0;
}

int own_watched(u64 addr, u64 size) {
    u64 c = addr, end = addr + size;
    int all = 1;
    if (!size || !own_any() || !region_owned(addr, end)) return 0;
    pm_lock(&g_own_lk);
    for (u32 i = first_after_nl(addr); c < end; i++) {
        if (i >= g_nown || g_own[i].lo > c || g_own[i].res) {
            all = 0;
            break;
        }
        c = g_own[i].hi;
    }
    pm_unlock(&g_own_lk);
    return all;
}

void own_unwatch(u64 addr, u64 size) {
    if (size && own_any() && region_owned(addr, addr + size)) unwatch(addr, addr + size, 0, NULL);
}

int own_trapped(u64 addr, u64 size) {
    u64 lo = addr & ~(g_page - 1), hi = (addr + size + g_page - 1) & ~(g_page - 1);
    int any;
    if (!size || !own_any() || !region_owned(lo, hi)) return 0;
    pm_lock(&g_own_lk);
    any = owned_nl(lo, hi);
    pm_unlock(&g_own_lk);
    return any;
}

void own_acquire(u64 addr, u64 size) {
    if (!size || !own_any()) return;
    land_pages(addr & ~(g_page - 1), (addr + size + g_page - 1) & ~(g_page - 1));
}

void own_drop(u64 addr, u64 size) {
    ResList rel = {0};
    if (!size || !own_any()) return;
    pm_lock(&g_own_lk);
    while (landing_nl(addr, addr + size)) pc_wait(&g_own_cv, &g_own_lk, -1);
    u32 first = first_after_nl(addr);
    if (first == g_nown || g_own[first].lo >= addr + size) {
        pm_unlock(&g_own_lk);
        return;
    }
    /* The caller holds the producer's lock (or, without it, there was no entry: see Locks above), so a landing cannot
       leave an entry removed but its trap still armed.
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
