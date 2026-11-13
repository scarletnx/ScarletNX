// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2016-2026 melonDS team
// Copyright 2026 the ScarletNX contributors
// The passes over a drawn layer, each blended onto it: edge marking (an opaque polygon's edge pixel whose neighbor is
// of another polygon ID and farther takes its ID's edge color) and fog (a density by the pixel's depth, from the
// table). After melonDS's 3DFinalPassEdgeFS.glsl and 3DFinalPassFogFS.glsl (GPL-3.0-or-later), with the software
// renderer's rule that only a polygon's edge pixels are marked.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "config3d.glsl"
#include "layers.glsl"
layout(push_constant) uniform Pass { int pass; }; // 1 edge marking, 2 fog
layout(location = 0) out vec4 o_color;

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec4 attrs = texelFetch(attribute_buffer, at, 0);
    float depth = texelFetch(depth_buffer, at, 0).r;
    o_color = vec4(0.0);
    if (pass == 1) {
        if (edge_marked(at, attrs, depth)) o_color = vec4(edge_colors[int(attrs.r * 63.0 + 0.5) >> 3].rgb, 1.0);
    } else if (pass == 2 && attrs.b != 0.0) {
        int z = int(depth * 16777216.0), index = 0, fraction = 0;
        if (z >= fog_offset) {
            uint u = ((uint(z) - uint(fog_offset)) >> 2) << uint(fog_shift);
            index = int(u >> 17);
            if (index >= 32)
                index = 32;
            else
                fraction = int(u & 0x1FFFFu);
        }
        float a = fog_density[index >> 2][index & 3], b = fog_density[(index + 1) >> 2][(index + 1) & 3];
        o_color = vec4(mix(a, b, float(fraction) / 131072.0));
    }
}
