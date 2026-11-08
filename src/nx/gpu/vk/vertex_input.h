/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Whether draws give the GPU their vertex layout as dynamic state (VK_EXT_vertex_input_dynamic_state), from the
   vertex_input_dynamic_state setting, what the device has and the drivers known to draw it wrong (extension_bans.txt's
   lines on it, which this setting decides), and the log line that says which. With it a pipeline serves every vertex layout its programs are drawn with (pipeline.c), and each
   draw sets its layout in the command buffer (render.c) */
#ifndef SNX_VK_VERTEX_INPUT_H
#define SNX_VK_VERTEX_INPUT_H
#include <stddef.h>
#include "common/cpu/aarch64/cpu.h"

/* vertex_input_dynamic_state's values: auto, dynamic where the device has the extension unless a ban holds for its
   driver or the runtime runs on Android; on, dynamic wherever it has it; off, never */
enum { SNX_VK_VI_AUTO, SNX_VK_VI_ON, SNX_VK_VI_OFF };
/* the value text names, SNX_VK_VI_*; -1 for none of auto, on and off */
int vk_vi_mode(const char* text);

/* a device as the choice reads it: its name, its driver's VkDriverId, driverName, driverInfo and driverVersion, what
   it has of the extension (0 nothing, 1 the extension without its vertexInputDynamicState feature, 2 both), whether
   the runtime runs on Android, and the ban on the extension that holds for its driver (vk_ban_why's text: the driver,
   its versions, what breaks and where that is known from), NULL for none */
typedef struct {
    const char *device, *driver_name, *driver_info;
    u32 driver_id, driver_version;
    int has, android;
    const char* ban;
} VkViDevice;
/* whether draws set their vertex layout as dynamic state on d with the setting at mode (SNX_VK_VI_*): 1 or 0, and the
   line the log says it with into line (*warn: a warning, as on was asked of a device that has no way to) */
int vk_vi_choose(int mode, const VkViDevice* d, char* line, size_t n, int* warn);
#endif
