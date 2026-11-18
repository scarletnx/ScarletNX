/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The driver's statistics of each graphics and compute pipeline (VK_KHR_pipeline_executable_properties), written to
   the CSV file that the pipeline_stats setting names: for each executable of a pipeline, such as a shader stage, each
   statistic that the driver reports, such as its instructions, registers and spills. A row names the programs of the
   pipeline's stages by the hashes of the log's lines, so a translator change can be measured in what the GPU runs,
   not in SPIR-V words. The shader corpus (shader_corpus) names a program by that hash with the key's 32-bit mix in
   its low half: its file starts with the same 8 hex digits */
#ifndef SNX_VK_PIPELINE_STATS_H
#define SNX_VK_PIPELINE_STATS_H
#include "nx/gpu/vk/vk.h"

/* the flag that makes the driver keep the statistics: VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR when the device
   has the extension on for the setting, else 0. Every graphics pipeline and part takes it, so its parts and links
   agree, and every compute pipeline of the game's programs. A driver can make the flag part of the pipeline cache's
   key, and then the pipelines of runs without it are misses. NVIDIA's does not: a run with it took 1042 of 1062
   optimized pipelines from the cache */
VkPipelineCreateFlags vk_pipeline_stats_flags(void);
/* the statistics of p, a whole or an optimized pipeline (what, for the row), whose stages run the programs with
   these corpus hashes (vertex, tessellation control and evaluation, geometry, fragment: 0 for none or a runtime
   shader). Any thread: the rows of a pipeline are written and flushed together, so the file is whole at any exit */
void vk_pipeline_stats_write(VkPipeline p, const char* what, const u64 programs[5]);
/* the same for the compute pipeline p of the program with that corpus hash: its rows name it in the cs column */
void vk_pipeline_stats_write_compute(VkPipeline p, u64 program);

#endif
