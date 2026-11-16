/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The one way a renderer makes its Vulkan device (common/gpu/vk/device.c), with stand-in drivers and no GPU: the
   settings applied before the bans (an on one going over a ban, an off one taking the extension out), off refused for
   what the renderer can't run without, an extension no setting names refused where the renderer takes settings and
   taken where it takes none, an extension whose driver lacks a command (or whose command vk_debug_hide_commands hides)
   taken out of the device made again, one the renderer needs failing it, the devices passed over in the loader's order
   each with its reason, the one a rating rates highest alone, each device's needs and the version the renderer runs it
   at, the queue family the renderer's queues need, vulkan_ban_ignore going over a ban, and the log: each setting of an
   extension the renderer asked about, and the device made */
#include "common/gpu/vk/device.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GPUS 3
typedef struct {
    const char* name;
    uint32_t api;
    const char* extensions[8];
    VkQueueFlags families[2];
    uint32_t family_count;
    VkResult create; /* what vkCreateDevice gives */
} Gpu;
static struct {
    unsigned checks, failures;
    Gpu gpus[GPUS];
    uint32_t gpu_count;
    const char* missing[2]; /* the commands the stand-in driver gives no pointer for */
    /* the renderer's choose: what it turns on, a device it passes over, and what it saw */
    const char* enable[6];
    const char* refuse;
    int agains;
    /* each device made: its extensions, comma-separated */
    char made[4][512];
    unsigned creates, destroys;
    char log[16384];
    VkExtensionSetting settings[5]; /* the player's vk_ext_*, as reset leaves them */
} g_test;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        g_test.checks++;                                                                                               \
        if (!(c)) {                                                                                                    \
            g_test.failures++;                                                                                         \
            printf("FAIL line %d: %s\nthe log:\n%s", __LINE__, what, g_test.log);                                      \
        }                                                                                                              \
    } while (0)

/* ---- the stand-in driver: each physical device is its index from 1 ---- */

static const Gpu* gpu_of(VkPhysicalDevice phys) { return &g_test.gpus[(uintptr_t)phys - 1]; }
static VKAPI_ATTR VkResult VKAPI_CALL enumerate(VkInstance instance, uint32_t* n, VkPhysicalDevice* out) {
    (void)instance;
    if (out)
        for (uint32_t i = 0; i < *n && i < g_test.gpu_count; i++) out[i] = (VkPhysicalDevice)(uintptr_t)(i + 1);
    *n = g_test.gpu_count;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice phys, VkPhysicalDeviceProperties* p) {
    *p = (VkPhysicalDeviceProperties){.apiVersion = gpu_of(phys)->api, .driverVersion = VK_MAKE_VERSION(26, 2, 6)};
    snprintf(p->deviceName, sizeof p->deviceName, "%s", gpu_of(phys)->name);
}
static VKAPI_ATTR void VKAPI_CALL properties2(VkPhysicalDevice phys, VkPhysicalDeviceProperties2* p) {
    properties(phys, &p->properties);
    for (VkBaseOutStructure* s = p->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
            VkPhysicalDeviceDriverProperties* d = (VkPhysicalDeviceDriverProperties*)s;
            d->driverID = VK_DRIVER_ID_MESA_RADV;
            snprintf(d->driverName, sizeof d->driverName, "radv");
            snprintf(d->driverInfo, sizeof d->driverInfo, "Mesa 26.2.6");
        }
}
static VKAPI_ATTR VkResult VKAPI_CALL extensions(VkPhysicalDevice phys, const char* layer, uint32_t* n,
                                                 VkExtensionProperties* out) {
    (void)layer;
    uint32_t count = 0;
    while (count < 8 && gpu_of(phys)->extensions[count]) count++;
    if (out)
        for (uint32_t i = 0; i < *n && i < count; i++)
            out[i] = (VkExtensionProperties){.specVersion = 1},
            snprintf(out[i].extensionName, sizeof out[i].extensionName, "%s", gpu_of(phys)->extensions[i]);
    *n = count;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL families(VkPhysicalDevice phys, uint32_t* n, VkQueueFamilyProperties* out) {
    if (out)
        for (uint32_t i = 0; i < *n && i < gpu_of(phys)->family_count; i++)
            out[i] = (VkQueueFamilyProperties){.queueFlags = gpu_of(phys)->families[i], .queueCount = 1};
    *n = gpu_of(phys)->family_count;
}
static VKAPI_ATTR VkResult VKAPI_CALL create(VkPhysicalDevice phys, const VkDeviceCreateInfo* info,
                                             const VkAllocationCallbacks* allocator, VkDevice* out) {
    (void)allocator;
    if (gpu_of(phys)->create != VK_SUCCESS) return gpu_of(phys)->create;
    char* made = g_test.made[g_test.creates < 4 ? g_test.creates : 3];
    made[0] = 0;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++)
        snprintf(made + strlen(made), 512 - strlen(made), "%s%s", i ? "," : "", info->ppEnabledExtensionNames[i]);
    g_test.creates++;
    *out = (VkDevice)(uintptr_t)0x1000;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice dev, const VkAllocationCallbacks* allocator) {
    (void)dev, (void)allocator;
    g_test.destroys++;
}
static void present(void) {}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice dev, const char* name) {
    (void)dev;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)destroy;
    for (int i = 0; i < 2; i++)
        if (g_test.missing[i] && !strcmp(g_test.missing[i], name)) return NULL;
    return present;
}

