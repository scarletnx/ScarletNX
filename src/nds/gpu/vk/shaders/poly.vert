// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2016-2026 melonDS team
// Copyright 2026 the ScarletNX contributors
// A polygon's vertex as the geometry engine left it (gpu/gpu3d.c): its place on the screen in pixels, its depth and
// its W, which every interpolation is corrected by. After melonDS's 3DRenderVS.glsl (GPL-3.0-or-later).
#version 450
layout(location = 0) in uvec4 position;  // x, y, z (16 bits, shifted down by the attributes' shift), w
layout(location = 1) in uvec4 color;     // r, g, b in 255ths, alpha in 31sts
layout(location = 2) in ivec2 texcoord;  // 16ths of a texel
layout(location = 3) in ivec3 attrs; // the polygon's attributes and the depth's shift, TEXIMAGE_PARAM, the palette
// which of its triangle's corners the vertex is (one of x, y, z is 1), and in w the triangle's sides that are the
// polygon's own: bit i the side across corner i, bit 3 a line, which is all edge
layout(location = 4) in uvec4 corner;
layout(location = 0) out vec4 f_color;
layout(location = 1) out vec2 f_texcoord;
layout(location = 2) flat out ivec3 f_attribute;
layout(location = 3) out float f_depth;
layout(location = 4) noperspective out vec3 f_corner;
layout(location = 5) flat out int f_sides;
void main() {
    int shift = (attrs.x >> 16) & 0x1F;
    float depth = float(position.z << shift) / 16777216.0;
    vec4 at;
    at.xy = vec2(position.xy) * 2.0 / vec2(256.0, 192.0) - 1.0;
    at.z = depth;
    at.w = float(position.w) / 65536.0;
    at.xyz *= at.w;
    f_depth = depth;
    f_color = vec4(color) / vec4(255.0, 255.0, 255.0, 31.0);
    f_texcoord = vec2(texcoord) / 16.0;
    f_attribute = ivec3(attrs.x & 0xFFE0FFFF, attrs.y, attrs.z);
    f_corner = vec3(corner.xyz);
    f_sides = int(corner.w);
    gl_Position = at;
}
