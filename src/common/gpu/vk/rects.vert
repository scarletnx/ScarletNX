/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A rectangle of a window's layer (common/utilities/ui_rects.h), an instance: its six corners made here from its place
   in the window's pixels, its edges on pixels' edges */
#version 450

layout(location = 0) in ivec2 at;
layout(location = 1) in uvec2 size;
layout(location = 2) in vec4 color;

layout(push_constant) uniform Window {
    vec2 size; /* in pixels */
} pc;

layout(location = 0) flat out vec4 c;

void main() {
    /* the two triangles' corners 0 1 2 and 0 2 3, clockwise from the top left: x 0 1 1 0, y 0 0 1 1 */
    uint k = (0x320210u >> (4u * uint(gl_VertexIndex))) & 3u;
    vec2 corner = vec2(float((6u >> k) & 1u), float(k >> 1));
    gl_Position = vec4((vec2(at) + corner * vec2(size)) / pc.size * 2.0 - 1.0, 0.0, 1.0);
    c = color;
}
