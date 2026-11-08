/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn's replay: a queue's submitted recordings run on the GPU thread, in submission order, as a GPFIFO
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
#include "nx/hle/pool.h"
#include "nx/runtime/options.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>

/* What the CPU sees of a submission: a fence's syncpoint increment, or bytes the stream writes (counter reports, event
   writes, an event's add of v to what is there) */
enum { FX_FENCE, FX_WRITE, FX_ADD };

enum { JOB_SUBMIT, JOB_FENCE, JOB_PRESENT, JOB_DESTROY, JOB_WAIT };

u32 nvn_replay_queue(void) {
    u32 i;
    pm_lock(&g_nvn.replay.lock);
    for (i = 0; i < NVN_QUEUES && g_nvn.replay.q[i].syncpt; i++) {}
    if (i == NVN_QUEUES) fatal("nvn: more than %d live queues", NVN_QUEUES);
    if (!g_nvn.replay.shown) g_nvn.replay.shown = syncpt_alloc();
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
   the recording's last record (or the empty recording's END) returns */
static void walk(u64 p, RecordFn fn, void* ctx, int depth, int sync_only) {
    if (depth > 8) fatal("nvn: recordings call each other more than 8 deep (at %llx)", (unsigned long long)p);
    u32 prev = OP_BEGIN;
    u64 start = p, piece = p; /* piece: where the records walked now start, its handle or the last JUMP's target */
    for (;;) {
        NvnRecHdr h;
        u32 op;
        memcpy(&h, GP(p), sizeof h);
        __atomic_thread_fence(__ATOMIC_ACQUIRE); /* a JUMP's target is written before its header (nvn_cmd.c) */
        op = NVN_REC_OP(h.op);
        if (op >= OP_COUNT_ || h.size < 8 || (h.size & 7)) {
            char where[512];
            nvn_cmd_describe(piece, where, sizeof where);
            fatal("nvn: a command record at %llx (after a record of op %u) has op %u and size %u, which nvn "
                  "never writes%s",
                  (unsigned long long)p, prev, op, h.size, where);
        }
        prev = op;
        if (op == OP_END) return;
        if (op == OP_JUMP || op == OP_CALL) {
            u64 to = gr64(p + 8);
            /* A game may record over a recording while another of its threads still submits it: the console's GPU
               then runs the commands both left there. Past a jump into a piece still cut from, or to where no record
               can start (a record the new one is writing now), nothing is final */
            if (!to || (to & 7) || (op == OP_JUMP && nvn_cmd_unfinished(to))) {
                LOG_LIMIT(4,
                          "nvn: the submitted recording %llx goes on in records its command buffer is writing now "
                          "(%s to %llx): the game records over a recording it submits, which ends there\n",
                          (unsigned long long)start, op == OP_JUMP ? "a jump" : "a call", (unsigned long long)to);
                return;
            }
            if (op == OP_JUMP) {
                p = piece = to;
                continue;
            }
            walk(to, fn, ctx, depth + 1, sync_only);
        } else if (!sync_only || op == OP_WAIT_SYNC || op == OP_WAIT_EVENT || op == OP_FENCE_SYNC)
            fn(ctx, op, p + 8, h.size - 8);
        if (h.op & NVN_REC_LAST) return;
        p += h.size;
    }
}

void nvn_sync_hold(u64* ready, u32* shown, const NvnSync* s) {
    if (!__atomic_load_n(&s->fenced, __ATOMIC_ACQUIRE)) return;
    if (s->ready > *ready) *ready = s->ready;
    if (!*shown || (s->shown && (i32)(s->shown - *shown) > 0)) *shown = s->shown;
}

/* At submission each fence gets its promised value. Capture waits in record order: another fence can reuse the
   same guest sync object before replay reaches the wait. Only submissions with waits retain command bytes. */
