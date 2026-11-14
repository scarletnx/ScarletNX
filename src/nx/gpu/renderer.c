/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The renderer state the GPU front end and the renderer share (renderer.h) */
#include "nx/gpu/renderer.h"
#include "nx/gpu/gpu_queue.h"
#include <stddef.h>
#include <string.h>
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

int g_renderer_up;
u64 g_draws;
u64 g_tex_cache_hits, g_tex_cache_misses, g_tex_upload_bytes, g_tex_frees;
/* set by the renderer while it holds results not yet in guest memory, read by every reader of guest memory: a reader
   that sees 0 has nothing to land. The compute and graphics sides set theirs from different threads: a cache line
   each */
_Alignas(64) _Atomic int g_cs_any;
_Alignas(64) _Atomic int g_gfx_any;
_Alignas(64) _Atomic int g_uma_any;

/* The renderer's copy of each front end's draw state, which the renderer reads in place of the front end's own, and
   of each owner (DrawOwner). A front end gives a group a new generation whenever its bytes change (DsState.gen), so a
   draw's record (gpu_queue.c) takes only the groups whose generation moved: at most 4.7 KB of 40 KB a draw, as a GPU
   channel never changes its inline blocks. The renderer's side reads a state that the front end's next methods don't
   change, which a renderer thread of its own needs.

   The front end's side keeps, for each state, the generations it sent (g_ds_sent). The renderer's side keeps the copy
   and the owner (g_ds_copy), in the same slot. The owner's predicate is the front end's and goes with each record.
   The capture buffers whose start the front end set (xfb_reload) go with the next record, and the renderer's side
   takes them in: the renderer clears them in its own copy as their capture starts over */
#define DS_COPIES 8
static struct {
    const DsState* src; /* the front end's state, NULL for an unused slot */
    u32 used;           /* g_ds_sent_use when last asked for: the slot used longest ago makes room */
    u64 gen[DS_GENS];   /* the generations sent to the renderer's side */
} g_ds_sent[DS_COPIES];
static u32 g_ds_sent_use;
static struct {
    DsState copy;
    DrawOwner own;
} g_ds_copy[DS_COPIES];
/* each group's bytes, in DsState's order (draw_state.h): DS_TARGET_FORMATS has only a generation */
static const size_t g_ds_group_at[DS_GENS + 1] = {
    offsetof(DsState, prog), offsetof(DsState, vfmt), offsetof(DsState, blend), offsetof(DsState, rkey),
    offsetof(DsState, xfb),  offsetof(DsState, tgt),  offsetof(DsState, dyn),   offsetof(DsState, vp),
    offsetof(DsState, vbuf), offsetof(DsState, index), offsetof(DsState, bind), offsetof(DsState, inl),
    offsetof(DsState, gen),  offsetof(DsState, gen)};
static const char* const g_ds_group_name[DS_GENS] = {"programs", "vertex format", "blend", "raster key",
                                                      "transform feedback", "targets", "dynamic", "viewports",
                                                      "vertex buffers", "index", "bindings", "inline",
                                                      "target formats"};
enum { DR_DRAW, DR_CLEAR, DR_TEXTURE, DR_DISCARD };
/* a record of a call with a draw state: then the bytes of the groups in groups, in their order, then indices words */
typedef struct {
    u32 kind;    /* DR_ */
    u32 slot;    /* the state's slot */
    u32 groups;  /* the groups whose bytes follow (bit per DS_ group) */
    u32 fresh;   /* a new state in the slot: its owner starts over */
    u32 pred;    /* the owner's predicate */
    u32 reload;  /* capture buffers whose start the front end set since the last record */
    u32 owner;   /* DsState.owner */
    u32 indices; /* the inline index words after the groups */
    u64 gen[DS_GENS];
    union {
        DrawCall dc;
        DrawClear c;
        DrawTexture t;
        u32 targets;
    } call;
} DsRecord;
/* the renderer's side: the record into its copy, then the renderer's call */
static void ds_run(const void* arg, u64 bytes) {
    DsRecord r;
    memcpy(&r, arg, sizeof r);
    const u8* p = (const u8*)arg + sizeof r;
    DsState* c = &g_ds_copy[r.slot].copy;
    DrawOwner* own = &g_ds_copy[r.slot].own;
    if (r.fresh) *own = (DrawOwner){0};
    for (u32 g = 0; g < DS_GENS; g++)
        if (r.groups >> g & 1) {
            size_t n = g_ds_group_at[g + 1] - g_ds_group_at[g];
            memcpy((u8*)c + g_ds_group_at[g], p, n);
            p += n;
        }
    c->owner = r.owner;
    memcpy(c->gen, r.gen, sizeof c->gen);
    /* a render enable of the 3D engine is decided on this side (maxwell/engines.c): its draws and clears are skipped
       while it turns them off, and they take its predicate */
    u32 pred = r.pred;
    int off = gpu_render_state(r.owner, &pred);
    own->pred = pred;
    own->xfb_reload = (u8)(own->xfb_reload | r.reload);
    if ((u64)(p - (const u8*)arg) + (u64)r.indices * 4 > bytes) fatal("gpu: a draw record is short\n");
    if (off == 1 && (r.kind == DR_DRAW || r.kind == DR_CLEAR)) return;
    switch (r.kind) {
        case DR_DRAW: {
            DrawCall dc = r.call.dc;
            /* the words after the groups: their offset is a multiple of 4, which a u32 needs */
            if (dc.indexed == 3) dc.inline_indices = r.indices ? (const u32*)(const void*)p : NULL;
            g_renderer->draw(c, &dc, own);
            break;
        }
        case DR_CLEAR: g_renderer->clear(c, &r.call.c, own); break;
        case DR_TEXTURE: g_renderer->draw_texture(c, &r.call.t); break;
        default: g_renderer->discard(c, r.call.targets); break;
    }
}
/* the front end's side: a record of a call with ds, o's predicate and its new capture starts, and the inline indices,
   with the call's own bytes at call */
