/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The commands each Vulkan device extension requires (common/gpu/vk/extension_commands.c, the table tools/vkcmdgen
   generates from the registry the build has), with no GPU: the table sorted, every depends of it parsing, the
   commands of the extensions the renderer has met missing (VK_EXT_depth_clamp_control's vkCmdSetDepthClampRangeEXT),
   a physical device's command left out; the registry's depends, , and + binding alike from the left; a stand-in
   device's check: a command its driver gives no pointer for, one the hide list names, a command under a depends that
   holds only with another extension or a version, an extension the table has not refused, and more than fit counted;
   a device just made, as each renderer checks it: made again without an extension it can go without, failing for one
   it can't or for a second device that lacks one; and the hide list checked, a name of no device command refused with
   the nearest */
#include "common/gpu/vk/extension_commands.h"
#include <stdio.h>
#include <string.h>

static struct {
    unsigned checks, failures;
    const char* missing[8]; /* the stand-in driver's commands it gives no pointer for */
} g_test;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        g_test.checks++;                                                                                               \
        if (!(c)) {                                                                                                    \
            g_test.failures++;                                                                                         \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
        }                                                                                                              \
    } while (0)

/* the entry's command named name: its depends, or "-" for none, NULL where the entry lists no such command */
static const char* depends_of(const char* extension, const char* name) {
    const SnxVkExtensionEntry* e = snx_vk_extension_entry(extension);
    for (uint32_t i = 0; e && i < e->count; i++) {
        const SnxVkExtensionCommand* c = &snx_vk_extension_commands[e->first + i];
        if (!strcmp(c->name, name)) return c->depends ? c->depends : "-";
    }
    return NULL;
}
static int none_hold(const char* name, size_t n, void* ctx) {
    (void)name, (void)n, (void)ctx;
    return 0;
}

static void check_table(void) {
    int sorted = snx_vk_extension_table_count > 300, parse = 1, instance = 0;
    for (uint32_t i = 0; i < snx_vk_extension_table_count; i++) {
        const SnxVkExtensionEntry* e = &snx_vk_extension_table[i];
        if (i && strcmp(snx_vk_extension_table[i - 1].name, e->name) >= 0) sorted = 0;
        for (uint32_t k = 0; k < e->count; k++) {
            const SnxVkExtensionCommand* c = &snx_vk_extension_commands[e->first + k];
            if (c->depends && snx_vk_depends(c->depends, none_hold, NULL) < 0) {
                printf("  %s of %s: \"%s\" does not parse\n", c->name, e->name, c->depends);
                parse = 0;
            }
            instance |= !strncmp(c->name, "vkGetPhysicalDevice", 19) || !strncmp(c->name, "vkCreateInstance", 16);
        }
    }
    CHECK(sorted, "the table has the registry's device extensions, sorted by name");
    CHECK(parse, "every depends of the table parses");
    CHECK(!instance, "no physical device's or instance's command: vkGetDeviceProcAddr gives none of those");
    CHECK(depends_of("VK_EXT_depth_clamp_control", "vkCmdSetDepthClampRangeEXT") &&
              !strcmp(depends_of("VK_EXT_depth_clamp_control", "vkCmdSetDepthClampRangeEXT"), "-"),
          "VK_EXT_depth_clamp_control requires vkCmdSetDepthClampRangeEXT with no condition");
    CHECK(depends_of("VK_KHR_swapchain", "vkAcquireNextImage2KHR") &&
              !strcmp(depends_of("VK_KHR_swapchain", "vkAcquireNextImage2KHR"), "VK_VERSION_1_1"),
          "a command the registry gives under a version keeps it");
    CHECK(depends_of("VK_EXT_extended_dynamic_state3", "vkCmdSetDepthClipEnableEXT") &&
              !strcmp(depends_of("VK_EXT_extended_dynamic_state3", "vkCmdSetDepthClipEnableEXT"),
                      "VK_EXT_depth_clip_enable"),
          "and one under another extension keeps that");
    CHECK(!depends_of("VK_EXT_sample_locations", "vkGetPhysicalDeviceMultisamplePropertiesEXT") &&
              depends_of("VK_EXT_sample_locations", "vkCmdSetSampleLocationsEXT"),
          "a physical device's command of an extension is left out, its device command kept");
    const SnxVkExtensionEntry* none = snx_vk_extension_entry("VK_EXT_depth_range_unrestricted");
    CHECK(none && !none->count, "an extension with no command is in the table, with none");
    CHECK(!snx_vk_extension_entry("VK_EXT_no_such_extension") && !snx_vk_extension_entry(""),
          "a name the registry has not is none");
}

