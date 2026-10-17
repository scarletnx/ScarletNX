/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/textures/astc.h):
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* ASTC decoding on the CPU (astc.c), the reference the tests check the compute shader (astc.comp) against */
#ifndef SNX_ASTC_H
#define SNX_ASTC_H
#include "common/cpu/aarch64/cpu.h"

/* a w x h image of bw x bh blocks (16 bytes each, rows of whole blocks, row after row) to RGBA8 pixels, w * 4
   bytes a row. Gives the bytes astc.comp writes: invalid blocks are transparent black and HDR values are clamped
   to [0, 1]. srgb nonzero decodes in the specification's sRGB mode, for SRGB images (LDR color endpoints expand
   as (e << 8) | 0x80, and R, G and B are the top 8 bits of the interpolated value) */
void astc_decode_image(const u8* src, u32 w, u32 h, u32 bw, u32 bh, u8* dst, int srgb);

#endif
