/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Pipeline records (pipeline_records=DIR): each graphics pipeline written down whole before the driver makes it, kept
   where the driver fails it or the process dies inside it, so the pipeline can be made again away from the game
   (vk_pipeline_replay) and a driver's crash taken apart. Off, nothing here runs: the commands stay volk's.

   On, the recorder stands between the renderer and the driver's commands that make a device, shader modules,
   samplers, descriptor set and pipeline layouts and graphics pipelines (volk's pointers, so every caller goes through
   it), and keeps:
     DIR/device.txt        the device as it was made: the GPU, its queues, extensions and features
     DIR/spirv/<hash>.spv  each shader module's SPIR-V, once, by its 64-bit FNV-1a in hex
     DIR/<pid>-<n>/        a pipeline's record, pipeline.txt: every state of its VkGraphicsPipelineCreateInfo and of
                           what it names (the stages' modules by hash, the layout with its sets and samplers, the
                           libraries it links), written before the driver is called and taken away when it made the
                           pipeline. A failure keeps it with copies of device.txt and the SPIR-V it names, and says so;
                           inside the call, the thread's note (the crash report's "it was ...") names the folder
   A structure the recorder does not know is written as "unrecorded" with its sType, and the replay refuses it */
#ifndef SNX_VK_PIPELINE_RECORD_H
#define SNX_VK_PIPELINE_RECORD_H
#include "nx/gpu/vk/vk.h"

/* before the device is made, with pipeline_records set: its folder made and vkCreateDevice taken, so device.txt has
   the device that is made. 0 when recording; -1, said with the folder and the system's reason, when the folder cannot
   be made: no records then */
int vk_pipeline_record_start(void);
/* once the device is made and its commands loaded (volkLoadDevice): its commands taken. Nothing without start */
void vk_pipeline_record_device(void);
/* For a test, a driver that fails on purpose: the command the recorder calls in vkCreateGraphicsPipelines' place
   (NULL: the driver's), and the one it called before */
PFN_vkCreateGraphicsPipelines vk_pipeline_record_inner(PFN_vkCreateGraphicsPipelines make);

/* The pipelines of the record in folder dir made again on a device made as device.txt says, through instance (NULL:
   one of its own, Vulkan 1.3, from volk's loader): its SPIR-V and device.txt from dir, else from the folder above
   it. Each step that is not as recorded is said on stdout (another GPU, a window's extension left out). 0 when the
   record was read and the driver called, its answer in *made; -1 with the reason in why (a line of the record that
   does not read, an unrecorded structure, a device that cannot be made) */
int vk_pipeline_replay(VkInstance instance, const char* dir, VkResult* made, char* why, size_t n);

#endif
