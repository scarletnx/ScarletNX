#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// A clear that a mask limits, drawn (gles2_draw.c): the clear color on each color attachment, which the pipeline's
// write masks limit to the channels and the attachments cleared
layout(push_constant) uniform Clear {
    vec4 color;
} clear;
layout(location = 0) out vec4 color0;
layout(location = 1) out vec4 color1;
layout(location = 2) out vec4 color2;
layout(location = 3) out vec4 color3;
void main() {
    color0 = clear.color;
    color1 = clear.color;
    color2 = clear.color;
    color3 = clear.color;
}
