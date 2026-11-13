/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2016-2026 melonDS team
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's geometry side, ported from melonDS (GPU3D.cpp at 497aa6c, GPL-3.0-or-later): the matrix stacks, the
   geometry commands, lighting, clipping and the viewport transform, the box, position and vector tests, and the swap
   of the polygon and vertex buffers at the vertical blank. Its numbers are the console's fixed point, and the game
   reads them back (the clip matrix, the tests' results, the buffers' counts), so they are worked out here, on the CPU;
   what is drawn of them, every pixel, is the GPU's (vk/render3d.c).

   The console's geometry engine takes its commands from a FIFO at its own pace. Here a command runs when it is
   written, so the FIFO is always empty, but for the commands after SWAP_BUFFERS: the console holds those until the
   buffers swap at the vertical blank, and so does this, in a queue. */
#include "nds/gpu/gpu3d.h"
#include "nds/gpu/gpu3d_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t command_params[256] =
{
    // 0x00
    0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x10
    1, 0, 1, 1, 1, 0, 16, 12, 16, 12, 9, 3, 3,
    0, 0, 0,
    // 0x20
    1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0,
    // 0x30
    1, 1, 1, 1, 32,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x40
    1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x50
    1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x60
    1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x70
    3, 2, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x80+
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};


static void matrix_load_identity(int32_t* m)
{
    m[0] = 0x1000; m[1] = 0;      m[2] = 0;       m[3] = 0;
    m[4] = 0;      m[5] = 0x1000; m[6] = 0;       m[7] = 0;
    m[8] = 0;      m[9] = 0;      m[10] = 0x1000; m[11] = 0;
    m[12] = 0;     m[13] = 0;     m[14] = 0;      m[15] = 0x1000;
}

static void matrix_load4x4(int32_t* m, const int32_t* s)
{
    memcpy(m, s, 16*4);
}

static void matrix_load4x3(int32_t* m, const int32_t* s)
{
    m[0] = s[0];  m[1] = s[1];  m[2] = s[2];    m[3] = 0;
    m[4] = s[3];  m[5] = s[4];  m[6] = s[5];    m[7] = 0;
    m[8] = s[6];  m[9] = s[7];  m[10] = s[8];   m[11] = 0;
    m[12] = s[9]; m[13] = s[10]; m[14] = s[11]; m[15] = 0x1000;
}

static void matrix_mult4x4(int32_t* m, const int32_t* s)
{
    int32_t tmp[16];
    memcpy(tmp, m, 16*4);

    // m = s*m
    m[0] = ((int64_t)s[0]*tmp[0] + (int64_t)s[1]*tmp[4] + (int64_t)s[2]*tmp[8] + (int64_t)s[3]*tmp[12]) >> 12;
    m[1] = ((int64_t)s[0]*tmp[1] + (int64_t)s[1]*tmp[5] + (int64_t)s[2]*tmp[9] + (int64_t)s[3]*tmp[13]) >> 12;
    m[2] = ((int64_t)s[0]*tmp[2] + (int64_t)s[1]*tmp[6] + (int64_t)s[2]*tmp[10] + (int64_t)s[3]*tmp[14]) >> 12;
    m[3] = ((int64_t)s[0]*tmp[3] + (int64_t)s[1]*tmp[7] + (int64_t)s[2]*tmp[11] + (int64_t)s[3]*tmp[15]) >> 12;

    m[4] = ((int64_t)s[4]*tmp[0] + (int64_t)s[5]*tmp[4] + (int64_t)s[6]*tmp[8] + (int64_t)s[7]*tmp[12]) >> 12;
    m[5] = ((int64_t)s[4]*tmp[1] + (int64_t)s[5]*tmp[5] + (int64_t)s[6]*tmp[9] + (int64_t)s[7]*tmp[13]) >> 12;
    m[6] = ((int64_t)s[4]*tmp[2] + (int64_t)s[5]*tmp[6] + (int64_t)s[6]*tmp[10] + (int64_t)s[7]*tmp[14]) >> 12;
    m[7] = ((int64_t)s[4]*tmp[3] + (int64_t)s[5]*tmp[7] + (int64_t)s[6]*tmp[11] + (int64_t)s[7]*tmp[15]) >> 12;

    m[8] = ((int64_t)s[8]*tmp[0] + (int64_t)s[9]*tmp[4] + (int64_t)s[10]*tmp[8] + (int64_t)s[11]*tmp[12]) >> 12;
    m[9] = ((int64_t)s[8]*tmp[1] + (int64_t)s[9]*tmp[5] + (int64_t)s[10]*tmp[9] + (int64_t)s[11]*tmp[13]) >> 12;
    m[10] = ((int64_t)s[8]*tmp[2] + (int64_t)s[9]*tmp[6] + (int64_t)s[10]*tmp[10] + (int64_t)s[11]*tmp[14]) >> 12;
    m[11] = ((int64_t)s[8]*tmp[3] + (int64_t)s[9]*tmp[7] + (int64_t)s[10]*tmp[11] + (int64_t)s[11]*tmp[15]) >> 12;

    m[12] = ((int64_t)s[12]*tmp[0] + (int64_t)s[13]*tmp[4] + (int64_t)s[14]*tmp[8] + (int64_t)s[15]*tmp[12]) >> 12;
    m[13] = ((int64_t)s[12]*tmp[1] + (int64_t)s[13]*tmp[5] + (int64_t)s[14]*tmp[9] + (int64_t)s[15]*tmp[13]) >> 12;
    m[14] = ((int64_t)s[12]*tmp[2] + (int64_t)s[13]*tmp[6] + (int64_t)s[14]*tmp[10] + (int64_t)s[15]*tmp[14]) >> 12;
    m[15] = ((int64_t)s[12]*tmp[3] + (int64_t)s[13]*tmp[7] + (int64_t)s[14]*tmp[11] + (int64_t)s[15]*tmp[15]) >> 12;
}

static void matrix_mult4x3(int32_t* m, const int32_t* s)
{
    int32_t tmp[16];
    memcpy(tmp, m, 16*4);

    // m = s*m
    m[0] = ((int64_t)s[0]*tmp[0] + (int64_t)s[1]*tmp[4] + (int64_t)s[2]*tmp[8]) >> 12;
    m[1] = ((int64_t)s[0]*tmp[1] + (int64_t)s[1]*tmp[5] + (int64_t)s[2]*tmp[9]) >> 12;
    m[2] = ((int64_t)s[0]*tmp[2] + (int64_t)s[1]*tmp[6] + (int64_t)s[2]*tmp[10]) >> 12;
    m[3] = ((int64_t)s[0]*tmp[3] + (int64_t)s[1]*tmp[7] + (int64_t)s[2]*tmp[11]) >> 12;

    m[4] = ((int64_t)s[3]*tmp[0] + (int64_t)s[4]*tmp[4] + (int64_t)s[5]*tmp[8]) >> 12;
    m[5] = ((int64_t)s[3]*tmp[1] + (int64_t)s[4]*tmp[5] + (int64_t)s[5]*tmp[9]) >> 12;
    m[6] = ((int64_t)s[3]*tmp[2] + (int64_t)s[4]*tmp[6] + (int64_t)s[5]*tmp[10]) >> 12;
    m[7] = ((int64_t)s[3]*tmp[3] + (int64_t)s[4]*tmp[7] + (int64_t)s[5]*tmp[11]) >> 12;

    m[8] = ((int64_t)s[6]*tmp[0] + (int64_t)s[7]*tmp[4] + (int64_t)s[8]*tmp[8]) >> 12;
    m[9] = ((int64_t)s[6]*tmp[1] + (int64_t)s[7]*tmp[5] + (int64_t)s[8]*tmp[9]) >> 12;
    m[10] = ((int64_t)s[6]*tmp[2] + (int64_t)s[7]*tmp[6] + (int64_t)s[8]*tmp[10]) >> 12;
    m[11] = ((int64_t)s[6]*tmp[3] + (int64_t)s[7]*tmp[7] + (int64_t)s[8]*tmp[11]) >> 12;

    m[12] = ((int64_t)s[9]*tmp[0] + (int64_t)s[10]*tmp[4] + (int64_t)s[11]*tmp[8] + (int64_t)0x1000*tmp[12]) >> 12;
    m[13] = ((int64_t)s[9]*tmp[1] + (int64_t)s[10]*tmp[5] + (int64_t)s[11]*tmp[9] + (int64_t)0x1000*tmp[13]) >> 12;
    m[14] = ((int64_t)s[9]*tmp[2] + (int64_t)s[10]*tmp[6] + (int64_t)s[11]*tmp[10] + (int64_t)0x1000*tmp[14]) >> 12;
    m[15] = ((int64_t)s[9]*tmp[3] + (int64_t)s[10]*tmp[7] + (int64_t)s[11]*tmp[11] + (int64_t)0x1000*tmp[15]) >> 12;
}

static void matrix_mult3x3(int32_t* m, const int32_t* s)
{
    int32_t tmp[12];
    memcpy(tmp, m, 12*4);

    // m = s*m
    m[0] = ((int64_t)s[0]*tmp[0] + (int64_t)s[1]*tmp[4] + (int64_t)s[2]*tmp[8]) >> 12;
    m[1] = ((int64_t)s[0]*tmp[1] + (int64_t)s[1]*tmp[5] + (int64_t)s[2]*tmp[9]) >> 12;
    m[2] = ((int64_t)s[0]*tmp[2] + (int64_t)s[1]*tmp[6] + (int64_t)s[2]*tmp[10]) >> 12;
    m[3] = ((int64_t)s[0]*tmp[3] + (int64_t)s[1]*tmp[7] + (int64_t)s[2]*tmp[11]) >> 12;

    m[4] = ((int64_t)s[3]*tmp[0] + (int64_t)s[4]*tmp[4] + (int64_t)s[5]*tmp[8]) >> 12;
    m[5] = ((int64_t)s[3]*tmp[1] + (int64_t)s[4]*tmp[5] + (int64_t)s[5]*tmp[9]) >> 12;
    m[6] = ((int64_t)s[3]*tmp[2] + (int64_t)s[4]*tmp[6] + (int64_t)s[5]*tmp[10]) >> 12;
    m[7] = ((int64_t)s[3]*tmp[3] + (int64_t)s[4]*tmp[7] + (int64_t)s[5]*tmp[11]) >> 12;

    m[8] = ((int64_t)s[6]*tmp[0] + (int64_t)s[7]*tmp[4] + (int64_t)s[8]*tmp[8]) >> 12;
    m[9] = ((int64_t)s[6]*tmp[1] + (int64_t)s[7]*tmp[5] + (int64_t)s[8]*tmp[9]) >> 12;
    m[10] = ((int64_t)s[6]*tmp[2] + (int64_t)s[7]*tmp[6] + (int64_t)s[8]*tmp[10]) >> 12;
    m[11] = ((int64_t)s[6]*tmp[3] + (int64_t)s[7]*tmp[7] + (int64_t)s[8]*tmp[11]) >> 12;
}

