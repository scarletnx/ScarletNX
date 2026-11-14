/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* g_nvn in a save state (gpu/nvn/nvn_state.h), as runtime/state.c writes g_hle: the struct as it is, less the host's
   fields (the locks, the pointers to heap memory, the GPU thread's scratch), then the heap memory its members own as
   records of a kind, a slot and a size. The rest of it is guest addresses, indices into its own arrays and counters,
   which hold in any process: the NVN objects and the command records are in the game's memory, which the state holds
   beside it, and the syncpoints its queues count on are g_hle's.

   What a state can't hold yet is refused at the save, never written to fail at the load: work a queue has waiting on
   the GPU thread (a submission stopped at a sync or event wait, a present behind another queue's), which only the GPU
   thread's own queue holds.

   A load checks the state's copy before the running game is touched (a state may come from another player, as
   state.c's hle_check says of g_hle's): every index into the runtime's own tables in range, every chain of them ending.
   Once the running game is gone it lets go of that game's host side (its queues' render predicates, its waiting
   submissions' records and its heap memory) and puts the state's in place. The renderer has forgotten all it kept by
   then (renderer.h reset), so the state's queues start with no render predicate, sample counter or capture counters
   of the renderer's, and with new generations for their draw state, which the saving process numbered. The pacer's
   clock (nvn_now) goes on from where the state left it, as the times in the game's syncs, queues and windows count
   on it */
#include "nx/gpu/nvn/nvn_state.h"
#include "nx/gpu/draw_state.h"
#include "nx/gpu/renderer.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/state_io.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NVN_STATE_VERSION 3 /* 3: the programs' early test flags */
/* the heap memory g_nvn's members own, after it: g_nvn.object's program metadata and linkage maps, and the blocks the
   small store of g_nvn.cmd was given back */
enum { NH_END, NH_XFB, NH_TESS, NH_SUBPROGRAM, NH_SUBLINKAGE, NH_SMALL, NH_EARLY_Z };
/* the most entries a state's heap array may hold: far above what a game makes (one program's metadata each) */
#define NH_MAX (1u << 24)

struct NvnStaged {
    u64 now; /* the pacer's clock at the save */
    Nvn n;
};

/* The host's fields of a copy of g_nvn cleared: a save writes them so, and a load clears them in the state's copy
   whatever the file held, before it fills any. The loader's recompiled code is the running process's, as is where
   its pacer's clock stands against its plat_ns, and the GPU thread's tables of a submission's effects and of a blit
   are scratch between its uses */
static void nil_host(Nvn* c) {
    u32 i;
    memset(&c->loader.lock, 0, sizeof c->loader.lock);
    c->loader.bootstrap_original = c->loader.getproc_original = NULL;
    c->loader.bootstrap_original32 = c->loader.getproc_original32 = NULL;
    c->loader.nvrm_get = c->loader.nvrm_init = NULL;
    c->queue.clock = 0;
    memset(&c->object.xfb_lock, 0, sizeof c->object.xfb_lock);
    c->object.xfb = NULL;
    c->object.tess = NULL;
    c->object.subprogram = NULL;
    c->object.sublinkage = NULL;
    c->object.early_z = NULL;
    c->object.xfb_capacity = c->object.tess_capacity = c->object.subprogram_capacity = 0;
    c->object.early_z_capacity = 0;
    c->object.sublinkage_capacity = 0;
    memset(&c->cmd.lock, 0, sizeof c->cmd.lock);
    for (i = 0; i < NVN_SMALL_CLASSES; i++) { /* written from their first, which the load's arrays start at */
        c->cmd.small[i].given = NULL;
        c->cmd.small[i].first = c->cmd.small[i].cap = 0;
    }
    memset(&c->replay.lock, 0, sizeof c->replay.lock);
    for (i = 0; i < NVN_JOBS; i++) c->replay.jobs[i].snapshot = NULL;
    memset(c->replay.fx, 0, sizeof c->replay.fx);
    c->replay.nfx = 0;
    memset(&c->copy.read_lock, 0, sizeof c->copy.read_lock);
    memset(c->copy.blit, 0, sizeof c->copy.blit);
}

static int heap_put(StateIo* io, u32 kind, u32 slot, const void* p, u64 n) {
    return io_put32(io, kind) | io_put32(io, slot) | io_put64(io, n) | io_put(io, p, n);
}
int nvn_state_save(StateIo* io, char* why, size_t whysize) {
    const NvnObjectState* o = &g_nvn.object;
    Nvn* c;
    u32 i, settling = 0;
    int r;
    for (i = 0; i < NVN_JOBS; i++) {
        u32 queue = g_nvn.replay.jobs[i].queue;
        if (queue > NVN_QUEUES) { /* nvn_replay_finish_all's: named only when no queue's work waits */
            settling = 1;
            continue;
        }
        if (queue) {
            snprintf(why, whysize,
                     "the game's NVN queue %u has work waiting on the GPU for a sync, an event or another queue's "
                     "present, which states don't hold yet",
                     queue);
            return -1;
        }
    }
    if (settling) {
        snprintf(why, whysize,
                 "the game waits on the GPU for all its NVN queues' submitted work, which states don't hold yet");
        return -1;
    }
    if (!(c = (Nvn*)malloc(sizeof *c))) {
        snprintf(why, whysize, "there is no memory for a copy of the game's NVN state (%zu bytes)", sizeof *c);
        return -1;
    }
    /* in the order the replay takes them (its submission walks the command memory under the first) */
    pm_lock(&g_nvn.replay.lock);
    pm_lock(&g_nvn.cmd.lock);
    pm_lock(&g_nvn.object.xfb_lock);
    memcpy(c, &g_nvn, sizeof *c);
    nil_host(c);
    r = io_put32(io, NVN_STATE_VERSION) | io_put64(io, nvn_now()) | io_put(io, c, sizeof *c);
    if (o->nxfb) r |= heap_put(io, NH_XFB, 0, o->xfb, (u64)o->nxfb * sizeof *o->xfb);
    if (o->ntess) r |= heap_put(io, NH_TESS, 0, o->tess, (u64)o->ntess * sizeof *o->tess);
    if (o->nsubprogram) r |= heap_put(io, NH_SUBPROGRAM, 0, o->subprogram, (u64)o->nsubprogram * sizeof *o->subprogram);
    if (o->nearly_z) r |= heap_put(io, NH_EARLY_Z, 0, o->early_z, (u64)o->nearly_z * sizeof *o->early_z);
    for (i = 0; i < o->nsublinkage; i++)
        r |= heap_put(io, NH_SUBLINKAGE, i, o->sublinkage[i],
                      (u64)offsetof(NvnSubLinkage, data) + o->sublinkage[i]->bytes);
    for (i = 0; i < NVN_SMALL_CLASSES; i++) {
        const NvnSmallClass* k = &g_nvn.cmd.small[i];
        if (k->n) r |= heap_put(io, NH_SMALL, i, k->given + k->first, (u64)k->n * sizeof *k->given);
    }
    r |= io_put32(io, NH_END);
    pm_unlock(&g_nvn.object.xfb_lock);
    pm_unlock(&g_nvn.cmd.lock);
    pm_unlock(&g_nvn.replay.lock);
    free(c);
    if (r) snprintf(why, whysize, "the game's NVN state couldn't be written into the state");
    return r ? -1 : 0;
}

/* the heap memory a copy of g_nvn owns, freed (the running game's, or a refused load's staged copy) */
static void heap_free(Nvn* n) {
    u32 i;
    free(n->object.xfb);
    free(n->object.tess);
    free(n->object.subprogram);
    free(n->object.early_z);
    for (i = 0; n->object.sublinkage && i < n->object.nsublinkage; i++) free(n->object.sublinkage[i]);
    free(n->object.sublinkage);
    for (i = 0; i < NVN_SMALL_CLASSES; i++) free(n->cmd.small[i].given);
}
void nvn_state_staged_free(NvnStaged* s) {
    if (!s) return;
    heap_free(&s->n);
    free(s);
}

/* the next n bytes of the state in a new block: NULL when it holds fewer, or there is no memory */
static void* heap_get(StateIo* io, u64 n) {
    void* p = malloc(n ? (size_t)n : 1);
    if (p && io_get(io, p, n)) {
        free(p);
        return NULL;
    }
    return p;
}
/* the chain from head through each piece's next (or cnext with control), each link a slot of the pieces: at most
   *links links, which it uses up. 0 when one is past the slots or the links run out (a loop) */
static int chain_ok(const NvnCmdState* c, u32 head, int control, u32* links) {
    for (u32 i = head; i; i = control ? c->p[i].cnext : c->p[i].next) {
        if (i > c->slots || !*links) return 0;
        --*links;
    }
    return 1;
}
/* g_nvn.cmd's indices: the segments and the pieces' slots within their arrays, each piece in use in a segment or a
   class of the small store, and the chains through the buckets and the free slots ending. The slots and segments past
   those in use are made all zero, as the runtime finds unused ones */
static int cmd_ok(NvnCmdState* c) {
    u32 i, live = 0, links;
    if (c->ns > NVN_SPILL_SEGS || c->slots > NVN_SPILL_PIECES || c->np > c->slots) return 0;
    for (i = 1; i <= c->slots; i++) {
        const NvnSpillPiece* q = &c->p[i];
        if (!q->anchor) continue;
        live++;
        if (q->seg >= NVN_SPILL_SMALL ? q->seg - NVN_SPILL_SMALL >= NVN_SMALL_CLASSES : q->seg >= c->ns) return 0;
    }
    if (live != c->np) return 0;
    links = c->slots; /* a slot is in one bucket's chain or the free list, through next */
    for (i = 0; i < NVN_SPILL_BUCKETS; i++)
        if (!chain_ok(c, c->bucket[i], 0, &links)) return 0;
    if (!chain_ok(c, c->free, 0, &links)) return 0;
    links = c->slots;
    for (i = 0; i < NVN_SPILL_BUCKETS; i++)
        if (!chain_ok(c, c->cbucket[i], 1, &links)) return 0;
    memset(&c->p[c->slots + 1], 0, (NVN_SPILL_PIECES - c->slots) * sizeof *c->p);
    memset(&c->s[c->ns], 0, (NVN_SPILL_SEGS - c->ns) * sizeof *c->s);
    return 1;
}
/* g_nvn.replay: no work queued (a save refuses a state with any), each live queue's draws its own owner as renderer.h
   numbers them, and the retired ones all zero, as the runtime finds a free queue */
static int replay_ok(NvnReplayState* r) {
    u32 i;
    for (i = 0; i < NVN_JOBS; i++)
        if (r->jobs[i].queue) return 0;
    memset(r->jobs, 0, sizeof r->jobs);
    for (i = 0; i < NVN_QUEUES; i++) {
        NvnReplayQueue* q = &r->q[i];
        if (!q->syncpt)
            memset(q, 0, sizeof *q);
        else if (q->d.ds.owner != (0x80000000u | i))
            return 0;
    }
    return 1;
}
NvnStaged* nvn_state_parse(StateIo* io) {
    NvnStaged* s = (NvnStaged*)malloc(sizeof *s);
    Nvn* n;
    NvnObjectState* o;
    u32 version, kind, slot, i;
    u64 size;
    if (!s) return NULL;
    n = &s->n;
    o = &n->object;
    if (io_get32(io, &version) || version != NVN_STATE_VERSION || io_get64(io, &s->now) || io_get(io, n, sizeof *n)) {
        free(s); /* none of its pointers are this process's */
        return NULL;
    }
    nil_host(n);
    if (o->nxfb > NH_MAX || o->ntess > NH_MAX || o->nsubprogram > NH_MAX || o->nsublinkage > NH_MAX ||
        o->nearly_z > NH_MAX)
        goto bad;
    for (i = 0; i < NVN_SMALL_CLASSES; i++)
        if (n->cmd.small[i].n > NH_MAX) goto bad;
    if (o->nsublinkage && !(o->sublinkage = (NvnSubLinkage**)calloc(o->nsublinkage, sizeof *o->sublinkage))) goto bad;
    o->sublinkage_capacity = o->nsublinkage;
    for (;;) {
        if (io_get32(io, &kind)) goto bad;
        if (kind == NH_END) break;
        if (io_get32(io, &slot) || io_get64(io, &size)) goto bad;
        switch (kind) {
            case NH_XFB:
                if (o->xfb || size != (u64)o->nxfb * sizeof *o->xfb || !(o->xfb = (NvnXfbEntry*)heap_get(io, size)))
                    goto bad;
                o->xfb_capacity = o->nxfb;
                break;
            case NH_TESS:
                if (o->tess || size != (u64)o->ntess * sizeof *o->tess ||
                    !(o->tess = (NvnTessEntry*)heap_get(io, size)))
                    goto bad;
                o->tess_capacity = o->ntess;
                break;
            case NH_SUBPROGRAM:
                if (o->subprogram || size != (u64)o->nsubprogram * sizeof *o->subprogram ||
                    !(o->subprogram = (NvnSubProgram*)heap_get(io, size)))
                    goto bad;
                o->subprogram_capacity = o->nsubprogram;
                break;
            case NH_EARLY_Z:
                if (o->early_z || size != (u64)o->nearly_z * sizeof *o->early_z ||
                    !(o->early_z = (u64*)heap_get(io, size)))
                    goto bad;
                o->early_z_capacity = o->nearly_z;
                break;
            case NH_SUBLINKAGE: {
                NvnSubLinkage* l;
                if (slot >= o->nsublinkage || o->sublinkage[slot] || size < offsetof(NvnSubLinkage, data) + 80 ||
                    size > offsetof(NvnSubLinkage, data) + (16u << 20) ||
                    !(l = o->sublinkage[slot] = (NvnSubLinkage*)malloc(sizeof *l + (size_t)size)))
                    goto bad;
                if (io_get(io, l, size) || offsetof(NvnSubLinkage, data) + (u64)l->bytes != size ||
                    !nvn_sub_linkage_valid(l))
                    goto bad;
                break;
            }
            case NH_SMALL: {
                NvnSmallClass* k = slot < NVN_SMALL_CLASSES ? &n->cmd.small[slot] : NULL;
                if (!k || k->given || size != (u64)k->n * sizeof *k->given ||
                    !(k->given = (NvnSmallFree*)heap_get(io, size)))
                    goto bad;
                k->cap = k->n;
                break;
            }
            default: goto bad;
        }
    }
    /* each array its count says it has */
    if ((o->nxfb && !o->xfb) || (o->ntess && !o->tess) || (o->nsubprogram && !o->subprogram) ||
        (o->nearly_z && !o->early_z))
        goto bad;
    for (i = 0; i < o->nsublinkage; i++)
        if (!o->sublinkage[i]) goto bad;
    for (i = 0; i < NVN_SMALL_CLASSES; i++)
        if (n->cmd.small[i].n && !n->cmd.small[i].given) goto bad;
    if (!cmd_ok(&n->cmd) || !replay_ok(&n->replay) || !nvn_loader_state_ok(&n->loader)) goto bad;
    return s;
bad:
    nvn_state_staged_free(s);
    return NULL;
}

void nvn_state_install(NvnStaged* s) {
    Nvn* n = &s->n;
    u32 i, g;
    /* the running game's host side: its queues' predicates (the renderer counts their holders), the records its
       waiting submissions kept, its heap memory */
    for (i = 0; i < NVN_QUEUES; i++) {
        const NvnDraw* d = &g_nvn.replay.q[i].d;
        if (g_nvn.replay.q[i].syncpt && d->own.pred && g_renderer->render_predicate_drop)
            gpu_predicate_drop(d->own.pred);
    }
    for (i = 0; i < NVN_JOBS; i++) free(g_nvn.replay.jobs[i].snapshot);
    heap_free(&g_nvn);
    /* what the state's queues had of the renderer was the saving process's renderer's */
    for (i = 0; i < NVN_QUEUES; i++) {
        NvnDraw* d = &n->replay.q[i].d;
        if (!n->replay.q[i].syncpt) continue;
        d->own.pred = 0;        /* draws run until the next render enable */
        d->own.xfb_started = 0; /* capture starts over at the buffers' offsets */
        d->samples = 0;         /* reports write the replay's own count until the game resets the counter again */
        for (g = 0; g < DS_GENS; g++) {
            d->ds.gen[g] = ds_generation_next();
            d->scratch.gen[g] = ds_generation_next();
        }
    }
    /* the running process's: its locks, and its loader, once it has taken the state's game's functions (a process
       that loads the state as the game starts hasn't) */
    nvn_loader_state_install(&n->loader);
    n->loader = g_nvn.loader;
    n->object.xfb_lock = g_nvn.object.xfb_lock;
    n->cmd.lock = g_nvn.cmd.lock;
    n->replay.lock = g_nvn.replay.lock;
    n->copy.read_lock = g_nvn.copy.read_lock;
    n->queue.clock = (i64)(s->now - plat_ns()); /* nvn_now() is the state's now from here */
    memcpy(&g_nvn, n, sizeof g_nvn);
    free(s);
}

/* the types nvn_state_save writes as raw bytes: g_nvn, whose walk takes in its structs but not its arrays, and the
   types of those arrays' elements, and of the heap records */
void nvn_state_schema(int (*dump)(u64* h, const char* fmt, ...), u64* h) {
    static const NvnResolved resolved;
    static const NvnSpillSeg seg;
    static const NvnSpillPiece piece;
    static const NvnCtl ctl;
    static const NvnSmallClass small;
    static const NvnSmallFree given;
    static const NvnXfbEntry xfb;
    static const NvnTessEntry tess;
    static const NvnSubProgram subprogram;
    static const NvnSubLinkage sublinkage;
    static const NvnReplayQueue queue;
    static const NvnJob job;
    static const NvnEffect effect;
    static const DsSurf surf;
    static const NvnVertexStream stream;
    static const NVNbufferRange range;
    __builtin_dump_struct(&g_nvn, dump, h);
    __builtin_dump_struct(&resolved, dump, h);
    __builtin_dump_struct(&seg, dump, h);
    __builtin_dump_struct(&piece, dump, h);
    __builtin_dump_struct(&ctl, dump, h);
    __builtin_dump_struct(&small, dump, h);
    __builtin_dump_struct(&given, dump, h);
    __builtin_dump_struct(&xfb, dump, h);
    __builtin_dump_struct(&tess, dump, h);
    __builtin_dump_struct(&subprogram, dump, h);
    __builtin_dump_struct(&sublinkage, dump, h);
    __builtin_dump_struct(&queue, dump, h);
    __builtin_dump_struct(&job, dump, h);
    __builtin_dump_struct(&effect, dump, h);
    __builtin_dump_struct(&surf, dump, h);
    __builtin_dump_struct(&stream, dump, h);
    __builtin_dump_struct(&range, dump, h);
}
