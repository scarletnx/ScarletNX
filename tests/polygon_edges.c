/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A draw's primitives made into lines or points on the CPU (vk/polygon_edges.h): each topology's sides in order, a quad
   and a polygon without their diagonals, a loop closed, restart ending a run, the indices' sizes, and the count it
   gives without writing the same as the indices it writes */
#include "nx/gpu/vk/polygon_edges.h"
#include <stdio.h>
#include <string.h>

static int failures;

/* topology's primitives of the count indices idx, as isz-byte little-endian indices (0: vertex numbers, which idx
   then lists), against the want_n indices want */
static void check(const char* what, uint32_t topology, uint32_t isz, const uint32_t* idx, uint32_t count, int restart,
                  uint32_t ri, int points, const uint32_t* want, uint32_t want_n) {
    uint8_t in[256], out[1024];
    for (uint32_t j = 0; j < count; j++)
        for (uint32_t b = 0; b < isz; b++) in[isz * j + b] = (uint8_t)(idx[j] >> (8 * b));
    SnxEdgeIndices src = {.bytes = isz ? in : NULL, .size = isz};
    uint32_t counted = snx_polygon_edges(topology, &src, count, restart, ri, points, NULL);
    memset(out, 0xAA, sizeof out);
    uint32_t n = snx_polygon_edges(topology, &src, count, restart, ri, points, out);
    if (n != want_n || counted != n) {
        fprintf(stderr, "FAIL: %s: %u indices written and %u counted, expected %u\n", what, n, counted, want_n);
        failures++;
        return;
    }
    for (uint32_t k = 0; k < n; k++)
        if (snx_ld_le32(out + 4 * k) != want[k]) {
            fprintf(stderr, "FAIL: %s: index %u is %u, expected %u\n", what, k, snx_ld_le32(out + 4 * k), want[k]);
            failures++;
            return;
        }
    for (size_t b = 4 * (size_t)n; b < sizeof out; b++)
        if (out[b] != 0xAA) {
            fprintf(stderr, "FAIL: %s: wrote past its %u indices, at byte %zu\n", what, n, b);
            failures++;
            break;
        }
    if (6 * count + 2 < n) {
        fprintf(stderr, "FAIL: %s: %u indices from %u, past the bound of 6 each and 2\n", what, n, count);
        failures++;
    }
}
#define CHECK_EDGES(what, topology, isz, idx, restart, ri, points, ...)                                                \
    do {                                                                                                               \
        static const uint32_t want_[] = {__VA_ARGS__};                                                                 \
        check(what, topology, isz, idx, (uint32_t)(sizeof idx / sizeof idx[0]), restart, ri, points, want_,            \
              (uint32_t)(sizeof want_ / sizeof want_[0]));                                                             \
    } while (0)