/* "A", "B"... hold as the bits of ctx: A bit 0 */
static int bits_hold(const char* name, size_t n, void* ctx) {
    return n == 1 && name[0] >= 'A' && name[0] <= 'Z' && (*(unsigned*)ctx >> (name[0] - 'A') & 1);
}
static int depends(const char* expr, unsigned bits) { return snx_vk_depends(expr, bits_hold, &bits); }

static void check_depends(void) {
    CHECK(depends("A", 1) == 1 && depends("A", 0) == 0, "a name holds where it holds");
    CHECK(depends("A,B", 2) == 1 && depends("A,B", 0) == 0, ", is or");
    CHECK(depends("A+B", 3) == 1 && depends("A+B", 1) == 0, "+ is and");
    /* A=1 B=0 C=0: (A,B)+C is 0, where A,(B+C) would be 1 */
    CHECK(depends("A,B+C", 1) == 0 && depends("A+B,C", 4) == 1, "both bind alike, from the left");
    CHECK(depends("A,(B+C)", 1) == 1 && depends("(A,B)+(C,D)", 9) == 1 && depends("(A,B)+(C,D)", 3) == 0,
          "parentheses group");
    CHECK(depends("VkPhysicalDeviceVulkan12Features::descriptorIndexing", 0) == 0, "a feature's name is a name");
    static const char* const bad[] = {"", "A,", ",A", "(A", "A)", "A++B", "A B", "()", "A,(B", "A-B"};
    int refused = 1;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        if (depends(bad[i], ~0u) != -1) {
            printf("  \"%s\" parsed\n", bad[i]);
            refused = 0;
        }
    CHECK(refused, "what does not parse is -1");
    char deep[80] = "";
    for (int i = 0; i < 33; i++) strcat(deep, "(");
    strcat(deep, "A");
    for (int i = 0; i < 33; i++) strcat(deep, ")");
    CHECK(depends(deep, 1) == -1 && depends("((((A))))", 1) == 1, "parentheses past 32 deep are refused");
}

/* the stand-in driver: a pointer for every command but g_test.missing's */
static void present(void) {}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL stand_in(VkDevice dev, const char* name) {
    (void)dev;
    for (int i = 0; i < 8 && g_test.missing[i]; i++)
        if (!strcmp(g_test.missing[i], name)) return NULL;
    return present;
}
static int lacking(const char* const* enabled, uint32_t n, uint32_t api, const char* hide, SnxVkLacking* out,
                   uint32_t max, char* err) {
    return snx_vk_commands_lacking(enabled, n, api, stand_in, VK_NULL_HANDLE, hide, out, max, err, 512);
}

