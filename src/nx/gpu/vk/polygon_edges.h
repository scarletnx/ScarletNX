/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A draw's primitives as a list of lines or points that Vulkan draws as the 3D engine does (vk/render.c draw_edges):
   a line loop closed from its last vertex to its first, and polygons in the polygon mode Line or Point. Each triangle
   gives its three sides, and each quad and polygon its outline, without the diagonals of the triangles Vulkan would
   make of them. The topologies are the 3D engine's: 2 a line loop, 4 triangles, 5 a triangle strip, 6 a triangle fan,
   7 quads, 8 a quad strip, 9 a polygon. A restart index ends a run as it ends a strip: a loop or a polygon closes
   there, and a polygon of fewer than three vertices draws nothing */
#ifndef SNX_VK_POLYGON_EDGES_H
#define SNX_VK_POLYGON_EDGES_H
#include <stddef.h>
#include <stdint.h>
#include "common/utilities/endian.h"

/* the draw's indices: little-endian ones of size bytes (1, 2 or 4) at bytes, host words at words (inline indices),
   or the vertex numbers 0 on when both are NULL */
typedef struct {
    const uint8_t* bytes;
    const uint32_t* words;
    uint32_t size;
} SnxEdgeIndices;

static inline uint32_t snx_edge_index(const SnxEdgeIndices* in, uint32_t j) {
    if (in->words) return in->words[j];
    if (!in->bytes) return j;
    const uint8_t* p = in->bytes + (size_t)j * in->size;
    return in->size == 1 ? p[0] : in->size == 2 ? snx_ld_le16(p) : snx_ld_le32(p);
}

/* vertex v as index *n of out (32-bit little-endian), or only counted when out is NULL */
static inline void snx_edge_put(uint8_t* out, uint32_t* n, uint32_t v) {
    if (out) snx_st_le32(out + 4 * (size_t)*n, v);
    (*n)++;
}
/* a primitive's k vertices v in its winding: its sides as lines (v0 v1) (v1 v2)... back to v0, or its vertices as
   points */
static inline void snx_edge_shape(uint8_t* out, uint32_t* n, const uint32_t* v, uint32_t k, int points) {
    for (uint32_t i = 0; i < k; i++) {
        snx_edge_put(out, n, v[i]);
        if (!points) snx_edge_put(out, n, v[i + 1 < k ? i + 1 : 0]);
    }
}

/* The 32-bit little-endian indices of the primitives of count indices of topology as lines, or as points (points:
   the polygon mode Point; a line loop is always lines), into out, or only counted when out is NULL: how many. With
   restart, the index ri (as the indices' size holds it) ends a run. At most 6 for each index and 2 for each run */
static inline uint32_t snx_polygon_edges(uint32_t topology, const SnxEdgeIndices* in, uint32_t count, int restart,
                                         uint32_t ri, int points, uint8_t* out) {
    uint32_t n = 0, start = 0, run = 0, first = 0, last = 0, v[4] = {0};
    if (topology == 2) points = 0;
    for (uint32_t j = 0; j <= count; j++) {
        uint32_t x = j < count ? snx_edge_index(in, j) : 0;
        if (j == count || (restart && x == ri)) { /* the run ends: a loop and a polygon close */
            if (topology == 9 && run < 3)
                n = start; /* no polygon: its sides so far go */
            else if ((topology == 2 && run >= 2) || (topology == 9 && !points)) {
                snx_edge_put(out, &n, last);
                snx_edge_put(out, &n, first);
            }
            run = 0;
            start = n;
            continue;
        }
        if (!run) first = x;
        run++;
        switch (topology) {
            case 2:
            case 9: /* a side from each vertex to the next; a polygon's points are its vertices */
                if (points)
                    snx_edge_put(out, &n, x);
                else if (run >= 2) {
                    snx_edge_put(out, &n, last);
                    snx_edge_put(out, &n, x);
                }
                break;
            case 4:
            case 7: { /* lists: a triangle at each third vertex, a quad at each fourth */
                uint32_t k = topology == 4 ? 3 : 4;
                v[(run - 1) % k] = x;
                if (run % k == 0) snx_edge_shape(out, &n, v, k, points);
                break;
            }
            case 5:
            case 6: /* a triangle at each vertex past the second, in Vulkan's order: (v0 v1 x), a strip's odd ones turned
                       (v1 v0 x), and a fan's around its first vertex (first v1 x). v0 and v1 are the two before */
                if (run >= 3) {
                    uint32_t t[3] = {topology == 6 ? first
                                     : (run & 1)   ? v[0]
                                                   : v[1],
                                     topology == 6 || (run & 1) ? v[1] : v[0], x};
                    snx_edge_shape(out, &n, t, 3, points);
                }
                v[0] = v[1];
                v[1] = x;
                break;
            case 8: /* a quad at each even vertex past the second: (v0 v1 x v2), the last pair (v0 v1) and this one
                       (v2 x), as the 3D engine winds it */
                if (run == 1)
                    v[0] = x;
                else if (run == 2)
                    v[1] = x;
                else if (run & 1)
                    v[2] = x;
                else {
                    uint32_t q[4] = {v[0], v[1], x, v[2]};
                    snx_edge_shape(out, &n, q, 4, points);
                    v[0] = v[2];
                    v[1] = x;
                }
                break;
            default: break;
        }
        last = x;
    }
    return n;
}

#endif
