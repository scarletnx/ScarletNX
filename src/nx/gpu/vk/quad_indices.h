/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Quads and quad strips as two triangles each, as texconv's TC_INDEX_QUADS makes them on the GPU (vk/render.c makes
   them here when the CPU has the indices). A quad's flat colour is its provoking vertex's: the last of its four (a
   quad's fourth, 2i + 3 of a strip's quad i), or its first under the first vertex convention. Both triangles carry
   that vertex where the convention reads it, so they split the quad by the diagonal through it, each wound as the
   quad is around its edge (a quad's 0 1 2 3, a strip's quad i 2i, 2i + 1, 2i + 3, 2i + 2) */
#ifndef SNX_VK_QUAD_INDICES_H
#define SNX_VK_QUAD_INDICES_H
#include <stdint.h>
#include <string.h>
#include "common/utilities/endian.h"

/* index j of little-endian indices of isz bytes (1, 2 or 4) */
static inline uint32_t snx_quad_index(const uint8_t* in, uint32_t isz, uint32_t j) {
    return isz == 1 ? in[j] : isz == 2 ? snx_ld_le16(in + 2 * (size_t)j) : snx_ld_le32(in + 4 * (size_t)j);
}
/* the quads count vertices make: a quad strip's one for each 2 past its first 2 */
static inline uint32_t snx_quads_of(uint32_t count, int strip) {
    return strip ? (count >= 4 ? (count - 2) / 2 : 0) : count / 4;
}
/* the six 32-bit little-endian indices of the quad of vertices v, in drawing order, into out */
static inline void snx_quad_triangles(const uint32_t v[4], int strip, int last, uint8_t* out) {
    static const int turned[6] = {1, 2, 0, 4, 5, 3}; /* each triangle turned to end on its first vertex */
    const uint32_t edge[4] = {v[0], v[1], strip ? v[3] : v[2], strip ? v[2] : v[3]};
    uint32_t p = last ? (strip ? 2u : 3u) : 0u, w[4]; /* p: the provoking vertex's place around the edge */
    for (uint32_t k = 0; k < 4; k++) w[k] = edge[(p + k) & 3];
    const uint32_t t[6] = {w[0], w[1], w[2], w[0], w[2], w[3]};
    for (int k = 0; k < 6; k++) snx_st_le32(out + 4 * k, t[last ? turned[k] : k]);
}
/* The triangles of count little-endian indices of isz bytes drawn as quads, or as a quad strip, into out (24 bytes a
   quad, snx_quads_of(count) at most), the last vertex provoking when last is set. Where restart is set the index ri
   ends the primitive under way, as the 3D engine's primitive restart does, and the next index begins another: a quad
   it cuts short is not drawn. Returns the quads made */
static inline uint32_t snx_quad_indices(const uint8_t* in, uint32_t isz, uint32_t count, int strip, int last,
                                        int restart, uint32_t ri, uint8_t* out) {
    uint32_t made = 0, start = 0, step = strip ? 2 : 4;
    for (uint32_t j = 0; j <= count; j++) {
        if (j < count && !(restart && snx_quad_index(in, isz, j) == ri)) continue;
        for (uint32_t q = 0, n = snx_quads_of(j - start, strip); q < n; q++) {
            uint32_t v[4];
            for (uint32_t k = 0; k < 4; k++) v[k] = snx_quad_index(in, isz, start + q * step + k);
            snx_quad_triangles(v, strip, last, out + 24 * (size_t)made++);
        }
        start = j + 1;
    }
    return made;
}

#endif
