/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_gpu_profile as the device runs it (gpuprof.c): the value in effect, and the line that says why it is not the one
   asked for. log=3 asks for it as well: its counters are the same work */
#ifndef SNX_VK_GPUPROF_SETTING_H
#define SNX_VK_GPUPROF_SETTING_H
#include <stddef.h>
#include <stdio.h>

/* asked: vk_gpu_profile, or log=3 in by; timestamps: the device's timestampComputeAndGraphics; portability: the device
   is a portability driver (it lists VK_KHR_portability_subset). The value in effect, with the line to warn in say,
   empty where the value asked for is the one in effect.
   A portability driver runs 0: MoltenVK gives each timestamp a Metal encoder of its own that waits for the work
   before it, which ran a scene 10 times slower with two timestamps a pass, and its timestamps are unreliable on Apple
   GPUs (KhronosGroup/MoltenVK issues 1585 and 2378) */
static inline int vk_gpuprof_setting(int asked, const char* by, int timestamps, int portability, const char* device,
                                     char* say, size_t n) {
    if (n) say[0] = 0;
    if (!asked) return 0;
    if (!timestamps) {
        snprintf(say, n,
                 "vulkan: %s asks for the GPU's time a pass, and %s has no timestamps on its graphics queue",
                 by, device);
        return 0;
    }
    if (portability) {
        snprintf(say, n,
                 "vulkan: %s asks for the GPU's time a pass, and %s is a portability driver "
                 "(VK_KHR_portability_subset): each timestamp there takes a Metal encoder of its own, which "
                 "slows a scene about 10 times, and reads unreliably (MoltenVK issues 1585 and 2378). "
                 "MTL_HUD_ENABLED=1 in the environment, or Instruments' Metal System Trace, shows the GPU's "
                 "time there",
                 by, device);
        return 0;
    }
    return asked;
}
#endif
