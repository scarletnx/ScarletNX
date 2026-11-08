#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// GLES 1.1's texture environment for texture unit 0 (hle_gles.c), as the GLES 1.1 specification's table 3.15 gives
// it for each base format
layout(push_constant) uniform Draw {
    mat4 mvp;
    int mode;
    int base;
} draw;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 f = v_color;
    if (draw.mode == 0) {
        o_color = f;
        return;
    }
    vec4 t = texture(tex, v_uv);
    bool has_color = draw.base != 2;                 // not an alpha texture
    bool has_alpha = draw.base == 0 || draw.base == 2 || draw.base == 4; // RGBA, alpha, luminance and alpha
    vec3 c = f.rgb;
    float a = f.a;
    if (draw.mode == 2) { // replace
        if (has_color) c = t.rgb;
        if (has_alpha) a = t.a;
    } else if (draw.mode == 3) { // decal: RGB and RGBA only
        c = draw.base == 0 ? mix(f.rgb, t.rgb, t.a) : t.rgb;
    } else if (draw.mode == 4) { // add
        if (has_color) c = min(f.rgb + t.rgb, vec3(1.0));
        if (has_alpha) a = f.a * t.a;
    } else { // modulate
        if (has_color) c = f.rgb * t.rgb;
        if (has_alpha) a = f.a * t.a;
    }
    o_color = vec4(c, a);
}
