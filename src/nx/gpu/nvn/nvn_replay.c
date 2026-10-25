/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's replay: a queue's submitted recordings run on the GPU thread, in submission order, as a GPFIFO
   submission's methods do (neutral_draw_state's 4f: never at record time, since a recording may be submitted many
   times and what it reads changes between). Each queue keeps the state its commands set, in a GpuChannel of its own
   (the renderer's draw and clear entries take one) whose DrawState the replay fills, as maxwell3d.c fills a channel's.

   What the replay runs so far: the render targets (SET_RT), the clears, and what the CPU sees (counter reports and
   event writes through the renderer's ordered releases, fences as increments of the queue's syncpoint). A sync is
   fenced at submission with the syncpoint value its point will reach, and signals when the GPU thread has replayed
   up to it and the renderer's work before it is done (the stream increment gpu.c makes after a GPFIFO job's work).
   A window present shows the presented texture once the work before it is replayed */
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>

#define NVN_QUEUES 8

/* a queue's replay state: the channel the renderer's entries take (its DrawState the queue's), only touched on the
   GPU thread but for the syncpoint */
typedef struct {
    GpuChannel* ch;
    u32 syncpt;
} ReplayQueue;
/* What the CPU sees of a submission: a fence's syncpoint increment, or bytes the stream writes (counter reports, event
   writes, an event's add of v to what is there) */
enum { FX_FENCE, FX_WRITE, FX_ADD };
typedef struct {
    u32 kind, len;
    u64 gva;
    u8 data[16];
} Effect;
/* the file's state: the queues, the lock that keeps their submissions' promises in the order they are queued, and the
   GPU thread's list of the submission being replayed's effects */
typedef struct {
    ReplayQueue q[NVN_QUEUES];
    u32 n;
    PMutex lock;
    Effect* fx;
    u32 nfx, fx_cap;
} NvnReplayState;
static NvnReplayState g_nvn_replay = {{{0, 0}}, 0, PMUTEX_INIT, NULL, 0, 0};

enum { JOB_SUBMIT, JOB_FENCE, JOB_PRESENT };
typedef struct {
    u32 queue, kind, n;
    u32 w, h, fmt, linear, bhl, pitch;
    i32 crop[4];
    u64 gva;
    u64 handles[]; /* n of them */
} Job;

u32 nvn_replay_queue(void) {
    u32 i;
    pm_lock(&g_nvn_replay.lock);
    if (g_nvn_replay.n == NVN_QUEUES) fatal("nvn: more than %d queues", NVN_QUEUES);
    i = g_nvn_replay.n++;
    g_nvn_replay.q[i].ch = (GpuChannel*)calloc(1, sizeof(GpuChannel));
    if (!g_nvn_replay.q[i].ch) fatal("nvn: out of memory for a queue's state");
    g_nvn_replay.q[i].syncpt = syncpt_alloc();
    g_nvn_replay.q[i].ch->id = g_nvn_replay.q[i].syncpt;
    pm_unlock(&g_nvn_replay.lock);
    return i + 1;
}
u32 nvn_replay_syncpt(u32 queue) { return queue ? g_nvn_replay.q[queue - 1].syncpt : 0; }

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

/* at submission: each fence the recordings hold gets the syncpoint value its point reaches */
typedef struct {
    u32 syncpt;
    u64 ready;
} Promise;
static void promise(void* ctx, u32 op, u64 payload, u32 size) {
    const Promise* pr = (const Promise*)ctx;
    NvnSync* s;
    (void)size;
    if (op != OP_FENCE_SYNC) return;
    s = NVN_OBJ(NvnSync, gr64(payload));
    s->syncpt = pr->syncpt;
    s->value = syncpt_promise(pr->syncpt, 1);
    s->ready = pr->ready;
    __atomic_store_n(&s->fenced, 1, __ATOMIC_RELEASE);
}

/* a syncpoint increment once the renderer's work before it is done (gpu.c's stream_incr) */
static void stream_incr(u32 id) {
    if (g_renderer->stream_incr)
        g_renderer->stream_incr(id);
    else
        syncpt_incr(id);
}

