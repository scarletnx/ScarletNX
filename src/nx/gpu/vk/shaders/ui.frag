// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The quad's texel (alpha premultiplied) times its colour, blended over the picture. A fill takes no texel; a letter
// of the font or an icon of the menus takes its bit, white or clear, from the glyphs' rows: no atlas read for either
#version 450
layout(set = 0, binding = 0) uniform sampler2D atlas;
// gui_vga_font's 256 letters, then each icon's halves (gui_icon_font), 16 rows of a byte each, four bytes a word
layout(std430, set = 0, binding = 1) readonly buffer Glyphs {
    uint rows[];
} glyphs;
layout(push_constant) uniform Picture {
    vec2 size;
    uint srgb;
    uint ring, slow, slow_from; // ui.vert's
    uint ground;                // a backed letter's clear pixels' colour
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in uint v_ink; // ui.h's UI_ATLAS..., and from bit 8 UI_BACKED
layout(location = 3) flat in uint v_glyph;
layout(location = 4) flat in vec2 v_size;
layout(location = 5) flat in uvec4 v_shape;
layout(location = 0) out vec4 o;
// sRGB bytes to linear light
vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
// a glyph's row of the font or the icons: g's row y, a byte
uint glyph_row(uint g, uint y) {
    uint k = g * 16u + y;
    return (glyphs.rows[k >> 2] >> ((k & 3u) * 8u)) & 0xFFu;
}
// a box's signed distance, half its size b, from its middle
float box_distance(vec2 p, vec2 b) {
    vec2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}
// UI_SHAPE's (ui.h): its signed distance at p from its top left, below 0 inside
float shape_distance(uint kind, vec2 p, vec2 size) {
    vec2 c = p - size * 0.5;
    float m = min(size.x, size.y);
    if (kind == 0u) // a circle on the shorter side
        return length(c) - m * 0.5;
    if (kind == 1u) // a box with round corners a quarter of the shorter side
        return box_distance(c, size * 0.5 - m * 0.25) - m * 0.25;
    if (kind == 3u) // a box
        return box_distance(c, size * 0.5);
    // a cross, its arms a third of the shorter side wide
    return min(box_distance(c, vec2(size.x * 0.5, m / 6.0)), box_distance(c, vec2(m / 6.0, size.y * 0.5)));
}
// the label's bit at p: its letters at the greatest whole scale in half the height and 0.6 of the width, in the middle;
// none where they don't fit 0.8 of each at their own size
bool label_bit(uint label, vec2 p, vec2 size) {
    uint c0 = label & 0xFFu, c1 = label >> 8, n = c1 != 0u ? 2u : c0 != 0u ? 1u : 0u;
    if (n == 0u || 16.0 > size.y * 0.8 || 8.0 * float(n) > size.x * 0.8) return false; // no room for it at 1
    float s = max(1.0, floor(min(size.y * 0.5 / 16.0, size.x * 0.6 / (8.0 * float(n)))));
    vec2 at = floor((size - vec2(8.0 * float(n), 16.0) * s) * 0.5), q = floor((p - at) / s);
    if (q.x < 0.0 || q.y < 0.0 || q.x >= 8.0 * float(n) || q.y >= 16.0) return false;
    uint x = uint(q.x);
    return ((glyph_row(x < 8u ? c0 : c1, uint(q.y)) >> (7u - (x & 7u))) & 1u) != 0u;
}
void main() {
    uint ink = v_ink & 0xFFu;
    vec4 t = vec4(1.0), color = v_color;
    if (ink == 2u || ink == 3u) { // UI_GLYPH, UI_ICON: the pixel's bit of its row
        uvec2 p = min(uvec2(v_uv), uvec2(ink == 2u ? 7u : 15u, 15u));
        uint g = ink == 2u ? v_glyph : 256u + 2u * v_glyph + (p.x >> 3);
        uint k = g * 16u + p.y;
        uint row = (glyphs.rows[k >> 2] >> ((k & 3u) * 8u)) & 0xFFu;
        if (((row >> (7u - (p.x & 7u))) & 1u) == 0u) { // clear: the ground where backed, else nothing written
            if ((v_ink >> 8) == 0u) discard;
            color = unpackUnorm4x8(pc.ground);
        }
    } else if (ink == 5u) { // UI_SHAPE: hard, the pixel's middle in it or not
        float d = shape_distance(v_shape.x, v_uv, v_size);
        bool mark = label_bit(v_shape.y, v_uv, v_size);
        if (d > 0.0) discard;
        if ((v_shape.z & 1u) != 0u) { // filled: the label cut out
            if (mark) discard;
        } else if (!mark && ((v_shape.z & 2u) != 0u || d <= -float(v_shape.w)))
            discard;
    } else if (ink == 0u) // UI_ATLAS: unnormalized coordinates, in texels: a sample at level 0 named
        t = textureLod(atlas, v_uv, 0.0);
    // UI_SOLID and UI_RING: the colour alone
    vec4 c = t * vec4(color.rgb, 1.0) * color.a;
    // an sRGB picture encodes what is written to it: the bytes go in decoded, and come out as they are
    if (pc.srgb != 0u && c.a > 0.0)
        c.rgb = decode(c.rgb / c.a) * c.a;
    o = c;
}