static void matrix_scale(int32_t* m, const int32_t* s)
{
    m[0] = ((int64_t)s[0]*m[0]) >> 12;
    m[1] = ((int64_t)s[0]*m[1]) >> 12;
    m[2] = ((int64_t)s[0]*m[2]) >> 12;
    m[3] = ((int64_t)s[0]*m[3]) >> 12;

    m[4] = ((int64_t)s[1]*m[4]) >> 12;
    m[5] = ((int64_t)s[1]*m[5]) >> 12;
    m[6] = ((int64_t)s[1]*m[6]) >> 12;
    m[7] = ((int64_t)s[1]*m[7]) >> 12;

    m[8] = ((int64_t)s[2]*m[8]) >> 12;
    m[9] = ((int64_t)s[2]*m[9]) >> 12;
    m[10] = ((int64_t)s[2]*m[10]) >> 12;
    m[11] = ((int64_t)s[2]*m[11]) >> 12;
}

static void matrix_translate(int32_t* m, const int32_t* s)
{
    m[12] += ((int64_t)s[0]*m[0] + (int64_t)s[1]*m[4] + (int64_t)s[2]*m[8]) >> 12;
    m[13] += ((int64_t)s[0]*m[1] + (int64_t)s[1]*m[5] + (int64_t)s[2]*m[9]) >> 12;
    m[14] += ((int64_t)s[0]*m[2] + (int64_t)s[1]*m[6] + (int64_t)s[2]*m[10]) >> 12;
    m[15] += ((int64_t)s[0]*m[3] + (int64_t)s[1]*m[7] + (int64_t)s[2]*m[11]) >> 12;
}

static void update_clip_matrix(NdsGpu3D* g)
{
    if (!g->clip_matrix_dirty) return;
    g->clip_matrix_dirty = false;

    memcpy(g->clip_matrix, g->proj_matrix, 16*4);
    matrix_mult4x4(g->clip_matrix, g->pos_matrix);
}



/* a vertex where the edge from vin (outside) to vout (inside) meets a side of the view volume */
static void clip_segment(NdsVertex* outbuf, const NdsVertex* vin, const NdsVertex* vout, int comp, int32_t plane,
                         bool attribs)
{
    int64_t factor_num = vin->position[3] - (plane*vin->position[comp]);
    int32_t factor_den = (int32_t)(factor_num - (vout->position[3] - (plane*vout->position[comp])));
    if (!factor_den) factor_den = 1;

#define INTERPOLATE(var, type)  { outbuf->var = (type)(vin->var + ((vout->var - vin->var) * factor_num) / factor_den); }

    if (comp != 0) INTERPOLATE(position[0], int32_t);
    if (comp != 1) INTERPOLATE(position[1], int32_t);
    if (comp != 2) INTERPOLATE(position[2], int32_t);
    INTERPOLATE(position[3], int32_t);
    outbuf->position[comp] = plane*outbuf->position[3];

    if (attribs)
    {
        INTERPOLATE(color[0], int32_t);
        INTERPOLATE(color[1], int32_t);
        INTERPOLATE(color[2], int32_t);

        INTERPOLATE(tex_coords[0], int16_t);
        INTERPOLATE(tex_coords[1], int16_t);
    }

    outbuf->clipped = true;

#undef INTERPOLATE
}

static int clip_against_plane(const NdsGpu3D* g, NdsVertex* vertices, int nverts, int clipstart, int comp,
                              bool attribs)
{
    NdsVertex temp[10];
    int prev, next;
    int c = clipstart;

    if (clipstart == 2)
    {
        temp[0] = vertices[0];
        temp[1] = vertices[1];
    }

    for (int i = clipstart; i < nverts; i++)
    {
        prev = i-1; if (prev < 0) prev = nverts-1;
        next = i+1; if (next >= nverts) next = 0;

        NdsVertex vtx = vertices[i];
        if (vtx.position[comp] > vtx.position[3])
        {
            if ((comp == 2) && (!(g->cur_polygon_attr & (1<<12)))) return 0;

            NdsVertex* vprev = &vertices[prev];
            if (vprev->position[comp] <= vprev->position[3])
            {
                clip_segment(&temp[c], &vtx, vprev, comp, 1, attribs);
                c++;
            }

            NdsVertex* vnext = &vertices[next];
            if (vnext->position[comp] <= vnext->position[3])
            {
                clip_segment(&temp[c], &vtx, vnext, comp, 1, attribs);
                c++;
            }
        }
        else
            temp[c++] = vtx;
    }

    nverts = c; c = clipstart;
    for (int i = clipstart; i < nverts; i++)
    {
        prev = i-1; if (prev < 0) prev = nverts-1;
        next = i+1; if (next >= nverts) next = 0;

        NdsVertex vtx = temp[i];
        if (vtx.position[comp] < -vtx.position[3])
        {
            NdsVertex* vprev = &temp[prev];
            if (vprev->position[comp] >= -vprev->position[3])
            {
                clip_segment(&vertices[c], &vtx, vprev, comp, -1, attribs);
                c++;
            }

            NdsVertex* vnext = &temp[next];
            if (vnext->position[comp] >= -vnext->position[3])
            {
                clip_segment(&vertices[c], &vtx, vnext, comp, -1, attribs);
                c++;
            }
        }
        else
            vertices[c++] = vtx;
    }

    // checkme
    for (int i = 0; i < c; i++)
    {
        NdsVertex* vtx = &vertices[i];

        vtx->color[0] &= ~0xFFF; vtx->color[0] += 0xFFF;
        vtx->color[1] &= ~0xFFF; vtx->color[1] += 0xFFF;
        vtx->color[2] &= ~0xFFF; vtx->color[2] += 0xFFF;
    }

    return c;
}

/* For each vertex outside the view volume whose neighbors are inside, a new vertex at the volume's side: Z, then Y,
   then X. The hardware seems to use another algorithm (it reacts differently to vertices with W=0), which melonDS
   leaves as a TODO */
static int clip_polygon(const NdsGpu3D* g, bool attribs, NdsVertex* vertices, int nverts, int clipstart)
{
    nverts = clip_against_plane(g, vertices, nverts, clipstart, 2, attribs);
    nverts = clip_against_plane(g, vertices, nverts, clipstart, 1, attribs);
    nverts = clip_against_plane(g, vertices, nverts, clipstart, 0, attribs);
    return nverts;
}

static void submit_polygon(NdsGpu3D* g);
static void calculate_lighting(NdsGpu3D* g);
static bool clip_coords_equal(const NdsVertex* a, const NdsVertex* b)
{
    return a->position[0] == b->position[0] &&
           a->position[1] == b->position[1] &&
           a->position[2] == b->position[2] &&
           a->position[3] == b->position[3];
}

