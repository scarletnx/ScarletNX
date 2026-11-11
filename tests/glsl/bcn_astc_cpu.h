/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BCn to ASTC transcoder (texconv.glsl built with SNX_TC_ASTC, its astc_encode.glsl) run on the CPU
   (bcn_astc_cpu.cpp), for the tests. Each call takes the width of the shader's floats: 0 for 32 bits, nonzero for
   every operation rounded to 16 (glsl.hpp's half), which a GPU may compute its relaxed precision in */
#ifndef SNX_BCN_ASTC_CPU_H
#define SNX_BCN_ASTC_CPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* bw x bh blocks of slices slices of BCn kind (tests/support/bcn.h's BCN_BC2, BCN_BC3 or BCN_BC7), rows of blocks
   after rows, into as many ASTC 4x4 blocks as vk/texconv.c dispatches texconv.comp's ASTC build */
void bcn_astc_cpu(int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices, int half, uint8_t* out);
/* sixteen RGBA8 texels (R in the low byte, row by row) as an ASTC 4x4 block, as stored, with what astc_encode.glsl's
   ax_encode is told of the source block: its second subset's texels as seed draws them (0: one subset) and the
   channel with indices of its own (4: none) */
void astc_block_cpu(const uint32_t texels[16], uint32_t part, uint32_t seed, uint32_t own, int half, uint8_t out[16]);
/* the encoder's block of one kind of those texels (ax_build: 0 a line of colours, 1 a line of colours and alpha, which
   follows the channel own alone when own is below 4, 2 a plane of its own for own, 3 two partitions, 4 a line of
   colours and alpha of 2 bit weights), as stored, and
   how far the encoder reckons it from the texels (ax_err), what it picks between two kinds by */
uint32_t astc_candidate_cpu(const uint32_t texels[16], uint32_t kind, uint32_t part, uint32_t seed, uint32_t own,
                            int half, uint8_t out[16]);
/* the level of a range of levels levels (192, 48 or 40) the encoder stores for the value v (ax_levels): its bits, its
   trit or quint, and what it decodes to */
void astc_level_cpu(uint32_t v, uint32_t levels, int half, uint32_t* bits, uint32_t* digit, uint32_t* value);
/* the encoder's tables: the ASTC seed (bits 0 to 9) and its second partition's texels (from bit 10) for a BC7
   partition of 2 or 3 subsets */
uint32_t astc_partition_cpu(uint32_t subsets, uint32_t partition);
#ifdef __cplusplus
}
#endif
#endif
