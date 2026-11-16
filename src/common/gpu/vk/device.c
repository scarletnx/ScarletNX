/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/device.h"
#include "common/gpu/vk/extension_commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void say_stderr(const char* line, int warn) { fprintf(stderr, "%s%s\n", warn ? "warning: " : "", line); }

/* name kept for the log, which says its setting once the device is made */
static void ask(SnxVkChoice* c, const char* name) {
    for (uint32_t i = 0; i < c->asked_count; i++)
        if (!strcmp(c->asked[i], name)) return;
    if (c->asked_count < SNX_VK_CHOICE_ASKED)
        c->asked[c->asked_count++] = name;
    else if (!c->error[0])
        snprintf(c->error, sizeof c->error, "the renderer asks about %s past the %d extensions a device keeps", name,
                 SNX_VK_CHOICE_ASKED);
}

bool snx_vk_choice_has(SnxVkChoice* c, const char* name) {
    ask(c, name);
    return snx_vk_extensions_has(c->list, name);
}

bool snx_vk_choice_enable(SnxVkChoice* c, const char* name) {
    ask(c, name);
    if (c->settings) {
        uint32_t k = 0;
        while (k < c->setting_count && strcmp(c->settings[k].name, name)) k++;
        if (k == c->setting_count) {
            if (!c->error[0])
                snprintf(c->error, sizeof c->error,
                         "the renderer turns on %s, which no setting names (a \"setting @%s\" line in "
                         "src/nx/runtime/settings.txt): every extension a device is made with is one the player can "
                         "turn off",
                         name, name);
            return false;
        }
    }
    if (!snx_vk_extensions_has(c->list, name)) return false;
    for (uint32_t i = 0; i < c->extension_count; i++)
        if (!strcmp(c->extensions[i], name)) return true;
    if (c->extension_count == SNX_VK_CHOICE_EXTENSIONS) {
        if (!c->error[0])
            snprintf(c->error, sizeof c->error, "the renderer turns on %s past the %d extensions a device is made with",
                     name, SNX_VK_CHOICE_EXTENSIONS);
        return false;
    }
    c->extensions[c->extension_count++] = name;
    return true;
}

bool snx_vk_choice_refuse(SnxVkChoice* c, const char* why) {
    snprintf(c->why, sizeof c->why, "%s", why);
    return false;
}

bool snx_vk_choice_create(SnxVkChoice* c, const void* features, const VkPhysicalDeviceFeatures* enabled,
                          const VkDeviceQueueCreateInfo* queues, uint32_t queue_count) {
    if (c->device) {
        if (!c->error[0]) snprintf(c->error, sizeof c->error, "the renderer makes its device twice on one choice");
        return false;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo one = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = c->family,
                                   .queueCount = 1,
                                   .pQueuePriorities = &priority};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .pNext = features,
                             .queueCreateInfoCount = queues ? queue_count : 1,
                             .pQueueCreateInfos = queues ? queues : &one,
                             .enabledExtensionCount = c->extension_count,
                             .ppEnabledExtensionNames = c->extensions,
                             .pEnabledFeatures = enabled};
    VkResult result = vkCreateDevice(c->phys, &di, NULL, &c->device);
    if (result == VK_SUCCESS) return true;
    c->device = VK_NULL_HANDLE;
    snprintf(c->why, sizeof c->why, "refuses the device: vkCreateDevice gave %s (%d)", snx_vk_result_str(result),
             (int)result);
    return false;
}

/* what a queue family must have, as a reason says it lacks one: "graphics and compute queues presenting to the
   window" */