static void submit_polygon(NdsGpu3D* g)
{
    NdsVertex clippedvertices[10];
    NdsVertex* reusedvertices[2];
    int clipstart = 0;
    int lastpolyverts = 0;

    int nverts = g->polygon_mode & 0x1 ? 4:3;

    // submitting a polygon starts the polygon pipeline
    // noting that for now we are only reserving one vertex slot
    // further slots only get reserved if the polygon makes it through culling/clipping

    // culling
    // TODO: work out how it works on the real thing
    // the normalization part is a wild guess

    NdsVertex *v0, *v1, *v2;
    int64_t normalX, normalY, normalZ;
    int64_t dot;

    v0 = &g->temp_vertex_buffer[0];
    v1 = &g->temp_vertex_buffer[1];
    v2 = &g->temp_vertex_buffer[2];

    normalX = ((int64_t)(v0->position[1]-v1->position[1]) * (v2->position[3]-v1->position[3]))
        - ((int64_t)(v0->position[3]-v1->position[3]) * (v2->position[1]-v1->position[1]));
    normalY = ((int64_t)(v0->position[3]-v1->position[3]) * (v2->position[0]-v1->position[0]))
        - ((int64_t)(v0->position[0]-v1->position[0]) * (v2->position[3]-v1->position[3]));
    normalZ = ((int64_t)(v0->position[0]-v1->position[0]) * (v2->position[1]-v1->position[1]))
        - ((int64_t)(v0->position[1]-v1->position[1]) * (v2->position[0]-v1->position[0]));

    while ((((normalX>>31) ^ (normalX>>63)) != 0) ||
           (((normalY>>31) ^ (normalY>>63)) != 0) ||
           (((normalZ>>31) ^ (normalZ>>63)) != 0))
    {
        normalX >>= 4;
        normalY >>= 4;
        normalZ >>= 4;
    }

    dot = ((int64_t)v1->position[0] * normalX) + ((int64_t)v1->position[1] * normalY) + ((int64_t)v1->position[3] * normalZ);

    bool facingview = (dot <= 0);

    if (dot < 0)
    {
        if (!(g->cur_polygon_attr & (1<<7)))
        {
            g->last_strip_polygon = NULL;
            return;
        }
    }
    else if (dot > 0)
    {
        if (!(g->cur_polygon_attr & (1<<6)))
        {
            g->last_strip_polygon = NULL;
            return;
        }
    }

    // for strips, check whether we can attach to the previous polygon
    // this requires two original vertices shared with the previous polygon, and that
    // the two polygons be of the same type

    if (g->polygon_mode >= 2 && g->last_strip_polygon)
    {
        int id0, id1;
        if (g->polygon_mode == 2)
        {
            if (g->num_consecutive_polygons & 1)
            {
                id0 = 2;
                id1 = 1;
            }
            else
            {
                id0 = 0;
                id1 = 2;
            }

            lastpolyverts = 3;
        }
        else
        {
            id0 = 3;
            id1 = 2;

            lastpolyverts = 4;
        }

        if (g->last_strip_polygon->num_vertices == (uint32_t)lastpolyverts &&
            !g->last_strip_polygon->vertices[id0]->clipped &&
            !g->last_strip_polygon->vertices[id1]->clipped)
        {
            reusedvertices[0] = g->last_strip_polygon->vertices[id0];
            reusedvertices[1] = g->last_strip_polygon->vertices[id1];

            clippedvertices[0] = *reusedvertices[0];
            clippedvertices[1] = *reusedvertices[1];

            clipstart = 2;
        }
    }

    for (int i = clipstart; i < nverts; i++)
        clippedvertices[i] = g->temp_vertex_buffer[i];

    // detect lines, for the OpenGL renderer

    int polytype = 0;
    if (nverts == 3)
    {
        if (clip_coords_equal(&clippedvertices[0], &clippedvertices[1]) ||
            clip_coords_equal(&clippedvertices[0], &clippedvertices[2]) ||
            clip_coords_equal(&clippedvertices[1], &clippedvertices[2]))
        {
            polytype = 1;
        }
    }
    else if (nverts == 4)
    {
        // TODO
    }

    // clipping

    nverts = clip_polygon(g, true, clippedvertices, nverts, clipstart);
    if (nverts == 0)
    {
        g->last_strip_polygon = NULL;
        return;
    }

    // reject the polygon if it's not going to fit in polygon/vertex RAM

    if (g->num_polygons >= 2048 || g->num_vertices+nverts > 6144)
    {
        g->last_strip_polygon = NULL;
        g->disp_cnt |= (1<<13);
        return;
    }

    // compute screen coordinates

    for (int i = clipstart; i < nverts; i++)
    {
        NdsVertex* vtx = &clippedvertices[i];

        // W is truncated to 24 bits at this point
        // if this W is zero, the polygon isn't rendered
        vtx->position[3] &= 0x00FFFFFF;

        // viewport transform
        // note: the DS performs these divisions using a 32-bit divider
        // thus, if W is greater than 0xFFFF, some precision is sacrificed
        // to make the numbers fit into the divider
        uint32_t posX, posY;
        uint32_t w = vtx->position[3];
        if (w == 0)
        {
            posX = 0;
            posY = 0;
        }
        else
        {
            posX = vtx->position[0] + w;
            posY = -vtx->position[1] + w;
            uint32_t den = w;

            if (w > 0xFFFF)
            {
                posX >>= 1;
                posY >>= 1;
                den  >>= 1;
            }

            den <<= 1;
            posX = ((posX * g->viewport[4]) / den) + g->viewport[0];
            posY = ((posY * g->viewport[5]) / den) + g->viewport[3];
        }

        vtx->final_position[0] = posX & 0x1FF;
        vtx->final_position[1] = posY & 0xFF;

        // hi-res positions
        // to consider: only do this when using the GL renderer? apply the aforementioned quirk to this?
        if (w != 0)
        {
            posX = ((((int64_t)(vtx->position[0] + w) * g->viewport[4]) << 4) / (((int64_t)w) << 1)) + (g->viewport[0] << 4);
            posY = ((((int64_t)(-vtx->position[1] + w) * g->viewport[5]) << 4) / (((int64_t)w) << 1)) + (g->viewport[3] << 4);

            vtx->hires_position[0] = posX & 0x1FFF;
            vtx->hires_position[1] = posY & 0xFFF;
        }
    }

    // zero-dot W check:
    // * if the polygon's vertices all have the same screen coordinates, it is considered to be zero-dot
    // * if all the vertices have a W greater than the threshold defined in register 0x04000610,
    //   the polygon is rejected, unless bit13 in the polygon attributes is set

    if (!(g->cur_polygon_attr & (1<<13)))
    {
        bool zerodot = true;
        bool allbehind = true;

        for (int i = 0; i < nverts; i++)
        {
            NdsVertex* vtx = &clippedvertices[i];

            if (vtx->final_position[0] != clippedvertices[0].final_position[0] ||
                vtx->final_position[1] != clippedvertices[0].final_position[1])
            {
                zerodot = false;
                break;
            }

            if ((uint32_t)vtx->position[3] <= g->zero_dot_w_limit)
            {
                allbehind = false;
                break;
            }
        }

        if (zerodot && allbehind)
        {
            g->last_strip_polygon = NULL;
            return;
        }
    }

    // build the actual polygon

    if (nverts == 4)
    {
    }
    else
    {
    }

    NdsPolygon* poly = &g->cur_polygon_ram[g->num_polygons++];
    poly->num_vertices = 0;

    poly->attr = g->cur_polygon_attr;
    poly->tex_param = g->tex_param;
    poly->tex_palette = g->tex_palette;

    poly->degenerate = false;
    poly->type = 0;

    poly->facing_view = facingview;

    uint32_t texfmt = (g->tex_param >> 26) & 0x7;
    uint32_t polyalpha = (g->cur_polygon_attr >> 16) & 0x1F;
    poly->translucent = (texfmt == 1 || texfmt == 6) || (polyalpha > 0 && polyalpha < 31);

    poly->is_shadow_mask = ((g->cur_polygon_attr & 0x3F000030) == 0x00000030);
    poly->is_shadow = ((g->cur_polygon_attr & 0x30) == 0x30) && !poly->is_shadow_mask;

    if (!poly->translucent) g->num_opaque_polygons++;

    poly->type = polytype;

    if (g->last_strip_polygon && clipstart > 0)
    {
        if (nverts == lastpolyverts)
        {
            poly->vertices[0] = reusedvertices[0];
            poly->vertices[1] = reusedvertices[1];
        }
        else
        {
            NdsVertex v0 = *reusedvertices[0];
            NdsVertex v1 = *reusedvertices[1];

            g->cur_vertex_ram[g->num_vertices] = v0;
            poly->vertices[0] = &g->cur_vertex_ram[g->num_vertices];
            g->cur_vertex_ram[g->num_vertices+1] = v1;
            poly->vertices[1] = &g->cur_vertex_ram[g->num_vertices+1];
            g->num_vertices += 2;
        }

        poly->num_vertices += 2;
    }

    for (int i = clipstart; i < nverts; i++)
    {
        NdsVertex* vtx = &g->cur_vertex_ram[g->num_vertices];
        *vtx = clippedvertices[i];
        poly->vertices[i] = vtx;

        g->num_vertices++;
        poly->num_vertices++;

        vtx->final_color[0] = vtx->color[0] >> 12;
        if (vtx->final_color[0]) vtx->final_color[0] = ((vtx->final_color[0] << 4) + 0xF);
        vtx->final_color[1] = vtx->color[1] >> 12;
        if (vtx->final_color[1]) vtx->final_color[1] = ((vtx->final_color[1] << 4) + 0xF);
        vtx->final_color[2] = vtx->color[2] >> 12;
        if (vtx->final_color[2]) vtx->final_color[2] = ((vtx->final_color[2] << 4) + 0xF);
    }

    // determine bounds of the polygon
    // also determine the W shift and normalize W
    // normalization works both ways
    // (ie two W's that span 12 bits or less will be brought to 16 bits)

    uint32_t vtop = 0, vbot = 0;
    int32_t ytop = 192, ybot = 0;
    int32_t xtop = 256, xbot = 0;
    uint32_t wsize = 0;

    for (int i = 0; i < nverts; i++)
    {
        NdsVertex* vtx = poly->vertices[i];

        if (vtx->final_position[1] < ytop)
        {
            xtop = vtx->final_position[0];
            ytop = vtx->final_position[1];
            vtop = i;
        }
        if (vtx->final_position[1] > ybot || (vtx->final_position[1] == ybot && vtx->final_position[0] > xbot))
        {
            xbot = vtx->final_position[0];
            ybot = vtx->final_position[1];
            vbot = i;
        }

        uint32_t w = (uint32_t)vtx->position[3];
        if (w == 0) poly->degenerate = true;

        while ((w >> wsize) && (wsize < 32))
            wsize += 4;
    }

    poly->v_top = vtop; poly->v_bottom = vbot;
    poly->y_top = ytop; poly->y_bottom = ybot;
    poly->x_top = xtop; poly->x_bottom = xbot;

    if (ybot > 192) poly->degenerate = true;

    poly->sort_key = (ybot << 8) | ytop;
    if (poly->translucent) poly->sort_key |= 0x10000;

    poly->w_buffer = (g->flush_attributes & 0x2);

    for (int i = 0; i < nverts; i++)
    {
        NdsVertex* vtx = poly->vertices[i];
        int32_t w, wshifted;

        // W is normalized, such that all the polygon's W values fit within 16 bits
        // the viewport transform for X/Y/Z uses the original W values, but
        // when W-buffering is used, the normalized W is used
        // W normalization is applied to separate polygons, even within strips

        if (wsize < 16)
        {
            w = vtx->position[3] << (16 - wsize);
            wshifted = w >> (16 - wsize);
        }
        else
        {
            w = vtx->position[3] >> (wsize - 16);
            wshifted = w << (wsize - 16);
        }

        int32_t z;
        if (g->flush_attributes & 0x2)
            z = wshifted;
        else if (vtx->position[3])
            z = ((((int64_t)vtx->position[2] * 0x4000) / vtx->position[3]) + 0x3FFF) * 0x200;
        else
            z = 0x7FFE00;

        // checkme (Z<0 shouldn't be possible, but Z>0xFFFFFF is possible)
        if (z < 0) z = 0;
        else if (z > 0xFFFFFF) z = 0xFFFFFF;

        poly->final_z[i] = z;
        poly->final_w[i] = w;
    }

    if (g->polygon_mode >= 2)
        g->last_strip_polygon = poly;
    else
        g->last_strip_polygon = NULL;
}

