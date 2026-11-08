/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#version 450
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Parameters { vec4 crop; uvec4 flags; } parameters;
layout(location = 0) in vec2 coordinate;
layout(location = 0) out vec4 color;
void main() {
    vec2 p = coordinate;
    if ((parameters.flags.x & 4u) != 0u) p = vec2(p.y, 1.0 - p.x);
    if ((parameters.flags.x & 1u) != 0u) p.x = 1.0 - p.x;
    if ((parameters.flags.x & 2u) != 0u) p.y = 1.0 - p.y;
    color = texture(source, parameters.crop.xy + p * parameters.crop.zw);
}
