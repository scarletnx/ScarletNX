// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The on-screen controls and the game's menu (gpu/vk/ui.c): an instance for each quad (ui.h's SnxUiQuad, as it is),
// its six corners made here, so the CPU writes no vertices
#version 450
layout(location = 0) in vec4 rect;    // x, y, w, h in the picture's pixels
layout(location = 1) in uvec4 texels; // u, v, uw, vh: uw 0 draws nothing
layout(location = 2) in vec4 color;   // R G B A bytes, as the menu's colours are written
layout(location = 3) in uvec4 how;    // the quarter turns, the sample, the ink (ui.h's UI_ATLAS...) and the glyph
layout(push_constant) uniform Picture {
    vec2 size;
    uint srgb;
    uint ring;      // the ring's newest slot, and its slots from bit 16 (ui.h's SnxUiRing)
    uint slow;      // the colour of a bar past slow_from, R G B A bytes
    uint slow_from; // the height past which a bar is slow
} pc;
// the ring's bytes, a bar's height in 255ths of the graph's for each slot, four a word
layout(std430, set = 0, binding = 2) readonly buffer Ring {
    uint heights[];
} ring;
// UI_ATLAS: the atlas's texel, as the samplers take it (unnormalized); a glyph's or an icon's: its own pixel from its
// top left, 8x16 or 16x16
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out uint v_ink;
layout(location = 3) flat out uint v_glyph;
// the corner of a quad, clockwise from the top left: x 0 1 1 0, y 0 0 1 1
vec2 corner(uint k) {
    return vec2(float((6u >> k) & 1u), float(k >> 1));
}
void main() {
    // the two triangles' corners 0 1 2 and 0 2 3, a nibble each
    uint k = (0x320210u >> (4u * uint(gl_VertexIndex))) & 3u;
    vec4 r = rect, c = color;
    uint height = 1u;
    v_ink = how.z;
    v_glyph = how.w;
    if (how.z == 4u) { // UI_RING: its slot's bar, as ui_drawn gives it
        uint n = pc.ring >> 16, slot = how.w;
        height = n != 0u && slot < n ? (ring.heights[slot >> 2] >> ((slot & 3u) * 8u)) & 0xFFu : 0u;
        if (height != 0u) {
            uint age = ((pc.ring & 0xFFFFu) + n - slot) % n;
            float h = r.w * float(height) / 255.0;
            r.x -= float(age + 1u) * r.z;
            r.y += r.w - h;
            r.w = h;
            if (height > pc.slow_from) c = unpackUnorm4x8(pc.slow);
        }
    }
    if (texels.z == 0u || height == 0u) { // a blank cell, or no bar: every corner on one point, which draws nothing
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
        v_uv = vec2(0.0);
        v_color = vec4(0.0);
        return;
    }
    gl_Position = vec4((r.xy + corner(k) * r.zw) / pc.size * 2.0 - 1.0, 0.0, 1.0);
    // the texels turned: a quarter turn shows the bottom left one at the top left
    v_uv = corner((k + 4u - (how.x & 3u)) & 3u) * vec2(texels.zw);
    if (how.z == 0u) v_uv += vec2(texels.xy);
    v_color = c;
}
