// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// A target's colour into its multisampled image: each pixel's colour to every sample of it
#version 450

layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) out vec4 color;

void main() {
    color = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
}
