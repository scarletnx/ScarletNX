/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The one way a renderer makes its Vulkan device. snx_vk_device_make reads the extension bans (extension_bans.h) and
   the player's settings, then for each physical device, in the same order for every renderer: the bans told the
   device, its extensions listed, the settings (vk_ext_*: auto, on, off) and then the bans applied to that list, and the
   renderer's choose given what is left, a queue family at a time. choose turns extensions on only through
   snx_vk_choice_enable, which refuses one no setting names, and makes the device through snx_vk_choice_create. Once
   made, each command of each extension it was made with is checked (extension_commands.h): one its driver gives no
   pointer for takes the extension out of the device made again, once. The log says each step: the bans that hold and
   the setting of each extension the renderer asked about (and nothing of those it did not), the devices passed over
   and why, and the device made */
#ifndef SNX_COMMON_VK_DEVICE_H
#define SNX_COMMON_VK_DEVICE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "common/gpu/vk/extension_bans.h"
#include "common/gpu/vk/vk_common.h"

#define SNX_VK_CHOICE_EXTENSIONS 96  /* the extensions a device is made with, at most */
#define SNX_VK_CHOICE_ASKED      128 /* the extensions a renderer asks about on one device, at most */
#define SNX_VK_NEEDS             16  /* the extensions a renderer can't run without on one device, at most */

/* A physical device and queue family a renderer may make its device on, and what it chose there. choose reads phys,
   family, properties and again, list where a helper reads it whole, may set api, and does the rest through the
   functions below */
typedef struct {
    VkPhysicalDevice phys;
    uint32_t family; /* of the request's queues, presenting to its surface */
    VkPhysicalDeviceProperties properties;
    int again;    /* the device is made again, without what the first one's driver lacked a command of */
    uint32_t api; /* the Vulkan version the renderer runs it at: the request's, or newer where choose sets it */
    const SnxVkExtensions* list; /* the device's extensions, after the settings and the bans */
    /* device.c's */
    const VkExtensionSetting* settings;
    uint32_t setting_count;
    const char* extensions[SNX_VK_CHOICE_EXTENSIONS];
    uint32_t extension_count;
    const char* asked[SNX_VK_CHOICE_ASKED];
    uint32_t asked_count;
    VkDevice device;
    char why[512];   /* why choose passed this one over (snx_vk_choice_refuse) */
    char error[512]; /* what the renderer asked that no device can give it: the build's fault */
} SnxVkChoice;

/* the device's list, after the settings and the bans, has the extension; the log says its bans and its setting once
   a device is made */
bool snx_vk_choice_has(SnxVkChoice* c, const char* name);
/* The device is made with the extension, where the list has it: true. False where it has it not, or where the request
   has the settings and none of them names it: an extension the player can't turn off is refused, and
   snx_vk_device_make fails saying it */
bool snx_vk_choice_enable(SnxVkChoice* c, const char* name);
/* why this device is passed over, for the log and the error where none is left (what follows its name: "has no
   dynamicRendering"); false, for choose to return */
bool snx_vk_choice_refuse(SnxVkChoice* c, const char* why);
/* The device made with the extensions enabled: features the head of its pNext chain (a VkPhysicalDeviceFeatures2, or
   another feature structure with enabled), enabled the core features apart (NULL where features carries them), queues
   its queue_count queues (NULL: one queue of family). True; false where vkCreateDevice refuses it, which why says */
bool snx_vk_choice_create(SnxVkChoice* c, const void* features, const VkPhysicalDeviceFeatures* enabled,
                          const VkDeviceQueueCreateInfo* queues, uint32_t queue_count);

/* What a renderer asks of its device */
typedef struct {
    const char* who;  /* its name, before its lines in the log: "nds gpu" */
    const char* none; /* the error where no device is left, before each device's reason: "no Vulkan 1.3 device with
                         dynamic rendering" */
    VkInstance instance;
    uint32_t api; /* the Vulkan version it needs, 1.2 or newer (the bans read the driver's 1.2 properties) */
    VkQueueFlags queues;
    VkSurfaceKHR surface; /* the family presents to it; VK_NULL_HANDLE for none */
    /* where given, the device it rates highest (the first of equal ones; a negative rating passes one over) is the one
       choose is asked about, on its first family of the queues; where not, each in the loader's order, each family */
    SnxVkScore score;
    /* Its choice on a physical device and queue family: the extensions it turns on, its features and queues, and the
       device made (snx_vk_choice_create), true; or false to pass it over. Called again for the device made again,
       which decides everything anew */
    bool (*choose)(SnxVkChoice* c, void* user);
    void* user;
    const VkExtensionNeed* need; /* what it can't run without: off is refused for it, and a command it lacks fails */
    uint32_t needs;
    /* where given, need for each device instead, from its properties and its extensions as its driver lists them, into
       need (SNX_VK_NEEDS at most): how many */
    uint32_t (*needs_of)(const VkPhysicalDeviceProperties* p, const SnxVkExtensions* list, VkExtensionNeed* need,
                         void* user);
    /* the player's vk_ext_* (common/gpu/vk/device_settings.h), NULL for a renderer that takes none; vulkan_ban_ignore
       and vk_debug_hide_commands, NULL for none */
    const VkExtensionSetting* settings;
    uint32_t setting_count;
    const char *ban_ignore, *hide;
    const char* bans; /* the extension bans' text, NULL for the build's (extension_bans.txt) */
    VkBanSay say;     /* each line of the log; NULL for stderr, a warning's after "warning: " */
} SnxVkDeviceRequest;

/* the device made, volk loaded for it, and its extensions */
typedef struct {
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t family;
    const char* extensions[SNX_VK_CHOICE_EXTENSIONS];
    uint32_t extension_count;
    /* where none is made: every physical device was passed over, as against a table, setting or command that stops
       the device whatever the renderer asks (the 3DS asks again with one window where none presents to two) */
    bool passed_over;
    /* where none is made for what the request gave, whatever the device: the bans' table that does not read (the
       build's), or a setting that does not (vk_ext_*, vulkan_ban_ignore, vk_debug_hide_commands: the player's, which
       the Switch stops the game for); SNX_VK_REFUSED_NONE otherwise */
    enum { SNX_VK_REFUSED_NONE, SNX_VK_REFUSED_TABLE, SNX_VK_REFUSED_SETTING } refused;
} SnxVkDevice;

/* The device of the first physical device, in the loader's order (or the one r->score rates highest), of r->api or
   newer whose queue family of r->queues (presenting to r->surface) r->choose takes: true. False with why in err: a table, ban key, hidden command or
   setting that does not read, no device left (r->none, then each device and why), an extension r->choose turned on
   that no setting names, or one r->need names lacking a command */
bool snx_vk_device_make(const SnxVkDeviceRequest* r, SnxVkDevice* out, char* err, size_t n);
#endif
