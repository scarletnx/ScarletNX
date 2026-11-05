/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* When the log suggests astc_mode=0 (gpu/texture/astc_bc.h): only with BC7 off (astc_mode=1, Exact), on a GPU that
   samples BCn but not ASTC, with ASTC textures decoded to RGBA8, and the images short of memory for one of its three
   reasons */
#include <assert.h>
#include "nx/gpu/texture/astc_bc.h"
#define MB (1ull << 20)
int main(void) {
    int why;
    for (why = SNX_ASTC_SHORT_NO_MEMORY; why <= SNX_ASTC_SHORT_WORKING_SET; why++) {
        assert(snx_astc_bc_suggest(0, 1, 0, 300 * MB, why));
        assert(!snx_astc_bc_suggest(1, 1, 0, 300 * MB, why)); /* on already */
        assert(!snx_astc_bc_suggest(0, 1, 1, 300 * MB, why)); /* the GPU samples ASTC: nothing to encode again */
        assert(!snx_astc_bc_suggest(0, 0, 0, 300 * MB, why)); /* no BCn: BC7 can't be sampled */
        assert(!snx_astc_bc_suggest(0, 1, 0, 0, why));        /* no ASTC texture decoded: nothing it would hold */
    }
    assert(!snx_astc_bc_suggest(0, 1, 0, 300 * MB, SNX_ASTC_SHORT_NONE)); /* the images had room */
    return 0;
}