static void queue_text(VkQueueFlags flags, VkSurfaceKHR surface, char* out, size_t n) {
    static const struct {
        VkQueueFlags bit;
        const char* name;
    } names[] = {
        {VK_QUEUE_GRAPHICS_BIT, "graphics"}, {VK_QUEUE_COMPUTE_BIT, "compute"}, {VK_QUEUE_TRANSFER_BIT, "transfer"}};
    size_t w = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof names / sizeof *names && w < n; i++)
        if (flags & names[i].bit) w += (size_t)snprintf(out + w, n - w, "%s%s", w ? " and " : "", names[i].name);
    if (w < n) snprintf(out + w, n - w, "%squeues%s", w ? " " : "", surface ? " presenting to the window" : "");
}

/* why the device named name is passed over, said and kept for the error where none is left */
static void pass_over(const SnxVkDeviceRequest* r, VkBanSay say, const char* name, const char* why, char* reasons,
                      size_t n) {
    char line[1200];
    snprintf(line, sizeof line, "%s: %s is passed over: it %s", r->who, name, why);
    say(line, 0);
    size_t w = strlen(reasons);
    if (w < n) snprintf(reasons + w, n - w, "%s%s %s", w ? "; " : "", name, why);
}

/* a device the renderer made and that does not stand, destroyed through its own driver: volk holds the commands of
   the device loaded last, which may be another's */
static void drop(VkDevice dev) {
    volkLoadDevice(dev);
    vkDestroyDevice(dev, NULL);
}

/* One pass over the physical devices: 1 with c's device made on the first r->choose takes, list its extensions as
   choose saw them; 0 with none, each one's reason in reasons; -1 with err, which stops the device */
static int pass(const SnxVkDeviceRequest* r, int again, VkBanSay say, SnxVkChoice* c, SnxVkExtensions* list,
                char* reasons, size_t reasons_n, char* err, size_t n) {
    uint32_t count = 0;
    VkPhysicalDevice* devices = snx_vk_physical_devices(r->instance, &count);
    int made = 0;
    if (!count) snprintf(reasons, reasons_n, "the instance lists no physical device");
    for (uint32_t i = 0; i < count && !made; i++) {
        VkPhysicalDeviceProperties props;
        char why[600];
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.apiVersion < r->api) {
            snprintf(why, sizeof why, "has Vulkan %u.%u, below the %u.%u it needs",
                     VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                     VK_API_VERSION_MAJOR(r->api), VK_API_VERSION_MINOR(r->api));
            pass_over(r, say, props.deviceName, why, reasons, reasons_n);
            continue;
        }
        /* the bans told the device, then its list: the settings first, an off one out and an on one over its bans,
           then the extensions a driver is banned from or lacked a command of out, before the renderer reads it */
        VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
        vkGetPhysicalDeviceProperties2(devices[i], &p2);
        VkBanDevice bd = {props.deviceName,    driver.driverName, driver.driverInfo, (uint32_t)driver.driverID,
                          props.driverVersion, props.deviceID,    vk_ban_system()};
        vk_bans_device(&bd);
        vk_bans_ignored(say);
        if (!snx_vk_device_extensions(devices[i], list)) {
            pass_over(r, say, props.deviceName, "gives no list of its extensions", reasons, reasons_n);
            continue;
        }
        if (!vk_extension_settings(list, r->settings, r->setting_count, r->need, r->needs, err, n)) {
            made = -1;
            break;
        }
        vk_bans_filter(list, NULL); /* each ban said below, for what the renderer asks about */
        why[0] = 0;
        uint32_t family = snx_vk_queue_family(devices[i], r->queues, r->surface, 0);
        if (family == UINT32_MAX) {
            char queues[160];
            queue_text(r->queues, r->surface, queues, sizeof queues);
            snprintf(why, sizeof why, "has no queue family of %s", queues);
        }
        for (; family != UINT32_MAX && !made;
             family = snx_vk_queue_family(devices[i], r->queues, r->surface, family + 1)) {
            *c = (SnxVkChoice){.phys = devices[i],
                               .family = family,
                               .properties = props,
                               .again = again,
                               .list = list,
                               .settings = r->settings,
                               .setting_count = r->setting_count};
            bool chosen = r->choose(c, r->user);
            if (!c->error[0] && chosen && !c->device)
                snprintf(c->error, sizeof c->error, "the renderer took %s without making its device", props.deviceName);
            if (c->error[0]) {
                if (c->device) drop(c->device);
                snprintf(err, n, "%s", c->error);
                made = -1;
            } else if (chosen)
                made = 1;
            else {
                if (c->device) drop(c->device); /* made, then passed over */
                c->device = VK_NULL_HANDLE;
                snprintf(why, sizeof why, "%s", c->why[0] ? c->why : "is not one the renderer takes");
            }
        }
        if (made) break;
        pass_over(r, say, props.deviceName, why, reasons, reasons_n);
        snx_vk_extensions_free(list);
    }
    free(devices);
    if (made < 0) snx_vk_extensions_free(list);
    return made;
}