static void submit_vertex(NdsGpu3D* g)
{
    int64_t vertex[4] = {(int64_t)g->cur_vertex[0], (int64_t)g->cur_vertex[1], (int64_t)g->cur_vertex[2], 0x1000};
    NdsVertex* vertextrans = &g->temp_vertex_buffer[g->vertex_num_in_poly];

    update_clip_matrix(g);
    vertextrans->position[0] = (vertex[0]*g->clip_matrix[0] + vertex[1]*g->clip_matrix[4] + vertex[2]*g->clip_matrix[8] + vertex[3]*g->clip_matrix[12]) >> 12;
    vertextrans->position[1] = (vertex[0]*g->clip_matrix[1] + vertex[1]*g->clip_matrix[5] + vertex[2]*g->clip_matrix[9] + vertex[3]*g->clip_matrix[13]) >> 12;
    vertextrans->position[2] = (vertex[0]*g->clip_matrix[2] + vertex[1]*g->clip_matrix[6] + vertex[2]*g->clip_matrix[10] + vertex[3]*g->clip_matrix[14]) >> 12;
    vertextrans->position[3] = (vertex[0]*g->clip_matrix[3] + vertex[1]*g->clip_matrix[7] + vertex[2]*g->clip_matrix[11] + vertex[3]*g->clip_matrix[15]) >> 12;

    // this probably shouldn't be.
    // the way color is handled during clipping needs investigation. TODO
    vertextrans->color[0] = (g->vertex_color[0] << 12) + 0xFFF;
    vertextrans->color[1] = (g->vertex_color[1] << 12) + 0xFFF;
    vertextrans->color[2] = (g->vertex_color[2] << 12) + 0xFFF;

    if ((g->tex_param >> 30) == 3)
    {
        vertextrans->tex_coords[0] = ((vertex[0]*g->tex_matrix[0] + vertex[1]*g->tex_matrix[4] + vertex[2]*g->tex_matrix[8]) >> 24) + g->raw_tex_coords[0];
        vertextrans->tex_coords[1] = ((vertex[0]*g->tex_matrix[1] + vertex[1]*g->tex_matrix[5] + vertex[2]*g->tex_matrix[9]) >> 24) + g->raw_tex_coords[1];
    }
    else
    {
        vertextrans->tex_coords[0] = g->tex_coords[0];
        vertextrans->tex_coords[1] = g->tex_coords[1];
    }

    vertextrans->clipped = false;

    g->vertex_num++;
    g->vertex_num_in_poly++;

    switch (g->polygon_mode)
    {
    case 0: // triangle
        if (g->vertex_num_in_poly == 3)
        {
            g->vertex_num_in_poly = 0;
            submit_polygon(g);
            g->num_consecutive_polygons++;
        }
        break;

    case 1: // quad
        if (g->vertex_num_in_poly == 4)
        {
            g->vertex_num_in_poly = 0;
            submit_polygon(g);
            g->num_consecutive_polygons++;
        }
        break;

    case 2: // triangle strip
        if (g->num_consecutive_polygons & 1)
        {
            NdsVertex tmp = g->temp_vertex_buffer[1];
            g->temp_vertex_buffer[1] = g->temp_vertex_buffer[0];
            g->temp_vertex_buffer[0] = tmp;

            g->vertex_num_in_poly = 2;
            submit_polygon(g);
            g->num_consecutive_polygons++;

            g->temp_vertex_buffer[1] = g->temp_vertex_buffer[2];
        }
        else if (g->vertex_num_in_poly == 3)
        {
            g->vertex_num_in_poly = 2;
            submit_polygon(g);
            g->num_consecutive_polygons++;

            g->temp_vertex_buffer[0] = g->temp_vertex_buffer[1];
            g->temp_vertex_buffer[1] = g->temp_vertex_buffer[2];
        }
        break;

    case 3: // quad strip
        if (g->vertex_num_in_poly == 4)
        {
            NdsVertex tmp = g->temp_vertex_buffer[3];
            g->temp_vertex_buffer[3] = g->temp_vertex_buffer[2];
            g->temp_vertex_buffer[2] = tmp;

            g->vertex_num_in_poly = 2;
            submit_polygon(g);
            g->num_consecutive_polygons++;

            g->temp_vertex_buffer[0] = g->temp_vertex_buffer[3];
            g->temp_vertex_buffer[1] = g->temp_vertex_buffer[2];
        }
        break;
    }

}

static void calculate_lighting(NdsGpu3D* g)
{
    if ((g->tex_param >> 30) == 2)
    {
        g->tex_coords[0] = g->raw_tex_coords[0] + (((int64_t)g->normal[0]*g->tex_matrix[0] + (int64_t)g->normal[1]*g->tex_matrix[4] + (int64_t)g->normal[2]*g->tex_matrix[8]) >> 21);
        g->tex_coords[1] = g->raw_tex_coords[1] + (((int64_t)g->normal[0]*g->tex_matrix[1] + (int64_t)g->normal[1]*g->tex_matrix[5] + (int64_t)g->normal[2]*g->tex_matrix[9]) >> 21);
    }

    int32_t normaltrans[3]; // should be 1 bit sign 10 bits frac
    normaltrans[0] = ((g->normal[0]*g->vec_matrix[0] + g->normal[1]*g->vec_matrix[4] + g->normal[2]*g->vec_matrix[8]) << 9) >> 21;
    normaltrans[1] = ((g->normal[0]*g->vec_matrix[1] + g->normal[1]*g->vec_matrix[5] + g->normal[2]*g->vec_matrix[9]) << 9) >> 21;
    normaltrans[2] = ((g->normal[0]*g->vec_matrix[2] + g->normal[1]*g->vec_matrix[6] + g->normal[2]*g->vec_matrix[10]) << 9) >> 21;

    int32_t c = 0;
    uint32_t vtxbuff[3] =
    {
        (uint32_t)g->mat_emission[0] << 14,
        (uint32_t)g->mat_emission[1] << 14,
        (uint32_t)g->mat_emission[2] << 14
    };
    for (int i = 0; i < 4; i++)
    {
        if (!(g->cur_polygon_attr & (1<<i)))
            continue;

        // (credit to azusa for working out most of the details of the diff. algorithm, and essentially the entire spec. algorithm)
        
        // calculate dot product
        // bottom 9 bits are discarded after multiplying and before adding
        int32_t dot = ((g->light_direction[i][0]*normaltrans[0]) >> 9) +
                  ((g->light_direction[i][1]*normaltrans[1]) >> 9) +
                  ((g->light_direction[i][2]*normaltrans[2]) >> 9);

        int32_t shinelevel;
        if (dot > 0) 
        {
            // -- diffuse lighting --
            
            // convert dot to signed 11 bit int
            // then we truncate the result of the multiplications to an unsigned 20 bits before adding to the vtx color
            int32_t diffdot = (dot << 21) >> 21;
            vtxbuff[0] += (g->mat_diffuse[0] * g->light_color[i][0] * diffdot) & 0xFFFFF;
            vtxbuff[1] += (g->mat_diffuse[1] * g->light_color[i][1] * diffdot) & 0xFFFFF;
            vtxbuff[2] += (g->mat_diffuse[2] * g->light_color[i][2] * diffdot) & 0xFFFFF;

            // -- specular lighting --
        
            // reuse the dot product from diffuse lighting
            dot += normaltrans[2];

            // convert to s11, then square it, and truncate to 10 bits
            dot = (dot << 21) >> 21;
            dot = ((dot * dot) >> 10) & 0x3FF;

            // multiply dot and reciprocal, the subtract '1'
            shinelevel = ((dot * g->spec_recip[i]) >> 8) - (1<<9);

            if (shinelevel < 0) shinelevel = 0;
            else
            {
                // sign extend to convert to signed 14 bit integer
                shinelevel = (shinelevel << 18) >> 18;
                if (shinelevel < 0) shinelevel = 0; // for some reason there seems to be a redundant check for <0?
                else if (shinelevel > 0x1FF) shinelevel = 0x1FF;
            }
        }
        else shinelevel = 0;

        // convert shinelevel to use for lookup in the shininess table if enabled.
        if (g->use_shininess_table)
        {
            shinelevel >>= 2;
            shinelevel = g->shininess_table[shinelevel];
            shinelevel <<= 1;
        }

        // Note: ambient seems to be a plain bitshift
        vtxbuff[0] += ((g->mat_specular[0] * shinelevel) + (g->mat_ambient[0] << 9)) * g->light_color[i][0];
        vtxbuff[1] += ((g->mat_specular[1] * shinelevel) + (g->mat_ambient[1] << 9)) * g->light_color[i][1];
        vtxbuff[2] += ((g->mat_specular[2] * shinelevel) + (g->mat_ambient[2] << 9)) * g->light_color[i][2];

        c++;
    }

    g->vertex_color[0] = (vtxbuff[0] >> 14 > 31) ? 31 : (vtxbuff[0] >> 14);
    g->vertex_color[1] = (vtxbuff[1] >> 14 > 31) ? 31 : (vtxbuff[1] >> 14);
    g->vertex_color[2] = (vtxbuff[2] >> 14 > 31) ? 31 : (vtxbuff[2] >> 14);

    if (c < 1) c = 1;
}


