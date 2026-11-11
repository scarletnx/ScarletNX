// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The on-screen controls and the game's menu (gpu/vk/ui.c): an instance for each quad (ui.h's SnxUiQuad, as it is),
// its six corners made here, so the CPU writes no vertices
#version 450
layout(location = 0) in vec4 rect;    // x, y, w, h in the picture's pixels
layout(location = 1) in uvec4 texels; // u, v, uw, vh: uw 0 draws nothing
layout(location = 2) in vec4 color;   // R G B A bytes, as the menu's colours are written
layout(location = 3) in uvec4 how;    // the quarter turns, the sample, two spare
layout(push_constant) uniform Picture {
    vec2 size;
    uint srgb;
} pc;
layout(location = 0) out vec2 v_uv; // in texels: the samplers take unnormalized coordinates
layout(location = 1) out vec4 v_color;
// the corner of a quad, clockwise from the top left: x 0 1 1 0, y 0 0 1 1
vec2 corner(uint k) {
    return vec2(float((6u >> k) & 1u), float(k >> 1));
}
void main() {
    // the two triangles' corners 0 1 2 and 0 2 3, a nibble each
    uint k = (0x320210u >> (4u * uint(gl_VertexIndex))) & 3u;
    if (texels.z == 0u) { // a blank cell: every corner on one point, which draws nothing
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
        v_uv = vec2(0.0);
        v_color = vec4(0.0);
        return;
    }
    gl_Position = vec4((rect.xy + corner(k) * rect.zw) / pc.size * 2.0 - 1.0, 0.0, 1.0);
    // the texels turned: a quarter turn shows the bottom left one at the top left
    v_uv = vec2(texels.xy) + corner((k + 4u - (how.x & 3u)) & 3u) * vec2(texels.zw);
    v_color = color;
}
