// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The on-screen layer's pixel (gpu/vk/ui.c), one of two pipelines by MODE, a specialization constant, so that each
// takes only its own work. Nothing is translucent: a pixel is written opaque or not at all.
// - MODE 0, plain: a quad that covers every pixel of its own (a fill, a bar, a backed letter or icon): its label's bit
//   picks its colour or the ground (a fill's label, the full block, is all set). No discard, no texture, no branch.
// - MODE 1 and 2, sparse: a shape, a letter or an icon that leaves its clear pixels alone, or the atlas's texels (the
//   user's picture, hard at its round edge, a save state's thumbnail). A pixel not drawn is discarded (1), or on a GPU
//   that renders in tiles, where a discard costs its tile's early tests, written clear and blended away (2: the blend
//   is premultiplied, so a clear pixel leaves the picture as it was).
// SRGB is set for a surface with no UNORM format, whose sRGB image encodes what is written to it: the bytes go in
// decoded, and come out as they are.
// SPACE is what the picture takes when it is the swapchain of a game's HDR frames (gpu/hdr_output.h): 1 PQ codes in
// BT.2020's primaries, 2 scRGB, linear light with 1.0 at 80 nits. The layer's colours are then written as that, their
// white at WHITE nits; 0, the bytes as above.
#version 450
layout(constant_id = 0) const uint MODE = 0u;
layout(constant_id = 1) const bool SRGB = false;
layout(constant_id = 2) const uint SPACE = 0u;
layout(constant_id = 3) const float WHITE = 203.0;
layout(set = 0, binding = 0) uniform sampler2D atlas;
// gui_vga_font's 256 letters, then each icon's halves (gui_icon_font), 16 rows of a byte each, four bytes a word
layout(std430, set = 0, binding = 1) readonly buffer Glyphs {
    uint rows[];
} glyphs;
layout(push_constant) uniform Picture {
    vec2 size;
    uint ring, slow, slow_from; // ui.vert's
    uint ground;                // a backed letter's clear pixels' colour
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in vec4 v_label;
layout(location = 3) flat in uvec2 v_glyphs;
layout(location = 4) flat in vec4 v_boxes;
layout(location = 5) flat in vec4 v_shape;
layout(location = 6) flat in uint v_flags;
layout(location = 0) out vec4 o;
// sRGB bytes to linear light
vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
// linear light in BT.709's primaries, 1.0 at 10000 nits, to SMPTE ST 2084's code in BT.2020's (BT.2087's matrix, by
// columns)
vec3 pq(vec3 l) {
    vec3 p = pow(max(mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956) * l, vec3(0.0)),
                 vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), vec3(78.84375));
}
// the label's bit at p (ui.vert's v_label: its box from v_label.xy, v_label.z a cell's pixels, v_label.w glyphs of
// eight columns): 0 outside it. The cell clamped into the label, so a pixel outside reads a row that is there
uint label_bit(vec2 p) {
    vec2 q = floor((p - v_label.xy) / v_label.z), last = vec2(max(8.0 * v_label.w - 1.0, 0.0), 15.0);
    uvec2 cell = uvec2(clamp(q, vec2(0.0), last));
    uint k = v_glyphs[cell.x >> 3] * 16u + cell.y;
    uint bit = (glyphs.rows[k >> 2] >> ((k & 3u) * 8u + 7u - (cell.x & 7u))) & 1u;
    return bit & uint(q == clamp(q, vec2(0.0), last)) & uint(v_label.w > 0.0); // bitwise: no branch for an &&
}
// a box's signed distance at p from its middle, half its size b
float box_distance(vec2 p, vec2 b) {
    vec2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}
void main() {
    vec3 c = v_color.rgb;
    bool drawn = true;
    if (MODE == 0u) // plain: the colour on its bits, the ground on the others
        c = mix(unpackUnorm4x8(pc.ground).rgb, c, float(label_bit(v_uv)));
    else if ((v_flags & 2u) != 0u) { // the atlas's texels (alpha premultiplied) times the colour, at half or more
        vec4 t = textureLod(atlas, v_uv, 0.0) * vec4(v_color.rgb, 1.0) * v_color.a;
        drawn = t.a >= 0.5;
        c = t.rgb / max(t.a, 1e-6);
    } else { // in the shape, and the label (cut out when filled) or the edge
        vec2 p = v_uv - v_shape.xy;
        float d = min(box_distance(p, v_boxes.xy), box_distance(p, v_boxes.zw)) - v_shape.z;
        // inside & ((label ^ filled) | edge), bitwise (no branch for an && or an ||): the same as inside it, then for a
        // filled one its label cut out, else its label or its edge (the edge's width 0 when filled or bare)
        drawn = (uint(!(d > 0.0)) & ((label_bit(v_uv) ^ (v_flags & 1u)) | uint(d > -v_shape.w))) != 0u;
    }
    if (SRGB) c = decode(c);
    if (SPACE == 1u) c = pq(decode(c) * (WHITE / 10000.0));
    if (SPACE == 2u) c = decode(c) * (WHITE / 80.0);
    if (MODE == 1u && !drawn) discard;
    o = vec4(c, 1.0) * (MODE == 2u ? float(drawn) : 1.0); // MODE 2: clear, blended away, when not drawn
}