/* ---- the renderer ---- */

static void say(const char* line, int warn) {
    size_t w = strlen(g_test.log);
    snprintf(g_test.log + w, sizeof g_test.log - w, "%s%s\n", warn ? "warn: " : "", line);
}
static int said(const char* text) { return strstr(g_test.log, text) != NULL; }

static bool choose(SnxVkChoice* c, void* user) {
    (void)user;
    g_test.agains += c->again;
    if (g_test.refuse && !strcmp(c->properties.deviceName, g_test.refuse))
        return snx_vk_choice_refuse(c, "has no dynamicRendering");
    for (int i = 0; i < 6 && g_test.enable[i]; i++) snx_vk_choice_enable(c, g_test.enable[i]);
    VkPhysicalDeviceFeatures2 want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    return snx_vk_choice_create(c, &want, NULL, NULL, 0);
}

#define HEAD "key | action | driver | versions | system | devices | breaks | source | retest | checked | override\n"
static const char g_bans[] =
    HEAD "VK_EXT_custom_border_color | off | radv | * | any | * | draws wrong | a test | headless: | never | -\n"
         "VK_EXT_shader_object | off | radv | * | any | * | draws wrong | a test | headless: | never | -\n"
         "VK_EXT_mesh_shader | warn | radv | * | any | * | draws wrong | a test | headless: | never | -\n";
static const VkExtensionNeed g_need[] = {{"VK_KHR_swapchain", "the window is drawn with it"}};

/* one stand-in GPU, Stand-in GPU A, with every extension the cases turn on and a graphics and compute family; the
   settings below, nothing missing, hidden or refused */