static void box_test(NdsGpu3D* g, const uint32_t* params)
{
    NdsVertex cube[8];
    NdsVertex face[10];
    int res;

    g->gx_stat &= ~(1<<1);

    int16_t x0 = (int16_t)(params[0] & 0xFFFF);
    int16_t y0 = ((int32_t)params[0]) >> 16;
    int16_t z0 = (int16_t)(params[1] & 0xFFFF);
    int16_t x1 = ((int32_t)params[1]) >> 16;
    int16_t y1 = (int16_t)(params[2] & 0xFFFF);
    int16_t z1 = ((int32_t)params[2]) >> 16;

    x1 += x0;
    y1 += y0;
    z1 += z0;

    cube[0].position[0] = x0; cube[0].position[1] = y0; cube[0].position[2] = z0;
    cube[1].position[0] = x1; cube[1].position[1] = y0; cube[1].position[2] = z0;
    cube[2].position[0] = x1; cube[2].position[1] = y1; cube[2].position[2] = z0;
    cube[3].position[0] = x0; cube[3].position[1] = y1; cube[3].position[2] = z0;
    cube[4].position[0] = x0; cube[4].position[1] = y1; cube[4].position[2] = z1;
    cube[5].position[0] = x0; cube[5].position[1] = y0; cube[5].position[2] = z1;
    cube[6].position[0] = x1; cube[6].position[1] = y0; cube[6].position[2] = z1;
    cube[7].position[0] = x1; cube[7].position[1] = y1; cube[7].position[2] = z1;

    update_clip_matrix(g);
    for (int i = 0; i < 8; i++)
    {
        int32_t x = cube[i].position[0];
        int32_t y = cube[i].position[1];
        int32_t z = cube[i].position[2];

        cube[i].position[0] = ((int64_t)x*g->clip_matrix[0] + (int64_t)y*g->clip_matrix[4] + (int64_t)z*g->clip_matrix[8] + (int64_t)0x1000*g->clip_matrix[12]) >> 12;
        cube[i].position[1] = ((int64_t)x*g->clip_matrix[1] + (int64_t)y*g->clip_matrix[5] + (int64_t)z*g->clip_matrix[9] + (int64_t)0x1000*g->clip_matrix[13]) >> 12;
        cube[i].position[2] = ((int64_t)x*g->clip_matrix[2] + (int64_t)y*g->clip_matrix[6] + (int64_t)z*g->clip_matrix[10] + (int64_t)0x1000*g->clip_matrix[14]) >> 12;
        cube[i].position[3] = ((int64_t)x*g->clip_matrix[3] + (int64_t)y*g->clip_matrix[7] + (int64_t)z*g->clip_matrix[11] + (int64_t)0x1000*g->clip_matrix[15]) >> 12;
    }

    // front face (-Z)
    face[0] = cube[0]; face[1] = cube[1]; face[2] = cube[2]; face[3] = cube[3];
    res = clip_polygon(g, false, face, 4, 0);
    if (res > 0)
    {
        g->gx_stat |= (1<<1);
        return;
    }

    // back face (+Z)
    face[0] = cube[4]; face[1] = cube[5]; face[2] = cube[6]; face[3] = cube[7];
    res = clip_polygon(g, false, face, 4, 0);
    if (res > 0)
    {
        g->gx_stat |= (1<<1);
        return;
    }

    // left face (-X)
    face[0] = cube[0]; face[1] = cube[3]; face[2] = cube[4]; face[3] = cube[5];
    res = clip_polygon(g, false, face, 4, 0);
    if (res > 0)
    {
        g->gx_stat |= (1<<1);
        return;
    }

    // right face (+X)
    face[0] = cube[1]; face[1] = cube[2]; face[2] = cube[7]; face[3] = cube[6];
    res = clip_polygon(g, false, face, 4, 0);
    if (res > 0)
    {
        g->gx_stat |= (1<<1);
        return;
    }

    // bottom face (-Y)
    face[0] = cube[0]; face[1] = cube[1]; face[2] = cube[6]; face[3] = cube[5];
    res = clip_polygon(g, false, face, 4, 0);
    if (res > 0)
    {
        g->gx_stat |= (1<<1);
        return;
    }

    // top face (+Y)
    face[0] = cube[2]; face[1] = cube[3]; face[2] = cube[4]; face[3] = cube[7];
    res = clip_polygon(g, false, face, 4, 0);
    if (res > 0)
    {
        g->gx_stat |= (1<<1);
        return;
    }
}

static void pos_test(NdsGpu3D* g)
{
    int64_t vertex[4] = {(int64_t)g->cur_vertex[0], (int64_t)g->cur_vertex[1], (int64_t)g->cur_vertex[2], 0x1000};

    update_clip_matrix(g);
    g->pos_test_result[0] = (vertex[0]*g->clip_matrix[0] + vertex[1]*g->clip_matrix[4] + vertex[2]*g->clip_matrix[8] + vertex[3]*g->clip_matrix[12]) >> 12;
    g->pos_test_result[1] = (vertex[0]*g->clip_matrix[1] + vertex[1]*g->clip_matrix[5] + vertex[2]*g->clip_matrix[9] + vertex[3]*g->clip_matrix[13]) >> 12;
    g->pos_test_result[2] = (vertex[0]*g->clip_matrix[2] + vertex[1]*g->clip_matrix[6] + vertex[2]*g->clip_matrix[10] + vertex[3]*g->clip_matrix[14]) >> 12;
    g->pos_test_result[3] = (vertex[0]*g->clip_matrix[3] + vertex[1]*g->clip_matrix[7] + vertex[2]*g->clip_matrix[11] + vertex[3]*g->clip_matrix[15]) >> 12;

}

static void vec_test(NdsGpu3D* g, uint32_t param)
{
    // TODO: maybe it overwrites the normal registers, too

    int16_t normal[3];

    normal[0] = (int16_t)((param & 0x000003FF) << 6) >> 6;
    normal[1] = (int16_t)((param & 0x000FFC00) >> 4) >> 6;
    normal[2] = (int16_t)((param & 0x3FF00000) >> 14) >> 6;

    g->vec_test_result[0] = (normal[0]*g->vec_matrix[0] + normal[1]*g->vec_matrix[4] + normal[2]*g->vec_matrix[8]) >> 9;
    g->vec_test_result[1] = (normal[0]*g->vec_matrix[1] + normal[1]*g->vec_matrix[5] + normal[2]*g->vec_matrix[9]) >> 9;
    g->vec_test_result[2] = (normal[0]*g->vec_matrix[2] + normal[1]*g->vec_matrix[6] + normal[2]*g->vec_matrix[10]) >> 9;

    if (g->vec_test_result[0] & 0x1000) g->vec_test_result[0] |= 0xF000;
    if (g->vec_test_result[1] & 0x1000) g->vec_test_result[1] |= 0xF000;
    if (g->vec_test_result[2] & 0x1000) g->vec_test_result[2] |= 0xF000;

}


