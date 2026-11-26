/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Graphics pipeline libraries (pipeline_libraries, VK_EXT_graphics_pipeline_library): a pipeline linked from four
   parts that each compile once, the vertex input, the pre-rasterization shaders, the fragment shader and the fragment
   output, fast where the driver links them so, and optimized after. What every renderer that makes them shares, the
   Switch's (nx/gpu/vk/pipeline.c) and the consoles': whether a device makes them, a part's request and a link's */
#ifndef SNX_COMMON_VK_GPL_H
#define SNX_COMMON_VK_GPL_H
#include "common/gpu/vk/vk_common.h"
#include <stdbool.h>
#include <stddef.h>

/* why a device makes no pipeline from libraries, NULL where it does: the setting off, the extension, its feature,
   fast linking (a link without it compiles, saving nothing), independent interpolation (without it the
   pre-rasterization and fragment parts must agree on decorations a translator need not promise) */
const char* snx_vk_gpl_off(int setting, bool has_ext, bool library, bool fast_link, bool independent_interpolation);

/* gi made a request for part (a VK_GRAPHICS_PIPELINE_LIBRARY_*_BIT_EXT, gl its info) of the state gi holds: gl first in
   its pNext chain, the library's flags with flags */
void snx_vk_gpl_part_info(VkGraphicsPipelineCreateInfo* gi, VkGraphicsPipelineLibraryCreateInfoEXT* gl,
                          VkGraphicsPipelineLibraryFlagsEXT part, VkPipelineCreateFlags flags);
/* gi made the request of a pipeline linked from count parts (li their info, next after it): optimized
   (VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT) or fast, with flags and layout */
void snx_vk_gpl_link_info(VkGraphicsPipelineCreateInfo* gi, VkPipelineLibraryCreateInfoKHR* li, const VkPipeline* parts,
                          uint32_t count, bool optimize, VkPipelineCreateFlags flags, VkPipelineLayout layout,
                          const void* next);

#endif
