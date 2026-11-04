// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The toast's rectangles, in the colour the vertex carries.
#version 450
layout(location = 0) in vec4 vcolor;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = vcolor;
}
