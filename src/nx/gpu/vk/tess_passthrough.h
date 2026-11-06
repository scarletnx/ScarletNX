/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_TESS_PASSTHROUGH_H
#define SNX_VK_TESS_PASSTHROUGH_H
#include "nx/gpu/vk/vk.h"
/* A control shader for an evaluation-only pipeline, preserving position and float vec4 attributes. A VS with other
   written builtins is refused until those builtins can be forwarded with their exact interface. */
VkSh* vk_tess_passthrough(u32 vertices, const VkSh* vertex, const VkSh* evaluation);
/* Pure emitter for validation by synthetic tests, with no driver or guest memory access. */
u32* vk_tess_passthrough_spirv(u32 vertices, u32 attributes, size_t* words);
u32* vk_tess_passthrough_spirv_full(u32 vertices, u32 attributes, int point_size, u32 clip_mask, size_t* words);
/* Call after pipeline work has stopped, before destroying the device. */
void vk_tess_passthrough_free(void);
#endif
