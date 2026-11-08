/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The drivers known to break a Vulkan extension or feature (extension_bans.txt, whose head explains each field), and
   what the device does about them: device.c loads the table, says which device it runs on, takes the banned extensions
   out of the device's list before anything reads it (vk_bans_filter), asks for each feature it can do without
   (vk_ban), and at the end says the "off" lines that held for something nothing asked for (vk_bans_unclaimed). Every
   line that holds is said in the log with the extension, the device, its driver and version, and why */
#ifndef SNX_VK_EXTENSION_BANS_H
#define SNX_VK_EXTENSION_BANS_H
#include <stddef.h>
#include <stdint.h>
#include "common/gpu/vk/vk_common.h"

/* extension_bans.txt's text, compiled in by cmake/vk_extension_bans.cmake */
extern const char snx_vk_extension_bans_text[];

/* the systems a line names, as bits; none is any */
enum {
    SNX_VK_BAN_LINUX = 1,
    SNX_VK_BAN_ANDROID = 2,
    SNX_VK_BAN_WINDOWS = 4,
    SNX_VK_BAN_MACOS = 8,
    SNX_VK_BAN_IOS = 16,
    SNX_VK_BAN_FREEBSD = 32,
    SNX_VK_BAN_OPENBSD = 64,
    SNX_VK_BAN_NETBSD = 128
};
/* the system this build runs on, SNX_VK_BAN_*; 0 for one the table names none of, which only "any" lines take */
uint32_t vk_ban_system(void);

/* a device as the table reads it: its name, its driver's driverName and driverInfo, VkDriverId, driverVersion and
   deviceID, and the system (SNX_VK_BAN_*) */
typedef struct {
    const char *device, *driver_name, *driver_info;
    uint32_t driver_id, driver_version, device_id, system;
} VkBanDevice;

/* a line for the log: warn for a warning, else plain */
typedef void (*VkBanSay)(const char* line, int warn);

/* reads text as the table: 1, or 0 with the first error in err (its line, the value and why) */
int vk_bans_load(const char* text, char* err, size_t n);
/* the device the queries below are about */
void vk_bans_device(const VkBanDevice* d);
/* the table's line of the "off" ban on key that holds for the device, 0 for none: every line on key that holds is
   claimed, as the renderer asked for it */
uint32_t vk_ban_find(const char* key);
/* vk_ban_find, and the line that says key is off said where have (the device has it, so it is turned off): 0 where
   no ban holds or have is 0 */
uint32_t vk_ban(const char* key, int have, VkBanSay say);
/* line's driver and versions as a sentence's subject: "NVIDIA's driver before 580.119.02", "Qualcomm's driver on
   Android", "Qualcomm's driver on the GPUs 0x6020200, 0x7000201" */
void vk_ban_scope(uint32_t line, char* out, size_t n);
/* the scope, what breaks, and where that is known from: "NVIDIA's driver before 580.119.02 breaks dynamic vertex
   input (Eden's ban, ...; extension_bans.txt:42)" */
void vk_ban_why(uint32_t line, char* out, size_t n);
/* takes out of e every extension an "off" line without an override holds for, each said; says the "warn" lines that
   hold for an extension e has and for a feature; the rest (features, overrides) wait for vk_ban and vk_ban_find. How
   many it took out */
uint32_t vk_bans_filter(SnxVkExtensions* e, VkBanSay say);
/* says each "off" line that holds and nothing claimed */
void vk_bans_unclaimed(VkBanSay say);
/* the first line after line that holds for the device, of either action; 0 past the last */
uint32_t vk_bans_holding(uint32_t line);
/* a field of a line (VK_BAN_*), as the table writes it */
enum {
    VK_BAN_KEY,
    VK_BAN_ACTION,
    VK_BAN_DRIVER,
    VK_BAN_VERSIONS,
    VK_BAN_SYSTEM,
    VK_BAN_DEVICES,
    VK_BAN_BREAKS,
    VK_BAN_SOURCE,
    VK_BAN_RETEST,
    VK_BAN_CHECKED,
    VK_BAN_OVERRIDE,
    VK_BAN_FIELDS
};
void vk_ban_field(uint32_t line, int field, char* out, size_t n);
/* the table's name of a VkDriverId (radv, nvidia, ...), NULL for one it has none for; and the VkDriverId of a name,
   0 for one the table has not */
const char* vk_ban_driver_name(uint32_t id);
uint32_t vk_ban_driver_id(const char* name);
/* text as a version of a driver of VkDriverId id, in its numbering (the table's versions), into *v: 1, or 0 with why */
int vk_ban_parse_version(uint32_t id, const char* text, uint32_t* v, char* why, size_t n);
/* driverVersion v of a driver of VkDriverId id, as its vendor numbers it: NVIDIA's 580.119.02, Intel's Windows
   101.4502 (the last two of its four numbers), MoltenVK's 1.2.11, the rest as Vulkan versions (Mesa's 26.1.8) */
void vk_driver_version_text(uint32_t id, uint32_t v, char* out, size_t n);
#endif
