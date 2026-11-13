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
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"

#define CPU_READ_SLOTS 4096
static struct {
    int traps; /* own_init found traps: publishing owns, else it lands */
    struct {
        u64 granule;
        u32 present;
    } cpu_read[CPU_READ_SLOTS];
    /* Separate from frame pools and rings: frame result callbacks may acquire
       the owner being read back. Reused after its fence completes, under vk_lock. */
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkBuffer back;
    DevMem mem;
    u8* ptr;
    u64 cap;
    u64 serial; /* the last VkOwnRes.serial given */
    /* the results with a buffer of their own that are in use, and their buffers' bytes: any thread adds and takes */
    u64 live[VK_OWN_BY_N], live_bytes[VK_OWN_BY_N];
    /* those let go since the last statistics, by how many presents they were kept: under 8, under 120, under 1200,
       longer */
    u64 let_go[4];
    /* since the last statistics: the results vk_own_trim landed and their bytes, the freed buffers it let go */
    u64 trim_landed, trim_bytes, trim_pooled;
    /* since the last statistics, for a result the heap had no room for: the submissions ended for the freed buffers
       they held (vk_own_room), the pool's free buffers given back to the heap and their bytes, the freed buffers
       waited for */
    u64 room_splits, room_dropped, room_dropped_bytes, room_waits;
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
typedef struct {
    VkBuffer buf;
    DevMem mem;
    u64 cap;   /* the buffer's bytes, at least the resource's */
    u64 after; /* the graphics submission and the compute batch that may still use it: free once both are done */
    u64 cafter;
    u32 put;   /* the present it came back at: one no result took for OWN_IDLE presents goes (vk_own_trim) */
} OwnPoolEntry;
static OwnPoolEntry g_pool[OWN_POOL_MAX];
static u32 g_pool_n;
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
    if ((put = g_pool_n < OWN_POOL_MAX))
        g_pool[g_pool_n++] = (OwnPoolEntry){.buf = buf,
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
    if (!__atomic_load_n(&g_pool_n, __ATOMIC_RELAXED)) return e;
    OwnDone done = own_done();
    pm_lock(&g_pool_lk);
    for (u32 i = 0; i < g_pool_n; i++)
        if (g_pool[i].cap >= size && own_pool_free(&g_pool[i], done) &&
            (best < 0 || g_pool[i].cap < g_pool[best].cap))
            best = (i32)i;
    if (best >= 0) {
        e = g_pool[best];
        g_pool[best] = g_pool[--g_pool_n];
    }
    pm_unlock(&g_pool_lk);
    return e;
}

/* Room for a result the heap has none for: its owner makes it, as devmem.h asks of every owner.

   The pool's buffers the GPU is done with go back to the heap, all of them: none holds the result, or it had taken
   one, and their memory is what the heap lacks. They are free at once (nothing uses them). Their count */
static u32 own_pool_drop(void) {
    u32 n = 0;
    if (!__atomic_load_n(&g_pool_n, __ATOMIC_RELAXED)) return 0;
    OwnDone done = own_done();
    pm_lock(&g_pool_lk);
    for (u32 i = 0; i < g_pool_n;) {
        if (!own_pool_free(&g_pool[i], done)) {
            i++;
            continue;
        }
        g_owned.room_dropped_bytes += g_pool[i].cap;
        vk_mem_untrack(g_pool[i].mem);
        vk_devmem_buffer_destroy(g_pool[i].buf, g_pool[i].mem);
        g_pool[i] = g_pool[--g_pool_n];
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
    if (!__atomic_load_n(&g_pool_n, __ATOMIC_RELAXED)) return 0;
    vk_submit_progress(&submitted, &done.gfx);
    vk_compute_progress(&csubmitted, &done.compute);
    pm_lock(&g_pool_lk);
    for (u32 i = 0; i < g_pool_n; i++) {
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

/* At a job's start (a dispatch, a copy), before anything of it is set up: when device memory is short and the
   submission being recorded holds OWN_OPEN_MIN of freed result buffers, that submission ends here, so the GPU does it
   and the buffers come free for the job's own results. A game that loads records hundreds of megabytes of results into
   one submission, each replacing the one before, and none of their buffers could serve the next. With memory to spare
   the submission goes on: its results take new buffers */
#define OWN_OPEN_MIN (32ull << 20)
void vk_own_room(void) {
    if (!__atomic_load_n(&g_pool_n, __ATOMIC_RELAXED) || vk_rec_unsafe()) return;
    u64 now = vk_submit_number(), open = 0, usage, budget;
    pm_lock(&g_pool_lk);
    for (u32 i = 0; i < g_pool_n; i++)
        if (g_pool[i].after == now) open += g_pool[i].cap;
    pm_unlock(&g_pool_lk);
    if (open < OWN_OPEN_MIN) return;
    vk_mem_use(&usage, &budget);
    if (usage < budget / 5 * 4) return;
    vk_submit_split();
    if (vk_submit_number() != now) g_owned.room_splits++;
}

/* statistics: what the ledger's kept results are, the ones in use and the freed buffers that wait in the pool */
void vk_own_log(void) {
    u64 pooled = 0, waiting = 0;
    OwnDone done = own_done();
    pm_lock(&g_pool_lk);
    u32 n = g_pool_n;
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
        u64 count = __atomic_load_n(&g_owned.live[k], __ATOMIC_RELAXED),
            bytes = __atomic_load_n(&g_owned.live_bytes[k], __ATOMIC_RELAXED);
        live += count;
        if (!count) continue;
        int w = snprintf(line + at, sizeof line - at, "%s %llu of %s (%.1f MB)", at ? "," : "",
                         (unsigned long long)count, by[k], (double)bytes / 1048576.0);
        if (w > 0) at += (size_t)w < sizeof line - at ? (size_t)w : sizeof line - at - 1;
    }
    u64 gone[4];
    for (u32 k = 0; k < 4; k++) gone[k] = __atomic_exchange_n(&g_owned.let_go[k], 0, __ATOMIC_RELAXED);
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
    if (g_owned.room_splits || g_owned.room_dropped || g_owned.room_waits)
        LOG("vulkan: kept results short of device memory: %llu submissions ended for the freed buffers they held, "
            "%llu free buffers of the pool given back (%.1f MB), %llu freed buffers waited for\n",
            (unsigned long long)g_owned.room_splits, (unsigned long long)g_owned.room_dropped,
            (double)g_owned.room_dropped_bytes / 1048576.0, (unsigned long long)g_owned.room_waits);
    g_owned.room_splits = g_owned.room_dropped = g_owned.room_dropped_bytes = g_owned.room_waits = 0;
}

/* The kept results within the budget. A result is the only copy of the guest bytes it holds, so it stays until the
   CPU touches them or something writes over them. One game's copy engines move every tile of its streamed textures,
   and nothing ever reads the tiles on the CPU: in a race 2267 such results held 1046 MB of device memory that no
   limit reached, the caches were left 5.2 of 5.5 GB, and the trim made room by evicting textures that the game was
   using, 6517 of which it then loaded again.

   So when device memory is short, the results that nothing has written or read for OWN_IDLE presents land in guest
   memory, as the CPU's touch would land them, the ones left alone longest first. What they were made for has read
   them by then (a texture is built from its tile within a frame or two), and a later read finds the bytes in guest
   memory. A landing is a round trip to the GPU, so only a few land a present, here at the frame's boundary where no
   draw is being set up. With memory to spare nothing lands. The freed buffers of the pool are no one's bytes: one
   that no result took for OWN_IDLE presents goes at any time */
void vk_own_trim(void) {
    if (g_vk_present % 4) return;
    u64 usage, budget;
    vk_mem_use(&usage, &budget);
    /* with device memory short a free buffer waits OWN_IDLE_SHORT presents for a result, not OWN_IDLE: at ten presents
       a second the pool held 120 to 230 MB for twelve seconds that the heap had no room without */
    const u32 idle = usage < budget / 5 * 4 ? OWN_IDLE : OWN_IDLE_SHORT;
    OwnDone done = own_done();
    pm_lock(&g_pool_lk);
    for (u32 i = 0; i < g_pool_n;) {
        if ((u32)(g_vk_present - g_pool[i].put) < idle || !own_pool_free(&g_pool[i], done)) {
            i++;
            continue;
        }
        vk_mem_untrack(g_pool[i].mem);
        vk_devmem_buffer_destroy(g_pool[i].buf, g_pool[i].mem); /* free: the GPU is done with it */
        g_pool[i] = g_pool[--g_pool_n];
        g_owned.trim_pooled++;
    }
    pm_unlock(&g_pool_lk);
    u32 n = __atomic_load_n(&g_own.nown, __ATOMIC_ACQUIRE);
    if (!g_owned.traps || !n || usage < budget / 5 * 4) return; /* four fifths: before the caches' trim starts */
    u32 max = n + 64; /* more than there are: a publish may add some before the lookup */
    OwnRange* r = malloc((size_t)max * sizeof *r);
    if (!r) return;
    struct {
        u64 addr, size;
        u32 used;
    } pick[OWN_LAND_MAX];
    u32 np = 0, k = own_lookup(0, 1ull << 48, r, max);
    u64 now = vk_submit_number();
    for (u32 i = 0; i < k && i < max; i++) {
        const VkOwnRes* o = (const VkOwnRes*)r[i].res;
        /* not one of another's buffer (the mirror's), nor one whose writes are still being recorded */
        if (!o || o->borrowed || (u32)(g_vk_present - o->used) < OWN_IDLE || o->written >= now) continue;
        u32 at = np;
        if (np == OWN_LAND_MAX) { /* full: in place of the one used last, if this one is older */
            at = 0;
            for (u32 j = 1; j < np; j++)
                if ((i32)(pick[j].used - pick[at].used) > 0) at = j;
            if ((i32)(o->used - pick[at].used) >= 0) continue;
        } else
            np++;
        pick[at].addr = r[i].addr;
        pick[at].size = r[i].size;
        pick[at].used = o->used;
    }
    free(r);
    for (u32 i = 0; i < np; i++) { /* a landing takes its whole pages: one picked beside another may be gone by now */
        own_acquire(pick[i].addr, pick[i].size);
        g_owned.trim_landed++;
        g_owned.trim_bytes += pick[i].size;
    }
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
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = want,
                                 /* bound where it is for whatever reads the bytes it holds: a draw's vertices, indices and
                                    buffers (buffer.c buf_get_owned), a dispatch's (compute.c res_bind), VIC's job words;
                                    and what draws write there, transform feedback's captures too (buffer.c keep_new) */
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                          VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                          (g_vk.has_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0),
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        DevMemWant ai = {.at = DM_DEVICE};
        land_sharing(&bi, fam);
        if (size > g_vk.alloc_cap) {
            free(r);
            return NULL;
        }
        /* without room, the pool makes it: its free buffers go back to the heap; then a freed buffer of the size
           that the GPU still uses is waited for and taken; then all of those are, and go back too. Only the last
           failure is said */
        int made = vk_buffer_create(&bi, &ai, &r->buf, &r->mem, NULL) == VK_SUCCESS ||
                   (own_pool_drop() && vk_buffer_create(&bi, &ai, &r->buf, &r->mem, NULL) == VK_SUCCESS);
        if (!made && own_pool_await(want) && (e = own_pool_take(want)).buf) {
            r->buf = e.buf;
            r->mem = e.mem;
            r->size = e.cap;
        } else if (!made) {
            if (own_pool_await(0)) own_pool_drop();
            if (!VK_OK(vk_buffer_create(&bi, &ai, &r->buf, &r->mem, NULL))) {
                free(r);
                return NULL;
            }
            made = 1;
        }
        if (made) {
            vk_mem_track(r->mem, VK_OWN_OWNED);
            r->size = want;
        }
    }
    r->refs = 1;
    r->serial = __atomic_add_fetch(&g_owned.serial, 1, __ATOMIC_RELAXED);
    r->by = (u8)by;
    r->born = r->used = g_vk_present;
    __atomic_fetch_add(&g_owned.live[by], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_owned.live_bytes[by], r->size, __ATOMIC_RELAXED);
    return r;
}
static void res_retain(void* res) { __atomic_add_fetch(&((VkOwnRes*)res)->refs, 1, __ATOMIC_RELAXED); }
VkOwnRes* vk_own_res_borrow(VkBuffer buf) {
    VkOwnRes* r = (VkOwnRes*)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->buf = buf;
    r->size = ~0ull;
    r->refs = 1;
    r->borrowed = 1;
    r->serial = __atomic_add_fetch(&g_owned.serial, 1, __ATOMIC_RELAXED);
    return r;
}
static void res_release(void* res) {
    VkOwnRes* r = (VkOwnRes*)res;
    if (__atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (r->borrowed) {
        free(r);
        return;
    }
    u32 kept = g_vk_present - r->born;
    __atomic_fetch_sub(&g_owned.live[r->by], 1, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&g_owned.live_bytes[r->by], r->size, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_owned.let_go[kept < 8 ? 0 : kept < 120 ? 1 : kept < 1200 ? 2 : 3], 1, __ATOMIC_RELAXED);
    vk_lock();
    /* the frame recording may still copy from it: the pool keeps it from the next result until the GPU has done what
       is recorded now. A full pool has it destroyed with the frame, as before the pool */
    if (!own_pool_put(r->buf, r->mem, r->size)) vk_defer_buffer(r->buf, r->mem);
    vk_unlock();
    free(r);
}
void vk_own_res_ref(VkOwnRes* r) { res_retain(r); }
void vk_own_res_unref(VkOwnRes* r) { res_release(r); }

/* every write the GPU did so far visible to copies (before), the copies' writes to the host (after) */
static void land_barrier(VkCommandBuffer cb, int before) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    if (before) {
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
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
   written storage buffer on the GPU only where the CPU hasn't read it lately (vk_own_cpu_read). Direct-mapped by
   granule, each slot holding its granule's number, under vk_lock (own_land, vk_dispatch) */
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
static void land_prepare(u64 size) {
    if (!g_owned.pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .queueFamilyIndex = land_apart() ? g_vk.compute_family : g_vk.gfx_family};
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!VK_OK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_owned.pool)) ||
            !VK_OK(vkCreateFence(g_vk.dev, &fi, NULL, &g_owned.fence)))
            fatal("vulkan: could not create the ownership readback queue resources");
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = g_owned.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (!VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &g_owned.cb)))
            fatal("vulkan: could not allocate the ownership readback commands");
    }
    if (size <= g_owned.cap) return;
    u64 cap = 4096, limit = g_vk.alloc_cap / 4;
    while (cap < size && cap < limit) cap = cap > limit / 2 ? limit : cap * 2;
    if (cap > limit) cap = limit;
    if (size > cap) fatal("vulkan: ownership readback exceeds its allocation limit");
    if (g_owned.back) {
        vk_mem_untrack(g_owned.mem);
        vk_devmem_buffer_destroy(g_owned.back, g_owned.mem);
    }
    u32 fam[2];
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    land_sharing(&bi, fam);
    DevMemWant ai = {.at = DM_CACHED, .mapped = 1};
    DevMemInfo info;
    if (!VK_OK(vk_buffer_create(&bi, &ai, &g_owned.back, &g_owned.mem, &info)))
        fatal("vulkan: no host memory for ownership readback");
    vk_mem_track(g_owned.mem, VK_OWN_OWNED);
    g_owned.ptr = info.ptr;
    g_owned.cap = cap;
}

