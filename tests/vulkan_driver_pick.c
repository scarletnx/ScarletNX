/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vulkan driver a macOS run picks (the vulkan_driver setting, docs/hosts/apple.md), on every system: the search goes
   through folders the test makes, an app's Frameworks, two package prefixes and a folder of Vulkan SDKs, with files
   that only stand for the drivers */
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* rel under root, made with its folders, holding its own name */
static char* make(const char* root, const char* rel) {
    char* path = snx_path_join(root, rel);
    char* dir = path ? snx_dirname(path) : NULL;
    assert(path && dir && !snx_mkdirs(dir) && !snx_write_file(path, rel, strlen(rel)));
    free(dir);
    return path;
}
static char* under(const char* root, const char* rel) {
    char* path = snx_path_join(root, rel);
    assert(path);
    return path;
}
/* the file a search found, held against the one expected (NULL for none): the same file, however the path names it */
static void same_file(const char* path, const char* expected) {
    char *a, *b;
    if (!expected) {
        assert(!path);
        return;
    }
    a = path ? snx_absolute(path) : NULL;
    b = snx_absolute(expected);
    if (!a || !b || strcmp(a, b)) fprintf(stderr, "vulkan_driver_pick: found %s, expected %s\n", path, expected);
    assert(a && b && !strcmp(a, b));
    free(a);
    free(b);
}
/* pick in at: its library, its ICD and its name; instead when that driver plays in place of the one asked for, the
   reason then holding says and also (NULL for nothing more) */
