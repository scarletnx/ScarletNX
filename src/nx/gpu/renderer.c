/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The renderer state the GPU front end and the renderer share (renderer.h) */
#include "nx/gpu/renderer.h"
#include "nx/gpu/gpu_queue.h"
#include <stddef.h>
#include <string.h>
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/bits.h"

int g_renderer_up;
u64 g_draws;
_Atomic u64 g_tex_cache_hits, g_tex_cache_misses, g_tex_upload_bytes, g_tex_frees;
/* set by the renderer while it holds results not yet in guest memory, read by every reader of guest memory: a reader
   that sees 0 has nothing to land. The compute and graphics sides set theirs from different threads: a cache line
   each */
_Alignas(64) _Atomic int g_cs_any;
_Alignas(64) _Atomic int g_gfx_any;
_Alignas(64) _Atomic int g_uma_any;
/* The GPU mirror's programs' writes that guest memory doesn't have yet (gpu/vk/mirror.c), a flag of their own: they
   are not graphics results (g_gfx_any is buffer.c's alone), and with none of the others set a read of guest memory
   under them asked for nothing and read the bytes from before the writes. One game's particle programs keep their
   allocation counter there; a dispatch that could not take the mirror read the counter in guest memory while the
   dispatch before it had written it in the mirror, and handed out the same records twice */
_Alignas(64) _Atomic int g_mir_any;

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
/* The inline group (DsState.inl) is 35 KB, and one NVN game changed a few of its bytes before most draws: each record
   sent the whole group, about 25 KB a draw on average, 1.8 GB a second through the queue, which was full most of the
   time. A front end that knows the bytes it changed names them (gpu_ds_inline_changed), and the record carries those
   ranges in place of the group (DS_PATCHES at most), where the slot holds the state as it was last sent */
#define DS_PATCHES 16
typedef struct {
    u32 off, len; /* bytes of the inline group, 4-aligned */
} DsPatch;
/* The inline group in a record is DsState.inl and, after it, each block's own generation (inl_gen, root_gen), which
   the renderer keeps a block's staged copy by. A record of ranges carries those generations whole after its ranges
   (DS_INLINE_GENS bytes): a block whose bytes a range changed would else keep the generation the renderer staged its
   old bytes under */
#define DS_INLINE_GENS (offsetof(DsState, gen) - offsetof(DsState, inl) - sizeof(DsInline))
static_assert(offsetof(DsState, inl_gen) >= offsetof(DsState, inl) + sizeof(DsInline) &&
                  offsetof(DsState, root_gen) + sizeof(u64) <= offsetof(DsState, gen),
              "the inline blocks' generations follow the inline group");
static struct {
    const DsState* src; /* the front end's state, NULL for an unused slot */
    u32 used;           /* g_ds_sent_use when last asked for: the slot used longest ago makes room */
    u64 gen[DS_GENS];   /* the generations sent to the renderer's side */
    /* the inline group's ranges changed since the last record, and the generation the last of them made: they stand
       for the group only while it is still the state's (no change that named no range came after), and while no range
       was lost (over) */
    DsPatch patch[DS_PATCHES];
    u32 npatch, over;
    u64 patch_gen;
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
/* A group of DS_CHUNKED_MIN bytes or more whose generation moved goes as the DS_CHUNK-byte chunks that differ from
   what the slot last sent, with a mask of them, when they are under half of it. One game's bindings (1,736 bytes)
   went whole in 86% of its draw records, 3.4 MB a present of its 6.3 MB, for a few slots each draw changed. The front
   end's side keeps the groups as it sent them (g_ds_shadow), and the renderer's side puts the chunks in its copy,
   which ends with the bytes the whole group would have given it */
#define DS_CHUNK       64
#define DS_CHUNKED_MIN 256
static DsState g_ds_shadow[DS_COPIES];
static int ds_chunked(u32 g) {
    size_t n = g_ds_group_at[g + 1] - g_ds_group_at[g];
    return g != DS_INLINE && n >= DS_CHUNKED_MIN && n <= 64 * DS_CHUNK; /* a u64 mask holds its chunks */
}
static_assert(DS_CHUNK % 4 == 0, "chunks keep the words after the groups 4-aligned");
enum { DR_DRAW, DR_CLEAR, DR_TEXTURE, DR_DISCARD, DR_XFB };
/* the renderer's side's: the id its last shading_rate call gave (gpu_shading_rate), for the draws after it */
static u32 g_rate_last;
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
    u32 npatch;  /* the inline group's ranges after the groups, each a DsPatch and its bytes, in place of the group */
    u32 chunked; /* the groups after those, each as a u64 mask of its chunks and the chunks (ds_chunked) */
    u64 gen[DS_GENS];
    union {
        DrawCall dc;
        DrawClear c;
        DrawTexture t;
        u32 targets;
        struct {
            u64 control;
            u32 what, stride;
        } xfb;
    } call;
} DsRecord;
/* the record's groups, chunks and ranges into the copy c, which holds the slot's state from before the record: the
   bytes after them (the inline indices) */
