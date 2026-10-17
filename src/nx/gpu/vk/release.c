/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Releases the game can observe: a semaphore or report written for the CPU to poll, a syncpoint incremented. The
   console's GPU makes everything before a release visible in memory first, so storage results recorded before it
   land before it happens. Landing them with a vk_finish at each release left the GPU thread waiting for most of
   TotK's frame. A release now waits in a queue instead, gated on the submit that holds the work before it, and the
   GPU thread goes on recording. The release thread waits for that submit on the GPU, lands its results and performs
   the releases it gates, in order. Whoever needs a queued release sooner (a stream that reads the semaphore, or writes
   over it) performs it itself.
   The compute queue's results are waited for the same way (its batches, put aside, land in order). Storage images
   still land at the release (vk_gfx_image_land). */
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

typedef struct {
    u64 gate;  /* the graphics submit whose completion it waits for */
    u64 cgate; /* and the compute batch, 0 for none */
    u64 gva;   /* the payload's address, or 0 for a syncpoint increment */
    u32 id, len;
    u8 data[16];
    /* samples: a samples-passed report (query.c), whose value is the count of segments [qfirst, qend), known once the
       gate is done. data holds the timestamp after it */
    int samples;
    u64 qfirst, qend;
} Release;
#define RQ_N   1024 /* queued releases at most: a full queue performs its oldest at once */
#define SYNC_N 128  /* nvdrv's syncpoint ids */

static Release g_rq[RQ_N];
static u64 g_rhead, g_rtail; /* releases queued, performed */
static u32 g_rincr[SYNC_N];  /* increments queued per syncpoint */
static PMutex g_rlock = PMUTEX_INIT;
static PCond g_rcv;
static int g_rthread;

static void perform(Release* r) {
    if (r->samples) { /* the count, as the report's structure size says: one word, or the value and the timestamp */
        u64 v = vk_query_value(r->qfirst, r->qend);
        u32 w = (u32)v;
        if (r->len == 4)
            memcpy(r->data, &w, 4);
        else
            memcpy(r->data, &v, 8);
    }
    vk_result_begin();
    if (r->gva)
        gmem_write(r->gva, r->data, r->len);
    else
        syncpt_incr(r->id);
    vk_result_end();
}
/* lands the results of every submit up to gate and compute batch up to cgate (they have finished) and performs the
   releases they gate, in order */
static void release_through(u64 gate, u64 cgate) {
    vk_lock();
    vk_result_begin();
    vk_compute_land_through(cgate);
    vk_land_through(gate);
    vk_result_end();
    for (;;) {
        Release r;
        pm_lock(&g_rlock);
        if (g_rtail == g_rhead || g_rq[g_rtail % RQ_N].gate > gate || g_rq[g_rtail % RQ_N].cgate > cgate) {
            pm_unlock(&g_rlock);
            break;
        }
        r = g_rq[g_rtail % RQ_N];
        g_rtail++;
        if (!r.gva) {
            /* release_value sees the transfer from queued to completed atomically. */
            perform(&r);
            if (r.id < SYNC_N) g_rincr[r.id]--;
        }
        if (g_rtail == g_rhead) __atomic_store_n(&g_gpu_releases, 0, __ATOMIC_RELEASE);
        pm_unlock(&g_rlock);
        if (r.gva) perform(&r);
    }
    vk_unlock();
}
static void release_thread(void* p) {
    (void)p;
    for (;;) {
        u64 gate, cgate;
        pm_lock(&g_rlock);
        while (g_rtail == g_rhead) pc_wait(&g_rcv, &g_rlock, -1);
        gate = g_rq[g_rtail % RQ_N].gate;
        cgate = g_rq[g_rtail % RQ_N].cgate;
        pm_unlock(&g_rlock);
        vk_wait_submit(gate);
        vk_compute_wait(cgate);
        release_through(gate, cgate);
    }
}

