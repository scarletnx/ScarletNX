// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The on-screen controls and the game's menu (gpu/vk/ui.c): an instance for each quad (ui.h's SnxUiQuad, 20 bytes,
// as it is), its six corners made here, so the CPU writes no vertices. Each quad's kind (its ink) is read here, once a
// vertex, and given to ui.frag as the few numbers its one way of drawing takes (flat), so that ui.frag tells no kind
// from another:
// - a label: the 8x16 font's glyphs at a scale in a box, a bit a pixel. A letter is its glyph over the quad, an icon
//   its two halves, a fill (UI_SOLID, UI_RING) the font's full block (0xDB) scaled over the whole quad;
// - a shape (the plain pipeline has none): the smaller of two boxes' signed distances, less a radius, at most an edge
//   in from 0. A circle is two boxes of no size and its radius, a round box itself less its corners twice, a box itself
//   with no radius, the cross its two bars; a letter of the sparse pipeline a box a pixel past the quad, its label the
//   letter;
// - or, for UI_ATLAS, the atlas's texels.
#version 450
layout(location = 0) in ivec2 at;     // x, y in quarters of the picture's pixels
layout(location = 1) in uvec2 size;   // w, h in quarters
layout(location = 2) in uvec4 texels; // u, v, uw, vh: uw 0 draws nothing
layout(location = 3) in uint bits;    // ui.h's UI_Q_*: R5 G5 B5 A1, turns, backed, sample, ink, glyph
layout(push_constant) uniform Picture {
    vec2 size;
    uint ring;      // the ring's newest slot, and its slots from bit 16 (ui.h's SnxUiRing)
    uint slow;      // the colour of a bar past slow_from, R G B A bytes
    uint slow_from; // the height past which a bar is slow
    uint ground;    // the backed letters' ground, R G B A bytes (ui.h's UI_BACKED): ui.frag's
} pc;
// the ring's bytes, a bar's height in 255ths of the graph's for each slot, four a word
layout(std430, set = 0, binding = 2) readonly buffer Ring {
    uint heights[];
} ring;
// the pixel from the quad's top left, or UI_ATLAS's texel (the samplers take unnormalized coordinates)
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out vec4 v_label;   // the label's box from the quad's top left, its scale, its glyphs
layout(location = 3) flat out uvec2 v_glyphs; // its glyphs: the first eight columns', the next eight's
layout(location = 4) flat out vec4 v_boxes;   // the two boxes' half sizes
layout(location = 5) flat out vec4 v_shape;   // the boxes' middle, the radius, the edge's width (0 for none)
layout(location = 6) flat out uint v_flags;   // 1 filled (the label cut out), 2 the atlas's texels
// the corner of a quad, clockwise from the top left: x 0 1 1 0, y 0 0 1 1
vec2 corner(uint k) {
    return vec2(float((6u >> k) & 1u), float(k >> 1));
}
// An outline shape's parts (ui.h's UI_SHAPE, unfilled, with an edge: a circle, a round box or the cross), each a quad
// of its own: where ui.frag writes, its edge and its label, and little more, so the inside it leaves alone takes no
// fragment. Part `part`'s corner k, from the shape's top left, into p; 0 for a part the shape has none of. size is the
// shape's, e its edge's width, label its label's box (v_label): each part reaches a pixel past what it covers, so every
// pixel's middle that ui.frag writes is in one
#define OUTLINE_PARTS 16u
bool outline_part(uint kind, vec4 label, float e, vec2 size, uint part, uint k, out vec2 p) {
    float m = min(size.x, size.y), t = e + 1.0;
    vec2 c = size * 0.5;
    vec4 b; // x, y, w, h
    p = vec2(0.0);
    if (part == OUTLINE_PARTS - 1u) { // the label's box, a pixel wider each way
        if (label.w == 0.0) return false;
        b = vec4(label.xy - 1.0, vec2(8.0 * label.w, 16.0) * label.z + 2.0);
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
    // a letter's (UI_GLYPH): its glyph over the quad, in a box a pixel past it with no radius and no edge
    vec2 half_size = r.zw * 0.5;
    float m = min(r.z, r.w);
    v_label = vec4(0.0, 0.0, r.w / 16.0, 1.0);
    v_glyphs = uvec2(glyph);
    v_boxes = vec4(half_size + 1.0, half_size + 1.0);
    v_shape = vec4(half_size, 0.0, 0.0);
    v_flags = 0u;
    if (ink == 3u) // UI_ICON: its two halves
        v_glyphs = uvec2(256u + 2u * glyph) + uvec2(0u, 1u), v_label.w = 2.0;
    else if (ink == 1u || ink == 4u) // UI_SOLID, UI_RING: the full block over all of it
        v_glyphs = uvec2(0xDBu), v_label.z = max(max(r.z / 8.0, r.w / 16.0), 1e-6);
    else if (ink == 0u) // UI_ATLAS: its texels
        v_flags = 2u;
    else if (ink == 5u) { // UI_SHAPE: kind, label, flags and edge in its texels (ui.h's ui_shape)
        uint kind = texels.x, label = texels.y, n = (label >> 8) != 0u ? 2u : (label & 0xFFu) != 0u ? 1u : 0u;
        // no room for it at 1: the font's 16 rows and a pixel above and under them, its columns in four fifths
        if (n == 0u || 16.0 > r.w - 2.0 || 8.0 * float(n) > r.z * 0.8)
            n = 0u;
        float s = max(1.0, floor(min(r.w * 0.5 / 16.0, r.z * 0.6 / (8.0 * float(max(n, 1u))))));
        v_label = vec4(floor((r.zw - vec2(8.0 * float(n), 16.0) * s) * 0.5), s, float(n));
        v_glyphs = uvec2(label & 0xFFu, label >> 8);
        // the boxes: a circle's of no size, its radius half the shorter side; a round box's itself less its corners
        // (a quarter of the shorter side); the cross's its two bars, a third of the shorter side wide; a box's itself
        vec2 inner = kind == 0u ? vec2(0.0) : kind == 1u ? half_size - m * 0.25 : half_size;
        v_boxes = kind == 2u ? vec4(half_size.x, m / 6.0, m / 6.0, half_size.y) : vec4(inner, inner);
        v_shape.z = kind == 0u ? m * 0.5 : kind == 1u ? m * 0.25 : 0.0;
        // filled, or bare (the label alone): no edge
        v_shape.w = (texels.z & 3u) != 0u ? 0.0 : float(texels.w);
        v_flags = texels.z & 1u;
    }
    // where the quad has its part's corner: an outline shape's parts (ui.c draws them with OUTLINE_PARTS parts),
    // anything else its one quad
    vec2 local = corner(k) * r.zw;
    bool shown = part == 0u;
    if (ink == 5u && (texels.z & 3u) == 0u && texels.x <= 2u)
        shown = outline_part(texels.x, v_label, float(texels.w), r.zw, part, k, local);
    if (texels.z == 0u || height == 0u || !shown) { // a blank cell, no bar, or no such part: every corner on one point,
        // which draws nothing
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
        v_uv = vec2(0.0);
        v_color = vec4(0.0);
        return;
    }
    gl_Position = vec4((r.xy + local) / pc.size * 2.0 - 1.0, 0.0, 1.0);
    // the texels turned: a quarter turn shows the bottom left one at the top left
    v_uv = ink == 0u ? vec2(texels.xy) + corner((k + 4u - turn) & 3u) * vec2(texels.zw) : local;
    v_color = c;
}