static void reset(void) {
    memset(&g_test.gpus, 0, sizeof g_test.gpus);
    g_test.gpus[0] = (Gpu){"Stand-in GPU A",
                           VK_API_VERSION_1_3,
                           {"VK_KHR_swapchain", "VK_EXT_custom_border_color", "VK_EXT_index_type_uint8",
                            "VK_EXT_shader_object", "VK_EXT_mesh_shader", "VK_EXT_conditional_rendering"},
                           {VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT},
                           1,
                           VK_SUCCESS};
    g_test.gpu_count = 1;
    memset(g_test.missing, 0, sizeof g_test.missing);
    memset(g_test.enable, 0, sizeof g_test.enable);
    memset(g_test.made, 0, sizeof g_test.made);
    g_test.refuse = NULL;
    g_test.agains = 0;
    g_test.creates = g_test.destroys = 0;
    g_test.log[0] = 0;
    static const VkExtensionSetting settings[5] = {{"VK_KHR_swapchain", "auto", 0},
                                                   {"VK_EXT_custom_border_color", "on", 0},
                                                   {"VK_EXT_index_type_uint8", "off", 14},
                                                   {"VK_EXT_shader_object", "auto", 0},
                                                   {"VK_EXT_mesh_shader", "auto", 0}};
    memcpy(g_test.settings, settings, sizeof settings);
}
static bool make(SnxVkDeviceRequest* r, SnxVkDevice* out, char* err) {
    *r = (SnxVkDeviceRequest){.who = "test gpu",
                              .none = "no stand-in device",
                              .api = VK_API_VERSION_1_3,
                              .queues = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
                              .choose = choose,
                              .need = g_need,
                              .needs = 1,
                              .settings = g_test.settings,
                              .setting_count = sizeof g_test.settings / sizeof *g_test.settings,
                              .bans = g_bans,
                              .say = say};
    return snx_vk_device_make(r, out, err, 512);
}

static void check_settings_before_bans(void) {
    SnxVkDeviceRequest r;
    SnxVkDevice d;
    char err[512] = "";
    reset();
    g_test.enable[0] = "VK_KHR_swapchain";
    g_test.enable[1] = "VK_EXT_custom_border_color";
    g_test.enable[2] = "VK_EXT_index_type_uint8";
    g_test.enable[3] = "VK_EXT_shader_object";
    CHECK(make(&r, &d, err) && d.device && d.family == 0, err);
    CHECK(!strcmp(g_test.made[0], "VK_KHR_swapchain,VK_EXT_custom_border_color"),
          "on goes over the ban (the settings before the bans), off takes it out, the ban holds for auto");
    CHECK(said("vulkan: VK_EXT_custom_border_color: on (vk_ext_custom_border_color=on goes over the ban: ") &&
              said("vulkan: VK_EXT_index_type_uint8: off (vk_ext_index_type_uint8=off)\n") &&
              said("vulkan: VK_EXT_shader_object: off (auto, as the ban holds: ") &&
              said("vulkan: VK_KHR_swapchain: on (auto: the driver has it, and no ban holds)\n"),
          "each setting of an extension the renderer asked about is said");
    CHECK(said("vulkan: VK_EXT_shader_object is off on Stand-in GPU A (radv, Mesa 26.2.6, driver version 26.2.6): "
               "RADV draws wrong (a test; extension_bans.txt:3)\n"),
          "and the ban that took one out");
    CHECK(!said("VK_EXT_mesh_shader"), "and nothing of one it did not, its ban neither");
    CHECK(said("test gpu: the device is Stand-in GPU A, driver radv Mesa 26.2.6, Vulkan 1.3, queue family 0, made with "
               "VK_KHR_swapchain, VK_EXT_custom_border_color\n"),
          "the device made is said, with its extensions");
    CHECK(g_test.creates == 1 && g_test.destroys == 0 && !g_test.agains, "made once");

    reset();
    g_test.settings[0].value = "off";
    g_test.enable[0] = "VK_KHR_swapchain";
    CHECK(!make(&r, &d, err) &&
              !strcmp(err, "vk_khr_swapchain=off turns off VK_KHR_swapchain, which ScarletNX can't run without on "
                           "this device: the window is drawn with it") &&
              !g_test.creates,
          "off for what the renderer can't run without fails the device, saying the value");
    CHECK(!d.passed_over && d.refused == SNX_VK_REFUSED_SETTING, "a setting's error, whatever the renderer would ask");

    reset();
    g_test.enable[0] = "VK_KHR_swapchain";
    g_test.enable[1] = "VK_EXT_shader_object";
    CHECK(make(&r, &d, err), err);
    CHECK(!strcmp(g_test.made[0], "VK_KHR_swapchain"), "the ban takes it out for auto");
}