/* the submission a resource's writes are in (0: unstamped, everything submitted): own.c's writer, on a thread without
   vk_lock too */
static u64 own_writer(void* res) { return __atomic_load_n(&((const VkOwnRes*)res)->written, __ATOMIC_ACQUIRE); }
/* own.c's land uses a private reusable command pool and host-cached buffer.
   Frame result callbacks run only once own.c has removed the landing marks. */
static void own_land(const OwnRange* r, u8* const* dst, u32 n) {
    u64 cap = g_vk.alloc_cap / 4, done = 0;
    u32 i = 0;
    SNX_ZONE_V(z, "vk own land", n);
    vk_lock();
    u64 writer = 0;
    for (u32 k = 0; k < n; k++) {
        u64 w = own_writer(r[k].res);
        cpu_read_note(r[k].addr, r[k].size);
        if (w > writer) writer = w;
    }
    int apart = land_apart();
    VkQueue queue = apart ? g_vk.compute : g_vk.gfx;
    /* done on the GPU already (own_wait waited for it with no lock held): the copy waits for that submission's
       signal, which is there, and for nothing of the recording thread, whose queue calls it takes the queue's lock
       beside. Waiting for them held vk_lock while the recording thread waited for a draw's pipeline */
    u64 gpu_done = 0;
    int finished = writer && writer <= vk_last_submit() &&
                   VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, vk_gfx_timeline(), &gpu_done)) && gpu_done >= writer;
    /* the open commands go only when the writes are among them: on the graphics queue the copy follows them anyway */
    if (!finished && (!apart || !writer || writer > vk_last_submit()))
        vk_submit_for_readback();
    else if (!finished)
        vk_rec_sync(); /* the recorder's calls on the queue (the compute one may be its too) before the copy's */
    /* unstamped: everything submitted. Past the last submission: nothing was recording, so nothing of it was written
       there, and a wait for a value never signaled would never end */
    if (!writer || writer > vk_last_submit()) writer = vk_last_submit();
    while (i < n) {
        Piece pc[64];
        u32 np = 0, j;
        u64 total = 0;
        while (i < n && np < sizeof pc / sizeof *pc && total < cap) {
            u64 len = r[i].size - done < cap - total ? r[i].size - done : cap - total;
            pc[np].res = (const VkOwnRes*)r[i].res;
            pc[np].src = r[i].res_off + done;
            pc[np].at = total;
            pc[np].size = len;
            pc[np].dst = dst[i] + done;
            np++;
            total += len;
            done += len;
            if (done == r[i].size) {
                i++;
                done = 0;
            }
        }
        land_prepare(total);
        VkCommandBuffer cb = g_owned.cb;
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if (!VK_OK(vkResetCommandPool(g_vk.dev, g_owned.pool, 0)) ||
            !VK_OK(vkBeginCommandBuffer(cb, &bi))) fatal("vulkan: could not begin ownership readback");
        land_barrier(cb, 1);
        for (j = 0; j < np; j++) {
            VkBufferCopy c = {pc[j].src, pc[j].at, pc[j].size};
            vkCmdCopyBuffer(cb, pc[j].res->buf, g_owned.back, 1, &c);
        }
        land_barrier(cb, 0);
        VkCommandBufferSubmitInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
        /* on the compute queue, the writer's submission alone: its signal carries the writes to the copy */
        VkSemaphoreSubmitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                    .semaphore = vk_gfx_timeline(),
                                    .value = writer,
                                    .stageMask = VK_PIPELINE_STAGE_2_COPY_BIT};
        VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                            .waitSemaphoreInfoCount = (apart || finished) && writer ? 1u : 0u,
                            .pWaitSemaphoreInfos = &wi,
                            .commandBufferInfoCount = 1,
                            .pCommandBufferInfos = &ci};
        if (!VK_OK(vkEndCommandBuffer(cb)) || !VK_OK(vkResetFences(g_vk.dev, 1, &g_owned.fence)))
            fatal("vulkan: could not submit ownership readback");
        if (apart || finished) {
            if (!finished) vk_rec_queue(queue); /* after the recorder's compute submissions, as cs_submit's */
            vk_queue_lock(queue);
        }
        VkResult sr = vkQueueSubmit2(queue, 1, &si, g_owned.fence);
        if (apart || finished) vk_queue_unlock(queue);
        if (!VK_OK(sr)) fatal("vulkan: could not submit ownership readback");
        gpu_wait_note("GPU-owned memory readback", 0);
        int ok = vk_wait_fence(g_owned.fence);
        gpu_wait_note(NULL, 0);
        if (!ok) { /* a GPU fault (Xid 109's CTX SWITCH TIMEOUT, say): name the draw that hung, from its checkpoints */
            vk_checkpoint_dump("device lost");
        }
        if (!ok || !VK_OK(vk_devmem_invalidate(g_owned.mem, 0, total)))
            fatal("vulkan: could not complete ownership readback");
        for (j = 0; j < np; j++) memcpy(pc[j].dst, g_owned.ptr + pc[j].at, pc[j].size);
    }
    vk_unlock();
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

