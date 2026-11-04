/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The presented frame's shown crop into the HDR composition: linear light in nits, BT.2020's primaries, 0 black, so
   the encode pass (hdr_encode.frag) or the UI's own pass writes it. The source is decoded as its transfer says: sRGB
   for an SDR frame, with only the top of its range lifted into the display's headroom; linear light at the paper white for a
   frame that already holds HDR data; PQ for a frame that is an HDR10 signal. A flip is the frame's Android transform.

   The numbers here are hdr_select.c's (snx_hdr_sdr_nits, snx_hdr_pq_decode, snx_hdr_bt709_to_bt2020), and the tests
   hold the two to each other. */
#version 450

layout(set = 0, binding = 0) uniform sampler2D source_tex;

layout(push_constant) uniform Compose {
    vec4 rect; /* x, y, w, h: where the shown crop lands in the composition, in its pixels */
    vec4 crop; /* x, y, w, h: the crop in the source's pixels */
    vec4 size;   /* the source image's width and height, then the flip of each axis */
    vec4 tone;   /* paper white in nits, the light white may reach as a multiple of it, the peak in nits, the source
                    (1 SDR, 2 linear, 3 PQ) */
    vec4 expand; /* the lift's share of the headroom 0 to 1, 0, 0, 0 */
} pc;

layout(location = 0) out vec4 o;

vec3 linear_to_srgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
}

vec3 srgb_to_linear(vec3 c) {
    vec3 lo = c / 12.92;
    vec3 hi = pow(max((c + 0.055) / 1.055, vec3(0.0)), vec3(2.4));
    return mix(hi, lo, lessThanEqual(c, vec3(0.04045)));
}

// SMPTE ST 2084's EOTF, the 10000 nits of HDR10
vec3 pq_decode(vec3 c) {
    const vec3 m1 = vec3(2610.0 / 16384.0), m2 = vec3(2523.0 / 32.0);
    const vec3 c1 = vec3(3424.0 / 4096.0), c2 = vec3(2413.0 / 128.0), c3 = vec3(2392.0 / 128.0);
    vec3 p = pow(max(c, vec3(0.0)), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, vec3(0.0)) / (c2 - c3 * p), 1.0 / m1);
}

// BT.709's primaries to BT.2020's (ITU-R BT.2087), for linear light
vec3 bt709_to_bt2020(vec3 c) {
    return vec3(dot(c, vec3(0.6274, 0.3293, 0.0433)), dot(c, vec3(0.0691, 0.9195, 0.0114)),
                dot(c, vec3(0.0164, 0.0880, 0.8956)));
}

void main() {
    vec2 uv = (gl_FragCoord.xy - pc.rect.xy) / pc.rect.zw; /* 0 to 1 across the crop */
    vec2 src;
    vec4 texel;
    vec3 nits;
    int source = int(pc.tone.w + 0.5);
    if (pc.size.z > 0.5) uv.x = 1.0 - uv.x;
    if (pc.size.w > 0.5) uv.y = 1.0 - uv.y;
    src = (pc.crop.xy + uv * pc.crop.zw) / pc.size.xy;
    texel = texture(source_tex, src);
    if (source == 3) { /* PQ, BT.2020 already */
        nits = pq_decode(texel.rgb);
    } else if (source == 2) { /* linear light, the paper white at 1.0, above it the frame's own headroom */
        nits = bt709_to_bt2020(max(texel.rgb, vec3(0.0)) * pc.tone.x);
    } else {
        /* sRGB, the console's display, mapped into HDR. Everything but the top of the range stays exactly as SDR, as
           BT.2446, Dolby's inverse tone mapping and Horizon's HDR keep it: the frame decodes at the paper white, and
           only its brightest part is lifted, towards the light the display has over that white. The colour's ratios
           never move, so its hue and saturation stay; the wider gamut of the target is the container, not a stretch
           of the colours. The knee starts at 80 % of the encoded range, so only the brightest part of the frame moves,
           as Horizon's HDR lifts only the world's light sources. */
        vec3 lin = srgb_to_linear(clamp(texel.rgb, vec3(0.0), vec3(1.0)));
        {
            float l = dot(lin, vec3(0.2126, 0.7152, 0.0722));
            float le = l > 0.0 ? linear_to_srgb(vec3(l)).r : 0.0;
            float k = clamp((le - 0.8) / 0.2, 0.0, 1.0);
            float knee = k * k * (3.0 - 2.0 * k);
            float lift = mix(1.0, pc.tone.y, knee);
            nits = bt709_to_bt2020(pc.tone.x * lin * lift);
        }
    }
    o = vec4(clamp(nits, vec3(0.0), vec3(pc.tone.z)), 1.0);
}
