/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The multisample moves' shaders' SPIR-V (msaa.vert, and msaa.frag built three ways). The words are in their .c
   files, which the build makes from the shaders with glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_MSAA_SPV_H
#define SNX_MSAA_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 msaa_vert_spv[];
extern const u64 msaa_vert_spv_size; /* in bytes, as each _size */
extern const u32 msaa_unpack_spv[];
extern const u64 msaa_unpack_spv_size;
extern const u32 msaa_unpack_depth_spv[];
extern const u64 msaa_unpack_depth_spv_size;
extern const u32 msaa_pack_spv[];
extern const u64 msaa_pack_spv_size;

#endif
