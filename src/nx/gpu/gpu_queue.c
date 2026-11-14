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
#include <stdlib.h>
#include <string.h>
#include "common/platform/thread.h"
#include "common/utilities/common.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"

enum { GQ_WRITE, GQ_RELEASE, GQ_I2M_BLOCK, GQ_CALL, GQ_SKIP };
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
    _Alignas(64) u64 head; /* bytes put in, from the start: modulo GQ_RING in the ring */
    u32 work;              /* moves with each publish: the renderer thread sleeps on it */
    int taker_sleeps;
    /* the renderer thread's side */
    _Alignas(64) u64 tail; /* bytes taken out */
    u64 done;              /* the fence of the last record that ran */
    u32 progress;          /* moves with each record done: the front end sleeps on it */
    int putter_sleeps;
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
static _Thread_local int t_gq_run; /* this thread runs a record: its writes are the record's own */

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
/* the next record run, and the taker's position and the fence published: 0 when there was none */
static int gq_take_one(void) {
    u64 tail = __atomic_load_n(&g_gq.tail, __ATOMIC_RELAXED);
    if (tail == __atomic_load_n(&g_gq.head, __ATOMIC_ACQUIRE)) return 0;
    const GqRecord* r = (const GqRecord*)(g_gq.ring + tail % GQ_RING);
    u64 fence = r->fence;
    u32 bytes = r->bytes;
    gq_run(r);
    __atomic_store_n(&g_gq.tail, tail + bytes, __ATOMIC_RELEASE);
    __atomic_store_n(&g_gq.done, fence, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_gq.progress, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_gq.putter_sleeps, __ATOMIC_SEQ_CST)) plat_wake_word(&g_gq.progress);
    return 1;
}
static void gq_thread(void* arg) {
    (void)arg;
    SNX_THREAD_NAME("GPU renderer");
    for (;;) {
        if (gq_take_one()) continue;
        u32 w = __atomic_load_n(&g_gq.work, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_gq.taker_sleeps, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g_gq.tail, __ATOMIC_RELAXED) == __atomic_load_n(&g_gq.head, __ATOMIC_SEQ_CST))
            plat_wait_word(&g_gq.work, w, GQ_SLEEP_NS);
        __atomic_store_n(&g_gq.taker_sleeps, 0, __ATOMIC_RELAXED);
    }
}
/* the front end waits until ok(arg) holds, which the renderer thread's progress makes true. Without the thread the
   records run here instead */
