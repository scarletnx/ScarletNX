/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* FSR 1 EASU: the input's shown crop to the output extent. AMD's ffx_fsr1.h does the filter, MIT (its notice is in
   ffx_fsr1.h and ffx_a.h). Built twice: fp32, and fp16 where the device's shaderFloat16 allows it. */
#version 460 core
#extension GL_GOOGLE_include_directive : require
#ifdef YUZU_USE_FP16
#define A_HALF 1
#endif
#include "fsr_common.h"

#ifdef YUZU_USE_FP16
#define FSR_EASU_H 1
AH4 FsrEasuRH(AF2 p) { return AH4(textureGather(InputTexture, FsrFlip(p), 0)); }
AH4 FsrEasuGH(AF2 p) { return AH4(textureGather(InputTexture, FsrFlip(p), 1)); }
AH4 FsrEasuBH(AF2 p) { return AH4(textureGather(InputTexture, FsrFlip(p), 2)); }
#else
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(InputTexture, FsrFlip(p), 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(InputTexture, FsrFlip(p), 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(InputTexture, FsrFlip(p), 2); }
#endif

#include "ffx_fsr1.h"

layout(location = 0) out vec4 FragColor;

void main() {
    AU2 ip = AU2(gl_FragCoord.xy);
    AF2 pp = AF2(ip) * AF2_AU2(Const0.xy) + AF2_AU2(Const0.zw);
    vec2 uv = FsrFlip(pp / vec2(textureSize(InputTexture, 0)));
    float alpha = texture(InputTexture, uv).a;
#ifdef YUZU_USE_FP16
    AH3 c;
    FsrEasuH(c, ip, Const0, Const1, Const2, Const3);
    FragColor = vec4(vec3(c), alpha);
#else
    AF3 c;
    FsrEasuF(c, ip, Const0, Const1, Const2, Const3);
    FragColor = AF4(c, alpha);
#endif
}
