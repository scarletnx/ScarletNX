/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's registers as the neutral draw state (gpu/draw_state.h) */
#ifndef SNX_GPU_MAXWELL_DRAW_STATE_H
#define SNX_GPU_MAXWELL_DRAW_STATE_H
#include "nx/gpu/draw_state.h"

/* A draw's dynamic state from the registers r, in guest units. polygon (DS_POLYGON_) is the mode the draw rasterizes
   with, which picks the depth bias enable; viewports (1 or DS_VIEWPORTS_MAX) is how many viewports to fill */
void mx_draw_dynamic(const u32* r, u32 polygon, u32 viewports, DsDynamic* d, DsViewports* v);

#endif
