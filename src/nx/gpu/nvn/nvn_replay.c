/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's replay: a queue's submitted recordings run on the GPU thread, in submission order, as a GPFIFO
   submission's methods do (neutral_draw_state's 4f: never at record time, since a recording may be submitted many
   times and what it reads changes between). Each queue keeps the state its commands set (gpu/nvn/nvn_draw.c), which
   makes the renderer's draw state of it as maxwell/draw_state.c makes a channel's, for the draws and clears.

   What the CPU sees happens in order too: counter reports and event writes through the renderer's ordered releases,
   fences as increments of the queue's syncpoint. A sync is fenced at submission with the syncpoint value its point
   will reach, and signals when the GPU thread has replayed up to it and the renderer's work before it is done (the
   stream increment gpu.c makes after a GPFIFO job's work). A window present shows the presented texture once the
   work before it is replayed, and its point on the presents' own syncpoint is reached once the display shows it (the
   renderer learns when, renderer.h present_buffer's shown_syncpt) */
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "nx/gpu/nvn/nvn_copy.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_verify.h"
#include "nx/hle/pool.h"
#include "nx/runtime/options.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <string.h>

/* What the CPU sees of a submission: a fence's syncpoint increment, or bytes the stream writes (counter reports, event
   writes, an event's add of v to what is there) */
enum { FX_FENCE, FX_WRITE, FX_ADD };

enum { JOB_SUBMIT, JOB_FENCE, JOB_PRESENT };

u32 nvn_replay_queue(void) {
    u32 i;
    pm_lock(&g_nvn.replay.lock);
    if (g_nvn.replay.n == NVN_QUEUES) fatal("nvn: more than %d queues", NVN_QUEUES);
    if (!g_nvn.replay.shown) g_nvn.replay.shown = syncpt_alloc();
    i = g_nvn.replay.n++;
    nvn_draw_init(&g_nvn.replay.q[i].d, 0x80000000u | i); /* renderer.h: the NVN replay's queues from 0x80000000 */
    g_nvn.replay.q[i].syncpt = syncpt_alloc();
    pm_unlock(&g_nvn.replay.lock);
    return i + 1;
}
u32 nvn_replay_syncpt(u32 queue) { return queue ? g_nvn.replay.q[queue - 1].syncpt : 0; }
u32 nvn_replay_shown_syncpt(void) { return g_nvn.replay.shown; }

/* ---- the records */
typedef void (*RecordFn)(void* ctx, u32 op, u64 payload, u32 size);
/* each record of the recording at p, in order: a JUMP continues in another segment, a CALL runs another recording,
   an END returns */
static void walk(u64 p, RecordFn fn, void* ctx, int depth) {
    if (depth > 8) fatal("nvn: recordings call each other more than 8 deep (at %llx)", (unsigned long long)p);
    u32 prev = OP_BEGIN;
    for (;;) {
        NvnRecHdr h;
        memcpy(&h, GP(p), sizeof h);
        if (h.op >= OP_COUNT_ || h.size < 8 || (h.size & 7))
            fatal("nvn: a command record at %llx (after a record of op %u) has op %u and size %u, which nvn_direct "
                  "never writes",
                  (unsigned long long)p, prev, h.op, h.size);
        prev = h.op;
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) {
            p = gr64(p + 8);
            continue;
        }
        if (h.op == OP_CALL)
            walk(gr64(p + 8), fn, ctx, depth + 1);
        else
            fn(ctx, h.op, p + 8, h.size - 8);
        p += h.size;
    }
}

void nvn_sync_hold(u64* ready, u32* shown, const NvnSync* s) {
    if (!__atomic_load_n(&s->fenced, __ATOMIC_ACQUIRE)) return;
    if (s->ready > *ready) *ready = s->ready;
    if (!*shown || (s->shown && (i32)(s->shown - *shown) > 0)) *shown = s->shown;
}

/* at submission, in record order: each fence the recordings hold gets the syncpoint value its point reaches, and the
   work after a wait is held behind its sync (the GPU thread has nothing to wait for: what a sync fenced before the
   submission waits for is replayed before it, in order) */
typedef struct {
    u32 syncpt, shown;
    u64 ready;
} Promise;
static void promise(void* ctx, u32 op, u64 payload, u32 size) {
    Promise* pr = (Promise*)ctx;
    NvnSync* s;
    (void)size;
    if (op == OP_WAIT_SYNC) {
        s = NVN_OBJ(NvnSync, gr64(payload));
        if (!__atomic_load_n(&s->fenced, __ATOMIC_ACQUIRE))
            LOG_LIMIT(4, "nvn: a recording waits for sync %llx, which nothing fenced before its submission\n",
                      (unsigned long long)gr64(payload));
        nvn_sync_hold(&pr->ready, &pr->shown, s);
        return;
    }
    if (op != OP_FENCE_SYNC) return;
    s = NVN_OBJ(NvnSync, gr64(payload));
    s->syncpt = pr->syncpt;
    s->value = syncpt_promise(pr->syncpt, 1);
    s->ready = pr->ready;
    s->shown = pr->shown;
    __atomic_store_n(&s->fenced, 1, __ATOMIC_RELEASE);
}

