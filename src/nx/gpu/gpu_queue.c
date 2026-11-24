/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The queue from the GPU front end to the renderer's side (gpu_queue.h). A ring of records, each with its fence.
   The front end puts a record in and goes on. The records run in their order: with gpu_split on a thread of their
   own, the renderer thread, else on the front end's thread right after they are queued. gpu_split=2 (auto) changes
   between the two as the game runs, at a present, by which one measures faster (gq_auto).

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

enum {
    GQ_WRITE,
    GQ_WRITE_CB,
    GQ_RELEASE,
    GQ_RELEASE_TICKS,
    GQ_RELEASE_STAMP,
    GQ_RELEASE_STAMP_TOP,
    GQ_I2M_BLOCK,
    GQ_CALL,
    GQ_SKIP
};
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
    int on;         /* it takes the records, where the front end doesn't run them itself: the front end's alone */
    u64 queued;     /* the fence of the last record put in: the front end's alone */
    u64 written[1u << GQ_SLOT_BITS];
    /* the front end's side */
    _Alignas(64) _Atomic u64 head; /* bytes put in, from the start: modulo GQ_RING in the ring */
    _Atomic u32 work;              /* moves with each publish: the renderer thread sleeps on it */
    _Atomic int taker_sleeps;
    /* the renderer thread's side */
    _Alignas(64) _Atomic u64 tail; /* bytes taken out */
    _Atomic u64 done;              /* the fence of the last record that ran */
    /* while a worker reads records that the taker already ran (draw_batches=2, gq_keep): the first byte that the front
       end must not put over, else GQ_KEEP_NONE */
    _Atomic u64 keep;
    _Atomic u32 progress;          /* moves with each record done: the front end sleeps on it */
    _Atomic int putter_sleeps;
    /* the change between the two (gq_mode): the front end clears split, and the renderer thread, once out of the
       record it runs, sets parked and takes no record until split is set again. One side takes the records at a
       time, so they run in their order */
    _Atomic int split, parked;
    u64 next_at;          /* the taker's: where the record after the one it runs starts (gq_peek_call) */
    _Atomic u64 presents; /* the renderer's presents (gq_present), for gq_auto */
    _Atomic u64 run_ns;   /* the renderer thread's time in records, for gq_auto */
} g_gq = {.keep = UINT64_MAX};
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
/* The records' time by kind (a probe): a record's kind, for a call its function and the first word of its payload
   (the kind of a draw state record, the op of a renderer call), the records and their counter ticks. The calls'
   functions are named by gq_name where their file gives a name */