/* r once everything recorded before it has landed: now, when nothing is pending or queued */
static void release(Release* r) {
    int queued;
    vk_lock();
    vk_gfx_image_land();
    r->cgate = vk_compute_put_aside();
    pm_lock(&g_rlock);
    queued = g_rtail != g_rhead;
    pm_unlock(&g_rlock);
    /* a samples-passed report always waits for the GPU: its count is the draws' */
    if (!r->samples && !vk_buf_pending_any() && !r->cgate && !queued) {
        perform(r);
        vk_unlock();
        return;
    }
    /* the work before it, submitted so its completion can be waited for; results of earlier submits are older */
    if (r->samples || vk_buf_pending_unsubmitted()) vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    r->gate = vk_last_submit();
    pm_lock(&g_rlock);
    if (g_rhead - g_rtail == RQ_N) { /* full: the oldest goes now */
        u64 oldest = g_rq[g_rtail % RQ_N].gate, coldest = g_rq[g_rtail % RQ_N].cgate;
        pm_unlock(&g_rlock);
        vk_wait_submit(oldest);
        vk_compute_wait(coldest);
        release_through(oldest, coldest);
        pm_lock(&g_rlock);
    }
    /* Gates cover the entire queued prefix even if ordinary reads already landed
       the batches belonging to earlier releases. */
    if (g_rhead != g_rtail) {
        const Release* prev = &g_rq[(g_rhead - 1) % RQ_N];
        if (r->gate < prev->gate) r->gate = prev->gate;
        if (r->cgate < prev->cgate) r->cgate = prev->cgate;
    }
    g_rq[g_rhead % RQ_N] = *r;
    g_rhead++;
    if (!r->gva && r->id < SYNC_N) g_rincr[r->id]++;
    __atomic_store_n(&g_gpu_releases, 1, __ATOMIC_RELEASE);
    if (!g_rthread) {
        pc_init(&g_rcv);
        g_rthread = 1;
        if (plat_thread(release_thread, NULL, (size_t)1 << 20)) fatal("could not start the GPU release thread");
    }
    pc_broadcast(&g_rcv);
    pm_unlock(&g_rlock);
    vk_unlock();
}

void vk_release_write(u64 gva, const void* data, u32 len) {
    Release r;
    memset(&r, 0, sizeof r);
    r.gva = gva;
    r.len = len < sizeof r.data ? len : (u32)sizeof r.data;
    memcpy(r.data, data, r.len);
    release(&r);
}
void vk_release_samples(u64 gva, u32 len, u64 first, u64 end, u64 ticks) {
    Release r;
    memset(&r, 0, sizeof r);
    r.gva = gva;
    r.len = len == 4 ? 4 : 16;
    r.samples = 1;
    r.qfirst = first;
    r.qend = end;
    memcpy(r.data + 8, &ticks, 8);
    release(&r);
}
void vk_release_incr(u32 id) {
    Release r;
    memset(&r, 0, sizeof r);
    r.id = id;
    release(&r);
}
/* Mapping changes hold vk_lock and drain the queue before changing translations.
   Thus these translations name the backing that was present at enqueue time. */
static int release_overlaps(const Release* r, u64 gva, u64 len) {
    if (!r->gva || !r->len || !len) return 0;
    if (gva >= r->gva ? gva - r->gva < r->len : r->gva - gva < len) return 1;
    for (u32 at = 0; at < r->len;) {
        u64 a = r->gva + at, cpu = gmmu_translate(a);
        u32 n = 4096 - (u32)(a & 4095);
        if (n > r->len - at) n = r->len - at;
        for (u64 off = 0; cpu && off < len;) {
            u64 b = gva + off, other = gmmu_translate(b), span = 4096 - (b & 4095);
            if (span > len - off) span = len - off;
            if (other && (other >= cpu ? other - cpu < n : cpu - other < span)) return 1;
            off += span;
        }
        at += n;
    }
    return 0;
}
/* Drain through the latest matching release. Gates are monotonic prefix maxima. */
static void release_match(u64 gva, u64 len, u32 id, int all) {
    u64 gate = 0, cgate = 0;
    int found = 0;
    if (vk_result_landing()) return;
    vk_lock();
    pm_lock(&g_rlock);
    for (u64 i = g_rtail; i < g_rhead; i++) {
        const Release* r = &g_rq[i % RQ_N];
        if (all || (len ? release_overlaps(r, gva, len) : !r->gva && r->id == id)) {
            gate = r->gate;
            cgate = r->cgate;
            found = 1;
        }
    }
    pm_unlock(&g_rlock);
    if (found) {
        vk_wait_submit(gate);
        vk_compute_wait(cgate);
        release_through(gate, cgate);
    }
    vk_unlock();
}
void vk_release_need(u64 gva, u64 len) {
    if (len) release_match(gva, len, 0, 0);
}
void vk_release_drain(void) { release_match(0, 0, 0, 1); }
void vk_release_retire(u32 id) { release_match(0, 0, id, 0); }
u32 vk_release_value(u32 id) {
    pm_lock(&g_rlock);
    u32 n = syncpt_read(id) + (id < SYNC_N ? g_rincr[id] : 0);
    pm_unlock(&g_rlock);
    return n;
}
