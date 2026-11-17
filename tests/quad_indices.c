/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Quads and quad strips made into triangles on the CPU (vk/quad_indices.h) as texconv's TC_INDEX_QUADS makes them on
   the GPU, in 32-bit little-endian indices from little-endian indices of 1, 2 or 4 bytes. Both triangles of a quad
   carry its provoking vertex where the convention reads it: the last of each with the last vertex provoking (a quad's
   fourth, 2i + 3 of a strip's quad i), the first of each with the first. A restart index ends the quad under way */
#include "nx/gpu/vk/quad_indices.h"
#include <stdio.h>

static int failures;

/* the indices idx (n of them) stored little-endian in isz bytes each, made into triangles and held to want (the
   positions in idx of the six indices of each quad, made quads of them) */
static void check(const char* what, uint32_t isz, const uint32_t* idx, uint32_t n, int strip, int last, int restart,
                  uint32_t ri, const int* want, uint32_t quads) {
    uint8_t in[64], out[24 * 16];
    for (uint32_t j = 0; j < n; j++)
        for (uint32_t b = 0; b < isz; b++) in[isz * j + b] = (uint8_t)(idx[j] >> (8 * b));
    uint32_t made = snx_quad_indices(in, isz, n, strip, last, restart, ri, out);
    if (made != quads) {
        fprintf(stderr, "FAIL: %s, %u-byte indices: %u quads made, expected %u\n", what, isz, made, quads);
        failures++;
        return;
    }
    for (uint32_t k = 0; k < 6 * quads; k++) {
        uint32_t expected = idx[want[k]], got = snx_ld_le32(out + 4 * k);
        if (got != expected) {
            fprintf(stderr, "FAIL: %s, %u-byte indices: index %u is %u, expected %u (index %d of the draw's)\n", what,
                    isz, k, got, expected, want[k]);
            failures++;
        }
    }
}

int main(void) {
    static const uint32_t small[8] = {0, 1, 2, 3, 4, 5, 6, 7}, wide[8] = {0x10, 0xff, 0x7f, 0x80, 7, 6, 5, 4};
    static const uint32_t large[8] = {0x1234, 0xfffe, 0x8000, 0x00ff, 0x10000, 0xabcdef, 0x7fffffff, 0xffffffff};
    /* quads: the last vertex provoking splits each by its second and fourth vertices, the first by its first and
       third */
    static const int quads_last[12] = {0, 1, 3, 1, 2, 3, 4, 5, 7, 5, 6, 7};
    static const int quads_first[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    /* a strip of 8 vertices, three quads (2i, 2i + 1, 2i + 3, 2i + 2 around the edge), split by 2i and 2i + 3 */
    static const int strip_last[18] = {2, 0, 3, 0, 1, 3, 4, 2, 5, 2, 3, 5, 6, 4, 7, 4, 5, 7};
    static const int strip_first[18] = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4, 4, 5, 7, 4, 7, 6};
    const uint32_t* sets[3] = {small, wide, large};
    for (uint32_t isz = 1, s = 0; isz <= 4; isz *= 2, s++) {
        const uint32_t* idx = sets[s];
        check("quads, the last vertex provoking", isz, idx, 8, 0, 1, 0, 0, quads_last, 2);
        check("quads, the first vertex provoking", isz, idx, 8, 0, 0, 0, 0, quads_first, 2);
        check("a quad strip, the last vertex provoking", isz, idx, 8, 1, 1, 0, 0, strip_last, 3);
        check("a quad strip, the first vertex provoking", isz, idx, 8, 1, 0, 0, 0, strip_first, 3);
        check("quads of 7 indices", isz, idx, 7, 0, 1, 0, 0, quads_last, 1);
        check("a quad strip of 3 indices", isz, idx, 3, 1, 1, 0, 0, strip_last, 0);
    }
    /* restart index 8 between two quads, and before an end that leaves a quad short: two quads, as the 3D engine
       draws them, where taking 8 for a vertex made (8 4 5 6) the second */
    static const uint32_t cut[11] = {0, 1, 2, 3, 8, 4, 5, 6, 7, 8, 9};
    static const int cut_last[12] = {0, 1, 3, 1, 2, 3, 5, 6, 8, 6, 7, 8};
    for (uint32_t isz = 1; isz <= 4; isz *= 2) check("quads restarting at 8", isz, cut, 11, 0, 1, 1, 8, cut_last, 2);
    /* the same indices with restart off: 8 is a vertex like any other */
    static const int whole[12] = {0, 1, 3, 1, 2, 3, 4, 5, 7, 5, 6, 7};
    check("quads with restart off", 2, cut, 11, 0, 1, 0, 8, whole, 2);
    /* a strip restarting at 0xff: two strips of one quad each */
    static const uint32_t strips[9] = {0, 1, 2, 3, 0xff, 4, 5, 6, 7};
    static const int strips_last[12] = {2, 0, 3, 0, 1, 3, 7, 5, 8, 5, 6, 8};
    check("a quad strip restarting at 0xff", 1, strips, 9, 1, 1, 1, 0xff, strips_last, 2);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("quad indices: ok\n");
    return 0;
}
