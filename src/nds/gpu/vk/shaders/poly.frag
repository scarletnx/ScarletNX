// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2016-2026 melonDS team
// Copyright 2026 the ScarletNX contributors
// A polygon's pixel: its vertex color, toon or highlight shaded, with its texel modulated or decaled, as melonDS's
// 3DRenderFS.glsl has them (GPL-3.0-or-later), and the texel decoded here from the texture memory (texture.glsl).
// The color goes out as the engine carries it, 6 bits a channel in 63rds and the alpha in 31sts; the second output is
// the pixel's attributes for the passes after (the polygon ID for edge marking, the fog bit).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "config3d.glsl"
#include "texture.glsl"
layout(push_constant) uniform Mode { int render_mode; }; // 0 opaque pixels, 1 translucent ones, 2 a shadow's mask
layout(location = 0) in vec4 f_color;
layout(location = 1) in vec2 f_texcoord;
layout(location = 2) flat in ivec3 f_attribute;
layout(location = 3) in float f_depth;
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_attribute;

vec4 final_color() {
    vec4 vertex = f_color, color;
    int blend = (f_attribute.x >> 4) & 3;
    bool highlight = (disp_cnt & 2) != 0;
    vec3 toon = toon_colors[int(vertex.r * 31.0 + 0.5) & 31].rgb;
    if (blend == 2) vertex.rgb = highlight ? vertex.rrr : toon;
    uint param = uint(f_attribute.y);
    if ((disp_cnt & 1) == 0 || ((param >> 26) & 7u) == 0u)
        color = vertex;
    else {
        uint alpha;
        uint c = texel(param, uint(f_attribute.z), ivec2(floor(f_texcoord)), alpha);
        // five bits to six as the engine widens them: doubled, and one more unless zero
        uvec3 c5 = uvec3(c & 0x1Fu, (c >> 5) & 0x1Fu, (c >> 10) & 0x1Fu);
        uvec3 c6 = c5 * 2u + uvec3(notEqual(c5, uvec3(0u)));
        vec4 t = vec4(vec3(c6) / 63.0, float(alpha) / 31.0);
        if ((blend & 1) != 0) {
            color.rgb = t.rgb * t.a + vertex.rgb * (1.0 - t.a);
            color.a = vertex.a;
        } else
            color = vertex * t;
    }
    if (blend == 2 && highlight) color.rgb = min(color.rgb + toon, 1.0);
    return color;
}

void main() {
    // W-buffered polygons carry their depth themselves; the others' is the rasterizer's
    gl_FragDepth = (f_attribute.x & 0x200) != 0 ? f_depth : gl_FragCoord.z;
    if (render_mode == 2) {
        o_color = vec4(0.0, 0.0, 0.0, 1.0);
        o_attribute = vec4(0.0);
        return;
    }
    vec4 color = final_color();
    int alpha = int(color.a * 31.0 + 0.5);
    if (alpha <= alpha_ref) discard;
    if (render_mode == 0) {
        if (alpha < 31) discard;
        o_attribute = vec4(float((f_attribute.x >> 24) & 0x3F) / 63.0, 0.0, float((f_attribute.x >> 15) & 1), 1.0);
    } else {
        if (alpha == 0 || alpha >= 31) discard;
        o_attribute = vec4(0.0, 0.0, 0.0, 1.0);
    }
    o_color = color;
}
