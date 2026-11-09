/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 encoder's SPIR-V (bc7.comp), its fast build (SNX_BC7_FAST 1, bc7_fast_spv), its build of mode 6
   alone (SNX_BC7_FAST 2, bc7_mode6_spv) and the quick one, mode 6 from one line (SNX_BC7_FAST 3, bc7_quick_spv). The
   words are in bc7_spv.c, which the build makes from the shader with glslang (cmake/glsl.cmake): nothing compiled is
   checked in. */
#ifndef SNX_BC7_SPV_H
#define SNX_BC7_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 bc7_spv[];
extern const u64 bc7_spv_size; /* in bytes */
extern const u32 bc7_fast_spv[];
extern const u64 bc7_fast_spv_size;
extern const u32 bc7_mode6_spv[];
extern const u64 bc7_mode6_spv_size;
extern const u32 bc7_quick_spv[];
extern const u64 bc7_quick_spv_size;

#endif
