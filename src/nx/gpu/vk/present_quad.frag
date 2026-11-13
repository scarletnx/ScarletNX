/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
// flags.x: the transform (1 flips x, 2 flips y, 4 turns a quarter); flags.y: 1 smooths the picture's edges (FXAA, the
// anti_aliasing setting) with the linear sampler, 2 when source holds linear light (its sRGB view)
#version 450
#extension GL_GOOGLE_include_directive : require
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Parameters { vec4 crop; uvec4 flags; } parameters;
layout(location = 0) in vec2 coordinate;
layout(location = 0) out vec4 color;
#include "common/gpu/vk/fxaa.glsl"
void main() {
    vec2 p = coordinate;
    if ((parameters.flags.x & 4u) != 0u) p = vec2(p.y, 1.0 - p.x);
    if ((parameters.flags.x & 1u) != 0u) p.x = 1.0 - p.x;
    if ((parameters.flags.x & 2u) != 0u) p.y = 1.0 - p.y;
    vec2 at = parameters.crop.xy + p * parameters.crop.zw;
    if ((parameters.flags.y & 1u) != 0u) {
        // the crop's first and last pixel centres: the game's frame is that part of the surface
        vec2 size = vec2(textureSize(source, 0)), first = parameters.crop.xy * size + 0.5,
             last = (parameters.crop.xy + parameters.crop.zw) * size - 0.5;
        color = fxaa(source, at * size, vec4(min(first, last), max(first, last)), (parameters.flags.y & 2u) != 0u);
    } else
        color = texture(source, at);
}
