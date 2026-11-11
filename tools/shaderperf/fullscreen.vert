// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// snx-shaderperf's vertex shader for a fragment shader given alone: one triangle over the framebuffer (3 vertices),
// and vec4 varyings at locations 0 to 7 that a fragment shader's float inputs of up to 4 components take
// (r = u, g = v across the screen, b = location / 8, a = 1). Regenerate fullscreen_vert.h with gen_vert.sh
#version 450
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
layout(location = 4) out vec4 o4;
layout(location = 5) out vec4 o5;
layout(location = 6) out vec4 o6;
layout(location = 7) out vec4 o7;
void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    o0 = vec4(uv, 0.0, 1.0);
    o1 = vec4(uv, 0.125, 1.0);
    o2 = vec4(uv, 0.25, 1.0);
    o3 = vec4(uv, 0.375, 1.0);
    o4 = vec4(uv, 0.5, 1.0);
    o5 = vec4(uv, 0.625, 1.0);
    o6 = vec4(uv, 0.75, 1.0);
    o7 = vec4(uv, 0.875, 1.0);
}
