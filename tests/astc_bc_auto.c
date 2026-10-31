/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* astc_bc's choice (texture/astc_bc.h): Auto on both sides of its budget, On and Off, and the GPUs where neither
   applies */
#include "nx/gpu/texture/astc_bc.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    int s;
    /* the values settings.txt words Auto|On|Off */
    assert(SNX_ASTC_BC_AUTO == 0 && SNX_ASTC_BC_ON == 1 && SNX_ASTC_BC_OFF == 2);
    /* Auto: an 8 GB card's budget encodes again, 8192 MB and up stays exact */
    assert(snx_astc_bc7(SNX_ASTC_BC_AUTO, 1, 0, 7079) && snx_astc_bc7(SNX_ASTC_BC_AUTO, 1, 0, 8191));
    assert(!snx_astc_bc7(SNX_ASTC_BC_AUTO, 1, 0, 8192) && !snx_astc_bc7(SNX_ASTC_BC_AUTO, 1, 0, 11490));
    /* On whatever the budget, Off never */
    assert(snx_astc_bc7(SNX_ASTC_BC_ON, 1, 0, 24000) && !snx_astc_bc7(SNX_ASTC_BC_OFF, 1, 0, 2000));
    /* a GPU that samples ASTC, or no BCn, keeps its ASTC as it is or as RGBA8 */
    for (s = SNX_ASTC_BC_AUTO; s <= SNX_ASTC_BC_OFF; ++s)
        assert(!snx_astc_bc7(s, 1, 1, 2000) && !snx_astc_bc7(s, 0, 0, 2000));
    puts("astc_bc_auto passed");
    return 0;
}
