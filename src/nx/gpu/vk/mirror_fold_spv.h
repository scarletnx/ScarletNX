/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU mirror fold's SPIR-V (mirror_fold.comp). The words are in mirror_fold_spv.c, which the build makes
   from the shader with glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_MIRROR_FOLD_SPV_H
#define SNX_MIRROR_FOLD_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 mirror_fold_spv[];
extern const u64 mirror_fold_spv_size; /* in bytes */

#endif
