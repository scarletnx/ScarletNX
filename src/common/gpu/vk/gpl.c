/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/gpl.h"
#include <stdio.h>

const char* snx_vk_gpl_off(int setting, bool has_ext, bool library, bool fast_link, bool independent_interpolation) {
    return !setting     ? "pipeline_libraries=0"
           : !has_ext   ? "the device has no VK_EXT_graphics_pipeline_library"
           : !library   ? "the device has VK_EXT_graphics_pipeline_library without graphicsPipelineLibrary"
           : !fast_link ? "the device links pipeline libraries without fast linking"
           : !independent_interpolation
               ? "the device has no independent interpolation decorations for pipeline libraries"
               : NULL;
}

void snx_vk_gpl_part_info(VkGraphicsPipelineCreateInfo* gi, VkGraphicsPipelineLibraryCreateInfoEXT* gl,
                          VkGraphicsPipelineLibraryFlagsEXT part, VkPipelineCreateFlags flags) {
    *gl = (VkGraphicsPipelineLibraryCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .pNext = gi->pNext, .flags = part};
    gi->pNext = gl;
    gi->flags =
        VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT | flags;
}

void snx_vk_gpl_link_info(VkGraphicsPipelineCreateInfo* gi, VkPipelineLibraryCreateInfoKHR* li, const VkPipeline* parts,
                          uint32_t count, bool optimize, VkPipelineCreateFlags flags, VkPipelineLayout layout,
                          const void* next) {
    *li = (VkPipelineLibraryCreateInfoKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
                                           .pNext = next,
                                           .libraryCount = count,
                                           .pLibraries = parts};
    *gi = (VkGraphicsPipelineCreateInfo){.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .pNext = li,
                                         .flags =
                                             (optimize ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0) | flags,
                                         .layout = layout};
}
