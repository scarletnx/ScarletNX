/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CAPTURE_TILES_SPIRV_H
#define SNX_CAPTURE_TILES_SPIRV_H
#include "common/utilities/spirv_emit.h"

/* std430 uint words: dispatch{count,1,1},tiles_x,tiles_y,width,height,layers,
   adaptiveDispatch{X,Y,Z},fullMode,flags[N],tileIDs[N].
   N=tiles_x*tiles_y*layers. The host bounds N, descriptor bytes and indirect group count before enabling this. */
#define SH_CAPTURE_TILE_HEADER 12u
#define SH_CAPTURE_ADAPTIVE (1u << 17)
#define SH_CAPTURE_ADAPTIVE_DISPATCH 32u
uint32_t shader_capture_tiles_declare(SpvEmitter* e, uint32_t u32_type, uint32_t set, uint32_t binding);
/* position is a loaded raw host FragCoord vec4, layer is a u32 value (0 for ordinary2D), live is a bool value.
   Call before guest instructions. Discarded and late-rejected fragments may add harmless extra tiles. */
void shader_capture_tile_mark(SpvEmitter* e, uint32_t f32, uint32_t u32_type, uint32_t boolean, uint32_t buffer,
                              uint32_t position, uint32_t layer, uint32_t live);
/* One8x8 workgroup owns each appended tile. The first local invocation resets its unique flag, ready for the
   next primitive after the host's compute-write->fragment-atomic barrier. Returns a uvec3 pixel/layer value. */
uint32_t shader_capture_tile_coordinates(SpvEmitter* e, uint32_t u32_type, uint32_t buffer,
                                         uint32_t workgroup_xyz, uint32_t local_xyz, int adaptive);
/* One GPU invocation finalizes the indirect header. Only complete occupancy selects the full grid. */
uint32_t* shader_capture_tiles_finalize_spirv(uint32_t binding, size_t* words);
#endif
