/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The presentation effects' SPIR-V, made by the build from gpu/vk/fx/ with glslang (cmake/glsl.cmake): nothing
   compiled is checked in. */
#ifndef SNX_COMMON_GPU_VK_FX_SPV_H
#define SNX_COMMON_GPU_VK_FX_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 fx_fsr_vert_spv[];
extern const u64 fx_fsr_vert_spv_size;
extern const u32 fx_copy_spv[];
extern const u64 fx_copy_spv_size;
extern const u32 fx_copy_linear_spv[];
extern const u64 fx_copy_linear_spv_size;
extern const u32 fx_fsr_easu_spv[];
extern const u64 fx_fsr_easu_spv_size;
extern const u32 fx_fsr_rcas_spv[];
extern const u64 fx_fsr_rcas_spv_size;
extern const u32 fx_fsr_easu_fp16_spv[];
extern const u64 fx_fsr_easu_fp16_spv_size;
extern const u32 fx_fsr_rcas_fp16_spv[];
extern const u64 fx_fsr_rcas_fp16_spv_size;
extern const u32 fx_fxaa_spv[];
extern const u64 fx_fxaa_spv_size;
extern const u32 fx_smaa_edge_spv[];
extern const u64 fx_smaa_edge_spv_size;
extern const u32 fx_smaa_weights_spv[];
extern const u64 fx_smaa_weights_spv_size;
extern const u32 fx_smaa_blend_spv[];
extern const u64 fx_smaa_blend_spv_size;
extern const u32 fx_hdr_vert_spv[];
extern const u64 fx_hdr_vert_spv_size;
extern const u32 fx_hdr_compose_spv[];
extern const u64 fx_hdr_compose_spv_size;
extern const u32 fx_hdr_encode_spv[];
extern const u64 fx_hdr_encode_spv_size;
extern const u32 fx_hdr_test_spv[];
extern const u64 fx_hdr_test_spv_size;
#endif
