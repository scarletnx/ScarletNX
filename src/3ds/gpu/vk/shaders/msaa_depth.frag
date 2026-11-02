// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// A target's depth into its multisampled image: each pixel's depth to every sample of it
#version 450

layout(set = 0, binding = 0) uniform sampler2D source;

void main() {
    gl_FragDepth = texelFetch(source, ivec2(gl_FragCoord.xy), 0).r;
}
