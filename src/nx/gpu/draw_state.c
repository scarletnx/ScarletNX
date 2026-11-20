/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What every front end of the draw state shares (gpu/draw_state.h) */
#include "nx/gpu/draw_state.h"
#include <stdatomic.h>

/* the generations handed out: one sequence for every owner of a DsState */
static struct {
    _Atomic u64 gen;
} g_draw_state;

/* A new generation (DsState.gen), never handed out before. The front ends may bump from different threads (a GPU
   channel's sync on the GPU thread, the NVN replay on its own): a relaxed atomic increment keeps the values unique,
   which is all a generation promises, and it is taken only when a group's bytes changed */
u64 ds_generation_next(void) { return (atomic_fetch_add_explicit(&g_draw_state.gen, 1, memory_order_relaxed) + 1); }
void ds_generations_renew(DsState* ds) {
    for (u32 g = 0; g < DS_GENS; g++) ds->gen[g] = ds_generation_next();
    for (u32 b = 0; b < 2; b++)
        for (u32 s = 0; s < DS_GFX_STAGES; s++) ds->inl_gen[b][s] = ds_generation_next();
    ds->root_gen = ds_generation_next();
}
