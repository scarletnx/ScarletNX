// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2016-2026 melonDS team
// Copyright 2026 the ScarletNX contributors
// A polygon's pixel: its vertex color, toon or highlight shaded, with its texel modulated or decaled, as melonDS's
// 3DRenderFS.glsl has them (GPL-3.0-or-later), and the texel decoded here from the texture memory (texture.glsl).
// The color goes out as the engine carries it, 6 bits a channel in 63rds and the alpha in 31sts; the second output is
// the pixel's attributes for the passes after: the polygon ID, the edge flag, the fog bit and the edge's coverage.
//
// The pixel knows how far it is from its polygon's sides (edge_distance), which melonDS's OpenGL renderer does not:
// that is what a wireframe polygon keeps its outline by, what edge marking and anti-aliasing take a polygon's edge
// pixels from, and what the coverage of an anti-aliased edge is. The software renderer's rules for them are the
// model (GPU3D_Soft.cpp): an edge pixel is the first or last of its row or column in the polygon.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "config3d.glsl"
#include "texture.glsl"
layout(push_constant) uniform Mode { int render_mode; }; // 0 opaque pixels, 1 translucent ones, 2 a shadow's mask
layout(location = 0) in vec4 f_color;
layout(location = 1) in vec2 f_texcoord;
layout(location = 2) flat in ivec3 f_attribute;
layout(location = 3) in float f_depth;
layout(location = 4) noperspective in vec3 f_corner;
layout(location = 5) flat in int f_sides;
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

// How many pixels the pixel is inside its polygon, counted along the row or the column, whichever crosses the nearest
// side faster: under 1 on the polygon's first and last pixel of a row or column. f_corner is 1 at a corner of the
// triangle and 0 on the side across it, so its value over its change a pixel is that count; only the sides that are
// the polygon's own count (a polygon of more than three vertices is a fan of triangles)
// across: the way out of the polygon over that side, 0 left, 1 right, 2 up, 3 down
float edge_distance(out int across) {
    across = 0;
    if ((f_sides & 8) != 0) return 0.0;
    vec3 along_row = dFdx(f_corner), along_column = dFdy(f_corner);
    vec3 step = max(max(abs(along_row), abs(along_column)), vec3(1.0e-6));
    vec3 inside = f_corner / step;
    float nearest = 1.0e9;
    for (int side = 0; side < 3; ++side) {
        if ((f_sides & (1 << side)) == 0 || inside[side] >= nearest) continue;
        nearest = inside[side];
        // the corner's value falls toward its side: out is against its rise
        if (abs(along_row[side]) >= abs(along_column[side]))
            across = along_row[side] > 0.0 ? 0 : 1;
        else
            across = along_column[side] > 0.0 ? 2 : 3;
    }
    return nearest;
}

void main() {
    // W-buffered polygons carry their depth themselves; the others' is the rasterizer's
    float depth = (f_attribute.x & 0x200) != 0 ? f_depth : gl_FragCoord.z;
    gl_FragDepth = depth;
    if (render_mode == 2) {
        o_color = vec4(0.0, 0.0, 0.0, 1.0);
        o_attribute = vec4(0.0);
        return;
    }
    bool wireframe = (f_attribute.x & 0x400) != 0;
    int across;
    float inside = edge_distance(across);
    bool edge = inside < 1.0;
    if (wireframe && !edge) discard; // a wireframe polygon is its outline
    vec4 color = final_color();
    if (wireframe) color.a = 1.0;
    int alpha = int(color.a * 31.0 + 0.5);
    if (alpha <= alpha_ref) discard;
    if (render_mode != 1) {
        if (alpha < 31) discard;
        // An anti-aliased edge's coverage, 0 to 31, kept one up so that 0 says no edge, and over it the way across the
        // edge; lines and outlines are whole. The pixel's center is inside the polygon, so the polygon has half of
        // the pixel and as much more as the center is inside
        int coverage = 31;
        if ((disp_cnt & 0x10) != 0 && !wireframe && (f_sides & 8) == 0)
            coverage = clamp(int(16.0 + inside * 32.0), 0, 31);
        o_attribute = vec4(float((f_attribute.x >> 24) & 0x3F) / 63.0, edge ? 1.0 : 0.0,
                           float((f_attribute.x >> 15) & 1), edge ? float((coverage + 1) | (across << 6)) / 255.0 : 0.0);
    } else {
        if (alpha == 0 || alpha >= 31) discard;
        o_attribute = vec4(0.0);
    }
    o_color = color;
}
