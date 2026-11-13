// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The 3D engine's registers as the frame latched them (render3d.c fills them): the toon and edge tables, the fog
layout(std140, set = 0, binding = 0) uniform Config {
    vec4 toon_colors[32];
    vec4 edge_colors[8];
    vec4 fog_color;
    vec4 fog_density[9]; // 34 values, four to a vector
    int disp_cnt;
    int fog_offset;
    int fog_shift;
    int alpha_ref;
};
