/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 encoder's GLSL (bc7_encode.glsl) run on the CPU (bc7_cpu.cpp), for the tests: a w x h image of depth
   slices of RGBA8 texels (R first), rows after rows, into ((w + 3) / 4) x ((h + 3) / 4) blocks a slice, rows of
   blocks after rows, 16 bytes each, as bc7.comp writes them */
#ifndef SNX_BC7_CPU_H
#define SNX_BC7_CPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void bc7_encode_cpu(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t depth, uint8_t* out);
/* the encoder's weight for a 4-bit index, which its errors and fits are worked out with */
uint32_t bc7_weight_cpu(uint32_t index);
#ifdef __cplusplus
}
#endif
#endif