static void check_no_setting(void) {
    SnxVkDeviceRequest r;
    SnxVkDevice d;
    char err[512] = "";
    reset();
    g_test.enable[0] = "VK_KHR_swapchain";
    g_test.enable[1] = "VK_EXT_conditional_rendering";
    CHECK(!make(&r, &d, err) &&
              !strcmp(err, "the renderer turns on VK_EXT_conditional_rendering, which no setting names (a \"setting "
                           "@VK_EXT_conditional_rendering\" line in src/nx/runtime/settings.txt): every extension a "
                           "device is made with is one the player can turn off"),
          "an extension no setting names is refused, naming it");
    CHECK(g_test.destroys == g_test.creates, "the device choose made anyway is destroyed");

    /* the Android apps' renderer takes no settings: the bans and the check alone */
    reset();
    g_test.enable[0] = "VK_KHR_swapchain";
    g_test.enable[1] = "VK_EXT_conditional_rendering";
    g_test.enable[2] = "VK_EXT_custom_border_color";
    r = (SnxVkDeviceRequest){.who = "test gpu",
                             .none = "no stand-in device",
                             .api = VK_API_VERSION_1_3,
                             .queues = VK_QUEUE_GRAPHICS_BIT,
                             .choose = choose,
                             .bans = g_bans,
                             .say = say};
    CHECK(snx_vk_device_make(&r, &d, err, sizeof err), err);
    CHECK(!strcmp(g_test.made[0], "VK_KHR_swapchain,VK_EXT_conditional_rendering"),
          "without settings an extension is taken as the driver lists it, and the bans hold");
    CHECK(said("vulkan: VK_EXT_custom_border_color is off on Stand-in GPU A (radv, Mesa 26.2.6, driver version "
               "26.2.6): ") &&
              !said("vulkan: VK_KHR_swapchain: "),
          "the ban that took one out is said, and no setting");
}

static void check_commands(void) {
    SnxVkDeviceRequest r;
    SnxVkDevice d;
    char err[512] = "";
    reset();
    g_test.settings[3].value = "on";
    g_test.missing[0] = "vkCreateShadersEXT";
    g_test.enable[0] = "VK_KHR_swapchain";
    g_test.enable[1] = "VK_EXT_shader_object";
    CHECK(make(&r, &d, err), err);
    CHECK(g_test.creates == 2 && g_test.destroys == 1 && g_test.agains == 1 &&
              !strcmp(g_test.made[0], "VK_KHR_swapchain,VK_EXT_shader_object") &&
              !strcmp(g_test.made[1], "VK_KHR_swapchain"),
          "a driver that lacks a command: the device made again without its extension, choose told so");
    CHECK(said("warn: vulkan: Stand-in GPU A's driver lists VK_EXT_shader_object without vkCreateShadersEXT, which it "
               "requires: the device is made again without it\n") &&
              said("vulkan: VK_EXT_shader_object: off (its driver lists it without vkCreateShadersEXT, which it "
                   "requires, which vk_ext_shader_object=on can't change)\n") &&
              said("vulkan: VK_EXT_shader_object is off on Stand-in GPU A (radv, Mesa 26.2.6, driver version 26.2.6): "
                   "its driver lists it without vkCreateShadersEXT, which it requires\n"),
          "said, and its setting says why it is off whatever the setting");

    reset();
    g_test.settings[3].value = "on";
    g_test.enable[0] = "VK_KHR_swapchain";
    g_test.enable[1] = "VK_EXT_shader_object";
    SnxVkDeviceRequest hidden = {.who = "test gpu",
                                 .none = "no stand-in device",
                                 .api = VK_API_VERSION_1_3,
                                 .queues = VK_QUEUE_GRAPHICS_BIT,
                                 .choose = choose,
                                 .need = g_need,
                                 .needs = 1,
                                 .settings = g_test.settings,
                                 .setting_count = sizeof g_test.settings / sizeof *g_test.settings,
                                 .hide = "vkCreateShadersEXT",
                                 .bans = g_bans,
                                 .say = say};
    CHECK(snx_vk_device_make(&hidden, &d, err, sizeof err), err);
    CHECK(g_test.creates == 2 && !strcmp(g_test.made[1], "VK_KHR_swapchain") &&
              said("vulkan: vk_debug_hide_commands=vkCreateShadersEXT hides 1 command of the extensions Stand-in GPU A "
                   "is made with\n") &&
              said("(vk_debug_hide_commands hides it from the check): the device is made again without it"),
          "a command vk_debug_hide_commands hides is taken as missing, and said as hidden");

    reset();
    g_test.missing[0] = "vkCreateSwapchainKHR";
    g_test.enable[0] = "VK_KHR_swapchain";
    CHECK(!make(&r, &d, err) &&
              !strcmp(err, "Stand-in GPU A's driver lists VK_KHR_swapchain without vkCreateSwapchainKHR, which it "
                           "requires, and ScarletNX can't run without the extension on this device: the window is "
                           "drawn with it") &&
              g_test.creates == 1 && g_test.destroys == 1,
          "a command lacking of what the renderer can't go without fails the device, destroyed");
}

