/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How the ASTC textures a GPU decodes are held (gpu/texture/astc_bc.h): astc_mode's Exact (1) as RGBA8, BC7 (2, 3) as
   BC7, Auto (0) as BC7 under 8 GiB of budget as the device starts and as RGBA8 from there. Then when the log suggests
   BC7: only where they are held as RGBA8, on a GPU that samples BCn but not ASTC, with ASTC textures decoded to RGBA8,
   and the images short of memory for one of its three reasons */
#include <assert.h>
#include "nx/gpu/texture/astc_bc.h"
#define MB (1ull << 20)
int main(void) {
    int why;
    /* Auto's line, 8192 MiB: a byte under it is BC7, the line and past it Exact (an RX 6700 XT's 11.8 GB) */
    assert(snx_astc_auto_bc7(8192 * MB - 1) && !snx_astc_auto_bc7(8192 * MB) && !snx_astc_auto_bc7(12100 * MB));
    assert(snx_astc_auto_bc7(5800 * MB) && snx_astc_auto_bc7(0));
    /* Exact never, BC7 always (2 between frames, 3 at load), Auto as the budget at start said; past 3 none of them */
    assert(!snx_astc_bc7(1, 0) && !snx_astc_bc7(1, 1));
    assert(snx_astc_bc7(2, 0) && snx_astc_bc7(2, 1));
    assert(!snx_astc_bc7(0, 0) && snx_astc_bc7(0, 1));
    assert(snx_astc_bc7(3, 0) && snx_astc_bc7(3, 1) && !snx_astc_bc7(4, 1));
    for (why = SNX_ASTC_SHORT_NO_MEMORY; why <= SNX_ASTC_SHORT_WORKING_SET; why++) {
        assert(snx_astc_bc_suggest(0, 1, 0, 300 * MB, why));
        assert(!snx_astc_bc_suggest(1, 1, 0, 300 * MB, why)); /* held as BC7 already */
        assert(!snx_astc_bc_suggest(0, 1, 1, 300 * MB, why)); /* the GPU samples ASTC: nothing to encode again */
        assert(!snx_astc_bc_suggest(0, 0, 0, 300 * MB, why)); /* no BCn: BC7 can't be sampled */
        assert(!snx_astc_bc_suggest(0, 1, 0, 0, why));        /* no ASTC texture decoded: nothing it would hold */
    }
    assert(!snx_astc_bc_suggest(0, 1, 0, 300 * MB, SNX_ASTC_SHORT_NONE)); /* the images had room */
    return 0;
}