static const u8* ds_apply(DsState* c, const DsRecord* rp, const u8* p) {
    const DsRecord r = *rp;
    for (u32 g = 0; g < DS_GENS; g++)
        if (r.groups >> g & 1) {
            size_t n = g_ds_group_at[g + 1] - g_ds_group_at[g];
            memcpy((u8*)c + g_ds_group_at[g], p, n);
            p += n;
        }
    for (u32 g = 0; g < DS_GENS; g++)
        if (r.chunked >> g & 1) {
            size_t n = g_ds_group_at[g + 1] - g_ds_group_at[g];
            u64 mask;
            memcpy(&mask, p, sizeof mask);
            p += sizeof mask;
            for (; mask; mask &= mask - 1) {
                size_t k = (size_t)snx_ctz64(mask) * DS_CHUNK, len = n - k < DS_CHUNK ? n - k : DS_CHUNK;
                if (k >= n) fatal("gpu: a draw record's chunk is out of range\n");
                memcpy((u8*)c + g_ds_group_at[g] + k, p, len);
                p += len;
            }
        }
    for (u32 i = 0; i < r.npatch; i++) {
        DsPatch pt;
        memcpy(&pt, p, sizeof pt);
        p += sizeof pt;
        if (pt.off > sizeof c->inl || pt.len > sizeof c->inl - pt.off) fatal("gpu: a draw record's patch is out of range\n");
        memcpy((u8*)&c->inl + pt.off, p, pt.len);
        p += pt.len;
    }
    if (r.npatch) { /* the blocks' generations, which the group's own bytes would have brought */
        memcpy((u8*)&c->inl + sizeof c->inl, p, DS_INLINE_GENS);
        p += DS_INLINE_GENS;
    }
    c->owner = r.owner;
    memcpy(c->gen, r.gen, sizeof c->gen);
    return p;
}
/* the record a draw of the renderer's side runs (gpu_ds_running): its payload, for the resolve made ahead of it */
/* a call the renderer's side makes at a draw record before it looks at it (the join of a worker's batch) */
static const void* g_ds_join_at;
static void (*g_ds_join_fn)(void);
static u32 g_ds_skip; /* the next draw records whose draws a worker drew (gpu_ds_batch_done) */
static const void* g_ds_running;
u64 g_draw_marks[DM_N];
static u64 g_draw_mark_sums[DM_N], g_draw_mark_n;
u64 gpu_draw_marks_take(u64 sums[DM_N]) {
    memcpy(sums, g_draw_mark_sums, sizeof g_draw_mark_sums);
    const u64 n = g_draw_mark_n;
    memset(g_draw_mark_sums, 0, sizeof g_draw_mark_sums);
    g_draw_mark_n = 0;
    return n;
}
const void* gpu_ds_running(void) { return g_ds_running; }
/* the renderer's side: the record into its copy, then the renderer's call */
static void ds_run(const void* arg, u64 bytes) {
    memset(g_draw_marks, 0, sizeof g_draw_marks);
    g_draw_marks[DM_START] = __builtin_readcyclecounter();
    DsRecord r;
    memcpy(&r, arg, sizeof r);
    DsState* c = &g_ds_copy[r.slot].copy;
    DrawOwner* own = &g_ds_copy[r.slot].own;
    if (r.fresh) *own = (DrawOwner){0};
    const u8* p = ds_apply(c, &r, (const u8*)arg + sizeof r);
    g_draw_marks[DM_APPLIED] = __builtin_readcyclecounter();
    /* a render enable of the 3D engine is decided on this side (maxwell/engines.c): its draws and clears are skipped
       while it turns them off, and they take its predicate */
    u32 pred = r.pred;
    int off = gpu_render_state(r.owner, &pred);
    own->pred = pred;
    own->xfb_reload = (u8)(own->xfb_reload | r.reload);
    if ((u64)(p - (const u8*)arg) + (u64)r.indices * 4 > bytes) fatal("gpu: a draw record is short\n");
    /* an owner's draws off: its draws, clears, textures and discards are skipped, as the front ends skipped them
       when they decided. A discard waits for no predicate: the draws it would keep may run */
    if (arg == g_ds_join_at) { /* a worker's batch starts here: its list goes in first (gpu_ds_join_at) */
        g_ds_join_at = NULL;
        g_ds_join_fn();
    }
    /* a draw a worker drew in its batch (gpu_ds_batch_done), skipped or not: its record is applied above */
    if (r.kind == DR_DRAW && g_ds_skip) {
        g_ds_skip--;
        return;
    }
    if (off == 1 && r.kind != DR_XFB) return;
    if (r.kind == DR_DISCARD && pred) return;
    switch (r.kind) {
        case DR_DRAW: {
            DrawCall dc = r.call.dc;
            if (dc.shading_rate == GPU_RATE_LAST) dc.shading_rate = g_rate_last;
            /* the words after the groups: their offset is a multiple of 4, which a u32 needs */
            if (dc.indexed == 3) dc.inline_indices = r.indices ? (const u32*)(const void*)p : NULL;
            g_ds_running = arg;
            g_draw_marks[DM_CALL] = __builtin_readcyclecounter();
            g_renderer->draw(c, &dc, own);
            g_ds_running = NULL;
            {
                const u64 end = __builtin_readcyclecounter();
                int all = 1;
                for (int k = 0; k < DM_N; k++) all &= g_draw_marks[k] != 0;
                if (all) {
                    g_draw_mark_n++;
                    for (int k = 1; k < DM_N; k++) g_draw_mark_sums[k] += g_draw_marks[k] - g_draw_marks[k - 1];
                    g_draw_mark_sums[0] += end - g_draw_marks[DM_UNLOCKED]; /* the record's end, in slot 0 */
                }
            }
            break;
        }
        case DR_CLEAR: g_renderer->clear(c, &r.call.c, own); break;
        case DR_TEXTURE: {
            DrawTexture t = r.call.t;
            if (off >= 0) t.owner.pred = pred;
            if (t.state.shading_rate == GPU_RATE_LAST) t.state.shading_rate = g_rate_last;
            g_renderer->draw_texture(c, &t);
            break;
        }
        case DR_XFB:
            if (!g_renderer->xfb_control(c->owner, r.call.xfb.control, r.call.xfb.what, r.call.xfb.stride, own))
                LOG_LIMIT(4, "gpu: transform-feedback control operation %u could not run\n", r.call.xfb.what);
            break;
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
    /* the chunked groups' chunks that differ from what was sent, in their place */
    u32 chunked = 0;
    u64 masks[DS_GENS];
    const u8* shadow = (const u8*)&g_ds_shadow[at];
    if (!fresh)
        for (u32 g = 0; g < DS_GENS; g++) {
            if (!(groups >> g & 1) || !ds_chunked(g)) continue;
            size_t n = g_ds_group_at[g + 1] - g_ds_group_at[g], changed = 0;
            const u8 *now = (const u8*)ds + g_ds_group_at[g], *was = shadow + g_ds_group_at[g];
            u64 m = 0;
            for (size_t k = 0, i = 0; k < n; k += DS_CHUNK, i++) {
                size_t len = n - k < DS_CHUNK ? n - k : DS_CHUNK;
                if (memcmp(now + k, was + k, len)) m |= 1ull << i, changed += len;
            }
            if (2 * changed >= n) continue; /* half or more: whole */
            groups &= ~(1u << g);
            chunked |= 1u << g;
            masks[g] = m;
            bytes += sizeof m + changed - n;
        }
    /* the inline group's changed ranges in its place, where they hold all its change since the last record */
    u32 npatch = 0;
    if ((groups >> DS_INLINE & 1) && !fresh && !g_ds_sent[at].over && g_ds_sent[at].npatch &&
        g_ds_sent[at].patch_gen == ds->gen[DS_INLINE]) {
        u64 patched = 0;
        for (u32 i = 0; i < g_ds_sent[at].npatch; i++) patched += sizeof(DsPatch) + g_ds_sent[at].patch[i].len;
        if (patched < sizeof ds->inl / 2) {
            groups &= ~(1u << DS_INLINE);
            bytes += patched - sizeof ds->inl; /* DS_INLINE_GENS of the group's bytes stay: they go after the ranges */
            npatch = g_ds_sent[at].npatch;
        }
    }
    u8* p = (u8*)gq_call_begin(ds_run, bytes);
    DsRecord r = {kind,      at,       groups, fresh,  o ? o->pred : 0, o ? o->xfb_reload : 0,
                  ds->owner, nindices, npatch, chunked};
    memcpy(r.gen, ds->gen, sizeof r.gen);
    memcpy(&r.call, call, call_bytes);
    memcpy(p, &r, sizeof r);
    p += sizeof r;
    for (u32 g = 0; g < DS_GENS; g++)
        if (groups >> g & 1) {
            size_t n = g_ds_group_at[g + 1] - g_ds_group_at[g];
            memcpy(p, (const u8*)ds + g_ds_group_at[g], n);
            if (ds_chunked(g)) memcpy((u8*)&g_ds_shadow[at] + g_ds_group_at[g], p, n);
            p += n;
        }
    for (u32 g = 0; g < DS_GENS; g++)
        if (chunked >> g & 1) {
            size_t n = g_ds_group_at[g + 1] - g_ds_group_at[g];
            const u8* now = (const u8*)ds + g_ds_group_at[g];
            u8* was = (u8*)&g_ds_shadow[at] + g_ds_group_at[g];
            memcpy(p, &masks[g], sizeof masks[g]);
            p += sizeof masks[g];
            for (u64 m = masks[g]; m; m &= m - 1) {
                size_t k = (size_t)snx_ctz64(m) * DS_CHUNK, len = n - k < DS_CHUNK ? n - k : DS_CHUNK;
                memcpy(p, now + k, len);
                memcpy(was + k, now + k, len);
                p += len;
            }
        }
    for (u32 i = 0; i < npatch; i++) {
        const DsPatch* pt = &g_ds_sent[at].patch[i];
        memcpy(p, pt, sizeof *pt);
        p += sizeof *pt;
        memcpy(p, (const u8*)&ds->inl + pt->off, pt->len);
        p += pt->len;
    }
    if (npatch) {
        memcpy(p, (const u8*)&ds->inl + sizeof ds->inl, DS_INLINE_GENS);
        p += DS_INLINE_GENS;
    }
    if (nindices) memcpy(p, indices, (size_t)nindices * 4);
    memcpy(g_ds_sent[at].gen, ds->gen, sizeof ds->gen);
    g_ds_sent[at].npatch = g_ds_sent[at].over = 0;
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
/* The resolving thread's copies of the renderer's side's states (gpu_ds_peek_draw), in the same slots: each a state
   that the next record has not changed yet on the renderer's side. It takes from the renderer's copy each group whose
   generation is not its own, then the next record's groups, as ds_run does. The renderer's side does not change
   its copies while it waits in a draw's run, where this thread reads them */
static DsState g_ds_ahead[DS_COPIES];
static DsState g_ds_batch[DS_COPIES]; /* a worker's copies, for its batches (gpu_ds_batch_draw) */
static DrawOwner g_ds_batch_own[DS_COPIES]; /* and of the owners, which its draws change (gpu_ds_batch_end) */
const DsState* gpu_ds_canonical(const DsState* ds) {
    if (ds >= g_ds_ahead && ds < g_ds_ahead + DS_COPIES) return &g_ds_copy[ds - g_ds_ahead].copy;
    if (ds >= g_ds_batch && ds < g_ds_batch + DS_COPIES) return &g_ds_copy[ds - g_ds_batch].copy;
    return ds;
}
int gpu_ds_peek_draw(const DsState** ds, DrawCall* dc, const void** record) {
    u64 bytes = 0;
    const void* arg = gq_peek_call(ds_run, &bytes);
    if (!arg || bytes < sizeof(DsRecord)) return 0;
    DsRecord r;
    memcpy(&r, arg, sizeof r);
    if (r.kind != DR_DRAW || r.slot >= DS_COPIES) return 0;
    DsState* c = &g_ds_ahead[r.slot];
    const DsState* from = &g_ds_copy[r.slot].copy;
    if (!r.fresh)
        for (u32 g = 0; g < DS_GENS; g++)
            if (c->gen[g] != from->gen[g] || !from->gen[g]) {
                size_t n = g_ds_group_at[g + 1] - g_ds_group_at[g];
                memcpy((u8*)c + g_ds_group_at[g], (const u8*)from + g_ds_group_at[g], n);
                c->gen[g] = from->gen[g];
            }
    const u8* p = ds_apply(c, &r, (const u8*)arg + sizeof r);
    if ((u64)(p - (const u8*)arg) + (u64)r.indices * 4 > bytes) return 0;
    *dc = r.call.dc;
    if (dc->shading_rate == GPU_RATE_LAST) dc->shading_rate = g_rate_last;
    if (dc->indexed == 3) dc->inline_indices = r.indices ? (const u32*)(const void*)p : NULL;
    *ds = c;
    *record = arg;
    return 1;
}
void gpu_ds_peek_log(void) { gq_peek_log(ds_run); }
/* Batches of draws for a worker (vk/render.c, docs/dev/parallel-draw-batches.md). gpu_ds_batch: the draw records
   that follow the one the renderer's side runs, up to max, the first record that is not a draw ending them. A worker
   then takes each in order (gpu_ds_batch_draw): its own copy of the record's state (g_ds_batch), taken from the
   renderer's copy once a batch for each slot and then changed by each record, as ds_run changes the renderer's. And
   the renderer's side passes over the draws the worker drew (gpu_ds_batch_done), with their records still applied to
   its copy */
static u32 g_ds_batch_synced; /* the slots taken from the renderer's copy in this batch */
u32 gpu_ds_batch(const void** recs, u32 max) {
    u64 sizes[64];
    if (max > 64) max = 64;
    const u32 n = gq_peek_calls(ds_run, recs, sizes, max);
    u32 k = 0;
    for (; k < n; k++) {
        DsRecord r;
        if (sizes[k] < sizeof r) break;
        memcpy(&r, recs[k], sizeof r);
        if (r.kind != DR_DRAW || r.slot >= DS_COPIES || r.fresh || r.reload) break;
    }
    g_ds_batch_synced = 0;
    return k;
}
/* the worker's copies of a slot's state and owner, taken from the renderer's side once a batch: only the groups whose
   generation differs, as gpu_ds_peek_draw takes them. The worker's copy got the same records as the renderer's, so a
   group with the same generation holds the same bytes. A whole copy of each slot cost about 13 us a split */
static void ds_batch_sync(u32 slot) {
    if (g_ds_batch_synced >> slot & 1) return;
    DsState* const c = &g_ds_batch[slot];
    const DsState* const from = &g_ds_copy[slot].copy;
    c->owner = from->owner; /* the one field outside the groups */
    for (u32 g = 0; g < DS_GENS; g++)
        if (c->gen[g] != from->gen[g] || !from->gen[g]) {
            memcpy((u8*)c + g_ds_group_at[g], (const u8*)from + g_ds_group_at[g],
                   g_ds_group_at[g + 1] - g_ds_group_at[g]);
            c->gen[g] = from->gen[g];
        }
    g_ds_batch_own[slot] = g_ds_copy[slot].own;
    g_ds_batch_synced |= 1u << slot;
}
/* draw_batches=2: the copies of the slots the records use, taken now on the renderer's side, before it runs the records
   after the running one (the worker runs at the same time) */
void gpu_ds_batch_snapshot(const void* const* recs, u32 n) {
    gq_keep(); /* the worker reads the records that this side runs first: the front end must not put over them */
    g_ds_batch_synced = 0;
    for (u32 i = 0; i < n; i++) {
        DsRecord r;
        memcpy(&r, recs[i], sizeof r);
        ds_batch_sync(r.slot);
    }
}
/* a record before the worker's draws: applied to its copies, not drawn */
void gpu_ds_batch_apply(const void* arg) {
    DsRecord r;
    memcpy(&r, arg, sizeof r);
    ds_batch_sync(r.slot);
    (void)ds_apply(&g_ds_batch[r.slot], &r, (const u8*)arg + sizeof r);
}
/* the owners the worker's draws changed (the capture's counters), back to the renderer's side */
u32 gpu_ds_record_owner(const void* rec) {
    DsRecord r;
    memcpy(&r, rec, sizeof r);
    return r.owner;
}
void gpu_ds_batch_end(void) {
    gq_keep_end();
    for (u32 slot = 0; slot < DS_COPIES; slot++)
        if (g_ds_batch_synced >> slot & 1) g_ds_copy[slot].own = g_ds_batch_own[slot];
}
void gpu_ds_join_at(const void* rec, void (*fn)(void)) {
    g_ds_join_at = rec;
    g_ds_join_fn = fn;
}
int gpu_ds_batch_draw(const void* arg, const DsState** ds, DrawCall* dc, DrawOwner** own) {
    DsRecord r;
    memcpy(&r, arg, sizeof r);
    DsState* c = &g_ds_batch[r.slot];
    ds_batch_sync(r.slot);
    const u8* p = ds_apply(c, &r, (const u8*)arg + sizeof r);
    u32 pred = r.pred;
    const int off = gpu_render_state(r.owner, &pred);
    DrawOwner* o = &g_ds_batch_own[r.slot];
    o->pred = pred;
    o->xfb_reload = (u8)(o->xfb_reload | r.reload);
    if (off == 1) return 0;
    *dc = r.call.dc;
    if (dc->shading_rate == GPU_RATE_LAST) dc->shading_rate = g_rate_last;
    if (dc->indexed == 3) dc->inline_indices = r.indices ? (const u32*)(const void*)p : NULL;
    *ds = c;
    *own = o;
    return 1;
}
void gpu_ds_batch_done(u32 draws) { g_ds_skip = draws; }
int gpu_ds_ahead_same(const DsState* ds) {
    const DsState* c = gpu_ds_canonical(ds);
    return c == ds || !memcmp(c, ds, sizeof *ds);
}
static void calls_named(void);
void gpu_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    if (!g_renderer->draw) return;
    calls_named();
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
void gpu_xfb_control(const DsState* ds, DrawOwner* o, u64 control, u32 what, u32 stride) {
    if (!g_renderer->xfb_control) return;
    struct {
        u64 control;
        u32 what, stride;
    } x = {control, what, stride};
    ds_queue(DR_XFB, ds, o, &x, sizeof x, NULL, 0);
}
void gpu_ds_inline_changed(const DsState* ds, u64 before, u32 off, u32 len) {
    u32 at = 0;
    while (at < DS_COPIES && g_ds_sent[at].src != ds) at++;
    if (at == DS_COPIES) return; /* not sent yet: its first record takes the whole state */
    /* the change before this one must have named its ranges too, or been sent: else these can't stand for the group */
    if (before != g_ds_sent[at].gen[DS_INLINE] && (!g_ds_sent[at].npatch || before != g_ds_sent[at].patch_gen))
        g_ds_sent[at].over = 1;
    g_ds_sent[at].patch_gen = ds->gen[DS_INLINE];
    if (g_ds_sent[at].over || !len) return;
    u32 lo = off & ~3u, hi = (off + len + 3) & ~3u;
    if (hi > sizeof ds->inl) hi = sizeof ds->inl;
    for (u32 i = 0; i < g_ds_sent[at].npatch; i++) { /* one that overlaps or touches it takes it in */
        DsPatch* pt = &g_ds_sent[at].patch[i];
        if (lo <= pt->off + pt->len && pt->off <= hi) {
            u32 end = pt->off + pt->len > hi ? pt->off + pt->len : hi;
            if (lo < pt->off) pt->off = lo;
            pt->len = end - pt->off;
            return;
        }
    }
    if (g_ds_sent[at].npatch == DS_PATCHES) {
        g_ds_sent[at].over = 1;
        return;
    }
    g_ds_sent[at].patch[g_ds_sent[at].npatch++] = (DsPatch){lo, hi - lo};
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
       RC_MEM_SEEN, RC_MEM_CHANGED,
       RC_PREDICATE_DROP };
typedef struct {
    u32 op; /* RC_ */
    u32 a, b;
} RendererCall;
/* the front end's syncpoint increments queued for the renderer's side, and those of them it took (gpu_release_value):
   nvdrv's 128 syncpoints. queued is the front end's alone */
#define RC_SYNCPTS 128
static _Atomic u32 g_incr_queued[RC_SYNCPTS], g_incr_taken[RC_SYNCPTS];
static void call_run(const void* arg, u64 bytes) {
    RendererCall c;
    (void)bytes;
    memcpy(&c, arg, sizeof c);
    switch (c.op) {
        case RC_BARRIER: g_renderer->method_barrier((int)c.a); break;
        case RC_STREAM_INCR:
            /* taken before the renderer counts it: in gpu_release_value a count of the renderer's that has it comes
               with this one, so the increment is never counted twice (a wait passed too early), at most once too few
               for a moment (a wait as before) */
            if (c.a < RC_SYNCPTS) atomic_fetch_add_explicit(&g_incr_taken[c.a], 1, memory_order_seq_cst);
            g_renderer->stream_incr(c.a);
            break;
        case RC_STREAM_END: g_renderer->stream_end(); break;
        case RC_READY: g_renderer->ready(); break;
        case RC_STREAM_RUN: g_renderer->stream_run((int)c.a); break;
        case RC_SAMPLES_RESET: g_renderer->samples_reset(c.a); break;
        case RC_SAMPLES_ENABLE: g_renderer->samples_enable(c.a, (int)c.b); break;
        case RC_MEM_SEEN: g_renderer->mem_seen(); break;
        case RC_MEM_CHANGED: g_renderer->mem_may_have_changed(); break;
        default: g_renderer->render_predicate_drop(c.a); break;
    }
}
/* the names of this file's queued calls, for the stats line of the records' time (gq_name) */
static void calls_named(void) {
    static int done;
    if (done++) return;
    static const char* const draws[] = {"draw", "clear", "texture", "discard", "xfb"};
    static const char* const calls[] = {"barrier", "stream incr", "stream end", "ready", "stream run",
                                        "samples reset", "samples enable", "mem seen", "mem changed",
                                        "predicate drop"};
    gq_name(ds_run, "draw state", draws, 5);
    gq_name(call_run, "renderer call", calls, 10);
}
static void call_queue(u32 op, u32 a, u32 b) {
    RendererCall c = {op, a, b};
    gq_call(call_run, &c, sizeof c);
}
void gpu_method_barrier(int kind) {
    if (g_renderer->method_barrier) call_queue(RC_BARRIER, (u32)kind, 0);
}
void gpu_stream_incr(u32 id) {
    if (!g_renderer->stream_incr) return;
    if (id < RC_SYNCPTS) atomic_fetch_add_explicit(&g_incr_queued[id], 1, memory_order_relaxed); /* before its record */
    call_queue(RC_STREAM_INCR, id, 0);
}
/* The value the renderer will give syncpoint id once its queued work is done (renderer.h release_value), with the
   increments still in the queue: those it took and counted, and those the front end queued that it hasn't taken yet.
   The replay's checks of its own fence drained the queue for the count (gpu_renderer_now) and waited for every record
   before them */
u32 gpu_release_value(u32 id) {
    if (!gq_queues() || id >= RC_SYNCPTS) return gpu_renderer_now()->release_value(id);
    const u32 counted = g_renderer->release_value(id);
    const u32 taken = atomic_load_explicit(&g_incr_taken[id], memory_order_seq_cst);
    return counted + (atomic_load_explicit(&g_incr_queued[id], memory_order_relaxed) - taken);
}
void gpu_stream_end(void) {
    if (g_renderer->stream_end) call_queue(RC_STREAM_END, 0, 0);
}
/* the renderer takes in the CPU's writes so far (renderer.h mem_seen, else mem_may_have_changed) after the work queued
   before it. Later than the front end asked, so its checks see more writes as new, never fewer. A drain at each
   wait (gpu_renderer_now) stopped the front end at every wait of a command buffer: one NVN game ran the two threads
   in turn, at 47 frames a second against 61 on one thread */
void gpu_mem_seen(void) {
    if (g_renderer->mem_seen) call_queue(RC_MEM_SEEN, 0, 0);
    else if (g_renderer->mem_may_have_changed) call_queue(RC_MEM_CHANGED, 0, 0);
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
void gpu_predicate_drop(u32 pred) {
    if (g_renderer->render_predicate_drop) call_queue(RC_PREDICATE_DROP, pred, 0);
}
static void rate_run(const void* arg, u64 bytes) {
    GpuShadingRate r;
    (void)bytes;
    memcpy(&r, arg, sizeof r);
    g_rate_last = g_renderer->shading_rate(&r);
}
/* A call that gave the draws their id at once waited for every record queued before it: one game binds its shading
   rate image again in each frame's passes, and the front end waited 6 ms a present there */
u32 gpu_shading_rate(const GpuShadingRate* r) {
    if (!g_renderer->shading_rate) return 0;
    if (!gq_queues()) return g_rate_last = g_renderer->shading_rate(r); /* on the renderer's side already */
    gq_call(rate_run, r, sizeof *r);
    return GPU_RATE_LAST;
}
/* any other call of the renderer: after every queued record, as it may read what they change or give a result that
   depends on them */
const Renderer* gpu_renderer_now_at(const char* file, int line) {
    if (gq_queues()) gq_drain_at(file, line);
    return g_renderer;
}
