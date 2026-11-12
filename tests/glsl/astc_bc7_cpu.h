/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* astc_bc7.comp's body (astc_decode.glsl, bc7_encode.glsl and astc_bc7.glsl) run on the CPU (astc_bc7_cpu.cpp), for
   the tests: a strip of depth slices of nx x ny ASTC blocks of bw x bh texels, w x h texels a slice, into
   ((w + 3) / 4) x ((h + 3) / 4) BC7 blocks a slice, in the encoder's build `build` (vk_bc7_build: 0 the full one, 1
   the fast one, 2 mode 6 alone, 3 the quick one), dispatched as vk/astc.c dispatches it */
#ifndef SNX_ASTC_BC7_CPU_H
#define SNX_ASTC_BC7_CPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void astc_bc7_cpu(uint32_t build, const uint8_t* blocks, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw,
                  uint32_t bh, int srgb, uint8_t* out);
#ifdef __cplusplus
}
#endif
#endif
