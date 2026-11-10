/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_VARYING_GEOMETRY_H
#define SNX_VK_VARYING_GEOMETRY_H
#include "nx/gpu/vk/vk.h"
/* The geometry program of a per-vertex pass on a device without VK_KHR_fragment_shader_barycentric
   (shader.h, SH_VARYING_GEOMETRY), between the draw's vertex program and its fragment program's variant: per_vertex
   the generics the pass takes, slot the first of the position's three. NULL when the device can't take its interface
   or the module isn't made */
VkSh* vk_varying_geometry(const VkSh* vertex, u32 per_vertex, u32 slot);
/* Its module alone, for the generics outputs: malloc'd words, NULL for generics that don't fit */
u32* vk_varying_geometry_spirv(u32 outputs, u32 per_vertex, u32 slot, size_t* words);
#endif
