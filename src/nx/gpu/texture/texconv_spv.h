/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The texture conversions' SPIR-V (texconv.comp), its ETC2 build's (SNX_TC_ETC2: BCn encoded again as ETC2 and EAC)
   and its ASTC build's (SNX_TC_ASTC: BC2, BC3 and BC7 as ASTC 4x4). The words are in texconv_spv.c,
   texconv_etc2_spv.c and texconv_astc_spv.c, which the build makes from the shader with glslang (cmake/glsl.cmake):
   nothing compiled is checked in. */
#ifndef SNX_TEXCONV_SPV_H
#define SNX_TEXCONV_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 texconv_spv[];
extern const u64 texconv_spv_size; /* in bytes */
extern const u32 texconv_etc2_spv[];
extern const u64 texconv_etc2_spv_size; /* in bytes */
extern const u32 texconv_astc_spv[];
extern const u64 texconv_astc_spv_size; /* in bytes */

#endif
