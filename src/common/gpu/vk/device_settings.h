/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's settings a device request takes (device.h), for a renderer whose program links snx_settings_runtime:
   g_vk_extension_settings' vk_ext_*, vulkan_ban_ignore and vk_debug_hide_commands */
#ifndef SNX_COMMON_VK_DEVICE_SETTINGS_H
#define SNX_COMMON_VK_DEVICE_SETTINGS_H
#include "common/gpu/vk/device.h"
#include "snx_settings.h"

/* r takes them, the vk_ext_* copied into table. A build with more than table holds gives r their whole count, which
   snx_vk_device_make refuses, saying it, before it reads any */
static inline void snx_vk_device_settings(SnxVkDeviceRequest* r, VkExtensionSetting table[VK_EXTENSION_SETTINGS]) {
    for (int i = 0; i < g_vk_extension_settings_count && i < VK_EXTENSION_SETTINGS; i++)
        table[i] = (VkExtensionSetting){g_vk_extension_settings[i].name, g_vk_extension_settings[i].value,
                                        g_vk_extension_settings[i].core};
    r->settings = table;
    r->setting_count = (uint32_t)g_vk_extension_settings_count;
    r->ban_ignore = g_settings.vulkan_ban_ignore;
    r->hide = g_settings.vulkan_hidden_commands;
}
#endif
