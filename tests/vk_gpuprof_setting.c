/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_gpu_profile as a device runs it (gpu/vk/gpuprof_setting.h): on where the device has timestamps, 0 on a
   portability driver (MoltenVK), where a phone ran a scene at 0.4 to 0.8 fps with it and 6.4 to 7.2 without, and the
   line that says the value asked for, the one in effect and where to look instead */
#include <stdio.h>
#include <string.h>
#include "nx/gpu/vk/gpuprof_setting.h"

static int g_failures;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            g_failures++;                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
        }                                                                                                              \
    } while (0)

int main(void) {
    char say[640] = "unset";
    CHECK(vk_gpuprof_setting(1, 1, 0, "AMD Radeon RX 6800 XT (RADV NAVI21)", say, sizeof say) == 1 && !say[0],
          "vk_gpu_profile=1 runs as 1, unsaid, on a device with timestamps");
    snprintf(say, sizeof say, "unset");
    CHECK(vk_gpuprof_setting(0, 1, 1, "Apple A16 GPU", say, sizeof say) == 0 && !say[0],
          "vk_gpu_profile=0 runs as 0, unsaid, on a portability driver");
    CHECK(vk_gpuprof_setting(1, 1, 1, "Apple A16 GPU", say, sizeof say) == 0,
          "vk_gpu_profile=1 runs as 0 on a portability driver");
    CHECK(strstr(say, "vulkan: vk_gpu_profile=1 runs as vk_gpu_profile=0 on Apple A16 GPU, a portability driver "
                      "(VK_KHR_portability_subset): "),
          say);
    CHECK(strstr(say, "MoltenVK issues 1585 and 2378") && strstr(say, "MTL_HUD_ENABLED=1") &&
              strstr(say, "Instruments' Metal System Trace"),
          say);
    CHECK(strlen(say) + 1 < sizeof say, "the line fits whole");
    CHECK(vk_gpuprof_setting(1, 0, 0, "llvmpipe", say, sizeof say) == 0 &&
              !strcmp(say, "vulkan: vk_gpu_profile=1 runs as vk_gpu_profile=0 on llvmpipe: the device has no "
                           "timestamps on its graphics queue"),
          say);
    /* a device without timestamps says so first: there is nothing a portability driver's rule turns off */
    CHECK(vk_gpuprof_setting(1, 0, 1, "Apple A16 GPU", say, sizeof say) == 0 && strstr(say, "has no timestamps"), say);
    if (g_failures) return 1;
    puts("vk_gpuprof_setting: ok");
    return 0;
}
