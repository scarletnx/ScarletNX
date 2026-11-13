/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_MINMAX_SPIRV_H
#define SNX_MINMAX_SPIRV_H
#include "common/utilities/spirv_emit.h"

/* the types a min/max sample is made of: float, unsigned int, bool, a vector of four floats, and the GLSL.std.450
   import */
typedef struct {
    uint32_t f32, u32, boolean, vec4, glsl;
} ShMinmaxTypes;

/* what a filtered sample of slot takes of its texels in the program, by a key's masks (ShaderInfo.tex_minmax_mask and
   tex_minmax_max): 1 the least, 2 the greatest, 0 neither */
static inline int shader_minmax_slot(uint64_t mask, uint64_t greatest, int slot) {
    return slot < 0 || slot >= 64 || !(mask >> slot & 1) ? 0 : (greatest >> slot & 1) ? 2 : 1;
}
/* A sampler's min/max reduction made in the program, where the device's sampler can't take it (vk/render.c's
   vk_tex_minmax_in_program, ShaderInfo.tex_minmax_mask): a filtered sample of the 2D texture at sampler (a pointer to
   its combined image sampler of type sampled_type; array: an array, its layer the coordinates' third component) as
   the least of the texels its bilinear footprint takes from the base level, or the greatest (greatest), component
   by component, leaving out those of zero weight as the reduction does: four gathers, one a component, around the
   coordinates moved by offset (a vector of ints, or 0; dynamic: not a constant). A weight is zero where the
   coordinate lands on a texel's centre exactly, which hardware that rounds weights to a sub-texel step may also take
   for one a little off it. Only for a sampler that reads the base level alone and filters linearly both ways */
uint32_t shader_minmax_sample(SpvEmitter* e, const ShMinmaxTypes* t, uint32_t sampled_type, uint32_t sampler,
                              uint32_t coordinates, int array, uint32_t offset, int dynamic_offset, int greatest);
#endif
