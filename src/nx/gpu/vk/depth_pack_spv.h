/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The depth and stencil packing shader's SPIR-V (depth_pack.comp). The words are in depth_pack_spv.c, which the build
   makes from the shader with glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_DEPTH_PACK_SPV_H
#define SNX_DEPTH_PACK_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 depth_pack_spv[];
extern const u64 depth_pack_spv_size; /* in bytes */

#endif
