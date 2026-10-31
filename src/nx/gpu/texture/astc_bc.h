/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The astc_bc setting (settings.txt): whether an ASTC texture the GPU decodes is encoded again as BC7, a quarter of
   RGBA8's memory, close to the game's blocks but not always the same (vk/astc.c) */
#ifndef SNX_TEXTURE_ASTC_BC_H
#define SNX_TEXTURE_ASTC_BC_H

/* its values, as settings.txt words them */
enum { SNX_ASTC_BC_AUTO, SNX_ASTC_BC_ON, SNX_ASTC_BC_OFF };
/* the images' budget Auto encodes again under, in MB: the only logs that ran past their budget were of 8 GB cards
   (budgets of 7079 to 7143 MB) */
#define SNX_ASTC_BC_AUTO_MB 8192
/* 1 where ASTC textures are encoded again as BC7, on a device that samples BCn (bc) but not ASTC (astc): with setting
   On, or Auto under SNX_ASTC_BC_AUTO_MB of budget_mb, the budget of the heap images take */
static inline int snx_astc_bc7(int setting, int bc, int astc, unsigned long long budget_mb) {
    if (!bc || astc || setting == SNX_ASTC_BC_OFF) return 0;
    return setting == SNX_ASTC_BC_ON || budget_mb < SNX_ASTC_BC_AUTO_MB;
}

#endif
