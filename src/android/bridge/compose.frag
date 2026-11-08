#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The renderer's layer (premultiplied) as it is: the pipeline blends it over the present with ONE, ONE_MINUS_SRC_ALPHA
layout(set = 0, binding = 0) uniform sampler2D layer;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;
void main() {
    color = texture(layer, v_uv);
}
