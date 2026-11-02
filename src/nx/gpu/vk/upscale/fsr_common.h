/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The constants, sampler and flip of the FSR 1 shaders (fsr_easu.frag, fsr_rcas.frag). The filter is AMD's MIT
   ffx_fsr1.h, included by each stage after its callbacks. Crop and Flip are normalized: Crop is the shown part of the
   source (left, top, right, bottom), and Flip mirrors that part (vi's FlipH and FlipV). */
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_shader_explicit_arithmetic_types : require

layout(push_constant) uniform constants {
    uvec4 Const0;
    uvec4 Const1;
    uvec4 Const2;
    uvec4 Const3;
    vec4 Flip; /* x: FlipH, y: FlipV */
    vec4 Crop; /* left, top, right, bottom, normalized */
};

layout(set = 0, binding = 0) uniform sampler2D InputTexture;

#define A_GPU 1
#define A_GLSL 1
#define FSR_RCAS_PASSTHROUGH_ALPHA 1

#include "ffx_a.h"

vec2 FsrFlip(vec2 p) {
    if (Flip.x > 0.5) p.x = Crop.x + Crop.z - p.x;
    if (Flip.y > 0.5) p.y = Crop.y + Crop.w - p.y;
    return p;
}