int vk_own_init(void) {
    /* vk_lock and vk_unlock themselves, not g_vklock: the outermost unlock also signals what the hold's releases
       reached (vk_release_events) */
    OwnHooks h = {.land = own_land,
                  .retain = res_retain,
                  .release = res_release,
                  .busy_tid = lock_owner,
                  .lock = vk_lock,
                  .unlock = vk_unlock,
                  .landed = own_landed,
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
    if (r->written < vk_submit_number()) __atomic_store_n(&r->written, vk_submit_number(), __ATOMIC_RELEASE);
    __atomic_store_n(&r->serial, __atomic_add_fetch(&g_owned.serial, 1, __ATOMIC_RELAXED), __ATOMIC_RELEASE);
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
    if (in_place >= 0)
        vk_own_res_wrote(r); /* every producer records its writes before it publishes them */
    else
        __atomic_store_n(&r->serial, __atomic_add_fetch(&g_owned.serial, 1, __ATOMIC_RELAXED), __ATOMIC_RELEASE);
    in_place = in_place > 0;
    vk_rt_before_write_cpu(cpu, size);
    gpu_mem_written_cpu(cpu, size); /* what is built from the bytes (textures, buffers) takes the new ones */
    /* Unified memory, over memory an import holds: the result is copied there on the GPU, in stream order, with no
       owner and no trap (each trap let go of the imports over its pages, and the next use made them again). Older
       owners of the bytes go: they are written over whole */
    if (in_place && vk_uma_held(cpu, size)) {
        own_drop(cpu, size);
        if (vk_uma_publish(cpu, size, r->buf, off)) return;
    }
    if (g_owned.traps) vk_uma_owning(cpu, size); /* what reads the pages in place runs and goes before the trap */
    /* under pend_lock: a change of guest memory that skips vk_lock found no owner on its pages, and none comes until
       it ends (vkr_mapping_begin) */
    pend_lock();
    int owned = g_owned.traps && !own_begin(cpu, size, r, off);
    pend_unlock();
    if (owned) return;
    /* not trapped here: older owners of the bytes let go of them, and the result lands now */
    own_drop(cpu, size);
    /* Other owned bytes can still trap the edge pages after this range is
       dropped. Land those neighbors before the fallback writes through GP. */
    own_acquire(cpu, size);
    o.addr = cpu;
    o.size = size;
    o.res = r;
    o.res_off = off;
    d = GP(cpu);
    own_land(&o, &d, 1);
}
void vk_own_publish(u64 cpu, u64 size, VkOwnRes* r, u64 off) { own_publish(cpu, size, r, off, 1); }
void vk_own_publish_owned(u64 cpu, u64 size, VkOwnRes* r, u64 off) { own_publish(cpu, size, r, off, 0); }
void vk_own_publish_submitted(u64 cpu, u64 size, VkOwnRes* r, u64 off) { own_publish(cpu, size, r, off, -1); }

/* ---------------------------------------------------------------- reading guest memory without landing it */
/* GPU writes before reading what they wrote (before), the reads' copies before what consumes them (after) */
static void read_barrier(VkCommandBuffer cb, int before) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = before ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_COPY_BIT,
                          .srcAccessMask = before ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          .dstStageMask = before ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = before ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_MEMORY_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}

