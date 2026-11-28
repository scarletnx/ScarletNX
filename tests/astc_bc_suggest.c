/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How the ASTC textures a GPU decodes are held (gpu/texture/astc_bc.h): astc_textures's BC7 (0, between frames) and BC7
   at load (2) as BC7, Exact (1) as RGBA8, from the start of the game to its end. Then when the log suggests BC7: only
   where Exact holds them as RGBA8, on a GPU that samples BCn but not ASTC, with ASTC textures decoded to RGBA8, and the
   images short of memory for one of its three reasons. Then the size from which a level held as BC7 at load is very
   large, from the encoder's time measured, and the encoder's queue. The budgets and times are given here, as
   vk_mem_use and the encoder's timestamps would give them */
#include <assert.h>
#include "nx/gpu/texture/astc_bc.h"
#define MB (1ull << 20)

int main(void) {
    int why;
    /* BC7 between frames and at load, from the start; Exact never; past 2 none of them */
    assert(snx_astc_bc7(0) && !snx_astc_bc7(1) && snx_astc_bc7(2) && !snx_astc_bc7(3) && !snx_astc_bc7(-1));
    for (why = SNX_ASTC_SHORT_NO_MEMORY; why <= SNX_ASTC_SHORT_WORKING_SET; why++) {
        assert(snx_astc_bc_suggest(0, 1, 0, 300 * MB, why));
        assert(!snx_astc_bc_suggest(1, 1, 0, 300 * MB, why)); /* held as BC7 already */
        assert(!snx_astc_bc_suggest(0, 1, 1, 300 * MB, why)); /* the GPU samples ASTC: nothing to encode again */
        assert(!snx_astc_bc_suggest(0, 0, 0, 300 * MB, why)); /* no BCn: BC7 can't be sampled */
        assert(!snx_astc_bc_suggest(0, 1, 0, 0, why));        /* no ASTC texture decoded: nothing it would hold */
    }
    assert(!snx_astc_bc_suggest(0, 1, 0, 300 * MB, SNX_ASTC_SHORT_NONE)); /* the images had room */
    /* very large levels: 0 is never, whatever was timed; the setting's megatexels until the fast build has been timed
       over as many texels; then the size the GPU encodes in the time the setting stands for at 25 ns a texel */
    assert(snx_astc_quick_texels(0, 0, 0) == 0 && snx_astc_quick_texels(0, 5 * (8 * MB), 8 * MB) == 0);
    assert(snx_astc_quick_texels(-1, 5 * (8 * MB), 8 * MB) == 0);
    assert(snx_astc_quick_texels(8, 0, 0) == 8 * MB);
    assert(snx_astc_quick_texels(8, 5 * (8 * MB - 1), 8 * MB - 1) == 8 * MB); /* a texel short of the start */
    assert(snx_astc_quick_texels(8, 0, 64 * MB) == 8 * MB);                   /* timed, but no time: as set */
    assert(snx_astc_quick_texels(8, 25 * (8 * MB), 8 * MB) == 8 * MB);        /* the 890M's own speed */
    assert(snx_astc_quick_texels(8, 5 * (8 * MB), 8 * MB) == 40 * MB);        /* five times faster: five times bigger */
    assert(snx_astc_quick_texels(8, 50 * (64 * MB), 64 * MB) == 4 * MB);      /* twice slower: half */
    assert(snx_astc_quick_texels(1, 100 * (3 * MB), 3 * MB) == MB / 4);
    /* sums of dispatches of unlike costs: 8 Mi texels at 2 ns and 8 at 8 ns are 5 ns a texel, 40 megatexels */
    assert(snx_astc_quick_texels(8, 2 * (8 * MB) + 8 * (8 * MB), 16 * MB) == 40 * MB);
    assert(snx_astc_quick_texels(4096, 1, 1ull << 40) == 9000000000000000000ull); /* bounded */
    assert(snx_astc_quick_texels(1, ~0ull, MB) == 1);                             /* at least a texel */
    /* the BC7 encoder's queue: the setting's thousands, else 256 thousand a 2 GiB of the GPU's own memory, from 256
       thousand (a GT 1030's 2 GB, or memory shared with the CPU) to 4 million (an RX 6700 XT's 12 GB gives 1.5) */
    assert(snx_bc7_queue_blocks(512, 12288 * MB, 0) == 512ull << 10);
    assert(snx_bc7_queue_blocks(0, 12288 * MB, 1) == 256ull << 10);
    assert(snx_bc7_queue_blocks(0, 2048 * MB, 0) == 256ull << 10 && snx_bc7_queue_blocks(0, 1024 * MB, 0) == 256ull
                                                                                                                 << 10);
    assert(snx_bc7_queue_blocks(0, 12288 * MB, 0) == 1536ull << 10);
    assert(snx_bc7_queue_blocks(0, 65536 * MB, 0) == 4096ull << 10);
    return 0;
}
