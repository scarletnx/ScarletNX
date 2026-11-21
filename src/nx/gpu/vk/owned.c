/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest memory the GPU holds: the renderer's side of memory/own.c (docs/layers/gpu/ownership.md). A resource is a
   device-local buffer holding guest bytes in their own layout; an engine that wrote it publishes it over those bytes,
   and the trap lands it when the CPU touches them: copied by the GPU into host-cached memory, waited for (that
   submission only), and copied into the pool's mirror. On a host without traps a publish lands at once. Landing runs
   on whatever thread faulted or acquired, under vk_lock, which own.c takes before it marks what lands (OwnHooks'
   lock) and own_land again for itself (it is reentrant, and a publish without traps lands with no landing of own.c's) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "common/utilities/bits.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"

#define CPU_READ_SLOTS 4096
static struct {
    int traps; /* own_init found traps: publishing owns, else it lands */
    struct {
        u64 granule;
        u32 present;
    } cpu_read[CPU_READ_SLOTS];
    /* each landing's copy signals it on one queue (land_apart's): a value is taken under vk_lock, as the copy is
       submitted, and waited for with or without it */
    VkHostTimeline done;
    _Atomic u64 serial;      /* the last VkOwnRes.serial given */
    VkOwnRes* _Atomic later; /* released while another thread held vk_lock: freed at a later vk_unlock (atomic stack) */
    /* the results with a buffer of their own that are in use, and their buffers' bytes: any thread adds and takes */
    _Atomic u64 live[VK_OWN_BY_N], live_bytes[VK_OWN_BY_N], live_dev_bytes;
    /* those let go since the last statistics, by how many presents they were kept: under 8, under 120, under 1200,
       longer */
    _Atomic u64 let_go[4];
    /* since the last statistics: the results vk_own_trim landed and their bytes, the freed buffers it let go */
    u64 trim_landed, trim_bytes, trim_pooled;
    /* since the last statistics, for a result the heap had no room for: the submissions ended for the freed buffers
       they held (vk_own_room), the pool's free buffers given back to the heap and their bytes, the freed buffers
       waited for */
    u64 room_splits, room_dropped, room_dropped_bytes, room_waits;
    u64 spilled, spilled_bytes; /* the results made in system memory for want of the device's, since then */
    /* For the statistics of landings (land_note), under vk_lock: by what made the result (VK_OWN_BY_*) and what
       asked (OWN_BY_*), the landings and their bytes. A game's landing is predictable when a game's landing of the
       same producer, start and size came in the 1200 presents before it: a copy back as the result is made would
       have had its bytes ready. The keys are a table that a newer key overwrites */
    u64 land_n[VK_OWN_BY_N][OWN_BY_N], land_bytes[VK_OWN_BY_N][OWN_BY_N];
    u64 pred_n[VK_OWN_BY_N], pred_bytes[VK_OWN_BY_N];
    struct {
        u64 key;
        u32 present;
        u32 unread; /* early copies of the key let go in a row with no landing (early_copy stops at EARLY_UNREAD) */
    } land_key[1024];
    /* early copies made (early_copy), their bytes, the landings that took one, and the ones let go unread, since the
       last report; and the bytes copied early this present (EARLY_BUDGET), under vk_lock */
    u64 early_n, early_bytes, early_hits, early_unread, early_now;
    u32 early_present;
    /* atomic: the results let go with no landing, and their bytes, by what made them: what a copy back of each
       result as it is made would have copied for nothing */
    _Atomic u64 unread[VK_OWN_BY_N], unread_bytes[VK_OWN_BY_N];
} g_owned;

/* Landings copy on the compute queue where it is a queue of its own: there the copy waits for the submission that wrote
   its resource alone (VkOwnRes.written, on the graphics timeline), not behind every submission queued after it, which
   one game's landings waited 30 to 130 ms for. On a device with one queue they copy on it, behind all of them */
static int land_apart(void) { return g_vk.compute && g_vk.compute != g_vk.gfx; }
/* a buffer both queues use: concurrent where they are of two families (an exclusive one would need its ownership
   moved at each landing) */
static void land_sharing(VkBufferCreateInfo* bi, u32 fam[2]) {
    fam[0] = g_vk.gfx_family;
    fam[1] = g_vk.compute_family;
    if (!land_apart() || fam[0] == fam[1]) return;
    bi->sharingMode = VK_SHARING_MODE_CONCURRENT;
    bi->queueFamilyIndexCount = 2;
    bi->pQueueFamilyIndices = fam;
}

/* Freed result buffers, pooled for reuse. An owned resource's buffer is made for its bytes and destroyed when the CPU
   lands them: one buffer per published storage image, about 24 a frame in an Ounce game's race, and each allocation of
   its own (the largest dedicated). Pooled, a publish takes the nearest freed buffer instead of allocating, and a landing
   returns it instead of destroying. The pool is small and low-contention: it is taken on the GPU thread (vk_own_res_new)
   and returned where the result is let go.

   A freed buffer waits for the GPU, not for its frame. What was recorded with it is in the submission being recorded
   when it is let go, or in an earlier one (cmd.c's dead_slot says the same of a release), and in the compute batch
   being recorded then: the entry keeps both numbers, and a result takes it once the GPU has done both. It used to come
   back at its frame slot's recycling, two frames on. One Ounce game's dispatches each write a range of up to 16 MiB
   into a result of its own, about eleven a present while it loads, and each replaces the one before: on a GPU of 2 GB,
   335 MB of freed buffers waited for their frames, beside 160 MB in use and 120 MB in the pool, when the heap had no
   room for the next one and the dispatches and copies after it were skipped. */
#define OWN_POOL_MAX 512
#define OWN_IDLE     120 /* presents a result or a freed buffer is left alone before vk_own_trim takes it */
#define OWN_IDLE_SHORT 8 /* presents a freed buffer is left alone while device memory is short */
#define OWN_LAND_MAX 16  /* results landed at one present */
#define OWN_LAND_SHORT 256 /* results landed at one present with device memory at the caches' trim mark */
typedef struct {
    VkBuffer buf;
    DevMem mem;
    u64 cap;   /* the buffer's bytes, at least the resource's */
    u64 after; /* the graphics submission and the compute batch that may still use it: free once both are done */
    u64 cafter;
    u32 put;   /* the present it came back at: one no result took for OWN_IDLE presents goes (vk_own_trim) */
} OwnPoolEntry;
static OwnPoolEntry g_pool[OWN_POOL_MAX];
static _Atomic u32 g_pool_n;
static PMutex g_pool_lk = PMUTEX_INIT;
/* what the GPU has done, for the pool's entries: asked of the driver, so before the pool's lock */
typedef struct {
    u64 gfx, compute;
} OwnDone;
static OwnDone own_done(void) {
    OwnDone d = {0};
    u64 submitted;
    vk_submit_progress(&submitted, &d.gfx);
    vk_compute_progress(&submitted, &d.compute);
    return d;
}
static int own_pool_free(const OwnPoolEntry* e, OwnDone d) { return e->after <= d.gfx && e->cafter <= d.compute; }
/* a let go result's buffer into the pool, 0 when the pool is full (vk_lock held: the numbers are the recording's) */
static int own_pool_put(VkBuffer buf, DevMem mem, u64 cap) {
    int put;
    pm_lock(&g_pool_lk);
    if ((put = atomic_load_explicit(&g_pool_n, memory_order_relaxed) < OWN_POOL_MAX))
        g_pool[atomic_fetch_add_explicit(&g_pool_n, 1, memory_order_relaxed)] =
            (OwnPoolEntry){.buf = buf,
                           .mem = mem,
                           .cap = cap,
                           .after = vk_submit_number(),
                           .cafter = vk_compute_number(),
                           .put = g_vk_present};
    pm_unlock(&g_pool_lk);
    return put;
}
/* the smallest pooled buffer of at least size bytes that the GPU is done with, removed: buf is VK_NULL_HANDLE when
   there is none */
static OwnPoolEntry own_pool_take(u64 size) {
    OwnPoolEntry e = {0};
    i32 best = -1;
    if (!atomic_load_explicit(&g_pool_n, memory_order_relaxed)) return e;
    OwnDone done = own_done();
    pm_lock(&g_pool_lk);
    for (u32 i = 0; i < atomic_load_explicit(&g_pool_n, memory_order_relaxed); i++)
        if (g_pool[i].cap >= size && own_pool_free(&g_pool[i], done) &&
            (best < 0 || g_pool[i].cap < g_pool[best].cap))
            best = (i32)i;
    if (best >= 0) {
        e = g_pool[best];
        g_pool[best] = g_pool[atomic_fetch_sub_explicit(&g_pool_n, 1, memory_order_relaxed) - 1];
    }
    pm_unlock(&g_pool_lk);
    return e;
}

/* Room for a result the heap has none for: its owner makes it, as devmem.h asks of every owner.

   The pool's buffers the GPU is done with go back to the heap, all of them: none holds the result, or it had taken
   one, and their memory is what the heap lacks. They are free at once (nothing uses them). Their count */
static u32 own_pool_drop(void) {
    u32 n = 0;
    if (!atomic_load_explicit(&g_pool_n, memory_order_relaxed)) return 0;
    OwnDone done = own_done();
    pm_lock(&g_pool_lk);
    for (u32 i = 0; i < atomic_load_explicit(&g_pool_n, memory_order_relaxed);) {
        if (!own_pool_free(&g_pool[i], done)) {
            i++;
            continue;
        }
        g_owned.room_dropped_bytes += g_pool[i].cap;
        vk_mem_untrack(g_pool[i].mem);
        vk_devmem_buffer_destroy(g_pool[i].buf, g_pool[i].mem);
        g_pool[i] = g_pool[atomic_fetch_sub_explicit(&g_pool_n, 1, memory_order_relaxed) - 1];
        n++;
    }
    pm_unlock(&g_pool_lk);
    g_owned.room_dropped += n;
    return n;
}
/* The pool's buffers the GPU still uses in a submission it has been given: waited for, the one the GPU reaches first
   among those of at least size bytes, else the last of them all (size 0), so that every one of them is free after.
   A wait for work the GPU has, with nothing recorded or landed, as a frame's recycling waits. One in the submission
   still being recorded is not waited for: that submission ends at a job's start (vk_own_room), not inside a job's
   setup. Nor is one a compute batch holds, which lands under a lock of its own. 1 when it waited */
static int own_pool_await(u64 size) {
    u64 submitted = 0, csubmitted = 0, wait = 0;
    OwnDone done = {0};
    if (!atomic_load_explicit(&g_pool_n, memory_order_relaxed)) return 0;
    vk_submit_progress(&submitted, &done.gfx);
    vk_compute_progress(&csubmitted, &done.compute);
    pm_lock(&g_pool_lk);
    for (u32 i = 0; i < atomic_load_explicit(&g_pool_n, memory_order_relaxed); i++) {
        const OwnPoolEntry* e = &g_pool[i];
        if (e->after <= done.gfx || e->after > submitted || e->cafter > done.compute) continue;
        if (size ? e->cap >= size && (!wait || e->after < wait) : e->after > wait) wait = e->after;
    }
    pm_unlock(&g_pool_lk);
    if (!wait) return 0;
    vk_wait_submit(wait);
    g_owned.room_waits++;
    return 1;
}

int vk_own_make_room(void) { return own_pool_drop() || (own_pool_await(0) && own_pool_drop()); }

/* At a job's start (a dispatch, a copy), before anything of it is set up: when device memory is short and the
   submission being recorded holds OWN_OPEN_MIN of freed result buffers, that submission ends here, so the GPU does it
   and the buffers come free for the job's own results. A game that loads records hundreds of megabytes of results into
   one submission, each replacing the one before, and none of their buffers could serve the next. With memory to spare
   the submission goes on: its results take new buffers */
#define OWN_OPEN_MIN (32ull << 20)
void vk_own_room(void) {
    if (!atomic_load_explicit(&g_pool_n, memory_order_relaxed) || vk_rec_unsafe()) return;
    u64 now = vk_submit_number(), open = 0, usage, budget;
    pm_lock(&g_pool_lk);
    for (u32 i = 0; i < atomic_load_explicit(&g_pool_n, memory_order_relaxed); i++)
        if (g_pool[i].after == now) open += g_pool[i].cap;
    pm_unlock(&g_pool_lk);
    if (open < OWN_OPEN_MIN) return;
    vk_mem_use(&usage, &budget);
    if (usage < budget / 5 * 4) return;
    vk_submit_split();
    if (vk_submit_number() != now) g_owned.room_splits++;
}

/* statistics of the landings since the last report (land_note, own_land_times): by producer and by what asked, the
   predictable ones, the results let go unread, and where a landing's time went */
static void lander_stats(u64* landed, u64* missed);
static void land_log(const char* const* by) {
    static const char* const ask[OWN_BY_N] = {"runtime", "read", "write"};
    static u64 bytes_was[OWN_BY_N], wait_was[OWN_BY_N], copy_was[OWN_BY_N], fill_was[OWN_BY_N], ready_was[OWN_BY_N],
        lands_was[OWN_BY_N];
    u64 bytes[OWN_BY_N], wait[OWN_BY_N], copy[OWN_BY_N], fill[OWN_BY_N], ready[OWN_BY_N], lands[OWN_BY_N];
    own_land_times(bytes, wait, copy, fill);
    own_land_ready(ready);
    own_landings_by(lands);
    static u64 begins_was, refused_was, refused_bytes_was, many_runs_was;
    u64 begins, refused, refused_bytes, many_runs;
    own_begin_stats(&begins, &refused, &refused_bytes, &many_runs);
    if (refused != refused_was || many_runs != many_runs_was)
        LOG("vulkan: ownership refused for %llu of %llu publishes (%.2f MB), %llu owned in more than 16 runs\n",
            (unsigned long long)(refused - refused_was), (unsigned long long)(begins - begins_was),
            (double)(refused_bytes - refused_bytes_was) / 1048576.0, (unsigned long long)(many_runs - many_runs_was));
    begins_was = begins, refused_was = refused, refused_bytes_was = refused_bytes, many_runs_was = many_runs;
    vk_lock();
    for (u32 k = 0; k < VK_OWN_BY_N; k++) {
        u64 un = atomic_exchange_explicit(&g_owned.unread[k], 0, memory_order_relaxed),
            unb = atomic_exchange_explicit(&g_owned.unread_bytes[k], 0, memory_order_relaxed);
        u64 any = un;
        for (u32 a = 0; a < OWN_BY_N; a++) any |= g_owned.land_n[k][a];
        if (!any) continue;
        LOG("vulkan: landings of %s: runtime %llu (%.2f MB), read %llu (%.2f MB), write %llu (%.2f MB); predictable "
            "%llu (%.2f MB); let go unread %llu (%.2f MB)\n",
            by[k], (unsigned long long)g_owned.land_n[k][0], (double)g_owned.land_bytes[k][0] / 1048576.0,
            (unsigned long long)g_owned.land_n[k][1], (double)g_owned.land_bytes[k][1] / 1048576.0,
            (unsigned long long)g_owned.land_n[k][2], (double)g_owned.land_bytes[k][2] / 1048576.0,
            (unsigned long long)g_owned.pred_n[k], (double)g_owned.pred_bytes[k] / 1048576.0, (unsigned long long)un,
            (double)unb / 1048576.0);
    }
    u64 early_landed, early_missed;
    lander_stats(&early_landed, &early_missed);
    if (g_owned.early_n || g_owned.early_hits)
        LOG("vulkan: early copies back: %llu made (%.2f MB), %llu landings took one, %llu let go unread; the lander "
            "landed %llu, %llu were no longer their result's\n",
            (unsigned long long)g_owned.early_n, (double)g_owned.early_bytes / 1048576.0,
            (unsigned long long)g_owned.early_hits, (unsigned long long)g_owned.early_unread,
            (unsigned long long)early_landed, (unsigned long long)early_missed);
    g_owned.early_n = g_owned.early_bytes = g_owned.early_hits = g_owned.early_unread = 0;
    memset(g_owned.land_n, 0, sizeof g_owned.land_n);
    memset(g_owned.land_bytes, 0, sizeof g_owned.land_bytes);
    memset(g_owned.pred_n, 0, sizeof g_owned.pred_n);
    memset(g_owned.pred_bytes, 0, sizeof g_owned.pred_bytes);
    vk_unlock();
    for (u32 a = 0; a < OWN_BY_N; a++) {
        if (bytes[a] != bytes_was[a] || wait[a] != wait_was[a])
            LOG("vulkan: landings for a %s: %llu (%llu of them with the GPU's work done), %.2f MB, %.2f ms waiting "
                "for the GPU's work, %.2f ms in the copy, %.2f ms into the trapped pages\n",
                ask[a], (unsigned long long)(lands[a] - lands_was[a]), (unsigned long long)(ready[a] - ready_was[a]),
                (double)(bytes[a] - bytes_was[a]) / 1048576.0, (double)(wait[a] - wait_was[a]) / 1e6,
                (double)(copy[a] - copy_was[a]) / 1e6, (double)(fill[a] - fill_was[a]) / 1e6);
        bytes_was[a] = bytes[a], wait_was[a] = wait[a], copy_was[a] = copy[a], fill_was[a] = fill[a];
        ready_was[a] = ready[a], lands_was[a] = lands[a];
    }
}
/* statistics: what the ledger's kept results are, the ones in use and the freed buffers that wait in the pool */
static void gread_log(void);
void vk_own_log(void) {
    gread_log();
    u64 pooled = 0, waiting = 0;
    OwnDone done = own_done();
    pm_lock(&g_pool_lk);
    u32 n = atomic_load_explicit(&g_pool_n, memory_order_relaxed);
    for (u32 i = 0; i < n; i++) {
        pooled += g_pool[i].cap;
        if (!own_pool_free(&g_pool[i], done)) waiting += g_pool[i].cap;
    }
    pm_unlock(&g_pool_lk);
    static const char* const by[VK_OWN_BY_N] = {"dispatches", "draws", "copies", "video", "render targets"};
    char line[320];
    size_t at = 0;
    u64 live = 0;
    for (u32 k = 0; k < VK_OWN_BY_N; k++) {
        u64 count = atomic_load_explicit(&g_owned.live[k], memory_order_relaxed),
            bytes = atomic_load_explicit(&g_owned.live_bytes[k], memory_order_relaxed);
        live += count;
        if (!count) continue;
        int w = snprintf(line + at, sizeof line - at, "%s %llu of %s (%.1f MB)", at ? "," : "",
                         (unsigned long long)count, by[k], (double)bytes / 1048576.0);
        if (w > 0) at += (size_t)w < sizeof line - at ? (size_t)w : sizeof line - at - 1;
    }
    u64 gone[4];
    for (u32 k = 0; k < 4; k++) gone[k] = atomic_exchange_explicit(&g_owned.let_go[k], 0, memory_order_relaxed);
    if (n || live)
        LOG("vulkan: kept results in use:%s; %u freed buffers in the pool (%.1f MB, %.1f of them the GPU still "
            "uses); let go since the last report after under 8 presents %llu, under 120 %llu, under 1200 %llu, "
            "longer %llu\n",
            at ? line : " none", n, (double)pooled / 1048576.0, (double)waiting / 1048576.0,
            (unsigned long long)gone[0],
            (unsigned long long)gone[1], (unsigned long long)gone[2], (unsigned long long)gone[3]);
    if (g_owned.trim_landed || g_owned.trim_pooled)
        LOG("vulkan: kept results left alone for %u presents: %llu landed in guest memory for the budget (%.1f MB), "
            "%llu freed buffers let go\n",
            OWN_IDLE, (unsigned long long)g_owned.trim_landed, (double)g_owned.trim_bytes / 1048576.0,
            (unsigned long long)g_owned.trim_pooled);
    g_owned.trim_landed = g_owned.trim_bytes = g_owned.trim_pooled = 0;
    if (g_owned.room_splits || g_owned.room_dropped || g_owned.room_waits || g_owned.spilled)
        LOG("vulkan: kept results short of device memory: %llu submissions ended for the freed buffers they held, "
            "%llu free buffers of the pool given back (%.1f MB), %llu freed buffers waited for, %llu results made in "
            "system memory (%.1f MB)\n",
            (unsigned long long)g_owned.room_splits, (unsigned long long)g_owned.room_dropped,
            (double)g_owned.room_dropped_bytes / 1048576.0, (unsigned long long)g_owned.room_waits,
            (unsigned long long)g_owned.spilled, (double)g_owned.spilled_bytes / 1048576.0);
    g_owned.room_splits = g_owned.room_dropped = g_owned.room_dropped_bytes = g_owned.room_waits = 0;
    g_owned.spilled = g_owned.spilled_bytes = 0;
    land_log(by);
}

/* The kept results within the budget. A result is the only copy of the guest bytes it holds, so it stays until the
   CPU touches them or something writes over them. One game's copy engines move every tile of its streamed textures,
   and nothing ever reads the tiles on the CPU: in a race 2267 such results held 1046 MB of device memory that no
   limit reached, the caches were left 5.2 of 5.5 GB, and the trim made room by evicting textures that the game was
   using, 6517 of which it then loaded again.

   So when device memory is short (four fifths of its budget in use, by the heap's count or the budget's, the larger of
   the two) or the results hold more than an eighth of the budget, the results that nothing has written or read for
   OWN_IDLE presents land in guest memory, as the CPU's touch would land them, the largest first and, of one size, the
   ones left alone longest. What they were made for has read them by then (a texture is built from its tile within a
   frame or two), and a later read finds the bytes in guest memory. A landing is a round trip to the GPU, so only a few
   land a present (OWN_LAND_MAX, and none once a call landed what is over those marks, or 64 MiB), here at the frame's
   boundary where no draw is being set up, and a landed result's buffer goes back to the heap, not to the pool. With
   memory to spare and the results under that eighth nothing lands. The freed buffers of the pool are no one's bytes:
   one that no result took for OWN_IDLE presents goes at any time.

   With device memory at the caches' trim mark (vk_mem_trim_start) that was too little: on a GPU of 2 GB the same
   game's 1,930 tile results held 770 MB from its boot, the caches had nothing left to free, its shadow maps' depth
   targets found no memory and 94% of its draws were skipped, at 4 presents a second, where 16 results every fourth
   present land in minutes. From that mark on the trim runs at each present, lets every freed buffer of the pool go,
   takes the results left alone for OWN_IDLE_SHORT presents, the largest first, and lands up to OWN_LAND_SHORT of
   them, as many as bring the use back to four fifths of the budget and the results to an eighth of it. Results in
   system memory (vk_own_res_new's last resort) stay: landing them frees nothing of the GPU's */
typedef struct {
    u64 addr, size;
    u32 used;
    VkOwnRes* res;
} OwnPick;
static void res_release(void* res);
/* a reference, unless another thread let go of the last one already: that one is freed at this thread's vk_unlock
   (vk_own_release_later), so it is still readable, but must not be held again */
static int res_hold_live(VkOwnRes* r) {
    u32 n = atomic_load_explicit(&r->refs, memory_order_relaxed);
    do
        if (!n) return 0;
    while (!atomic_compare_exchange_weak_explicit(&r->refs, &n, n + 1, memory_order_relaxed, memory_order_relaxed));
    return 1;
}
static int pick_trim(const void* a, const void* b) {
    const OwnPick *pa = (const OwnPick*)a, *pb = (const OwnPick*)b;
    if (pa->size != pb->size) return pa->size > pb->size ? -1 : 1;
    i32 d = (i32)(pa->used - pb->used);
    return (d > 0) - (d < 0);
}
void vk_own_trim(void) {
    u64 usage, budget;
    vk_mem_use(&usage, &budget);
    const u64 heap0 = vk_mem_heap_use(), pressure0 = heap0 > usage ? heap0 : usage;
    const int past = pressure0 >= vk_mem_trim_start(budget);
    if (g_vk_present % 4 && !past) return;
    /* with device memory short a free buffer waits OWN_IDLE_SHORT presents for a result, not OWN_IDLE: at ten presents
       a second the pool held 120 to 230 MB for twelve seconds that the heap had no room without; at the caches' trim
       mark every free buffer goes at once */
    const u32 idle = past ? 0 : pressure0 < budget / 5 * 4 ? OWN_IDLE : OWN_IDLE_SHORT;
    OwnDone done = own_done();
    pm_lock(&g_pool_lk);
    for (u32 i = 0; i < atomic_load_explicit(&g_pool_n, memory_order_relaxed);) {
        if ((u32)(g_vk_present - g_pool[i].put) < idle || !own_pool_free(&g_pool[i], done)) {
            i++;
            continue;
        }
        vk_mem_untrack(g_pool[i].mem);
        vk_devmem_buffer_destroy(g_pool[i].buf, g_pool[i].mem); /* free: the GPU is done with it */
        g_pool[i] = g_pool[atomic_fetch_sub_explicit(&g_pool_n, 1, memory_order_relaxed) - 1];
        g_owned.trim_pooled++;
    }
    pm_unlock(&g_pool_lk);
    vk_mem_use(&usage, &budget);
    const u64 heap = vk_mem_heap_use(), pressure = heap > usage ? heap : usage;
    const u64 mark = budget / 5 * 4, own_cap = budget / 8;
    const u64 dev_bytes = atomic_load_explicit(&g_owned.live_dev_bytes, memory_order_relaxed);
    const u64 over_p = pressure > mark ? pressure - mark : 0;
    const u64 over_o = dev_bytes > own_cap ? dev_bytes - own_cap : 0;
    const u64 excess = over_p > over_o ? over_p : over_o;
    u32 n = atomic_load_explicit(&g_own.nown, memory_order_acquire);
    if (!g_owned.traps || !n || !excess) return;
    u32 max = n + 64; /* more than there are: a publish may add some before the lookup */
    OwnRange* r = malloc((size_t)max * sizeof *r);
    OwnPick* pick = malloc((size_t)max * sizeof *pick);
    if (!r || !pick) {
        free(r);
        free(pick);
        return;
    }
    const u32 alone = past ? OWN_IDLE_SHORT : OWN_IDLE, most = past ? OWN_LAND_SHORT : OWN_LAND_MAX;
    u32 np = 0, k = own_lookup(0, 1ull << 48, r, max);
    u64 now = vk_submit_number();
    for (u32 i = 0; i < k && i < max; i++) {
        VkOwnRes* o = (VkOwnRes*)r[i].res;
        /* not one of another's buffer (the mirror's), nor one whose writes are still being recorded, nor one in system
           memory */
        if (!o || o->borrowed || o->host || (u32)(g_vk_present - o->used) < alone ||
            atomic_load_explicit(&o->written, memory_order_relaxed) >= now)
            continue;
        /* held while the loop below runs: a landing takes its whole pages, so it can let go of a pick beside the one
           it lands, and this thread holds vk_lock, so res_release frees that pick at once */
        if (!res_hold_live(o)) continue;
        pick[np++] = (OwnPick){r[i].addr, r[i].size, o->used, o};
    }
    free(r);
    qsort(pick, np, sizeof *pick, pick_trim);
    /* at or past the caches' trim mark: as many bytes as the heap is over four fifths of its budget. Below it, landings
       stop at 64 MiB a call, so that draining gigabytes of idle copies doesn't stall a frame, but the first pick, the
       largest, lands whole. The bytes counted are the ranges landed: a buffer with another range still owned stays,
       and a later call lands that range */
    const u64 want = past ? excess : (excess < (64ull << 20) ? excess : (64ull << 20));
    u64 got = 0;
    for (u32 i = 0; i < np && i < most && (!i || got < want); i++) {
        /* res_free_buffer gives the buffer back to the heap, not to the pool */
        pick[i].res->no_pool = 1;
        own_acquire(pick[i].addr, pick[i].size);
        got += pick[i].size;
        g_owned.trim_landed++;
        g_owned.trim_bytes += pick[i].size;
    }
    for (u32 i = 0; i < np; i++) res_release(pick[i].res);
    free(pick);
}

VkOwnRes* vk_own_res_new(u64 size, u32 by) {
    VkOwnRes* r = (VkOwnRes*)calloc(1, sizeof *r);
    u64 want = size ? size : 16;
    if (!r) return NULL;
    OwnPoolEntry e = own_pool_take(want);
    if (e.buf) {
        r->buf = e.buf;
        r->mem = e.mem;
        r->size = e.cap;
    } else {
        u32 fam[2];
        VkBufferCreateInfo bi = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = want,
            /* bound where it is for whatever reads the bytes it holds: a draw's vertices, indices and
                                    buffers (buffer.c buf_get_owned), a dispatch's (compute.c res_bind), VIC's job words;
                                    and what draws write there, transform feedback's captures too (buffer.c keep_new) */
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                     (g_vk.has_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0) |
                     /* an indirect copy or an in-place read names it by its address
                                             (copy_batch_flush, inplace_note) */
                                          (g_vk.has_copy_indirect || g_vk.has_inplace_loads
                                               ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                               : 0),
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        DevMemWant ai = {.at = DM_DEVICE, .brief = 1}; /* results come and go by the hundred MB (DM_KIND_BRIEF) */
        land_sharing(&bi, fam);
        if (size > g_vk.alloc_cap) {
            free(r);
            return NULL;
        }
        /* With device memory short, a freed buffer of the size that the GPU still uses is waited for and taken
           before more memory is: the results then come no faster than the GPU frees them, where they took the heap
           to its budget while 370 to 460 MB of freed buffers waited for the GPU, and the rings and the buffer cache
           found none.
           Without room, the pool makes it: its free buffers go back to the heap; then a freed buffer of the size is
           waited for; then all of them are, and go back too. Last, the result is made in system memory, which the
           GPU reads and writes across the bus: slower, where the dispatch or the copy that makes it was skipped, and
           what read its bytes after (one game's indirect draws their counts) read what was there before. Only that
           one's failure is said */
        int made = 0;
        u64 usage, budget;
        vk_mem_use(&usage, &budget);
        const u64 heap = vk_mem_heap_use(), pressure = heap > usage ? heap : usage;
        if (pressure + want > budget / 5 * 4 && own_pool_await(want)) e = own_pool_take(want);
        /* A copy engine's result with the heap past four fifths of its budget: in system memory from the start. They
           are a game's streamed bytes, which a texture's load reads once, over the bus: in a racing game on a GPU
           of 2 GB, 2,000 tile results took 370 MB of the GPU's memory as its race loaded, and its render targets and
           storage images found none */
        if (!e.buf && by == VK_OWN_BY_COPY && pressure + want > budget / 5 * 4 && g_vk.mem_model != VK_MEM_UMA) {
            ai.at = DM_HOST;
            made = vk_buffer_create(&bi, &ai, &r->buf, &r->mem, NULL) == VK_SUCCESS;
            ai.at = DM_DEVICE;
            if (made) {
                r->host = 1;
                g_owned.spilled++;
                g_owned.spilled_bytes += want;
            }
        }
        if (!e.buf && !made)
            made = vk_buffer_create(&bi, &ai, &r->buf, &r->mem, NULL) == VK_SUCCESS ||
                   (own_pool_drop() && vk_buffer_create(&bi, &ai, &r->buf, &r->mem, NULL) == VK_SUCCESS);
        if (!e.buf && !made && own_pool_await(want)) e = own_pool_take(want);
        if (e.buf) {
            r->buf = e.buf;
            r->mem = e.mem;
            r->size = e.cap;
        } else if (!made) {
            if (own_pool_await(0)) own_pool_drop();
            if (vk_buffer_create(&bi, &ai, &r->buf, &r->mem, NULL) != VK_SUCCESS) {
                ai.at = DM_HOST;
                if (g_vk.mem_model == VK_MEM_UMA || !VK_OK(vk_buffer_create(&bi, &ai, &r->buf, &r->mem, NULL))) {
                    free(r);
                    return NULL;
                }
                r->host = 1;
                g_owned.spilled++;
                g_owned.spilled_bytes += want;
                LOG_LIMIT(1, "vulkan: no device memory for a kept result of %llu KiB: it is in system memory, as "
                             "each one is that finds none (the statistics count them)\n",
                          (unsigned long long)(want >> 10));
            }
            made = 1;
        }
        if (made) {
            vk_mem_track(r->mem, VK_OWN_OWNED);
            r->size = want;
        }
    }
    atomic_store_explicit(&r->refs, 1, memory_order_relaxed);
    atomic_store_explicit(&r->serial, (atomic_fetch_add_explicit(&g_owned.serial, 1, memory_order_relaxed) + 1),
                          memory_order_relaxed);
    r->by = (u8)by;
    r->born = r->used = g_vk_present;
    atomic_fetch_add_explicit(&g_owned.live[by], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_owned.live_bytes[by], r->size, memory_order_relaxed);
    if (!r->host) atomic_fetch_add_explicit(&g_owned.live_dev_bytes, r->size, memory_order_relaxed);
    return r;
}
static void res_retain(void* res) { atomic_fetch_add_explicit(&((VkOwnRes*)res)->refs, 1, memory_order_relaxed); }
VkOwnRes* vk_own_res_borrow(VkBuffer buf) {
    VkOwnRes* r = (VkOwnRes*)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->buf = buf;
    r->size = ~0ull;
    atomic_store_explicit(&r->refs, 1, memory_order_relaxed);
    r->borrowed = 1;
    atomic_store_explicit(&r->serial, (atomic_fetch_add_explicit(&g_owned.serial, 1, memory_order_relaxed) + 1),
                          memory_order_relaxed);
    return r;
}
static void res_free_buffer(VkOwnRes* r);
static void res_release(void* res) {
    VkOwnRes* r = (VkOwnRes*)res;
    if ((atomic_fetch_sub_explicit(&r->refs, 1, memory_order_acq_rel) - 1)) return;
    if (r->borrowed) {
        free(r);
        return;
    }
    u32 kept = g_vk_present - r->born;
    if (!r->landed) {
        atomic_fetch_add_explicit(&g_owned.unread[r->by], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_owned.unread_bytes[r->by], r->size, memory_order_relaxed);
    }
    atomic_fetch_sub_explicit(&g_owned.live[r->by], 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g_owned.live_bytes[r->by], r->size, memory_order_relaxed);
    if (!r->host) atomic_fetch_sub_explicit(&g_owned.live_dev_bytes, r->size, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_owned.let_go[kept < 8      ? 0
                                              : kept < 120  ? 1
                                              : kept < 1200 ? 2
                                                            : 3],
                              1, memory_order_relaxed);
    /* Another thread holds vk_lock: the buffer goes later, at that thread's outermost vk_unlock or the next one
       (vk_own_release_later). A landing lets its resources go after it let go of vk_lock (own.c's land_start), and a
       wait for the lock there held the faulting game thread behind the renderer's draws: 6.2 s in 83 s of one game */
    if (!vk_trylock()) {
        VkOwnRes* head = atomic_load_explicit(&g_owned.later, memory_order_relaxed);
        do r->later = head;
        while (!atomic_compare_exchange_weak_explicit(&g_owned.later, &head, r, memory_order_release,
                                                      memory_order_relaxed));
        return;
    }
    res_free_buffer(r);
    vk_unlock();
}
/* under vk_lock: a resource no one refers to, its buffer to the pool or to the frame's end, and its record */
static u32 land_slot_of(u64 key);
static void res_free_buffer(VkOwnRes* r) {
    if (r->early) { /* under vk_lock: the copy may still be on the GPU, so it goes with the frame */
        if (!r->landed) {
            u32 slot = land_slot_of(r->early_key);
            if (g_owned.land_key[slot].key == r->early_key) g_owned.land_key[slot].unread++;
            g_owned.early_unread++;
        }
        vk_defer_buffer(r->early, r->early_mem);
    }
    /* the frame recording may still copy from it: the pool keeps it from the next result until the GPU has done what
       is recorded now. A full pool has it destroyed with the frame, as before the pool */
    /* one in system memory goes with the frame too: the next result is made in the device's, when it has room */
    if (r->host || r->no_pool || !own_pool_put(r->buf, r->mem, r->size)) {
        vk_defer_buffer(r->buf, r->mem);
        if (!r->host) {
            vk_defer_bytes(r->size);
            if (vk_devmem_emptying()) vk_defer_moved(r->size);
        }
    }
    free(r);
}
int vk_own_later_any(void) { return atomic_load_explicit(&g_owned.later, memory_order_acquire) != NULL; }
void vk_own_release_later(void) {
    VkOwnRes* r = atomic_exchange_explicit(&g_owned.later, NULL, memory_order_acquire);
    while (r) {
        VkOwnRes* next = r->later;
        res_free_buffer(r);
        r = next;
    }
}
void vk_own_res_ref(VkOwnRes* r) { res_retain(r); }
void vk_own_res_unref(VkOwnRes* r) { res_release(r); }
/* The results whose device address a command not recorded yet reads (an in-place read's table, an indirect copy's
   list), on this thread: each holds a reference until this thread's hold of vk_lock ends (vk_own_release_held), as
   the reads are recorded by then. Without it, one released before its read is recorded goes to the pool, and a
   submission in between lets the next result take its buffer and write it before the read. Each thread its own: a
   texture load can let go of vk_lock part-way (vk_submit_upload_wait), and another thread's end of a hold would let
   go of this one's results */
static _Thread_local struct {
    VkOwnRes** r;
    u32 n, cap;
} t_held;
/* under vk_lock: r kept until this hold of vk_lock ends. 0 when there is no room to note it: the caller copies it
   now */
static int res_hold(VkOwnRes* r) {
    if (t_held.n == t_held.cap) {
        u32 cap = t_held.cap ? t_held.cap * 2 : 256;
        VkOwnRes** h = (VkOwnRes**)realloc(t_held.r, cap * sizeof *h);
        if (!h) return 0;
        t_held.r = h;
        t_held.cap = cap;
    }
    res_retain(r);
    t_held.r[t_held.n++] = r;
    return 1;
}
int vk_own_held_any(void) { return t_held.n != 0; }
void vk_own_release_held(void) {
    while (t_held.n) res_release(t_held.r[--t_held.n]);
}

/* every write the GPU did so far to the results' buffers visible to copies (before), the copies' writes to the host
   (after). On the compute queue (apart) the writer's submission's writes come with its semaphore, and the barrier
   names the stages that queue has */
static void land_barrier(VkCommandBuffer cb, int before, int apart) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    if (before) {
        b.srcStageMask =
            apart ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT : vk_buffer_stages();
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    } else {
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    }
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

/* a piece of a landing: res's bytes from src land at dst, staged at `at` in the batch's host-cached copy */
typedef struct {
    const VkOwnRes* res;
    u64 src, at, size;
    u8* dst;
} Piece;

/* The guest memory the CPU made land, by 64 KB granule, with the present it landed at: a compute dispatch keeps a
   written storage buffer on the GPU only where the CPU hasn't read it lately (vk_own_cpu_read). Only a guest access
   arms it: a landing the runtime makes for itself (own_publish, own_acquire) says nothing about the CPU, and armed
   the eager landing of the mirror for 1200 presents where the game read a buffer once.
   Direct-mapped by granule, each slot holding its granule's number, under vk_lock (own_land, vk_dispatch) */
static void cpu_read_note(u64 cpu, u64 size) {
    for (u64 g = cpu >> 16, e = (cpu + size - 1) >> 16, k = 0; g <= e && k < 256; g++, k++) {
        u32 slot = (u32)(g * 0x9E3779B97F4A7C15ull >> 52);
        g_owned.cpu_read[slot].granule = g + 1;
        g_owned.cpu_read[slot].present = g_vk_present;
    }
}
void vk_own_cpu_touched(u64 cpu, u64 size) {
    if (size) cpu_read_note(cpu, size);
}
int vk_own_cpu_read(u64 cpu, u64 size, u32 presents) {
    for (u64 g = cpu >> 16, e = (cpu + size - 1) >> 16, k = 0; g <= e && k < 256; g++, k++) {
        u32 slot = (u32)(g * 0x9E3779B97F4A7C15ull >> 52);
        if (g_owned.cpu_read[slot].granule == g + 1 && g_vk_present - g_owned.cpu_read[slot].present < presents) return 1;
    }
    return 0;
}
/* A landing's staging: the copy's commands and the host-cached buffer it copies into. Separate from frame pools and
   rings: frame result callbacks may acquire the owner being read back. A landing that lets go of vk_lock while the GPU
   copies (own_land_start) keeps its slot until own_land_end, and the landings meanwhile take others. A slot is one
   landing's at a time: taken and put back under g_land_lk */
typedef struct LandSlot {
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkBuffer back;
    DevMem mem;
    u8* ptr;
    u64 cap;
    struct LandSlot* next;
} LandSlot;
static LandSlot* g_land_free;
static PMutex g_land_lk = PMUTEX_INIT;
static LandSlot* land_slot_take(void) {
    pm_lock(&g_land_lk);
    LandSlot* s = g_land_free;
    if (s) g_land_free = s->next;
    pm_unlock(&g_land_lk);
    if (!s && !(s = (LandSlot*)calloc(1, sizeof *s))) fatal("vulkan: out of memory for an ownership readback");
    return s;
}
static void land_slot_put(LandSlot* s) {
    pm_lock(&g_land_lk);
    s->next = g_land_free;
    g_land_free = s;
    pm_unlock(&g_land_lk);
}
/* under vk_lock: the slot's commands, and room for size bytes */
static void land_prepare(LandSlot* s, u64 size) {
    if (!s->pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .queueFamilyIndex = land_apart() ? g_vk.compute_family : g_vk.gfx_family};
        if (!VK_OK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &s->pool)))
            fatal("vulkan: could not create the ownership readback queue resources");
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = s->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (!VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &s->cb)))
            fatal("vulkan: could not allocate the ownership readback commands");
    }
    if (size <= s->cap) return;
    u64 cap = 4096, limit = g_vk.alloc_cap / 4;
    while (cap < size && cap < limit) cap = cap > limit / 2 ? limit : cap * 2;
    if (cap > limit) cap = limit;
    if (size > cap) fatal("vulkan: ownership readback exceeds its allocation limit");
    if (s->back) {
        vk_mem_untrack(s->mem);
        vk_devmem_buffer_destroy(s->back, s->mem);
    }
    u32 fam[2];
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    land_sharing(&bi, fam);
    DevMemWant ai = {.at = DM_CACHED, .mapped = 1};
    DevMemInfo info;
    if (!VK_OK(vk_buffer_create(&bi, &ai, &s->back, &s->mem, &info)))
        fatal("vulkan: no host memory for ownership readback");
    vk_mem_track(s->mem, VK_OWN_OWNED);
    s->ptr = info.ptr;
    s->cap = cap;
}

