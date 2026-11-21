/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_hook.h: the chains of the hook modules that apply (hle/hook/module.h), followed per queue as its records
   replay. Each chain keeps the step it waits for. Its OBSERVE steps may have other work between them. From its first
   SKIP step the chain holds the queue: its dispatches and barriers are held back (a dispatch as the state the renderer
   would get, nvn_draw_dispatch_state), state binds go on as usual, and anything else breaks the chain, after what was
   held back runs in its order. At the ANCHOR the handler gets what the steps saw */
#include "nx/gpu/nvn/nvn_hook.h"
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/hook/module.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

enum { CHAINS = 16, QUEUES = 16, MEMO = 256 };

static struct {
    int armed;
    u32 nchains;
    const NvnHookChain* chains[CHAINS];
    u64 main_base; /* for a dispatch's caller, an offset from it */
    int trace;     /* the draws and dispatches gpu_hook_trace still logs */
    /* program hashes by code address, with the code's first 8 bytes to tell a new program at the same address */
    struct {
        u64 code, head, hash;
        u32 bytes;
    } memo[MEMO];
} g_hooks;

/* something held back: a dispatch's state, or a barrier record */
typedef struct {
    int barrier;
    u64 payload;
    u32 size;
    DispatchState x;
} Held;

/* a queue's: each chain's next step and what its steps saw, and what the holding chain held back */
typedef struct {
    const NvnDraw* d;
    u32 next[CHAINS];
    int holding; /* the chain past its first SKIP step, -1 none */
    u32 nheld;
    Held held[NVN_HOOK_HELD];
    NvnHookMatch match[]; /* one per chain */
} Run;
static Run* g_runs[QUEUES];

u64 nvn_hook_program_hash(u64 code, u32 bytes) {
    u8 chunk[256];
    u64 head = 0;
    if (!code) return 0;
    if (!bytes) bytes = NVN_HOOK_CODE;
    if (gmem_read(code, &head, sizeof head)) return 0; /* its first page unmapped (gpu.h: -1) */
    u32 slot = (u32)((code >> 4) ^ (code >> 12)) % MEMO;
    if (g_hooks.memo[slot].code == code && g_hooks.memo[slot].bytes == bytes && g_hooks.memo[slot].head == head)
        return g_hooks.memo[slot].hash;
    u64 h = 0xcbf29ce484222325ull;
    for (u32 at = 0; at < bytes; at += sizeof chunk) {
        u32 n = bytes - at < sizeof chunk ? bytes - at : (u32)sizeof chunk;
        gmem_read_code(code + at, chunk, n);
        for (u32 i = 0; i < n; i++) h = (h ^ chunk[i]) * 0x100000001b3ull;
    }
    g_hooks.memo[slot].code = code, g_hooks.memo[slot].bytes = bytes, g_hooks.memo[slot].head = head;
    g_hooks.memo[slot].hash = h;
    return h;
}

/* OBSERVE steps (draws or dispatches), then SKIP dispatches, then one ANCHOR dispatch */
static int well_formed(const NvnHookChain* c) {
    if (!c->nsteps || c->nsteps > NVN_HOOK_STEPS || !c->replace) return 0;
    for (u32 i = 0; i < c->nsteps; i++) {
        const NvnHookStep* s = &c->steps[i];
        int last = i + 1 == c->nsteps;
        if ((s->role == NVN_HOOK_ANCHOR) != last || (s->kind != NVN_HOOK_DISPATCH && s->kind != NVN_HOOK_DRAW) ||
            (s->kind == NVN_HOOK_DRAW && s->role != NVN_HOOK_OBSERVE) ||
            (s->role == NVN_HOOK_OBSERVE && i && c->steps[i - 1].role == NVN_HOOK_SKIP))
            return 0;
    }
    return 1;
}

int nvn_hook_upscale(GpuUpscale* u) {
    if (!g_renderer_up || !g_renderer->upscale) return 0;
    u->flags |= GPU_UPSCALE_MODULE;
    return g_renderer->upscale(u);
}