typedef struct {
    u32 syncpt, shown;
    u64 ready;
    NvnReplayWait* waits;
    u32 nwaits, capacity;
    int has_wait;
} Promise;
static void promise(void* ctx, u32 op, u64 payload, u32 size) {
    Promise* pr = (Promise*)ctx;
    NvnSync* s;
    (void)size;
    if (op == OP_WAIT_EVENT) {
        pr->has_wait = 1;
        return;
    }
    if (op == OP_WAIT_SYNC) {
        s = NVN_OBJ(NvnSync, gr64(payload));
        int fenced = __atomic_load_n(&s->fenced, __ATOMIC_ACQUIRE);
        if (!fenced)
            LOG_LIMIT(4, "nvn: a recording waits for sync %llx, which nothing fenced before its submission\n",
                      (unsigned long long)gr64(payload));
        nvn_sync_hold(&pr->ready, &pr->shown, s);
        if (pr->nwaits == pr->capacity) {
            u32 capacity = pr->capacity ? pr->capacity * 2 : 8;
            if (capacity < pr->capacity || capacity > UINT32_MAX / sizeof *pr->waits) fatal("nvn: too many sync waits");
            NvnReplayWait* grown = realloc(pr->waits, capacity * sizeof *grown);
            if (!grown) fatal("nvn: no memory for sync waits");
            pr->waits = grown;
            pr->capacity = capacity;
        }
        pr->waits[pr->nwaits++] = fenced ? (NvnReplayWait){s->syncpt, s->value} : (NvnReplayWait){0};
        pr->has_wait = 1;
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

/* Fences before a wait must be able to signal. Retain these submissions before publishing a fence, so command
   memory can be reused without invalidating a suspended cursor. Flatten CALL/JUMP records in their execution order;
   resource addresses in payloads still refer to guest memory and are read at execution time. */
typedef struct {
    u8* data;
    size_t size, capacity;
    const NvnReplayWait* waits;
    u32 wait, nwaits;
} Snapshot;
static void snapshot(void* ctx, u32 op, u64 payload, u32 size) {
    Snapshot* s = ctx;
    size_t record_size = (size_t)size + sizeof(NvnRecHdr);
    if (s->size > SIZE_MAX - record_size) fatal("nvn: command snapshot is too large");
    size_t end = s->size + record_size;
    if (end > s->capacity) {
        size_t capacity = s->capacity ? s->capacity : 4096;
        while (capacity < end) {
            if (capacity > SIZE_MAX / 2) {
                capacity = end;
                break;
            }
            capacity *= 2;
        }
        u64 old = (u64)(uintptr_t)s->data - (u64)(uintptr_t)g_mem;
        u8* grown = realloc(s->data, capacity);
        if (!grown) fatal("nvn: no memory for waiting command snapshot");
        s->data = grown;
        s->capacity = capacity;
        u64 base = (u64)(uintptr_t)grown - (u64)(uintptr_t)g_mem;
        if (base != old)
            for (size_t at = 0; at < s->size;) {
                NvnRecHdr h;
                memcpy(&h, grown + at, sizeof h);
                nvn_record_relocate(base + at, old + at, h.op, h.size);
                at += h.size;
            }
    }
    NvnRecHdr h = {op, (u32)record_size};
    memcpy(s->data + s->size, &h, sizeof h);
    if (op == OP_WAIT_SYNC) {
        if (size < sizeof(NvnReplayWait) || s->wait >= s->nwaits) fatal("nvn: invalid sync wait snapshot");
        memcpy(s->data + s->size + sizeof h, &s->waits[s->wait++], sizeof(NvnReplayWait));
    } else if (size)
        memcpy(s->data + s->size + sizeof h, GP(payload), size);
    nvn_record_relocate((u64)(uintptr_t)(s->data + s->size) - (u64)(uintptr_t)g_mem, payload - sizeof h, op, h.size);
    s->size = end;
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
        fatal("nvn: a submission signals more than %u fences and events, which nvn holds", NVN_EFFECTS);
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
        case OP_REPORT_COUNTER: {
            /* {counter, timestamp}, written in order with the stream, as the 3D engine's reports are (a render enable
               later in the same submission reads it, maxwell/engines.c): samples passed as the renderer counts the
               queue's draws, else a made-up count where it can't count; the timestamp types put 0 in the counter, as
               the game's driver writes them (a report of payload 0). One game makes each conditional clear of a target
               run once a frame that way: it sets the counter to 1, clears when it differs from the word 16 bytes on,
               and then reports a timestamp over it */
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

/* Replay resumes at an unmet event or captured sync wait. Waiting submissions retain flattened command records;
   direct submissions keep CALL cursors so nested recordings do not repeat earlier draws. */
static int replay_records(NvnJob* j, NvnReplayQueue* q) {
    Replay replay_context = {.q = q};
    while (j->handle < j->n) {
        if (!j->depth) {
            j->cursor[0].pc = j->cursor[0].piece = j->handles[j->handle];
            j->cursor[0].previous = OP_BEGIN;
            j->depth = 1;
        }
        while (j->depth) {
            u32 level = j->depth - 1;
            u64 pc = j->cursor[level].pc;
            if (!pc) {
                j->depth--;
                continue;
            }
            NvnRecHdr h;
            memcpy(&h, GP(pc), sizeof h);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            u32 op = NVN_REC_OP(h.op);
            if (op >= OP_COUNT_ || h.size < 8 || (h.size & 7))
                fatal("nvn: invalid resumed record at %llx: op %u size %u after %u", (unsigned long long)pc, op, h.size,
                      j->cursor[level].previous);
            if (op == OP_WAIT_EVENT || op == OP_WAIT_SYNC) {
                effects(q);
                if (!j->waiting && g_renderer->stream_end) g_renderer->stream_end();
                int met;
                if (op == OP_WAIT_SYNC) {
                    NvnReplayWait wait;
                    if (!j->snapshot || h.size < sizeof h + sizeof wait) fatal("nvn: invalid captured sync wait");
                    memcpy(&wait, GP(pc + 8), sizeof wait);
                    met = !wait.syncpt || (i32)(syncpt_read(wait.syncpt) - wait.value) >= 0;
                } else {
                    if (h.size < sizeof h + sizeof(NvnPWaitEvent)) fatal("nvn: truncated event wait");
                    NvnPWaitEvent event;
                    memcpy(&event, GP(pc + 8), sizeof event);
                    /* A preceding signal in this recording must be submitted before its wait can see it. Other NVN
                   queues can then signal the event while this job stays at its cursor. */
                    u32 current = gmem_r32(event.address);
                    met = event.mode == NVN_EVENT_WAIT_MODE_EQUAL
                              ? current == event.value
                              : event.mode == NVN_EVENT_WAIT_MODE_GEQUAL_WRAP && (i32)(current - event.value) >= 0;
                }
                if (!met) {
                    j->waiting = 1;
                    return 0;
                }
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                j->waiting = 0;
                if (g_renderer->mem_may_have_changed) g_renderer->mem_may_have_changed();
            }
            j->cursor[level].previous = op;
            j->cursor[level].pc = h.op & NVN_REC_LAST ? 0 : pc + h.size;
            if (op == OP_END) {
                j->depth--;
                continue;
            }
            if (op == OP_JUMP || op == OP_CALL) {
                u64 to = gr64(pc + 8);
                if (!to || (to & 7) || (op == OP_JUMP && nvn_cmd_unfinished(to))) {
                    LOG_LIMIT(4, "nvn: a recording reaches unfinished command memory at %llx\n",
                              (unsigned long long)to);
                    j->depth--;
                    continue;
                }
                if (op == OP_CALL) {
                    if (j->depth == 9) fatal("nvn: recordings call each other more than 8 deep");
                    level = j->depth++;
                }
                j->cursor[level].pc = j->cursor[level].piece = to;
                j->cursor[level].previous = OP_BEGIN;
            } else if (op != OP_WAIT_EVENT && op != OP_WAIT_SYNC)
                replay(&replay_context, op, pc + 8, h.size - 8);
        }
        j->handle++;
    }
    return 1;
}
static int run(void* arg) {
    NvnJob* j = (NvnJob*)arg;
    NvnReplayQueue* q = &g_nvn.replay.q[j->queue - 1];
    switch (j->kind) {
        case JOB_SUBMIT: {
            if (g_renderer->stream_run) g_renderer->stream_run(1);
            int done = replay_records(j, q);
            effects(q);
            if (g_renderer->stream_run) g_renderer->stream_run(0);
            if (!done) return 0;
            break;
        }
        case JOB_WAIT:
            if (j->wait.syncpt && (i32)(syncpt_read(j->wait.syncpt) - j->wait.value) < 0) {
                if (!j->waiting && g_renderer->stream_end) g_renderer->stream_end();
                j->waiting = 1;
                return 0;
            }
            break;
        case JOB_FENCE: stream_incr(q->syncpt); break;
        case JOB_DESTROY:
            nvn_draw_free(&q->d);
            stream_incr(q->syncpt);
            break;
        case JOB_PRESENT: {
            if (j->present_order != g_nvn.replay.present_walked + 1) return 0;
            const NvnReplayPresent* p = &j->present;
            u64 cpu = p->gva ? gmmu_translate(p->gva) : 0;
            if (g_renderer_up && cpu)
                g_renderer->present_buffer(cpu, p->w, p->h, p->pitch, p->linear ? 0 : 3, p->bhl, p->fmt, p->crop,
                                           p->transform, p->released_ns, p->period_ns, g_nvn.replay.shown);
            else if (g_renderer_up) /* nothing to show: black, which still ends the renderer's frame */
                g_renderer->present_buffer(0, 0, 0, 0, 0, 0, 0, p->crop, 0, p->released_ns, p->period_ns,
                                           g_nvn.replay.shown);
            else /* no display to reach */
                syncpt_incr(g_nvn.replay.shown);
            stream_incr(q->syncpt); /* shown: the window's texture comes free (nvn_WindowAcquireTexture) */
            g_nvn.replay.present_walked = j->present_order;
            break;
        }
    }
    /* the stream stops here, as gpu.c's job_end: the renderer holds a stream's releases (its increments and writes)
       until then (release.c), and the game may now look at them */
    if (g_renderer->stream_end) g_renderer->stream_end();
    free(j->snapshot);
    j->snapshot = NULL;
    pm_lock(&g_nvn.replay.lock);
    j->serial = 0;
    POOL_PUT(j);
    /* A later queue can finish while an older one waits. Reclaim command spill memory only through the first
       unfinished submission, never by the number of jobs that happened to complete out of order. */
    u64 frontier = __atomic_load_n(&g_nvn.replay.submitted, __ATOMIC_RELAXED);
    for (u32 i = 0; i < NVN_JOBS; i++) {
        u64 serial = g_nvn.replay.jobs[i].serial;
        if (serial && serial <= frontier) frontier = serial - 1;
    }
    __atomic_store_n(&g_nvn.replay.walked, frontier, __ATOMIC_RELEASE);
    pm_unlock(&g_nvn.replay.lock);
    return 1;
}

/* work for the GPU thread, from the pool (under g_nvn.replay.lock): non-zero before the lock is let go, as its queue
   is 1 up */
static NvnJob* job(u32 queue, u32 kind, u32 n) {
    if (n > NVN_JOB_HANDLES)
        fatal("nvn: a submission of %u command handles, more than the %u nvn holds", n, NVN_JOB_HANDLES);
    NvnJob* j = POOL_TAKE(g_nvn.replay.jobs, g_nvn.replay.next_job, "NVN jobs");
    if (!j)
        fatal("nvn: more than %u submissions, fences and presents queued for the GPU thread at once, which nvn "
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
    Promise p = {0};
    u32 i;
    p.syncpt = nvn_replay_syncpt(queue);
    p.shown = *shown;
    p.ready = *ready;
    for (i = 0; i < n; i++) j->handles[i] = gr64(handles + 8 * (u64)i);
    /* what the CPU wrote before the submission is for its work to see: the renderer's caches check guest memory
       again (gpu.c does the same before a GPFIFO job) */
    if (g_renderer->mem_may_have_changed) g_renderer->mem_may_have_changed();
    pm_lock(&g_nvn.replay.lock);
    /* Every header and nested call is still read: the guest can rewrite command memory between submissions.
       Only waits and fences affect promises, so ordinary records need no callback in this pass. */
    for (i = 0; i < n; i++) walk(j->handles[i], promise, &p, 0, 1);
    if (p.has_wait) {
        Snapshot s = {.waits = p.waits, .nwaits = p.nwaits};
        for (i = 0; i < n; i++) walk(j->handles[i], snapshot, &s, 0, 0);
        snapshot(&s, OP_END, 0, 0);
        if (s.wait != p.nwaits) fatal("nvn: command memory changed its sync waits during submission");
        j->snapshot = s.data;
        j->handles[0] = (u64)(uintptr_t)s.data - (u64)(uintptr_t)g_mem;
        j->n = 1;
    }
    free(p.waits);
    j->serial = __atomic_add_fetch(&g_nvn.replay.submitted, 1, __ATOMIC_RELEASE);
    gpu_call_poll(run, j, queue - 1);
    pm_unlock(&g_nvn.replay.lock);
    *ready = p.ready;
    *shown = p.shown;
}
u64 nvn_replay_submitted(void) { return __atomic_load_n(&g_nvn.replay.submitted, __ATOMIC_ACQUIRE); }
u64 nvn_replay_walked(void) { return __atomic_load_n(&g_nvn.replay.walked, __ATOMIC_ACQUIRE); }
void nvn_replay_wait(u32 queue, const NvnSync* s) {
    if (!__atomic_load_n(&s->fenced, __ATOMIC_ACQUIRE) || !s->syncpt) return;
    pm_lock(&g_nvn.replay.lock);
    NvnJob* j = job(queue, JOB_WAIT, 0);
    j->wait = (NvnReplayWait){s->syncpt, s->value};
    gpu_call_poll(run, j, queue - 1);
    pm_unlock(&g_nvn.replay.lock);
}
void nvn_replay_fence(u32 queue, u64 sync, u64 ready, u32 shown) {
    NvnSync* s = NVN_OBJ(NvnSync, sync);
    pm_lock(&g_nvn.replay.lock);
    s->syncpt = nvn_replay_syncpt(queue);
    s->value = syncpt_promise(s->syncpt, 1);
    s->ready = ready;
    s->shown = shown;
    __atomic_store_n(&s->fenced, 1, __ATOMIC_RELEASE);
    gpu_call_poll(run, job(queue, JOB_FENCE, 0), queue - 1);
    pm_unlock(&g_nvn.replay.lock);
}
void nvn_replay_queue_free(u32 queue) {
    u32 id = nvn_replay_syncpt(queue), value;
    pm_lock(&g_nvn.replay.lock);
    value = syncpt_promise(id, 1);
    gpu_call_poll(run, job(queue, JOB_DESTROY, 0), queue - 1);
    pm_unlock(&g_nvn.replay.lock);
    syncpt_wait(id, value, -1);
    pm_lock(&g_nvn.replay.lock);
    syncpt_free(id);
    g_nvn.replay.q[queue - 1] = (NvnReplayQueue){0};
    pm_unlock(&g_nvn.replay.lock);
}
void nvn_replay_finish(u32 queue) {
    u32 id = nvn_replay_syncpt(queue), value;
    pm_lock(&g_nvn.replay.lock);
    value = syncpt_promise(id, 1);
    gpu_call_poll(run, job(queue, JOB_FENCE, 0), queue - 1);
    pm_unlock(&g_nvn.replay.lock);
    syncpt_wait(id, value, -1);
}
u32 nvn_replay_present(u32 queue, const NvnReplayPresent* p, u32* shown) {
    u32 value;
    pm_lock(&g_nvn.replay.lock);
    NvnJob* j = job(queue, JOB_PRESENT, 0);
    j->present = *p;
    j->present_order = ++g_nvn.replay.present_submitted;
    value = syncpt_promise(nvn_replay_syncpt(queue), 1);
    *shown = syncpt_promise(g_nvn.replay.shown, 1); /* the presents reach the display in the order queued here */
    gpu_call_poll(run, j, queue - 1);
    pm_unlock(&g_nvn.replay.lock);
    return value;
}
