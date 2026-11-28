/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Graphics pipeline libraries (pipeline_libraries, VK_EXT_graphics_pipeline_library): a pipeline linked from four
   parts that each compile once, the vertex input, the pre-rasterization shaders, the fragment shader and the fragment
   output, fast where the driver links them so, and optimized after. What every renderer that makes them shares, the
   Switch's (nx/gpu/vk/pipeline.c) and the consoles': whether a device makes them, a part's request and a link's, and
   the split of a whole pipeline's request into its parts */
#ifndef SNX_COMMON_VK_GPL_H
#define SNX_COMMON_VK_GPL_H
#include "common/gpu/vk/device.h"
#include <SDL3/SDL_mutex.h>
#include <stdbool.h>
#include <stddef.h>

#define SNX_VK_GPL_PARTS 4 /* vertex input, pre-rasterization, fragment shader, fragment output: the flags' order */

/* why a device makes no pipeline from libraries, NULL where it does: the setting off, the extension, its feature,
   fast linking (a link without it compiles, saving nothing), independent interpolation (without it the
   pre-rasterization and fragment parts must agree on decorations a translator need not promise) */
const char* snx_vk_gpl_off(int setting, bool has_ext, bool library, bool fast_link, bool independent_interpolation);

/* a device's libraries, as snx_vk_gpl_choose found them: on, and the feature it is made with */
typedef struct {
    bool on;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT feature;
} SnxVkGpl;
/* The libraries on c's device where setting (pipeline_libraries) asks and the device has all they need: its two
   extensions enabled and the feature before next in the chain, returned; else next, and line says why not. line is
   the run's line either way */
void* snx_vk_gpl_choose(SnxVkGpl* g, SnxVkChoice* c, int setting, void* next, char* line, size_t n);

/* gi made a request for part (a VK_GRAPHICS_PIPELINE_LIBRARY_*_BIT_EXT, gl its info) of the state gi holds: gl first in
   its pNext chain, the library's flags with flags */
void snx_vk_gpl_part_info(VkGraphicsPipelineCreateInfo* gi, VkGraphicsPipelineLibraryCreateInfoEXT* gl,
                          VkGraphicsPipelineLibraryFlagsEXT part, VkPipelineCreateFlags flags);
/* gi made the request of a pipeline linked from count parts (li their info, next after it): optimized
   (VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT) or fast, with flags and layout */
void snx_vk_gpl_link_info(VkGraphicsPipelineCreateInfo* gi, VkPipelineLibraryCreateInfoKHR* li, const VkPipeline* parts,
                          uint32_t count, bool optimize, VkPipelineCreateFlags flags, VkPipelineLayout layout,
                          const void* next);

/* A whole pipeline's request (whole) as its four parts' requests, in the flags' order, each holding the state its
   part takes: the vertex input its input and assembly; the pre-rasterization shaders the vertex stage, the viewport,
   the rasterization, the tessellation and the layout; the fragment shader its stage, the depth and stencil, the
   multisampling and the layout; the fragment output the blending and the multisampling. Every part but the vertex
   input takes whole's pNext chain (the rendering's view mask and formats), and each whole's dynamic states, of
   which a part keeps the ones of its own state */
typedef struct {
    VkGraphicsPipelineCreateInfo info[SNX_VK_GPL_PARTS];
} SnxVkGplSplit;
void snx_vk_gpl_split(const VkGraphicsPipelineCreateInfo* whole, SnxVkGplSplit* out);
/* the part flag of each of the four, in order */
VkGraphicsPipelineLibraryFlagsEXT snx_vk_gpl_part(unsigned i);

/* A renderer's parts made so far, each by its state written out whole (its stages' modules, its structs' values and
   arrays, its chain's rendering formats), which a part of the same state reuses; its threads share it */
typedef struct {
    SDL_Mutex* lock;
    struct SnxVkGplPart* made;
    size_t count, room;
    unsigned reused; /* parts a pipeline took that an earlier one made */
} SnxVkGplParts;
/* whole made as its four parts, each made where parts has it not, then linked from them: optimized, or fast (to draw
   with until an optimized one replaces it), linked the parts it took (VK_NULL_HANDLE where a part failed). A
   request of a state too large to write out makes its part every time */
VkResult snx_vk_gpl_make(SnxVkGplParts* parts, VkDevice device, VkPipelineCache cache,
                         const VkGraphicsPipelineCreateInfo* whole, bool optimize, VkPipeline linked[SNX_VK_GPL_PARTS],
                         VkPipeline* out);
/* the optimized pipeline linked from the parts a fast one took, with layout */
VkResult snx_vk_gpl_optimize(VkDevice device, VkPipelineCache cache, const VkPipeline linked[SNX_VK_GPL_PARTS],
                             VkPipelineLayout layout, VkPipeline* out);
/* every part destroyed, the lock freed */
void snx_vk_gpl_parts_free(SnxVkGplParts* parts, VkDevice device);
#endif