/* guest CPU bytes [cpu, cpu + size) into host, and the owned parts copied by the GPU into buf at at (host's place
   there) over them: the copies recorded so far, their barriers left to the caller (*any set when there were some) */
static void cpu_read(u64 cpu, u64 size, u8* host, VkBuffer buf, u64 at, int* any) {
    OwnRange o[16];
    u32 n, i;
    VkBuffer ib;
    u64 io;
    /* written in place (uma.c) by a submit not done yet: copied from the import on the GPU, after the write. Never
       GPU-owned as well: those are written over whole */
    if (vk_uma_copy_pending(cpu, size, &ib, &io)) {
        VkBufferCopy c = {io, at, size};
        if (!*any) read_barrier(vk_cmd(), 1);
        *any = 1;
        rec_CmdCopyBuffer(vk_cmd(), ib, buf, 1, &c);
        return;
    }
    vk_uma_mem_need_cpu(cpu, size); /* across imports: waited for */
    if (!own_any()) {
        memcpy(host, GP(cpu), (size_t)size);
        return;
    }
    n = own_stage(cpu, size, host, o, sizeof o / sizeof *o);
    if (n > sizeof o / sizeof *o) { /* more owners than noted: they land, and the bytes are read as they are then */
        own_acquire(cpu, size);
        memcpy(host, GP(cpu), (size_t)size);
        return;
    }
    for (i = 0; i < n; i++) {
        VkBufferCopy c = {o[i].res_off, at + (o[i].addr - cpu), o[i].size};
        if (!*any) read_barrier(vk_cmd(), 1);
        *any = 1;
        rec_CmdCopyBuffer(vk_cmd(), ((const VkOwnRes*)o[i].res)->buf, buf, 1, &c);
        ((VkOwnRes*)o[i].res)->used = g_vk_present;
    }
}
void vk_cpu_read(u64 cpu, u64 size, u8* host, VkBuffer buf, u64 at) {
    int any = 0;
    cpu_read(cpu, size, host, buf, at, &any);
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
void vk_guest_read(u64 gva, u64 size, u8* host, VkBuffer buf, u64 at) {
    u64 o = 0;
    int any = 0;
    if (!size) return;
    vk_uma_gpu_reads(1);  /* the in-place writes there are copied below (cpu_read), not waited for */
    gmem_need(gva, size); /* results the renderer has pending there land first, as gmem_read has them */
    vk_uma_gpu_reads(0);
    while (o < size) {
        u64 cpu = 0, n = gmmu_run(gva + o, size - o, &cpu);
        if (!n) { /* an unmapped page reads as zero */
            n = 0x1000 - ((gva + o) & 0xFFF);
            if (n > size - o) n = size - o;
            memset(host + o, 0, (size_t)n);
        } else {
            n = inplace_part(cpu, n);
            cpu_read(cpu, n, host + o, buf, at + o, &any);
        }
        o += n;
    }
    if (any) read_barrier(vk_cmd(), 0);
}

/* Device memory's side of vk_guest_read: the guest bytes [gva, gva + size) into buf at at, which the CPU doesn't map.
   The CPU's bytes are staged and copied, the owned ones copied from their owners, unmapped pages read as zero. Staged
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
                if (((const VkOwnRes*)r[i].res)->written >= now) return 0;
        }
        o += n;
    }
    return 1;
}
/* vk_guest_read_device, and with ahead its copies in the submission's upload command buffer: 0 then, with nothing
   recorded, when a byte was written in this submission, which those copies would run before */
static int guest_read(u64 gva, u64 size, VkBuffer buf, u64 at, int ahead) {
    DevRead d = {.buf = buf, .at = at, .size = size, .ahead = ahead};
    u64 o = 0;
    int any = ahead; /* the upload command buffer waits for earlier submissions' writes itself */
    if (!size) return 1;
    if (ahead && !read_settled(gva, size)) return 0;
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
        } else if (!n) { /* an unmapped page reads as zero */
            n = 0x1000 - ((gva + o) & 0xFFF);
            if (n > size - o) n = size - o;
            if (!dev_staging(&d)) return 0;
            memset(d.p + o, 0, (size_t)n);
            dev_gap(&d, o, n);
        } else if (!own_any()) {
            if (!dev_staging(&d)) return 0;
            memcpy(d.p + o, GP(cpu), (size_t)n);
            dev_gap(&d, o, n);
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
                    if (!dev_staging(&d)) return 0;
                    k = own_stage(c, part, d.p + o + (c - cpu), r, max);
                }
                u64 from = c; /* the CPU's bytes between the owners, which own_stage staged; the owners' from them */
                for (i = 0; i < k; i++) {
                    VkBufferCopy cp = {r[i].res_off, at + o + (r[i].addr - cpu), r[i].size};
                    dev_gap(&d, o + (from - cpu), r[i].addr - from);
                    from = r[i].addr + r[i].size;
                    if (!any) read_barrier(vk_cmd(), 1);
                    any = 1;
                    rec_CmdCopyBuffer(dev_cmd(ahead), ((const VkOwnRes*)r[i].res)->buf, buf, 1, &cp);
                    ((VkOwnRes*)r[i].res)->used = g_vk_present;
                }
                dev_gap(&d, o + (from - cpu), c + part - from);
            }
        o += n;
    }
    dev_flush(&d);
    if (ahead) return 1; /* the submission's work waits for the upload command buffer's copies (cmd.c submit) */
    /* before what reads the range, and what writes over it (render targets drawn over the bytes, vk_rt_overlay) */
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(vk_cmd(), &dep);
    return 1;
}
int vk_guest_read_device(u64 gva, u64 size, VkBuffer buf, u64 at) { return guest_read(gva, size, buf, at, 0); }
int vk_guest_read_ahead(u64 gva, u64 size, VkBuffer buf, u64 at) { return guest_read(gva, size, buf, at, 1); }
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
                    u64 serial = __atomic_load_n(&((const VkOwnRes*)r[i].res)->serial, __ATOMIC_ACQUIRE);
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
    OwnRange o[2];
    u64 cpu = 0;
    if (gmmu_run(gva, size, &cpu) != size || own_lookup(cpu, size, o, 2) != 1 || o[0].addr != cpu || o[0].size != size)
        return 0;
    *r = o[0];
    return 1;
}
int vk_guest_owned_whole(u64 gva, u64 size, u64* tag) {
    OwnRange r;
    if (!size || !own_any()) return 0;
    gmem_need(gva, size);
    if (!owned_one(gva, size, &r)) return 0;
    *tag = __atomic_load_n(&((const VkOwnRes*)r.res)->serial, __ATOMIC_ACQUIRE) * 0x9E3779B97F4A7C15ull ^ r.res_off;
    return 1;
}
int vk_guest_owned_by(u64 gva, u64 size, u64 serial) {
    OwnRange r;
    return size && serial && own_any() && owned_one(gva, size, &r) &&
           __atomic_load_n(&((const VkOwnRes*)r.res)->serial, __ATOMIC_ACQUIRE) == serial;
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
