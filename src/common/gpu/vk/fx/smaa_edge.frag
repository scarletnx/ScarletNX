/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SMAA 1x, the edge detection pass. The algorithm is Jorge Jimenez's and the SMAA contributors' (MIT); this Vulkan
   GLSL form follows the three pass port by the OpenQ4 contributors and the SMAA reference. A pixel with no edge
   writes nothing, and the pass clears its image first. */
#version 450

layout(set = 0, binding = 0) uniform sampler2D ColorTex;

layout(push_constant) uniform Smaa {
    vec4 inv; /* 1/width, 1/height, 0, 0 */
} pc;

layout(location = 0) out vec4 o;

const float kThreshold = 0.1;
const float kLocalContrastAdaptationFactor = 2.0;
const vec3 kLumaWeights = vec3(0.2126, 0.7152, 0.0722);

vec2 uv_at(vec2 pixel) { return pixel * pc.inv.xy; }
float sample_luma(vec2 uv) { return dot(texture(ColorTex, uv).rgb, kLumaWeights); }

void main() {
    vec2 uv = gl_FragCoord.xy * pc.inv.xy;
    vec4 offset0 = pc.inv.xyxy * vec4(-1.0, 0.0, 0.0, -1.0) + uv.xyxy;
    vec4 offset1 = pc.inv.xyxy * vec4(1.0, 0.0, 0.0, 1.0) + uv.xyxy;
    vec4 offset2 = pc.inv.xyxy * vec4(-2.0, 0.0, 0.0, -2.0) + uv.xyxy;

    float luma = sample_luma(uv);
    float luma_left = sample_luma(offset0.xy);
    float luma_top = sample_luma(offset0.zw);

    vec4 delta;
    delta.xy = abs(luma - vec2(luma_left, luma_top));
    vec2 edges = step(vec2(kThreshold, kThreshold), delta.xy);
    if (dot(edges, vec2(1.0, 1.0)) == 0.0) discard;

    float luma_right = sample_luma(offset1.xy);
    float luma_bottom = sample_luma(offset1.zw);
    delta.zw = abs(luma - vec2(luma_right, luma_bottom));

    vec2 max_delta = max(delta.xy, delta.zw);
    float luma_left_left = sample_luma(offset2.xy);
    float luma_top_top = sample_luma(offset2.zw);
    delta.zw = abs(vec2(luma_left, luma_top) - vec2(luma_left_left, luma_top_top));

    max_delta = max(max_delta, delta.zw);
    float final_delta = max(max_delta.x, max_delta.y);
    edges *= step(vec2(final_delta, final_delta),
                 vec2(kLocalContrastAdaptationFactor, kLocalContrastAdaptationFactor) * delta.xy);
    o = vec4(edges, 0.0, 0.0);
}
