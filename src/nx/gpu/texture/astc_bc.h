/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* When the log suggests astc_mode=0 (settings.txt), the default, which encodes the ASTC textures a GPU decodes again as
   BC7, a quarter of RGBA8's memory, close to the game's blocks but not the same: to a player who chose Exact */
#ifndef SNX_TEXTURE_ASTC_BC_H
#define SNX_TEXTURE_ASTC_BC_H

/* why the images ran short of memory (vk_mem_trim): an image allocation found none, a trim left the caches past their
   budget, or a trim took textures or targets the last two presents used, the working set, which the next frames load
   again: the game held slow by eviction */
enum { SNX_ASTC_SHORT_NONE, SNX_ASTC_SHORT_NO_MEMORY, SNX_ASTC_SHORT_PAST_BUDGET, SNX_ASTC_SHORT_WORKING_SET };
/* 1 where the log suggests astc_mode=0: BC7 is off (setting 0), the GPU samples BCn (bc) but not ASTC (astc), ASTC
   textures were decoded to RGBA8 (astc_bytes), and the images ran short (why) */
static inline int snx_astc_bc_suggest(int setting, int bc, int astc, unsigned long long astc_bytes, int why) {
    return why != SNX_ASTC_SHORT_NONE && !setting && bc && !astc && astc_bytes;
}

#endif
