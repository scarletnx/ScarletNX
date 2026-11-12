/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ASTC 4x4 encoder's GLSL (astc_encode.glsl over bc7_encode.glsl's fitter) run on the CPU (astc_encode_cpu.cpp),
   for the tests: a w x h image of depth slices of RGBA8 texels (R first), rows after rows, into ((w + 3) / 4) x
   ((h + 3) / 4) blocks a slice, rows of blocks after rows, 16 bytes each. A block past the edge repeats its last
   texels */
#ifndef SNX_ASTC_ENCODE_CPU_H
#define SNX_ASTC_ENCODE_CPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void astc_encode_cpu(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t depth, uint8_t* out);
/* one block of 16 texels (RGBA8 words, R the low byte): its 16 bytes, and the squared error the encoder worked out
   for it, over its texels and channels */
uint32_t astc_encode_block_cpu(const uint32_t texels[16], uint8_t out[16]);
#ifdef __cplusplus
}
#endif
#endif