static void check_device(void) {
    SnxVkLacking out[4];
    char err[512] = "";
    static const char* const clamp[] = {"VK_KHR_swapchain", "VK_EXT_depth_clamp_control"};
    memset(g_test.missing, 0, sizeof g_test.missing);
    CHECK(lacking(clamp, 2, VK_API_VERSION_1_3, "", out, 4, err) == 0, "a driver with every command lacks none");
    g_test.missing[0] = "vkCmdSetDepthClampRangeEXT";
    CHECK(lacking(clamp, 2, VK_API_VERSION_1_3, "", out, 4, err) == 1 && out[0].extension == clamp[1] &&
              !strcmp(out[0].command, "vkCmdSetDepthClampRangeEXT") && !out[0].hidden,
          "NVIDIA's VK_EXT_depth_clamp_control without vkCmdSetDepthClampRangeEXT: the extension and the command");
    g_test.missing[0] = NULL;
    CHECK(lacking(clamp, 2, VK_API_VERSION_1_3, " vkCmdSetDepthClampRangeEXT ,vkCmdSetCullModeEXT", out, 4, err) == 1 &&
              out[0].extension == clamp[1] && out[0].hidden,
          "a command the hide list names counts as missing, the driver's having it said");
    /* under a depends: the command counts only where the other extension, or the version, is on */
    static const char* const eds3[] = {"VK_EXT_extended_dynamic_state3"};
    static const char* const eds3_clip[] = {"VK_EXT_extended_dynamic_state3", "VK_EXT_depth_clip_enable"};
    g_test.missing[0] = "vkCmdSetDepthClipEnableEXT";
    CHECK(lacking(eds3, 1, VK_API_VERSION_1_3, NULL, out, 4, err) == 0,
          "vkCmdSetDepthClipEnableEXT is no matter without VK_EXT_depth_clip_enable");
    CHECK(lacking(eds3_clip, 2, VK_API_VERSION_1_3, NULL, out, 4, err) == 1 && out[0].extension == eds3_clip[0] &&
              !strcmp(out[0].command, "vkCmdSetDepthClipEnableEXT"),
          "and lacking with it");
    static const char* const swapchain[] = {"VK_KHR_swapchain"};
    g_test.missing[0] = "vkAcquireNextImage2KHR";
    CHECK(lacking(swapchain, 1, VK_API_VERSION_1_0, NULL, out, 4, err) == 0 &&
              lacking(swapchain, 1, VK_API_VERSION_1_1, NULL, out, 4, err) == 1,
          "a command under VK_VERSION_1_1 counts from Vulkan 1.1");
    /* more than fit: counted, max of them written */
    g_test.missing[0] = "vkCmdSetPolygonModeEXT";
    g_test.missing[1] = "vkCmdSetRasterizationSamplesEXT";
    g_test.missing[2] = "vkCmdSetSampleMaskEXT";
    out[2].command = "untouched";
    CHECK(lacking(eds3, 1, VK_API_VERSION_1_3, NULL, out, 2, err) == 3 && !strcmp(out[2].command, "untouched"),
          "three lacking, two room: three counted, two written");
    /* the table is the build's: an extension it has not stops the check */
    static const char* const unknown[] = {"VK_KHR_swapchain", "VK_EXT_no_such_extension"};
    CHECK(lacking(unknown, 2, VK_API_VERSION_1_3, NULL, out, 4, err) == -1 &&
              strstr(err, "VK_EXT_no_such_extension, which the command table"),
          "an enabled extension the table has not is refused, named");
}

static char g_said[8][1024];
static unsigned g_nsaid;
static void say(const char* line, int warn) {
    if (g_nsaid < 8) snprintf(g_said[g_nsaid++], sizeof g_said[0], "%s%s", warn ? "warn: " : "", line);
}
static int said(const char* text) {
    for (unsigned i = 0; i < g_nsaid; i++)
        if (strstr(g_said[i], text)) return 1;
    return 0;
}

/* a device just made, as each renderer checks it (snx_vk_device_commands, through volk's vkGetDeviceProcAddr): an
   extension it can go without said and kept for the device made again, whose filter takes it out; one it can't go
   without, a second device lacking one, and the hide list's count */
