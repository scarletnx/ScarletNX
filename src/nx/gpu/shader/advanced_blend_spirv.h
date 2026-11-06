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
    SH_ALPHA_COVERAGE = 1u << 14,
    SH_CAPTURE_SOURCE = 1u << 15,
    SH_CAPTURE_TILED = 1u << 16,
    SH_ALPHA_TO_ONE = 1u << 18, /* replace float output alpha after software alpha coverage uses its original value */
    SH_COVERAGE_TO_COLOR = 1u << 20,
    SH_COVERAGE_OUTPUT_SHIFT = 21
};
/* Inline the selected published advanced equation into a fragment epilogue. The caller supplies ordered
   destination access, tests/discards, attachment conversion and component write masks. Return0 for invalid state. */
u32 shader_advanced_blend_spirv(SpvEmitter* e, u32 f32, u32 boolean, u32 vec4, u32 source, u32 destination, u32 mode,
                                int premultiplied, u32 overlap);
/* Convert RGB between stored sRGB and linear blending values. Alpha remains linear in both directions. */
u32 shader_blend_srgb(SpvEmitter* e, u32 f32, u32 boolean, u32 vec4, u32 color, int encode);
/* Resolve one primitive's surviving samples after native late depth/stencil tests. Set0 bindings0/1/2 are the
   captured source, a separate validity image (red0 or1), and the real destination. Images are float4 storage
   views of the same 2D/array/MS shape. Dispatch ceil(width/8),ceil(height/8),layers. Keep the primitive's guest
   shader and side effects in the capture pass only, and resolve before capturing the next primitive. */
u32* shader_blend_resolve_spirv(u64 packed, u32 flags, size_t* words);
#endif
