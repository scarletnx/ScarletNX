/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SMAA 1x, the blending weight calculation. The algorithm is Jorge Jimenez's and the SMAA contributors' (MIT); this
   Vulkan GLSL form follows the three pass port by the OpenQ4 contributors and the SMAA reference, with the
   distribution's 160x560 RG8 AreaTex and 64x16 R8 SearchTex. */
#version 450

layout(set = 0, binding = 0) uniform sampler2D EdgesTex;
layout(set = 0, binding = 1) uniform sampler2D AreaTex;
layout(set = 0, binding = 2) uniform sampler2D SearchTex;

layout(push_constant) uniform Smaa {
    vec4 inv; /* 1/width, 1/height, 0, 0 */
} pc;

layout(location = 0) out vec4 o;

const float kMaxSearchSteps = 8.0;
const float kAreaMaxDistance = 16.0;
const vec2 kAreaTexPixelSize = vec2(1.0 / 160.0, 1.0 / 560.0);
const float kAreaTexSubtexSize = 1.0 / 7.0;
const vec2 kSearchTexSize = vec2(66.0, 33.0);
const vec2 kSearchTexPackedSize = vec2(64.0, 16.0);

vec2 round_vec2(vec2 value) { return floor(value + vec2(0.5, 0.5)); }

float sample_search_length(vec2 e, float offset_value) {
    vec2 scale = kSearchTexSize * vec2(0.5, -1.0);
    vec2 bias = kSearchTexSize * vec2(offset_value, 1.0);
    scale += vec2(-1.0, 1.0);
    bias += vec2(0.5, -0.5);
    scale /= kSearchTexPackedSize;
    bias /= kSearchTexPackedSize;
    return texture(SearchTex, scale * e + bias).r;
}

float search_x_left(vec2 texcoord, float end_value) {
    vec2 e = vec2(0.0, 1.0);
    while (texcoord.x > end_value && e.g > 0.8281 && e.r == 0.0) {
        e = texture(EdgesTex, texcoord).rg;
        texcoord -= vec2(2.0 * pc.inv.x, 0.0);
    }
    float offset_value = -(255.0 / 127.0) * sample_search_length(e, 0.0) + 3.25;
    return pc.inv.x * offset_value + texcoord.x;
}

float search_x_right(vec2 texcoord, float end_value) {
    vec2 e = vec2(0.0, 1.0);
    while (texcoord.x < end_value && e.g > 0.8281 && e.r == 0.0) {
        e = texture(EdgesTex, texcoord).rg;
        texcoord += vec2(2.0 * pc.inv.x, 0.0);
    }
    float offset_value = -(255.0 / 127.0) * sample_search_length(e, 0.5) + 3.25;
    return -pc.inv.x * offset_value + texcoord.x;
}

float search_y_up(vec2 texcoord, float end_value) {
    vec2 e = vec2(1.0, 0.0);
    while (texcoord.y > end_value && e.r > 0.8281 && e.g == 0.0) {
        e = texture(EdgesTex, texcoord).rg;
        texcoord -= vec2(0.0, 2.0 * pc.inv.y);
    }
    float offset_value = -(255.0 / 127.0) * sample_search_length(e.gr, 0.0) + 3.25;
    return pc.inv.y * offset_value + texcoord.y;
}

float search_y_down(vec2 texcoord, float end_value) {
    vec2 e = vec2(1.0, 0.0);
    while (texcoord.y < end_value && e.r > 0.8281 && e.g == 0.0) {
        e = texture(EdgesTex, texcoord).rg;
        texcoord += vec2(0.0, 2.0 * pc.inv.y);
    }
    float offset_value = -(255.0 / 127.0) * sample_search_length(e.gr, 0.5) + 3.25;
    return -pc.inv.y * offset_value + texcoord.y;
}

vec2 sample_area(vec2 dist, float e1, float e2, float offset_value) {
    vec2 texcoord = vec2(kAreaMaxDistance, kAreaMaxDistance) * round_vec2(4.0 * vec2(e1, e2)) + dist;
    texcoord = kAreaTexPixelSize * texcoord + 0.5 * kAreaTexPixelSize;
    texcoord.y = kAreaTexSubtexSize * offset_value + texcoord.y;
    return texture(AreaTex, texcoord).rg;
}

void main() {
    vec2 texcoord = gl_FragCoord.xy * pc.inv.xy;
    vec2 rt_size = 1.0 / max(pc.inv.xy, vec2(0.000001));
    vec2 pixcoord = texcoord * rt_size;

    vec4 offset0 = pc.inv.xyxy * vec4(-0.25, -0.125, 1.25, -0.125) + texcoord.xyxy;
    vec4 offset1 = pc.inv.xyxy * vec4(-0.125, -0.25, -0.125, 1.25) + texcoord.xyxy;
    vec4 offset2 = vec4(pc.inv.x, pc.inv.x, pc.inv.y, pc.inv.y) * (vec4(-2.0, 2.0, -2.0, 2.0) * kMaxSearchSteps) +
                   vec4(offset0.x, offset0.z, offset1.y, offset1.w);

    vec4 weights = vec4(0.0);
    vec2 e = texture(EdgesTex, texcoord).rg;

    if (e.g > 0.0) {
        vec3 coords;
        coords.x = search_x_left(offset0.xy, offset2.x);
        coords.y = offset1.y;
        float left_edge = texture(EdgesTex, coords.xy).r;
        coords.z = search_x_right(offset0.zw, offset2.y);
        vec2 d = abs(round_vec2(rt_size.xx * vec2(coords.x, coords.z) - pixcoord.xx));
        vec2 sqrt_d = sqrt(d);
        float right_edge = texture(EdgesTex, coords.zy + vec2(pc.inv.x, 0.0)).r;
        weights.rg = sample_area(sqrt_d, left_edge, right_edge, 0.0);
    }

    if (e.r > 0.0) {
        vec3 coords;
        coords.y = search_y_up(offset1.xy, offset2.z);
        coords.x = offset0.x;
        float top_edge = texture(EdgesTex, coords.xy).g;
        coords.z = search_y_down(offset1.zw, offset2.w);
        vec2 d = abs(round_vec2(rt_size.yy * vec2(coords.y, coords.z) - pixcoord.yy));
        vec2 sqrt_d = sqrt(d);
        float bottom_edge = texture(EdgesTex, coords.xz + vec2(0.0, pc.inv.y)).g;
        weights.ba = sample_area(sqrt_d, top_edge, bottom_edge, 0.0);
    }

    o = weights;
}
