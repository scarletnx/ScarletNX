/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* nvdrv: nvmap, nvhost-as-gpu, nvhost-ctrl, nvhost-ctrl-gpu, nvhost-gpu, and nvhost-nvdec and nvhost-vic (host1x
   channels, gpu/video/host1x.c). Enough for NVN and deko3d. */
#include "nx/hle/services/nvdrv.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/kernel/ipc.h"
#include "hle/services/nvdrv.swipc.inc"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/video/host1x.h"
#include "nx/runtime/clock.h"
#include <stdlib.h>
#include <string.h>
static void nv_events_check(void);

/* ============================================================ nvmap */
typedef struct {
    u32 size, align, heap, flags;
    u8 kind;
    u64 addr;
    u32 id;
    int alive;
    int allocated;
    u32 refs; /* CREATE and each FROM_ID take one, FREE drops one: the handle dies with the last */
} NvMapH;
/* what MAP_BUFFER_EX mapped, for UNMAP_BUFFER: [gva, gva+size) maps cpu. base is the address UNMAP_BUFFER names; a
   modify (flag 0x100) or FREE_SPACE splits a mapping into pieces that keep it. va is the VA the mapping took from
   the allocator (0 when the game placed it), given back when it is unmapped */
typedef struct {
    u64 gva, size, cpu;
    u64 base, va;
} GMap;
typedef struct {
    u64 a, e;
} VaRange;
/* nvhost-ctrl events = async syncpoint waits */
typedef struct {
    KEvent* ev;
    u32 id, thresh;
    volatile int armed;
} NvEvent;
typedef struct {
    int dev;
    u32 syncpt;
    u64 user_data;
    /* QueryEvent's events of a channel (1 SM exception, 2 SM pause, 3 error notifier, as Ryujinx numbers them), made
       on first query and never signaled: nothing here raises them */
    KEvent* ev[4];
    H1Ch* h1x; /* DEV_NVDEC, DEV_VIC: the fd's host1x channel (host1x_open) */
    /* the ioctls running on the fd. Close during one only marks it closed, and the last ioctl to leave releases it:
       the entry and its channel stay while an ioctl uses them, and Open doesn't hand the fd out again meanwhile */
    u32 users;
    int closed;
} NvFile;
#define SYNCPT_N 128 /* the syncpoint ids syncpt_alloc hands out (see there) */
/* nv_init sets the two fields that don't start at 0: an initializer would put the whole table in the binary */
static struct {
    NvMapH nvh[8192];
    u32 nvh_next; /* handles below this have been handed out */
    /* freed handles, handed out again before nvh_next grows: movies create and free buffers for every clip, and a
       handle past the table would be written outside it */
    u32 nvh_free[8192];
    u32 nvh_nfree;
    PMutex lock;
    u64** gmmu;    /* gmmu[i] -> 4096 entries of (guest addr | 1) */
    PMutex aslock; /* held across ioctl_as: the mappings below and the VA allocator */
    GMap* gmaps;
    u32 ngmaps, capgmaps;
    u32 big_page_bits; /* 64K unless asked otherwise */
    VaRange* vafree;
    u32 nvafree, capvafree;
    int va_ready; /* the free ranges hold the whole region at first (va_init) */
    u32 syncpt[SYNCPT_IDS];
    u32 syncpt_max[SYNCPT_IDS];
    PCond sync_cv;
    PMutex sync_lk;
    u8 syncpt_used[SYNCPT_N];
    u8 syncpt_gpu[SYNCPT_N]; /* a GPU channel's: submissions promise its increments (syncpt_max), the GPU thread makes
                                them (sync_lk) */
    PMutex submit_lk;        /* a submission's promise and its place in the GPU queue, in the same order */
    /* each ctrl fd's 64 event slots, made on first use and dropped with the fd, under nvev_lk: as on the console, the
       slots are the fd's. The game's GPU fence waits (one fd) and its movie player (another) both use slot 0, and
       with one table between them a thread resetting its event took the other's wake, which then waited for good */
    NvEvent* ctrl_ev[256];
    /* arming (game thread) races with nv_events_check (GPU/video threads after an incr).
       clear the event BEFORE arming and keep both under one lock, otherwise an incr in between
       signals and the clear wipes it -> lost wakeup, game waits forever. A mutex, not a spin: kevent_signal runs under
       it */
    PMutex nvev_lk;
    NvFile nvf[256];
} nv;
/* host1x (nvmap_get) and vi (nvmap_addr_by_id) read handles on the game's other threads while they create, alloc
   and free them, so under the lock, the address and the size in one hold: two would let them describe two handles */
int nvmap_get(u32 h, u64* addr, u32* size) {
    pm_lock(&nv.lock);
    int alive = h && h < 8192 && nv.nvh[h].alive && nv.nvh[h].allocated;
    *addr = alive ? nv.nvh[h].addr : 0;
    *size = alive ? nv.nvh[h].size : 0;
    pm_unlock(&nv.lock);
    return alive;
}
/* the live handle an id names, 0 for none. Called with the lock held */
static u32 nvmap_from_id(u32 id) {
    for (u32 i = 1; i < nv.nvh_next; i++)
        if (nv.nvh[i].alive && nv.nvh[i].id == id) return i;
    return 0;
}
u64 nvmap_addr_by_id(u32 id) {
    pm_lock(&nv.lock);
    u32 h = nvmap_from_id(id);
    u64 a = h && nv.nvh[h].allocated ? nv.nvh[h].addr : 0;
    pm_unlock(&nv.lock);
    return a;
}

/* ============================================================ GPU VA space (40 bit) */
/* two level table, gva -> guest address, 4K granularity. The GPU thread reads it while ioctl_as
   changes it on others: an L2 table is installed with a CAS and loaded with acquire, entries are relaxed atomics */
