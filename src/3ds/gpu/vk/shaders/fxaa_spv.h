/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* fxaa.comp's SPIR-V. The words are in fxaa_spv.c, which the build makes from the shader with glslang
   (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_CTR_FXAA_SPV_H
#define SNX_CTR_FXAA_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 snx_ctr_fxaa_spv[];
extern const u64 snx_ctr_fxaa_spv_size; /* in bytes */

#endif
