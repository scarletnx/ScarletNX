/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Quads as two triangles each, (0 1 2) and (0 2 3), as texconv's TC_INDEX_QUADS makes them on the GPU (vk/render.c
   makes them here when the CPU has the indices) */
#ifndef SNX_VK_QUAD_INDICES_H
#define SNX_VK_QUAD_INDICES_H
#include <stdint.h>
#include <string.h>
#include "common/utilities/endian.h"

/* index j of little-endian indices of isz bytes (1, 2 or 4) */
static inline uint32_t snx_quad_index(const uint8_t* in, uint32_t isz, uint32_t j) {
    return isz == 1 ? in[j] : isz == 2 ? snx_ld_le16(in + 2 * (size_t)j) : snx_ld_le32(in + 4 * (size_t)j);
}
/* nq quads' 32-bit little-endian triangle indices, 6 a quad, into out, from their indices in in */
static inline void snx_quad_indices(const uint8_t* in, uint32_t isz, uint32_t nq, uint8_t* out) {
    for (uint32_t q = 0; q < nq; q++) {
        uint32_t v0 = snx_quad_index(in, isz, 4 * q), v1 = snx_quad_index(in, isz, 4 * q + 1);
        uint32_t v2 = snx_quad_index(in, isz, 4 * q + 2), v3 = snx_quad_index(in, isz, 4 * q + 3);
        uint8_t* o = out + 24 * (size_t)q;
        snx_st_le32(o, v0);
        snx_st_le32(o + 4, v1);
        snx_st_le32(o + 8, v2);
        snx_st_le32(o + 12, v0);
        snx_st_le32(o + 16, v2);
        snx_st_le32(o + 20, v3);
    }
}

#endif