/* a render target from a texture snapshot and its view, as the 3D engine's registers describe one */
static int target(u64 snapshot, const NvnView* v, int has_view, DsSurf* s, int* depth) {
    NvnTexture t;
    NvnTexLayout l;
    u32 format, level = 0, layer = 0, layers = 1;
    unsigned th, td;
    memset(s, 0, sizeof *s);
    if (!snapshot) return 0;
    memcpy(&t, GP(snapshot), sizeof t);
    if (t.magic != NVN_MAGIC('T', 'E', 'X', 0) || !t.gpu) return 0;
    format = has_view && (v->set & NVN_VIEW_FORMAT) ? v->format : t.d.format;
    s->format = nvn_rt_format(format, depth);
    if (!s->format) {
        LOG_LIMIT(8, "nvn: a render target of format %#x, which no target takes\n", format);
        return 0;
    }
    nvn_tex_layout(&t.d, &l);
    if (has_view && (v->set & NVN_VIEW_LEVELS)) level = v->base_level;
    if (t.d.target == NVN_TEXTURE_TARGET_2D_ARRAY || t.d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY ||
        t.d.target == NVN_TEXTURE_TARGET_CUBEMAP || t.d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) {
        layers = t.d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 : t.d.depth;
        if (has_view && (v->set & NVN_VIEW_LAYERS)) {
            layer = v->min_layer;
            layers = v->layers ? v->layers : layers - layer;
        }
    }
    s->gva = t.gpu + nvn_tex_level_offset(&t.d, level) + (u64)layer * l.layer_size;
    s->width = t.d.width >> level ? t.d.width >> level : 1;
    s->height = t.d.height >> level ? t.d.height >> level : 1;
    s->layers = layers;
    s->layer_stride = l.layer_size;
    if (t.d.flags & NVN_TEXTURE_FLAGS_LINEAR) {
        int bw, bh, bpb = nvn_fmt_bpb((int)t.d.format, &bw, &bh);
        u32 align = (t.d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET) ? 128 : 32;
        s->mem = 1u << 12;
        s->width = t.d.stride ? t.d.stride : ((u32)bpb * s->width + align - 1) & ~(align - 1);
    } else {
        nvn_tex_level_tiles(&t.d, level, &th, &td);
        if (t.d.target == NVN_TEXTURE_TARGET_3D) { /* a volume: its slices, the GOB slices of the 3D surface */
            s->layers = t.d.depth >> level ? t.d.depth >> level : 1;
            s->mem |= 1u << 16;
        }
        s->mem |= th << 4 | td << 8;
    }
    return 1;
}

/* on the GPU thread: a record's work */
typedef struct {
    ReplayQueue* q;
} Replay;

/* A sync signals, an event is written, once the work before it is done, and the game may then take back the memory
   of the recordings it covers. The walk still reads records after them (at least the END), which on the console the
   GPU had fetched by then: what the CPU sees of a submission happens once all of it is read, in order, after its
   work, which is later than the console but never earlier */
