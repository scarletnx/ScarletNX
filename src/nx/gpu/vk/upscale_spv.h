/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The spatial upscalers' SPIR-V, made by the build from gpu/vk/upscale/ with glslang (cmake/glsl.cmake): nothing
   compiled is checked in. nis_spv writes RGBA16F and nis8_spv RGBA8, the fallback where the device has no 16-bit
   storage image. */
#ifndef SNX_GPU_VK_UPSCALE_SPV_H
#define SNX_GPU_VK_UPSCALE_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 fsr_vert_spv[];
extern const u64 fsr_vert_spv_size;
extern const u32 fsr_easu_spv[];
extern const u64 fsr_easu_spv_size;
extern const u32 fsr_rcas_spv[];
extern const u64 fsr_rcas_spv_size;
extern const u32 fsr_easu_fp16_spv[];
extern const u64 fsr_easu_fp16_spv_size;
extern const u32 fsr_rcas_fp16_spv[];
extern const u64 fsr_rcas_fp16_spv_size;
extern const u32 nis_spv[];
extern const u64 nis_spv_size;
extern const u32 nis8_spv[];
extern const u64 nis8_spv_size;
extern const u32 sgsr_vert_spv[];
extern const u64 sgsr_vert_spv_size;
extern const u32 sgsr_frag_spv[];
extern const u64 sgsr_frag_spv_size;
#endif