void nvn_hook_arm(void) {
    for (u32 q = 0; q < QUEUES; q++) free(g_runs[q]), g_runs[q] = NULL;
    memset(&g_hooks, 0, sizeof g_hooks);
    for (u32 i = 0; g_hook_modules[i]; i++) {
        if (!hook_module_applies(i)) continue;
        for (u32 k = 0; k < g_hook_modules[i]->nchains; k++) {
            const NvnHookChain* c = g_hook_modules[i]->chains[k];
            if (!well_formed(c)) {
                WARN("hooks: module %s's chain %s isn't OBSERVE steps, then dispatches to SKIP, then its one ANCHOR, a "
                     "dispatch (at most %d steps): it is left out\n",
                     g_hook_modules[i]->name, c->name, NVN_HOOK_STEPS);
                continue;
            }
            if (g_hooks.nchains == CHAINS) {
                WARN("hooks: more than %d GPU chains: %s is left out\n", CHAINS, c->name);
                continue;
            }
            g_hooks.chains[g_hooks.nchains++] = c;
        }
    }
    if (g_mod)
        for (u32 i = 0; i < g_mod->nmods; i++)
            if (g_mod->mods[i].name && !strcmp(g_mod->mods[i].name, "main")) g_hooks.main_base = g_mod->mods[i].base;
    g_hooks.trace = g_settings.gpu_hook_trace;
    g_hooks.armed = 1;
}

void nvn_hook_start(void) {
    if (!g_hooks.armed) nvn_hook_arm();
}

static Run* run_of(const NvnDraw* d) {
    u32 free_slot = QUEUES;
    for (u32 q = 0; q < QUEUES; q++) {
        if (g_runs[q] && g_runs[q]->d == d) return g_runs[q];
        if (!g_runs[q] && free_slot == QUEUES) free_slot = q;
    }
    if (free_slot == QUEUES) {
        LOG_LIMIT(1, "hooks: more than %d NVN queues: the others' GPU chains aren't looked for\n", QUEUES);
        return NULL;
    }
    Run* r = (Run*)calloc(1, sizeof *r + g_hooks.nchains * sizeof(NvnHookMatch));
    if (!r) return NULL;
    r->d = d;
    r->holding = -1;
    g_runs[free_slot] = r;
    return r;
}

void nvn_hook_forget(const NvnDraw* d) {
    for (u32 q = 0; q < QUEUES; q++)
        if (g_runs[q] && g_runs[q]->d == d) free(g_runs[q]), g_runs[q] = NULL;
}

/* what was held back, run in its order (keep 0), or only its barriers (keep 1: the chain was replaced) */
static void release(Run* r, NvnDraw* d, int replaced) {
    u32 n = r->nheld;
    r->nheld = 0;
    if (r->holding >= 0) r->next[r->holding] = 0;
    r->holding = -1;
    for (u32 i = 0; i < n; i++) {
        Held* h = &r->held[i];
        if (h->barrier)
            nvn_draw_record(d, OP_BARRIER, h->payload, h->size);
        else if (!replaced && g_renderer->dispatch)
            g_renderer->dispatch(&h->x);
    }
}

void nvn_hook_flush(NvnDraw* d) {
    if (!g_hooks.nchains) return;
    for (u32 q = 0; q < QUEUES; q++)
        if (g_runs[q] && g_runs[q]->d == d) {
            Run* r = g_runs[q];
            if (r->holding >= 0)
                LOG_LIMIT(4, "hooks: chain %s stops at a wait or the end of a submission: the game's passes run\n",
                          g_hooks.chains[r->holding]->name);
            release(r, d, 0); /* the other chains' OBSERVE steps carry over */
        }
}

/* a draw's or dispatch's record as a step sees it */
typedef struct {
    u8 kind;
    u64 program, caller;
    u32 block[3], format;
} Seen;

static int matches(const NvnHookStep* s, const Seen* v) {
    return s->kind == v->kind && (!s->program || s->program == v->program) && (!s->caller || s->caller == v->caller) &&
           (!s->block[0] || !memcmp(s->block, v->block, sizeof v->block)) && (!s->format || s->format == v->format);
}

