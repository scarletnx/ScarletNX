/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest memory the GPU holds (own.c, docs/layers/gpu/ownership.md): what a GPU engine wrote stays in its GPU copy, and the
   guest's own pages under it are trapped, so a CPU access lands the copy first */
#ifndef SNX_MEMORY_OWN_H
#define SNX_MEMORY_OWN_H
#include "common/cpu/aarch64/cpu.h"
#include "common/platform/thread.h"

/* guest bytes [addr, addr + size) whose contents are in the producer's resource res, from res_off on */
typedef struct {
    u64 addr, size;
    void* res;
    u64 res_off;
} OwnRange;

/* the producer's side (the renderer's): what owning, landing and letting go of a resource does */
typedef struct {
    /* the n ranges' contents from their resources written to dst[i] (host memory that isn't trapped), done when it
       returns: it waits for the GPU. Called with the producer's lock held (lock) and none of own.c's, on any thread */
    void (*land)(const OwnRange* r, u8* const* dst, u32 n);
    void (*retain)(void* res);
    void (*release)(void* res); /* may take the producer's lock */
    /* the thread (plat_tid) holding the producer's lock, or 0: its fault on an owned page can't be answered (the
       landing needs that lock), and ends the process; one on a page no entry is on any more goes on */
    u32 (*busy_tid)(void);
    /* the producer's lock (the renderer's vk_lock), reentrant: a landing takes it before it marks its entries, and
       every own_begin and own_drop caller holds it */
    void (*lock)(void);
    void (*unlock)(void);
    /* the host pages [addr, addr + size) of a landing, whose bytes (size of them) are as it left them, read before
       the pages are let go of: the write tracking, which takes pages it watches again for written, can tell a later
       write from them. Called with the producer's lock held. Optional */
    void (*landed)(u64 addr, u64 size, const u8* bytes);
    /* before a landing takes the producer's lock: writer(res) of each entry it would land (called under own.c's
       lock), and wait(the largest of them) with no lock held, which may wait for the GPU. The landing is done under
       the producer's lock as before, and finds that work done. Optional */
    u64 (*writer)(void* res);
    void (*wait)(u64 writer);
    /* a CPU access of a watched page [addr, addr + size) (own_watch): the producer publishes what it holds of those
       bytes over them now (own_begin), and the access then lands that. Called with the producer's lock held and no lock
       or mark of own.c's, on a thread that did not hold the producer's lock before. The watches left over the bytes
       after it go. Optional: without it, a watch only goes */
    void (*resolve)(u64 addr, u64 size);
    /* a landing of the host pages [lo, hi), once it holds the producer's lock and before it looks for their entries:
       the producer publishes over them what it held back from them (a storage image the guest released, which nothing
       read since), so the landing finds those bytes. Called with the producer's lock held and none of own.c's.
       Optional */
    void (*claim)(u64 lo, u64 hi);
} OwnHooks;

/* 0 when the host can trap guest memory (plat_trap_init): own_begin can work from then on. -1: producers land their
   results in guest memory at their own fence */
int own_init(const OwnHooks* h);
/* [addr, addr + size) is res's from res_off on, from now on: older owners of those bytes let go of them. 0, or -1
   when the range can't be trapped (no traps, memory that isn't mapped or is mapped somewhere else too): the producer
   then lands it itself. The caller holds the producer's lock (the renderer's, which host memory changes take) */
int own_begin(u64 addr, u64 size, void* res, u64 res_off);
/* A watch over [addr, addr + size): its pages are trapped as an owner's, but no resource holds the bytes. The producer
   holds a result for them that it did not publish yet (a render target drawn since the last release). The first CPU
   access asks the producer for it (OwnHooks' resolve), and lands what it publishes. A watch holds no bytes: own_lookup
   and own_stage skip it, and its CPU bytes stay readable through the mirror (or, in place, a shadow of each page). It
   never cuts a real owner: it goes into the gaps between them, over older watches. A thread that held the producer's
   lock before it touched a watched page drops the watch and goes on with the CPU's bytes: it cannot wait for a publish.
   0, or -1 as own_begin. The caller holds the producer's lock */
int own_watch(u64 addr, u64 size);
/* 1 when watches cover every byte of [addr, addr + size) */
int own_watched(u64 addr, u64 size);
/* the watches over [addr, addr + size) go (clipped to it), and their pages open where no entry is left. Nothing lands.
   The caller holds the producer's lock */
void own_unwatch(u64 addr, u64 size);
/* 1 when an owner or a watch is on a host page of [addr, addr + size): the pages are trapped, and a direct read of
   them faults. own_lookup counts only owners, and only their bytes */
int own_trapped(u64 addr, u64 size);
/* the owned parts of [addr, addr + size), in address order and clipped to it, into out: how many there are (more
   than max when they didn't all fit). With the producer's lock held, as the resources live while it is */
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max);
/* the CPU's bytes of [addr, addr + size) copied to dst without touching a trapped page (read through the pool's
   mirror), and its owned parts (clipped, in address order) in out, whose bytes in dst are left as they were: the
   caller copies them from their resources over dst there. How many owned parts there are; more than max when they didn't fit, and
   nothing was copied then. With the producer's lock held, as own_lookup */