/* the submission a resource's writes are in (0: unstamped, everything submitted): own.c's writer, on a thread without
   vk_lock too */
static u64 own_writer(void* res) {
    return atomic_load_explicit(&((const VkOwnRes*)res)->written, memory_order_acquire);
}

/* what a landing's copies wait for, worked out under vk_lock before the first */
typedef struct {
    u64 writer;
    int apart, finished;
    VkQueue queue;
} LandCtx;
/* Early copy back (vk_early_copy_back, the GPU plan's M10a). A result the game's CPU reads lands when its first access
   faults: the faulting thread waits for the GPU's work, submits a copy into host memory and waits for it, then copies
   the bytes into guest memory. One racing game's title screen read about two render targets a present back that way,
   98% of them at the same address and size as a read in the 1200 presents before. A result at such a key (hot: a read
   of the game's at the same producer, start and size within EARLY_KEEP_PRESENTS presents, land_note) is copied into
   host-cached memory in the producer's own submission, right after its writes. Its landing then copies those bytes,
   with no submission and no second wait, while the result is unchanged (its serial, which every write of it moves)
   and that submission is done. Else it lands as before. A key whose copies go unread EARLY_UNREAD times in a row
   stops being copied until a read lands it again. Only render targets for now, on discrete GPUs */
#define EARLY_KEEP_PRESENTS 1200u
#define EARLY_UNREAD        8u
#define EARLY_BUDGET        (64ull << 20) /* bytes copied early in one present, at most */
static u64 land_key_of(u32 prod, u64 addr, u64 size) {
    return snx_fnv_step(snx_fnv_step(snx_fnv_step(SNX_FNV_BASIS, prod), addr), size);
}
static u32 land_slot_of(u64 key) { return (u32)(key >> 54) & 1023; }
static void lander_queue(u64 addr, u64 size, VkOwnRes* r, u64 value);
/* under vk_lock: r's bytes [off, off + size), published at guest cpu, copied early when its key is hot */
static void early_copy(u64 cpu, u64 size, VkOwnRes* r, u64 off) {
    if (!g_settings.early_copy_back || r->by != VK_OWN_BY_TARGET || r->borrowed || r->early ||
        (vk_uma() && !vk_uma_storage_copied()) ||
        size > g_vk.alloc_cap / 4)
        return;
    u64 key = land_key_of(r->by, cpu, size);
    u32 slot = land_slot_of(key);
    if (g_owned.land_key[slot].key != key || g_vk_present - g_owned.land_key[slot].present > EARLY_KEEP_PRESENTS ||
        g_owned.land_key[slot].unread >= EARLY_UNREAD)
        return;
    if (g_owned.early_present != g_vk_present) g_owned.early_present = g_vk_present, g_owned.early_now = 0;
    if (g_owned.early_now + size > EARLY_BUDGET) return;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant ai = {.at = DM_CACHED, .mapped = 1};
    DevMemInfo info;
    if (!VK_OK(vk_buffer_create(&bi, &ai, &r->early, &r->early_mem, &info))) {
        r->early = VK_NULL_HANDLE;
        return;
    }
    VkCommandBuffer cb = vk_cmd();
    vk_all_barrier(cb); /* the producer's writes, before the copy reads them */
    VkBufferCopy c = {off, 0, size};
    rec_CmdCopyBuffer(cb, r->buf, r->early, 1, &c);
    /* the copy's bytes for the host, and a later write of buf after the copy's read */
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                          .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
    r->early_ptr = info.ptr;
    r->early_off = off;
    r->early_size = size;
    r->early_serial = atomic_load_explicit(&r->serial, memory_order_relaxed);
    r->early_value = vk_submit_number();
    r->early_key = key;
    g_owned.early_now += size;
    g_owned.early_n++;
    g_owned.early_bytes += size;
    lander_queue(cpu, size, r, r->early_value);
}
/* The early lander (vk_early_landing, the GPU plan's M10b). Of the game's reads of results copied early, 85 to 90% found
   the GPU's work done when they faulted: they still stopped for the fault and copied the bytes into the trapped pages,
   about 0.5 MB and 0.5 ms a present on the game's thread. A thread of the renderer's lands each early copy instead, as
   soon as its submission is done, through own.c's landing for the runtime (own_acquire), as any runtime access lands:
   the game's read then finds the bytes and no trap. It lands a range only while the result it copied still owns it
   whole (own_lookup), else the game's access or a newer result decides. It waits on the graphics timeline for the
   value, which submits nothing (a submission that isn't made yet is waited for a while at a time). It holds no lock when
   it lands, never touches guest memory itself, and holds a reference to each result until it is done with it */