/* a target as the 2D engine addresses it (nvn_copy.c's surf2d does the same from a texture) */
static Surf2D surf(const DsSurf* t, int depth) {
    Surf2D o = {0};
    if (!t->gva) return o;
    o.addr = t->gva;
    o.format = t->format;
    o.linear = t->mem >> 12 & 1;
    o.bh = t->mem >> 4 & 15;
    o.depth = 1;
    o.height = t->height;
    o.width = t->width;
    if (o.linear && !depth) { /* a pitch-linear target's width is its row pitch */
        u32 bpp = gpu_color_bpp(t->format);
        o.pitch = t->width;
        o.width = bpp ? t->width / bpp : 0;
    }
    return o;
}

/* the step a record matched: kept for the handler */
static void keep(Run* r, u32 c, u32 step, NvnDraw* d, const Seen* v, const DispatchState* x) {
    NvnHookMatch* m = &r->match[c];
    m->chain = g_hooks.chains[c];
    m->d = d;
    if (v->kind == NVN_HOOK_DISPATCH) {
        m->dispatch[step] = *x;
        m->caller[step] = v->caller;
        m->color[step] = m->depth[step] = (Surf2D){0};
    } else {
        memset(&m->dispatch[step], 0, sizeof m->dispatch[step]);
        m->caller[step] = 0;
        m->color[step] = d->ds.tgt.count ? surf(&d->ds.tgt.color[0], 0) : (Surf2D){0};
        m->depth[step] = surf(&d->ds.tgt.depth, 1);
    }
}

/* the record of a draw or dispatch v (x its state for a dispatch): 1 when held back or replaced */
static int step(Run* r, NvnDraw* d, const Seen* v, const DispatchState* x) {
    /* the chain holding the queue: its next step, or it breaks */
    if (r->holding >= 0) {
        u32 c = (u32)r->holding;
        const NvnHookChain* ch = g_hooks.chains[c];
        const NvnHookStep* s = &ch->steps[r->next[c]];
        if (!matches(s, v)) {
            LOG_LIMIT(4, "hooks: chain %s breaks at step %u (another %s): the game's passes run\n", ch->name,
                      r->next[c], v->kind == NVN_HOOK_DRAW ? "draw" : "dispatch");
            release(r, d, 0);
        } else if (s->role == NVN_HOOK_SKIP) {
            if (r->nheld == NVN_HOOK_HELD) {
                LOG_LIMIT(4, "hooks: chain %s holds more than %d records: the game's passes run\n", ch->name,
                          NVN_HOOK_HELD);
                release(r, d, 0);
                return 0;
            }
            keep(r, c, r->next[c]++, d, v, x);
            r->held[r->nheld].barrier = 0;
            r->held[r->nheld++].x = *x;
            return 1;
        } else { /* the anchor */
            keep(r, c, r->next[c], d, v, x);
            int replaced = ch->replace(&r->match[c]);
            if (!replaced)
                LOG_LIMIT(4, "hooks: chain %s's handler leaves this frame to the game's passes\n", ch->name);
            else
                LOG_LIMIT(1, "hooks: chain %s replaced (first time)\n", ch->name);
            release(r, d, replaced);
            return replaced;
        }
    }
    /* the chains on their OBSERVE steps, or at their first SKIP or an anchor with nothing skipped */
    for (u32 c = 0; c < g_hooks.nchains; c++) {
        const NvnHookChain* ch = g_hooks.chains[c];
        const NvnHookStep* s = &ch->steps[r->next[c]];
        if (!matches(s, v)) {
            if (r->next[c] && matches(&ch->steps[0], v))
                r->next[c] = 0; /* again from the start */
            else
                continue;
            s = &ch->steps[0];
        }
        if (v->kind == NVN_HOOK_DRAW || s->role == NVN_HOOK_OBSERVE) {
            if (s->role == NVN_HOOK_OBSERVE) keep(r, c, r->next[c]++, d, v, x);
            continue;
        }
        if (s->role == NVN_HOOK_SKIP) {
            r->holding = (int)c;
            keep(r, c, r->next[c]++, d, v, x);
            r->held[0].barrier = 0;
            r->held[0].x = *x;
            r->nheld = 1;
            return 1;
        }
        /* an anchor after observing alone */
        keep(r, c, r->next[c], d, v, x);
        int replaced = ch->replace(&r->match[c]);
        r->next[c] = 0;
        if (replaced) return 1;
        LOG_LIMIT(4, "hooks: chain %s's handler leaves this frame to the game's passes\n", ch->name);
    }
    return 0;
}

