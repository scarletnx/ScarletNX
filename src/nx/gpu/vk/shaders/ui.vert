// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The on-screen controls and the game's menu (gpu/vk/ui.c): an instance for each quad (ui.h's SnxUiQuad, 20 bytes,
// as it is), its six corners made here, so the CPU writes no vertices
#version 450
layout(location = 0) in ivec2 at;     // x, y in quarters of the picture's pixels
layout(location = 1) in uvec2 size;   // w, h in quarters
layout(location = 2) in uvec4 texels; // u, v, uw, vh: uw 0 draws nothing
layout(location = 3) in uint bits;    // ui.h's UI_Q_*: R5 G5 B5 A1, turns, backed, sample, ink, glyph
layout(push_constant) uniform Picture {
    vec2 size;
    uint srgb;
    uint ring;      // the ring's newest slot, and its slots from bit 16 (ui.h's SnxUiRing)
    uint slow;      // the colour of a bar past slow_from, R G B A bytes
    uint slow_from; // the height past which a bar is slow
    uint ground;    // the backed letters' ground, R G B A bytes (ui.h's UI_BACKED)
} pc;
// the ring's bytes, a bar's height in 255ths of the graph's for each slot, four a word
layout(std430, set = 0, binding = 2) readonly buffer Ring {
    uint heights[];
} ring;
// UI_ATLAS: the atlas's texel, as the samplers take it (unnormalized); a glyph's or an icon's: its own pixel from its
// top left, 8x16 or 16x16
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out uint v_ink; // and from bit 8, UI_BACKED
layout(location = 3) flat out uint v_glyph;
layout(location = 4) flat out vec2 v_size;   // UI_SHAPE: its size in pixels, v_uv its pixel from the top left
layout(location = 5) flat out uvec4 v_shape; // and its kind, label, flags and edge (ui.h's ui_shape)
// the corner of a quad, clockwise from the top left: x 0 1 1 0, y 0 0 1 1
vec2 corner(uint k) {
    return vec2(float((6u >> k) & 1u), float(k >> 1));
}
// An outline shape's parts (ui.h's UI_SHAPE, unfilled, with an edge: a circle, a round box or the cross), each a quad
// of its own: where ui.frag writes, its edge and its label, and little more, so the inside it leaves alone takes no
// fragment. Part `part`'s corner k, from the shape's top left, into p; 0 for a part the shape has none of. size is the
// shape's, e its edge's width: each part reaches a pixel past what it covers, so every pixel's middle that ui.frag
// writes is in one
#define OUTLINE_PARTS 16u
bool outline_part(uint kind, uint label, float e, vec2 size, uint part, uint k, out vec2 p) {
    float m = min(size.x, size.y), t = e + 1.0;
    vec2 c = size * 0.5;
    vec4 b = vec4(0.0); // x, y, w, h
    p = vec2(0.0);
    if (part == OUTLINE_PARTS - 1u) { // the label's box, as ui.frag places it, a pixel wider each way
        uint n = (label >> 8) != 0u ? 2u : (label & 0xFFu) != 0u ? 1u : 0u;
        if (n == 0u || 16.0 > size.y * 0.8 || 8.0 * float(n) > size.x * 0.8) return false;
        float s = max(1.0, floor(min(size.y * 0.5 / 16.0, size.x * 0.6 / (8.0 * float(n)))));
        vec2 at = floor((size - vec2(8.0 * float(n), 16.0) * s) * 0.5);
        b = vec4(at - 1.0, vec2(8.0 * float(n), 16.0) * s + 2.0);
    } else if (kind == 0u) { // a circle: a ring of segments, from inside its edge to past the circle
        float r = m * 0.5, n = float(OUTLINE_PARTS - 1u), a = 6.28318530718 / n;
        float inner = max(r - t, 0.0), outer = (r + 1.0) / cos(a * 0.5);
        float angle = a * float(part + ((k == 2u || k == 3u) ? 1u : 0u));
        p = c + vec2(cos(angle), sin(angle)) * ((k == 1u || k == 2u) ? outer : inner);
        return true;
    } else if (kind == 1u) { // a round box: its sides' strips, and its corners' squares
        float q = m * 0.25 + 1.0;
        vec4 parts[8] = vec4[8](vec4(0.0, 0.0, size.x, t), vec4(0.0, size.y - t, size.x, t), vec4(0.0, 0.0, t, size.y),
                                vec4(size.x - t, 0.0, t, size.y), vec4(0.0, 0.0, q, q), vec4(size.x - q, 0.0, q, q),
                                vec4(0.0, size.y - q, q, q), vec4(size.x - q, size.y - q, q, q));
        if (part >= 8u) return false;
        b = parts[part];
    } else { // the cross: each bar's four sides' strips, its arms a third of the shorter side wide
        float a = m / 3.0, x0 = c.x - a * 0.5, y0 = c.y - a * 0.5;
        vec4 parts[8] = vec4[8](vec4(0.0, y0, size.x, t), vec4(0.0, y0 + a - t, size.x, t), vec4(0.0, y0, t, a),
                                vec4(size.x - t, y0, t, a), vec4(x0, 0.0, t, size.y), vec4(x0 + a - t, 0.0, t, size.y),
                                vec4(x0, 0.0, a, t), vec4(x0, size.y - t, a, t));
        if (part >= 8u) return false;
        b = parts[part];
    }
    p = b.xy + corner(k) * b.zw;
    return true;
}
void main() {
    // the two triangles' corners 0 1 2 and 0 2 3, a nibble each, six vertices a part (an outline shape's parts)
    uint vertex = uint(gl_VertexIndex), part = vertex / 6u, k = (0x320210u >> (4u * (vertex % 6u))) & 3u;
    uint turn = (bits >> 16) & 3u, ink = (bits >> 21) & 7u, glyph = bits >> 24;
    // each 5 bits widened as ui.c's from5: the top bits again under them; the alpha a fill's or a sprite's glyph byte,
    // else A1
    uvec3 c5 = uvec3(bits, bits >> 5, bits >> 10) & 31u;
    vec4 r = vec4(vec2(at), vec2(size)) * 0.25,
         c = vec4(vec3(c5 << 3 | c5 >> 2) / 255.0, ink <= 1u ? float(glyph) / 255.0 : float((bits >> 15) & 1u));
    uint height = 1u;
    v_ink = ink | (bits >> 18 & 1u) << 8;
    v_glyph = glyph;
    v_shape = texels;
    if (ink == 4u) { // UI_RING: its slot's bar, as ui_drawn gives it, w / n wide over the graph
        uint n = pc.ring >> 16, slot = glyph;
        height = n != 0u && slot < n ? (ring.heights[slot >> 2] >> ((slot & 3u) * 8u)) & 0xFFu : 0u;
        if (height != 0u) {
            uint age = ((pc.ring & 0xFFFFu) + n - slot) % n;
            float h = r.w * float(height) / 255.0, bw = r.z / float(n);
            r.x += bw * float(n - 1u - age);
            r.z = bw;
            r.y += r.w - h;
            r.w = h;
            if (height > pc.slow_from) c = unpackUnorm4x8(pc.slow);
        }
    }
    // where the quad has its part's corner: an outline shape's part (ui.c draws them with OUTLINE_PARTS parts),
    // anything else its one quad
    vec2 local = corner(k) * r.zw;
    bool shown = part == 0u;
    if (ink == 5u && (texels.z & 3u) == 0u && texels.x <= 2u)
        shown = outline_part(texels.x, texels.y, float(texels.w), r.zw, part, k, local);
    if (texels.z == 0u || height == 0u || !shown) { // a blank cell, no bar, or no such part: every corner on one point,
        // which draws nothing
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
        v_uv = vec2(0.0);
        v_size = vec2(0.0);
        v_color = vec4(0.0);
        return;
    }
    gl_Position = vec4((r.xy + local) / pc.size * 2.0 - 1.0, 0.0, 1.0);
    // the texels turned: a quarter turn shows the bottom left one at the top left
    v_size = r.zw;
    if (ink == 5u) // UI_SHAPE: no texels, its own pixels
        v_uv = local;
    else
        v_uv = corner((k + 4u - turn) & 3u) * vec2(texels.zw);
    if (ink == 0u) v_uv += vec2(texels.xy);
    v_color = c;
}