static void ds_queue(u32 kind, const DsState* ds, DrawOwner* o, const void* call, size_t call_bytes,
                     const u32* indices, u32 nindices) {
    u32 at = 0, fresh = 0;
    while (at < DS_COPIES && g_ds_sent[at].src != ds) at++;
    if (at == DS_COPIES) { /* a new front end's state: sent whole */
        at = 0;
        for (u32 i = 1; i < DS_COPIES && g_ds_sent[at].src; i++)
            if (!g_ds_sent[i].src || (i32)(g_ds_sent[i].used - g_ds_sent[at].used) < 0) at = i;
        g_ds_sent[at].src = ds;
        fresh = 1;
    }
    g_ds_sent[at].used = ++g_ds_sent_use;
    u32 groups = 0;
    u64 bytes = sizeof(DsRecord) + (u64)nindices * 4;
    for (u32 g = 0; g < DS_GENS; g++)
        if (fresh || g_ds_sent[at].gen[g] != ds->gen[g]) {
            groups |= 1u << g;
            bytes += g_ds_group_at[g + 1] - g_ds_group_at[g];
        }
    u8* p = (u8*)gq_call_begin(ds_run, bytes);
    DsRecord r = {.kind = kind,
                  .slot = at,
                  .groups = groups,
                  .fresh = fresh,
                  .pred = o ? o->pred : 0,
                  .reload = o ? o->xfb_reload : 0,
                  .owner = ds->owner,
                  .indices = nindices};
    memcpy(r.gen, ds->gen, sizeof r.gen);
    memcpy(&r.call, call, call_bytes);
    memcpy(p, &r, sizeof r);
    p += sizeof r;
    for (u32 g = 0; g < DS_GENS; g++)
        if (groups >> g & 1) {
            size_t n = g_ds_group_at[g + 1] - g_ds_group_at[g];
            memcpy(p, (const u8*)ds + g_ds_group_at[g], n);
            p += n;
        }
    if (nindices) memcpy(p, indices, (size_t)nindices * 4);
    memcpy(g_ds_sent[at].gen, ds->gen, sizeof ds->gen);
    if (o) o->xfb_reload = 0; /* the renderer's side has them now */
    gq_call_end();
    /* draw_state_check: a group whose bytes changed under the same generation would draw the old bytes. The copy is
       compared once the renderer's side ran the record */
    if (g_settings.draw_state_check) {
        gq_drain();
        if (memcmp(&g_ds_copy[at].copy, ds, sizeof *ds)) {
            for (u32 g = 0; g < DS_GENS; g++)
                if (memcmp((u8*)&g_ds_copy[at].copy + g_ds_group_at[g], (const u8*)ds + g_ds_group_at[g],
                           g_ds_group_at[g + 1] - g_ds_group_at[g]))
                    LOG_LIMIT(16, "gpu: owner %x changed the %s of its draw state and kept its generation\n",
                              ds->owner, g_ds_group_name[g]);
            g_ds_sent[at].src = NULL; /* sent whole at its next call */
        }
    }
}
void gpu_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    if (!g_renderer->draw) return;
    /* a draw predicated on the GPU (gpu.h render_pred) counts too: whether it runs isn't known here without the wait
       the predicate is there to avoid, and the counters made up from this (maxwell/engines.c counter_report) only
       stand in for what the console counts, growing with the work given */
    g_draws++;
    u32 n = dc->indexed == 3 && dc->inline_indices ? dc->first + dc->count : 0;
    ds_queue(DR_DRAW, ds, o, dc, sizeof *dc, dc->inline_indices, n);
}
void gpu_clear(const DsState* ds, const DrawClear* c, const DrawOwner* o) {
    if (!g_renderer->clear) return;
    DrawOwner own = o ? *o : (DrawOwner){0};
    own.xfb_reload = 0; /* a clear's owner is only read: its capture starts go with the next draw */
    ds_queue(DR_CLEAR, ds, &own, c, sizeof *c, NULL, 0);
}
void gpu_draw_texture(const DsState* ds, const DrawTexture* t) {
    if (g_renderer->draw_texture) ds_queue(DR_TEXTURE, ds, NULL, t, sizeof *t, NULL, 0);
}
void gpu_discard(const DsState* ds, u32 targets) {
    if (g_renderer->discard) ds_queue(DR_DISCARD, ds, NULL, &targets, sizeof targets, NULL, 0);
}
static void dispatch_run(const void* arg, u64 bytes) {
    DispatchState d;
    (void)bytes;
    memcpy(&d, arg, sizeof d);
    g_renderer->dispatch(&d);
}
void gpu_dispatch(const DispatchState* d) {
    if (!g_renderer->dispatch) return;
    if (g_settings.compute_dispatches) r3d_pending_flush(); /* a merged draw the front end is holding comes first */
    gq_call(dispatch_run, d, sizeof *d);
}
/* the front end's states were replaced (a state load): each is sent whole again, and each owner starts over */
void gpu_draw_states_reset(void) {
    gq_drain();
    gpu_render_states_reset();
    for (u32 i = 0; i < DS_COPIES; i++) {
        g_ds_sent[i].src = NULL;
        g_ds_copy[i].own = (DrawOwner){0};
    }
}