static void gq_wait(int (*ok)(u64), u64 arg) {
    while (!ok(arg)) {
        if (!g_gq.started) {
            if (!gq_take_one()) fatal("gpu: the queue waits for a record it doesn't have\n");
            continue;
        }
        u32 w = __atomic_load_n(&g_gq.progress, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_gq.putter_sleeps, 1, __ATOMIC_SEQ_CST);
        if (!ok(arg)) plat_wait_word(&g_gq.progress, w, GQ_SLEEP_NS);
        __atomic_store_n(&g_gq.putter_sleeps, 0, __ATOMIC_RELAXED);
    }
}
static int gq_done_by(u64 fence) { return __atomic_load_n(&g_gq.done, __ATOMIC_ACQUIRE) >= fence; }
static int gq_room(u64 bytes) {
    return GQ_RING - (g_gq.head - __atomic_load_n(&g_gq.tail, __ATOMIC_ACQUIRE)) >= bytes;
}
void gq_drain(void) {
    if (g_gq.queued) gq_wait(gq_done_by, g_gq.queued);
}
static void gq_publish(void) {
    __atomic_store_n(&g_gq.head, g_gq.head, __ATOMIC_SEQ_CST);
    if (!g_gq.started) return;
    __atomic_add_fetch(&g_gq.work, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_gq.taker_sleeps, __ATOMIC_SEQ_CST)) plat_wake_word(&g_gq.work);
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
    u64 at = g_gq.head % GQ_RING;
    if (at + bytes > GQ_RING) { /* the ring's end: a filler record to it, and this one from the ring's start */
        u64 fill = GQ_RING - at;
        gq_wait(gq_room, fill);
        GqRecord* f = (GqRecord*)(g_gq.ring + at);
        *f = (GqRecord){GQ_SKIP, (u32)fill, ++g_gq.queued, 0, 0};
        g_gq.head += fill;
        gq_publish();
        if (!g_gq.started) gq_drain();
        at = 0;
    }
    gq_wait(gq_room, bytes);
    GqRecord* r = (GqRecord*)(g_gq.ring + at);
    *r = (GqRecord){kind, (u32)bytes, ++g_gq.queued, gva, size};
    return r;
}
static void gq_commit(const GqRecord* r) {
    g_gq.head += r->bytes;
    gq_publish();
    if (!g_gq.started) gq_drain(); /* without the renderer thread the records run here */
}
int gq_queues(void) { return gpu_on_thread() && !t_gq_run; }
void gq_write(u64 gva, const void* src, u64 size) {
    GqRecord* r = gq_put(GQ_WRITE, gva, size);
    if (!r) {
        gq_drain();
        gmem_write_now(gva, src, size);
        return;
    }
    memcpy(r + 1, src, (size_t)size);
    gq_mark(gva, size, r->fence);
    gq_recent_add(gva, size, r->fence, g_gq.head, 1);
    gq_commit(r);
}
void gq_release(u64 gva, const void* data, u32 len) {
    GqRecord* r = gq_put(GQ_RELEASE, gva, len);
    if (!r) {
        gq_drain();
        gpu_release_write_now(gva, data, len);
        return;
    }
    memcpy(r + 1, data, len);
    gq_mark(gva, len, r->fence);
    gq_recent_add(gva, len, r->fence, g_gq.head, 1); /* its payload, as release_read gives it */
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
    gq_recent_add(at, size, r->fence, g_gq.head, 0);
    gq_commit(r);
}
/* the front end's read of [gva, gva + size): 1 when dst took its bytes from a queued write, else after the waits it
   needs memory holds them. dst NULL: only the waits */
static int gq_read(u64 gva, void* dst, u64 size) {
    if (!size || !gq_queues()) return 0;
    u64 done = __atomic_load_n(&g_gq.done, __ATOMIC_ACQUIRE);
    if (done == g_gq.queued) return 0;
    if (g_gq_lost <= done) { /* the list holds every write not run yet: the newest over these bytes decides */
        u32 n = g_gq_recent_n < GQ_RECENT ? g_gq_recent_n : GQ_RECENT;
        /* every write over the bytes still in the ring: memory's bytes, then each write's over them, oldest first.
           One game builds each compute launch's description from several small writes just before the launch */
        if (dst && size <= 4096) {
            u32 over[64], nover = 0, all = 1;
            for (u32 i = 0; i < n; i++) {
                u32 k = (g_gq_recent_n - 1 - i) % GQ_RECENT;
                const GqRecent* e = &g_gq_recent[k];
                if (e->fence <= done) break;
                if (gva >= e->gva + e->size || e->gva >= gva + size) continue;
                if (!e->fwd || g_gq.head - e->at > GQ_RING - GQ_RING / 4 || nover == 64) {
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
        for (u32 i = 0; i < n; i++) {
            const GqRecent* e = &g_gq_recent[(g_gq_recent_n - 1 - i) % GQ_RECENT];
            if (e->fence <= done) break;
            if (gva >= e->gva + e->size || e->gva >= gva + size) continue;
            if (dst && e->fwd && gva >= e->gva && gva + size <= e->gva + e->size &&
                g_gq.head - e->at <= GQ_RING - GQ_RING / 4) {
                const GqRecord* r = (const GqRecord*)(g_gq.ring + e->at % GQ_RING);
                memcpy(dst, (const u8*)(r + 1) + (gva - e->gva), (size_t)size);
                return 1;
            }
            gq_wait(gq_done_by, e->fence); /* every older write is done by then too */
            return 0;
        }
        return 0;
    }
    u64 first = gva >> 12, last = (gva + size - 1) >> 12, need = 0;
    if (last - first >= (1u << GQ_SLOT_BITS)) last = first + (1u << GQ_SLOT_BITS) - 1;
    for (u64 p = first; p <= last; p++)
        if (g_gq.written[gq_slot(p)] > need) need = g_gq.written[gq_slot(p)];
    if (need) gq_wait(gq_done_by, need);
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
