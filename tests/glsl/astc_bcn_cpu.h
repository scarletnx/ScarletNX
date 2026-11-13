/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* astc_bcn.comp's body (astc_decode.glsl, bcn_encode.glsl and astc_bcn.glsl) run on the CPU (astc_bcn_cpu.cpp), for
   the tests: a strip of depth slices of nx x ny ASTC blocks of bw x bh texels, w x h texels a slice, into
   ((w + 3) / 4) x ((h + 3) / 4) blocks a slice of BC1, BC4 or BC5 (kind 1, 4 or 5; 8, 8 and 16 bytes each), as
   vk/astc.c dispatches it: what a GPU's blocks are held to */
#ifndef SNX_ASTC_BCN_CPU_H
#define SNX_ASTC_BCN_CPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void astc_bcn_cpu(uint32_t kind, const uint8_t* blocks, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw,
                  uint32_t bh, int srgb, uint8_t* out);
#ifdef __cplusplus
}
#endif
#endif
