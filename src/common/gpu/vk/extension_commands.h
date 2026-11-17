/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The commands each Vulkan device extension requires (tools/vkcmdgen writes the table from the Khronos registry the
   build has, vk.xml), and the check of a device made with some of them: each command, as vkGetDeviceProcAddr gives
   it. A driver can list an extension and give no pointer for one of its commands (NVIDIA's VK_EXT_depth_clamp_control
   had no vkCmdSetDepthClampRangeEXT), and the renderer would call a null pointer: each renderer makes the device
   again without such an extension (snx_vk_device_commands, extension_bans.h's vk_bans_lacking) */
#ifndef SNX_VK_EXTENSION_COMMANDS_H
#define SNX_VK_EXTENSION_COMMANDS_H
#include <stddef.h>
#include <stdint.h>
#include "common/gpu/vk/vk_common.h"
#include "common/gpu/vk/extension_bans.h"

/* a device command an extension requires, and the condition the registry puts on it (its <require>'s depends: the
   command is the device's only where that holds), NULL for none */
typedef struct {
    const char* name;
    const char* depends;
} SnxVkExtensionCommand;
/* an extension the registry has as a Vulkan device extension, and its commands: count of them from
   snx_vk_extension_commands[first] */
typedef struct {
    const char* name;
    uint32_t first, count;
} SnxVkExtensionEntry;
/* the generated table (generated/vk_extension_commands.c): every device extension, sorted by name */
extern const SnxVkExtensionCommand snx_vk_extension_commands[];
extern const SnxVkExtensionEntry snx_vk_extension_table[];
extern const uint32_t snx_vk_extension_table_count;

/* the table's entry of the extension name, NULL for a name the registry has not as a device extension */
const SnxVkExtensionEntry* snx_vk_extension_entry(const char* name);

/* expr, a depends of the registry, given whether each name in it holds (holds(name, its length, ctx)): names joined
   by , for or and + for and, which bind alike from the left, parentheses grouping. 1, 0, or -1 for one that does not
   parse */
int snx_vk_depends(const char* expr, int (*holds)(const char* name, size_t n, void* ctx), void* ctx);

/* an extension the device was made with, and a command it requires that the device gave no pointer for */
typedef struct {
    const char* extension; /* one of the enabled names given */
    const char* command;   /* the table's */
    int hidden;            /* the hide list named it: the driver had it */
} SnxVkLacking;

/* each command the n extensions of enabled require on dev, a device of Vulkan version api (a depends' VK_VERSION_x_y
   holds up to it, an extension name where it is one of enabled, any other name not), asked of get. The commands the
   comma-separated list hide names count as given none (vk_debug_hide_commands, to test this path). Those the device
   has not go into out, max at most, in enabled's order: their count, past max too. -1, with err, for an enabled
   extension the table has not, or a depends of it that does not parse: the table is the build's, and wrong */
int snx_vk_commands_lacking(const char* const* enabled, uint32_t n, uint32_t api, PFN_vkGetDeviceProcAddr get,
                            VkDevice dev, const char* hide, SnxVkLacking* out, uint32_t max, char* err, size_t err_n);

/* The check of a device just made, volk loaded for dev (device: its name), of Vulkan version api, with the n
   extensions of enabled: snx_vk_commands_lacking, hide's commands counting as given none. Each extension that lacks a
   command is said (a warning naming the device, the extension and the command) and kept by vk_bans_lacking, so the
   device made again goes without it: vk_bans_filter takes it out of that device's list. The count of commands lacking,
   0 for a device that stands; the caller then destroys dev and makes it again, once (again set for that one). -1,
   with err, where dev can't stand: one of need lacks a command, or again is set, or more lack than vk_bans_lacking
   keeps, or the table refuses an enabled extension. hide not empty says how many of its commands this device has */
int snx_vk_device_commands(VkDevice dev, const char* device, uint32_t api, const char* const* enabled, uint32_t n,
                           const VkExtensionNeed* need, uint32_t needs, const char* hide, int again, VkBanSay say,
                           char* err, size_t err_n);

/* hide, as snx_vk_commands_lacking takes it, checked against the table: 1 when each name is a device command an
   extension requires, else 0 with err naming the first that is not and the three nearest the table has */
int snx_vk_hide_check(const char* hide, char* err, size_t n);
/* whether hide names command */
int snx_vk_hide_has(const char* hide, const char* command);
#endif
