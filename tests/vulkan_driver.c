/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vulkan drivers an Apple app carries (MoltenVK, and KosmicKrisp with its loader): found beside a program or in its
   bundle, picked for a run (snx_vulkan_driver_find), and put beside the runtime copy a game runs, which is outside
   the bundle where SDL finds neither the app's Frameworks nor a system loader (docs/APPLE.md) */
#define _DARWIN_C_SOURCE /* chflags and UF_IMMUTABLE, which hide behind it in a strict C99 compile */
#include "common/utilities/common.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>

static void put(const char* path, const char* text) { assert(!snx_write_file(path, text, strlen(text))); }
static void holds(const char* path, const char* text) {
    size_t size = 0;
    unsigned char* data = snx_read_file(path, &size);
    assert(data && size == strlen(text) && !memcmp(data, text, size));
    free(data);
}
/* the file a search found, held against the one expected: the same file, however the path names it */
static void named(const char* path, const char* expected) {
    char* a = path ? snx_absolute(path) : NULL;
    char* b = a ? snx_absolute(expected) : NULL;
    assert(a && b && !strcmp(a, b));
    free(a);
    free(b);
}
static void found(char* path, const char* expected) {
    named(path, expected);
    free(path);
}
/* The KosmicKrisp this Mac has, through its ICD and the Vulkan loader: an instance and a device, whose driver ID says
   that the ICD the pick named was taken. A Mac without it checks nothing: the driver needs macOS 26 or later and an
   Apple silicon GPU, and a machine that has neither, or has no mesa or Vulkan SDK, has no ICD to name */