static void check_device_commands(void) {
    static const char* const enabled[] = {"VK_KHR_swapchain", "VK_EXT_depth_clamp_control"};
    const VkExtensionNeed need[] = {{"VK_KHR_swapchain", "the window is drawn with it"}};
    char err[512] = "";
    vkGetDeviceProcAddr = stand_in;
    memset(g_test.missing, 0, sizeof g_test.missing);
    CHECK(vk_bans_load(snx_vk_extension_bans_text, err, sizeof err), err);
    VkBanDevice device = {"Stand-in GPU", "stand-in", "1.0", 0, 0, 0, 0};
    vk_bans_device(&device);
    g_nsaid = 0;
    CHECK(snx_vk_device_commands(VK_NULL_HANDLE, "Stand-in GPU", VK_API_VERSION_1_3, enabled, 2, need, 1, "", 0, say,
                                 err, sizeof err) == 0 &&
              g_nsaid == 0,
          "a device with every command stands, unsaid");
    g_test.missing[0] = "vkCmdSetDepthClampRangeEXT";
    CHECK(snx_vk_device_commands(VK_NULL_HANDLE, "Stand-in GPU", VK_API_VERSION_1_3, enabled, 2, need, 1, NULL, 0, say,
                                 err, sizeof err) == 1 &&
              said("warn: vulkan: Stand-in GPU's driver lists VK_EXT_depth_clamp_control without "
                   "vkCmdSetDepthClampRangeEXT, which it requires: the device is made again without it"),
          "an extension it can go without: said, a warning, and made again");
    VkExtensionProperties list[2];
    SnxVkExtensions e = {list, 2};
    snprintf(list[0].extensionName, sizeof list[0].extensionName, "%s", "VK_KHR_swapchain");
    snprintf(list[1].extensionName, sizeof list[1].extensionName, "%s", "VK_EXT_depth_clamp_control");
    CHECK(vk_bans_filter(&e, say) == 1 && e.count == 1 && !strcmp(list[0].extensionName, "VK_KHR_swapchain"),
          "the device made again goes without it: the filter takes it out");
    CHECK(snx_vk_device_commands(VK_NULL_HANDLE, "Stand-in GPU", VK_API_VERSION_1_3, enabled, 2, need, 1, NULL, 1, say,
                                 err, sizeof err) == -1 &&
              strstr(err, "Stand-in GPU, made again without what its first device lacked, lacks a command again: its "
                          "driver lists VK_EXT_depth_clamp_control without vkCmdSetDepthClampRangeEXT"),
          "a device made again that still lacks one does not stand, saying what");
    g_test.missing[0] = "vkAcquireNextImage2KHR";
    CHECK(snx_vk_device_commands(VK_NULL_HANDLE, "Stand-in GPU", VK_API_VERSION_1_3, enabled, 2, need, 1, NULL, 0, say,
                                 err, sizeof err) == -1 &&
              !strcmp(err, "Stand-in GPU's driver lists VK_KHR_swapchain without vkAcquireNextImage2KHR, which it "
                           "requires, and ScarletNX can't run without the extension on this device: the window is "
                           "drawn with it"),
          "one it can't go without fails the device, saying why");
    g_test.missing[0] = NULL;
    g_nsaid = 0;
    CHECK(snx_vk_device_commands(VK_NULL_HANDLE, "Stand-in GPU", VK_API_VERSION_1_3, enabled, 2, need, 1,
                                 "vkCmdSetDepthClampRangeEXT", 0, say, err, sizeof err) == 1 &&
              said("vulkan: vk_debug_hide_commands=vkCmdSetDepthClampRangeEXT hides 1 command of the extensions "
                   "Stand-in GPU is made with") &&
              said("(vk_debug_hide_commands hides it from the check)"),
          "the hide list: how many it hides, and each said as hidden");
}

static void check_hide(void) {
    char err[512] = "";
    CHECK(snx_vk_hide_check("", err, sizeof err) && snx_vk_hide_check(NULL, err, sizeof err), "an empty list");
    CHECK(snx_vk_hide_check("vkCmdSetDepthClampRangeEXT, vkCmdBeginConditionalRenderingEXT", err, sizeof err), err);
    CHECK(!snx_vk_hide_check("vkCmdSetDepthClampRangeEXT,vkCmdSetDepthClampRange", err, sizeof err) &&
              strstr(err, "vk_debug_hide_commands=vkCmdSetDepthClampRangeEXT,vkCmdSetDepthClampRange: "
                          "vkCmdSetDepthClampRange is no device command a Vulkan extension requires; the nearest: "
                          "vkCmdSetDepthClampRangeEXT, "),
          "a name of no command is refused, with the value given and the nearest first");
    CHECK(!snx_vk_hide_check("vkGetPhysicalDeviceMultisamplePropertiesEXT", err, sizeof err),
          "a physical device's command is no device command");
    CHECK(snx_vk_hide_has("a, vkCmdX ,b", "vkCmdX") && !snx_vk_hide_has("vkCmdXY", "vkCmdX") &&
              !snx_vk_hide_has(NULL, "vkCmdX"),
          "the hide list's names, commas and spaces between");
}

int main(void) {
    check_table();
    check_depends();
    check_device();
    check_device_commands();
    check_hide();
    printf("%s: %u checks, %u failed\n", g_test.failures ? "FAIL" : "PASS", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