/* The renderer's calls without a result that the front end makes often: each a record in its place in the queue */
enum { RC_BARRIER, RC_STREAM_INCR, RC_STREAM_END, RC_READY, RC_STREAM_RUN, RC_SAMPLES_RESET, RC_SAMPLES_ENABLE,
       RC_MEM_SEEN, RC_PREDICATE_DROP };
typedef struct {
    u32 op; /* RC_ */
    u32 a, b;
} RendererCall;
static void call_run(const void* arg, u64 bytes) {
    RendererCall c;
    (void)bytes;
    memcpy(&c, arg, sizeof c);
    switch (c.op) {
        case RC_BARRIER: g_renderer->method_barrier((int)c.a); break;
        case RC_STREAM_INCR: g_renderer->stream_incr(c.a); break;
        case RC_STREAM_END: g_renderer->stream_end(); break;
        case RC_READY: g_renderer->ready(); break;
        case RC_STREAM_RUN: g_renderer->stream_run((int)c.a); break;
        case RC_SAMPLES_RESET: g_renderer->samples_reset(c.a); break;
        case RC_SAMPLES_ENABLE: g_renderer->samples_enable(c.a, (int)c.b); break;
        case RC_MEM_SEEN:
            if (g_renderer->mem_seen)
                g_renderer->mem_seen();
            else
                g_renderer->mem_may_have_changed();
            break;
        default: g_renderer->render_predicate_drop(c.a); break;
    }
}
static void call_queue(u32 op, u32 a, u32 b) {
    RendererCall c = {op, a, b};
    gq_call(call_run, &c, sizeof c);
}
void gpu_method_barrier(int kind) {
    if (g_renderer->method_barrier) call_queue(RC_BARRIER, (u32)kind, 0);
}
void gpu_stream_incr(u32 id) {
    if (g_renderer->stream_incr) call_queue(RC_STREAM_INCR, id, 0);
}
void gpu_stream_end(void) {
    if (g_renderer->stream_end) call_queue(RC_STREAM_END, 0, 0);
}
void gpu_ready(void) {
    if (g_renderer->ready) call_queue(RC_READY, 0, 0);
}
void gpu_stream_run(int on) {
    if (g_renderer->stream_run) call_queue(RC_STREAM_RUN, (u32)on, 0);
}
void gpu_samples_reset(u32 owner) {
    if (g_renderer->samples_reset) call_queue(RC_SAMPLES_RESET, owner, 0);
}
void gpu_samples_enable(u32 owner, int on) {
    if (g_renderer->samples_enable) call_queue(RC_SAMPLES_ENABLE, owner, (u32)on);
}
void gpu_mem_seen(void) {
    if (g_renderer->mem_seen || g_renderer->mem_may_have_changed) call_queue(RC_MEM_SEEN, 0, 0);
}
void gpu_predicate_drop(u32 pred) {
    if (g_renderer->render_predicate_drop) call_queue(RC_PREDICATE_DROP, pred, 0);
}
/* any other call of the renderer: after every queued record, as it may read what they change or give a result that
   depends on them */
const Renderer* gpu_renderer_now(void) {
    if (gq_queues()) gq_drain();
    return g_renderer;
}
