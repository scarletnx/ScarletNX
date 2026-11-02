#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// A bitmap's texels (premultiplied). TINT: the color where the bitmap is, as a color filter of SrcIn tints an icon.
// Otherwise the texels times the color, which is the paint's alpha, or a filter's color for Modulate
layout(constant_id = 0) const int TINT = 0;
layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 color;
void main() {
    vec4 t = texture(image, v_uv);
    color = TINT != 0 ? v_color * t.a : t * v_color;
}