u32 own_stage(u64 addr, u64 size, u8* dst, OwnRange* out, u32 max);
/* change detection over [addr, addr + size) without touching it: 1 when GPU-owned bytes are on its pages, which must
   then not be read (they are trapped), with the owners' identity (and the watches') mixed into *h (FNV-1a steps). A new owner changes
   it, and so does the CPU touching the bytes (which ends the ownership). 0, *h unchanged, when nothing is owned there */
int own_hash(u64 addr, u64 size, u64* h);
/* the CPU is about to touch [addr, addr + size): what the GPU holds of the pages it is on lands now, and they are the
   CPU's again. The runtime's own code calls it before it hands guest memory to the kernel (a system call fails on
   trapped pages) and on any thread that holds the producer's lock */
void own_acquire(u64 addr, u64 size);
/* [addr, addr + size)'s GPU contents don't matter any more (unmapped, or about to be overwritten whole): let go of
   without landing */
void own_drop(u64 addr, u64 size);
/* landings so far (each a wait for the GPU: the statistics count them), and whether own_init found traps */
u64 own_landings(void);
int own_traps(void);
u64 own_faults(void);
u64 own_land_bytes(void);
/* own.c's state. Other files read only nown (own_any) */
typedef struct {
    u64 lo, hi;  /* guest bytes */
    u64 pool;    /* the pool offset of lo */
    void* res;   /* the producer's copy, lo at res_off: NULL for a watch (own_watch) */
    u64 res_off;
    int landing; /* being landed by some thread: the others wait */
} Own;
/* The CPU's bytes of a trapped page, where there is no mirror to read them through: its bytes as they were when it was
   first armed with CPU bytes on it (an owner's edge page, or a watch's page). While the page is armed no CPU write can
   change them: a write lands it first. They go when it is let go. The bytes are slots of one slab, a page each, and an
   entry names its slot: a watch arms tens of pages at once, and an allocation for each page took a heap call per page
   and a page fault on Windows for each new one. A slot let go goes on the free list for the next. The slab moves when
   it grows, so its bytes are read under trap_lk only, and it goes when no shadow is left */
typedef struct {
    u64 page;
    u32 slot;
} Shadow;
/* A buffer kept for the next use: a fresh allocation of megabytes takes a page fault per page on Windows. Its users run
   one at a time under the producer's lock, but a test's may not: a second one at once gets an allocation of its own */
typedef struct {
    u8* p;
    u64 cap;
    int busy; /* atomic */
} Scratch;
typedef struct {
    OwnHooks hooks;
    int traps;  /* own_init found traps */
    u64 page;   /* the host's page, which traps are armed in */
    u8* mirror; /* the pool's mirror, which landings write through; NULL where they land in place */
    u64 lands; /* atomic: landings so far, for the statistics */
    char own_pad_lands[64];
    u64 faults; /* atomic: the faults that landed */
    char own_pad_faults[64];
    u64 land_bytes; /* atomic: the bytes landed */
    PMutex lk;  /* the entries' lock */
    /* arming pages and the check that lets them go, one at a time: a page let go just as an entry lands on it stays
       trapped (taken before lk) */
    PMutex trap_lk;
    PCond cv; /* a landing's marks let go */
    Own* ent; /* the entries, sorted by lo, apart, under lk */
    u32 nown; /* how many: changed under lk with __atomic_store_n, and read without it (own_any) */
    u32 capown;
    /* The entries on each 4 MB region of guest memory, changed with them under lk and read without it: the GPU thread
       reads guest memory through copy_runs, which asked under the lock whether any entry was on each run it read, and
       with storage buffers kept on the GPU there almost always is one somewhere. Those lock round trips were about 2%
       of the GPU thread in one game's underground scene. A count of 0 answers for the region without the lock; a
       reader racing an entry being made finds it or not, as one taking the lock first or second would. NULL: the lock
       always */
    u32* region;
    u64 nregion;
    Shadow* shadow; /* sorted by page, under trap_lk */
    u32 nshadow, capshadow;
    u8* slab; /* nslot slots of page bytes */
    u32 nslot;
    u32 slot_top;   /* slots [0, slot_top) were handed out at least once */
    u32* slot_free; /* the free ones of those, nslot of room */
    u32 nfree;
    Scratch stage;       /* a landing's staging copy, in place */
    Scratch snap;        /* the pages own_watch and own_begin copy into shadows */
    Scratch held;        /* own_begin's and own_watch's: the pages an entry was on before theirs (arm_new) */
    int said_unshadowed; /* atomic: a trapped page with no shadow was read as zero, and the log said so once */
} OwnState;
extern OwnState g_own;
/* whether anything is owned: the common case's test, without a lock */
static inline int own_any(void) { return __atomic_load_n(&g_own.nown, __ATOMIC_ACQUIRE) != 0; }

#endif
