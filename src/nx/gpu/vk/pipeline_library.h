/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Pipelines linked from parts (VK_EXT_graphics_pipeline_library): whether the device links them, the log line that
   says it, and which part each dynamic state belongs to. With them a draw's new pipeline is linked from its four
   parts, each made once for all the pipelines that share it, and the pipeline made whole replaces it once a worker
   has made that (pipeline.c) */
#ifndef SNX_VK_PIPELINE_LIBRARY_H
#define SNX_VK_PIPELINE_LIBRARY_H
#include <stddef.h>
#include "common/cpu/aarch64/cpu.h"

/* a device as the choice reads it: its name, its driver's VkDriverId, driverName, driverInfo and driverVersion, what
   it has of the extensions (0 not both of VK_KHR_pipeline_library and VK_EXT_graphics_pipeline_library, 1 both
   without the graphicsPipelineLibrary feature, 2 the feature too), its graphicsPipelineLibraryFastLinking and
   graphicsPipelineLibraryIndependentInterpolationDecoration, and the ban on the extension that holds for its driver
   (extension_bans.h's vk_ban_why: the driver, its versions, what breaks and where that is known from), NULL for
   none */
typedef struct {
    const char *device, *driver_name, *driver_info;
    u32 driver_id, driver_version;
    int has, fast_linking, independent_interpolation;
    const char* ban;
    int cpu; /* a CPU device (VK_PHYSICAL_DEVICE_TYPE_CPU): the link stays, made whole never (VkState.gpl_whole) */
} VkGplDevice;
/* whether new pipelines are linked from parts on d: 1 or 0, and the log line that says it, or why not, into line */
int vk_gpl_choose(const VkGplDevice* d, char* line, size_t n);
/* the part of a pipeline (VK_GRAPHICS_PIPELINE_LIBRARY_*_BIT_EXT) whose library a VkDynamicState is given to, as the
   extension's state groups assign them: 0 for a state this table doesn't know, which no pipeline may then list */
u32 vk_gpl_dynamic_part(u32 state);
/* the dynamic states every graphics pipeline has (pipe_create adds the feedback loop's, the binding stride and the
   vertex input where they apply), *n of them */
const u32* vk_pipe_dynamic_states(u32* n);
#endif