/* a syncpoint increment once the renderer's work before it is done (gpu.c's stream_incr) */
static void stream_incr(u32 id) {
    if (g_renderer->stream_incr)
        g_renderer->stream_incr(id);
    else
        syncpt_incr(id);
}

/* on the GPU thread: a record's work */
typedef struct {
    NvnReplayQueue* q;
} Replay;

/* A sync signals, an event is written, once the work before it is done, and the game may then take back the memory
   of the recordings it covers. The walk still reads records after them (at least the END), which on the console the
   GPU had fetched by then: what the CPU sees of a submission happens once all of it is read, in order, after its
   work, which is later than the console but never earlier */
static void effect(u32 kind, u64 gva, const void* data, u32 len) {
    NvnReplayState* st = &g_nvn.replay;
    if (st->nfx == NVN_EFFECTS)
        fatal("nvn: a submission signals more than %u fences and events, which nvn_direct holds", NVN_EFFECTS);
    NvnEffect* e = &st->fx[st->nfx++];
    e->kind = kind;
    e->gva = gva;
    e->len = len;
    if (len) memcpy(e->data, data, len);
}
static void effects(NvnReplayQueue* q) {
    NvnReplayState* st = &g_nvn.replay;
    u32 i;
    for (i = 0; i < st->nfx; i++) {
        NvnEffect* e = &st->fx[i];
        if (e->kind == FX_FENCE) {
            stream_incr(q->syncpt);
            continue;
        }
        if (e->kind == FX_ADD) { /* what is there as the stream sees it, past the releases queued before (gmem_read): an
                                    event's 32-bit value, which the game's CPU waits on */
            u32 v;
            memcpy(&v, e->data, 4);
            v += gmem_r32(e->gva);
            memcpy(e->data, &v, 4);
        }
        gpu_release_write(e->gva, e->data, e->len);
    }
    st->nfx = 0;
}
static void replay(void* ctx, u32 op, u64 payload, u32 size) {
    NvnReplayQueue* q = ((Replay*)ctx)->q;
    switch (op) {
        case OP_FENCE_SYNC: effect(FX_FENCE, 0, NULL, 0); break;
        case OP_WAIT_SYNC: break; /* held at submission (promise) */
        case OP_REPORT_COUNTER: {
            /* {counter, timestamp}, written in order with the stream, as the 3D engine's reports are (a render enable
               later in the same submission reads it, maxwell/engines.c): samples passed as the renderer counts the
               queue's draws since its reset, else a made-up count, so an occlusion test before any reset finds
               something drawn; the timestamp types put 0 in the counter, as the game's driver writes them (a report
               of payload 0). One game makes each conditional clear of a target run once a frame that way: it sets the
               counter to 1, clears when it differs from the word 16 bytes on, and then reports a timestamp over it */
            const NvnPReportCounter* r = NVN_OBJ(const NvnPReportCounter, payload);
            u8 v[16];
            u64 ticks = guest_ticks();
            if (r->type == NVN_COUNTER_TYPE_SAMPLES_PASSED && nvn_draw_samples(&q->d, r->address, ticks)) break;
            snx_st_le64(v, r->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 1000 : 0);
            snx_st_le64(v + 8, ticks);
            gpu_release_write(r->address, v, sizeof v);
            break;
        }
        case OP_SIGNAL_EVENT: {
            const NvnPSignalEvent* e = NVN_OBJ(const NvnPSignalEvent, payload);
            effect(e->mode == NVN_EVENT_SIGNAL_MODE_ADD ? FX_ADD : FX_WRITE, e->value, &e->v, sizeof e->v);
            break;
        }
        default:
            if (!nvn_draw_record(&q->d, op, payload, size) && !nvn_copy_record(op, payload, size, q->stride) &&
                !(g_nvn.replay.unknown >> op & 1)) {
                g_nvn.replay.unknown |= 1ull << op; /* each once */
                LOG("nvn: records of op %u aren't replayed yet\n", op);
            }
            break;
    }
}

