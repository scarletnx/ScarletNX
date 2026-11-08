/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The write-back merge's SPIR-V (wlog_merge.comp). The words are in wlog_merge_spv.c, which the build makes from the
   shader with glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_WLOG_MERGE_SPV_H
#define SNX_WLOG_MERGE_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 wlog_merge_spv[];
extern const u64 wlog_merge_spv_size; /* in bytes */

#endif
