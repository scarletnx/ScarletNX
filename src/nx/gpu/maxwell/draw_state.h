/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's registers as the neutral draw state (gpu/draw_state.h) */
#ifndef SNX_GPU_MAXWELL_DRAW_STATE_H
#define SNX_GPU_MAXWELL_DRAW_STATE_H
#include "nx/gpu/draw_state.h"
#include "nx/gpu/shader/shader.h"

#include "nx/gpu/gpu.h"

/* ch's draw state (GpuChannel.ds) brought up to its registers: before each draw, clear and texture draw */
void mx_draw_state_sync(GpuChannel* ch);
/* the groups (bit DS_ per group) of ch's draw state that differ from its registers read again: none, unless a
   register a group reads is missing from its register group (SNX_DIRTY_CHECK) */
u32 mx_draw_state_stale(const GpuChannel* ch);
/* a draw's dynamic state and viewports from the registers r, in guest units */
void mx_draw_dynamic(const u32* r, DsDynamic* d);
void mx_draw_viewports(const u32* r, DsViewports* v);
/* the groups a pipeline and its programs are keyed by, from the registers r */
void mx_draw_programs(const u32* r, DsPrograms* p);
void mx_draw_vertex_format(const u32* r, DsVertexFormat* f);
void mx_draw_blend(const u32* r, DsBlend* b);
void mx_draw_raster_key(const u32* r, DsRasterKey* k);
void mx_draw_targets(const u32* r, DsTargets* t);
/* the vertex streams and the index buffer */
void mx_draw_vertex_buffers(const u32* r, DsVertexBuffers* b);
void mx_draw_index(const u32* r, DsIndex* x);
/* the bindings' registers into b, leaving its constant buffer slots as they are */
void mx_draw_bindings(const u32* r, DsBindings* b);
/* what transform feedback captures, for programs that DsPrograms.xfb_on says capture */
void mx_draw_xfb(const u32* r, ShXfb* x);
/* a clear (CLEAR_SURFACE's word v) and a texture draw (DRAW_TEXTURE) from the registers r */
void mx_draw_clear(const u32* r, u32 v, DrawClear* c);
void mx_draw_texture(const u32* r, DrawTexture* t);
/* a compute launch from its description q (the QMD) and the compute engine's registers r, its programs at code_loc */
void mx_dispatch_state(const u32* q, const u32* r, u64 code_loc, DispatchState* d);
#endif
