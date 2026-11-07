#version 300 es
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The window's coordinates: gl_FragCoord and gl_PointCoord, which a backend can read through GlslcOptions.flip_y
precision highp float;
uniform float height;
out vec4 color;
void main() {
    float row = gl_FragCoord.y / height;
    color = vec4(gl_PointCoord, row, gl_FragCoord.x > 0.5 ? 1.0 : 0.0);
}
