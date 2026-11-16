/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ASTC 4x4 encoder of each GPU vendor run on the CPU (astc_vendor_cpu.cpp), for the tests: vendor is vk.h's
   VK_ASTC_ENC_*, texconv.glsl's SNX_ASTC_VENDOR: 0 astc_encode.glsl, 1 astc_encode_arm.glsl (Mali), 2
   astc_encode_qcom.glsl (Adreno). Each function is astc_encode_cpu.h's or bcn_etc2_cpu.h's of the same name without
   _vendor, built with that vendor's encoder */
#ifndef SNX_ASTC_VENDOR_CPU_H
#define SNX_ASTC_VENDOR_CPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void astc_encode_vendor_cpu(int vendor, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t depth, uint8_t* out);
uint32_t astc_encode_block_vendor_cpu(int vendor, const uint32_t texels[16], uint8_t out[16]);
void bcn_astc_vendor_cpu(int vendor, int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices,
                         uint8_t* out);
void bcn_astc_vendor_passes_cpu(int vendor, int kind, const uint8_t* blocks, uint32_t n, uint8_t* out,
                                uint32_t listed[2]);
/* a candidate as the encoder keeps its best (astc_encode.glsl's as_np, as_cem, as_enc, as_ib, the ends, the
   weights a nibble a texel, two planes and their channel, the partition), packed into a block by the vendor's AsPack */
typedef struct {
    uint32_t np, cem, enc, ib, s0a, s1a, s0b, s1b, lo, hi, dual, plane, lo2, hi2, part;
} AstcCandidate;
void astc_pack_vendor_cpu(int vendor, const AstcCandidate* c, uint8_t out[16]);
#ifdef __cplusplus
}
#endif
#endif
