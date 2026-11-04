/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The HDR passes' SPIR-V, made by the build from gpu/vk/hdr/ with glslang (cmake/glsl.cmake): nothing compiled is
   checked in. */
#ifndef SNX_GPU_VK_HDR_SPV_H
#define SNX_GPU_VK_HDR_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 hdr_vert_spv[];
extern const u64 hdr_vert_spv_size;
extern const u32 hdr_compose_frag_spv[];
extern const u64 hdr_compose_frag_spv_size;
extern const u32 hdr_encode_frag_spv[];
extern const u64 hdr_encode_frag_spv_size;
extern const u32 hdr_test_frag_spv[];
extern const u64 hdr_test_frag_spv_size;
#endif
