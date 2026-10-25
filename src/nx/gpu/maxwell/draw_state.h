/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's registers as the neutral draw state (gpu/draw_state.h) */
#ifndef SNX_GPU_MAXWELL_DRAW_STATE_H
#define SNX_GPU_MAXWELL_DRAW_STATE_H
#include "nx/gpu/draw_state.h"
#include "nx/gpu/shader/shader.h"

/* A draw's dynamic state from the registers r, in guest units. polygon (DS_POLYGON_) is the mode the draw rasterizes
   with, which picks the depth bias enable; viewports (1 or DS_VIEWPORTS_MAX) is how many viewports to fill */
void mx_draw_dynamic(const u32* r, u32 polygon, u32 viewports, DsDynamic* d, DsViewports* v);
/* the groups a pipeline and its programs are keyed by, from the registers r */
void mx_draw_programs(const u32* r, DsPrograms* p);
void mx_draw_vertex_format(const u32* r, DsVertexFormat* f);
void mx_draw_blend(const u32* r, DsBlend* b);
void mx_draw_raster_key(const u32* r, DsRasterKey* k);
void mx_draw_targets(const u32* r, DsTargets* t);
/* what transform feedback captures, for programs that DsPrograms.xfb_on says capture */
void mx_draw_xfb(const u32* r, ShXfb* x);

#endif
