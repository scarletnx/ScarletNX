/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ADVANCED_BLEND_SPIRV_H
#define SNX_ADVANCED_BLEND_SPIRV_H
#include "common/utilities/spirv_emit.h"
#include "common/cpu/aarch64/cpu.h"
/* The low nibble remains the component write mask. The other bits select storage-image interfaces and tests. */
enum {
    SH_BLEND_MASK = 15,
    SH_BLEND_SRGB = 1u << 4,
    SH_BLEND_ARRAY = 1u << 5,
    SH_BLEND_MULTISAMPLE = 1u << 6,
    SH_BLEND_EARLY_TESTS = 1u << 7,
    SH_COVERAGE_TO_COLOR = 1u << 20,
    SH_COVERAGE_OUTPUT_SHIFT = 21
};
/* Inline the selected published advanced equation into a fragment epilogue. The caller supplies ordered
   destination access, tests/discards, attachment conversion and component write masks. Return0 for invalid state. */
u32 shader_advanced_blend_spirv(SpvEmitter* e, u32 f32, u32 boolean, u32 vec4, u32 source, u32 destination, u32 mode,
                                int premultiplied, u32 overlap);
/* Convert RGB between stored sRGB and linear blending values. Alpha remains linear in both directions. */
u32 shader_blend_srgb(SpvEmitter* e, u32 f32, u32 boolean, u32 vec4, u32 color, int encode);
#endif
