// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// A target's stencil into its multisampled image, one bit a draw: the pipeline replaces the stencil with all ones under
// the write mask of that bit, and a pixel whose bit is clear is left out
#version 450

layout(set = 0, binding = 0) uniform usampler2D source;
layout(push_constant) uniform Push { uint bit; } push;

void main() {
    if (((texelFetch(source, ivec2(gl_FragCoord.xy), 0).r >> push.bit) & 1u) == 0u) discard;
}
