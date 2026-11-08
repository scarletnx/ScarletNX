#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The renderer's layer over a present (hle_vulkan.c): one triangle that covers the target, with the layer's texture
// coordinates from 0 to 1 over the target
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 at = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1) * 2.0;
    v_uv = at;
    gl_Position = vec4(at * 2.0 - 1.0, 0.0, 1.0);
}