static void check_devices(void) {
    SnxVkDeviceRequest r;
    SnxVkDevice d;
    char err[512] = "";
    reset();
    g_test.gpus[1] = g_test.gpus[0];
    g_test.gpus[2] = g_test.gpus[0];
    g_test.gpus[0].name = "Stand-in GPU Old";
    g_test.gpus[0].api = VK_API_VERSION_1_1;
    g_test.gpus[1].name = "Stand-in GPU B";
    g_test.gpus[1].create = VK_ERROR_INITIALIZATION_FAILED;
    g_test.gpus[2].name = "Stand-in GPU C";
    g_test.gpus[2].families[0] = VK_QUEUE_TRANSFER_BIT;
    g_test.gpu_count = 3;
    g_test.enable[0] = "VK_KHR_swapchain";
    CHECK(!make(&r, &d, err) &&
              !strcmp(err, "no stand-in device: Stand-in GPU Old has Vulkan 1.1, below the 1.3 it needs; Stand-in "
                           "GPU B refuses the device: vkCreateDevice gave VK_ERROR_INITIALIZATION_FAILED (-3); "
                           "Stand-in GPU C has no queue family of graphics and compute queues"),
          "no device left: each passed over, in the loader's order, with its reason");
    CHECK(d.passed_over && !d.device && d.refused == SNX_VK_REFUSED_NONE,
          "and the device says so, as against an error that stops it");
    CHECK(said("test gpu: Stand-in GPU Old is passed over: it has Vulkan 1.1, below the 1.3 it needs\n"),
          "each said as it is passed over");

    g_test.log[0] = 0;
    g_test.gpus[1].create = VK_SUCCESS;
    g_test.refuse = "Stand-in GPU B";
    g_test.gpus[2].families[0] = VK_QUEUE_GRAPHICS_BIT;
    g_test.gpus[2].families[1] = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    g_test.gpus[2].family_count = 2;
    CHECK(make(&r, &d, err) && d.phys == (VkPhysicalDevice)(uintptr_t)3 && d.family == 1, err);
    CHECK(said("test gpu: Stand-in GPU B is passed over: it has no dynamicRendering\n") &&
              said("test gpu: the device is Stand-in GPU C, driver radv Mesa 26.2.6, Vulkan 1.3, queue family 1, "
                   "made with VK_KHR_swapchain\n"),
          "the first the renderer takes, on the first family of its queues");
}

