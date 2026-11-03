/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* FXAA 3.11's console quality preset, after Timothy Lottes' public domain FXAA. It reads the upscaled image at the
   window's size, finds the pixels whose luma step is visible, and blends along the edge; a frame below the luma
   threshold is passed through exactly. */
#version 450

layout(set = 0, binding = 0) uniform sampler2D source_tex;

layout(push_constant) uniform Fxaa {
    vec4 rcp; /* 1/width, 1/height, the sharpness, 0 */
} pc;

layout(location = 0) out vec4 o;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec2 uv = gl_FragCoord.xy * pc.rcp.xy;
    vec2 rcp = pc.rcp.xy;
    vec3 rgbM = texture(source_tex, uv).rgb;
    float lumaM = luma(rgbM);
    float lumaNW = luma(texture(source_tex, uv + vec2(-1.0, -1.0) * rcp).rgb);
    float lumaNE = luma(texture(source_tex, uv + vec2(1.0, -1.0) * rcp).rgb);
    float lumaSW = luma(texture(source_tex, uv + vec2(-1.0, 1.0) * rcp).rgb);
    float lumaSE = luma(texture(source_tex, uv + vec2(1.0, 1.0) * rcp).rgb);
    float lumaN = luma(texture(source_tex, uv + vec2(0.0, -1.0) * rcp).rgb);
    float lumaS = luma(texture(source_tex, uv + vec2(0.0, 1.0) * rcp).rgb);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(min(lumaSW, lumaSE), min(lumaN, lumaS))));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(max(lumaSW, lumaSE), max(lumaN, lumaS))));
    if (lumaMax - lumaMin < max(0.0312, lumaMax * 0.125)) {
        o = vec4(rgbM, 1.0);
        return;
    }
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.25), 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * rcp;
    vec3 rgbA = 0.5 * (texture(source_tex, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                       texture(source_tex, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(source_tex, uv - dir * 0.5).rgb +
                                     texture(source_tex, uv + dir * 0.5).rgb);
    float lumaB = luma(rgbB);
    o = vec4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0);
}