#define LANDER_N 64u
static struct {
    PMutex lk;
    PCond cv;
    struct {
        u64 addr, size, value;
        VkOwnRes* res;
    } q[LANDER_N];
    u32 head, n;
    int started;
    _Atomic u64 landed, missed; /* for the statistics: ranges landed, and those whose result was not the owner */
} g_lander = {.lk = PMUTEX_INIT};
static void lander_run(void* arg) {
    (void)arg;
    VkSemaphore sem = vk_gfx_timeline();
    for (;;) {
        pm_lock(&g_lander.lk);
        while (!g_lander.n) pc_wait(&g_lander.cv, &g_lander.lk, -1);
        u64 addr = g_lander.q[g_lander.head].addr, size = g_lander.q[g_lander.head].size,
            value = g_lander.q[g_lander.head].value;
        VkOwnRes* res = g_lander.q[g_lander.head].res;
        pm_unlock(&g_lander.lk);
        VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                  .semaphoreCount = 1,
                                  .pSemaphores = &sem,
                                  .pValues = &value};
        VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 50000000ull);
        if (r == VK_TIMEOUT) continue; /* not done yet: the same one again */
        pm_lock(&g_lander.lk);
        g_lander.head = (g_lander.head + 1) % LANDER_N;
        g_lander.n--;
        pm_unlock(&g_lander.lk);
        OwnRange o;
        if (r == VK_SUCCESS && own_lookup(addr, size, &o, 1) == 1 && o.res == res && o.addr == addr && o.size == size) {
            own_acquire(addr, size);
            atomic_fetch_add_explicit(&g_lander.landed, 1, memory_order_relaxed);
        } else
            atomic_fetch_add_explicit(&g_lander.missed, 1, memory_order_relaxed);
        vk_own_res_unref(res);
    }
}
/* the lander's counts since the last report, which it zeroes */
static void lander_stats(u64* landed, u64* missed) {
    *landed = atomic_exchange_explicit(&g_lander.landed, 0, memory_order_relaxed);
    *missed = atomic_exchange_explicit(&g_lander.missed, 0, memory_order_relaxed);
}
/* under vk_lock: r's early copy of guest [addr, addr + size), in submission value, for the lander. Left to the game's
   access when the queue is full */
