/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The queue from the GPU front end to the renderer's side (gpu_queue.h). A ring of records, each with its fence.
   The front end puts a record in and goes on. The records run in their order: with gpu_split on a thread of their
   own, the renderer thread, else on the front end's thread right after they are queued.

   A guest write of the front end (LOAD_CB's constant buffer data, inline-to-memory, a release) is a record, so a draw
   queued before it reads the memory from before it. The front end's own reads of memory wait for the fence of the
   last write queued over their pages (written[]), and no longer.

   One thread puts records in (the front end's) and one takes them out (the renderer thread), with no lock: each side
   publishes its position with a release store and reads the other's with an acquire load. A side with nothing to do
   sleeps on a word (plat_wait_word), and the other side wakes it only when it said so: the sleeper sets its flag, then
   looks again before it sleeps, and the waker publishes its position before it reads the flag */
#include "nx/gpu/gpu_queue.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "common/platform/thread.h"
#include "common/utilities/common.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"
#include "common/platform/time.h"

enum { GQ_WRITE, GQ_WRITE_CB, GQ_RELEASE, GQ_I2M_BLOCK, GQ_CALL, GQ_SKIP };
typedef struct {
    u32 kind;  /* GQ_ */
    u32 bytes; /* the record's bytes, this header and its payload */
    u64 fence; /* its sequence number */
    u64 gva;   /* the write's address (GQ_CALL: its function) */
    u64 size;  /* its bytes after the header (GQ_I2M_BLOCK: the upload's GpuUploadRegs, then its bytes) */
} GqRecord;
#define GQ_RING      (8u << 20)
#define GQ_ALIGN     32u     /* the header's size: the room left at the ring's end always takes a filler record */
#define GQ_SLEEP_NS  1000000 /* a sleeper looks again after this, whatever woke it */
/* written[]: for each slot of pages, the highest fence that wrote one of them. Two pages in one slot share it, so a
   read can wait longer than it needs, never less */
#define GQ_SLOT_BITS 16
static struct {
    u8* ring;
    GqRecord* open; /* the call between gq_call_begin and gq_call_end, in the ring */
    void* big;      /* or its bytes outside it, too many for the ring: the call runs from there */
    GqFn big_fn;
    u64 big_bytes;
    int started;    /* the renderer thread runs (gpu_split) */
    u64 queued;     /* the fence of the last record put in: the front end's alone */
    u64 written[1u << GQ_SLOT_BITS];
    /* the front end's side */
    _Alignas(64) _Atomic u64 head; /* bytes put in, from the start: modulo GQ_RING in the ring */
    _Atomic u32 work;              /* moves with each publish: the renderer thread sleeps on it */
    _Atomic int taker_sleeps;
    /* the renderer thread's side */
    _Alignas(64) _Atomic u64 tail; /* bytes taken out */
    _Atomic u64 done;              /* the fence of the last record that ran */
    _Atomic u32 progress;          /* moves with each record done: the front end sleeps on it */
    _Atomic int putter_sleeps;
} g_gq;
/* the front end's last guest writes, in their order, for its reads of guest memory (gq_read): a read waits only for
   the newest write over its bytes, or takes the bytes from that write's record. written[] is the fallback once the
   list lost a write that the renderer's side hasn't run */
#define GQ_RECENT 16384
typedef struct {
    u64 gva, size, fence;
    u64 at;  /* where the record starts, as head counts: its bytes stay until head passes it by GQ_RING */
    u32 fwd; /* the record's payload holds the bytes from gva on */
    u32 pad;
} GqRecent;
static GqRecent g_gq_recent[GQ_RECENT];
static u32 g_gq_recent_n;
static u64 g_gq_lost; /* the fence of the last write that fell out of the list */
static void gq_recent_add(u64 gva, u64 size, u64 fence, u64 at, u32 fwd) {
    GqRecent* e = &g_gq_recent[g_gq_recent_n % GQ_RECENT];
    if (g_gq_recent_n >= GQ_RECENT) g_gq_lost = e->fence;
    g_gq_recent_n++;
    *e = (GqRecent){gva, size, fence, at, fwd, 0};
}
static _Thread_local int t_gq_run;    /* this thread runs a record: its writes are the record's own */
static _Thread_local int t_gq_direct; /* this thread holds the renderer's lock (gq_direct): it runs renderer code */

static u32 gq_slot(u64 page) { return (u32)((page * 0x9E3779B97F4A7C15ull) >> (64 - GQ_SLOT_BITS)); }
static void gq_mark(u64 gva, u64 size, u64 fence) {
    if (!size) return;
    u64 first = gva >> 12, last = (gva + size - 1) >> 12;
    if (last - first >= (1u << GQ_SLOT_BITS)) last = first + (1u << GQ_SLOT_BITS) - 1; /* every slot already */
    for (u64 p = first; p <= last; p++) g_gq.written[gq_slot(p)] = fence;
}
static void gq_run(const GqRecord* r) {
    const u8* payload = (const u8*)(r + 1);
    t_gq_run = 1;
    switch (r->kind) {
        case GQ_WRITE: gmem_write_now(r->gva, payload, r->size); break;
        case GQ_WRITE_CB: gmem_write_cb_now(r->gva, payload, r->size); break;
        case GQ_RELEASE: gpu_release_write_now(r->gva, payload, (u32)r->size); break;
        case GQ_I2M_BLOCK: {
            GpuI2m t = {0};
            memcpy(&t.regs, payload, sizeof t.regs);
            t.inner_buffer = (u8*)payload + sizeof t.regs;
            t.copy_size = (u32)(r->size - sizeof t.regs);
            gpu_i2m_block_now(&t);
            break;
        }
        case GQ_CALL: {
            GqFn fn;
            memcpy(&fn, &r->gva, sizeof fn);
            fn(payload, r->size);
            break;
        }
        case GQ_SKIP: break;
        default: fatal("gpu: queue record of kind %u\n", r->kind);
    }
    t_gq_run = 0;
}
/* For the stats line: the front end's waits on the renderer thread, by what it waited for (a full ring, a drain, a
   queued write before a read), the
   drains asked and those that found every record run, and the renderer thread's records and its time in them. The
   front end adds the waits, the renderer thread its runs, and the present's thread takes them (gq_stats_log) */
enum { GQW_ROOM, GQW_DRAIN, GQW_READ }; /* gq_wait's: what the front end waits for */
enum {
    GQS_ROOM_N,
    GQS_ROOM_NS,
    GQS_DRAIN_N,
    GQS_DRAIN_NS,
    GQS_READ_N,
    GQS_READ_NS,
    GQS_DRAINS,
    GQS_DRAINS_IDLE,
    GQS_RUNS,
    GQS_RUN_BYTES,
    GQS_RUN_NS,
    GQS_N
};
static _Atomic u64 g_gqs[GQS_N];
static void gqs_add(int at, u64 v) { atomic_fetch_add_explicit(&g_gqs[at], v, memory_order_relaxed); }
void gq_stats_log(u64 presents) {
    if (!g_gq.started || !presents) return;
    u64 v[GQS_N];
    for (int i = 0; i < GQS_N; i++) v[i] = atomic_exchange_explicit(&g_gqs[i], 0, memory_order_relaxed);
    double n = (double)presents;
    LOG("gpu: split: the front end waited on a full ring %.2f times a present (%.3f ms), on drains %.2f (%.3f ms; "
        "%.2f drains asked a present, %.2f of them with every record run), for queued writes before reads %.2f (%.3f "
        "ms); the renderer thread ran %.0f records a "
        "present (%.1f KB) in %.3f ms\n",
        (double)v[GQS_ROOM_N] / n, (double)v[GQS_ROOM_NS] / 1e6 / n, (double)v[GQS_DRAIN_N] / n,
        (double)v[GQS_DRAIN_NS] / 1e6 / n, (double)v[GQS_DRAINS] / n, (double)v[GQS_DRAINS_IDLE] / n,
        (double)v[GQS_READ_N] / n, (double)v[GQS_READ_NS] / 1e6 / n, (double)v[GQS_RUNS] / n,
        (double)v[GQS_RUN_BYTES] / 1024.0 / n, (double)v[GQS_RUN_NS] / 1e6 / n);
}
/* the next record run, and the taker's position and the fence published: its bytes, 0 when there was none */
static u32 gq_take_one(void) {
    u64 tail = atomic_load_explicit(&g_gq.tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&g_gq.head, memory_order_acquire)) return 0;
    const GqRecord* r = (const GqRecord*)(g_gq.ring + tail % GQ_RING);
    u64 fence = r->fence;
    u32 bytes = r->bytes;
    gq_run(r);
    atomic_store_explicit(&g_gq.tail, tail + bytes, memory_order_release);
    atomic_store_explicit(&g_gq.done, fence, memory_order_release);
    atomic_fetch_add_explicit(&g_gq.progress, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&g_gq.putter_sleeps, memory_order_seq_cst)) plat_wake_word(&g_gq.progress);
    return bytes;
}
static void gq_thread(void* arg) {
    (void)arg;
    SNX_THREAD_NAME("GPU renderer");
    for (;;) {
        /* the records there are, timed and counted as one run: timed one by one, they were half of this thread's
           reads of the clock, about 3,000 a present in one game */
        if (atomic_load_explicit(&g_gq.tail, memory_order_relaxed) !=
            atomic_load_explicit(&g_gq.head, memory_order_acquire)) {
            u64 t0 = plat_ns(), n = 0, all = 0;
            for (u32 b; (b = gq_take_one()) != 0; n++) all += b;
            gqs_add(GQS_RUN_NS, plat_ns() - t0);
            gqs_add(GQS_RUNS, n);
            gqs_add(GQS_RUN_BYTES, all);
            continue;
        }
        u32 w = atomic_load_explicit(&g_gq.work, memory_order_seq_cst);
        atomic_store_explicit(&g_gq.taker_sleeps, 1, memory_order_seq_cst);
        if (atomic_load_explicit(&g_gq.tail, memory_order_relaxed) ==
            atomic_load_explicit(&g_gq.head, memory_order_seq_cst))
            plat_wait_word(&g_gq.work, w, GQ_SLEEP_NS);
        atomic_store_explicit(&g_gq.taker_sleeps, 0, memory_order_relaxed);
    }
}
/* the front end waits until ok(arg) holds, which the renderer thread's progress makes true. Without the thread the
   records run here instead */
