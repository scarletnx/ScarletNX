/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Quads made into triangles on the CPU (vk/quad_indices.h) as texconv's TC_INDEX_QUADS makes them on the GPU: each quad
   (0 1 2 3) as (0 1 2) and (0 2 3), in 32-bit little-endian indices, from little-endian indices of 1, 2 or 4 bytes */
#include "nx/gpu/vk/quad_indices.h"
#include <stdio.h>

static int failures;

/* two quads of the given indices, stored little-endian in isz bytes each */
static void check(uint32_t isz, const uint32_t idx[8]) {
    uint8_t in[32], out[48];
    for (int j = 0; j < 8; j++)
        for (uint32_t b = 0; b < isz; b++) in[isz * (uint32_t)j + b] = (uint8_t)(idx[j] >> (8 * b));
    snx_quad_indices(in, isz, 2, out);
    static const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int q = 0; q < 2; q++)
        for (int k = 0; k < 6; k++) {
            uint32_t want = idx[4 * q + order[k]], got = snx_ld_le32(out + 24 * q + 4 * k);
            if (got != want) {
                fprintf(stderr, "FAIL: %u-byte indices, quad %d index %d: expected %u, found %u\n", isz, q, k, want,
                        got);
                failures++;
            }
        }
}

int main(void) {
    static const uint32_t small[8] = {0, 1, 2, 3, 4, 5, 6, 7}, wide[8] = {0x10, 0xff, 0x7f, 0x80, 7, 6, 5, 4};
    static const uint32_t large[8] = {0x1234, 0xfffe, 0x8000, 0x00ff, 0x10000, 0xabcdef, 0x7fffffff, 0xffffffff};
    check(1, small);
    check(1, wide);
    check(2, wide);
    check(2, (const uint32_t[8]){0x1234, 0xfffe, 0x8000, 0x00ff, 1, 2, 3, 0xffff});
    check(4, large);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("quad indices: ok\n");
    return 0;
}