static void execute_command(NdsGpu3D* g, uint32_t command, uint32_t param)
{

    //printf("FIFO: processing %02X %08X. Levels: FIFO=%d, PIPE=%d\n", command, param, CmdFIFO->Level(), CmdPIPE->Level());

    // each FIFO entry takes 1 cycle to be processed
    // commands (presumably) run when all the needed parameters have been read
    // which is where we add the remaining cycles if any

    uint32_t paramsRequiredCount = command_params[command];
    if (paramsRequiredCount <= 1)
    {
        // fast path for command which only have a single parameter

        /*printf("[GXS:%08X] 0x%02X,  0x%08X", g->gx_stat, command, param);*/

        switch (command)
        {
        case 0x10: // matrix mode
            g->matrix_mode = param & 0x3;
            break;

        case 0x11: // push matrix
            if (g->matrix_mode == 0)
            {
                if (g->proj_matrix_stack_pointer > 0) g->gx_stat |= (1<<15);

                memcpy(g->proj_matrix_stack, g->proj_matrix, 16*4);
                g->proj_matrix_stack_pointer++;
                g->proj_matrix_stack_pointer &= 0x1;
            }
            else if (g->matrix_mode == 3)
            {
                if (g->tex_matrix_stack_pointer > 0) g->gx_stat |= (1<<15);

                memcpy(g->tex_matrix_stack, g->tex_matrix, 16*4);
                g->tex_matrix_stack_pointer++;
                g->tex_matrix_stack_pointer &= 0x1;
            }
            else
            {
                if (g->pos_matrix_stack_pointer > 30) g->gx_stat |= (1<<15);

                memcpy(g->pos_matrix_stack[g->pos_matrix_stack_pointer & 0x1F], g->pos_matrix, 16*4);
                memcpy(g->vec_matrix_stack[g->pos_matrix_stack_pointer & 0x1F], g->vec_matrix, 16*4);
                g->pos_matrix_stack_pointer++;
                g->pos_matrix_stack_pointer &= 0x3F;
            }
            break;

        case 0x12: // pop matrix
            if (g->matrix_mode == 0)
            {
                if (g->proj_matrix_stack_pointer == 0) g->gx_stat |= (1<<15);

                g->proj_matrix_stack_pointer--;
                g->proj_matrix_stack_pointer &= 0x1;
                memcpy(g->proj_matrix, g->proj_matrix_stack, 16*4);
                g->clip_matrix_dirty = true;
            }
            else if (g->matrix_mode == 3)
            {
                if (g->tex_matrix_stack_pointer == 0) g->gx_stat |= (1<<15);

                g->tex_matrix_stack_pointer--;
                g->tex_matrix_stack_pointer &= 0x1;
                memcpy(g->tex_matrix, g->tex_matrix_stack, 16*4);
            }
            else
            {
                int32_t offset = (int32_t)(param << 26) >> 26;
                g->pos_matrix_stack_pointer -= offset;
                g->pos_matrix_stack_pointer &= 0x3F;

                if (g->pos_matrix_stack_pointer > 30) g->gx_stat |= (1<<15);

                memcpy(g->pos_matrix, g->pos_matrix_stack[g->pos_matrix_stack_pointer & 0x1F], 16*4);
                memcpy(g->vec_matrix, g->vec_matrix_stack[g->pos_matrix_stack_pointer & 0x1F], 16*4);
                g->clip_matrix_dirty = true;
            }
            break;

        case 0x13: // store matrix
            if (g->matrix_mode == 0)
            {
                memcpy(g->proj_matrix_stack, g->proj_matrix, 16*4);
            }
            else if (g->matrix_mode == 3)
            {
                memcpy(g->tex_matrix_stack, g->tex_matrix, 16*4);
            }
            else
            {
                uint32_t addr = param & 0x1F;
                if (addr > 30) g->gx_stat |= (1<<15);

                memcpy(g->pos_matrix_stack[addr], g->pos_matrix, 16*4);
                memcpy(g->vec_matrix_stack[addr], g->vec_matrix, 16*4);
            }
            break;

        case 0x14: // restore matrix
            if (g->matrix_mode == 0)
            {
                memcpy(g->proj_matrix, g->proj_matrix_stack, 16*4);
                g->clip_matrix_dirty = true;
            }
            else if (g->matrix_mode == 3)
            {
                memcpy(g->tex_matrix, g->tex_matrix_stack, 16*4);
            }
            else
            {
                uint32_t addr = param & 0x1F;
                if (addr > 30) g->gx_stat |= (1<<15);

                memcpy(g->pos_matrix, g->pos_matrix_stack[addr], 16*4);
                memcpy(g->vec_matrix, g->vec_matrix_stack[addr], 16*4);
                g->clip_matrix_dirty = true;
            }
            break;

        case 0x15: // identity
            if (g->matrix_mode == 0)
            {
                matrix_load_identity(g->proj_matrix);
                g->clip_matrix_dirty = true;
            }
            else if (g->matrix_mode == 3)
                matrix_load_identity(g->tex_matrix);
            else
            {
                matrix_load_identity(g->pos_matrix);
                if (g->matrix_mode == 2)
                    matrix_load_identity(g->vec_matrix);
                g->clip_matrix_dirty = true;
            }
            break;

        case 0x20: // vertex color
            {
                uint32_t c = param;
                g->vertex_color[0] = c & 0x1F;
                g->vertex_color[1] = (c >> 5) & 0x1F;
                g->vertex_color[2] = (c >> 10) & 0x1F;
            }
            break;

        case 0x21: // normal
            g->normal[0] = (int16_t)((param & 0x000003FF) << 6) >> 6;
            g->normal[1] = (int16_t)((param & 0x000FFC00) >> 4) >> 6;
            g->normal[2] = (int16_t)((param & 0x3FF00000) >> 14) >> 6;
            calculate_lighting(g);
            break;

        case 0x22: // texcoord
            g->raw_tex_coords[0] = param & 0xFFFF;
            g->raw_tex_coords[1] = param >> 16;
            if ((g->tex_param >> 30) == 1)
            {
                g->tex_coords[0] = (g->raw_tex_coords[0]*g->tex_matrix[0] + g->raw_tex_coords[1]*g->tex_matrix[4] + g->tex_matrix[8] + g->tex_matrix[12]) >> 12;
                g->tex_coords[1] = (g->raw_tex_coords[0]*g->tex_matrix[1] + g->raw_tex_coords[1]*g->tex_matrix[5] + g->tex_matrix[9] + g->tex_matrix[13]) >> 12;
            }
            else
            {
                g->tex_coords[0] = g->raw_tex_coords[0];
                g->tex_coords[1] = g->raw_tex_coords[1];
            }
            break;

        case 0x24: // 10-bit vertex
            g->cur_vertex[0] = (param & 0x000003FF) << 6;
            g->cur_vertex[1] = (param & 0x000FFC00) >> 4;
            g->cur_vertex[2] = (param & 0x3FF00000) >> 14;
            submit_vertex(g);
            break;

        case 0x25: // vertex XY
            g->cur_vertex[0] = param & 0xFFFF;
            g->cur_vertex[1] = param >> 16;
            submit_vertex(g);
            break;

        case 0x26: // vertex XZ
            g->cur_vertex[0] = param & 0xFFFF;
            g->cur_vertex[2] = param >> 16;
            submit_vertex(g);
            break;

        case 0x27: // vertex YZ
            g->cur_vertex[1] = param & 0xFFFF;
            g->cur_vertex[2] = param >> 16;
            submit_vertex(g);
            break;

        case 0x28: // 10-bit delta vertex
            g->cur_vertex[0] += (int16_t)((param & 0x000003FF) << 6) >> 6;
            g->cur_vertex[1] += (int16_t)((param & 0x000FFC00) >> 4) >> 6;
            g->cur_vertex[2] += (int16_t)((param & 0x3FF00000) >> 14) >> 6;
            submit_vertex(g);
            break;

        case 0x29: // polygon attributes
            g->polygon_attr = param;
            break;

        case 0x2A: // texture param
            g->tex_param = param;
            break;

        case 0x2B: // texture palette
            g->tex_palette = param & 0x1FFF;
            break;

        case 0x30: // diffuse/ambient material
            g->mat_diffuse[0] = param & 0x1F;
            g->mat_diffuse[1] = (param >> 5) & 0x1F;
            g->mat_diffuse[2] = (param >> 10) & 0x1F;
            g->mat_ambient[0] = (param >> 16) & 0x1F;
            g->mat_ambient[1] = (param >> 21) & 0x1F;
            g->mat_ambient[2] = (param >> 26) & 0x1F;
            if (param & 0x8000)
            {
                g->vertex_color[0] = g->mat_diffuse[0];
                g->vertex_color[1] = g->mat_diffuse[1];
                g->vertex_color[2] = g->mat_diffuse[2];
            }
            break;

        case 0x31: // specular/emission material
            g->mat_specular[0] = param & 0x1F;
            g->mat_specular[1] = (param >> 5) & 0x1F;
            g->mat_specular[2] = (param >> 10) & 0x1F;
            g->mat_emission[0] = (param >> 16) & 0x1F;
            g->mat_emission[1] = (param >> 21) & 0x1F;
            g->mat_emission[2] = (param >> 26) & 0x1F;
            g->use_shininess_table = (param & 0x8000) != 0;
            break;

        case 0x32: // light direction
            {
                uint32_t l = param >> 30;
                int16_t dir[3];
                dir[0] = (int16_t)((param & 0x000003FF) << 6) >> 6;
                dir[1] = (int16_t)((param & 0x000FFC00) >> 4) >> 6;
                dir[2] = (int16_t)((param & 0x3FF00000) >> 14) >> 6;
                // the order of operations here is very specific: discard bottom 12 bits -> negate -> then sign extend to convert to 11 bit signed int
                // except for when used to calculate the specular reciprocal; then it's: sign extend -> discard lsb -> negate.
                g->light_direction[l][0] = (-((dir[0]*g->vec_matrix[0] + dir[1]*g->vec_matrix[4] + dir[2]*g->vec_matrix[8] ) >> 12) << 21) >> 21;
                g->light_direction[l][1] = (-((dir[0]*g->vec_matrix[1] + dir[1]*g->vec_matrix[5] + dir[2]*g->vec_matrix[9] ) >> 12) << 21) >> 21;
                g->light_direction[l][2] = (-((dir[0]*g->vec_matrix[2] + dir[1]*g->vec_matrix[6] + dir[2]*g->vec_matrix[10]) >> 12) << 21) >> 21;
                int32_t den =              -(((dir[0]*g->vec_matrix[2] + dir[1]*g->vec_matrix[6] + dir[2]*g->vec_matrix[10]) << 9) >> 21) + (1<<9);

                if (den == 0) g->spec_recip[l] = 0;
                else g->spec_recip[l] = (1<<18) / den;
            }
            break;

        case 0x33: // light color
            {
                uint32_t l = param >> 30;
                g->light_color[l][0] = param & 0x1F;
                g->light_color[l][1] = (param >> 5) & 0x1F;
                g->light_color[l][2] = (param >> 10) & 0x1F;
            }
            break;

        case 0x40: // begin polygons
            // TODO: check if there was a polygon being defined but incomplete
            // such cases seem to freeze the GPU
            g->polygon_mode = param & 0x3;
            g->vertex_num = 0;
            g->vertex_num_in_poly = 0;
            g->num_consecutive_polygons = 0;
            g->last_strip_polygon = NULL;
            g->cur_polygon_attr = g->polygon_attr;
            break;

        case 0x41: // end polygons
            // TODO: research this?
            // it doesn't seem to have any effect whatsoever, but
            // its timing characteristics are different from those of other
            // no-op commands
            break;

        case 0x50: // flush
            g->flush_request = 1;
            g->flush_attributes = param & 0x3;
            // probably safe to just reset all pipelines
            // but needs checked
            break;

        case 0x60: // viewport x1,y1,x2,y2
            // note: viewport Y coordinates are upside-down
            g->viewport[0] = param & 0xFF;                             // x0
            g->viewport[1] = (191 - ((param >> 8) & 0xFF)) & 0xFF;     // y0
            g->viewport[2] = (param >> 16) & 0xFF;                     // x1
            g->viewport[3] = (191 - (param >> 24)) & 0xFF;             // y1
            g->viewport[4] = (g->viewport[2] - g->viewport[0] + 1) & 0x1FF;          // width
            g->viewport[5] = (g->viewport[1] - g->viewport[3] + 1) & 0xFF;           // height
            break;

        case 0x72: // vec test
            vec_test(g, param);
            break;

        default:
            //printf("!! UNKNOWN GX COMMAND %02X %08X\n", command, param);
            break;
        }
    }
    else
    {
        g->exec_params[g->exec_param_count] = param;
        g->exec_param_count++;

        if (g->exec_param_count == 1)
        {
            // delay the first command entry as needed
            switch (command)
            {
            // commands that stall the polygon pipeline
            case 0x34:
            case 0x71:
                break;
            }
        }
        else
        {

            if (g->exec_param_count >= paramsRequiredCount)
            {
                /*printf("[GXS:%08X] 0x%02X,  ", g->gx_stat, command);
                for (int k = 0; k < g->exec_param_count; k++) printf("0x%08X, ", g->exec_params[k]);
                printf("\n");*/

                g->exec_param_count = 0;

                switch (command)
                {
                case 0x16: // load 4x4
                    if (g->matrix_mode == 0)
                    {
                        matrix_load4x4(g->proj_matrix, g->exec_params_s);
                        g->clip_matrix_dirty = true;
                    }
                    else if (g->matrix_mode == 3)
                    {
                        matrix_load4x4(g->tex_matrix, g->exec_params_s);
                    }
                    else
                    {
                        matrix_load4x4(g->pos_matrix, g->exec_params_s);
                        if (g->matrix_mode == 2)
                            matrix_load4x4(g->vec_matrix, g->exec_params_s);
                        g->clip_matrix_dirty = true;
                    }
                    break;

                case 0x17: // load 4x3
                    if (g->matrix_mode == 0)
                    {
                        matrix_load4x3(g->proj_matrix, g->exec_params_s);
                        g->clip_matrix_dirty = true;
                    }
                    else if (g->matrix_mode == 3)
                    {
                        matrix_load4x3(g->tex_matrix, g->exec_params_s);
                    }
                    else
                    {
                        matrix_load4x3(g->pos_matrix, g->exec_params_s);
                        if (g->matrix_mode == 2)
                            matrix_load4x3(g->vec_matrix, g->exec_params_s);
                        g->clip_matrix_dirty = true;
                    }
                    break;

                case 0x18: // mult 4x4
                    if (g->matrix_mode == 0)
                    {
                        matrix_mult4x4(g->proj_matrix, g->exec_params_s);
                        g->clip_matrix_dirty = true;
                    }
                    else if (g->matrix_mode == 3)
                    {
                        matrix_mult4x4(g->tex_matrix, g->exec_params_s);
                    }
                    else
                    {
                        matrix_mult4x4(g->pos_matrix, g->exec_params_s);
                        if (g->matrix_mode == 2)
                        {
                            matrix_mult4x4(g->vec_matrix, g->exec_params_s);
                        }
                        g->clip_matrix_dirty = true;
                    }
                    break;

                case 0x19: // mult 4x3
                    if (g->matrix_mode == 0)
                    {
                        matrix_mult4x3(g->proj_matrix, g->exec_params_s);
                        g->clip_matrix_dirty = true;
                    }
                    else if (g->matrix_mode == 3)
                    {
                        matrix_mult4x3(g->tex_matrix, g->exec_params_s);
                    }
                    else
                    {
                        matrix_mult4x3(g->pos_matrix, g->exec_params_s);
                        if (g->matrix_mode == 2)
                        {
                            matrix_mult4x3(g->vec_matrix, g->exec_params_s);
                        }
                        g->clip_matrix_dirty = true;
                    }
                    break;

                case 0x1A: // mult 3x3
                    if (g->matrix_mode == 0)
                    {
                        matrix_mult3x3(g->proj_matrix, g->exec_params_s);
                        g->clip_matrix_dirty = true;
                    }
                    else if (g->matrix_mode == 3)
                    {
                        matrix_mult3x3(g->tex_matrix, g->exec_params_s);
                    }
                    else
                    {
                        matrix_mult3x3(g->pos_matrix, g->exec_params_s);
                        if (g->matrix_mode == 2)
                        {
                            matrix_mult3x3(g->vec_matrix, g->exec_params_s);
                        }
                        g->clip_matrix_dirty = true;
                    }
                    break;

                case 0x1B: // scale
                    if (g->matrix_mode == 0)
                    {
                        matrix_scale(g->proj_matrix, g->exec_params_s);
                        g->clip_matrix_dirty = true;
                    }
                    else if (g->matrix_mode == 3)
                    {
                        matrix_scale(g->tex_matrix, g->exec_params_s);
                    }
                    else
                    {
                        matrix_scale(g->pos_matrix, g->exec_params_s);
                        g->clip_matrix_dirty = true;
                    }
                    break;

                case 0x1C: // translate
                    if (g->matrix_mode == 0)
                    {
                        matrix_translate(g->proj_matrix, g->exec_params_s);
                        g->clip_matrix_dirty = true;
                    }
                    else if (g->matrix_mode == 3)
                    {
                        matrix_translate(g->tex_matrix, g->exec_params_s);
                    }
                    else
                    {
                        matrix_translate(g->pos_matrix, g->exec_params_s);
                        if (g->matrix_mode == 2)
                        {
                            matrix_translate(g->vec_matrix, g->exec_params_s);
                        }
                        g->clip_matrix_dirty = true;
                    }
                    break;

                case 0x23: // full vertex
                    g->cur_vertex[0] = g->exec_params[0] & 0xFFFF;
                    g->cur_vertex[1] = g->exec_params[0] >> 16;
                    g->cur_vertex[2] = g->exec_params[1] & 0xFFFF;
                    submit_vertex(g);
                    break;

                case 0x34: // shininess table
                    {
                        for (int i = 0; i < 128; i += 4)
                        {
                            uint32_t val = g->exec_params[i >> 2];
                            g->shininess_table[i + 0] = val & 0xFF;
                            g->shininess_table[i + 1] = (val >> 8) & 0xFF;
                            g->shininess_table[i + 2] = (val >> 16) & 0xFF;
                            g->shininess_table[i + 3] = val >> 24;
                        }
                    }
                    break;

                case 0x71: // pos test
                    g->cur_vertex[0] = g->exec_params[0] & 0xFFFF;
                    g->cur_vertex[1] = g->exec_params[0] >> 16;
                    g->cur_vertex[2] = g->exec_params[1] & 0xFFFF;
                    pos_test(g);
                    break;

                case 0x70: // box test
                    box_test(g, g->exec_params);
                    break;

                default:
                    __builtin_unreachable();
                }
            }
        }
    }
}

