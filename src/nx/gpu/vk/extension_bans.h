/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The drivers known to break a Vulkan extension or feature (extension_bans.txt, whose head explains each field), and
   what the device does about them: device.c loads the table, says which device it runs on, applies the extension
   settings (vk_extension_settings: auto, on, off for each extension, auto by the bans), takes the banned extensions
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
/* vulkan_ban_ignore: the keys whose bans a run goes over, to test again whether a driver still breaks them; list,
   separated by commas, each a key of the table (every line on it). 1, or 0 with the first key the table has not and
   the three nearest it has in err. A new load lets them go */
int vk_bans_ignore(const char* list, char* err, size_t n);
/* the device the queries below are about */
void vk_bans_device(const VkBanDevice* d);
/* says each key vulkan_ban_ignore names: a warning with the ban it goes over where an off line on it holds for the
   device, else a line that none does */
void vk_bans_ignored(VkBanSay say);
/* the table's line of the "off" ban on key that holds for the device, 0 for none or one vulkan_ban_ignore goes over:
   every line on key that holds is claimed, as the renderer asked for it */
uint32_t vk_ban_find(const char* key);
/* vk_ban_find, and the line that says key is off said where have (the device has it, so it is turned off): 0 where
   no ban holds, vulkan_ban_ignore goes over it, or have is 0 */
uint32_t vk_ban(const char* key, int have, VkBanSay say);
/* line's driver and versions as a sentence's subject: "NVIDIA's driver before 580.119.02", "Qualcomm's driver on
   Android", "Qualcomm's driver on the GPUs 0x6020200, 0x7000201" */
void vk_ban_scope(uint32_t line, char* out, size_t n);
/* the scope, what breaks, and where that is known from: "NVIDIA's driver before 580.119.02 breaks dynamic vertex
   input (Eden's ban, ...; extension_bans.txt:42)" */
void vk_ban_why(uint32_t line, char* out, size_t n);
/* takes out of e every extension an "off" line without an override holds for, but those vulkan_ban_ignore goes over,
   each said; says the "warn" lines that hold for an extension e has and for a feature; the rest (features, overrides)
   wait for vk_ban and vk_ban_find. How many it took out */
uint32_t vk_bans_filter(SnxVkExtensions* e, VkBanSay say);
/* says each "off" line that holds and nothing claimed */
void vk_bans_unclaimed(VkBanSay say);
/* The extension settings (settings.txt's g_vk_extensions_ini, vk_ext_*), one per extension the renderer can use: its
   name, the setting's value (auto, on or off) and the Vulkan version it was promoted to (13 for 1.3, 0 for none).
   auto leaves the extension to the bans; on goes over every ban on it; off takes it out of the device's list */
typedef struct {
    const char* name;
    const char* value;
    int core;
} VkExtensionSetting;
/* an extension the renderer can't run without on this device, and why: off is refused for it */
typedef struct {
    const char* name;
    const char* why;
} VkExtensionNeed;
/* the settings applied to e, the device's list as its driver gives it, before vk_bans_filter: each off one taken out,
   each on one's bans gone over. 1, or 0 with why in err: a value not auto, on or off, or off for a needed extension
   the driver has. Kept for vk_extension_settings_say */
int vk_extension_settings(SnxVkExtensions* e, const VkExtensionSetting* s, uint32_t count, const VkExtensionNeed* need,
                          uint32_t needs, char* err, size_t n);
/* after vk_bans_filter, a line for each setting: its extension on or off in e, the list the renderer reads, and why.
   api: the Vulkan version the renderer runs the device at, for an extension that is core there */
void vk_extension_settings_say(const SnxVkExtensions* e, uint32_t api, VkBanSay say);
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
