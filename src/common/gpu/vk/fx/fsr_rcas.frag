/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* FSR 1 RCAS: the EASU output sharpened. AMD's ffx_fsr1.h does the filter, MIT (its notice is in ffx_fsr1.h and
   ffx_a.h). Built twice: fp32, and fp16 where the device's shaderFloat16 allows it. */
#version 460 core
#extension GL_GOOGLE_include_directive : require
#ifdef YUZU_USE_FP16
#define A_HALF 1
#endif
#include "fsr_common.h"

#ifdef YUZU_USE_FP16
#define FSR_RCAS_H 1
AH4 FsrRcasLoadH(ASW2 p) {
    ASU2 ip = ASU2(p);
    ASU2 size = ASU2(textureSize(InputTexture, 0));
    if (Flip.x > 0.5) ip.x = size.x - 1 - ip.x;
    if (Flip.y > 0.5) ip.y = size.y - 1 - ip.y;
    return AH4(texelFetch(InputTexture, ip, 0));
}
void FsrRcasInputH(inout AH1 r, inout AH1 g, inout AH1 b) {}
#else
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) {
    ASU2 size = ASU2(textureSize(InputTexture, 0));
    if (Flip.x > 0.5) p.x = size.x - 1 - p.x;
    if (Flip.y > 0.5) p.y = size.y - 1 - p.y;
    return texelFetch(InputTexture, p, 0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#endif

#include "ffx_fsr1.h"

layout(location = 0) out vec4 FragColor;

void main() {
    AU2 ip = AU2(gl_FragCoord.xy);
#ifdef YUZU_USE_FP16
    AH4 c;
    FsrRcasH(c.r, c.g, c.b, c.a, ip, Const0);
    FragColor = vec4(c);
#else
    AF4 c;
    FsrRcasF(c.r, c.g, c.b, c.a, ip, Const0);
    FragColor = c;
#endif
}