/* a stable sort by the polygons' sort keys */
static void y_sort(NdsPolygon** list, uint32_t count)
{
    static NdsPolygon* temp[2048];
    for (uint32_t width = 1; width < count; width *= 2)
    {
        for (uint32_t at = 0; at < count; at += width * 2)
        {
            uint32_t mid = at + width < count ? at + width : count, end = at + width * 2 < count ? at + width * 2 : count;
            uint32_t a = at, b = mid, o = at;
            while (a < mid && b < end) temp[o++] = list[b]->sort_key < list[a]->sort_key ? list[b++] : list[a++];
            while (a < mid) temp[o++] = list[a++];
            while (b < end) temp[o++] = list[b++];
        }
        memcpy(list, temp, count * sizeof *list);
    }
}

static void v_blank(NdsGpu3D* g, bool geometry_enabled, bool rendering_enabled)
{
    if (geometry_enabled)
    {
        if (rendering_enabled)
        {
            if (g->flush_request)
            {
                if (g->num_polygons)
                {
                    // separate translucent polygons from opaque ones

                    uint32_t io = 0, it = g->num_opaque_polygons;
                    for (uint32_t i = 0; i < g->num_polygons; i++)
                    {
                        NdsPolygon* poly = &g->cur_polygon_ram[i];
                        if (poly->translucent)
                            g->render_polygon_ram[it++] = poly;
                        else
                            g->render_polygon_ram[io++] = poly;
                    }

                    /* Y-sorting: opaque polygons first, then by the lower bottom, then the lower top, then
                       as they came (the sort key has all three) */
                    y_sort(g->render_polygon_ram, (g->flush_attributes & 0x1) ? g->num_opaque_polygons : g->num_polygons);
                }

                g->render_num_polygons = g->num_polygons;
                g->render_frame_identical = false;
            }
            else
            {
                g->render_frame_identical = g->render_disp_cnt == g->disp_cnt
                    && g->render_alpha_ref == g->alpha_ref
                    && g->render_clear_attr1 == g->clear_attr1
                    && g->render_clear_attr2 == g->clear_attr2
                    && g->render_fog_color == g->fog_color
                    && g->render_fog_offset == g->fog_offset * 0x200
                    && memcmp(g->render_edge_table, g->edge_table, 8*2) == 0
                    && memcmp(g->render_fog_density_table + 1, g->fog_density_table, 32) == 0
                    && memcmp(g->render_toon_table, g->toon_table, 32*2) == 0;
            }

            g->render_disp_cnt = g->disp_cnt;
            g->render_alpha_ref = g->alpha_ref;

            memcpy(g->render_edge_table, g->edge_table, 8*2);
            memcpy(g->render_toon_table, g->toon_table, 32*2);

            g->render_fog_color = g->fog_color;
            g->render_fog_offset = g->fog_offset * 0x200;
            g->render_fog_shift = (g->render_disp_cnt >> 8) & 0xF;
            g->render_fog_density_table[0] = g->fog_density_table[0];
            memcpy(&g->render_fog_density_table[1], g->fog_density_table, 32);
            g->render_fog_density_table[33] = g->fog_density_table[31];

            g->render_clear_attr1 = g->clear_attr1;
            g->render_clear_attr2 = g->clear_attr2;
        }

        if (g->flush_request)
        {
            g->cur_ram_bank = g->cur_ram_bank?0:1;
            g->cur_vertex_ram = &g->vertex_ram[g->cur_ram_bank ? 6144 : 0];
            g->cur_polygon_ram = &g->polygon_ram[g->cur_ram_bank ? 2048 : 0];

            g->num_vertices = 0;
            g->num_polygons = 0;
            g->num_opaque_polygons = 0;

            g->flush_request = 0;
        }
    }
}



/* ---- the registers and the board's side ---- */

#define GEOMETRY_ON(board)  ((board)->powcnt & 8)
#define RENDERING_ON(board) ((board)->powcnt & 4)

/* the state and the registers of 0x04000330 to 0x040003BF as written */
typedef struct {
    NdsGpu3D state;
    uint8_t raw[0x90];
} Gpu3D;

static uint32_t fifo_level(const NdsGpu3D* g) { return g->waiting_count < 256 ? g->waiting_count : 256; }

bool nds_gpu3d_irq(NdsBoard* board)
{
    const NdsGpu3D* g = board->gpu3d;
    unsigned mode = g->gx_stat >> 30;
    uint32_t level = fifo_level(g);
    bool raised = (mode == 1 && level < 128) || (mode == 2 && level == 0);
    if (raised) nds_irq(board, NDS_IRQ_GXFIFO);
    return raised;
}

