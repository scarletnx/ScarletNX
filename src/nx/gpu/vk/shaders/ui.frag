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
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in uint v_ink;
layout(location = 3) flat in uint v_glyph;
layout(location = 0) out vec4 o;
// sRGB bytes to linear light
vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
void main() {
    vec4 t;
    if (v_ink == 1u || v_ink == 4u) // UI_SOLID, UI_RING
        t = vec4(1.0);
    else if (v_ink == 2u || v_ink == 3u) { // UI_GLYPH, UI_ICON: the pixel's bit of its row
        uvec2 p = min(uvec2(v_uv), uvec2(v_ink == 2u ? 7u : 15u, 15u));
        uint g = v_ink == 2u ? v_glyph : 256u + 2u * v_glyph + (p.x >> 3);
        uint k = g * 16u + p.y;
        uint row = (glyphs.rows[k >> 2] >> ((k & 3u) * 8u)) & 0xFFu;
        t = vec4(float((row >> (7u - (p.x & 7u))) & 1u));
    } else // unnormalized coordinates, in texels: a sample at level 0 named
        t = textureLod(atlas, v_uv, 0.0);
    vec4 c = t * vec4(v_color.rgb, 1.0) * v_color.a;
    // an sRGB picture encodes what is written to it: the bytes go in decoded, and come out as they are
    if (pc.srgb != 0u && c.a > 0.0)
        c.rgb = decode(c.rgb / c.a) * c.a;
    o = c;
}