static void run(void* arg) {
    NvnJob* j = (NvnJob*)arg;
    NvnReplayQueue* q = &g_nvn.replay.q[j->queue - 1];
    switch (j->kind) {
        case JOB_SUBMIT: {
            Replay r;
            u32 i;
            r.q = q;
            if (g_settings.nvn_verify) nvn_verify_replay_submit(j->queue);
            if (g_renderer->stream_run) g_renderer->stream_run(1);
            for (i = 0; i < j->n; i++) walk(j->handles[i], replay, &r, 0);
            effects(q);
            if (g_renderer->stream_run) g_renderer->stream_run(0);
            break;
        }
        case JOB_FENCE: stream_incr(q->syncpt); break;
        case JOB_PRESENT: {
            const NvnReplayPresent* p = &j->present;
            u64 cpu = p->gva ? gmmu_translate(p->gva) : 0;
            if (g_renderer_up && cpu)
                g_renderer->present_buffer(cpu, p->w, p->h, p->pitch, p->linear ? 0 : 3, p->bhl, p->fmt, p->crop, 0,
                                           p->released_ns, p->period_ns, g_nvn.replay.shown);
            else if (g_renderer_up) /* nothing to show: black, which still ends the renderer's frame */
                g_renderer->present_buffer(0, 0, 0, 0, 0, 0, 0, p->crop, 0, p->released_ns, p->period_ns,
                                           g_nvn.replay.shown);
            else /* no display to reach */
                syncpt_incr(g_nvn.replay.shown);
            stream_incr(q->syncpt); /* shown: the window's texture comes free (nvn_WindowAcquireTexture) */
            break;
        }
    }
    /* the stream stops here, as gpu.c's job_end: the renderer holds a stream's releases (its increments and writes)
       until then (release.c), and the game may now look at them */
    if (g_renderer->stream_end) g_renderer->stream_end();
    pm_lock(&g_nvn.replay.lock);
    POOL_PUT(j);
    pm_unlock(&g_nvn.replay.lock);
}

/* work for the GPU thread, from the pool (under g_nvn.replay.lock): non-zero before the lock is let go, as its queue
   is 1 up */
static NvnJob* job(u32 queue, u32 kind, u32 n) {
    if (n > NVN_JOB_HANDLES)
        fatal("nvn: a submission of %u command handles, more than the %u nvn_direct holds", n, NVN_JOB_HANDLES);
    NvnJob* j = POOL_TAKE(g_nvn.replay.jobs, g_nvn.replay.next_job, "NVN jobs");
    if (!j)
        fatal("nvn: more than %u submissions, fences and presents queued for the GPU thread at once, which nvn_direct "
              "holds",
              NVN_JOBS);
    j->queue = queue;
    j->kind = kind;
    j->n = n;
    return j;
}
void nvn_replay_submit(u32 queue, u32 n, u64 handles, u64* ready, u32* shown) {
    pm_lock(&g_nvn.replay.lock);
    NvnJob* j = job(queue, JOB_SUBMIT, n);
    pm_unlock(&g_nvn.replay.lock);
    Promise p;
    u32 i;
    p.syncpt = nvn_replay_syncpt(queue);
    p.shown = *shown;
    p.ready = *ready;
    for (i = 0; i < n; i++) j->handles[i] = gr64(handles + 8 * (u64)i);
    /* what the CPU wrote before the submission is for its work to see: the renderer's caches check guest memory
       again (gpu.c does the same before a GPFIFO job) */
    if (g_renderer->mem_may_have_changed) g_renderer->mem_may_have_changed();
    pm_lock(&g_nvn.replay.lock);
    for (i = 0; i < n; i++) walk(j->handles[i], promise, &p, 0);
    gpu_call(run, j, NULL, 0);
    pm_unlock(&g_nvn.replay.lock);
    *ready = p.ready;
    *shown = p.shown;
}
void nvn_replay_fence(u32 queue, u64 sync, u64 ready, u32 shown) {
    NvnSync* s = NVN_OBJ(NvnSync, sync);
    pm_lock(&g_nvn.replay.lock);
    s->syncpt = nvn_replay_syncpt(queue);
    s->value = syncpt_promise(s->syncpt, 1);
    s->ready = ready;
    s->shown = shown;
    __atomic_store_n(&s->fenced, 1, __ATOMIC_RELEASE);
    gpu_call(run, job(queue, JOB_FENCE, 0), NULL, 0);
    pm_unlock(&g_nvn.replay.lock);
}
void nvn_replay_finish(u32 queue) {
    u32 id = nvn_replay_syncpt(queue), value;
    pm_lock(&g_nvn.replay.lock);
    value = syncpt_promise(id, 1);
    gpu_call(run, job(queue, JOB_FENCE, 0), NULL, 0);
    pm_unlock(&g_nvn.replay.lock);
    syncpt_wait(id, value, -1);
}
u32 nvn_replay_present(u32 queue, const NvnReplayPresent* p, u32* shown) {
    u32 value;
    pm_lock(&g_nvn.replay.lock);
    NvnJob* j = job(queue, JOB_PRESENT, 0);
    j->present = *p;
    value = syncpt_promise(nvn_replay_syncpt(queue), 1);
    *shown = syncpt_promise(g_nvn.replay.shown, 1); /* the presents reach the display in the order queued here */
    gpu_call(run, j, NULL, 0);
    pm_unlock(&g_nvn.replay.lock);
    return value;
}