/* a command and one of its parameters: run now, or kept while the buffers wait to swap */
static void command_entry(NdsBoard* board, uint32_t command, uint32_t param)
{
    NdsGpu3D* g = board->gpu3d;
    if (!g->flush_request)
    {
        execute_command(g, command, param);
        return;
    }
    if (g->waiting_count == g->waiting_capacity)
    {
        uint32_t capacity = g->waiting_capacity ? g->waiting_capacity * 2 : 4096;
        NdsGxEntry* grown = realloc(g->waiting, capacity * sizeof *grown);
        if (!grown) return;
        g->waiting = grown, g->waiting_capacity = capacity;
    }
    g->waiting[g->waiting_count++] = (NdsGxEntry){param, (uint8_t)command};
}

/* a word at the FIFO's port: up to four packed commands, then their parameters */
static void write_gxfifo(NdsBoard* board, uint32_t val)
{
    NdsGpu3D* g = board->gpu3d;
    if (g->num_commands == 0)
    {
        g->num_commands = 4;
        g->cur_command = val;
        g->param_count = 0;
        g->total_params = command_params[g->cur_command & 0xFF];
        if (g->total_params > 0) return;
    }
    else
        g->param_count++;

    for (;;)
    {
        if ((g->cur_command & 0xFF) || (g->num_commands == 4 && g->cur_command == 0))
            command_entry(board, g->cur_command & 0xFF, val);

        if (g->param_count >= g->total_params)
        {
            g->cur_command >>= 8;
            g->num_commands--;
            if (g->num_commands == 0) break;

            g->param_count = 0;
            g->total_params = command_params[g->cur_command & 0xFF];
        }
        if (g->param_count < g->total_params)
            break;
    }
}

static void rendering_registers(NdsGpu3D* g, const uint8_t* raw)
{
#define RAW16(at) ((uint32_t)raw[(at) - 0x330] | (uint32_t)raw[(at) - 0x330 + 1] << 8)
#define RAW32(at) (RAW16(at) | RAW16((at) + 2) << 16)
    for (unsigned i = 0; i < 8; ++i) g->edge_table[i] = (uint16_t)RAW16(0x330 + i * 2);
    g->alpha_ref_val = raw[0x340 - 0x330] & 0x1F;
    g->alpha_ref = (g->disp_cnt & (1<<2)) ? g->alpha_ref_val : 0;
    g->clear_attr1 = RAW32(0x350);
    g->clear_attr2 = RAW32(0x354);
    g->fog_color = RAW32(0x358);
    g->fog_offset = RAW16(0x35C) & 0x7FFF;
    for (unsigned i = 0; i < 32; ++i) g->fog_density_table[i] = raw[0x360 - 0x330 + i] & 0x7F;
    for (unsigned i = 0; i < 32; ++i) g->toon_table[i] = (uint16_t)RAW16(0x380 + i * 2);
#undef RAW16
#undef RAW32
}

uint32_t nds_gpu3d_read(NdsBoard* board, uint32_t offset, unsigned bytes)
{
    NdsGpu3D* g = board->gpu3d;
    Gpu3D* whole = (Gpu3D*)g;
    uint32_t at = offset & ~3u, value = 0;
    if (offset >= 0x330 && offset < 0x3C0)
    {
        for (unsigned i = 0; i < bytes && offset + i < 0x3C0; ++i) value |= (uint32_t)whole->raw[offset - 0x330 + i] << (i * 8);
        return value;
    }
    switch (at)
    {
    case 0x060: value = g->disp_cnt; break;
    case 0x320: value = 46; break; /* the lines the rasterizer has drawn ahead: always the most */
    case 0x600:
        {
            uint32_t level = fifo_level(g);
            value = g->gx_stat |
                    ((g->pos_matrix_stack_pointer & 0x1F) << 8) |
                    ((g->proj_matrix_stack_pointer & 0x1) << 13) |
                    (level << 16) |
                    (level < 128 ? (1u<<25) : 0) |
                    (level == 0  ? (1u<<26) : 0) |
                    (g->flush_request ? (1u<<27) : 0); /* busy until the buffers swap */
        }
        break;
    case 0x604: value = g->num_polygons | (g->num_vertices << 16); break;
    case 0x620: case 0x624: case 0x628: case 0x62C: value = (uint32_t)g->pos_test_result[(at - 0x620) >> 2]; break;
    case 0x630: value = (uint16_t)g->vec_test_result[0] | (uint32_t)(uint16_t)g->vec_test_result[1] << 16; break;
    case 0x634: value = (uint16_t)g->vec_test_result[2]; break;
    default:
        if (at >= 0x640 && at < 0x680)
        {
            update_clip_matrix(g);
            value = (uint32_t)g->clip_matrix[(at & 0x3C) >> 2];
        }
        else if (at >= 0x680 && at < 0x6A4)
        {
            static const uint8_t place[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
            value = (uint32_t)g->vec_matrix[place[(at - 0x680) >> 2]];
        }
        break;
    }
    value >>= (offset & 3) * 8;
    return bytes == 4 ? value : bytes == 2 ? value & 0xFFFF : value & 0xFF;
}

void nds_gpu3d_write(NdsBoard* board, uint32_t offset, unsigned bytes, uint32_t val)
{
    NdsGpu3D* g = board->gpu3d;
    Gpu3D* whole = (Gpu3D*)g;
    if (offset == 0x60 || offset == 0x61)
    {
        if (offset == 0x61) val <<= 8, val |= g->disp_cnt & 0xFF;
        else if (bytes == 1) val |= g->disp_cnt & 0xFF00;
        g->disp_cnt = (val & 0x4FFF) | (g->disp_cnt & 0x3000);
        if (val & (1<<12)) g->disp_cnt &= ~(1u<<12);
        if (val & (1<<13)) g->disp_cnt &= ~(1u<<13);
        g->alpha_ref = (g->disp_cnt & (1<<2)) ? g->alpha_ref_val : 0;
        return;
    }
    if (offset < 0x320) return;
    if (!RENDERING_ON(board) && offset < 0x400) return;
    if (!GEOMETRY_ON(board) && offset >= 0x400) return;
    if (offset >= 0x330 && offset < 0x3C0)
    {
        for (unsigned i = 0; i < bytes && offset + i < 0x3C0; ++i) whole->raw[offset - 0x330 + i] = (uint8_t)(val >> (i * 8));
        rendering_registers(g, whole->raw);
        return;
    }
    if (offset >= 0x400 && offset < 0x440)
    {
        if (bytes == 4) write_gxfifo(board, val);
        return;
    }
    if (offset >= 0x440 && offset < 0x5CC)
    {
        if (bytes == 4) command_entry(board, (offset & 0x1FC) >> 2, val);
        return;
    }
    if (offset >= 0x600 && offset < 0x604)
    {
        uint32_t shift = (offset & 3) * 8, mask = (bytes == 4 ? 0xFFFFFFFFu : bytes == 2 ? 0xFFFFu : 0xFFu) << shift;
        val <<= shift;
        if (val & mask & 0x8000)
        {
            g->gx_stat &= ~0x8000u;
            g->proj_matrix_stack_pointer = 0;
            g->tex_matrix_stack_pointer = 0; // CHECKME
        }
        if (mask & 0xC0000000u)
        {
            g->gx_stat = (g->gx_stat & 0x3FFFFFFFu) | (val & 0xC0000000u);
            (void)nds_gpu3d_irq(board);
        }
        return;
    }
    if (offset == 0x610)
    {
        val &= 0x7FFF;
        g->zero_dot_w_limit = (val * 0x200) + 0x1FF;
        return;
    }
}

void nds_gpu3d_vblank(NdsBoard* board)
{
    NdsGpu3D* g = board->gpu3d;
    bool swapped = g->flush_request != 0;
    v_blank(g, GEOMETRY_ON(board) != 0, RENDERING_ON(board) != 0);
    if (swapped && !g->flush_request)
    {
        ++g->frames_swapped;
        /* the commands that waited, up to the next swap among them */
        uint32_t done = 0;
        while (done < g->waiting_count && !g->flush_request)
        {
            execute_command(g, g->waiting[done].command, g->waiting[done].param);
            ++done;
        }
        if (done) /* the queue is allocated at its first command: none ran, nothing to move */
        {
            memmove(g->waiting, g->waiting + done, (g->waiting_count - done) * sizeof *g->waiting);
            g->waiting_count -= done;
        }
    }
    (void)nds_gpu3d_irq(board);
}

const NdsGpu3D* nds_gpu3d_state(const NdsBoard* board) { return board->gpu3d; }

bool nds_gpu3d_init(NdsBoard* board)
{
    Gpu3D* whole = calloc(1, sizeof *whole);
    if (!whole) return false;
    NdsGpu3D* g = &whole->state;
    board->gpu3d = g;
    g->zero_dot_w_limit = 0xFFFFFF;
    matrix_load_identity(g->proj_matrix);
    matrix_load_identity(g->pos_matrix);
    matrix_load_identity(g->vec_matrix);
    matrix_load_identity(g->tex_matrix);
    g->clip_matrix_dirty = true;
    update_clip_matrix(g);
    whole->raw[0x353 - 0x330] = 0x3F; /* the clear's polygon ID */
    whole->raw[0x354 - 0x330] = 0xFF, whole->raw[0x355 - 0x330] = 0x7F;
    rendering_registers(g, whole->raw);
    g->render_clear_attr1 = 0x3F000000;
    g->render_clear_attr2 = 0x00007FFF;
    g->cur_vertex_ram = &g->vertex_ram[0];
    g->cur_polygon_ram = &g->polygon_ram[0];
    return true;
}

void nds_gpu3d_free(NdsBoard* board)
{
    if (board->gpu3d) free(board->gpu3d->waiting);
    free(board->gpu3d);
    board->gpu3d = NULL;
}