static void lander_queue(u64 addr, u64 size, VkOwnRes* r, u64 value) {
    if (!g_settings.early_landing) return;
    pm_lock(&g_lander.lk);
    if (!g_lander.started) {
        pc_init(&g_lander.cv);
        g_lander.started = plat_thread(lander_run, NULL, 0) == 0 ? 1 : -1;
    }
    if (g_lander.started > 0 && g_lander.n < LANDER_N) {
        u32 at = (g_lander.head + g_lander.n) % LANDER_N;
        res_retain(r);
        g_lander.q[at].addr = addr;
        g_lander.q[at].size = size;
        g_lander.q[at].value = value;
        g_lander.q[at].res = r;
        g_lander.n++;
        pc_signal(&g_lander.cv);
    }
    pm_unlock(&g_lander.lk);
}
/* under vk_lock: the ranges landed from their results' early copies, when each has a valid one whose submission is
   done. 0, with nothing copied, when one hasn't */
static int early_land(const OwnRange* r, u8* const* dst, u32 n) {
    u64 done = 0;
    for (u32 k = 0; k < n; k++) {
        const VkOwnRes* res = (const VkOwnRes*)r[k].res;
        if (!res || !res->early || atomic_load_explicit(&res->serial, memory_order_acquire) != res->early_serial ||
            r[k].res_off < res->early_off || r[k].res_off + r[k].size > res->early_off + res->early_size)
            return 0;
        if (done < res->early_value &&
            (!VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, vk_gfx_timeline(), &done)) || done < res->early_value))
            return 0;
    }
    for (u32 k = 0; k < n; k++) {
        const VkOwnRes* res = (const VkOwnRes*)r[k].res;
        if (!VK_OK(vk_devmem_invalidate(res->early_mem, 0, VK_WHOLE_SIZE)))
            fatal("vulkan: could not read an early copy of a GPU result");
        memcpy(dst[k], res->early_ptr + (r[k].res_off - res->early_off), r[k].size);
    }
    g_owned.early_hits++;
    return 1;
}
/* under vk_lock: a landing's counts for the statistics (g_owned.land_n and the rest). Its producer is the one of its
   largest range */
