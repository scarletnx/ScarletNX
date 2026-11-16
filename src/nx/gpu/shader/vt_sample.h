/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A fragment program's sample of a texture that may be virtual (shader.h's table, vk/vt.c), for both translators: the
   sparse sample, its coarser level where a texel has no memory, and its mark. */
#ifndef SNX_SHADER_VT_SAMPLE_H
#define SNX_SHADER_VT_SAMPLE_H
#include "common/utilities/spirv_emit.h"
#include "nx/gpu/shader/shader.h"

/* the ids shader_vt_sample makes once a module: zeroed before the first */
typedef struct {
    u32 pair, table_ptr, word_ptr, address;
} ShVtIds;
/* One sample: the module's types, the slot's sampler, the coordinates as the sample takes them (coord) and apart (x,
   y, the layer as a float: 0 without one), mode 0 for the level of the derivatives, 1 for the level `level`, 2 for the
   bias `level`, a constant offset (0: none), the texture's identity, the fragment's pixel (FragCoord's x and y) and the
   frame's phase, both as the renderer gives them */
typedef struct {
    u32 t_bool, t_u32, t_i32, t_f32, t_v2f, t_v4f, glsl;
    u32 sampled_type, sampler, coord, x, y, layer;
    int array;
    u32 mode, level, offset;
    u32 id, pixel_x, pixel_y, phase;
} ShVtSample;
u32 shader_vt_sample(SpvEmitter* e, ShVtIds* ids, const ShVtSample* s);
#endif
