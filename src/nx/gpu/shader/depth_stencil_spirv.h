/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_DEPTH_STENCIL_SPIRV_H
#define SNX_DEPTH_STENCIL_SPIRV_H
#include "common/utilities/spirv_emit.h"

/* A GPU-only D32F/S8 shadow. The host copies each native image aspect separately, without format conversion.
   The first16 uint words are immutable during a draw. Depth words follow; optional stencil bytes are packed
   exactly as an S8 image-to-buffer copy. Every stencil access is atomic because adjacent pixels share a word.
   The caller keeps the original native depth attachment format for raster depth-bias units, disables native
   depth/stencil operations, and imports/exports the shadow outside rendering with the required GPU barriers.

   Initial integration requires one sample, one layer and one viewport, no early fragment tests or queries, and
   no feedback access to the depth/stencil image. Per-sample interpolated depth and fixed-point depth conversion
   are deliberately outside this interface's contract. Guest discard, alpha and sample masks precede its use. */
enum {
    SH_DS_WIDTH,
    SH_DS_HEIGHT,
    SH_DS_DEPTH_OFFSET,
    SH_DS_STENCIL_OFFSET,
    SH_DS_FLAGS,
    SH_DS_BOUNDS_MIN,
    SH_DS_BOUNDS_MAX,
    SH_DS_CLAMP_MIN,
    SH_DS_CLAMP_MAX,
    SH_DS_FRONT_CONTROL,
    SH_DS_FRONT_VALUES,
    SH_DS_BACK_CONTROL,
    SH_DS_BACK_VALUES,
    SH_DS_HEADER_WORDS = 16,
    SH_DS_DEPTH_TEST = 1u << 0,
    SH_DS_DEPTH_WRITE = 1u << 1,
    SH_DS_STENCIL_TEST = 1u << 2,
    SH_DS_DEPTH_BOUNDS = 1u << 3,
    SH_DS_DEPTH_CLAMP = 1u << 4,
    SH_DS_FIXED_MASK_OFF = 1u << 5,
    SH_DS_COMPARE_SHIFT = 8
};
/* Control=compare|fail<<3|pass<<6|depthFail<<9. Values=reference8|compareMask8<<8|writeMask8<<16.
   Compare and stencil-op numbers are the Vulkan core enumerants, each in0..7. Bounds use stored depth. */
typedef struct { uint32_t word[SH_DS_HEADER_WORDS]; } ShaderDepthStencilHeader;
typedef struct { uint32_t pass, depth, stencil; } ShaderDepthStencilResult;

/* Returns the checked descriptor byte count, or0 for an empty/overflowing shape. Offsets are uint words.
   The final stencil word is allocated in full even when the image has fewer than four trailing stencil bytes. */
uint64_t shader_depth_stencil_layout(uint32_t width, uint32_t height, int stencil,
                                      uint32_t* depth_offset, uint32_t* stencil_offset);
uint32_t shader_depth_stencil_declare(SpvEmitter* e, uint32_t u32_type, uint32_t set, uint32_t binding);
/* Header entries, incoming depth and all other arguments are SSA IDs. Depth is a raw binary32 word, stencil
   is an unsigned byte carried in uint, front/covered are bools. This pure SSA form also serves independent tests. */
ShaderDepthStencilResult shader_depth_stencil_evaluate(SpvEmitter* e, uint32_t f32, uint32_t u32_type,
                                                        uint32_t boolean, const uint32_t header[SH_DS_HEADER_WORDS],
                                                        uint32_t incoming_depth, uint32_t old_depth,
                                                        uint32_t old_stencil, uint32_t front, uint32_t covered);
/* Position is raw host FragCoord. Outside coordinates produce index0 and inside=false before float-to-uint
   conversion, so even inactive helper invocations cannot form an out-of-bounds storage-buffer address. */
uint32_t shader_depth_stencil_coordinates(SpvEmitter* e, uint32_t f32, uint32_t u32_type, uint32_t boolean,
                                           uint32_t buffer, uint32_t position, uint32_t* inside);
/* Call inside one ordered pixel-interlock region, exactly once after guest code. The caller intersects covered
   with live/helper, guest discard, alpha test and bit0 of both input/output coverage. The header also carries the
   original fixed bit0 independently of SampleMask input. Return the color-pass bool.
   Stencil fail/depth-fail writes still occur when color fails. Native depth writes must remain disabled. */
uint32_t shader_depth_stencil_test(SpvEmitter* e, uint32_t f32, uint32_t u32_type, uint32_t boolean,
                                   uint32_t buffer, uint32_t index, uint32_t incoming_depth,
                                   uint32_t front, uint32_t covered);
#endif
