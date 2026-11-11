/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen layer's shaders' SPIR-V (shaders/ui.vert and ui.frag, gpu/vk/ui.c). The words are in their .c files,
   which the build makes from the shaders with glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_UI_SPV_H
#define SNX_UI_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 snx_ui_vs_spv[];
extern const u64 snx_ui_vs_spv_size; /* in bytes, as each _size */
extern const u32 snx_ui_fs_spv[];
extern const u64 snx_ui_fs_spv_size;

#endif