/* a rating: Stand-in GPU A 1, B 3, C 3 (B first of the two), below 0 for the rest */
static int rating(VkPhysicalDevice phys, const VkPhysicalDeviceProperties* p, void* user) {
    (void)phys, (void)user;
    return !strcmp(p->deviceName, "Stand-in GPU A")   ? 1
           : !strcmp(p->deviceName, "Stand-in GPU B") ? 3
           : !strcmp(p->deviceName, "Stand-in GPU C") ? 3
                                                      : -1;
}
/* what a device can't run without: the swapchain, and on one of Vulkan 1.2 VK_EXT_index_type_uint8 too */
static uint32_t needs_of(const VkPhysicalDeviceProperties* p, const SnxVkExtensions* list, VkExtensionNeed* need,
                         void* user) {
    (void)list, (void)user;
    need[0] = (VkExtensionNeed){"VK_KHR_swapchain", "the window is drawn with it"};
    need[1] = (VkExtensionNeed){"VK_EXT_index_type_uint8", "a 1.2 device draws 8-bit indices with it"};
    return p->apiVersion < VK_API_VERSION_1_3 ? 2 : 1;
}
static bool choose_api(SnxVkChoice* c, void* user) {
    c->api = c->properties.apiVersion; /* the device's own version, as the Switch runs a device */
    return choose(c, user);
}

/* the device a rating rates highest, alone (the Switch's way): the first of equal ones, none of the others asked
   about, and none left where it is passed over; the needs of each device, and the version choose runs it at */
static void check_best(void) {
    SnxVkDeviceRequest r;
    SnxVkDevice d;
    char err[512] = "";
    reset();
    g_test.gpus[1] = g_test.gpus[0];
    g_test.gpus[2] = g_test.gpus[0];
    g_test.gpus[1].name = "Stand-in GPU B";
    g_test.gpus[2].name = "Stand-in GPU C";
    g_test.gpu_count = 3;
    g_test.enable[0] = "VK_KHR_swapchain";
    make(&r, &d, err);
    r.score = rating;
    r.choose = choose_api;
    g_test.log[0] = 0;
    CHECK(snx_vk_device_make(&r, &d, err, sizeof err) && d.phys == (VkPhysicalDevice)(uintptr_t)2, err);
    CHECK(!said("is passed over") && said("test gpu: the device is Stand-in GPU B, driver radv Mesa 26.2.6, Vulkan "
                                          "1.3, queue family 0, made with VK_KHR_swapchain\n"),
          "the first of the two rated highest, the others not asked about");
    g_test.refuse = "Stand-in GPU B";
    CHECK(!snx_vk_device_make(&r, &d, err, sizeof err) && d.passed_over &&
              !strcmp(err, "no stand-in device: Stand-in GPU B has no dynamicRendering"),
          "the one rated highest passed over leaves none: the next is not asked about");
    g_test.refuse = NULL;
    g_test.gpus[0].api = VK_API_VERSION_1_1;
    g_test.gpus[1].name = g_test.gpus[2].name = "Stand-in GPU D";
    CHECK(!snx_vk_device_make(&r, &d, err, sizeof err) && d.passed_over &&
              !strcmp(err, "no stand-in device: Stand-in GPU A has Vulkan 1.1, below the 1.3 it needs; Stand-in GPU "
                           "D is not one it can run on (its rating is below 0); Stand-in GPU D is not one it can run "
                           "on (its rating is below 0)"),
          "none rated: each passed over, the version below the one asked for said");

    /* a device's needs and version: on a 1.2 one VK_EXT_index_type_uint8 can't be off */
    reset();
    g_test.gpus[0].api = VK_API_VERSION_1_2;
    g_test.enable[0] = "VK_KHR_swapchain";
    make(&r, &d, err);
    r.api = VK_API_VERSION_1_2;
    r.needs_of = needs_of;
    r.choose = choose_api;
    CHECK(!snx_vk_device_make(&r, &d, err, sizeof err) &&
              !strcmp(err, "vk_ext_index_type_uint8=off turns off VK_EXT_index_type_uint8, which ScarletNX can't run "
                           "without on this device: a 1.2 device draws 8-bit indices with it"),
          "a need of this device's: off refused");
    g_test.settings[2].value = "auto";
    g_test.log[0] = 0;
    CHECK(snx_vk_device_make(&r, &d, err, sizeof err) && said(", Vulkan 1.2, queue family 0"), err);
    g_test.missing[0] = "vkCreateSwapchainKHR";
    CHECK(!snx_vk_device_make(&r, &d, err, sizeof err) && strstr(err, "the window is drawn with it"),
          "the needs of the device in the command check too");
}

