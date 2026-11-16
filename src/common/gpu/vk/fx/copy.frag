/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shown crop of a frame to the pass's output rectangle, with the frame's flip: where the setting names no filter
   but the anti-aliasing still needs an image (Super Sampling's first pass, or FXAA over a bilinear scale), Super
   Sampling's downsample, and a window's frame with neither on. */
#version 450

layout(set = 0, binding = 0) uniform sampler2D source_tex;

layout(push_constant) uniform Copy {
    vec4 rect; /* x, y, w, h: the output, in its pixels */
    vec4 crop; /* x, y, w, h: the shown crop, in the source's pixels */
    vec4 size; /* the source's width and height, then the flip of each axis */
    vec4 mode; /* x: 1 for sharp pixels, each the texel under its centre in whole numbers, so a tie (a centre on a
                  texel's edge at a scale like 1.5x) is the right or lower texel on every device, where the sampler's
                  rounding takes either */
} pc;

layout(location = 0) out vec4 o;

void main() {
    if (pc.mode.x > 0.5) {
        ivec2 out_size = ivec2(pc.rect.zw), d = ivec2(gl_FragCoord.xy - pc.rect.xy);
        if (pc.size.z > 0.5) d.x = out_size.x - 1 - d.x;
        if (pc.size.w > 0.5) d.y = out_size.y - 1 - d.y;
        ivec2 texel = ivec2(pc.crop.xy) + (2 * d + 1) * ivec2(pc.crop.zw) / (2 * out_size);
        o = vec4(texelFetch(source_tex, texel, 0).rgb, 1.0);
        return;
    }
    vec2 uv = (gl_FragCoord.xy - pc.rect.xy) / pc.rect.zw;
    if (pc.size.z > 0.5) uv.x = 1.0 - uv.x;
    if (pc.size.w > 0.5) uv.y = 1.0 - uv.y;
    vec2 src = (pc.crop.xy + uv * pc.crop.zw) / pc.size.xy;
    o = vec4(texture(source_tex, src).rgb, 1.0);
}
