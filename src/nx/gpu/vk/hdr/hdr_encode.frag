/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The HDR composition to the target image: PQ (SMPTE ST 2084, BT.2020) for an HDR10 swapchain, or the SDR range with a
   knee for an SDR one, so HDR content is tone mapped where the display has no HDR. The composition is linear light in
   nits and BT.2020's primaries (hdr_compose.frag). The numbers are hdr_select.c's (snx_hdr_pq_encode,
   snx_hdr_tonemap, snx_hdr_bt2020_to_bt709, snx_hdr_linear_to_srgb) and the tests hold the two to each other. */
#version 450

layout(set = 0, binding = 0) uniform sampler2D composed;

layout(push_constant) uniform Encode {
    vec4 tone; /* paper white in nits, the peak in nits, 0 for the SDR range, 1 PQ, 2 scRGB, 3 Display P3, and 1 when
                  the target itself encodes sRGB */
} pc;

layout(location = 0) out vec4 o;

vec3 pq_encode(vec3 nits) {
    const vec3 m1 = vec3(2610.0 / 16384.0), m2 = vec3(2523.0 / 32.0);
    const vec3 c1 = vec3(3424.0 / 4096.0), c2 = vec3(2413.0 / 128.0), c3 = vec3(2392.0 / 128.0);
    vec3 y = pow(clamp(nits / 10000.0, vec3(0.0), vec3(1.0)), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

// BT.2020's primaries to BT.709's, for linear light
vec3 bt2020_to_bt709(vec3 c) {
    return vec3(dot(c, vec3(1.6605, -0.5876, -0.0728)), dot(c, vec3(-0.1246, 1.1329, -0.0083)),
                dot(c, vec3(-0.0182, -0.1006, 1.1187)));
}
// BT.2020's primaries to Display P3's, for linear light
vec3 bt2020_to_p3(vec3 c) {
    return vec3(dot(c, vec3(1.343578, -0.282180, -0.061399)), dot(c, vec3(-0.065297, 1.075788, -0.010490)),
                dot(c, vec3(0.002822, -0.019598, 1.016777)));
}

vec3 linear_to_srgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
}

// the SDR range up to the paper white takes 90% of the output, whole and even; the frame's own headroom above it takes
// the last 10%, so a highlight is lighter than white and monotonic, and the peak lands at 1
float tonemap(float nits) {
    const float sp = 0.9;
    float tp = pc.tone.x / pc.tone.y;
    float t = nits / pc.tone.y;
    if (!(nits > 0.0)) return 0.0;
    if (tp > 1.0) tp = 1.0;
    if (t > 1.0) t = 1.0;
    if (t <= tp) return tp > 0.0 ? t * sp / tp : 0.0;
    if (tp >= 1.0) return sp;
    return sp + (1.0 - sp) * (t - tp) / (1.0 - tp);
}


void main() {
    vec3 nits = texture(composed, gl_FragCoord.xy / vec2(textureSize(composed, 0))).rgb;
    if (pc.tone.z > 0.5 && pc.tone.z < 1.5) {
        o = vec4(pq_encode(max(nits, vec3(0.0))), 1.0);
    } else if (pc.tone.z > 2.5) {
        /* Display P3: the same extended linear light in P3's wider primaries */
        vec3 lin = bt2020_to_p3(max(nits, vec3(0.0))) / pc.tone.x;
        o = vec4(clamp(lin, vec3(0.0), vec3(64.0)), 1.0);
    } else if (pc.tone.z > 1.5) {
        /* scRGB: linear light in sRGB's primaries, 1.0 at the paper white, so the SDR range is exactly what the
           display shows in SDR and the frame's own headroom is above 1 */
        vec3 lin = bt2020_to_bt709(max(nits, vec3(0.0))) / pc.tone.x;
        o = vec4(clamp(lin, vec3(0.0), vec3(64.0)), 1.0);
    } else {
        // the knee on the luminance, so a highlight keeps its hue
        float y = max(dot(nits, vec3(0.2627, 0.6780, 0.0593)), 1e-6);
        vec3 lin = bt2020_to_bt709(nits * (tonemap(y) / y));
        lin = clamp(lin, vec3(0.0), vec3(1.0));
        o = vec4(pc.tone.w > 0.5 ? lin : linear_to_srgb(lin), 1.0);
    }
}
