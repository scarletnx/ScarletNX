/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* compose.frag's SPIR-V. The words are in android_compose_frag_spv.c, which the build makes from the shader with
   glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_ANDROID_COMPOSE_FRAG_SPV_H
#define SNX_ANDROID_COMPOSE_FRAG_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 android_compose_frag_spv[];
extern const u64 android_compose_frag_spv_size; /* in bytes */

#endif
