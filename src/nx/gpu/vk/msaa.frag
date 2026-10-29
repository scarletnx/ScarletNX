#version 450
/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A multisampled image's samples moved to and from the guest's layout of them (vk/msaa.c). The guest lays a pixel's
   samples out as a block of X by Y texels of a surface X times wider and Y times higher than the image, sample i at
   (i % X, i / X) in the block (gpu/draw_state.h DS_MSAA_, as Ryujinx's ColorCopyToNonMs shaders take them), X and Y
   powers of two given by their log2 in pc.lg. Color moves through views in the unsigned format of the texels' bytes
   (vk_raw_format), so every bit goes as it is. One source, built with one of:
     MS_UNPACK        the image's samples (src) to that surface's texels: the render target is the surface
     MS_UNPACK_DEPTH  the same for a depth image, into a depth target of its format
     MS_PACK          that surface's texels (src) to the image's samples: the render target is the image, shaded per
                      sample (gl_SampleID), each invocation writing its own sample
   CMake compiles each into SPIR-V at build time (cmake/glsl.cmake). */
layout(push_constant) uniform Params {
    ivec2 lg; /* log2 of X and Y */
} pc;

#if defined(MS_UNPACK) || defined(MS_UNPACK_DEPTH)
#ifdef MS_UNPACK
layout(set = 0, binding = 0) uniform usampler2DMS src;
layout(location = 0) out uvec4 o;
#else
layout(set = 0, binding = 0) uniform sampler2DMS src;
#endif
void main() {
    ivec2 t = ivec2(gl_FragCoord.xy); /* the texel, at its center: its integer part */
    ivec2 m = (ivec2(1) << pc.lg) - 1;
    int s = (t.x & m.x) | ((t.y & m.y) << pc.lg.x);
#ifdef MS_UNPACK
    o = texelFetch(src, t >> pc.lg, s);
#else
    gl_FragDepth = texelFetch(src, t >> pc.lg, s).r;
#endif
}
#elif defined(MS_PACK)
layout(set = 0, binding = 0) uniform usampler2D src;
layout(location = 0) out uvec4 o;
void main() {
    /* per sample, gl_FragCoord is the sample's position in the pixel: its integer part is the pixel */
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 at = ivec2(gl_SampleID & ((1 << pc.lg.x) - 1), gl_SampleID >> pc.lg.x);
    o = texelFetch(src, (p << pc.lg) + at, 0);
}
#else
#error "msaa.frag: build it with MS_UNPACK, MS_UNPACK_DEPTH or MS_PACK"
#endif