/* the records that may come between two steps of a holding chain: state that no pass reads before the next draw or
   dispatch takes it */
static int binds(u32 op) {
    switch (op) {
        case OP_BIND_PROGRAM:
        case OP_BIND_TEX:
        case OP_BIND_SAMP:
        case OP_BIND_IMG:
        case OP_BIND_TEX1:
        case OP_BIND_UBO:
        case OP_BIND_SSBO:
        case OP_BIND_BUFS:
        case OP_PUSH_CONSTANTS:
        case OP_SET_TEX_POOL:
        case OP_SET_SAMP_POOL: return 1;
    }
    return 0;
}

/* gpu_hook_trace: a draw or dispatch as a chain's step would name it, for writing a module from a run */
static void trace(const NvnDraw* d, const Seen* v, const DispatchState* x) {
    g_hooks.trace--;
    if (v->kind == NVN_HOOK_DISPATCH)
        LOG("hooks: trace queue %x dispatch program %016llx (%u bytes at %llx) caller main+%llx grid %ux%ux%u block "
            "%ux%ux%u%s\n",
            (unsigned)d->ds.owner, (unsigned long long)v->program, x->code_bytes ? x->code_bytes : NVN_HOOK_CODE,
            (unsigned long long)x->code, (unsigned long long)v->caller, x->grid[0], x->grid[1], x->grid[2], x->block[0],
            x->block[1], x->block[2], x->indirect ? " (indirect)" : "");
    else {
        const DsTargets* t = &d->ds.tgt;
        LOG("hooks: trace queue %x draw fragment program %016llx, target 0 format %#x %ux%u at %llx, depth format %#x "
            "%ux%u at %llx\n",
            (unsigned)d->ds.owner, (unsigned long long)v->program, v->format, t->count ? t->color[0].width : 0,
            t->count ? t->color[0].height : 0, t->count ? (unsigned long long)t->color[0].gva : 0ull, t->depth.format,
            t->depth.width, t->depth.height, (unsigned long long)t->depth.gva);
    }
}

int nvn_hook_record(NvnDraw* d, u32 op, u64 payload, u32 size) {
    if (!g_hooks.nchains && !g_hooks.trace) return 0;
    Run* r = g_hooks.nchains ? run_of(d) : NULL;
    Seen v = {0};
    DispatchState x;
    switch (op) {
        case OP_DISPATCH: {
            const NvnPDispatch* p = NVN_OBJ(const NvnPDispatch, payload);
            if (!nvn_draw_dispatch_state(d, payload, &x)) break;
            v.kind = NVN_HOOK_DISPATCH;
            v.program = nvn_hook_program_hash(x.code, x.code_bytes);
            v.caller = p->caller && p->caller >= g_hooks.main_base ? p->caller - g_hooks.main_base : 0;
            memcpy(v.block, x.block, sizeof v.block);
            if (g_hooks.trace > 0) trace(d, &v, &x);
            return r ? step(r, d, &v, &x) : 0;
        }
        case OP_DRAW:
        case OP_DRAW_IDX:
        case OP_DRAW_MESH:
            v.kind = NVN_HOOK_DRAW;
            v.program = nvn_hook_program_hash(d->code[DS_PROGRAM_SLOTS - 1], 0);
            v.format = d->ds.tgt.count ? d->ds.tgt.color[0].format : 0;
            if (g_hooks.trace > 0) trace(d, &v, NULL);
            return r ? step(r, d, &v, NULL) : 0;
        case OP_BARRIER:
            if (!r || r->holding < 0) return 0;
            if (r->nheld == NVN_HOOK_HELD) {
                release(r, d, 0);
                return 0;
            }
            r->held[r->nheld].barrier = 1;
            r->held[r->nheld].payload = payload;
            r->held[r->nheld++].size = size;
            return 1;
    }
    if (r && r->holding >= 0 && !binds(op)) {
        LOG_LIMIT(4, "hooks: chain %s breaks at a record of op %u: the game's passes run\n",
                  g_hooks.chains[r->holding]->name, op);
        release(r, d, 0);
    }
    return 0;
}
