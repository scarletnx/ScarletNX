/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shared fonts' stand-ins, given the private-use characters the console's fonts carry (pl_font.c) */
#ifndef SNX_PL_FONT_H
#define SNX_PL_FONT_H
#include "common/cpu/aarch64/cpu.h"

/* font with a cmap that also answers for the console shared fonts' private-use characters, U+E000 to U+E1FF, each
   to the font's space glyph: a malloc'd TrueType font, its size in *out, or NULL when font isn't one this can read
   (the caller keeps font). The stand-ins are the Noto subsets (docs/platforms/nx/filesystem.md) and yuzu's button glyphs (pl.c) */
u8* pl_font_cover(const u8* font, u64 size, u64* out);

#endif
