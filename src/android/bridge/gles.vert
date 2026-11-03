#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// GLES 1.1's vertex stage (hle_gles.c): the position through the projection and modelview matrices, GL's clip depth
// (-w to w) made Vulkan's (0 to w), and the texture coordinate and color as the draw gives them
layout(push_constant) uniform Draw {
    mat4 mvp;
    int mode;  // the texture environment: 0 no texture, 1 modulate, 2 replace, 3 decal, 4 add
    int base;  // the texture's base format: 0 RGBA, 1 RGB, 2 alpha, 3 luminance, 4 luminance and alpha
} draw;
layout(location = 0) in vec4 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
void main() {
    vec4 p = draw.mvp * position;
    p.z = (p.z + p.w) * 0.5;
    gl_Position = p;
    gl_PointSize = 1.0;
    v_uv = uv;
    v_color = color;
}
