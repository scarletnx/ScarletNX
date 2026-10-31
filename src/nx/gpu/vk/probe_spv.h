/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Recommended Settings' compile load's SPIR-V (probe.comp). The words are in probe_spv.c, which the build makes from
   the shader with glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_VK_PROBE_SPV_H
#define SNX_VK_PROBE_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 probe_spv[];
extern const u64 probe_spv_size; /* in bytes */

#endif
