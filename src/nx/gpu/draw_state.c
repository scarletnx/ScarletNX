/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What every front end of the draw state shares (gpu/draw_state.h) */
#include "nx/gpu/draw_state.h"

/* the generations handed out: one sequence for every owner of a DsState */
static struct {
    u64 gen;
} g_draw_state;

/* A new generation (DsState.gen), never handed out before. The front ends may bump from different threads (a GPU
   channel's sync on the GPU thread, the NVN replay on its own): a relaxed atomic increment keeps the values unique,
   which is all a generation promises, and it is taken only when a group's bytes changed */
u64 ds_generation_next(void) { return __atomic_add_fetch(&g_draw_state.gen, 1, __ATOMIC_RELAXED); }
