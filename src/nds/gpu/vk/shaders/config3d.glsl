// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The 3D engine's registers as the frame latched them (render3d.c fills them): the toon and edge tables, the fog, the clear plane
layout(std140, set = 0, binding = 0) uniform Config {
    vec4 toon_colors[32];
    vec4 edge_colors[8];
    vec4 fog_color;
    vec4 fog_density[9]; // 34 values, four to a vector
    int disp_cnt;
    int fog_offset;
    int fog_shift;
    int alpha_ref;
    int clear_attr1; // CLEAR_COLOR: the plane's polygon ID in bits 24 to 29
    int clear_attr2; // CLEAR_DEPTH: the clear bitmap's offset in bits 16 to 31
};
