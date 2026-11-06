/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#version 450
layout(location = 0) out vec2 coordinate;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    coordinate = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
