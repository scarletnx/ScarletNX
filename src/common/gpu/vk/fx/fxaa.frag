/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* FXAA over the effects' image, a pixel for a pixel: the shared filter (common/gpu/vk/fxaa.glsl) at each pixel's
   centre, held inside the image, which is the shown crop alone. flags.x is 1 where the image holds linear light (a
   frame read through its sRGB view), whose luma the filter judges as the display shows it. */
#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 0) uniform sampler2D source_tex;

layout(push_constant) uniform Fxaa {
    uvec4 flags;
} pc;

layout(location = 0) out vec4 o;

#include "common/gpu/vk/fxaa.glsl"

void main() {
    vec2 size = vec2(textureSize(source_tex, 0));
    o = vec4(fxaa(source_tex, gl_FragCoord.xy, vec4(vec2(0.5), size - 0.5), pc.flags.x != 0u).rgb, 1.0);
}
