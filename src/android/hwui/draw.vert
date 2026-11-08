#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The vertices of the Android renderer (render.c): positions in device pixels, premultiplied colors, and texture
// coordinates for bitmaps
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;
layout(location = 2) in vec2 uv;
layout(push_constant) uniform Push {
    vec2 scale; // 2 / the target's size
} push;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
void main() {
    v_color = color;
    v_uv = uv;
    gl_Position = vec4(position * push.scale - 1.0, 0.0, 1.0);
}