static void gq_wait(int (*ok)(u64), u64 arg, int why) {
    u64 t0 = 0;
    if (g_gq.started && !ok(arg)) t0 = plat_ns();
    while (!ok(arg)) {
        if (!g_gq.started) {
            if (!gq_take_one()) fatal("gpu: the queue waits for a record it doesn't have\n");
            continue;
        }
        u32 w = atomic_load_explicit(&g_gq.progress, memory_order_seq_cst);
        atomic_store_explicit(&g_gq.putter_sleeps, 1, memory_order_seq_cst);
        if (!ok(arg)) plat_wait_word(&g_gq.progress, w, GQ_SLEEP_NS);
        atomic_store_explicit(&g_gq.putter_sleeps, 0, memory_order_relaxed);
    }
    if (t0) {
        int at = why == GQW_DRAIN ? GQS_DRAIN_N : why == GQW_READ ? GQS_READ_N : GQS_ROOM_N;
        gqs_add(at, 1);
        gqs_add(at + 1, plat_ns() - t0); /* each count's nanoseconds come after it */
    }
}
static int gq_done_by(u64 fence) { return atomic_load_explicit(&g_gq.done, memory_order_acquire) >= fence; }
/* head: only the front end stores it, so its own loads of it are relaxed */
static u64 gq_head(void) { return atomic_load_explicit(&g_gq.head, memory_order_relaxed); }
static int gq_room(u64 bytes) {
    return GQ_RING - (gq_head() - atomic_load_explicit(&g_gq.tail, memory_order_acquire)) >= bytes;
}
void gq_drain(void) {
    if (g_gq.started) {
        gqs_add(GQS_DRAINS, 1);
        if (gq_done_by(g_gq.queued)) gqs_add(GQS_DRAINS_IDLE, 1);
    }
    if (g_gq.queued) gq_wait(gq_done_by, g_gq.queued, GQW_DRAIN);
}
/* head moved past the records written before it: the renderer thread may take them from this store on */
static void gq_publish(u64 head) {
    atomic_store_explicit(&g_gq.head, head, memory_order_seq_cst);
    if (!g_gq.started) return;
    atomic_fetch_add_explicit(&g_gq.work, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&g_gq.taker_sleeps, memory_order_seq_cst)) plat_wake_word(&g_gq.work);
}
/* room for a record of payload bytes, its header filled: it doesn't wrap, a filler record takes the ring's end */
static GqRecord* gq_put(u32 kind, u64 gva, u64 size) {
    u64 bytes = (sizeof(GqRecord) + size + GQ_ALIGN - 1) & ~(u64)(GQ_ALIGN - 1);
    if (bytes > GQ_RING / 4) return NULL; /* too big for the ring: the caller runs it after the queue */
    if (!g_gq.ring) {
        if (!(g_gq.ring = (u8*)malloc(GQ_RING))) return NULL;
        if (g_settings.gpu_split) {
            if (plat_thread(gq_thread, NULL, 1u << 20)) {
                char why[256];
                fatal("gpu: could not start the renderer thread: %s\n", snx_os_error(why, sizeof why));
            }
            g_gq.started = 1;
            LOG("gpu: gpu_split=1: the renderer sets up draws on a thread of its own\n");
        }
    }
    u64 at = gq_head() % GQ_RING;
    if (at + bytes > GQ_RING) { /* the ring's end: a filler record to it, and this one from the ring's start */
        u64 fill = GQ_RING - at;
        gq_wait(gq_room, fill, GQW_ROOM);
        GqRecord* f = (GqRecord*)(g_gq.ring + at);
        *f = (GqRecord){GQ_SKIP, (u32)fill, ++g_gq.queued, 0, 0};
        gq_publish(gq_head() + fill);
        if (!g_gq.started) gq_drain();
        at = 0;
    }
    gq_wait(gq_room, bytes, GQW_ROOM);
    GqRecord* r = (GqRecord*)(g_gq.ring + at);
    *r = (GqRecord){kind, (u32)bytes, ++g_gq.queued, gva, size};
    return r;
}
static void gq_commit(const GqRecord* r) {
    gq_publish(gq_head() + r->bytes);
    if (!g_gq.started) gq_drain(); /* without the renderer thread the records run here */
}
int gq_queues(void) { return gpu_on_thread() && !t_gq_run && !t_gq_direct; }
void gq_direct(int on) { t_gq_direct = on; }
/* a write record of kind (GQ_WRITE or GQ_WRITE_CB), or the write now after the queue when it is too big */
static void gq_write_kind(u32 kind, u64 gva, const void* src, u64 size) {
    GqRecord* r = gq_put(kind, gva, size);
    if (!r) {
        gq_drain();
        if (kind == GQ_WRITE_CB)
            gmem_write_cb_now(gva, src, size);
        else
            gmem_write_now(gva, src, size);
        return;
    }
    memcpy(r + 1, src, (size_t)size);
    gq_mark(gva, size, r->fence);
    gq_recent_add(gva, size, r->fence, gq_head(), 1);
    gq_commit(r);
}
void gq_write(u64 gva, const void* src, u64 size) { gq_write_kind(GQ_WRITE, gva, src, size); }
void gq_write_cb(u64 gva, const void* src, u64 size) { gq_write_kind(GQ_WRITE_CB, gva, src, size); }
void gq_release(u64 gva, const void* data, u32 len) {
    GqRecord* r = gq_put(GQ_RELEASE, gva, len);
    if (!r) {
        gq_drain();
        gpu_release_write_now(gva, data, len);
        return;
    }
    memcpy(r + 1, data, len);
    gq_mark(gva, len, r->fence);
    gq_recent_add(gva, len, r->fence, gq_head(), 1); /* its payload, as release_read gives it */
    gq_commit(r);
}
void gq_i2m_block(const GpuI2m* t, u64 at, u64 size) {
    GqRecord* r = gq_put(GQ_I2M_BLOCK, t->regs.dest.address, sizeof t->regs + t->copy_size);
    if (!r) {
        gq_drain();
        gpu_i2m_block_now(t);
        return;
    }
    memcpy(r + 1, &t->regs, sizeof t->regs);
    memcpy((u8*)(r + 1) + sizeof t->regs, t->inner_buffer, t->copy_size);
    gq_mark(at, size, r->fence);
    gq_recent_add(at, size, r->fence, gq_head(), 0);
    gq_commit(r);
}
/* the highest fence of a queued write over a page of [gva, gva + size), or higher where pages share a slot */
static u64 gq_written(u64 gva, u64 size) {
    u64 first = gva >> 12, last = (gva + size - 1) >> 12, need = 0;
    if (last - first >= (1u << GQ_SLOT_BITS)) last = first + (1u << GQ_SLOT_BITS) - 1;
    for (u64 p = first; p <= last; p++)
        if (g_gq.written[gq_slot(p)] > need) need = g_gq.written[gq_slot(p)];
    return need;
}
/* the front end's read of [gva, gva + size): 1 when dst took its bytes from a queued write, else after the waits it
   needs memory holds them. dst NULL: only the waits */
