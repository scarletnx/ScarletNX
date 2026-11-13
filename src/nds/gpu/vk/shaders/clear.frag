// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2016-2026 melonDS team
// Copyright 2026 the ScarletNX contributors
// The clear bitmap: with DISP3DCNT's bit 14 the 3D picture starts from two 256 by 256 bitmaps in texture memory, not
// from a plain plane. Slot 2 has each pixel's color and whether it is there, slot 3 its depth and fog bit, both moved
// by CLRIMAGE_OFFSET and wrapped. Read where they lie in the copy of texture memory, as melonDS's renderers read them
// (GPU3D_OpenGL.cpp and 3DClearBitmapFS.glsl, GPL-3.0-or-later).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "config3d.glsl"
#include "texture.glsl"
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_attribute;

void main() {
    uvec2 at = (uvec2(gl_FragCoord.xy) + uvec2((clear_attr2 >> 16) & 0xFF, (clear_attr2 >> 24) & 0xFF)) & 255u;
    uint offset = (at.y * 256u + at.x) * 2u;
    uint c = tex16(0x40000u + offset), d = tex16(0x60000u + offset);
    uvec3 c5 = uvec3(c & 0x1Fu, (c >> 5) & 0x1Fu, (c >> 10) & 0x1Fu);
    uvec3 c6 = c5 * 2u + uvec3(notEqual(c5, uvec3(0u)));
    o_color = vec4(vec3(c6) / 63.0, (c & 0x8000u) != 0u ? 1.0 : 0.0);
    o_attribute = vec4(float((clear_attr1 >> 24) & 0x3F) / 63.0, 0.0, float(d >> 15), 0.0);
    gl_FragDepth = float((d & 0x7FFFu) * 0x200u + 0x1FFu) / 16777216.0;
}
