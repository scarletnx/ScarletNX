// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The toast's rectangles: the vertices are already in clip space (toast.c makes them from the target's pixels), and
// each carries the colour it fills with.
#version 450
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vcolor;
void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    vcolor = color;
}
