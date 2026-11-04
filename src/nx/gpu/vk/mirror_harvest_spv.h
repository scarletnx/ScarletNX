/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU mirror harvest's SPIR-V (mirror_harvest.comp). The words are in mirror_harvest_spv.c, which the build makes
   from the shader with glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_MIRROR_HARVEST_SPV_H
#define SNX_MIRROR_HARVEST_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 mirror_harvest_spv[];
extern const u64 mirror_harvest_spv_size; /* in bytes */

#endif