static void land_note(const OwnRange* r, u32 n, int by) {
    u64 all = 0, most = 0;
    u32 prod = VK_OWN_BY_COMPUTE;
    for (u32 k = 0; k < n; k++) {
        VkOwnRes* res = (VkOwnRes*)r[k].res;
        all += r[k].size;
        if (!res) continue;
        res->landed = 1;
        if (r[k].size > most) most = r[k].size, prod = res->by;
    }
    if (!n || by < 0 || by >= OWN_BY_N) return;
    g_owned.land_n[prod][by]++;
    g_owned.land_bytes[prod][by] += all;
    if (by == OWN_BY_RUNTIME) return;
    u64 key = land_key_of(prod, r[0].addr, all);
    u32 slot = land_slot_of(key);
    if (g_owned.land_key[slot].key == key && g_vk_present - g_owned.land_key[slot].present <= EARLY_KEEP_PRESENTS) {
        g_owned.pred_n[prod]++;
        g_owned.pred_bytes[prod] += all;
    }
    g_owned.land_key[slot].unread = 0; /* a read of the game's: its copies are read again */
    g_owned.land_key[slot].key = key;
    g_owned.land_key[slot].present = g_vk_present;
}
static LandCtx land_ctx(const OwnRange* r, u32 n, int by) {
    LandCtx c = {0};
    land_note(r, n, by);
    for (u32 k = 0; k < n; k++) {
        u64 w = own_writer(r[k].res);
        if (by != OWN_BY_RUNTIME) cpu_read_note(r[k].addr, r[k].size);
        if (w > c.writer) c.writer = w;
    }
    c.apart = land_apart();
    c.queue = c.apart ? g_vk.compute : g_vk.gfx;
    /* done on the GPU already (own_wait waited for it with no lock held): the copy waits for that submission's
       signal, which is there, and for nothing of the recording thread, whose queue calls it takes the queue's lock
       beside. Waiting for them held vk_lock while the recording thread waited for a draw's pipeline */
    u64 gpu_done = 0;
    c.finished = c.writer && c.writer <= vk_last_submit() &&
                 VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, vk_gfx_timeline(), &gpu_done)) && gpu_done >= c.writer;
    /* the open commands go only when the writes are among them: on the graphics queue the copy follows them anyway */
    if (!c.finished && (!c.apart || !c.writer || c.writer > vk_last_submit()))
        vk_submit_for_readback();
    else if (!c.finished)
        vk_rec_sync(); /* the recorder's calls on the queue (the compute one may be its too) before the copy's */
    /* unstamped: everything submitted. Past the last submission: nothing was recording, so nothing of it was written
       there, and a wait for a value never signaled would never end */
    if (!c.writer || c.writer > vk_last_submit()) c.writer = vk_last_submit();
    return c;
}
#define LAND_PIECES 64
/* the next pieces of r[] from (*i, *done), up to cap bytes and LAND_PIECES pieces: their bytes */
static u64 land_gather(const OwnRange* r, u8* const* dst, u32 n, u32* i, u64* done, u64 cap, Piece* pc, u32* np) {
    u64 total = 0;
    *np = 0;
    while (*i < n && *np < LAND_PIECES && total < cap) {
        u64 len = r[*i].size - *done < cap - total ? r[*i].size - *done : cap - total;
        pc[*np].res = (const VkOwnRes*)r[*i].res;
        pc[*np].src = r[*i].res_off + *done;
        pc[*np].at = total;
        pc[*np].size = len;
        pc[*np].dst = dst[*i] + *done;
        (*np)++;
        total += len;
        *done += len;
        if (*done == r[*i].size) {
            (*i)++;
            *done = 0;
        }
    }
    return total;
}
/* under vk_lock: the pieces copied into s's buffer, submitted. The value the copy signals on g_owned.done */
static u64 land_submit(LandSlot* s, const LandCtx* c, const Piece* pc, u32 np, u64 total) {
    land_prepare(s, total);
    VkCommandBuffer cb = s->cb;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (!VK_OK(vkResetCommandPool(g_vk.dev, s->pool, 0)) || !VK_OK(vkBeginCommandBuffer(cb, &bi)))
        fatal("vulkan: could not begin ownership readback");
    land_barrier(cb, 1, c->apart);
    for (u32 j = 0; j < np; j++) {
        VkBufferCopy cp = {pc[j].src, pc[j].at, pc[j].size};
        vkCmdCopyBuffer(cb, pc[j].res->buf, s->back, 1, &cp);
    }
    land_barrier(cb, 0, c->apart);
    VkCommandBufferSubmitInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    /* on the compute queue, the writer's submission alone: its signal carries the writes to the copy */
    VkSemaphoreSubmitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                .semaphore = vk_gfx_timeline(),
                                .value = c->writer,
                                .stageMask = VK_PIPELINE_STAGE_2_COPY_BIT};
    u64 value = vk_host_next(&g_owned.done);
    VkSemaphoreSubmitInfo di = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                .semaphore = g_owned.done.sem,
                                .value = value,
                                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .waitSemaphoreInfoCount = (c->apart || c->finished) && c->writer ? 1u : 0u,
                        .pWaitSemaphoreInfos = &wi,
                        .commandBufferInfoCount = 1,
                        .pCommandBufferInfos = &ci,
                        .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &di};
    if (!value || !VK_OK(vkEndCommandBuffer(cb))) fatal("vulkan: could not submit ownership readback");
    if (c->apart || c->finished) {
        if (!c->finished) vk_rec_queue(c->queue); /* after the recorder's compute submissions, as cs_submit's */
        vk_queue_lock(c->queue);
    }
    VkResult sr = vkQueueSubmit2(c->queue, 1, &si, VK_NULL_HANDLE);
    if (c->apart || c->finished) vk_queue_unlock(c->queue);
    if (!VK_OK(sr)) fatal("vulkan: could not submit ownership readback");
    return value;
}
/* with vk_lock or without it: the copy done, its bytes where the pieces go */
static void land_finish(const LandSlot* s, u64 value, const Piece* pc, u32 np, u64 total) {
    gpu_wait_note("GPU-owned memory readback", 0);
    int ok = vk_host_wait(&g_owned.done, value);
    gpu_wait_note(NULL, 0);
    if (!ok) { /* a GPU fault (Xid 109's CTX SWITCH TIMEOUT, say): name the draw that hung, from its checkpoints */
        vk_checkpoint_dump("device lost");
    }
    if (!ok || !VK_OK(vk_devmem_invalidate(s->mem, 0, total))) fatal("vulkan: could not complete ownership readback");
    for (u32 j = 0; j < np; j++) memcpy(pc[j].dst, s->ptr + pc[j].at, pc[j].size);
}
/* own.c's land: every copy waited for under vk_lock */
static void own_land(const OwnRange* r, u8* const* dst, u32 n, int by) {
    u64 cap = g_vk.alloc_cap / 4, done = 0;
    u32 i = 0;
    SNX_ZONE_V(z, "vk own land", n);
    vk_lock();
    if (early_land(r, dst, n)) {
        land_note(r, n, by);
        vk_unlock();
        return;
    }
    LandCtx c = land_ctx(r, n, by);
    LandSlot* s = land_slot_take();
    while (i < n) {
        Piece pc[LAND_PIECES];
        u32 np;
        u64 total = land_gather(r, dst, n, &i, &done, cap, pc, &np);
        land_finish(s, land_submit(s, &c, pc, np, total), pc, np, total);
    }
    land_slot_put(s);
    vk_unlock();
}
/* a landing's copy submitted and not waited for yet (own_land_start), for own_land_end */
typedef struct {
    LandSlot* s;
    u64 value, total;
    u32 np;
    Piece pc[LAND_PIECES];
} LandCopy;
/* own.c's land_start: a landing that fits one copy is submitted, and own_land_end waits for it with vk_lock let go.
   Its resources live until then: own.c holds them until the landing is done. A bigger one lands here, as own_land */
static void* own_land_start(const OwnRange* r, u8* const* dst, u32 n, int by) {
    u64 cap = g_vk.alloc_cap / 4, all = 0;
    for (u32 k = 0; k < n; k++) all += r[k].size;
    LandCopy* h = n <= LAND_PIECES && all <= cap ? (LandCopy*)malloc(sizeof *h) : NULL;
    if (h) { /* landed from an early copy: nothing for own_land_end */
        vk_lock();
        int early = early_land(r, dst, n);
        if (early) land_note(r, n, by);
        vk_unlock();
        if (early) {
            free(h);
            return NULL;
        }
    }
    if (!h) {
        own_land(r, dst, n, by);
        return NULL;
    }
    SNX_ZONE_V(z, "vk own land start", n);
    vk_lock();
    LandCtx c = land_ctx(r, n, by);
    u32 i = 0;
    u64 done = 0;
    h->s = land_slot_take();
    h->total = land_gather(r, dst, n, &i, &done, cap, h->pc, &h->np);
    h->value = land_submit(h->s, &c, h->pc, h->np, h->total);
    vk_unlock();
    return h;
}
/* own.c's land_end: with no lock held */
static void own_land_end(void* handle) {
    LandCopy* h = (LandCopy*)handle;
    SNX_ZONE_V(z, "vk own land end", h->np);
    land_finish(h->s, h->value, h->pc, h->np, h->total);
    land_slot_put(h->s);
    free(h);
}

static u32 lock_owner(void) { return vk_lock_owner(); }
/* own.c's, before a landing takes vk_lock: the wait for the largest of its entries' own_writer. The thread holds no
   vk_lock here but while it hands the commands being recorded to the GPU, when the writes may be among them, as
   own_land would. A thread that holds vk_lock already (the GPU thread reading guest memory) lands under it as
   before */
static void own_wait(u64 writer) {
    u64 submitted = 0, done = 0;
    if (vk_lock_held()) return;
    vk_submit_progress(&submitted, &done);
    if (!writer || writer > submitted) {
        vk_lock();
        vk_submit_open();
        if (!writer || writer > vk_last_submit()) writer = vk_last_submit();
        vk_unlock();
    }
    if (writer > done) vk_wait_submit(writer); /* handed to the recording thread: it submits it */
}
/* own.c's, under vk_lock before a landing looks for its entries: the storage images the guest released, which the
   release left on the GPU, published over those pages first */
static void own_claim(u64 lo, u64 hi) { vk_gfx_image_cpu_need(lo, hi - lo); }
/* a landing's bytes: the write tracking takes them as they were left, and the GPU mirror lets go of its copy's
   shadow */
static void own_landed(u64 addr, u64 size, const u8* bytes) {
    vk_ww_landed(addr, size, bytes);
    vk_mirror_landed(addr, size);
}
/* owners dropped with no landing (an unmap, a remap, a decommit): the GPU mirror's copy of those pages is no longer
   guest memory's, and its next binding of them fills them again, as a landing has it. A page the mirror owns leaves
   its granule's check whole (mirror.c), which no other change of these pages would end */
static void own_dropped(u64 addr, u64 size) { vk_mirror_landed(addr, size); }

int vk_own_init(void) {
    /* vk_lock and vk_unlock themselves, not g_vklock: the outermost unlock also signals what the hold's releases
       reached (vk_release_events) */
    OwnHooks h = {.land = own_land,
                  .land_start = own_land_start,
                  .land_end = own_land_end,
                  .retain = res_retain,
                  .release = res_release,
                  .busy_tid = lock_owner,
                  .lock = vk_lock,
                  .unlock = vk_unlock,
                  .landed = own_landed,
                  .dropped = own_dropped,
                  .writer = own_writer,
                  .wait = own_wait,
                  .resolve = vk_rt_watch_hit,
                  .refresh = vk_rt_watch_hit,
                  .claim = own_claim};
    g_owned.traps = !own_init(&h);
    return g_owned.traps ? 0 : -1;
}

