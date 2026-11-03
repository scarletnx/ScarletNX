/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Dynamic vertex input or a pipeline per vertex layout: the setting, the device and the drivers' bans
   (vertex_input.h) */
#include "nx/gpu/vk/vertex_input.h"
#include <stdio.h>
#include <string.h>
#include "volk.h"

/* the setting's values, by SNX_VK_VI_* */
static const char* const vi_words[] = {"auto", "on", "off"};

int vk_vi_mode(const char* text) {
    for (int i = 0; i < 3; i++)
        if (!strcmp(text, vi_words[i])) return i;
    return -1;
}

void vk_driver_version_text(u32 id, u32 v, char* out, size_t n) {
    if (id == VK_DRIVER_ID_NVIDIA_PROPRIETARY)
        snprintf(out, n, "%u.%u.%02u", v >> 22, v >> 14 & 0xffu, v >> 6 & 0xffu);
    else if (id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS)
        snprintf(out, n, "%u.%u", v >> 14, v & 0x3fffu);
    else /* VK_MAKE_VERSION's fields: a major of 10 bits, as Qualcomm's 512 needs */
        snprintf(out, n, "%u.%u.%u", v >> 22, v >> 12 & 0x3ffu, v & 0xfffu);
}

/* Eden's (vulkan_device.cpp, Device::GetSuitability), each in its vendor's numbering (vk_driver_version_text):
   NVIDIA's major << 22 | minor << 14 | patch << 6, and Intel's Windows driver's last two numbers, major << 14 |
   minor, where 27.20.100.0 is 100.0. Eden compares with VK_MAKE_API_VERSION(580, 119, 02, 0), whose first argument is
   the variant: above every driverVersion its mask leaves, which bans every version */
static const struct {
    u32 id, below;
    const char* what;
} vi_bans[] = {
    {VK_DRIVER_ID_NVIDIA_PROPRIETARY, 580u << 22 | 119u << 14 | 2u << 6, "NVIDIA's driver before 580.119.02"},
    {VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, 100u << 14, "Intel's Windows driver before 27.20.100.0"},
};

const char* vk_vi_ban(u32 id, u32 v) {
    for (size_t i = 0; i < sizeof vi_bans / sizeof *vi_bans; i++)
        if (vi_bans[i].id == id && v < vi_bans[i].below) return vi_bans[i].what;
    return NULL;
}

int vk_vi_choose(int mode, const VkViDevice* d, char* line, size_t n, int* warn) {
    const char* ban = vk_vi_ban(d->driver_id, d->driver_version);
    const char* lacks = d->has == 0   ? "has no VK_EXT_vertex_input_dynamic_state"
                        : d->has == 1 ? "has VK_EXT_vertex_input_dynamic_state without its vertexInputDynamicState"
                                      : NULL;
    char version[32], device[400];
    int dynamic = mode != SNX_VK_VI_OFF && !lacks && (mode == SNX_VK_VI_ON || !ban);
    vk_driver_version_text(d->driver_id, d->driver_version, version, sizeof version);
    snprintf(device, sizeof device, "%s (%s, %s, driver version %s)", d->device, d->driver_name, d->driver_info,
             version);
    *warn = mode == SNX_VK_VI_ON && lacks;
    if (dynamic)
        snprintf(line, n,
                 "vulkan: vertex_input_dynamic_state=%s: draws set their vertex layout as dynamic state "
                 "(VK_EXT_vertex_input_dynamic_state) on %s%s%s",
                 vi_words[mode], device, ban ? ", over the ban on " : "", ban ? ban : "");
    else if (mode == SNX_VK_VI_OFF)
        snprintf(line, n,
                 "vulkan: vertex_input_dynamic_state=off: each vertex layout takes a pipeline of its own on %s",
                 device);
    else if (lacks)
        snprintf(line, n,
                 "vulkan: vertex_input_dynamic_state=%s, but %s %s: each vertex layout takes a pipeline of its own",
                 vi_words[mode], device, lacks);
    else
        snprintf(line, n,
                 "vulkan: vertex_input_dynamic_state=auto: each vertex layout takes a pipeline of its own on %s, as %s "
                 "draws dynamic vertex input wrong (vertex_input_dynamic_state=on sets it anyway)",
                 device, ban);
    return dynamic;
}