#define GPAGE_BITS 12
#define GL1        (1u << 16) /* 40-12 = 28 bits, split 16 + 12 */
u64 gmmu_translate(u64 gva) {
    u64 p = gva >> GPAGE_BITS;
    u32 i = (u32)(p >> 12) & (GL1 - 1), j = (u32)p & 0xFFF;
    u64* t = __atomic_load_n(&nv.gmmu[i], __ATOMIC_ACQUIRE);
    if (!t) return 0;
    u64 e = __atomic_load_n(&t[j], __ATOMIC_RELAXED);
    if (!(e & 1)) return 0;
    return (e & ~1ull) + (gva & 0xFFF);
}
static u64* gmmu_l2(u32 i) {
    u64* t = __atomic_load_n(&nv.gmmu[i], __ATOMIC_ACQUIRE);
    if (t) return t;
    u64* n = calloc(4096, 8);
    if (!n) fatal("nvdrv: out of memory for the GPU page table\n");
    if (__atomic_compare_exchange_n(&nv.gmmu[i], &t, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return n;
    free(n);
    return t;
}
static void gmmu_map(u64 gva, u64 cpu, u64 size) {
    if (size && g_gpu_mapping_begin) g_gpu_mapping_begin(gva, size, 0);
    for (u64 o = 0; o < size; o += 0x1000) {
        u64 p = (gva + o) >> GPAGE_BITS;
        u32 i = (u32)(p >> 12) & (GL1 - 1), j = (u32)p & 0xFFF;
        __atomic_store_n(&gmmu_l2(i)[j], (cpu + o) | 1, __ATOMIC_RELAXED);
    }
    if (size) gpu_mem_written(gva, size); /* an existing buffer cache may describe the old mapping */
    if (size && g_gpu_mapping_end) g_gpu_mapping_end();
}
/* clears the pages of [gva, gva+size). With own set, only the pages still pointing where a mapping of cpu put them:
   unmapping a buffer leaves alone what was mapped over it since */
static void gmmu_unmap(u64 gva, u64 size, int own, u64 cpu) {
    if (size && g_gpu_mapping_begin) g_gpu_mapping_begin(gva, size, 0);
    for (u64 o = 0; o < size; o += 0x1000) {
        u64 p = (gva + o) >> GPAGE_BITS;
        u32 i = (u32)(p >> 12) & (GL1 - 1), j = (u32)p & 0xFFF;
        u64* t = __atomic_load_n(&nv.gmmu[i], __ATOMIC_ACQUIRE);
        if (!t) continue;
        if (!own || __atomic_load_n(&t[j], __ATOMIC_RELAXED) == ((cpu + o) | 1))
            __atomic_store_n(&t[j], 0, __ATOMIC_RELAXED);
    }
    if (size) gpu_mem_written(gva, size);
    if (size && g_gpu_mapping_end) g_gpu_mapping_end();
}
static GMap* gmap_add(void) {
    if (nv.ngmaps == nv.capgmaps) {
        u32 nc = nv.capgmaps ? nv.capgmaps * 2 : 1024;
        GMap* g = realloc(nv.gmaps, nc * sizeof *g);
        if (!g) fatal("nvdrv: out of memory for GPU mappings\n");
        nv.gmaps = g;
        nv.capgmaps = nc;
    }
    return &nv.gmaps[nv.ngmaps++];
}
/* takes [gva, gva+size) out of the tracked pieces, splitting a piece that straddles it */
static void gmap_cut(u64 gva, u64 size) {
    u64 e = gva + size;
    for (u32 k = 0; k < nv.ngmaps;) {
        GMap* g = &nv.gmaps[k];
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
            nv.gmaps[k] = nv.gmaps[--nv.ngmaps];
    }
}
/* GPU VA for ALLOC_SPACE and MAP_BUFFER_EX: first fit from 0x04_0000_0000 to the end of the 40-bit space the gmmu
   covers, in 64K units. Not bounded to the big page region GET_VA_REGIONS reports: Catherine maps more than its
   0x1BFFF big pages (its NVN init maps 0x20000000 bytes past it) and checks nothing against the regions. Free ranges
   are kept sorted and merged */
#define VA_BASE 0x0400000000ull
#define VA_END  (1ull << 40)
static void va_insert(u32 k, u64 a, u64 e) {
    if (nv.nvafree == nv.capvafree) {
        u32 nc = nv.capvafree ? nv.capvafree * 2 : 64;
        VaRange* v = realloc(nv.vafree, nc * sizeof *v);
        if (!v) fatal("nvdrv: out of memory for the GPU address space\n");
        nv.vafree = v;
        nv.capvafree = nc;
    }
    memmove(&nv.vafree[k + 1], &nv.vafree[k], (nv.nvafree - k) * sizeof *nv.vafree);
    nv.vafree[k].a = a;
    nv.vafree[k].e = e;
    nv.nvafree++;
}
static void va_remove(u32 k) {
    memmove(&nv.vafree[k], &nv.vafree[k + 1], (nv.nvafree - k - 1) * sizeof *nv.vafree);
    nv.nvafree--;
}
static void va_init(void) {
    if (nv.va_ready) return;
    nv.va_ready = 1;
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
    for (u32 k = 0; k < nv.nvafree;) {
        VaRange r = nv.vafree[k];
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
    while (k < nv.nvafree && nv.vafree[k].a < a) k++;
    if (k > 0 && nv.vafree[k - 1].e == a) {
        a = nv.vafree[k - 1].a;
        va_remove(--k);
    }
    if (k < nv.nvafree && nv.vafree[k].a == e) {
        e = nv.vafree[k].e;
        va_remove(k);
    }
    va_insert(k, a, e);
}
/* 0 when the region has no room */
static u64 va_alloc(u64 size, u64 align) {
    va_init();
    if (align < 0x10000) align = 0x10000;
    for (u32 k = 0; k < nv.nvafree; k++) {
        u64 a = (nv.vafree[k].a + align - 1) & ~(align - 1);
        if (a < nv.vafree[k].a || a >= nv.vafree[k].e || va_round(size) > nv.vafree[k].e - a) continue;
        va_reserve(a, size);
        return a;
    }
    WARN("nvdrv: no room left in the GPU address space for %llx bytes\n", (unsigned long long)size);
    return 0;
}

/* ============================================================ syncpoints */
u32 syncpt_read(u32 id) { return id < SYNCPT_IDS ? __atomic_load_n(&nv.syncpt[id], __ATOMIC_SEQ_CST) : 0; }
void syncpt_add(u32 id, u32 n) {
    if (id >= SYNCPT_IDS || !n) return;
    pm_lock(&nv.sync_lk);
    u32 v = __atomic_add_fetch(&nv.syncpt[id], n, __ATOMIC_SEQ_CST);
    if ((i32)(v - nv.syncpt_max[id]) > 0) nv.syncpt_max[id] = v; /* host1x and the CPU promise nothing first */
    pc_broadcast(&nv.sync_cv);
    pm_unlock(&nv.sync_lk);
    nv_events_check();
    gpu_kick(); /* a GPU channel's stream may wait for it */
}
void syncpt_incr(u32 id) { syncpt_add(id, 1); }
int syncpt_wait(u32 id, u32 thresh, i64 timeout) {
    if (id >= SYNCPT_IDS) return 0;
    pm_lock(&nv.sync_lk);
    u64 end = plat_ns() + (u64)(timeout < 0 ? 0 : timeout);
    while ((i32)(syncpt_read(id) - thresh) < 0) {
        if (timeout == 0) {
            pm_unlock(&nv.sync_lk);
            return 0;
        }
        i64 rem = -1;
        if (timeout > 0) {
            u64 now = plat_ns();
            if (now >= end) {
                pm_unlock(&nv.sync_lk);
                return 0;
            }
            rem = (i64)(end - now);
        }
        pc_wait(&nv.sync_cv, &nv.sync_lk, rem);
    }
    pm_unlock(&nv.sync_lk);
    return 1;
}
/* every syncpoint user (GPU channels here, host1x channels in host1x.c) allocates here, so ids stay unique and low.
   NVIDIA's stream library sizes its per-syncpoint tables at 128 (NvRmChannelNumSyncPoints is a stub that fails), and an
   id past them writes into the game's heap: running out is fatal, and a closed channel (GPU or host1x) gives its ids
   back */
u32 syncpt_alloc(void) {
    pm_lock(&nv.sync_lk);
    for (u32 id = 20; id < SYNCPT_N; id++)
        if (!nv.syncpt_used[id]) {
            nv.syncpt_used[id] = 1;
            nv.syncpt_max[id] = nv.syncpt[id]; /* an id used before starts with nothing promised past its value */
            pm_unlock(&nv.sync_lk);
            return id;
        }
    pm_unlock(&nv.sync_lk);
    fatal("nvdrv: all %u syncpoints are in use (NVIDIA's stream library has room for %u ids)\n", SYNCPT_N - 20,
          SYNCPT_N);
}
void syncpt_free(u32 id) {
    if (id >= SYNCPT_N) return;
    pm_lock(&nv.sync_lk);
    nv.syncpt_used[id] = 0;
    nv.syncpt_gpu[id] = 0;
    pm_unlock(&nv.sync_lk);
}
/* whether a present can leave a fence to the GPU thread: a GPU channel's value that submissions already promised is
   reached by that channel's jobs queued ahead of the present (a promise and its job are queued in one hold of
   submit_lk, which the caller takes after this). Any other fence (a host1x engine's, one the CPU increments, or one
   nothing promised yet) is waited for here, on the calling thread: a call on the GPU thread can't stand aside to wait
   as a stream can (gpu_park) */
static int fence_on_gpu_thread(u32 id, u32 val) {
    if (id == ~0u) return 0;
    pm_lock(&nv.sync_lk);
    int gpu = id < SYNCPT_N && nv.syncpt_gpu[id] && (i32)(val - nv.syncpt_max[id]) <= 0;
    pm_unlock(&nv.sync_lk);
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
    pm_lock(&nv.submit_lk); /* after the submissions promising the fences left to the GPU thread */
    gpu_call(fn, arg, chans, nchans);
    pm_unlock(&nv.submit_lk);
}
/* a GPU channel's syncpoint, allocated on first use. Two ioctls on the fd at once may both allocate: the one that
   stores it first under nv.lock wins, the other gives its id back (there are 108 to hand out) */
static u32 gpu_channel_syncpt(NvFile* f) {
    pm_lock(&nv.lock);
    u32 sp = f->syncpt;
    pm_unlock(&nv.lock);
    if (sp) return sp;
    u32 mine = syncpt_alloc();
    pm_lock(&nv.sync_lk); /* a GPU channel's before any ioctl can submit on it */
    nv.syncpt_gpu[mine] = 1;
    pm_unlock(&nv.sync_lk);
    pm_lock(&nv.lock);
    if (!f->syncpt) f->syncpt = mine;
    sp = f->syncpt;
    pm_unlock(&nv.lock);
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

static void nvev_lock(void) { pm_lock(&nv.nvev_lk); }
static void nvev_unlock(void) { pm_unlock(&nv.nvev_lk); }
/* ctrl fd fd's event slot, its table made if make is set (NULL without memory, or when it isn't made). Called with the
   lock held, for an fd that stays open meanwhile */
static NvEvent* nvev_slot(u32 fd, u32 slot, int make) {
    if (!nv.ctrl_ev[fd] && make) nv.ctrl_ev[fd] = calloc(64, sizeof(NvEvent));
    return nv.ctrl_ev[fd] ? &nv.ctrl_ev[fd][slot & 63] : NULL;
}
/* the KEvent of an event slot, made on first use. nvdrv keeps a reference of its own, so the game closing its
   handle doesn't free an event a later syncpoint increment signals. Called with the lock held */
static KEvent* nvev_get(u32 fd, u32 slot) {
    NvEvent* e = nvev_slot(fd, slot, 1);
    if (!e) return NULL;
    if (!e->ev) {
        e->ev = kevent_new("nvhost event");
        kobj_ref(&e->ev->o);
    }
    return e->ev;
}
/* the fd's slots, taken from the table when its entry is cleared (with nv.lock held, so a new fd with the number
   doesn't meet them); nv_release drops them */
static NvEvent* nvev_detach(u32 fd) {
    nvev_lock();
    NvEvent* t = nv.ctrl_ev[fd];
    nv.ctrl_ev[fd] = NULL;
    nvev_unlock();
    return t;
}
static void nv_events_check(void) {
    nvev_lock();
    for (int fd = 0; fd < 256; fd++)
        for (int i = 0; nv.ctrl_ev[fd] && i < 64; i++) {
            NvEvent* e = &nv.ctrl_ev[fd][i];
            if (e->armed && (i32)(syncpt_read(e->id) - e->thresh) >= 0) {
                e->armed = 0;
                kevent_signal(e->ev);
            }
        }
    nvev_unlock();
}

/* ============================================================ files (fds) */
enum { DEV_NONE, DEV_NVMAP, DEV_AS, DEV_CTRL, DEV_CTRLGPU, DEV_GPU, DEV_NVDEC, DEV_VIC, DEV_OTHER };

static u32 ioctl_nvmap(u32 cmd, u8* d, u32 n) {
    (void)n;
    switch (cmd & 0xFF) {
        case 0x01: {
            u32 size;
            memcpy(&size, d, 4);
            pm_lock(&nv.lock);
            u32 h = nv.nvh_nfree ? nv.nvh_free[--nv.nvh_nfree] : nv.nvh_next < 8192 ? nv.nvh_next++ : 0;
            if (h) {
                memset(&nv.nvh[h], 0, sizeof nv.nvh[h]);
                nv.nvh[h].size = size;
                nv.nvh[h].id = h;
                nv.nvh[h].refs = 1;
                nv.nvh[h].alive = 1;
            }
            pm_unlock(&nv.lock);
            memcpy(d + 4, &h, 4);
            if (!h) {
                WARN("nvmap: all 8191 handles are in use\n");
                return 6; /* InsufficientMemory */
            }
            return 0;
        }
        case 0x03: { /* FROM_ID: another reference to the same handle */
            u32 id;
            memcpy(&id, d, 4);
            pm_lock(&nv.lock);
            u32 h = nvmap_from_id(id);
            if (h) nv.nvh[h].refs++;
            pm_unlock(&nv.lock);
            if (!h) return 1;
            memcpy(d + 4, &h, 4);
            return 0;
        }
        case 0x04: { /* ALLOC: the handle's memory. Under the lock: vi and host1x read the address from their threads */
            u32 h;
            u64 addr;
            memcpy(&h, d, 4);
            memcpy(&addr, d + 24, 8);
            if (!h || h >= 8192) return 1;
            pm_lock(&nv.lock);
            NvMapH* m = &nv.nvh[h];
            int alive = m->alive;
            int valid = alive && m->size && addr <= AS_SIZE - m->size && !(addr & 0xFFF);
            if (valid) {
                memcpy(&m->heap, d + 4, 4);
                memcpy(&m->flags, d + 8, 4);
                memcpy(&m->align, d + 12, 4);
                m->kind = d[16];
                m->addr = addr;
                m->allocated = 1;
            }
            pm_unlock(&nv.lock);
            if (!valid) return alive ? 4 : 1;
            LOGV("nvmap alloc h=%u addr=%llx\n", h, (unsigned long long)addr);
            return 0;
        }
        case 0x05: {
            u32 h;
            memcpy(&h, d, 4);
            if (!h || h >= 8192) return 1; /* no handle: a bad value, as for ALLOC */
            {
                pm_lock(&nv.lock);
                int last = nv.nvh[h].alive && --nv.nvh[h].refs == 0;
                u64 a = nv.nvh[h].addr;
                u32 s = nv.nvh[h].size, f = nv.nvh[h].flags;
                if (last) nv.nvh[h].alive = 0;
                pm_unlock(&nv.lock);
                memcpy(d + 8, &a, 8);
                memcpy(d + 16, &s, 4);
                memcpy(d + 20, &f, 4);
                if (last) {
                    /* NVDEC and VIC keep a mapping per handle: drop it before the number can name another buffer */
                    host1x_forget_handle(h);
                    pm_lock(&nv.lock);
                    nv.nvh_free[nv.nvh_nfree++] = h;
                    pm_unlock(&nv.lock);
                }
            }
            return 0;
        }
        case 0x09: {
            u32 h, p, r = 0;
            memcpy(&h, d, 4);
            memcpy(&p, d + 4, 4);
            pm_lock(&nv.lock); /* ALLOC and FREE write the handle under it */
            int alive = h && h < 8192 && nv.nvh[h].alive;
            if (alive) {
                NvMapH* m = &nv.nvh[h];
                switch (p) {
                    case 1: r = m->size; break;
                    case 2: r = m->align ? m->align : 0x1000; break;
                    case 3: r = (u32)m->addr; break; /* Base: the address, as Ryujinx answers */
                    case 4: r = 0x40000000; break;
                    case 5: r = m->kind; break;
                    case 6: r = 0; break;
                }
            }
            pm_unlock(&nv.lock);
            memcpy(d + 8, &r, 4);
            return alive ? 0 : 1;
        }
        case 0x0E: {
            u32 h;
            memcpy(&h, d + 4, 4);
            pm_lock(&nv.lock);
            int alive = h && h < 8192 && nv.nvh[h].alive;
            u32 id = alive ? nv.nvh[h].id : 0;
            pm_unlock(&nv.lock);
            memcpy(d, &id, 4);
            return alive ? 0 : 1;
        }
    }
    WARN_LIMIT(16, "nvmap ioctl %x not handled\n", cmd);
    return 0;
}

static u32 ioctl_as(u32 cmd, u8* d, u32 n, u8* extra, u32 nextra) {
    (void)n;
    switch (cmd & 0xFF) {
        case 0x01: return 0;
        case 0x02: {
            u32 pages, psz, flags;
            u64 off;
            memcpy(&pages, d, 4);
            memcpy(&psz, d + 4, 4);
            memcpy(&flags, d + 8, 4);
            memcpy(&off, d + 16, 8);
            u64 size = (u64)pages * psz, a = off, align = off > psz ? off : psz;
            /* fixed (flags bit 0): the field is the address. Otherwise it is the alignment, a power of two */
            if (!gva_ok(flags & 1 ? a : 0, size) || (!(flags & 1) && (align & (align - 1)))) {
                WARN_LIMIT(4, "as alloc_space: %u pages of %x at %llx (flags %x) refused\n", pages, psz,
                           (unsigned long long)off, flags);
                return 4; /* BadParameter */
            }
            if (flags & 1) /* fixed: keep the allocator out of it */
                va_reserve(a, size);
            else if (!(a = va_alloc(size, align)))
                return 6; /* InsufficientMemory */
            memcpy(d + 16, &a, 8);
            LOGV("as alloc_space %llx size %llx\n", (unsigned long long)a, (unsigned long long)size);
            return 0;
        }
        case 0x03: {
            u64 off;
            u32 pages, psz;
            memcpy(&off, d, 8);
            memcpy(&pages, d + 8, 4);
            memcpy(&psz, d + 12, 4);
            u64 size = (u64)pages * psz;
            if (!gva_ok(off, size)) return 4;
            gmmu_unmap(off, size, 0, 0);
            gmap_cut(off, size);
            va_free(off, size);
            return 0;
        }
        case 0x05: { /* UNMAP_BUFFER: every piece of the mapping at off */
            u64 off, va = 0;
            memcpy(&off, d, 8);
            for (u32 k = 0; k < nv.ngmaps;) {
                if (nv.gmaps[k].base != off) {
                    k++;
                    continue;
                }
                gmmu_unmap(nv.gmaps[k].gva, nv.gmaps[k].size, 1, nv.gmaps[k].cpu);
                if (nv.gmaps[k].va) va = nv.gmaps[k].va;
                nv.gmaps[k] = nv.gmaps[--nv.ngmaps];
            }
            if (va) va_free(off, va);
            return 0;
        }
        case 0x06: {
            u32 flags, kind, h, psz;
            i64 boff;
            u64 msize, off;
            memcpy(&flags, d, 4);
            memcpy(&kind, d + 4, 4);
            memcpy(&h, d + 8, 4);
            memcpy(&psz, d + 12, 4);
            memcpy(&boff, d + 16, 8);
            memcpy(&msize, d + 24, 8);
            memcpy(&off, d + 32, 8);
            if ((flags & 0x100) && h == 0) return 0; /* just updates kind on an existing map */
            pm_lock(&nv.lock);                       /* ALLOC and FREE write the handle under it */
            int alive = h && h < 8192 && nv.nvh[h].alive && nv.nvh[h].allocated;
            u64 haddr = alive ? nv.nvh[h].addr : 0, hsize = alive ? nv.nvh[h].size : 0;
            pm_unlock(&nv.lock);
            if (!alive) {
                WARN_LIMIT(8, "as map: invalid nvmap handle %u flags=%x kind=%x psz=%x boff=%llx msize=%llx off=%llx\n",
                           h, flags, kind, psz, (unsigned long long)boff, (unsigned long long)msize,
                           (unsigned long long)off);
                return 1;
            }
            /* an allocation at address 0 nvservices backs with its own memory (Ryujinx's Alloc): here it maps the
               guest's lowest pages, so it is logged to see whether a game does it */
            if (!haddr) WARN_LIMIT(4, "as map: nvmap handle %u has address 0\n", h);
            u64 size = msize ? msize : hsize;
            u64 cpu = haddr + (u64)boff;
            /* PTEs expose complete 4 KiB pages, even when the requested byte count is smaller. */
            u64 span = size <= AS_SIZE ? (size + 0xFFF) & ~0xFFFull : 0;
            if (boff < 0 || !span || (cpu & 0xFFF) || !cpu_ok(cpu, span) ||
                ((flags & 0x101) && ((off & 0xFFF) || !gva_ok(off, span))) ||
                (!(flags & 0x101) && psz && (psz & (psz - 1)))) {
                WARN_LIMIT(4, "as map: handle %u offset %llx size %llx at %llx (flags %x) refused\n", h,
                           (unsigned long long)boff, (unsigned long long)size, (unsigned long long)off, flags);
                return 4;
            }
            if (flags & 0x100) { /* modify: remap inside an existing mapping, whose unmap takes these pages too */
                u64 base = off, va = 0;
                for (u32 k = 0; k < nv.ngmaps; k++)
                    if (nv.gmaps[k].gva <= off && off < nv.gmaps[k].gva + nv.gmaps[k].size) {
                        base = nv.gmaps[k].base;
                        va = nv.gmaps[k].va;
                        break;
                    }
                gmmu_map(off, cpu, size);
                gmap_cut(off, size);
                GMap* g = gmap_add();
                g->gva = off;
                g->size = size;
                g->cpu = cpu;
                g->base = base;
                g->va = va;
                return 0;
            }
            u64 gva = off, va = 0;
            if (!(flags & 1)) {
                if (!(gva = va_alloc(size, psz ? psz : 0x10000))) return 6; /* InsufficientMemory */
                va = size;
            }
            gmmu_map(gva, cpu, size);
            GMap* g = gmap_add();
            g->gva = gva;
            g->size = size;
            g->cpu = cpu;
            g->base = gva;
            g->va = va;
            memcpy(d + 32, &gva, 8);
            LOGV("as map h=%u cpu=%llx gva=%llx size=%llx\n", h, (unsigned long long)cpu, (unsigned long long)gva,
                 (unsigned long long)size);
            return 0;
        }
        case 0x08: {
            u32 bufsz = 2 * 24;
            memcpy(d + 8, &bufsz, 4);
            u8* rg = d + 16;
            u64 o0 = 0x04000000;
            u32 ps0 = 0x1000;
            u64 np0 = 0x3FFBFFF;
            u64 o1 = 0x0400000000ull;
            u32 ps1 = 1u << nv.big_page_bits;
            u64 np1 = 0x1BFFF;
            memcpy(rg, &o0, 8);
            memcpy(rg + 8, &ps0, 4);
            memset(rg + 12, 0, 4);
            memcpy(rg + 16, &np0, 8);
            memcpy(rg + 24, &o1, 8);
            memcpy(rg + 32, &ps1, 4);
            memset(rg + 36, 0, 4);
            memcpy(rg + 40, &np1, 8);
            if (extra && nextra) memcpy(extra, rg, nextra < 48 ? nextra : 48);
            return 0;
        }
        case 0x09: { /* ALLOC_AS_EX: {u32 flags, i32 as_fd, u32 big_page_size, u32 pad, u64 va_start, va_end, va_split}
                    big_page_size 0 means 128K (yuzu DEFAULT_BIG_PAGE_SIZE). REMAP uses big page units */
            u32 bps;
            memcpy(&bps, d + 8, 4);
            if (!bps) bps = 0x20000;
            if (bps == 0x10000 || bps == 0x20000) nv.big_page_bits = bps == 0x20000 ? 17 : 16;
            LOG("nvdrv: GPU address space init (big page field %x) -> %u KB big pages\n", bps,
                (1u << nv.big_page_bits) >> 10);
            return 0;
        }
        case 0x14: { /* REMAP entries: {u16 flags, u16 kind, u32 handle, u32 mem_off, u32 gpu_off, u32 pages}, offsets in big pages */
            u32 cnt = n / 20;
            for (u32 i = 0; i < cnt; i++) {
                u8* e = d + i * 20;
                u32 h, moff, goff, pages;
                memcpy(&h, e + 4, 4);
                memcpy(&moff, e + 8, 4);
                memcpy(&goff, e + 12, 4);
                memcpy(&pages, e + 16, 4);
                /* big page size comes from ALLOC_AS_EX */
                u64 gva = (u64)goff << nv.big_page_bits, size = (u64)pages << nv.big_page_bits;
                pm_lock(&nv.lock); /* ALLOC and FREE write the handle under it */
                int alive = h && h < 8192 && nv.nvh[h].alive && nv.nvh[h].allocated;
                u64 haddr = alive ? nv.nvh[h].addr : 0;
                u64 mem_off = (u64)moff << nv.big_page_bits;
                u64 cpu = haddr + mem_off;
                pm_unlock(&nv.lock);
                if (alive && !haddr) /* as for MAP_BUFFER_EX */
                    WARN_LIMIT(4, "nvdrv: remap of nvmap handle %u, which has address 0\n", h);
                /* a refused entry is skipped and the ioctl still succeeds: what the SDK does with an error from it
                   isn't known */
                if (!gva_ok(gva, size) || (h && (!alive || !cpu_ok(cpu, size)))) {
                    WARN_LIMIT(4, "nvdrv: remap of handle %u, %llx bytes at gpu %llx, refused\n", h,
                               (unsigned long long)size, (unsigned long long)gva);
                    continue;
                }
                if (!h)
                    gmmu_unmap(gva, size, 0, 0);
                else
                    gmmu_map(gva, cpu, size);
                LOG_LIMIT(4, "nvdrv: remap handle %u: %llx bytes at gpu %llx (big page %u KB)\n", h,
                          (unsigned long long)size, (unsigned long long)gva, (1u << nv.big_page_bits) >> 10);
            }
            return 0;
        }
    }
    (void)extra;
    (void)nextra;
    WARN_LIMIT(16, "as ioctl %x not handled\n", cmd);
    return 0;
}

static u32 ioctl_ctrl(u32 fd, u32 cmd, u8* d, u32 n) {
    u32 nr = cmd & 0xFF;
    if ((nr >= 0x14 && nr <= 0x1A && nr != 0x17 && nr != 0x18) || nr == 0x1D || nr == 0x1E) {
        u32 id; /* the syncpoint ioctls name it first */
        memcpy(&id, d, 4);
        if (id >= SYNCPT_IDS) {
            WARN_LIMIT(4, "ctrl ioctl %x: syncpoint %u, past the %u there are\n", cmd, id, SYNCPT_IDS);
            return 4; /* BadParameter */
        }
    }
    switch (nr) {
        case 0x14: {
            u32 id;
            memcpy(&id, d, 4);
            u32 v = syncpt_read(id);
            memcpy(d + 4, &v, 4);
            return 0;
        }
        case 0x15: {
            u32 id;
            memcpy(&id, d, 4);
            syncpt_incr(id);
            return 0;
        }
        case 0x16:
        case 0x19: {
            u32 id, th;
            i32 to;
            memcpy(&id, d, 4);
            memcpy(&th, d + 4, 4);
            memcpy(&to, d + 8, 4);
            int ok = syncpt_wait(id, th, to < 0 ? -1 : (i64)to * 1000000);
            if (n >= 16) {
                u32 v = syncpt_read(id);
                memcpy(d + 12, &v, 4);
            }
            return ok ? 0 : 5;
        }
        case 0x1A: {
            u32 id;
            memcpy(&id, d, 4);
            pm_lock(&nv.sync_lk); /* submissions raise it under the lock */
            u32 v = nv.syncpt_max[id];
            pm_unlock(&nv.sync_lk);
            memcpy(d + 4, &v, 4);
            return 0;
        }
        case 0x1B: { /* GET_CONFIG {char domain[0x41], param[0x41], value[0x101]}: no value */
            memset(d + 0x82, 0, 0x101);
            return 0;
        }
        case 0x1C: { /* CLEAR_EVENT_WAIT: cancel the pending fence wait and reset its event. */
            u32 ev;
            memcpy(&ev, d, 4);
            u32 slot = ev & 63;
            nvev_lock();
            NvEvent* e = nvev_slot(fd, slot, 0);
            if (e && e->ev) {
                e->armed = 0;
                kevent_clear(e->ev);
            }
            nvev_unlock();
            LOG_LIMIT(2, "nvdrv: event %u wait canceled by the game\n", slot);
            return 0;
        }
        /* EVENT_WAIT and EVENT_WAIT_ASYNC {u32 id, thresh; i32 timeout; u32 value}. NvRmChannelSyncPointWaitTimeout
           sends only 0x1E: value is a slot it registered with 0x1F and queried as 0x10000000 | slot, it waits on that
           event itself on 5, and cancels the wait with 0x1C when its own wait times out. A timeout of 0 only asks:
           5 without arming the slot or clearing its event, which another wait on the fd's slot may hold signaled,
           as Ryujinx's EventWait answers it */
        case 0x1D:
        case 0x1E: {
            u32 id, th;
            i32 to;
            u32 ev;
            memcpy(&id, d, 4);
            memcpy(&th, d + 4, 4);
            memcpy(&to, d + 8, 4);
            memcpy(&ev, d + 12, 4);
            if (syncpt_wait(id, th, 0)) {
                u32 v = syncpt_read(id);
                memcpy(d + 12, &v, 4);
                return 0;
            }
            if (to == 0) return 5;
            if ((cmd & 0xFF) == 0x1D) { /* synchronous: its value field names no event slot to arm */
                if (syncpt_wait(id, th, to < 0 ? -1 : (i64)to * 1000000)) {
                    u32 v = syncpt_read(id);
                    memcpy(d + 12, &v, 4);
                    return 0;
                }
                return 5; /* Timeout */
            }
            u32 slot = ev & 63;
            nvev_lock();
            NvEvent* e = nvev_slot(fd, slot, 0);
            int have = e && e->ev;
            if (have) {
                kevent_clear(e->ev);
                e->id = id;
                e->thresh = th;
                e->armed = 1;
            }
            nvev_unlock();
            if (have) nv_events_check();
            u32 r = 0x10000000u | slot | ((id & 0xFFF) << 16);
            memcpy(d + 12, &r, 4);
            return 5;
        } /* timeout, caller will wait on the event */
        case 0x1F: {
            u32 ev;
            memcpy(&ev, d, 4);
            nvev_lock();
            KEvent* k = nvev_get(fd, ev & 63);
            nvev_unlock();
            return k ? 0 : 6; /* InsufficientMemory */
        }
        case 0x20:   /* EVENT_UNREGISTER {u32 slot} */
        case 0x21: { /* EVENT_KILL {u64 slots}: a bit per slot */
            u64 mask = 0;
            if (nr == 0x20) {
                u32 ev;
                memcpy(&ev, d, 4);
                mask = 1ull << (ev & 63);
            } else
                memcpy(&mask, d, 8);
            nvev_lock();
            for (u32 slot = 0; slot < 64; slot++) {
                NvEvent* e = ((mask >> slot) & 1) ? nvev_slot(fd, slot, 0) : NULL;
                if (e && e->ev) {
                    e->armed = 0;
                    kevent_clear(e->ev);
                }
            }
            nvev_unlock();
            return 0;
        }
    }
    WARN_LIMIT(16, "ctrl ioctl %x not handled\n", cmd);
    return 0;
}

static u32 ioctl_ctrlgpu(u32 cmd, u8* d, u32 n, u8* extra, u32 nextra) {
    (void)n;
    switch (cmd & 0xFF) {
        case 0x01: {
            u32 v = 0x28000;
            memcpy(d, &v, 4);
            return 0;
        }
        case 0x02: {
            u32 z[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
            memcpy(d, z, 40);
            return 0;
        }
        case 0x03:
        case 0x04: return 0;
        case 0x05: {
            u32 w[0xA0 / 4];
            u8* ch = (u8*)w;
            memset(w, 0, sizeof w);
            w[0] = 0x120;
            w[1] = 0xB;
            w[2] = 0xA1;
            w[3] = 1;
            u64 l2 = 0x40000;
            memcpy(ch + 0x10, &l2, 8);
            w[8] = 2;
            w[9] = 0x20;
            w[10] = 0x20000;
            w[11] = 0x20000;
            w[12] = 0x1B;
            w[13] = 0x30000;
            w[14] = 1;
            w[15] = 0x503;
            w[16] = 0x503;
            w[17] = 0x80;
            w[18] = 0x28;
            u64 fl = 0x55;
            memcpy(ch + 0x50, &fl, 8);
            w[22] = 0x902D;
            w[23] = 0xB197;
            w[24] = 0xB1C0;
            w[25] = 0xB06F;
            w[26] = 0xA140;
            w[27] = 0xB0B5;
            w[28] = 1;
            w[29] = 0;
            w[30] = 2;
            w[31] = 1;
            w[32] = 0;
            w[33] = 1;
            w[34] = 0x21D70;
            w[35] = 0;
            u64 cn = 0x6230326D67ull;
            memcpy(ch + 0x90, &cn, 8);
            u64 sz = 0xA0;
            memcpy(d, &sz, 8); /* d + 8, the game's buffer address, goes back as it came */
            memcpy(d + 16, ch, 0xA0);
            if (extra && nextra)
                memcpy(extra, ch, nextra < 0xA0 ? nextra : 0xA0); /* Ioctl3 passes data in the extra buffer */
            return 0;
        }
        case 0x06: {
            u32 m = 3;
            memcpy(d + 16, &m, 4);
            if (extra && nextra >= 4) memcpy(extra, &m, 4);
            return 0;
        }
        case 0x13: { /* NUM_VSMS: GM20B = 2 SMs (1 GPC x 2 TPC). Ioctl3 variant also returns {gpc,tpc} per SM */
            u32 z[2] = {2, 0};
            memcpy(d, z, 8);
            if (extra && nextra) {
                u8 mp[4] = {0, 0, 0, 1};
                memcpy(extra, mp, nextra < 4 ? nextra : 4);
            }
            return 0;
        }
        case 0x14:
            if ((cmd >> 30) == 3) { /* VSMS_MAPPING (0xC0044714): the 4 bytes in place are {gpc,tpc} per SM */
                u8 mp[4] = {0, 0, 0, 1};
                memcpy(d, mp, 4);
                if (extra && nextra) memcpy(extra, mp, nextra < 4 ? nextra : 4);
                return 0;
            }
            {
                u32 z[2] = {7, 1};
                memcpy(d, z, 8);
                return 0;
            } /* GET_ACTIVE_SLOT_MASK */
        case 0x15: {
            u32 z[2] = {7, 1};
            memcpy(d, z, 8);
            return 0;
        }
        case 0x1C: {
            u64 t = guest_ns();
            memcpy(d, &t, 8);
            return 0;
        } /* GET_GPU_TIME, same timebase as the system tick */
    }
    WARN_LIMIT(16, "ctrl-gpu ioctl %x not handled\n", cmd);
    return 0;
}

static u32 ioctl_gpu(NvFile* f, u32 cmd, u8* d, u32 n, u8* extra, u32 nextra) {
    switch (cmd & 0xFF) {
        case 0x01:
        case 0x03:
        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0x1D:
        case 0x0A:
        case 0x17: return 0;
        case 0x14: memcpy(&f->user_data, d, 8); return 0;
        case 0x15: memcpy(d, &f->user_data, 8); return 0;
        case 0x16: memset(d, 0, n); return 0;
        case 0x09: { /* ALLOC_OBJ_CTX: the object's id is its class */
            u32 cls;
            memcpy(&cls, d, 4);
            u64 id = cls;
            memcpy(d + 8, &id, 8);
            return 0;
        }
        case 0x1A: { /* ALLOC_GPFIFO_EX2 {u32 num_entries, num_jobs, flags, NvFence fence, u32 reserved[3]} */
            u32 sp = gpu_channel_syncpt(f);
            u32 fence[2] = {sp, syncpt_read(sp)};
            memcpy(d + 12, fence, 8);
            return 0;
        }
        case 0x08:
        case 0x1B: {
            u32 num, flags;
            memcpy(&num, d + 8, 4);
            memcpy(&flags, d + 12, 4);
            u32 fid, fval;
            memcpy(&fid, d + 16, 4);
            memcpy(&fval, d + 20, 4);
            const u8* entries; /* u64 entries at any alignment: gpu_submit copies them */
            u32 room;
            if ((cmd & 0xFF) == 0x08) { /* the entries follow the header in the ioctl buffer */
                entries = d + 24;
                room = n < 24 ? 0 : (n - 24) / 8;
            } else {
                entries = extra;
                room = nextra / 8;
            }
            if (num > room) {
                WARN_LIMIT(4,
                           "gpu: a submission of %u GPFIFO entries has room for %u in its buffer, the rest dropped\n",
                           num, room);
                num = room;
            }
            u32 sp = gpu_channel_syncpt(f);
            /* flags bit 0: wait for the fence passed in before running these. It is the stream's first wait (gpu_park),
               as nvgpu puts a syncpoint wait ahead of the entries and returns at once, whoever increments it */
            u32 wait_id = (flags & 1) && fid != ~0u ? fid : ~0u;
            /* the fence promises every increment of this submission. With IncrementWithValue (0x100) the stream
               increments the syncpoint fval times itself; the host adds 2 after it only for FenceIncrement (2), as
               Ryujinx's SubmitGpfifo does */
            u32 host = (flags & 2) ? 2 : 0;
            u32 incs = host + ((flags & 0x100) ? fval : 0);
            pm_lock(&nv.submit_lk);
            pm_lock(&nv.sync_lk);
            if ((i32)(nv.syncpt_max[sp] - nv.syncpt[sp]) < 0) nv.syncpt_max[sp] = nv.syncpt[sp];
            nv.syncpt_max[sp] += incs;
            u32 promised = nv.syncpt_max[sp];
            pm_unlock(&nv.sync_lk);
            gpu_submit(entries, num, sp, host, wait_id, fval, promised);
            pm_unlock(&nv.submit_lk);
            fid = sp;
            fval = promised;
            memcpy(d + 16, &fid, 4);
            memcpy(d + 20, &fval, 4);
            return 0;
        }
    }
    WARN_LIMIT(16, "gpu channel ioctl %x not handled\n", cmd);
    return 0;
}

static int nns_nvdrv_INvDrvServices_Open(SvcObj* self, IpcBuf path, u32* fd, u32* error) {
    char name[64] = {0};
    if (path.addr) memcpy(name, GP(path.addr), path.size < 63 ? path.size : 63);
    int dev = !strcmp(name, "/dev/nvmap")             ? DEV_NVMAP
              : !strcmp(name, "/dev/nvhost-as-gpu")   ? DEV_AS
              : !strcmp(name, "/dev/nvhost-ctrl")     ? DEV_CTRL
              : !strcmp(name, "/dev/nvhost-ctrl-gpu") ? DEV_CTRLGPU
              : !strcmp(name, "/dev/nvhost-gpu")      ? DEV_GPU
              : !strcmp(name, "/dev/nvhost-nvdec")    ? DEV_NVDEC
              : !strcmp(name, "/dev/nvhost-vic")      ? DEV_VIC
                                                      : DEV_OTHER;
    /* an NVDEC or VIC fd is a host1x channel of its own, with its registers, syncpoints and decoder */
    int h1x_dev = dev == DEV_NVDEC || dev == DEV_VIC;
    H1Ch* h1x = h1x_dev ? host1x_open(dev == DEV_VIC) : NULL;
    int slot = -1;
    pm_lock(&nv.lock);
    for (int i = 1; i < 256 && (h1x || !h1x_dev); i++)
        if (!nv.nvf[i].dev) {
            slot = i;
            memset(&nv.nvf[i], 0, sizeof nv.nvf[i]);
            nv.nvf[i].dev = dev;
            nv.nvf[i].h1x = h1x;
            break;
        }
    pm_unlock(&nv.lock);
    if (slot < 0) host1x_close(h1x);
    LOG("nvdrv: open %s -> %d\n", name, slot);
    *fd = (u32)slot;
    *error = slot < 0 ? 0x30003u : 0; /* FileOperationFailed when every fd is taken (or no memory) */
    return 0;
}
/* what goes with a closed fd: the channel and its syncpoint once the GPU thread has run the jobs queued on it (freed
   now, the id could go to a new channel those jobs would then increment), an NVDEC or VIC channel with its decoder
   and syncpoints, and the fd's references to its events */
static void nv_release(NvFile* old, NvEvent* slots) {
    if (old->dev == DEV_GPU && old->syncpt)
        gpu_call(gpu_channel_retire, (void*)(uintptr_t)old->syncpt, &old->syncpt, 1);
    host1x_close(old->h1x);
    for (int k = 0; k < 4; k++)
        if (old->ev[k]) kobj_unref(&old->ev[k]->o);
    for (int k = 0; slots && k < 64; k++) /* a ctrl fd's events: the game's handles keep them as long as they need */
        if (slots[k].ev) kobj_unref(&slots[k].ev->o);
    free(slots);
}
/* Ioctl, Ioctl2 (extra is a second input) and Ioctl3 (extra is a second output): the ioctl's in and out buffers go
   through tmp, the extra one straight to the device. A GPU channel takes Ioctl2's and Ioctl3's, as-gpu and ctrl-gpu
   only Ioctl3's. ipc is the command, for the log */
static u32 nv_ioctl(u32 fd, u32 cmd, IpcBuf in, IpcBuf out, IpcBuf extra, int extra_out, u32 ipc) {
    u64 ea = extra.addr, es = extra.size;
    u32 size = (cmd >> 16) & 0x3FFF;
    u8 tmp[0x4000];
    memset(tmp, 0, sizeof tmp);
    u64 isz = in.size, osz = out.size;
    if (in.addr) memcpy(tmp, GP(in.addr), isz > sizeof tmp ? sizeof tmp : isz);
    /* nb is what the command's size field (or the larger buffer) says, n what the device may touch: both stay inside
       tmp */
    u64 nb64 = size ? size : (isz > osz ? isz : osz);
    u32 nb = (u32)(nb64 > sizeof tmp ? sizeof tmp : nb64);
    u32 n = (u32)(isz > nb ? (isz > sizeof tmp ? sizeof tmp : isz) : nb);
    pm_lock(&nv.lock);
    if (fd >= 256 || !nv.nvf[fd].dev || nv.nvf[fd].closed) {
        pm_unlock(&nv.lock);
        return 8;
    }
    NvFile* f = &nv.nvf[fd];
    f->users++;
    pm_unlock(&nv.lock);
    u32 r = 0;
    { /* each device's ioctl logged once: game threads issue them at once */
        static struct {
            u32 cmd;
            int dev;
        } seen[256];
        static int nseen;
        static PMutex seen_lk = PMUTEX_INIT;
        int k, first = 0;
        pm_lock(&seen_lk);
        for (k = 0; k < nseen; k++)
            if (seen[k].cmd == cmd && seen[k].dev == f->dev) break;
        if (k == nseen && nseen < 256) {
            seen[nseen].cmd = cmd;
            seen[nseen++].dev = f->dev;
            first = 1;
        }
        pm_unlock(&seen_lk);
        if (first)
            LOG("nvdrv: ioctl %08x (disp %d, ipc %u, in %llu out %llu extra %llu)\n", cmd, f->dev, ipc,
                (unsigned long long)isz, (unsigned long long)osz, (unsigned long long)es);
    }
    switch (f->dev) {
        case DEV_NVMAP: r = ioctl_nvmap(cmd, tmp, nb); break;
        case DEV_AS:
            pm_lock(&nv.aslock);
            r = ioctl_as(cmd, tmp, n, (extra_out && ea) ? GP(ea) : NULL, (u32)es);
            pm_unlock(&nv.aslock);
            break;
        case DEV_CTRL: r = ioctl_ctrl(fd, cmd, tmp, nb); break;
        case DEV_CTRLGPU: r = ioctl_ctrlgpu(cmd, tmp, nb, (extra_out && ea) ? GP(ea) : NULL, (u32)es); break;
        case DEV_GPU: r = ioctl_gpu(f, cmd, tmp, n, ea ? GP(ea) : NULL, (u32)es); break;
        case DEV_NVDEC:
        case DEV_VIC: r = host1x_ioctl(f->h1x, cmd, tmp, n); break;
        default: WARN_LIMIT(16, "nvdrv: ioctl %x to unknown device\n", cmd); break;
    }
    if (out.addr) memcpy(GP(out.addr), tmp, osz > sizeof tmp ? sizeof tmp : osz);
    NvFile old;
    NvEvent* slots = NULL;
    int release = 0;
    pm_lock(&nv.lock);
    if (!--f->users && f->closed) {
        old = *f;
        memset(f, 0, sizeof *f);
        slots = nvev_detach(fd);
        release = 1;
    }
    pm_unlock(&nv.lock);
    if (release) nv_release(&old, slots);
    return r;
}
static int nns_nvdrv_INvDrvServices_Ioctl(SvcObj* self, u32 fd, u32 request, IpcBuf in, IpcBuf out, u32* error) {
    IpcBuf none = {0, 0, 0};
    *error = nv_ioctl(fd, request, in, out, none, 0, 1);
    return 0;
}
static int nns_nvdrv_INvDrvServices_Ioctl2(SvcObj* self, u32 fd, u32 request, IpcBuf in, IpcBuf inline_in, IpcBuf out,
                                           u32* error) {
    *error = nv_ioctl(fd, request, in, out, inline_in, 0, 11);
    return 0;
}
static int nns_nvdrv_INvDrvServices_Ioctl3(SvcObj* self, u32 fd, u32 request, IpcBuf in, IpcBuf out, IpcBuf inline_out,
                                           u32* error) {
    *error = nv_ioctl(fd, request, in, out, inline_out, 1, 12);
    return 0;
}
static int nns_nvdrv_INvDrvServices_Close(SvcObj* self, u32 fd, u32* error) {
    NvFile old;
    NvEvent* slots = NULL;
    int release = 0, open = 0;
    pm_lock(&nv.lock);
    if (fd < 256 && nv.nvf[fd].dev && !nv.nvf[fd].closed) {
        open = 1;
        if (nv.nvf[fd].users) { /* the last ioctl releases it */
            nv.nvf[fd].closed = 1;
        } else {
            old = nv.nvf[fd];
            memset(&nv.nvf[fd], 0, sizeof nv.nvf[fd]);
            slots = nvev_detach(fd);
            release = 1;
        }
    }
    pm_unlock(&nv.lock);
    if (release) nv_release(&old, slots);
    *error = open ? 0 : 8; /* an fd that isn't open, or closed already: the NvError the ioctls answer for it */
    return 0;
}
/* the ctrl fd's ids name event slots, a channel's ids its own events */
static int nns_nvdrv_INvDrvServices_QueryEvent(SvcObj* self, u32 fd, u32 event_id, u32* error, u32* event) {
    KEvent* e = NULL;
    int held = 0;
    pm_lock(&nv.lock);
    int dev = fd < 256 && !nv.nvf[fd].closed ? nv.nvf[fd].dev : DEV_NONE;
    if (dev != DEV_NONE && dev != DEV_CTRL && event_id >= 1 && event_id <= 3) {
        if (!nv.nvf[fd].ev[event_id]) {
            nv.nvf[fd].ev[event_id] = kevent_new("nv channel event");
            kobj_ref(&nv.nvf[fd].ev[event_id]->o); /* the fd's reference, dropped on close */
        }
        e = nv.nvf[fd].ev[event_id];
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
    pm_unlock(&nv.lock);
    LOG_LIMIT(8, "nvdrv: QueryEvent fd %u (device %d) id %x\n", fd, dev, event_id);
    if (dev == DEV_NONE) { /* an fd that isn't open: the NvError the ioctls answer for it, and no handle to leak */
        *event = 0;
        *error = 8;
        return 0;
    }
    if (!e) e = kevent_new("nv query");
    *event = h_new(&e->o);
    if (held) kobj_unref(&e->o);
    *error = 0;
    return 0;
}
static SvcObj* new_nvdrv(void) { return nns_nvdrv_INvDrvServices_new(0); }

void nv_init(void) {
    nv.nvh_next = 1;
    nv.big_page_bits = 16;
    pm_init(&nv.lock);
    pm_init(&nv.aslock);
    pm_init(&nv.nvev_lk);
    pm_init(&nv.sync_lk);
    pm_init(&nv.submit_lk);
    pc_init(&nv.sync_cv);
    nv.gmmu = calloc(GL1, sizeof(u64*));
    nns_nvdrv_INvDrvServices_register(new_nvdrv);
}
