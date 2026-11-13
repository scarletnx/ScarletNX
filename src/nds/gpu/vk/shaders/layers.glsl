// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2016-2026 melonDS team
// Copyright 2026 the ScarletNX contributors
// A drawn layer's attributes and depth, for the passes over the picture: red the opaque polygon ID in 63rds, green
// the edge flag, blue the fog bit, alpha an edge's coverage (poly.frag). The edge marking test is melonDS's software
// renderer's (GPU3D_Soft.cpp, GPL-3.0-or-later).
layout(set = 0, binding = 2) uniform sampler2D attribute_buffer;
layout(set = 0, binding = 3) uniform sampler2D depth_buffer;

bool differs(ivec2 at, int id, float depth) {
    // past the picture's border lies the clear plane: its polygon ID, at its depth
    int other = clear_id;
    float beyond = clear_depth;
    if (at.x >= 0 && at.y >= 0 && at.x <= 255 && at.y <= 191) {
        other = int(texelFetch(attribute_buffer, at, 0).r * 63.0 + 0.5);
        beyond = texelFetch(depth_buffer, at, 0).r;
    }
    return other != id && depth < beyond;
}

// an edge pixel of an opaque polygon whose neighbor is of another polygon ID and farther: it takes the edge color
bool edge_marked(ivec2 at, vec4 attrs, float depth) {
    if (attrs.g == 0.0) return false;
    int id = int(attrs.r * 63.0 + 0.5);
    return differs(at + ivec2(0, -1), id, depth) || differs(at + ivec2(0, 1), id, depth) ||
           differs(at + ivec2(-1, 0), id, depth) || differs(at + ivec2(1, 0), id, depth);
}
