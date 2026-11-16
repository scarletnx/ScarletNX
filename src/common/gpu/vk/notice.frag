/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A notice's band (common/utilities/notice.h) over a window's picture, drawn in the window's rendering after it
   (notice.c's snx_vk_notice_draw): each pixel the band's, read from the buffer snx_notice_pixels wrote, R G B A in a
   word, rect.z words a row. The band is opaque and the attachment 8-bit UNORM, so its bytes land as they are */
#version 450

layout(set = 0, binding = 0, std430) readonly buffer Band {
    uint px[];
};

layout(push_constant) uniform Rect {
    ivec4 rect; /* the band's left, top, width and height in the window's pixels */
} pc;

layout(location = 0) out vec4 o;

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy) - pc.rect.xy;
    o = unpackUnorm4x8(px[at.y * pc.rect.z + at.x]);
}
