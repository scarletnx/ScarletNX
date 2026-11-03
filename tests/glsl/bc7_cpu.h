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
/* the same encoder without modes 4 and 5, what they are measured against */
void bc7_encode_cpu_without_45(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t depth, uint8_t* out);
/* the encoder's three subset partition table, two bits a texel */
uint32_t bc7_partition3_cpu(uint32_t partition);
/* the encoder's weight for an index of bits bits (2, 3 or 4), which its errors and fits are worked out with */
uint32_t bc7_weight_cpu(uint32_t bits, uint32_t index);
/* the encoder's least squares ends (RGBA8 words) for subset subset of partition partition of a block of subsets
   subsets (1, 2 or 3), texel k at the weight of index[k] of bits bits: what its fit refines a line to. 0 and the ends
   left as they are when every texel of the subset took one weight, which has no fit; else 1 */
int bc7_least_squares_cpu(const uint32_t texels[16], const uint8_t index[16], uint32_t bits, uint32_t subsets,
                          uint32_t partition, uint32_t subset, uint32_t ends[2]);
#ifdef __cplusplus
}
#endif
#endif
