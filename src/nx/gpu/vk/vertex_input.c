/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Dynamic vertex input or a pipeline per vertex layout: the setting, the device and the drivers' bans
   (vertex_input.h) */
#include "nx/gpu/vk/vertex_input.h"
#include <stdio.h>
#include <string.h>
#include "common/gpu/vk/extension_bans.h"

/* the setting's values, by SNX_VK_VI_* */
static const char* const vi_words[] = {"auto", "on", "off"};

int vk_vi_mode(const char* text) {
    for (int i = 0; i < 3; i++)
        if (!strcmp(text, vi_words[i])) return i;
    return -1;
}

int vk_vi_choose(int mode, const VkViDevice* d, char* line, size_t n, int* warn) {
    const char* ban = d->ban;
    const char* lacks = d->has == 0   ? "has no VK_EXT_vertex_input_dynamic_state"
                        : d->has == 1 ? "has VK_EXT_vertex_input_dynamic_state without its vertexInputDynamicState"
                                      : NULL;
    char version[32], device[400];
    /* auto is off on Android whatever the driver, as Eden and rEden have it (settings.h, vertex_input_dynamic_state):
       Qualcomm's driver 512.800.48 drew draws of edenprobe's coherence suite wrong with it */
    int android = mode == SNX_VK_VI_AUTO && d->android;
    int dynamic = mode != SNX_VK_VI_OFF && !lacks && (mode == SNX_VK_VI_ON || (!ban && !android));
    vk_driver_version_text(d->driver_id, d->driver_version, version, sizeof version);
    snprintf(device, sizeof device, "%s (%s, %s, driver version %s)", d->device, d->driver_name, d->driver_info,
             version);
    *warn = mode == SNX_VK_VI_ON && lacks;
    if (dynamic)
        snprintf(line, n,
                 "vulkan: vertex_input_dynamic_state=%s: draws set their vertex layout as dynamic state "
                 "(VK_EXT_vertex_input_dynamic_state) on %s%s%s",
                 vi_words[mode], device, ban ? ", over the ban: " : "", ban ? ban : "");
    else if (mode == SNX_VK_VI_OFF)
        snprintf(line, n,
                 "vulkan: vertex_input_dynamic_state=off: each vertex layout takes a pipeline of its own on %s",
                 device);
    else if (lacks)
        snprintf(line, n,
                 "vulkan: vertex_input_dynamic_state=%s, but %s %s: each vertex layout takes a pipeline of its own",
                 vi_words[mode], device, lacks);
    else if (android)
        snprintf(
            line, n,
            "vulkan: vertex_input_dynamic_state=auto: each vertex layout takes a pipeline of its own on %s: off on "
            "Android, as Eden (vertex_input_dynamic_state=on sets it anyway)",
            device);
    else
        snprintf(line, n,
                 "vulkan: vertex_input_dynamic_state=auto: each vertex layout takes a pipeline of its own on %s, as "
                 "%s (vertex_input_dynamic_state=on sets it anyway)",
                 device, ban);
    return dynamic;
}