static int gq_read(u64 gva, void* dst, u64 size) {
    if (!size || !gq_queues()) return 0;
    u64 done = atomic_load_explicit(&g_gq.done, memory_order_acquire);
    if (done == g_gq.queued) return 0;
    /* no write over these pages that hasn't run: nothing to wait for or forward. Else no write newer than need is
       over these bytes, so the scans below start at the newest write of fence need or lower. Each read scanned
       every write not run before (thousands while the renderer thread is behind), and that was about half of the
       front end's time in one game */
    u64 need = gq_written(gva, size);
    if (need <= done) return 0;
    if (g_gq_lost <= done) { /* the list holds every write not run yet: the newest over these bytes decides */
        u32 n = g_gq_recent_n < GQ_RECENT ? g_gq_recent_n : GQ_RECENT, skip = 0, hi = n;
        /* the fences in the list grow with its order: skip, the count of the newest entries above need */
        while (skip < hi) {
            u32 mid = skip + (hi - skip) / 2;
            if (g_gq_recent[(g_gq_recent_n - 1 - mid) % GQ_RECENT].fence > need) skip = mid + 1;
            else hi = mid;
        }
        /* every write over the bytes still in the ring: memory's bytes, then each write's over them, oldest first.
           One game builds each compute launch's description from several small writes just before the launch */
        if (dst && size <= 4096) {
            u32 over[64], nover = 0, all = 1;
            for (u32 i = skip; i < n; i++) {
                u32 k = (g_gq_recent_n - 1 - i) % GQ_RECENT;
                const GqRecent* e = &g_gq_recent[k];
                if (e->fence <= done) break;
                if (gva >= e->gva + e->size || e->gva >= gva + size) continue;
                if (!e->fwd || gq_head() - e->at > GQ_RING - GQ_RING / 4 || nover == 64) {
                    all = 0;
                    break;
                }
                over[nover++] = k;
            }
            if (all && !nover) return 0;
            if (all) {
                gmem_read_quiet(gva, dst, size);
                while (nover--) {
                    const GqRecent* e = &g_gq_recent[over[nover]];
                    u64 lo = gva > e->gva ? gva : e->gva, hi = gva + size < e->gva + e->size ? gva + size : e->gva + e->size;
                    const GqRecord* r = (const GqRecord*)(g_gq.ring + e->at % GQ_RING);
                    memcpy((u8*)dst + (lo - gva), (const u8*)(r + 1) + (lo - e->gva), (size_t)(hi - lo));
                }
                return 1;
            }
        }
        for (u32 i = skip; i < n; i++) {
            const GqRecent* e = &g_gq_recent[(g_gq_recent_n - 1 - i) % GQ_RECENT];
            if (e->fence <= done) break;
            if (gva >= e->gva + e->size || e->gva >= gva + size) continue;
            if (dst && e->fwd && gva >= e->gva && gva + size <= e->gva + e->size &&
                gq_head() - e->at <= GQ_RING - GQ_RING / 4) {
                const GqRecord* r = (const GqRecord*)(g_gq.ring + e->at % GQ_RING);
                memcpy(dst, (const u8*)(r + 1) + (gva - e->gva), (size_t)size);
                return 1;
            }
            gq_wait(gq_done_by, e->fence, GQW_READ); /* every older write is done by then too */
            return 0;
        }
        return 0;
    }
    gq_wait(gq_done_by, need, GQW_READ);
    return 0;
}
void gq_read_wait(u64 gva, u64 size) { gq_read(gva, NULL, size); }
int gq_forward(u64 gva, void* dst, u64 size) { return gq_read(gva, dst, size); }
void* gq_call_begin(GqFn fn, u64 bytes) {
    u64 at = 0;
    memcpy(&at, &fn, sizeof fn);
    g_gq.open = gq_put(GQ_CALL, at, bytes);
    if (g_gq.open) return g_gq.open + 1;
    /* too big for the ring: its bytes on the heap, and the call runs after every record before it */
    g_gq.big = malloc(bytes ? (size_t)bytes : 1);
    if (!g_gq.big) fatal("gpu: out of memory for a queued call of %llu bytes\n", (unsigned long long)bytes);
    g_gq.big_fn = fn;
    g_gq.big_bytes = bytes;
    return g_gq.big;
}
void gq_call_end(void) {
    if (g_gq.open) {
        GqRecord* r = g_gq.open;
        g_gq.open = NULL;
        gq_commit(r);
        return;
    }
    gq_drain();
    t_gq_run = 1;
    g_gq.big_fn(g_gq.big, g_gq.big_bytes);
    t_gq_run = 0;
    free(g_gq.big);
    g_gq.big = NULL;
}
void gq_call(GqFn fn, const void* arg, u64 bytes) {
    void* p = gq_call_begin(fn, bytes);
    if (bytes) memcpy(p, arg, (size_t)bytes);
    gq_call_end();
}