int main(void) {
    static const uint32_t four[4] = {0, 1, 2, 3}, six[6] = {0, 1, 2, 3, 4, 5}, seven[7] = {0, 1, 2, 3, 4, 5, 6};
    static const uint32_t wide[4] = {0x10, 0xfe, 0x7f, 0x80}, big[4] = {0x1234, 0xabcdef, 0x7fffffff, 0xfffffffe};
    /* a quad and a polygon: the four sides, no diagonal (0 2) */
    CHECK_EDGES("a quad", 7, 0, four, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 0);
    CHECK_EDGES("a polygon", 9, 0, four, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 0);
    CHECK_EDGES("two triangles", 4, 0, six, 0, 0, 0, 0, 1, 1, 2, 2, 0, 3, 4, 4, 5, 5, 3);
    /* a strip's odd triangle turned, (2 1 3), and a fan around its first vertex */
    CHECK_EDGES("a triangle strip", 5, 0, four, 0, 0, 0, 0, 1, 1, 2, 2, 0, 2, 1, 1, 3, 3, 2);
    CHECK_EDGES("a triangle fan", 6, 0, four, 0, 0, 0, 0, 1, 1, 2, 2, 0, 0, 2, 2, 3, 3, 0);
    /* a quad strip's quads (0 1 3 2) and (2 3 5 4): its sides and the crossings, no diagonal */
    CHECK_EDGES("a quad strip", 8, 0, six, 0, 0, 0, 0, 1, 1, 3, 3, 2, 2, 0, 2, 3, 3, 5, 5, 4, 4, 2);
    CHECK_EDGES("a line loop", 2, 0, four, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 0);
    /* what a list leaves over is no primitive: the seventh vertex of triangles, the fourth of a strip's pairs */
    CHECK_EDGES("triangles and a vertex over", 4, 0, seven, 0, 0, 0, 0, 1, 1, 2, 2, 0, 3, 4, 4, 5, 5, 3);
    CHECK_EDGES("a quad strip and a vertex over", 8, 0, seven, 0, 0, 0, 0, 1, 1, 3, 3, 2, 2, 0, 2, 3, 3, 5, 5, 4, 4, 2);
    /* points: each primitive's vertices, a polygon's once */
    CHECK_EDGES("a quad as points", 7, 0, four, 0, 0, 1, 0, 1, 2, 3);
    CHECK_EDGES("a polygon as points", 9, 0, four, 0, 0, 1, 0, 1, 2, 3);
    CHECK_EDGES("a strip as points", 5, 0, four, 0, 0, 1, 0, 1, 2, 2, 1, 3);
    CHECK_EDGES("a loop as points stays lines", 2, 0, four, 0, 0, 1, 0, 1, 1, 2, 2, 3, 3, 0);
    /* the indices' sizes */
    CHECK_EDGES("a quad of 8-bit indices", 7, 1, wide, 0, 0, 0, 0x10, 0xfe, 0xfe, 0x7f, 0x7f, 0x80, 0x80, 0x10);
    CHECK_EDGES("a quad of 16-bit indices", 7, 2, wide, 0, 0, 0, 0x10, 0xfe, 0xfe, 0x7f, 0x7f, 0x80, 0x80, 0x10);
    CHECK_EDGES("a quad of 32-bit indices", 7, 4, big, 0, 0, 0, 0x1234, 0xabcdef, 0xabcdef, 0x7fffffff, 0x7fffffff,
                0xfffffffe, 0xfffffffe, 0x1234);
    /* restart: a run ends there, a polygon and a loop close, a quad left short goes, and with restart off the index
       is a vertex */
    static const uint32_t polys[9] = {0, 1, 2, 3, 8, 4, 5, 6, 7}, loops[7] = {0, 1, 2, 8, 3, 4, 5};
    static const uint32_t short_quad[9] = {0, 1, 2, 8, 3, 4, 5, 6, 8}, short_poly[6] = {0, 1, 8, 2, 3, 4};
    CHECK_EDGES("two polygons", 9, 2, polys, 1, 8, 0, 0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4);
    CHECK_EDGES("two loops", 2, 1, loops, 1, 8, 0, 0, 1, 1, 2, 2, 0, 3, 4, 4, 5, 5, 3);
    CHECK_EDGES("a short quad before a restart", 7, 4, short_quad, 1, 8, 0, 3, 4, 4, 5, 5, 6, 6, 3);
    CHECK_EDGES("a polygon of two vertices", 9, 2, short_poly, 1, 8, 0, 2, 3, 3, 4, 4, 2);
    /* one of fewer than three vertices last puts nothing down, not even where the count leaves no room */
    static const uint32_t two[2] = {0, 1}, short_last[6] = {0, 1, 2, 8, 3, 4};
    check("a polygon of two vertices alone", 9, 0, two, 2, 0, 0, 0, NULL, 0);
    check("a polygon of two vertices alone, as points", 9, 0, two, 2, 0, 0, 1, NULL, 0);
    CHECK_EDGES("a polygon of two vertices last", 9, 2, short_last, 1, 8, 0, 0, 1, 1, 2, 2, 0);
    CHECK_EDGES("a polygon of two vertices last, as points", 9, 2, short_last, 1, 8, 1, 0, 1, 2);
    CHECK_EDGES("a strip restarting", 5, 2, polys, 1, 8, 0, 0, 1, 1, 2, 2, 0, 2, 1, 1, 3, 3, 2, 4, 5, 5, 6, 6, 4, 6, 5,
                5, 7, 7, 6);
    CHECK_EDGES("a fan restarting with its own hub", 6, 2, polys, 1, 8, 0, 0, 1, 1, 2, 2, 0, 0, 2, 2, 3, 3, 0, 4, 5, 5,
                6, 6, 4, 4, 6, 6, 7, 7, 4);
    CHECK_EDGES("a polygon with restart off", 9, 2, polys, 0, 8, 0, 0, 1, 1, 2, 2, 3, 3, 8, 8, 4, 4, 5, 5, 6, 6, 7, 7,
                0);
    /* inline indices: host words */
    {
        SnxEdgeIndices words = {.words = wide};
        uint8_t out[32];
        uint32_t n = snx_polygon_edges(9, &words, 4, 0, 0, 0, out);
        if (n != 8 || snx_ld_le32(out + 4) != 0xfe || snx_ld_le32(out + 28) != 0x10) {
            fprintf(stderr, "FAIL: a polygon of inline indices: %u indices\n", n);
            failures++;
        }
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("polygon edges: ok\n");
    return 0;
}
