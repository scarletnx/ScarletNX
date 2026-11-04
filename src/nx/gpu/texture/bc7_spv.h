/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 encoder's SPIR-V (bc7.comp), and its fast build (SNX_BC7_FAST, bc7_fast_spv). The words are in bc7_spv.c, which the build makes from the shader with
   glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_BC7_SPV_H
#define SNX_BC7_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 bc7_spv[];
extern const u64 bc7_spv_size; /* in bytes */
extern const u32 bc7_fast_spv[];
extern const u64 bc7_fast_spv_size;

#endif
