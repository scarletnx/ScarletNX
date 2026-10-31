/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_PROBE_H
#define SNX_VK_PROBE_H
#include <stdio.h>
/* Recommended Settings' test of the GPU (scarletnx --probe), a line each on out: "probe: gpu", then gpu=, gpu_driver=,
   gpu_type=, gpu_bc=, gpu_astc=, gpu_heap_mb=, gpu_budget_mb= and gpu_budget_from= (driver, or heap where the driver
   gives no budget); "probe: pipelines", then pipelines_auto= (the workers pipeline_threads' Auto makes) and
   pipelines= (each number of threads and the new pipelines a second it made, threads:rate between commas). A device
   that answers nothing gives gpu_error= with Vulkan's word for it instead, and no pipelines. 0 when it ran */
int vk_probe(FILE* out);
#endif
