/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_DEPTH_SHADOW_H
#define SNX_VK_DEPTH_SHADOW_H
#include "nx/gpu/vk/vk.h"

/* These calls do no CPU readback. Prepare imports raw native D32F/S8 aspects into a device-local SSBO and stages
   the immutable per-draw header. The renderer retains the original native depth attachment but disables its
   tests/writes during one ordered-interlock draw. Initial caller gates: one viewport, no depth clamp, no guest
   FragDepth replacement, safe native depth range/bias, no depth feedback or queries, and one sample/layer.
   Reserve fixes the buffer/range before descriptor creation; prepare later imports into that same reservation.
   Call finish after every recorded draw. Cancel is only for setup failure/retry before the original draw.
   The descriptor is valid through finish; finish must precede another prepare or any observation of the surface. */
int vk_depth_shadow_reserve(Surf* surface, VkDescriptorBufferInfo* descriptor);
/* Metadata-only proof: true for an alias or unknown/noncontiguous guest backing. Includes texture mip spans,
   render-target padding/layers, GMMU synonyms and CPU mappings of shared physical pages. */
int vk_depth_shadow_aliases(const Surf* depth, const Surf* other);
/* The same bounded proof for guest storage-buffer and transform-feedback ranges. Unknown ranges reject. */
int vk_depth_shadow_range_aliases(const Surf* depth, u64 gva, u64 bytes);
int vk_depth_shadow_prepare(Surf* surface, const DsDynamic* dynamic, const DrawCall* call,
                              u32 fixed_mask, VkDescriptorBufferInfo* descriptor);
int vk_depth_shadow_finish(Surf* surface);
void vk_depth_shadow_cancel(void);
/* The device is idle before destruction. Replaced buffers use the normal deferred frame lifetime. */
void vk_depth_shadow_destroy(void);
#endif