static void played(const SnxVulkanPlaces* at, const char* pick, const char* library, const char* icd, const char* name,
                   int instead, const char* says, const char* also) {
    SnxVulkanDriver d;
    char reason[2048] = "";
    int found = snx_vulkan_driver_find(at, pick, &d, reason, sizeof reason);
    if (reason[0]) printf("vulkan_driver_pick: %s: %s\n", pick, reason);
    assert(found);
    same_file(d.library, library);
    same_file(d.icd, icd);
    if (strcmp(d.name, name)) fprintf(stderr, "vulkan_driver_pick: %s is %s, expected %s\n", pick, d.name, name);
    assert(!strcmp(d.name, name) && d.instead == instead);
    assert(!says || strstr(reason, says));
    assert(!also || strstr(reason, also));
    snx_vulkan_driver_free(&d);
}
static void picks(const SnxVulkanPlaces* at, const char* pick, const char* library, const char* icd, const char* name) {
    played(at, pick, library, icd, name, 0, NULL, NULL);
}
/* pick in at, which the Mac has not: auto's driver plays instead, the reason saying why and naming it */
static void stands_in(const SnxVulkanPlaces* at, const char* pick, const char* library, const char* icd,
                      const char* name, const char* says, const char* also) {
    played(at, pick, library, icd, name, 1, says, also);
}
/* pick refused in at, its reason holding says and also (NULL for nothing more) */
static void refused(const SnxVulkanPlaces* at, const char* pick, const char* says, const char* also) {
    SnxVulkanDriver d;
    char reason[2048] = "";
    int found = snx_vulkan_driver_find(at, pick, &d, reason, sizeof reason);
    printf("vulkan_driver_pick: %s: %s\n", pick, found ? "found" : reason);
    assert(!found && !d.library && !d.icd);
    assert(strstr(reason, says) && (!also || strstr(reason, also)));
}
int main(void) {
    char* root = snx_temp_dir(NULL);
    assert(root);
    char* app = make(root, "ScarletNX.app/Contents/MacOS/scarletnx");
    char* app_moltenvk = make(root, "ScarletNX.app/Contents/Frameworks/libMoltenVK.dylib");
    char* bare = make(root, "build/scarletnx"); /* a checkout's: it carries no driver */
    char* brew = under(root, "brew");
    char* brew_moltenvk = make(brew, "lib/libMoltenVK.dylib");
    char* brew_loader = make(brew, "lib/libvulkan.1.dylib");
    char* brew_icd = make(brew, "share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json");
    char* empty = under(root, "empty"); /* a prefix with nothing in it */
    char* sdks = under(root, "home/VulkanSDK");
    char* old_loader = make(sdks, "1.4.9.0/macOS/lib/libvulkan.1.dylib");
    char* sdk_moltenvk = make(sdks, "1.4.10.0/macOS/lib/libMoltenVK.dylib");
    char* sdk_loader = make(sdks, "1.4.10.0/macOS/lib/libvulkan.1.dylib");
    char* sdk_icd = make(sdks, "1.4.10.0/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json");
    char* old_icd = make(sdks, "1.4.9.0/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json");
    char* icd_only = under(root, "icd-only"); /* a driver's ICD without the loader it needs */
    char* icd_only_icd = make(icd_only, "share/vulkan/icd.d/kosmickrisp_icd.json");
    char* mesa = under(root, "mesa"); /* KosmicKrisp and the loader, without MoltenVK */
    char* mesa_loader = make(mesa, "lib/libvulkan.1.dylib");
    char* mesa_icd = make(mesa, "share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json");
    assert(!snx_mkdirs(empty));

    /* auto: KosmicKrisp where the Mac has it, before the MoltenVK the app carries, and the log line says why */
    played(&(SnxVulkanPlaces){.program = app, .prefixes = {empty, brew}, .sdks = sdks}, "auto", brew_loader, brew_icd,
           "KosmicKrisp", 0, "KosmicKrisp, ICD ", "(vulkan_driver=auto takes KosmicKrisp where the Mac has it)");
    picks(&(SnxVulkanPlaces){.program = bare, .sdks = sdks}, "", sdk_loader, sdk_icd, "KosmicKrisp");
    /* a Mac that cannot run it (macOS before 26, an Intel build): the app's MoltenVK, the reason said */
    played(&(SnxVulkanPlaces){.program = app, .prefixes = {brew, NULL}, .no_kosmickrisp = "too old a Mac"}, "auto",
           app_moltenvk, NULL, "MoltenVK", 0, "MoltenVK at ", "(vulkan_driver=auto, and too old a Mac)");
    /* none on the Mac: the app's MoltenVK, else the system's loader */
    played(&(SnxVulkanPlaces){.program = app, .prefixes = {empty, NULL}}, "auto", app_moltenvk, NULL, "MoltenVK", 0,
           "MoltenVK at ", "(vulkan_driver=auto, and KosmicKrisp was not found: no ICD beside the program");
    played(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, NULL}}, "auto", NULL, NULL, "the system's", 0,
           "the system's Vulkan loader (vulkan_driver=auto, the app carries no MoltenVK, and KosmicKrisp was not found",
           NULL);
    /* a driver the environment names (VK_DRIVER_FILES): the loader it goes through, and no ICD of the pick's */
    played(&(SnxVulkanPlaces){.program = app, .prefixes = {empty, brew}, .driver_files = "VK_DRIVER_FILES=/x/icd.json"},
           "auto", brew_loader, NULL, "the environment's", 0,
           "the driver VK_DRIVER_FILES=/x/icd.json names, through the Vulkan loader ",
           "(vulkan_driver=auto leaves the driver the environment names)");
    played(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, NULL}, .driver_files = "VK_ICD_FILENAMES=/x.json"},
           "auto", NULL, NULL, "the environment's", 0,
           "the driver VK_ICD_FILENAMES=/x.json names, through the Vulkan loader SDL finds", NULL);

    /* moltenvk: the app's, else the first prefix's, else the newest SDK's; none is refused, naming where it looked */
    played(&(SnxVulkanPlaces){.program = app, .prefixes = {brew, empty}, .sdks = sdks}, "moltenvk", app_moltenvk, NULL,
           "MoltenVK", 0, "MoltenVK at ", " (vulkan_driver=moltenvk)");
    picks(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, brew}, .sdks = sdks}, "moltenvk", brew_moltenvk, NULL,
          "MoltenVK");
    picks(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, NULL}, .sdks = sdks}, "moltenvk", sdk_moltenvk, NULL,
          "MoltenVK");
    stands_in(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, NULL}}, "moltenvk", NULL, NULL, "the system's",
              "MoltenVK was not found: not beside the program or in its app's Frameworks, in",
              ": the system's Vulkan loader plays instead");
    /* and where the Mac has KosmicKrisp, auto's KosmicKrisp plays in its place */
    stands_in(&(SnxVulkanPlaces){.program = bare, .prefixes = {mesa, NULL}}, "moltenvk", mesa_loader, mesa_icd,
              "KosmicKrisp", "MoltenVK was not found", ": KosmicKrisp, ICD ");

    { /* the folder of SDKs the search went through, named as it was given: $HOME/VulkanSDK where HOME is not the
         user's own folder (a test's), which "~/VulkanSDK" would hide */
        char* elsewhere = under(root, "elsewhere/VulkanSDK");
        char said[1024];
        snprintf(said, sizeof said, "or in a Vulkan SDK under %s", elsewhere);
        stands_in(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, NULL}, .sdks = elsewhere}, "moltenvk", NULL,
                  NULL, "the system's", "MoltenVK was not found", said);
        stands_in(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, NULL}, .sdks = elsewhere}, "kosmickrisp",
                  NULL, NULL, "the system's", "KosmicKrisp was not found", said);
        stands_in(&(SnxVulkanPlaces){.program = bare, .prefixes = {icd_only, NULL}, .sdks = elsewhere}, "kosmickrisp",
                  NULL, NULL, "the system's", "KosmicKrisp needs the Vulkan loader", said);
        free(elsewhere);
    }

    /* kosmickrisp: an ICD and the loader, each the first found; the newest SDK's over an older one */
    picks(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, brew}, .sdks = sdks}, "kosmickrisp", brew_loader,
          brew_icd, "KosmicKrisp");
    picks(&(SnxVulkanPlaces){.program = bare, .sdks = sdks}, "kosmickrisp", sdk_loader, sdk_icd, "KosmicKrisp");
    stands_in(&(SnxVulkanPlaces){.program = app, .prefixes = {empty, NULL}}, "kosmickrisp", app_moltenvk, NULL,
              "MoltenVK", "KosmicKrisp was not found: no ICD beside the program or in its app's Frameworks, in",
              ": MoltenVK at ");
    stands_in(&(SnxVulkanPlaces){.program = bare, .prefixes = {icd_only, NULL}}, "kosmickrisp", NULL, NULL,
              "the system's", "KosmicKrisp needs the Vulkan loader", "icd-only/lib");
    stands_in(&(SnxVulkanPlaces){.program = app, .prefixes = {brew, NULL}, .no_kosmickrisp = "not on this system"},
              "kosmickrisp", app_moltenvk, NULL, "MoltenVK", "not on this system: MoltenVK at ", " plays instead");

    /* the app that carries KosmicKrisp: its own ICD and loader, before the Mac's */
    {
        char* app_loader = make(root, "ScarletNX.app/Contents/Frameworks/libvulkan.1.dylib");
        char* app_icd = make(root, "ScarletNX.app/Contents/Frameworks/libkosmickrisp_icd.json");
        picks(&(SnxVulkanPlaces){.program = app, .prefixes = {brew, empty}, .sdks = sdks}, "kosmickrisp", app_loader,
              app_icd, "KosmicKrisp");
        picks(&(SnxVulkanPlaces){.program = app, .prefixes = {brew, empty}, .sdks = sdks}, "auto", app_loader, app_icd,
              "KosmicKrisp");
        free(app_loader);
        free(app_icd);
    }

    /* a system that is not a Mac's: its own driver, whatever the value, and never the files a Mac would take */
    picks(&(SnxVulkanPlaces){.program = app, .prefixes = {brew, NULL}, .system_driver = "this system's driver"}, "auto",
          NULL, NULL, "the system's");
    stands_in(&(SnxVulkanPlaces){.program = app, .prefixes = {brew, NULL}, .system_driver = "this system's driver"},
              "moltenvk", NULL, NULL, "the system's", "MoltenVK is a driver of macOS",
              ": this system's driver plays instead");
    stands_in(&(SnxVulkanPlaces){.program = app, .prefixes = {brew, NULL}, .system_driver = "this system's driver"},
              "kosmickrisp", NULL, NULL, "the system's", "KosmicKrisp is a driver of macOS",
              ": this system's driver plays instead");

    /* a value that is no driver, on a Mac and elsewhere */
    refused(&(SnxVulkanPlaces){.program = app}, "vulkan", "vulkan_driver=vulkan is not auto, moltenvk or kosmickrisp",
            NULL);
    refused(&(SnxVulkanPlaces){.system_driver = "this system's driver"}, "MoltenVK",
            "vulkan_driver=MoltenVK is not auto, moltenvk or kosmickrisp", NULL);

    { /* the Macs KosmicKrisp runs on, by their kernel: Darwin 25 is macOS 26 */
        char why[512] = "";
        assert(snx_vulkan_kosmickrisp_runs("25.0.0", why, sizeof why) && !why[0]);
        assert(snx_vulkan_kosmickrisp_runs("26.1.0", why, sizeof why));
        assert(!snx_vulkan_kosmickrisp_runs("24.6.0", why, sizeof why));
        assert(strstr(why, "KosmicKrisp needs macOS 26 or later (Darwin 25), and this Mac runs Darwin 24.6.0"));
        assert(!snx_vulkan_kosmickrisp_runs("Darwin", why, sizeof why) && strstr(why, "\"Darwin\""));
        assert(!snx_vulkan_kosmickrisp_runs(NULL, why, sizeof why));
    }

    assert(!snx_remove_tree(root));
    free(root);
    free(app);
    free(app_moltenvk);
    free(bare);
    free(brew);
    free(brew_moltenvk);
    free(brew_loader);
    free(brew_icd);
    free(empty);
    free(sdks);
    free(old_loader);
    free(sdk_moltenvk);
    free(sdk_loader);
    free(sdk_icd);
    free(old_icd);
    free(icd_only);
    free(icd_only_icd);
    free(mesa);
    free(mesa_loader);
    free(mesa_icd);
    puts("vulkan_driver_pick: auto takes KosmicKrisp, then the app's MoltenVK, then the system's; moltenvk and "
         "kosmickrisp found in the app, the prefixes and the SDKs");
    return 0;
}
