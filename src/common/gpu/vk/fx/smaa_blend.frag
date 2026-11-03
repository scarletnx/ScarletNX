/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SMAA 1x, the neighbourhood blending pass. The algorithm is Jorge Jimenez's and the SMAA contributors' (MIT); this
   Vulkan GLSL form follows the three pass port by the OpenQ4 contributors and the SMAA reference. */
#version 450

layout(set = 0, binding = 0) uniform sampler2D ColorTex;
layout(set = 0, binding = 1) uniform sampler2D BlendTex;

layout(push_constant) uniform Smaa {
    vec4 inv; /* 1/width, 1/height, 0, 0 */
} pc;

layout(location = 0) out vec4 o;

void main() {
    vec2 texcoord = gl_FragCoord.xy * pc.inv.xy;
    vec4 a;
    a.x = texture(BlendTex, texcoord + vec2(pc.inv.x, 0.0)).a;
    a.y = texture(BlendTex, texcoord + vec2(0.0, pc.inv.y)).g;
    a.wz = texture(BlendTex, texcoord).xz;
    if (dot(a, vec4(1.0)) < 0.00001) {
        o = vec4(texture(ColorTex, texcoord).rgb, 1.0);
        return;
    }
    bool horizontal = max(a.x, a.z) > max(a.y, a.w);
    vec4 blending_offset = vec4(0.0, a.y, 0.0, a.w);
    vec2 blending_weight = a.yw;
    if (horizontal) {
        blending_offset = vec4(a.x, 0.0, a.z, 0.0);
        blending_weight = a.xz;
    }
    blending_weight /= max(dot(blending_weight, vec2(1.0)), 0.00001);
    vec4 blending_coord = blending_offset * vec4(pc.inv.xy, -pc.inv.xy) + texcoord.xyxy;
    vec3 color = blending_weight.x * texture(ColorTex, blending_coord.xy).rgb;
    color += blending_weight.y * texture(ColorTex, blending_coord.zw).rgb;
    o = vec4(color, 1.0);
}