static void check_request(void) {
    SnxVkDeviceRequest r;
    SnxVkDevice d;
    char err[512] = "";
    reset();
    g_test.enable[0] = "VK_EXT_shader_object";
    r = (SnxVkDeviceRequest){.who = "test gpu",
                             .none = "no stand-in device",
                             .api = VK_API_VERSION_1_3,
                             .queues = VK_QUEUE_GRAPHICS_BIT,
                             .choose = choose,
                             .settings = g_test.settings,
                             .setting_count = sizeof g_test.settings / sizeof *g_test.settings,
                             .ban_ignore = "VK_EXT_shader_object",
                             .bans = g_bans,
                             .say = say};
    CHECK(snx_vk_device_make(&r, &d, err, sizeof err) && !strcmp(g_test.made[0], "VK_EXT_shader_object") &&
              said("warn: vulkan: VK_EXT_shader_object is not banned on Stand-in GPU A"),
          "vulkan_ban_ignore goes over the ban, saying it");
    r.ban_ignore = "VK_EXT_shader_objects";
    CHECK(!snx_vk_device_make(&r, &d, err, sizeof err) &&
              strstr(err, "vulkan_ban_ignore=VK_EXT_shader_objects: VK_EXT_shader_objects is no key of the extension "
                          "bans; the nearest it has: VK_EXT_shader_object") == err &&
              d.refused == SNX_VK_REFUSED_SETTING,
          "a key the table has not is refused, with the value given: a setting's error");
    r.ban_ignore = NULL;
    r.hide = "vkCreateShaderEXT";
    CHECK(!snx_vk_device_make(&r, &d, err, sizeof err) &&
              strstr(err, "vk_debug_hide_commands=vkCreateShaderEXT: vkCreateShaderEXT is no device command") &&
              d.refused == SNX_VK_REFUSED_SETTING,
          "a hidden command no extension requires is refused, with the value given: a setting's error");
    r.hide = NULL;
    r.bans = "key | action\n";
    CHECK(!snx_vk_device_make(&r, &d, err, sizeof err) && d.refused == SNX_VK_REFUSED_TABLE &&
              strstr(err, "extension_bans.txt:1: "),
          "a table that does not read is the table's error, said with its line");
    r.bans = g_bans;
    r.api = VK_API_VERSION_1_1;
    CHECK(!snx_vk_device_make(&r, &d, err, sizeof err) &&
              !strcmp(err, "test gpu asks for Vulkan 1.1, below the 1.2 whose properties the bans read a driver by"),
          "a request below Vulkan 1.2 is refused, with the version asked for");
    CHECK(d.refused == SNX_VK_REFUSED_NONE, "the renderer's own error, not a setting's");
    r.api = VK_API_VERSION_1_3;
    r.setting_count = VK_EXTENSION_SETTINGS + 1;
    CHECK(!snx_vk_device_make(&r, &d, err, sizeof err) &&
              !strcmp(err, "161 extension settings, past the 160 the renderer keeps") &&
              d.refused == SNX_VK_REFUSED_SETTING,
          "more settings than the renderer keeps are refused before any is read");
}

int main(void) {
    vkEnumeratePhysicalDevices = enumerate;
    vkGetPhysicalDeviceProperties = properties;
    vkGetPhysicalDeviceProperties2 = properties2;
    vkEnumerateDeviceExtensionProperties = extensions;
    vkGetPhysicalDeviceQueueFamilyProperties = families;
    vkCreateDevice = create;
    vkDestroyDevice = destroy;
    vkGetDeviceProcAddr = device_proc;
    check_settings_before_bans();
    check_no_setting();
    check_commands();
    check_devices();
    check_best();
    check_request();
    printf("%s: %u checks, %u failed\n", g_test.failures ? "FAIL" : "PASS", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
