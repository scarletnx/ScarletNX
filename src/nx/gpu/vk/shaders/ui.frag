// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The atlas's texel (alpha premultiplied) times the quad's colour, blended over the picture
#version 450
layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(push_constant) uniform Picture {
    vec2 size;
    uint srgb;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o;
// sRGB bytes to linear light
vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
void main() {
    // unnormalized coordinates, in texels: a sample at level 0 named
    vec4 c = textureLod(atlas, v_uv, 0.0) * vec4(v_color.rgb, 1.0) * v_color.a;
    // an sRGB picture encodes what is written to it: the bytes go in decoded, and come out as they are
    if (pc.srgb != 0u && c.a > 0.0)
        c.rgb = decode(c.rgb / c.a) * c.a;
    o = c;
}