void vk_own_res_wrote(VkOwnRes* r) {
    r->used = g_vk_present;
    /* atomic: own_writer reads it on a thread without vk_lock */
    if (atomic_load_explicit(&r->written, memory_order_relaxed) < vk_submit_number())
        atomic_store_explicit(&r->written, vk_submit_number(), memory_order_release);
    atomic_store_explicit(&r->serial, (atomic_fetch_add_explicit(&g_owned.serial, 1, memory_order_relaxed) + 1),
                          memory_order_release);
}

int vk_own_watch(u64 cpu, u64 size) {
    /* an import reads and writes those pages in place: a trap would take them from it */
    if (!g_owned.traps || !size || vk_uma_held(cpu, size)) return -1;
    vk_uma_owning(cpu, size); /* what reads the pages in place runs and goes before the trap */
    pend_lock();              /* as own_publish: a change of guest memory without vk_lock finds no entry there */
    int r = own_watch(cpu, size);
    pend_unlock();
    return r;
}

static void own_publish(u64 cpu, u64 size, VkOwnRes* r, u64 off, int in_place) {
    OwnRange o;
    u8* d;
    if (!size) return;
    /* a landing's publish, valued in bytes; the text says which path made it necessary */
    SNX_ZONE(z, "vk mem landing publish");
    SNX_ZONE_VALUE(z, size);
    static const char k_bytes[] = "vk mem landing bytes";
    if (in_place >= 0)
        vk_own_res_wrote(r); /* every producer records its writes before it publishes them */
    else
        atomic_store_explicit(&r->serial, (atomic_fetch_add_explicit(&g_owned.serial, 1, memory_order_relaxed) + 1),
                              memory_order_release);
    in_place = in_place > 0;
    vk_rt_before_publish_cpu(cpu, size);
    gpu_mem_written_cpu(cpu, size); /* what is built from the bytes (textures, buffers) takes the new ones */
    /* Unified memory, over memory an import holds: the result is copied there on the GPU, in stream order, with no
       owner and no trap (each trap let go of the imports over its pages, and the next use made them again). Older
       owners of the bytes go: they are written over whole */
    if (in_place && vk_uma_held(cpu, size)) {
        own_drop(cpu, size);
        if (vk_uma_publish(cpu, size, r->buf, off)) {
            SNX_ZONE_TEXT(z, "UMA holds %llu bytes, copied into its import", (unsigned long long)size);
            SNX_PLOT(k_bytes, (double)size);
            return;
        }
    }
    if (g_owned.traps) vk_uma_owning(cpu, size); /* what reads the pages in place runs and goes before the trap */
    /* under pend_lock: a change of guest memory that skips vk_lock found no owner on its pages, and none comes until
       it ends (vkr_mapping_begin) */
    pend_lock();
    int owned = g_owned.traps && !own_begin(cpu, size, r, off);
    pend_unlock();
    if (owned) {
        if (in_place >= 0) early_copy(cpu, size, r, off); /* its writes are in the submission being recorded */
        SNX_ZONE_TEXT(z, "a trap owns %llu bytes, no landing", (unsigned long long)size);
        return;
    }
    /* not trapped here: older owners of the bytes let go of them, and the result lands now */
    own_drop(cpu, size);
    /* the runtime's own publish: it says nothing about the CPU reading those bytes (see land_ctx) */
    SNX_ZONE_TEXT(z, "no trap, %llu bytes land through GP now", (unsigned long long)size);
    SNX_PLOT(k_bytes, (double)size);
    SNX_ZONE_END(z); /* own_acquire and the landing can wait for the GPU; not part of this zone */
    /* Other owned bytes can still trap the edge pages after this range is
       dropped. Land those neighbors before the fallback writes through GP. */
    own_acquire(cpu, size);
    o.addr = cpu;
    o.size = size;
    o.res = r;
    o.res_off = off;
    d = GP(cpu);
    own_land(&o, &d, 1, OWN_BY_RUNTIME);
}
void vk_own_publish(u64 cpu, u64 size, VkOwnRes* r, u64 off) { own_publish(cpu, size, r, off, 1); }
void vk_own_publish_owned(u64 cpu, u64 size, VkOwnRes* r, u64 off) { own_publish(cpu, size, r, off, 0); }
void vk_own_publish_submitted(u64 cpu, u64 size, VkOwnRes* r, u64 off) { own_publish(cpu, size, r, off, -1); }

/* ---------------------------------------------------------------- reading guest memory without landing it */
/* the guest bytes read for the GPU since the last statistics line, by where they came from (VK_GREAD_*): [0] into host
   staging (vk_guest_read), [1] into device memory (guest_read). Under vk_lock */
static u64 g_gread[2][VK_GREAD_N];
/* n bytes at guest cpu that the CPU copied: and of them those a region of unified memory holds (vk_uma_held), which
   the GPU could have read there */
static void gread_cpu(int side, u64 cpu, u64 n) {
    g_gread[side][VK_GREAD_CPU] += n;
    if (vk_uma() && vk_uma_held(cpu, n)) g_gread[side][VK_GREAD_CPU_REGION] += n;
}
void vk_guest_read_counts(u64 counts[2][VK_GREAD_N]) {
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < VK_GREAD_N; k++) counts[s][k] = g_gread[s][k];
}
static void gread_log(void) {
    static const char* const side[2] = {"for host staging", "for device memory"};
    const double kb = 600.0 * 1024.0; /* KB a present over the 600 */
    for (int s = 0; s < 2; s++) {
        const u64* c = g_gread[s];
        LOG("vulkan: guest reads %s over the last 600 presents, KB a present: %.1f copied by the CPU (%.1f of them in a "
            "region), %.1f copied on the GPU from owners, %.1f from regions, %.1f from writes in place pending, %.1f "
            "from pending results, %.1f bound in place, %.1f zero\n",
            side[s], (double)c[VK_GREAD_CPU] / kb, (double)c[VK_GREAD_CPU_REGION] / kb, (double)c[VK_GREAD_OWNERS] / kb,
            (double)c[VK_GREAD_REGIONS] / kb, (double)c[VK_GREAD_PENDING] / kb, (double)c[VK_GREAD_RESULTS] / kb,
            (double)c[VK_GREAD_BOUND] / kb, (double)c[VK_GREAD_ZERO] / kb);
    }
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < VK_GREAD_N; k++) g_gread[s][k] = 0;
}
/* GPU writes before reading what they wrote (before), the reads' copies before what consumes them (after) */
static void read_barrier(VkCommandBuffer cb, int before) {
    /* an indirect copy's reads and writes are in its own stage: the MEMORY access types cover them in any stage */
    VkPipelineStageFlags2 copy =
        VK_PIPELINE_STAGE_2_COPY_BIT | (g_vk.has_copy_indirect ? VK_PIPELINE_STAGE_2_COPY_INDIRECT_BIT_KHR : 0);
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = before ? vk_buffer_stages() : copy,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = before ? copy : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}

/* a result's buffer's device address, asked for once */
static VkDeviceAddress res_addr(VkOwnRes* r) {
    if (!r->addr) {
        VkBufferDeviceAddressInfo i = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = r->buf};
        r->addr = vkGetBufferDeviceAddress(g_vk.dev, &i);
    }
    return r->addr;
}

/* A dispatch's read of results in place (vk_guest_read_inplace): the runs noted, from base in the buffer read, and
   whether the barrier before the dispatch's reads of them is recorded. Copied into staging and read from there, a
   texture load's results went out to host memory and back over PCIe, 17 to 38 MB a frame in one game's race */
typedef struct {
    VkInRun* r;
    u32 n, max;
    u64 base;
    int barrier;
} InPlace;
u64 g_vk_inplace_runs, g_vk_inplace_bytes;
/* the results' writes, by any stage that writes buffers, before a dispatch's reads of them by address */
static void inplace_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = vk_buffer_stages(),
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}
/* a result's run, its bytes at at in the buffer read, noted for an in-place read: 0 when ip takes no more, or the
   result has no address of ours, or the run is not in whole 16 byte units. The result is held until the read is
   recorded (res_hold) */
static int inplace_note(InPlace* ip, VkOwnRes* res, u64 res_off, u64 at, u64 size) {
    u64 to = at - ip->base;
    if (ip->n == ip->max || res->borrowed || res->host || ((to | res_off | size) & 15) || (to + size) >> 36) return 0;
    VkDeviceAddress a = res_addr(res) + res_off; /* the shader reads it 16 bytes at a time (buffer_reference_align) */
    if ((a & 15) || !res_hold(res)) return 0;
    if (!ip->barrier) inplace_barrier(vk_cmd());
    ip->barrier = 1;
    ip->r[ip->n++] = (VkInRun){(u32)(to >> 4), (u32)((to + size) >> 4), a};
    res->used = g_vk_present;
    g_vk_inplace_runs++;
    g_vk_inplace_bytes += size;
    return 1;
}

/* A run a region holds is copied from there on the GPU where the copies' barriers are recorded
   anyway, or where it is this many bytes or more: under it, the CPU's copy takes less time than the barrier after the
   copies (READ_COPIED) costs the GPU. A load's whole range is bound where it is instead (vk_guest_in_place) */
#define REGION_COPY_MIN (64u << 10)
/* cpu_read's *any: the barrier before the copies is recorded (after the GPU's writes that they read), and copies are
   recorded (the caller records the barrier after them) */
enum { READ_ORDERED = 1, READ_COPIED = 2 };
/* guest CPU bytes [cpu, cpu + size) into host, and the owned parts copied by the GPU into buf at at (host's place
   there) over them, or noted in ip to be read in place: the copies recorded so far, their barriers left to the caller
   (*any, READ_*). A run a region holds is copied from there (REGION_COPY_MIN) */
static void cpu_read(u64 cpu, u64 size, u8* host, VkBuffer buf, u64 at, int* any, InPlace* ip) {
    OwnRange o[16];
    u32 n, i;
    VkBuffer ib;
    u64 io;
    /* written in place (uma.c) by a submit not done yet: copied from the import on the GPU, after the write. Never
       GPU-owned as well: those are written over whole */
    if (vk_uma_copy_pending(cpu, size, &ib, &io)) {
        VkBufferCopy c = {io, at, size};
        if (!(*any & READ_ORDERED)) read_barrier(vk_cmd(), 1);
        *any = READ_ORDERED | READ_COPIED;
        rec_CmdCopyBuffer(vk_cmd(), ib, buf, 1, &c);
        g_gread[0][VK_GREAD_PENDING] += size;
        return;
    }
    vk_uma_mem_need_cpu(cpu, size); /* across imports: waited for */
    /* in a region, with no page whose newer bytes are elsewhere: the read in place's barrier orders it after the writes
       in place (uma.c's order), the only GPU writes there */
    if ((*any || size >= REGION_COPY_MIN) && vk_uma() && vk_uma_read_cpu(cpu, size, 1, 1, &ib, &io)) {
        VkBufferCopy c = {io, at, size};
        *any |= READ_COPIED;
        rec_CmdCopyBuffer(vk_cmd(), ib, buf, 1, &c);
        g_gread[0][VK_GREAD_REGIONS] += size;
        return;
    }
    if (!own_any()) {
        memcpy(host, GP(cpu), (size_t)size);
        gread_cpu(0, cpu, size);
        return;
    }
    n = own_stage(cpu, size, host, o, sizeof o / sizeof *o);
    if (n > sizeof o / sizeof *o) { /* more owners than noted: they land, and the bytes are read as they are then */
        own_acquire(cpu, size);
        memcpy(host, GP(cpu), (size_t)size);
        gread_cpu(0, cpu, size);
        return;
    }
    u64 owned = 0; /* the CPU's bytes between the owners' are in no region: an import takes no trapped page */
    for (i = 0; i < n; i++) {
        VkBufferCopy c = {o[i].res_off, at + (o[i].addr - cpu), o[i].size};
        if (ip && inplace_note(ip, (VkOwnRes*)o[i].res, c.srcOffset, c.dstOffset, c.size)) continue;
        if (!(*any & READ_ORDERED)) read_barrier(vk_cmd(), 1);
        *any = READ_ORDERED | READ_COPIED;
        rec_CmdCopyBuffer(vk_cmd(), ((const VkOwnRes*)o[i].res)->buf, buf, 1, &c);
        ((VkOwnRes*)o[i].res)->used = g_vk_present;
        owned += o[i].size;
    }
    g_gread[0][VK_GREAD_OWNERS] += owned;
    g_gread[0][VK_GREAD_CPU] += size - owned;
}
void vk_cpu_read(u64 cpu, u64 size, u8* host, VkBuffer buf, u64 at) {
    int any = 0;
    cpu_read(cpu, size, host, buf, at, &any, NULL);
    if (any) read_barrier(vk_cmd(), 0);
}
/* A run of guest memory [cpu, cpu + n) with writes in place pending (uma.c) that no one import holds whole: its first
   part, up to an import's edge, so that each part is copied from its import on the GPU after the write, or read as
   before where no import is, with nothing to wait for. A logged result written back across an import's edge
   (buffer.c's write-back) left such runs, and waiting for its submit there took all the work recorded before it */
