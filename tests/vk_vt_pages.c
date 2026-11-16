/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The calculations of the virtual textures (gpu/vk/vt_pages.c): a texture's blocks, the blocks a sample's mark names
   (under its cell, within a filter's reach, for a coordinate outside 0 to 1), a block's parent, and the age from which
   blocks go under a budget. */
#include "nx/gpu/vk/vt_pages.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int has(const u32* list, u32 n, u32 v) {
    for (u32 i = 0; i < n; i++)
        if (list[i] == v) return 1;
    return 0;
}
/* the block of level l at column x, row y of layer */
static u32 at(const VtShape* s, u32 layer, u32 l, u32 x, u32 y) {
    return layer * s->per_layer + s->first[l] + y * s->bx[l] + x;
}

int main(void) {
    /* a 4096x2048 texture of 13 levels and 2 layers in blocks of 256x256 (BC7's), its tail from level 5 (128x64):
       16x8, 8x4, 4x2, 2x1 and 1x1 blocks */
    VtShape s;
    assert(vt_shape(&s, 4096, 2048, 2, 13, 256, 256, 5));
    assert(s.bx[0] == 16 && s.by[0] == 8 && s.bx[3] == 2 && s.by[3] == 1 && s.bx[4] == 1 && s.by[4] == 1);
    assert(s.first[1] == 128 && s.first[2] == 160 && s.first[3] == 168 && s.first[4] == 170 && s.per_layer == 171);
    assert(!vt_shape(&s, 0, 8, 1, 1, 256, 256, 0) && !vt_shape(&s, 8, 8, 1, 17, 256, 256, 0));
    assert(vt_shape(&s, 4096, 2048, 2, 13, 256, 256, 5));

    u32 under[VT_MARK_BLOCKS], near[VT_MARK_BLOCKS], nu, nn;
    /* a cell in a block's middle: that block alone. Cell 40 of level 0 is texels 160 to 163, 32 of v 64 to 65 */
    VtMark m = {.level = 0, .layer = 1, .u = 40, .v = 32};
    vt_mark_blocks(&s, &m, under, &nu, near, &nn);
    assert(nu == 1 && under[0] == at(&s, 1, 0, 0, 0) && nn == 0);
    /* 16 texels from the block's right edge or less (texels 252 to 255): the block beside it is near */
    m = (VtMark){.level = 0, .layer = 0, .u = 63, .v = 32};
    vt_mark_blocks(&s, &m, under, &nu, near, &nn);
    assert(nu == 1 && under[0] == at(&s, 0, 0, 0, 0) && nn == 1 && near[0] == at(&s, 0, 0, 1, 0));
    /* texels 236 to 239 reach texel 255 and no further: 17 from the edge is alone again one cell before */
    m.u = 59;
    vt_mark_blocks(&s, &m, under, &nu, near, &nn);
    assert(nu == 1 && nn == 0);
    /* a corner: the three blocks around it */
    m = (VtMark){.level = 0, .layer = 0, .u = 64, .v = 128}; /* texels 256.., rows 256.. */
    vt_mark_blocks(&s, &m, under, &nu, near, &nn);
    assert(nu == 1 && under[0] == at(&s, 0, 0, 1, 1) && nn == 3 && has(near, nn, at(&s, 0, 0, 0, 1)) &&
           has(near, nn, at(&s, 0, 0, 1, 0)) && has(near, nn, at(&s, 0, 0, 0, 0)));
    /* the level's left edge: a repeating sampler's filter reads the right edge, whose block is near */
    m = (VtMark){.level = 0, .layer = 0, .u = 0, .v = 32};
    vt_mark_blocks(&s, &m, under, &nu, near, &nn);
    assert(nu == 1 && under[0] == at(&s, 0, 0, 0, 0) && nn == 1 && near[0] == at(&s, 0, 0, 15, 0));
    /* a coordinate over 1 whose fraction is cell 100 (texel 400): the wrapped cell's block, the mirrored cell's
       (923: texel 3692, block 14) and the right edge's (block 15) are under it, as the address mode is not known */
    m = (VtMark){.level = 0, .layer = 0, .u = 100, .v = 32, .uo = 2};
    vt_mark_blocks(&s, &m, under, &nu, near, &nn);
    assert(nu == 3 && has(under, nu, at(&s, 0, 0, 1, 0)) && has(under, nu, at(&s, 0, 0, 14, 0)) &&
           has(under, nu, at(&s, 0, 0, 15, 0)));
    assert(has(near, nn, at(&s, 0, 0, 0, 0)) && !has(near, nn, at(&s, 0, 0, 1, 0)));
    /* under 0: the left edge's */
    m.uo = 1;
    vt_mark_blocks(&s, &m, under, &nu, near, &nn);
    assert(nu == 3 && has(under, nu, at(&s, 0, 0, 0, 0)) && has(under, nu, at(&s, 0, 0, 1, 0)));
    /* level 4 is one block: any cell is it, and nothing is near. The tail and a layer too many name nothing */
    m = (VtMark){.level = 4, .layer = 0, .u = 1023, .v = 0};
    vt_mark_blocks(&s, &m, under, &nu, near, &nn);
    assert(nu == 1 && under[0] == at(&s, 0, 4, 0, 0) && nn == 0);
    m.level = 5;
    vt_mark_blocks(&s, &m, under, &nu, near, &nn);
    assert(nu == 0 && nn == 0);
    m = (VtMark){.level = 0, .layer = 2, .u = 1, .v = 1};
    vt_mark_blocks(&s, &m, under, &nu, near, &nn);
    assert(nu == 0 && nn == 0);

    /* a block's parent holds its middle: (5, 3) of level 0 is texel (1408, 896), (704, 448) of level 1: (2, 1) */
    assert(vt_block_parent(&s, at(&s, 1, 0, 5, 3)) == at(&s, 1, 1, 2, 1));
    assert(vt_block_parent(&s, at(&s, 0, 1, 7, 3)) == at(&s, 0, 2, 3, 1));
    assert(vt_block_parent(&s, at(&s, 0, 3, 1, 0)) == at(&s, 0, 4, 0, 0));
    assert(vt_block_parent(&s, at(&s, 0, 4, 0, 0)) == ~0u);
    /* every block's chain of parents ends in the tail within the levels */
    for (u32 b = 0; b < s.layers * s.per_layer; b++) {
        u32 p = b, steps = 0;
        while (p != ~0u) {
            assert(p < s.layers * s.per_layer && p / s.per_layer == b / s.per_layer && steps++ < s.tail_first);
            p = vt_block_parent(&s, p);
        }
    }
    /* a level whose size is no multiple of the block: 1000x600 in 128x128, 8x5 then 4x3 blocks, tail from 300x... */
    VtShape o;
    assert(vt_shape(&o, 1000, 600, 1, 10, 128, 128, 3));
    assert(o.bx[0] == 8 && o.by[0] == 5 && o.bx[1] == 4 && o.by[1] == 3 && o.bx[2] == 2 && o.by[2] == 2);
    assert(vt_block_parent(&o, at(&o, 0, 0, 7, 4)) == at(&o, 0, 1, 3, 2));
    m = (VtMark){.level = 0, .layer = 0, .u = 1023, .v = 1023}; /* the last texels: 999, 599 */
    vt_mark_blocks(&o, &m, under, &nu, near, &nn);
    assert(nu == 1 && under[0] == at(&o, 0, 0, 7, 4) && has(near, nn, at(&o, 0, 0, 0, 0)));

    /* ages: 5 held blocks seen 0, 1, 1, 40 and 2000 presents ago, one not held, one held and never seen */
    u32 hist[VT_AGES];
    const u32 now = 5000, seen[7] = {5000, 4999, 4999, 4960, 3000, 4990, 0};
    const u8 held = 0x5f; /* blocks 0 to 4 and 6 */
    memset(hist, 0, sizeof hist);
    vt_age_count(hist, seen, &held, 7, now);
    assert(hist[0] == 1 && hist[1] == 2 && hist[40] == 1 && hist[10] == 0 && hist[VT_AGES - 1] == 2);
    /* room for all 6: nothing goes. For 4: the two never seen or seen long ago. For 3: the one of age 40 too. For 2:
       the two of age 1 go together, as neither is older */
    assert(vt_age_cut(hist, 6, 0) == VT_AGES);
    assert(vt_age_cut(hist, 4, 0) == VT_AGES - 1);
    assert(vt_age_cut(hist, 3, 0) == 40);
    assert(vt_age_cut(hist, 2, 0) == 1);
    /* a floor of 32 presents keeps what was seen in them whatever the budget */
    assert(vt_age_cut(hist, 0, 32) == 32 && vt_age_cut(hist, 2, 32) == 32 && vt_age_cut(hist, 3, 32) == 40);
    printf("PASS vt pages: a mark's blocks under it and near it, across edges and outside 0 to 1; parents; ages\n");
    return 0;
}
