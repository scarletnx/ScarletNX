/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The samples-passed counts shader's SPIR-V (query_count.comp). The words are in query_count_spv.c, which the build
   makes from the shader with glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_QUERY_COUNT_SPV_H
#define SNX_QUERY_COUNT_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 query_count_spv[];
extern const u64 query_count_spv_size; /* in bytes */

#endif
