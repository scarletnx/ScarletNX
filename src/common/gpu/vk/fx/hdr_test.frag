/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The HDR test pattern (hdr_test=1): a band at the bottom of the composition with six patches of known light, 0, the
   paper white, and 1.25, 1.5, 1.75 and 2 times it, clamped to the peak. A patch above the paper white shows only where
   the display's headroom is real: the steps between them are the proof that the lift reaches the screen, and where the
   last ones look the same, the display (or its layer) is clipping there. Drawn after the frame, over the band. */
#version 450

layout(push_constant) uniform Pattern {
    vec4 v; /* paper white in nits, the peak in nits, the target's width in pixels, the band's height in pixels */
} pc;

layout(location = 0) out vec4 o;

void main() {
    float slot = floor(gl_FragCoord.x / (pc.v.z / 6.0));
    float step = slot < 1.0 ? 0.0
                 : slot < 2.0 ? 1.0
                 : slot < 3.0 ? 1.25
                 : slot < 4.0 ? 1.5
                 : slot < 5.0 ? 1.75
                               : 2.0;
    o = vec4(vec3(min(pc.v.x * step, pc.v.y)), 1.0);
}
