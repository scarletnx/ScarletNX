/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shown crop of a frame to the pass's output extent, with the frame's flip. It is the pass that runs where the
   setting names no filter but the anti aliasing still needs an image (Super Sampling's first pass, or FXAA over a
   bilinear blit). */
#version 450

layout(set = 0, binding = 0) uniform sampler2D source_tex;

layout(push_constant) uniform Copy {
    vec4 rect; /* x, y, w, h: the output, in its pixels */
    vec4 crop; /* x, y, w, h: the shown crop, in the source's pixels */
    vec4 size; /* the source's width and height, then the flip of each axis */
} pc;

layout(location = 0) out vec4 o;

void main() {
    vec2 uv = (gl_FragCoord.xy - pc.rect.xy) / pc.rect.zw;
    if (pc.size.z > 0.5) uv.x = 1.0 - uv.x;
    if (pc.size.w > 0.5) uv.y = 1.0 - uv.y;
    vec2 src = (pc.crop.xy + uv * pc.crop.zw) / pc.size.xy;
    o = vec4(texture(source_tex, src).rgb, 1.0);
}
