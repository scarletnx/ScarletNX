/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The renderer state the GPU front end and the renderer share (renderer.h) */
#include "nx/gpu/renderer.h"
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

/* The renderer's copy of each front end's draw state, which the renderer reads in place of the front end's own. A
   front end gives a group a new generation whenever its bytes change (DsState.gen), so the copy takes only the groups
   whose generation moved: at most 4.7 KB of 40 KB a draw, as a GPU channel never changes its inline blocks. This lets
   the renderer read a state that the front end's next methods don't change, which another thread needs */
#define DS_COPIES 8
static struct {
    const DsState* src; /* the front end's state, NULL for an unused copy */
    u32 used;           /* g_ds_copy_use when last asked for: the copy used longest ago makes room */
    DsState copy;
} g_ds_copy[DS_COPIES];
static u32 g_ds_copy_use;
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
static const DsState* ds_copy(const DsState* ds) {
    u32 at = 0;
    while (at < DS_COPIES && g_ds_copy[at].src != ds) at++;
    if (at == DS_COPIES) { /* a new front end's state: copied whole */
        at = 0;
        for (u32 i = 1; i < DS_COPIES && g_ds_copy[at].src; i++)
            if (!g_ds_copy[i].src || (i32)(g_ds_copy[i].used - g_ds_copy[at].used) < 0) at = i;
        g_ds_copy[at].src = ds;
        g_ds_copy[at].copy = *ds;
    }
    DsState* c = &g_ds_copy[at].copy;
    g_ds_copy[at].used = ++g_ds_copy_use;
    c->owner = ds->owner;
    for (u32 g = 0; g < DS_GENS; g++)
        if (c->gen[g] != ds->gen[g]) {
            memcpy((u8*)c + g_ds_group_at[g], (const u8*)ds + g_ds_group_at[g], g_ds_group_at[g + 1] - g_ds_group_at[g]);
            c->gen[g] = ds->gen[g];
        }
    /* draw_state_check: a group whose bytes changed under the same generation would draw the old bytes */
    if (g_settings.draw_state_check && memcmp(c, ds, sizeof *ds)) {
        for (u32 g = 0; g < DS_GENS; g++)
            if (memcmp((u8*)c + g_ds_group_at[g], (const u8*)ds + g_ds_group_at[g],
                       g_ds_group_at[g + 1] - g_ds_group_at[g]))
                LOG_LIMIT(16, "gpu: owner %x changed the %s of its draw state and kept its generation\n", ds->owner,
                          g_ds_group_name[g]);
        *c = *ds;
    }
    return c;
}
void gpu_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    if (!g_renderer->draw) return;
    /* a draw predicated on the GPU (gpu.h render_pred) counts too: whether it runs isn't known here without the wait
       the predicate is there to avoid, and the counters made up from this (maxwell/engines.c counter_report) only
       stand in for what the console counts, growing with the work given */
    g_draws++;
    g_renderer->draw(ds_copy(ds), dc, o);
}
void gpu_clear(const DsState* ds, const DrawClear* c, const DrawOwner* o) {
    if (g_renderer->clear) g_renderer->clear(ds_copy(ds), c, o);
}
void gpu_draw_texture(const DsState* ds, const DrawTexture* t) {
    if (g_renderer->draw_texture) g_renderer->draw_texture(ds_copy(ds), t);
}
void gpu_discard(const DsState* ds, u32 targets) {
    if (g_renderer->discard) g_renderer->discard(ds_copy(ds), targets);
}
void gpu_dispatch(const DispatchState* d) {
    if (!g_renderer->dispatch) return;
    if (g_settings.compute_dispatches) r3d_pending_flush(); /* a merged draw the front end is holding comes first */
    g_renderer->dispatch(d);
}
