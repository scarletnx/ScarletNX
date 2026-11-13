// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2016-2026 melonDS team
// Copyright 2026 the ScarletNX contributors
// Anti-aliasing, the last pass: an opaque polygon's edge pixel is blended by its coverage with what is under it. The
// arithmetic is melonDS's software renderer's (GPU3D_Soft.cpp, GPL-3.0-or-later): colors of 6 bits and alphas of 5,
// the color blended only over a pixel that is there, the alpha always, and an edge that edge marking colored half
// covered.
//
// The console keeps a second pixel under each edge pixel: the neighbor polygon's where two polygons of a surface
// meet (both fill their shared edge), else what the polygon hides. Here the second pixel is the picture's pixel
// across the edge (poly.frag says which way that is), taken only where it is farther than the edge pixel: that is
// the hidden surface, a pixel further on. Where two polygons of a surface meet, the blend of a surface with itself
// changes nothing on the console, and the pixel is left alone here, so a mesh's inner edges do not smear its texture.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "config3d.glsl"
#include "layers.glsl"
layout(set = 0, binding = 4) uniform sampler2D picture;
layout(location = 0) out vec4 o_color;

const ivec2 ACROSS[4] = ivec2[4](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));

bool within(ivec2 at) { return at.x >= 0 && at.y >= 0 && at.x <= 255 && at.y <= 191; }

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec4 attrs = texelFetch(attribute_buffer, at, 0), top = texelFetch(picture, at, 0);
    o_color = top;
    int code = int(attrs.a * 255.0 + 0.5);
    if ((code & 63) == 0) return; // no edge, or a translucent pixel drawn over it
    int coverage = (code & 63) - 1;
    float depth = texelFetch(depth_buffer, at, 0).r;
    if ((disp_cnt & 0x20) != 0 && edge_marked(at, attrs, depth)) coverage = 16;
    if (coverage >= 31) return;
    ivec2 out_of = ACROSS[code >> 6], beside = at + out_of, inward = at - out_of;
    // past the picture's border the clear plane is under the edge
    vec4 under = clear_color;
    if (within(beside)) {
        // Only a pixel that is farther than the polygon's own slope explains is under the edge. A nearer one hides
        // nothing of it. One at the surface's own depth is the neighbor polygon's, and what the console has under the
        // edge there is that polygon at this very pixel: the same surface, so the pixel stays as it is
        float slope = within(inward) ? abs(depth - texelFetch(depth_buffer, inward, 0).r) : 0.0;
        if (texelFetch(depth_buffer, beside, 0).r <= depth + 2.0 * slope + 64.0 / 16777216.0) return;
        under = texelFetch(picture, beside, 0);
    }
    if (coverage == 0) {
        o_color = under;
        return;
    }
    ivec3 top_rgb = ivec3(top.rgb * 63.0 + 0.5), under_rgb = ivec3(under.rgb * 63.0 + 0.5);
    int top_a = int(top.a * 31.0 + 0.5), under_a = int(under.a * 31.0 + 0.5);
    ++coverage;
    if (under_a > 0) top_rgb = (top_rgb * coverage + under_rgb * (32 - coverage)) >> 5;
    top_a = (top_a * coverage + under_a * (32 - coverage)) >> 5;
    o_color = vec4(vec3(top_rgb) / 63.0, float(top_a) / 31.0);
}
