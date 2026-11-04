// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The atlas's texel (alpha premultiplied) times the quad's colour, blended over the picture
#version 450
layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(push_constant) uniform Picture {
    vec2 size;
    uint mode; // 0: the target holds the encoded bytes, 1: it is sRGB, 2: it is linear light in nits (HDR)
    float gain; // the paper white in nits, for mode 2
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o;
// sRGB bytes to linear light
vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
void main() {
    vec4 c = texture(atlas, v_uv) * vec4(v_color.rgb, 1.0) * v_color.a;
    // an sRGB target, or the HDR composition, blends in linear light: the colour goes in decoded, and the sRGB
    // target encodes it once (as before), or the composition keeps it (in nits, BT.2020's primaries)
    if (pc.mode != 0u && c.a > 0.0) {
        vec3 lin = decode(c.rgb / c.a);
        // BT.709's primaries to BT.2020's, for linear light: the rows of the matrix, which GLSL's mat3 takes by
        // columns (hdr_compose.frag's dot form, written the same way)
        if (pc.mode == 2u)
            lin = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956) * lin * pc.gain;
        c.rgb = lin * c.a;
    }
    o = c;
}