static u64 inplace_part(u64 cpu, u64 n) {
    int held;
    if (!vk_uma_pending_cpu(cpu, n) || vk_uma_held(cpu, n)) return n;
    u64 part = vk_uma_import_part(cpu, n, &held);
    return part ? part : n;
}
/* Guest [gva, gva + size) into buf at at on the GPU, in stream order, when draws' or dispatches' results the renderer
   has pending lie over it (buffer.c): from where they are, composed over guest memory there when they cover only part
   of it (vk_buf_copy). Landing them first, as gmem_need does, finished all the work recorded when one was in the
   submission being recorded: one battle's texture checks and uploads of linear textures that a draw had just written
   made about 2200 such finishes in 600 presents. In-place writes there too (uma.c) are copied from their import under
   the results (vk_buf_copy). 0 when there are none */
static int pending_gpu_read(u64 gva, u64 size, VkBuffer buf, u64 at) {
    VkBuffer sb;
    u64 so;
    if (!vk_buf_pending(gva, size)) return 0;
    vk_render_end(); /* copies happen outside rendering passes */
    if (!vk_buf_copy(gva, size, &sb, &so)) return 0;
    VkCommandBuffer cb = vk_cmd();
    VkBufferCopy c = {so, at, size};
    read_barrier(cb, 1); /* the results' writers first */
    rec_CmdCopyBuffer(cb, sb, buf, 1, &c);
    /* before what reads buf there, and what writes over it (vk_rt_overlay), as guest_read's own copies */
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &dep);
    return 1;
}
int vk_guest_in_place(u64 gva, u64 size, u64 align, VkBuffer* buf, u64* off) {
    u64 cpu = 0;
    if (!size || !vk_uma() || gmmu_run(gva, size, &cpu) < size || !vk_uma_read_cpu(cpu, size, align, 1, buf, off))
        return 0;
    if (g_settings.draw_state_check) { /* the buffer cache's own checks, which the region's blk bits stand for */
        const char* why = vk_buf_fast_why(gva, size, *buf, *off);
        if (why) vk_uma_check_diff(why, gva, size);
    }
    g_gread[0][VK_GREAD_BOUND] += size;
    return 1;
}
static void host_read(u64 gva, u64 size, u8* host, VkBuffer buf, u64 at, InPlace* ip) {
    u64 o = 0;
    int any = 0;
    if (!size) return;
    if (pending_gpu_read(gva, size, buf, at)) {
        g_gread[0][VK_GREAD_RESULTS] += size;
        return;
    }
    vk_uma_gpu_reads(1);  /* the in-place writes there are copied below (cpu_read), not waited for */
    gmem_need(gva, size); /* results the renderer has pending there land first, as gmem_read has them */
    vk_uma_gpu_reads(0);
    while (o < size) {
        u64 cpu = 0, n = gmmu_run(gva + o, size - o, &cpu);
        if (!n) { /* an unmapped page reads as zero */
            n = 0x1000 - ((gva + o) & 0xFFF);
            if (n > size - o) n = size - o;
            memset(host + o, 0, (size_t)n);
            g_gread[0][VK_GREAD_ZERO] += n;
        } else {
            n = inplace_part(cpu, n);
            cpu_read(cpu, n, host + o, buf, at + o, &any, ip);
        }
        o += n;
    }
    if (any) read_barrier(vk_cmd(), 0);
}
void vk_guest_read(u64 gva, u64 size, u8* host, VkBuffer buf, u64 at) { host_read(gva, size, host, buf, at, NULL); }
u32 vk_guest_read_inplace(u64 gva, u64 size, u8* host, VkBuffer buf, u64 at, VkInRun* runs, u32 max) {
    InPlace ip = {runs, 0, max, at, 0};
    host_read(gva, size, host, buf, at, g_vk.has_inplace_loads ? &ip : NULL);
    return ip.n;
}

/* Device memory's side of vk_guest_read: the guest bytes [gva, gva + size) into buf at at, which the CPU doesn't map.
   The CPU's bytes are staged and copied, the owned ones copied from their owners, unmapped pages read as zero. A run
   that a region of unified memory holds is copied from there on the GPU, with no staging: the barrier of its read in
   place puts it after the writes in place, the only GPU writes there. Staged
   into host memory and read there by the GPU, a copy engine's spans went over PCIe several times: the GPU copied its
   own results and render targets out to host memory and read them back, about 34 MB a frame in one game's menu.
   Staging is taken at the range's first CPU byte, so a range its owners hold whole takes none: in a texture test 60% of
   the copy engine's destinations were such, and the pooled staging chunks they took and never wrote cost the CPU a
   page fault per page when it first wrote there, 7 ms for an 8 MiB copy source */
typedef struct {
    VkBuffer sb, buf; /* the staging copied from, holding the range's byte o at so + o */
    u64 so, at, size;
    u8* p; /* the staging, NULL until a CPU byte needs it */
    VkBufferCopy r[64];
    u32 n;
    int ahead; /* recorded in the submission's upload command buffer (vk_guest_read_ahead) */
} DevRead;
/* where a read's copies go: ahead of the submission's work, after the writes of earlier submissions, or here */
static VkCommandBuffer dev_cmd(int ahead) { return ahead ? vk_cmd_upload(1) : vk_cmd(); }
static u8* dev_staging(DevRead* d) {
    if (!d->p) d->p = vk_stage_upload(d->size, 16, &d->sb, &d->so);
    return d->p;
}
/* the bytes of a run own_lookup's k owners hold, sorted, apart and clipped to it */
static u64 owned_bytes(const OwnRange* r, u32 k) {
    u64 n = 0;
    for (u32 i = 0; i < k; i++) n += r[i].size;
    return n;
}
static void dev_flush(DevRead* d) {
    if (d->n) rec_CmdCopyBuffer(dev_cmd(d->ahead), d->sb, d->buf, d->n, d->r);
    d->n = 0;
}
/* the staged bytes [o, o + n) of the range go to buf */
static void dev_gap(DevRead* d, u64 o, u64 n) {
    if (!n) return;
    if (d->n && d->r[d->n - 1].dstOffset + d->r[d->n - 1].size == d->at + o) { /* after the last one */
        d->r[d->n - 1].size += n;
        return;
    }
    if (d->n == sizeof d->r / sizeof *d->r) dev_flush(d);
    d->r[d->n++] = (VkBufferCopy){d->so + o, d->at + o, n};
}
/* whether every byte of guest [gva, gva + size) was written before the submission being recorded: none by an in-place
   write of a submit not done yet (uma.c), and the owners' last writes recorded in earlier submissions
   (VkOwnRes.written), no more of them than guest_read notes at a time */
static int read_settled(u64 gva, u64 size) {
    u64 now = vk_submit_number();
    for (u64 o = 0; o < size;) {
        u64 cpu = 0, n = gmmu_run(gva + o, size - o, &cpu);
        OwnRange r[16];
        u32 k;
        if (!n) {
            n = 0x1000 - ((gva + o) & 0xFFF);
            if (n > size - o) n = size - o;
        } else if (vk_uma_pending_cpu(cpu, n))
            return 0;
        else if (own_any() && (k = own_lookup(cpu, n, r, sizeof r / sizeof *r)) > 0) {
            if (k > sizeof r / sizeof *r) return 0;
            for (u32 i = 0; i < k; i++)
                if (atomic_load_explicit(&(((const VkOwnRes*)r[i].res)->written), memory_order_relaxed) >= now)
                    return 0;
        }
        o += n;
    }
    return 1;
}
/* vk_guest_read_device, and with ahead its copies in the submission's upload command buffer: 0 then, with nothing
   recorded, when a byte was written in this submission, which those copies would run before */
/* guest_read's copies from results into one buffer, gathered for one vkCmdCopyMemoryIndirectKHR: a publish read about
   35 results' bytes a time into its target's buffer, 750 copies a frame in one game's race, each a command of
   its own with the GPU's gaps between them */
#define COPY_BATCH 64
typedef struct {
    VkDeviceAddress dst; /* the destination buffer's address, 0: no batch, each copy at once */
    u32 n;
    VkCopyMemoryIndirectCommandKHR cmd[COPY_BATCH];
    VkBuffer src[COPY_BATCH];
    VkBufferCopy copy[COPY_BATCH];
} CopyBatch;
u64 g_vk_copy_indirect_cmds, g_vk_copy_indirect_copies; /* the indirect copies made, and the copies in them */
static void copy_batch_flush(CopyBatch* b, VkCommandBuffer cb, VkBuffer buf) {
    VkBuffer lb;
    u64 lo;
    u8* list = b->n > 1 ? vk_stage(b->n * sizeof *b->cmd, 16, &lb, &lo) : NULL;
    if (list) {
        VkBufferDeviceAddressInfo i = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = lb};
        VkCopyMemoryIndirectInfoKHR ci = {
            .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_INDIRECT_INFO_KHR,
            /* both ends are results of our own in the device's memory: one in system memory, or the mirror's, is
               copied as before (guest_read) */
            .srcCopyFlags = VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR,
            .dstCopyFlags = VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR,
            .copyCount = b->n,
            .copyAddressRange = {vkGetBufferDeviceAddress(g_vk.dev, &i) + lo, b->n * sizeof *b->cmd, sizeof *b->cmd}};
        memcpy(list, b->cmd, b->n * sizeof *b->cmd);
        rec_CmdCopyMemoryIndirectKHR(cb, &ci);
        g_vk_copy_indirect_cmds++;
        g_vk_copy_indirect_copies += b->n;
    } else /* one copy, or no room in the staging ring: each as before */
        for (u32 k = 0; k < b->n; k++) rec_CmdCopyBuffer(cb, b->src[k], buf, 1, &b->copy[k]);
    b->n = 0;
}
/* A read of several runs into one buffer (vk_guest_read_owned_runs): one barrier before the first copy and one after
   the last, and the copies from results gathered across the runs. Each run's own barriers and indirect copy, a
   publish's hundreds of gaps took as many barriers as runs */
typedef struct {
    CopyBatch batch;
    int any; /* the barrier before the copies is recorded */
} RunsRead;
/* after a read's copies: before what reads the range, and what writes over it (render targets drawn over the bytes,
   vk_rt_overlay) */
