// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// One triangle over the whole target: the draws that fill a multisampled target from the single-sample image beside it
#version 450

void main() {
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