static void kosmickrisp_device(const char* program) {
    SnxVulkanDriver pick;
    char reason[4096];
    /* a Mac without it plays auto's driver instead, which is not KosmicKrisp */
    if (!snx_vulkan_driver_choose(program, "kosmickrisp", &pick, reason, sizeof reason) || pick.instead) {
        printf("vulkan_driver: no KosmicKrisp on this Mac, so no device: %s\n", reason);
        snx_vulkan_driver_free(&pick);
        return;
    }
    assert(pick.icd && pick.library);
    assert(!setenv("VK_DRIVER_FILES", pick.icd, 1));
    {
        void* loader = dlopen(pick.library, RTLD_NOW | RTLD_LOCAL);
        PFN_vkGetInstanceProcAddr gipa =
            loader ? (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr") : NULL;
        PFN_vkCreateInstance create = gipa ? (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance") : NULL;
        VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .pApplicationName = "ScarletNX test",
                                .apiVersion = VK_API_VERSION_1_3};
        VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
        VkInstance instance = VK_NULL_HANDLE;
        VkResult r = create ? create(&ci, NULL, &instance) : VK_ERROR_INITIALIZATION_FAILED;
        if (r != VK_SUCCESS) {
            /* an ICD of a Mac older than macOS 26, or one that the loader cannot open: the pick still names it */
            printf("vulkan_driver: KosmicKrisp made no instance (%d), so no device\n", (int)r);
        } else {
            PFN_vkEnumeratePhysicalDevices list =
                (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
            PFN_vkGetPhysicalDeviceProperties2 props =
                (PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2");
            PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
            VkPhysicalDevice devices[4];
            uint32_t n = 4;
            assert(list && props && destroy && list(instance, &n, devices) == VK_SUCCESS && n >= 1);
            {
                VkPhysicalDeviceDriverProperties dp = {.sType =
                                                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                  .pNext = &dp};
                props(devices[0], &p2);
                printf("vulkan_driver: KosmicKrisp made %s: %s %s, driver ID %d\n", p2.properties.deviceName,
                       dp.driverName, dp.driverInfo, (int)dp.driverID);
                assert(dp.driverID == VK_DRIVER_ID_MESA_KOSMICKRISP);
            }
            destroy(instance, NULL);
        }
        if (loader) dlclose(loader);
    }
    snx_vulkan_driver_free(&pick);
}
int main(void) {
    char* dir = snx_temp_dir(NULL);
    char *app, *app_macos, *app_frameworks, *app_program, *app_driver;
    char *phone, *phone_frameworks, *phone_program, *phone_driver;
    char *game_dir, *game, *game_driver, *plain_dir, *plain;
    char *app_loader, *app_kosmickrisp, *app_icd, *game_loader, *game_kosmickrisp, *game_icd;
    struct stat app_st, game_st;
    assert(dir);
    app = snx_path_join(dir, "ScarletNX.app");
    app_macos = snx_path_join(app, "Contents/MacOS");
    app_frameworks = snx_path_join(app, "Contents/Frameworks");
    app_program = snx_path_join(app_macos, "scarletnx");
    app_driver = snx_path_join(app_frameworks, "libMoltenVK.dylib");
    app_loader = snx_path_join(app_frameworks, "libvulkan.1.dylib");
    app_kosmickrisp = snx_path_join(app_frameworks, "libvulkan_kosmickrisp.dylib");
    app_icd = snx_path_join(app_frameworks, "libkosmickrisp_icd.json");
    phone = snx_path_join(dir, "scarletnx.app");
    phone_frameworks = snx_path_join(phone, "Frameworks/MoltenVK.framework");
    phone_program = snx_path_join(phone, "scarletnx");
    phone_driver = snx_path_join(phone_frameworks, "MoltenVK");
    game_dir = snx_path_join(dir, "recompiled/0100000000000000");
    game = snx_path_join(game_dir, "0100000000000000");
    game_driver = snx_path_join(game_dir, "libMoltenVK.dylib");
    game_loader = snx_path_join(game_dir, "libvulkan.1.dylib");
    game_kosmickrisp = snx_path_join(game_dir, "libvulkan_kosmickrisp.dylib");
    game_icd = snx_path_join(game_dir, "libkosmickrisp_icd.json");
    plain_dir = snx_path_join(dir, "build");
    plain = snx_path_join(plain_dir, "scarletnx");
    assert(!snx_mkdirs(app_macos) && !snx_mkdirs(app_frameworks) && !snx_mkdirs(phone_frameworks));
    assert(!snx_mkdirs(game_dir) && !snx_mkdirs(plain_dir));
    put(app_program, "the app's runtime");
    put(app_driver, "MoltenVK, the app's");
    put(phone_program, "the phone app's runtime");
    put(phone_driver, "MoltenVK, the phone app's");
    put(plain, "a checkout's runtime");
    /* the app's bundle, and the framework an iOS app holds in the bundle itself; a checkout's build has neither */
    found(snx_vulkan_driver(app_program), app_driver);
    found(snx_vulkan_driver(phone_program), phone_driver);
    assert(!snx_vulkan_driver(plain));
    { /* the run's pick in the app's own folders, without the Mac's installs and environment (vulkan_driver_pick's):
         auto and moltenvk are the app's MoltenVK while it carries no KosmicKrisp; a value that is no driver is refused
         with the reason */
        SnxVulkanPlaces at = {.program = app_program};
        SnxVulkanDriver pick;
        char reason[4096];
        assert(snx_vulkan_driver_find(&at, "auto", &pick, reason, sizeof reason));
        named(pick.library, app_driver);
        assert(!pick.icd && !strcmp(pick.name, "MoltenVK"));
        snx_vulkan_driver_free(&pick);
        assert(snx_vulkan_driver_find(&at, "moltenvk", &pick, reason, sizeof reason));
        named(pick.library, app_driver);
        assert(!pick.icd && !pick.instead);
        snx_vulkan_driver_free(&pick);
        assert(!snx_vulkan_driver_find(&at, "vulkan", &pick, reason, sizeof reason));
        assert(strstr(reason, "auto, moltenvk or kosmickrisp") && !pick.library && !pick.icd);
    }
    /* the game's folder gets the app's, and its own copy of the runtime finds it first */
    assert(!snx_deploy_vulkan_driver(app_program, game_dir));
    holds(game_driver, "MoltenVK, the app's");
    found(snx_vulkan_driver(game), game_driver);
    /* a newer app's replaces the game's, whichever name the older one left it under */
    put(app_driver, "MoltenVK, the app's, newer");
    assert(!snx_deploy_vulkan_driver(app_program, game_dir));
    holds(game_driver, "MoltenVK, the app's, newer");
    /* a program the disk takes no second name from (an app on a read-only image, or one macOS translocated) leaves the
       copy: the deploy fails the run without it */
    assert(!snx_remove_tree(game_driver));
    assert(!chflags(app_driver, UF_IMMUTABLE));
    assert(snx_link_file(app_driver, game_driver)); /* the disk refuses this name */
    assert(!snx_deploy_vulkan_driver(app_program, game_dir));
    holds(game_driver, "MoltenVK, the app's, newer");
    assert(!stat(app_driver, &app_st) && !stat(game_driver, &game_st) && app_st.st_ino != game_st.st_ino);
    assert(!chflags(app_driver, 0));
    /* nothing to put beside a game from a program that carries none, and the game's stays */
    assert(!snx_deploy_vulkan_driver(plain, game_dir));
    holds(game_driver, "MoltenVK, the app's, newer");
    /* a game's folder that lost the library gets it back, as the refresh of a runtime deploys it */
    assert(!snx_remove_tree(game_driver));
    assert(!snx_vulkan_driver(game));
    assert(!snx_deploy_vulkan_driver(app_program, game_dir));
    holds(game_driver, "MoltenVK, the app's, newer");
    /* KosmicKrisp: an app that carries its loader, driver and ICD JSON finds them for the pick, and puts the three
       beside a game's runtime too */
    put(app_loader, "the Vulkan loader");
    put(app_kosmickrisp, "Mesa's KosmicKrisp");
    put(app_icd, "the ICD JSON of KosmicKrisp");
    { /* and auto takes it before the app's MoltenVK */
        SnxVulkanPlaces at = {.program = app_program};
        SnxVulkanDriver pick;
        char reason[4096];
        assert(snx_vulkan_driver_find(&at, "kosmickrisp", &pick, reason, sizeof reason));
        named(pick.library, app_loader);
        named(pick.icd, app_icd);
        assert(!strcmp(pick.name, "KosmicKrisp") && !pick.instead);
        snx_vulkan_driver_free(&pick);
        assert(snx_vulkan_driver_find(&at, "auto", &pick, reason, sizeof reason));
        named(pick.library, app_loader);
        named(pick.icd, app_icd);
        snx_vulkan_driver_free(&pick);
    }
    assert(!snx_deploy_vulkan_driver(app_program, game_dir));
    holds(game_loader, "the Vulkan loader");
    holds(game_kosmickrisp, "Mesa's KosmicKrisp");
    holds(game_icd, "the ICD JSON of KosmicKrisp");
    found(snx_vulkan_kosmickrisp_icd(game), game_icd);
    /* and the KosmicKrisp the Mac itself has, where it has one: its ICD through the loader makes a device */
    kosmickrisp_device(plain);
    assert(!snx_remove_tree(dir));
    free(dir);
    free(app);
    free(app_macos);
    free(app_frameworks);
    free(app_program);
    free(app_driver);
    free(app_loader);
    free(app_kosmickrisp);
    free(app_icd);
    free(phone);
    free(phone_frameworks);
    free(phone_program);
    free(phone_driver);
    free(game_dir);
    free(game);
    free(game_driver);
    free(game_loader);
    free(game_kosmickrisp);
    free(game_icd);
    free(plain_dir);
    free(plain);
    puts("vulkan_driver: the app's MoltenVK and KosmicKrisp, found in its bundle and put beside a game's runtime");
    return 0;
}