static void read_end(VkCommandBuffer cb) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT |
                                          (g_vk.has_copy_indirect ? VK_PIPELINE_STAGE_2_COPY_INDIRECT_BIT_KHR : 0),
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &dep);
}
static int guest_read(u64 gva, u64 size, VkBuffer buf, u64 at, int ahead, VkDeviceAddress dst_addr, int last,
                      RunsRead* runs) {
    CopyBatch own;
    CopyBatch* bp = runs ? &runs->batch : &own;
    if (!runs) {
        own.dst = dst_addr;
        own.n = 0;
    }
    DevRead d = {.buf = buf, .at = at, .size = size, .ahead = ahead};
    u64 o = 0;
    int any = ahead || (runs && runs->any); /* the upload command buffer waits for earlier submissions' writes itself */
    if (!size) return 1;
    if (ahead && !read_settled(gva, size)) return 0;
    if (!ahead && pending_gpu_read(gva, size, buf, at)) {
        g_gread[1][VK_GREAD_RESULTS] += size;
        return 1;
    }
    if (!ahead) vk_render_end(); /* the copies happen outside rendering passes */
    vk_uma_gpu_reads(1);  /* the in-place writes there are copied below, not waited for */
    gmem_need(gva, size); /* results the renderer has pending there land first, as gmem_read has them */
    vk_uma_gpu_reads(0);
    while (o < size) {
        u64 cpu = 0, n = gmmu_run(gva + o, size - o, &cpu), io;
        OwnRange r[16];
        u32 k, i;
        VkBuffer ib;
        if (n) n = inplace_part(cpu, n);
        /* written in place (uma.c) by a submit not done yet: copied from the import on the GPU, after the write, or
           waited for where no import holds it */
        int inplace = n && vk_uma_copy_pending(cpu, n, &ib, &io);
        if (n && !inplace) vk_uma_mem_need_cpu(cpu, n);
        if (inplace) {
            VkBufferCopy c = {io, at + o, n};
            if (!any) read_barrier(vk_cmd(), 1);
            any = 1;
            rec_CmdCopyBuffer(dev_cmd(ahead), ib, buf, 1, &c);
            g_gread[1][VK_GREAD_PENDING] += n;
        } else if (!n) { /* an unmapped page reads as zero */
            n = 0x1000 - ((gva + o) & 0xFFF);
            if (n > size - o) n = size - o;
            if (!dev_staging(&d)) goto fail;
            memset(d.p + o, 0, (size_t)n);
            dev_gap(&d, o, n);
            g_gread[1][VK_GREAD_ZERO] += n;
        } else if (vk_uma() && vk_uma_read_cpu(cpu, n, 1, !ahead, &ib, &io)) {
            /* ahead, no write in place is pending there (read_settled): what wrote it before is done, and the upload
               command buffer runs before the submission's work, so no barrier */
            VkBufferCopy c = {io, at + o, n};
            rec_CmdCopyBuffer(dev_cmd(ahead), ib, buf, 1, &c);
            g_gread[1][VK_GREAD_REGIONS] += n;
        } else if (!own_any()) {
            if (!dev_staging(&d)) goto fail;
            memcpy(d.p + o, GP(cpu), (size_t)n);
            dev_gap(&d, o, n);
            gread_cpu(1, cpu, n);
        } else /* the owners as many at a time as r holds: landing the range when it had more waits for the GPU and
                  reads all its results back (a copy engine's tiles cut a surface into many owners). Staging only
                  where CPU bytes lie among them: a part its owners hold whole is copied from them alone */
            for (u64 c = cpu, part; c < cpu + n; c += part) {
                const u32 max = sizeof r / sizeof *r;
                part = cpu + n - c;
                if ((k = own_lookup(c, part, r, max)) > max) { /* to the last noted one's end */
                    part = r[max - 1].addr + r[max - 1].size - c;
                    k = own_lookup(c, part, r, max);
                }
                if (owned_bytes(r, k) != part) { /* CPU bytes among the owners': staged */
                    if (!dev_staging(&d)) goto fail;
                    k = own_stage(c, part, d.p + o + (c - cpu), r, max);
                }
                g_gread[1][VK_GREAD_OWNERS] += owned_bytes(r, k);
                g_gread[1][VK_GREAD_CPU] += part - owned_bytes(r, k);
                u64 from = c; /* the CPU's bytes between the owners, which own_stage staged; the owners' from them */
                for (i = 0; i < k; i++) {
                    VkBufferCopy cp = {r[i].res_off, at + o + (r[i].addr - cpu), r[i].size};
                    dev_gap(&d, o + (from - cpu), r[i].addr - from);
                    from = r[i].addr + r[i].size;
                    if (!any) read_barrier(vk_cmd(), 1);
                    any = 1;
                    VkOwnRes* res = (VkOwnRes*)r[i].res;
                    /* a buffer of our own, by its address: both ends and the size in 4 byte units, as an indirect
                       copy takes them, and held until the list is recorded */
                    VkDeviceAddress sa = bp->dst && !res->borrowed && !res->host ? res_addr(res) + cp.srcOffset : 0;
                    if (sa && !((sa | (bp->dst + cp.dstOffset) | cp.size) & 3) && res_hold(res)) {
                        if (bp->n == COPY_BATCH) copy_batch_flush(bp, dev_cmd(ahead), buf);
                        bp->cmd[bp->n] = (VkCopyMemoryIndirectCommandKHR){sa, bp->dst + cp.dstOffset, cp.size};
                        bp->src[bp->n] = res->buf;
                        bp->copy[bp->n++] = cp;
                    } else
                        rec_CmdCopyBuffer(dev_cmd(ahead), res->buf, buf, 1, &cp);
                    res->used = g_vk_present;
                }
                dev_gap(&d, o + (from - cpu), c + part - from);
            }
        o += n;
    }
    if (runs) runs->any = any;
    if (bp->n && last) copy_batch_flush(bp, dev_cmd(ahead), buf);
    dev_flush(&d);
    if (ahead || !last) return 1; /* the submission's work waits for the upload command buffer's copies (cmd.c
                                     submit); a run before a read's last has its barrier after that one */
    read_end(vk_cmd());
    return 1;
fail: /* the copies recorded before it write the buffer, which the caller lets go: the barrier after them, here or by
         the caller of a read of several runs */
    if (runs)
        runs->any = any;
    else if (any && !ahead)
        read_end(vk_cmd());
    return 0;
}
int vk_guest_read_device(u64 gva, u64 size, VkBuffer buf, u64 at) {
    return guest_read(gva, size, buf, at, 0, 0, 1, NULL);
}
int vk_guest_read_ahead(u64 gva, u64 size, VkBuffer buf, u64 at) {
    return guest_read(gva, size, buf, at, 1, 0, 1, NULL);
}
int vk_guest_read_owned(u64 gva, u64 size, VkOwnRes* dst, u64 at) {
    VkDeviceAddress a = g_vk.has_copy_indirect && !dst->borrowed && !dst->host ? res_addr(dst) : 0;
    return guest_read(gva, size, dst->buf, at, 0, a, 1, NULL);
}
int vk_guest_read_owned_runs(u64 gva, const u64 (*runs)[2], u32 n, VkOwnRes* dst) {
    RunsRead rr;
    rr.batch.dst = g_vk.has_copy_indirect && !dst->borrowed && !dst->host ? res_addr(dst) : 0;
    rr.batch.n = 0;
    rr.any = 0;
    /* what the renderer has pending over the runs lands first, all of it: a landing records commands, which the one
       barrier before the copies has to follow */
    vk_render_end();
    vk_uma_gpu_reads(1);
    for (u32 i = 0; i < n; i++) gmem_need(gva + runs[i][0], runs[i][1] - runs[i][0]);
    vk_uma_gpu_reads(0);
    u32 end = n; /* the last run with bytes, whose read flushes the copies and ends them with the barrier */
    while (end && runs[end - 1][1] == runs[end - 1][0]) end--;
    for (u32 i = 0; i < end; i++)
        if (!guest_read(gva + runs[i][0], runs[i][1] - runs[i][0], dst->buf, runs[i][0], 0, 0, i + 1 == end, &rr)) {
            /* the copies recorded so far write the buffer, which goes back to the pool: they end with the barrier */
            if (rr.any) read_end(vk_cmd());
            return 0;
        }
    return 1;
}
int vk_guest_read_seen(u64 gva, u64 size, VkBuffer buf, u64 at) {
    if (!vk_guest_read_device(gva, size, buf, at)) return 0;
    vk_rt_overlay(gva, size, buf, at);
    return 1;
}

void vk_guest_peek(u64 gva, void* dst, u64 size) {
    u8* d = (u8*)dst;
    u64 o = 0;
    if (!size) return;
    gmem_need(gva, size);
    while (o < size) {
        u64 cpu = 0, n = gmmu_run(gva + o, size - o, &cpu);
        if (!n) {
            n = 0x1000 - ((gva + o) & 0xFFF);
            if (n > size - o) n = size - o;
            memset(d + o, 0, (size_t)n);
        } else if (!own_any())
            memcpy(d + o, GP(cpu), (size_t)n);
        else {
            OwnRange r[16];
            u32 k = own_stage(cpu, n, d + o, r, sizeof r / sizeof *r), i;
            if (k > sizeof r / sizeof *r) {
                own_acquire(cpu, n);
                memcpy(d + o, GP(cpu), (size_t)n);
            } else
                for (i = 0; i < k; i++) { /* the owner's identity in place of its stale bytes: its serial, as a new
                                             owner can take a freed one's address (a glyph cache's copies did) */
                    u64 serial = atomic_load_explicit(&((const VkOwnRes*)r[i].res)->serial, memory_order_acquire);
                    u64 tag = (serial * 0x9E3779B97F4A7C15ull) ^ (r[i].res_off - (r[i].addr - cpu));
                    u8* t = d + o + (r[i].addr - cpu);
                    u8 word[8];
                    u64 b = 0;
                    /* the byte at an address is the tag's byte of the address modulo 8: whole words of guest memory
                       take the tag's 8 bytes in order, written as such. Byte by byte took a third of the GPU
                       thread hashing textures over GPU-owned memory (mem_hash) */
                    for (u32 j = 0; j < 8; j++) word[j] = (u8)(tag >> (8 * j));
                    for (; b < r[i].size && ((r[i].addr + b) & 7); b++) t[b] = word[(r[i].addr + b) & 7];
                    for (; b + 8 <= r[i].size; b += 8) memcpy(t + b, word, 8);
                    for (; b < r[i].size; b++) t[b] = word[(r[i].addr + b) & 7];
                }
        }
        o += n;
    }
}

/* the one GPU-owned range that holds all of guest bytes [gva, gva + size), one run of the GPU's page table, in *r: 0
   when there is none */
static int owned_one(u64 gva, u64 size, OwnRange* r) {
    OwnRange o[16]; /* an owner is an entry for each run of the pool's pages under it (own_begin): its pieces are one */
    u64 cpu = 0, at;
    u32 n;
    if (gmmu_run(gva, size, &cpu) != size || !(n = own_lookup(cpu, size, o, 16)) || n > 16) return 0;
    at = cpu;
    for (u32 i = 0; i < n; i++) {
        if (o[i].addr != at || o[i].res != o[0].res || o[i].res_off != o[0].res_off + (at - cpu)) return 0;
        at += o[i].size;
    }
    if (at != cpu + size) return 0;
    *r = o[0];
    r->size = size;
    return 1;
}
int vk_guest_owned_whole(u64 gva, u64 size, u64* tag) {
    OwnRange r;
    if (!size || !own_any()) return 0;
    gmem_need(gva, size);
    if (!owned_one(gva, size, &r)) return 0;
    *tag = atomic_load_explicit(&((const VkOwnRes*)r.res)->serial, memory_order_acquire) * 0x9E3779B97F4A7C15ull ^
           r.res_off;
    return 1;
}
int vk_guest_owned_by(u64 gva, u64 size, u64 serial) {
    OwnRange r;
    return size && serial && own_any() && owned_one(gva, size, &r) &&
           atomic_load_explicit(&((const VkOwnRes*)r.res)->serial, memory_order_acquire) == serial;
}

VkPipelineStageFlags2 vk_buffer_stages(void) {
    return VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
           VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
           (g_vk.has_xfb ? VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT : 0) |
           (g_vk.has_cond_render ? VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT : 0) |
           (g_vk.has_copy_indirect ? VK_PIPELINE_STAGE_2_COPY_INDIRECT_BIT_KHR : 0);
}
void vk_all_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}
static _Atomic u64 g_all_barriers[VK_BAR_N];
void vk_all_barrier_by(int site, VkCommandBuffer cb) {
    atomic_fetch_add_explicit(&g_all_barriers[site], 1, memory_order_relaxed);
    vk_all_barrier(cb);
}
void vk_all_barrier_log(void) {
    static const char* const name[VK_BAR_N] = {"blit",      "buffer", "compute", "dma",     "nvdec",
                                               "pagespace", "render", "surface", "texture", "uma"};
    char line[400];
    size_t at = 0;
    for (u32 k = 0; k < VK_BAR_N; k++) {
        u64 n = atomic_exchange_explicit(&g_all_barriers[k], 0, memory_order_relaxed);
        if (!n || at >= sizeof line) continue;
        int w = snprintf(line + at, sizeof line - at, " %s %llu", name[k], (unsigned long long)n);
        if (w > 0) at += (size_t)w < sizeof line - at ? (size_t)w : sizeof line - at - 1;
    }
    if (at) LOG("vulkan: full barriers of the last 600 presents by file:%s\n", line);
}
void vk_copy_rows(VkCommandBuffer cb, VkBuffer src, u64 src_at, i64 src_pitch, VkBuffer dst, u64 dst_at, i64 dst_pitch,
                  u32 row_bytes, u32 rows) {
    VkBufferCopy c[64];
    u32 y = 0;
    if (!row_bytes) return;
    while (y < rows) {
        u32 n = 0;
        for (; y < rows && n < sizeof c / sizeof *c; y++, n++) {
            c[n].srcOffset = src_at + (u64)(y * src_pitch);
            c[n].dstOffset = dst_at + (u64)(y * dst_pitch);
            c[n].size = row_bytes;
        }
        rec_CmdCopyBuffer(cb, src, dst, n, c);
    }
}
