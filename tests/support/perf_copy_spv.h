/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The reference shader's SPIR-V (tests/perf_copy.comp), which the build makes with glslang (cmake/glsl.cmake) */
#ifndef SNX_TEST_PERF_COPY_SPV_H
#define SNX_TEST_PERF_COPY_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 perf_copy_spv[];
extern const u64 perf_copy_spv_size; /* in bytes */

#endif
