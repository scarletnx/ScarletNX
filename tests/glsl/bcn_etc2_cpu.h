/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BCn to ETC2 and EAC transcoder (texconv.glsl built with SNX_TC_ETC2, its etc2_encode.glsl) run on the CPU
   (bcn_etc2_cpu.cpp), for the tests */
#ifndef SNX_BCN_ETC2_CPU_H
#define SNX_BCN_ETC2_CPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* bw x bh blocks of slices slices of BCn kind (tests/support/bcn.h's BCN_*, BC6H excepted), rows of blocks after
   rows, into as many ETC2 or EAC blocks of the same size as vk/texconv.c dispatches texconv.comp's ETC2 build: BC1
   to ETC2 RGB8A1, BC2, BC3 and BC7 to ETC2 RGBA8, BC4 and BC5 to EAC R11 and RG11 */
void bcn_etc2_cpu(int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices, uint8_t* out);
/* sixteen RGBA8 texels (R in the low byte, row by row) as an ETC2 RGB block, or RGB8A1 (punch), as stored */
void etc2_rgb_cpu(const uint32_t texels[16], int punch, uint8_t out[8]);
/* the encoder's candidate c of those texels (etc2_encode.glsl's et_build: 0 and 1 individual, 2 and 3 differential,
   4 H, 5 and 6 T, 7 planar), as stored, and its error as the encoder works it out (et_err) */
uint32_t etc2_candidate_cpu(const uint32_t texels[16], int punch, uint32_t c, uint8_t out[8]);
/* whether the encoder tries candidate c of those texels (et_encode's choice of candidates) */
int etc2_tries_cpu(const uint32_t texels[16], int punch, uint32_t c);
/* sixteen values (row by row) as an EAC block of kind 0 (8 bit alpha), 1 (11 bit unsigned) or 2 (signed), as
   stored */
void eac_cpu(int kind, const int32_t values[16], uint8_t out[8]);
/* the same blocks of kind BC2, BC3 or BC7 (2, 3 and 9) encoded again as ASTC 4x4 (texconv.comp's ASTC build, run by
   bcn_astc_cpu.cpp), 16 bytes each */
void bcn_astc_cpu(int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices, uint8_t* out);
/* the same without the BCn block's hints (astc_encode.glsl's SNX_ASTC_HINTS 0): every block searched for */
void bcn_astc_plain_cpu(int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices, uint8_t* out);
/* of n BC7 blocks in a row, how many the first of the ASTC build's two passes leaves to the second (those no hint
   settles), and of which BC7 mode each: left[0] to left[7] */
uint32_t bcn_astc_left_cpu(const uint8_t* blocks, uint32_t n, uint32_t left[8]);
#ifdef __cplusplus
}
#endif
#endif