static void effect(u32 kind, u64 gva, const void* data, u32 len) {
    NvnReplayState* st = &g_nvn_replay;
    Effect* e;
    if (st->nfx == st->fx_cap) {
        u32 cap = st->fx_cap ? st->fx_cap * 2 : 64;
        Effect* fx = (Effect*)realloc(st->fx, cap * sizeof *fx);
        if (!fx) fatal("nvn: out of memory for a submission's effects");
        st->fx = fx;
        st->fx_cap = cap;
    }
    e = &st->fx[st->nfx++];
    e->kind = kind;
    e->gva = gva;
    e->len = len;
    if (len) memcpy(e->data, data, len);
}
static void effects(ReplayQueue* q) {
    NvnReplayState* st = &g_nvn_replay;
    u32 i;
    for (i = 0; i < st->nfx; i++) {
        Effect* e = &st->fx[i];
        if (e->kind == FX_FENCE) {
            stream_incr(q->syncpt);
            continue;
        }
        if (e->kind == FX_ADD) { /* what is there as the stream sees it, past the releases queued before (gmem_read) */
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
    ReplayQueue* q = ((Replay*)ctx)->q;
    GpuChannel* ch = q->ch;
    (void)size;
    switch (op) {
        case OP_SET_RT: {
            NvnPSetRT r;
            DsTargets* tg = &ch->ds.tgt;
            i32 i;
            int depth;
            memcpy(&r, GP(payload), sizeof r);
            memset(tg, 0, sizeof *tg);
            tg->count = r.count > 0 && r.count <= DS_TARGETS_MAX ? (u32)r.count : 0;
            tg->srgb_write = 1; /* NVN writes sRGB targets as sRGB */
            for (i = 0; i < (i32)tg->count; i++)
                if (!target(r.color[i], &r.cview[i], r.has_cview[i], &tg->color[i], &depth) || depth)
                    memset(&tg->color[i], 0, sizeof tg->color[i]);
            if (!target(r.depth, &r.dview, r.has_dview, &tg->depth, &depth) || !depth)
                memset(&tg->depth, 0, sizeof tg->depth);
            break;
        }
        case OP_CLEAR_COLOR: {
            NvnPClearColor r;
            DrawClear c;
            memcpy(&r, GP(payload), sizeof r);
            memset(&c, 0, sizeof c);
            c.color_mask = (u32)r.mask & 15;
            c.slot = (u32)r.index;
            memcpy(c.color, r.rgba, sizeof c.color);
            if (c.color_mask && g_renderer->clear) g_renderer->clear(&ch->ds, &c, ch);
            break;
        }
        case OP_CLEAR_DS: {
            NvnPClearDS r;
            DrawClear c;
            memcpy(&r, GP(payload), sizeof r);
            memset(&c, 0, sizeof c);
            c.aspects = (r.depth_mask ? 1u : 0) | (r.stencil_mask ? 2u : 0);
            c.depth = r.depth;
            c.stencil = (u32)r.stencil;
            if (c.aspects && g_renderer->clear) g_renderer->clear(&ch->ds, &c, ch);
            break;
        }
        case OP_FENCE_SYNC: effect(FX_FENCE, 0, NULL, 0); break;
        case OP_REPORT_COUNTER: {
            /* {counter, timestamp}: the counter is the timestamp for the timestamp types, 0 for the others, which
               count nothing drawn yet */
            const NvnPReportCounter* r = NVN_OBJ(const NvnPReportCounter, payload);
            u64 v[2];
            v[1] = guest_ticks();
            v[0] = r->type == NVN_COUNTER_TYPE_TIMESTAMP || r->type == NVN_COUNTER_TYPE_TIMESTAMP_TOP ? v[1] : 0;
            effect(FX_WRITE, r->address, v, sizeof v);
            break;
        }
        case OP_SIGNAL_EVENT: {
            const NvnPSignalEvent* e = NVN_OBJ(const NvnPSignalEvent, payload);
            effect(e->mode == NVN_EVENT_SIGNAL_MODE_ADD ? FX_ADD : FX_WRITE, e->value, &e->v, sizeof e->v);
            break;
        }
        default: break; /* the draws and the rest: not replayed yet */
    }
}

static void run(void* arg) {
    Job* j = (Job*)arg;
    ReplayQueue* q = &g_nvn_replay.q[j->queue - 1];
    switch (j->kind) {
        case JOB_SUBMIT: {
            Replay r;
            u32 i;
            r.q = q;
            if (g_renderer->stream_run) g_renderer->stream_run(1);
            for (i = 0; i < j->n; i++) walk(j->handles[i], replay, &r, 0);
            effects(q);
            if (g_renderer->stream_run) g_renderer->stream_run(0);
            break;
        }
        case JOB_FENCE: stream_incr(q->syncpt); break;
        case JOB_PRESENT: {
            u64 cpu = j->gva ? gmmu_translate(j->gva) : 0;
            if (g_renderer_up && cpu)
                g_renderer->present_buffer(cpu, j->w, j->h, j->pitch, j->linear ? 0 : 3, j->bhl, j->fmt, j->crop, 0);
            else if (g_renderer_up) /* nothing to show: black, which still ends the renderer's frame */
                g_renderer->present_buffer(0, 0, 0, 0, 0, 0, 0, j->crop, 0);
            stream_incr(q->syncpt); /* shown: the window's texture comes free (nvn_WindowAcquireTexture) */
            break;
        }
    }
    /* the stream stops here, as gpu.c's job_end: the renderer holds a stream's releases (its increments and writes)
       until then (release.c), and the game may now look at them */
    if (g_renderer->stream_end) g_renderer->stream_end();
    free(j);
}

static Job* job(u32 queue, u32 kind, u32 n) {
    Job* j = (Job*)calloc(1, sizeof *j + n * sizeof j->handles[0]);
    if (!j) fatal("nvn: out of memory queueing a submission");
    j->queue = queue;
    j->kind = kind;
    j->n = n;
    return j;
}
void nvn_replay_submit(u32 queue, u32 n, u64 handles, u64 ready) {
    Job* j = job(queue, JOB_SUBMIT, n);
    Promise p;
    u32 i;
    p.syncpt = nvn_replay_syncpt(queue);
    p.ready = ready;
    for (i = 0; i < n; i++) j->handles[i] = gr64(handles + 8 * (u64)i);
    pm_lock(&g_nvn_replay.lock);
    for (i = 0; i < n; i++) walk(j->handles[i], promise, &p, 0);
    gpu_call(run, j, NULL, 0);
    pm_unlock(&g_nvn_replay.lock);
}
void nvn_replay_fence(u32 queue, u64 sync, u64 ready) {
    NvnSync* s = NVN_OBJ(NvnSync, sync);
    pm_lock(&g_nvn_replay.lock);
    s->syncpt = nvn_replay_syncpt(queue);
    s->value = syncpt_promise(s->syncpt, 1);
    s->ready = ready;
    __atomic_store_n(&s->fenced, 1, __ATOMIC_RELEASE);
    gpu_call(run, job(queue, JOB_FENCE, 0), NULL, 0);
    pm_unlock(&g_nvn_replay.lock);
}
void nvn_replay_finish(u32 queue) {
    u32 id = nvn_replay_syncpt(queue), value;
    pm_lock(&g_nvn_replay.lock);
    value = syncpt_promise(id, 1);
    gpu_call(run, job(queue, JOB_FENCE, 0), NULL, 0);
    pm_unlock(&g_nvn_replay.lock);
    syncpt_wait(id, value, -1);
}
u32 nvn_replay_present(u32 queue, u64 gva, u32 w, u32 h, u32 fmt, u32 linear, u32 bhl, u32 pitch, const i32 crop[4]) {
    Job* j = job(queue, JOB_PRESENT, 0);
    u32 value;
    j->gva = gva;
    j->w = w;
    j->h = h;
    j->fmt = fmt;
    j->linear = linear;
    j->bhl = bhl;
    j->pitch = pitch;
    memcpy(j->crop, crop, sizeof j->crop);
    pm_lock(&g_nvn_replay.lock);
    value = syncpt_promise(nvn_replay_syncpt(queue), 1);
    gpu_call(run, j, NULL, 0);
    pm_unlock(&g_nvn_replay.lock);
    return value;
}
