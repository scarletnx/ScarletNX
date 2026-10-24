/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The texture check's shaders' SPIR-V (texcheck.comp, and texstore.comp built for array images and, -DVOLUME, for 3D
   ones). The words are in texcheck_spv.c, texstore_spv.c and texstore3d_spv.c, which the build makes from the shaders
   with glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_TEXCHECK_SPV_H
#define SNX_TEXCHECK_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 texcheck_spv[];
extern const u64 texcheck_spv_size; /* in bytes */
extern const u32 texstore_spv[];
extern const u64 texstore_spv_size;
extern const u32 texstore3d_spv[];
extern const u64 texstore3d_spv_size;

#endif