#define GQ_TIMED 64
#define GQ_NAMES 16
static struct {
    struct {
        u32 kind, sub;
        GqFn fn;
        u64 n, ticks;
    } e[GQ_TIMED];
    struct {
        GqFn fn;
        const char* name;
        const char* const* subs;
        u32 nsubs;
    } names[GQ_NAMES];
    u64 tsc0, ns0;
} g_gq_timed;
void gq_name(GqFn fn, const char* name, const char* const* subs, u32 nsubs) {
    for (u32 i = 0; i < GQ_NAMES; i++)
        if (!g_gq_timed.names[i].fn) {
            g_gq_timed.names[i].fn = fn;
            g_gq_timed.names[i].name = name;
            g_gq_timed.names[i].subs = subs;
            g_gq_timed.names[i].nsubs = nsubs;
            return;
        }
}
static void gq_timed_add(const GqRecord* r, u64 ticks) {
    GqFn fn = NULL;
    u32 sub = 0;
    if (r->kind == GQ_CALL) {
        memcpy(&fn, &r->gva, sizeof fn);
        if (r->size >= 4) memcpy(&sub, r + 1, 4);
    }
    u32 h = (u32)((((uintptr_t)fn >> 4) ^ r->kind * 31u ^ sub * 131u) & (GQ_TIMED - 1));
    for (u32 i = 0; i < GQ_TIMED; i++, h = (h + 1) & (GQ_TIMED - 1)) {
        if (!g_gq_timed.e[h].n) {
            g_gq_timed.e[h].kind = r->kind;
            g_gq_timed.e[h].fn = fn;
            g_gq_timed.e[h].sub = sub;
        } else if (g_gq_timed.e[h].kind != r->kind || g_gq_timed.e[h].fn != fn || g_gq_timed.e[h].sub != sub)
            continue;
        g_gq_timed.e[h].n++;
        g_gq_timed.e[h].ticks += ticks;
        return;
    }
}
static void gq_timed_log(u64 presents) {
    const u64 tsc = __builtin_readcyclecounter(), ns = plat_ns();
    const double tick_ns = g_gq_timed.tsc0 && tsc > g_gq_timed.tsc0 ? (double)(ns - g_gq_timed.ns0) / (double)(tsc - g_gq_timed.tsc0) : 0;
    g_gq_timed.tsc0 = tsc;
    g_gq_timed.ns0 = ns;
    if (tick_ns && presents) {
        /* one name for each kind, in the order of the enum: a kind with no name here would read past the table */
        static const char* const kinds[] = {"write",         "cb write", "release", "release ticks", "release stamp",
                                            "release stamp top", "i2m", "call", "skip"};
        static_assert(sizeof kinds / sizeof *kinds == GQ_SKIP + 1, "a record kind has no name");
        char line[1400];
        int at = 0;
        for (u32 k = 0; k < 16 && at < (int)sizeof line - 120; k++) {
            u32 best = GQ_TIMED;
            for (u32 i = 0; i < GQ_TIMED; i++)
                if (g_gq_timed.e[i].n && (best == GQ_TIMED || g_gq_timed.e[i].ticks > g_gq_timed.e[best].ticks)) best = i;
            if (best == GQ_TIMED) break;
            const u32 kind = g_gq_timed.e[best].kind, sub = g_gq_timed.e[best].sub;
            char name[64];
            snprintf(name, sizeof name, "%s", kind <= GQ_SKIP ? kinds[kind] : "?");
            if (kind == GQ_CALL) {
                u32 j = 0;
                while (j < GQ_NAMES && g_gq_timed.names[j].fn && g_gq_timed.names[j].fn != g_gq_timed.e[best].fn) j++;
                if (j < GQ_NAMES && g_gq_timed.names[j].fn) {
                    if (sub < g_gq_timed.names[j].nsubs)
                        snprintf(name, sizeof name, "%s %s", g_gq_timed.names[j].name, g_gq_timed.names[j].subs[sub]);
                    else
                        snprintf(name, sizeof name, "%s", g_gq_timed.names[j].name);
                } else {
                    uintptr_t f = 0, b = 0;
                    const GqFn fn = g_gq_timed.e[best].fn;
                    void (*const base)(u64) = gq_timed_log;
                    memcpy(&f, &fn, sizeof f);
                    memcpy(&b, &base, sizeof b);
                    snprintf(name, sizeof name, "call at gq_timed_log%+lld", (long long)((intptr_t)f - (intptr_t)b));
                }
            }
            const double n = (double)g_gq_timed.e[best].n, t = (double)g_gq_timed.e[best].ticks * tick_ns;
            at += snprintf(line + at, sizeof line - (size_t)at, "%s %s %.0f a present %.3f ms (%.0f ns each)",
                           k ? ";" : "", name, n / (double)presents, t / 1e6 / (double)presents, t / n);
            g_gq_timed.e[best].n = 0; /* taken: the next look finds the next */
            g_gq_timed.e[best].ticks = 0;
        }
        LOG("gpu: split: the records' time by kind:%s\n", line);
    }
    memset(g_gq_timed.e, 0, sizeof g_gq_timed.e);
}
/* the records run, and the constant buffer writes of them, for the batch probe (vk/render.c) */
static u64 g_gq_seq_all, g_gq_seq_cb;
void gq_seq(u64* all, u64* cb) {
    *all = g_gq_seq_all;
    *cb = g_gq_seq_cb;
}
static void gq_run(const GqRecord* r) {
    const u8* payload = (const u8*)(r + 1);
    t_gq_run = 1;
    switch (r->kind) {
        case GQ_WRITE: gmem_write_now(r->gva, payload, r->size); break;
        case GQ_WRITE_CB: gmem_write_cb_now(r->gva, payload, r->size); break;
        case GQ_RELEASE: gpu_release_write_now(r->gva, payload, (u32)r->size); break;
        case GQ_RELEASE_TICKS: gpu_release_ticks_now(r->gva); break;
        case GQ_RELEASE_STAMP:
        case GQ_RELEASE_STAMP_TOP: gpu_release_stamp_ticked(r->gva, payload, r->kind == GQ_RELEASE_STAMP_TOP); break;
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
/* The drains by the place that asked (gpu_renderer_now's file and line, NULL for gq_drain's own callers), and of each
   the drains that found records not run, for the stats line: which calls to make records next. The front end adds,
   the present's thread reads and resets the counts */
#define GQ_SITES 32
static struct {
    _Atomic(const char*) file;
    _Atomic int line;
    _Atomic u64 n, busy;
} g_gq_site[GQ_SITES];
void gq_stats_log(u64 presents) {
    gq_timed_log(presents);
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
    char line[640];
    int at = 0;
    for (u32 i = 0; i < GQ_SITES && at < (int)sizeof line - 64; i++) {
        const char* f = atomic_load_explicit(&g_gq_site[i].file, memory_order_acquire);
        if (!f) break;
        u64 d = atomic_exchange_explicit(&g_gq_site[i].n, 0, memory_order_relaxed),
            b = atomic_exchange_explicit(&g_gq_site[i].busy, 0, memory_order_relaxed);
        if (!d) continue;
        const char* base = f;
        for (const char* c = f; *c; c++)
            if (*c == '/' || *c == '\\') base = c + 1;
        at += snprintf(line + at, sizeof line - (size_t)at, "%s %s:%d %.2f (%.2f busy)", at ? "," : "", base,
                       atomic_load_explicit(&g_gq_site[i].line, memory_order_relaxed), (double)d / n, (double)b / n);
    }
    if (at) LOG("gpu: split: drains a present by where they were asked:%s\n", line);
}
/* the next record run, and the taker's position and the fence published: its bytes, 0 when there was none */
static u32 gq_take_one(void) {
    u64 tail = atomic_load_explicit(&g_gq.tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&g_gq.head, memory_order_acquire)) return 0;
    const GqRecord* r = (const GqRecord*)(g_gq.ring + tail % GQ_RING);
    u64 fence = r->fence;
    u32 bytes = r->bytes;
    g_gq.next_at = tail + bytes; /* where the record after this one starts (gq_peek_call) */
    g_gq_seq_all++;
    if (r->kind == GQ_WRITE_CB) g_gq_seq_cb++;
    const u64 t0 = __builtin_readcyclecounter();
    gq_run(r);
    gq_timed_add(r, __builtin_readcyclecounter() - t0);
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
        if (!atomic_load_explicit(&g_gq.split, memory_order_seq_cst)) {
            /* the front end runs the records on its thread: it waits for parked, after this thread's last record */
            u32 w = atomic_load_explicit(&g_gq.work, memory_order_seq_cst);
            if (!atomic_load_explicit(&g_gq.parked, memory_order_relaxed)) {
                atomic_store_explicit(&g_gq.parked, 1, memory_order_seq_cst);
                atomic_fetch_add_explicit(&g_gq.progress, 1, memory_order_seq_cst);
                plat_wake_word(&g_gq.progress);
            }
            if (!atomic_load_explicit(&g_gq.split, memory_order_seq_cst)) plat_wait_word(&g_gq.work, w, GQ_SLEEP_NS);
            continue;
        }
        /* the records there are, timed and counted as one run: timed one by one, they were half of this thread's
           reads of the clock, about 3,000 a present in one game */
        if (atomic_load_explicit(&g_gq.tail, memory_order_relaxed) !=
            atomic_load_explicit(&g_gq.head, memory_order_acquire)) {
            u64 t0 = plat_ns(), n = 0, all = 0;
            for (u32 b; atomic_load_explicit(&g_gq.split, memory_order_relaxed) && (b = gq_take_one()) != 0; n++)
                all += b;
            u64 ns = plat_ns() - t0;
            gqs_add(GQS_RUN_NS, ns);
            gqs_add(GQS_RUNS, n);
            gqs_add(GQS_RUN_BYTES, all);
            atomic_fetch_add_explicit(&g_gq.run_ns, ns, memory_order_relaxed);
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
    if (g_gq.on && !ok(arg)) t0 = plat_ns();
    while (!ok(arg)) {
        if (!g_gq.on) {
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
    /* tail first: the taker stores keep before the tail that passes it, so a tail seen here comes with its keep */
    u64 tail = atomic_load_explicit(&g_gq.tail, memory_order_acquire);
    const u64 keep = atomic_load_explicit(&g_gq.keep, memory_order_acquire);
    if (keep < tail) tail = keep;
    return GQ_RING - (gq_head() - tail) >= bytes;
}
/* The taker's: keep the records from the one after the record it runs, until gq_keep_end. A worker reads them after
   the taker ran them (draw_batches=2) */
void gq_keep(void) { atomic_store_explicit(&g_gq.keep, g_gq.next_at, memory_order_seq_cst); }
void gq_keep_end(void) {
    atomic_store_explicit(&g_gq.keep, UINT64_MAX, memory_order_seq_cst);
    atomic_fetch_add_explicit(&g_gq.progress, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&g_gq.putter_sleeps, memory_order_seq_cst)) plat_wake_word(&g_gq.progress);
}
static void gq_site_count(const char* file, int line, int busy) {
    for (u32 i = 0; i < GQ_SITES; i++) {
        const char* f = atomic_load_explicit(&g_gq_site[i].file, memory_order_relaxed);
        if (!f) { /* the front end alone adds sites */
            atomic_store_explicit(&g_gq_site[i].line, line, memory_order_relaxed);
            atomic_store_explicit(&g_gq_site[i].file, file ? file : "gq_drain", memory_order_release);
        } else if (atomic_load_explicit(&g_gq_site[i].line, memory_order_relaxed) != line || strcmp(f, file ? file : "gq_drain"))
            continue;
        atomic_fetch_add_explicit(&g_gq_site[i].n, 1, memory_order_relaxed);
        if (busy) atomic_fetch_add_explicit(&g_gq_site[i].busy, 1, memory_order_relaxed);
        return;
    }
}
void gq_drain_at(const char* file, int line) {
    if (g_gq.on) {
        const int busy = !gq_done_by(g_gq.queued);
        gqs_add(GQS_DRAINS, 1);
        if (!busy) gqs_add(GQS_DRAINS_IDLE, 1);
        gq_site_count(file, line, busy);
    }
    if (g_gq.queued) gq_wait(gq_done_by, g_gq.queued, GQW_DRAIN);
}
/* head moved past the records written before it: the renderer thread may take them from this store on */
static void gq_publish(u64 head) {
    atomic_store_explicit(&g_gq.head, head, memory_order_seq_cst);
    if (!g_gq.on) return;
    atomic_fetch_add_explicit(&g_gq.work, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&g_gq.taker_sleeps, memory_order_seq_cst)) plat_wake_word(&g_gq.work);
}
/* the front end's: the renderer thread takes the records (on), or this thread runs them as they are queued. A change
   off waits for the renderer thread's record to end, then runs the rest here */
static void gq_mode(int on) {
    if (!g_gq.started || on == g_gq.on) return;
    if (!on) {
        atomic_store_explicit(&g_gq.split, 0, memory_order_seq_cst);
        atomic_fetch_add_explicit(&g_gq.work, 1, memory_order_seq_cst);
        plat_wake_word(&g_gq.work);
        while (!atomic_load_explicit(&g_gq.parked, memory_order_seq_cst)) {
            u32 w = atomic_load_explicit(&g_gq.progress, memory_order_seq_cst);
            atomic_store_explicit(&g_gq.putter_sleeps, 1, memory_order_seq_cst);
            if (!atomic_load_explicit(&g_gq.parked, memory_order_seq_cst))
                plat_wait_word(&g_gq.progress, w, GQ_SLEEP_NS);
            atomic_store_explicit(&g_gq.putter_sleeps, 0, memory_order_relaxed);
        }
        g_gq.on = 0;
        gq_drain(); /* what the renderer thread left, here */
        return;
    }
    gq_drain(); /* nothing: on this thread each record ran as it was queued */
    atomic_store_explicit(&g_gq.parked, 0, memory_order_seq_cst);
    atomic_store_explicit(&g_gq.split, 1, memory_order_seq_cst);
    g_gq.on = 1;
    atomic_fetch_add_explicit(&g_gq.work, 1, memory_order_seq_cst);
    plat_wake_word(&g_gq.work);
}
void gq_present(void) { atomic_fetch_add_explicit(&g_gq.presents, 1, memory_order_relaxed); }
/* for draw_prep=2 (vk/prep.c): the presents, the renderer thread's time in records, and whether it takes them now */
u64 gq_presents(void) { return atomic_load_explicit(&g_gq.presents, memory_order_relaxed); }
u64 gq_run_ns(void) { return atomic_load_explicit(&g_gq.run_ns, memory_order_relaxed); }
int gq_split_now(void) { return g_gq.started && atomic_load_explicit(&g_gq.split, memory_order_relaxed); }
/* gpu_split=2: which of the two runs the game's work a present faster, measured as it runs. A mode's cost is the GPU
   thread's busy time a present (gpu_thread_busy_ns: outside its waits for the game's work, and with its waits for the
   renderer thread in it), or with the split the renderer thread's time in records where that is longer: the frame
   waits for the slower of the two. The split ran a race 13% slower on one machine and other games faster: a guess from
   the numbers of one mode for the other missed that, so the other mode is run and measured.

   After hold presents in the chosen mode: its cost over GQA_MEASURE presents (before), GQA_WARM presents in the other
   mode (left out: caches and the thread's wake) and its cost over GQA_MEASURE (the probe), then GQA_WARM back in the
   chosen mode and its cost again (after). The probe wins when it costs under 95% of both before and after: a scene that
   grows lighter or heavier across the probe can't make a win. A probe whose before and after differ by more than a
   quarter counts for neither (a shader built, a scene changed), and none starts while the chosen mode costs under
   GQA_FLOOR_MS a present: a loading screen's near-empty presents switched one run to one thread at its start, with
   1.2 ms against 6.1 ms, and it ran the race there for most of a minute. Two wins in a row change the choice. A probe
   that loses doubles hold, up to GQA_HOLD_MAX. The changes happen between records at the first queue after a present,
   never inside a frame's record */
#define GQA_HOLD     600u
#define GQA_HOLD_MAX 4800u
#define GQA_MEASURE  60u
#define GQA_WARM     10u
#define GQA_FLOOR_MS 2.0
static struct {
    u64 seen;  /* g_gq.presents at the last look */
    u64 until; /* the phase ends at this present */
    u64 p0, busy0, run0;
    double before_ms, probe_ms;
    u32 hold, wins;
    /* 0 the chosen mode, 1 its measure before, 2 the other mode's warm-up, 3 its measure, 4 the chosen mode's warm-up,
       5 its measure after */
    int phase;
} g_gqa;
static void gqa_mark(u64 p) {
    g_gqa.p0 = p;
    g_gqa.busy0 = gpu_thread_busy_ns();
    g_gqa.run0 = atomic_load_explicit(&g_gq.run_ns, memory_order_relaxed);
}
static double gqa_cost(u64 p) {
    u64 busy = gpu_thread_busy_ns() - g_gqa.busy0, run = atomic_load_explicit(&g_gq.run_ns, memory_order_relaxed) - g_gqa.run0;
    u64 c = g_gq.on && run > busy ? run : busy;
    return p > g_gqa.p0 ? (double)c / 1e6 / (double)(p - g_gqa.p0) : 0;
}
static const char* gqa_name(int on) { return on ? "two threads" : "one thread"; }
static void gq_auto(void) {
    u64 p = atomic_load_explicit(&g_gq.presents, memory_order_relaxed);
    if (p == g_gqa.seen) return;
    g_gqa.seen = p;
    if (!g_gqa.hold) g_gqa.hold = GQA_HOLD, g_gqa.until = GQA_HOLD;
    if (p < g_gqa.until) return;
    switch (g_gqa.phase) {
        case 0:
            gqa_mark(p);
            g_gqa.phase = 1, g_gqa.until = p + GQA_MEASURE;
            break;
        case 1:
            g_gqa.before_ms = gqa_cost(p);
            if (g_gqa.before_ms < GQA_FLOOR_MS) { /* too light to tell: the same hold again */
                g_gqa.phase = 0, g_gqa.until = p + g_gqa.hold;
                break;
            }
            gq_mode(!g_gq.on);
            g_gqa.phase = 2, g_gqa.until = p + GQA_WARM;
            break;
        case 2:
        case 4:
            gqa_mark(p);
            g_gqa.phase++, g_gqa.until = p + GQA_MEASURE;
            break;
        case 3:
            g_gqa.probe_ms = gqa_cost(p);
            gq_mode(!g_gq.on);
            g_gqa.phase = 4, g_gqa.until = p + GQA_WARM;
            break;
        default: {
            const double before = g_gqa.before_ms, after = gqa_cost(p), probe = g_gqa.probe_ms;
            const double lo = before < after ? before : after, hi = before < after ? after : before;
            const char *chosen = gqa_name(g_gq.on), *other = gqa_name(!g_gq.on);
            g_gqa.phase = 0;
            if (hi > lo * 1.25) { /* the scene moved across the probe: neither a win nor a loss */
                LOG("gpu: gpu_split=auto: %.3f ms a present on %s between %.3f and %.3f ms on %s: unsettled, again "
                    "later\n",
                    probe, other, before, after, chosen);
                g_gqa.until = p + g_gqa.hold;
                break;
            }
            const int win = probe < before * 0.95 && probe < after * 0.95;
            g_gqa.wins = win ? g_gqa.wins + 1 : 0;
            if (g_gqa.wins >= 2) {
                gq_mode(!g_gq.on);
                g_gqa.wins = 0;
                g_gqa.hold = GQA_HOLD;
                LOG("gpu: gpu_split=auto: %.3f ms a present on %s against %.3f and %.3f ms on %s: it runs on %s from "
                    "now\n",
                    probe, other, before, after, chosen, other);
            } else {
                if (!win) g_gqa.hold = g_gqa.hold * 2 < GQA_HOLD_MAX ? g_gqa.hold * 2 : GQA_HOLD_MAX;
                LOG("gpu: gpu_split=auto: %.3f ms a present on %s against %.3f and %.3f ms on %s: %s\n", probe, other,
                    before, after, chosen, win ? "a win, one more to change" : "it stays");
            }
            g_gqa.until = p + g_gqa.hold;
        }
    }
}
/* room for a record of payload bytes, its header filled: it doesn't wrap, a filler record takes the ring's end */
static GqRecord* gq_put(u32 kind, u64 gva, u64 size) {
    u64 bytes = (sizeof(GqRecord) + size + GQ_ALIGN - 1) & ~(u64)(GQ_ALIGN - 1);
    if (bytes > GQ_RING / 4) return NULL; /* too big for the ring: the caller runs it after the queue */
    if (!g_gq.ring) {
        if (!(g_gq.ring = (u8*)malloc(GQ_RING))) return NULL;
        if (g_settings.gpu_split) {
            atomic_store_explicit(&g_gq.split, 1, memory_order_relaxed);
            if (plat_thread(gq_thread, NULL, 1u << 20)) {
                char why[256];
                fatal("gpu: could not start the renderer thread: %s\n", snx_os_error(why, sizeof why));
            }
            g_gq.started = g_gq.on = 1;
            LOG("gpu: gpu_split=%s: the renderer sets up draws on a thread of its own%s\n",
                g_settings.gpu_split == 2 ? "auto" : "1",
                g_settings.gpu_split == 2 ? ", or on this one where that measures faster" : "");
        }
    }
    if (g_settings.gpu_split == 2 && g_gq.started) gq_auto();
    u64 at = gq_head() % GQ_RING;
    if (at + bytes > GQ_RING) { /* the ring's end: a filler record to it, and this one from the ring's start */
        u64 fill = GQ_RING - at;
        gq_wait(gq_room, fill, GQW_ROOM);
        GqRecord* f = (GqRecord*)(g_gq.ring + at);
        *f = (GqRecord){GQ_SKIP, (u32)fill, ++g_gq.queued, 0, 0};
        gq_publish(gq_head() + fill);
        if (!g_gq.on) gq_drain();
        at = 0;
    }
    gq_wait(gq_room, bytes, GQW_ROOM);
    GqRecord* r = (GqRecord*)(g_gq.ring + at);
    *r = (GqRecord){kind, (u32)bytes, ++g_gq.queued, gva, size};
    return r;
}
static void gq_commit(const GqRecord* r) {
    gq_publish(gq_head() + r->bytes);
    if (!g_gq.on) gq_drain(); /* without the renderer thread the records run here */
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
void gq_release_ticks(u64 gva) {
    GqRecord* r = gq_put(GQ_RELEASE_TICKS, gva, 0);
    if (!r) {
        gq_drain();
        gpu_release_ticks_now(gva);
        return;
    }
    gq_mark(gva, 8, r->fence);
    gq_recent_add(gva, 8, r->fence, gq_head(), 0); /* nothing to forward: the record makes its bytes as it runs */
    gq_commit(r);
}
void gq_release_stamp(u64 gva, const void* data, int top) {
    GqRecord* r = gq_put(top ? GQ_RELEASE_STAMP_TOP : GQ_RELEASE_STAMP, gva, 16);
    if (!r) {
        gq_drain();
        gpu_release_stamp_ticked(gva, data, top);
        return;
    }
    memcpy(r + 1, data, 16);
    gq_mark(gva, 16, r->fence);
    /* its first 8 bytes go as they are; the ticks after them are the record's to make as it runs */
    gq_recent_add(gva, 8, r->fence, gq_head(), 1);
    gq_recent_add(gva + 8, 8, r->fence, gq_head(), 0);
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
/* The payload of the record after the one the taker runs, while it runs that one, when the next is a call of fn
   that the front end has put in whole: NULL else. Another thread may ask while the taker waits for it, in the run:
   the bytes stay until the taker passes them */
/* what gq_peek_call found after the record run, for the stats line: none put in yet, a record of each kind, and
   the calls of another function by the function (the first GQ_PEEK_FNS seen) */
#define GQ_PEEK_FNS 8
static struct {
    u64 none, kind[GQ_SKIP + 1], fn_n[GQ_PEEK_FNS];
    GqFn fn[GQ_PEEK_FNS];
} g_gq_peek;
void gq_peek_log(GqFn base) {
    char line[512];
    int at = snprintf(line, sizeof line, "none put in %llu, writes %llu, cb writes %llu, releases %llu, i2m %llu, calls %llu;",
                      (unsigned long long)g_gq_peek.none, (unsigned long long)g_gq_peek.kind[GQ_WRITE],
                      (unsigned long long)g_gq_peek.kind[GQ_WRITE_CB], (unsigned long long)g_gq_peek.kind[GQ_RELEASE],
                      (unsigned long long)g_gq_peek.kind[GQ_I2M_BLOCK], (unsigned long long)g_gq_peek.kind[GQ_CALL]);
    for (u32 i = 0; i < GQ_PEEK_FNS && g_gq_peek.fn[i] && at < (int)sizeof line - 48; i++) {
        uintptr_t f = 0, b = 0;
        memcpy(&f, &g_gq_peek.fn[i], sizeof f < sizeof g_gq_peek.fn[i] ? sizeof f : sizeof g_gq_peek.fn[i]);
        memcpy(&b, &base, sizeof b < sizeof base ? sizeof b : sizeof base);
        at += snprintf(line + at, sizeof line - (size_t)at, " fn%+lld %llu", (long long)((intptr_t)f - (intptr_t)b),
                       (unsigned long long)g_gq_peek.fn_n[i]);
        g_gq_peek.fn_n[i] = 0;
    }
    LOG("gpu: split: the records after a draw: %s\n", line);
    g_gq_peek.none = 0;
    memset(g_gq_peek.kind, 0, sizeof g_gq_peek.kind);
}
const void* gq_peek_call(GqFn fn, u64* bytes) {
    u64 at = g_gq.next_at, head = atomic_load_explicit(&g_gq.head, memory_order_acquire);
    if (at == head) g_gq_peek.none++;
    for (int hop = 0; hop < 2 && at != head; hop++) {
        const GqRecord* r = (const GqRecord*)(const void*)(g_gq.ring + at % GQ_RING);
        if (r->kind == GQ_SKIP) { /* the filler at the ring's end */
            at += r->bytes;
            continue;
        }
        GqFn f;
        memcpy(&f, &r->gva, sizeof f);
        if (r->kind <= GQ_SKIP) g_gq_peek.kind[r->kind]++;
        if (r->kind == GQ_CALL && f != fn)
            for (u32 i = 0; i < GQ_PEEK_FNS; i++)
                if (!g_gq_peek.fn[i] || g_gq_peek.fn[i] == f) {
                    g_gq_peek.fn[i] = f;
                    g_gq_peek.fn_n[i]++;
                    break;
                }
        if (r->kind != GQ_CALL || f != fn) return NULL;
        *bytes = r->size;
        return r + 1;
    }
    return NULL;
}
/* The payloads of the calls of fn that follow the record the taker runs, while it runs that one, up to max: their
   count. The bytes stay until the taker passes them */
u32 gq_peek_calls(GqFn fn, const void** args, u64* sizes, u32 max) {
    u64 at = g_gq.next_at, head = atomic_load_explicit(&g_gq.head, memory_order_acquire);
    u32 n = 0;
    while (n < max && at != head) {
        const GqRecord* r = (const GqRecord*)(const void*)(g_gq.ring + at % GQ_RING);
        if (r->kind != GQ_SKIP) {
            GqFn f;
            memcpy(&f, &r->gva, sizeof f);
            if (r->kind != GQ_CALL || f != fn) break;
            args[n] = r + 1;
            sizes[n++] = r->size;
        }
        at += r->bytes;
    }
    return n;
}
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