/* the line that says the device made, with what */
static void say_made(const SnxVkDeviceRequest* r, const SnxVkChoice* c, VkBanSay say) {
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    char line[3072];
    size_t w = (size_t)snprintf(line, sizeof line,
                                "%s: the device is %s, driver %s%s%s, Vulkan %u.%u, queue family %u, made with %s",
                                r->who, c->properties.deviceName, driver.driverName, driver.driverInfo[0] ? " " : "",
                                driver.driverInfo, VK_API_VERSION_MAJOR(r->api), VK_API_VERSION_MINOR(r->api),
                                c->family, c->extension_count ? "" : "no extension");
    for (uint32_t i = 0; i < c->extension_count && w < sizeof line; i++)
        w += (size_t)snprintf(line + w, sizeof line - w, "%s%s", i ? ", " : "", c->extensions[i]);
    say(line, 0);
}

bool snx_vk_device_make(const SnxVkDeviceRequest* r, SnxVkDevice* out, char* err, size_t n) {
    VkBanSay say = r->say ? r->say : say_stderr;
    *out = (SnxVkDevice){0};
    /* the drivers known to break an extension or feature, read before any is turned on: a table the build carries
       that does not read is the build's fault; a key of vulkan_ban_ignore the table has not, or a hidden command no
       extension requires, is the player's, who meant something that would otherwise go untested */
    if (!vk_bans_load(r->bans ? r->bans : snx_vk_extension_bans_text, err, n) ||
        !vk_bans_ignore(r->ban_ignore ? r->ban_ignore : "", err, n) || !snx_vk_hide_check(r->hide, err, n))
        return false;
    SnxVkChoice c;
    SnxVkExtensions list = {0};
    /* made again once at most: a device made again that lacks a command fails (snx_vk_device_commands) */
    for (int again = 0;; again = 1) {
        char reasons[2048] = "";
        int made = pass(r, again, say, &c, &list, reasons, sizeof reasons, err, n);
        if (made < 0) return false;
        if (!made) {
            snprintf(err, n, "%s: %s", r->none, reasons);
            return false;
        }
        /* each command of each extension it is made with, as its driver gives them: one with none takes the
           extension out of the device made again, once; one the renderer can't go without fails it */
        volkLoadDevice(c.device);
        int lacks = snx_vk_device_commands(c.device, c.properties.deviceName, r->api, c.extensions, c.extension_count,
                                           r->need, r->needs, r->hide, again, say, err, n);
        if (!lacks) break;
        vkDestroyDevice(c.device, NULL);
        snx_vk_extensions_free(&list);
        if (lacks < 0) return false;
    }
    /* each extension the renderer asked about: the bans that hold for it, and its setting, on or off and why */
    for (uint32_t i = 0; i < c.asked_count; i++) {
        vk_bans_say(c.asked[i], say);
        vk_extension_setting_say(&list, r->api, c.asked[i], say);
    }
    snx_vk_extensions_free(&list);
    say_made(r, &c, say);
    *out = (SnxVkDevice){c.phys, c.device, c.family};
    return true;
}
