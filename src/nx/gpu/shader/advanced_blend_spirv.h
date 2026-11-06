/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ADVANCED_BLEND_SPIRV_H
#define SNX_ADVANCED_BLEND_SPIRV_H
#include "common/utilities/spirv_emit.h"
#include "common/cpu/aarch64/cpu.h"
/* Inline the selected published advanced equation into a fragment epilogue. The caller supplies ordered
   destination access, tests/discards, attachment conversion and component write masks. Return0 for invalid state. */
u32 shader_advanced_blend_spirv(SpvEmitter* e, u32 f32, u32 boolean, u32 vec4, u32 source, u32 destination, u32 mode,
                                int premultiplied, u32 overlap);
#endif
