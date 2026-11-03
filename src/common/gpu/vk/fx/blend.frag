/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The frame between two frames: the linear average of the previous and current image, for frame interpolation. */
#version 450

layout(set = 0, binding = 0) uniform sampler2D previous_tex;
layout(set = 0, binding = 1) uniform sampler2D current_tex;

layout(push_constant) uniform Blend {
    vec4 mix; /* the step, 0 the previous frame, 1 the current one */
} pc;

layout(location = 0) out vec4 o;

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(current_tex, 0));
    o = mix(texture(previous_tex, uv), texture(current_tex, uv), pc.mix.x);
}
