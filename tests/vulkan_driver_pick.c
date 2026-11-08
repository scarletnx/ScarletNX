/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vulkan driver a macOS run picks (the vulkan_driver setting, docs/APPLE.md), on every system: the search goes
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
/* pick, found in at: its library, its ICD and its name */
static void picks(const SnxVulkanPlaces* at, const char* pick, const char* library, const char* icd, const char* name) {
    SnxVulkanDriver d;
    char reason[2048] = "";
    int found = snx_vulkan_driver_find(at, pick, &d, reason, sizeof reason);
    if (!found) fprintf(stderr, "vulkan_driver_pick: %s refused: %s\n", pick, reason);
    assert(found);
    same_file(d.library, library);
    same_file(d.icd, icd);
    if (strcmp(d.name, name)) fprintf(stderr, "vulkan_driver_pick: %s is %s, expected %s\n", pick, d.name, name);
    assert(!strcmp(d.name, name));
    snx_vulkan_driver_free(&d);
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
    assert(!snx_mkdirs(empty));

    /* auto: the MoltenVK the app carries, else the system's */
    picks(&(SnxVulkanPlaces){.program = app, .prefixes = {brew, empty}, .sdks = sdks}, "auto", app_moltenvk, NULL,
          "MoltenVK");
    picks(&(SnxVulkanPlaces){.program = bare, .prefixes = {brew, empty}, .sdks = sdks}, "", NULL, NULL, "the system's");

    /* moltenvk: the app's, else the first prefix's, else the newest SDK's; none is refused, naming where it looked */
    picks(&(SnxVulkanPlaces){.program = app, .prefixes = {brew, empty}, .sdks = sdks}, "moltenvk", app_moltenvk, NULL,
          "MoltenVK");
    picks(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, brew}, .sdks = sdks}, "moltenvk", brew_moltenvk, NULL,
          "MoltenVK");
    picks(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, NULL}, .sdks = sdks}, "moltenvk", sdk_moltenvk, NULL,
          "MoltenVK");
    refused(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, NULL}}, "moltenvk", "MoltenVK was not found",
            "beside the program or in its app's Frameworks, in");

    { /* the folder of SDKs the search went through, named as it was given: $HOME/VulkanSDK where HOME is not the
         user's own folder (a test's), which "~/VulkanSDK" would hide */
        char* elsewhere = under(root, "elsewhere/VulkanSDK");
        char said[1024];
        snprintf(said, sizeof said, "or in a Vulkan SDK under %s", elsewhere);
        refused(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, NULL}, .sdks = elsewhere}, "moltenvk",
                "MoltenVK was not found", said);
        refused(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, NULL}, .sdks = elsewhere}, "kosmickrisp",
                "KosmicKrisp was not found", said);
        refused(&(SnxVulkanPlaces){.program = bare, .prefixes = {icd_only, NULL}, .sdks = elsewhere}, "kosmickrisp",
                "KosmicKrisp needs the Vulkan loader", said);
        free(elsewhere);
    }

    /* kosmickrisp: an ICD and the loader, each the first found; the newest SDK's over an older one */
    picks(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, brew}, .sdks = sdks}, "kosmickrisp", brew_loader,
          brew_icd, "KosmicKrisp");
    picks(&(SnxVulkanPlaces){.program = bare, .sdks = sdks}, "kosmickrisp", sdk_loader, sdk_icd, "KosmicKrisp");
    refused(&(SnxVulkanPlaces){.program = bare, .prefixes = {empty, NULL}}, "kosmickrisp", "KosmicKrisp was not found",
            "no ICD beside the program or in its app's Frameworks, in");
    refused(&(SnxVulkanPlaces){.program = bare, .prefixes = {icd_only, NULL}}, "kosmickrisp",
            "KosmicKrisp needs the Vulkan loader", "icd-only/lib");
    refused(&(SnxVulkanPlaces){.program = bare, .prefixes = {brew, NULL}, .no_kosmickrisp = "not on this system"},
            "kosmickrisp", "not on this system", NULL);

    /* the app that carries KosmicKrisp: its own ICD and loader, before the Mac's */
    {
        char* app_loader = make(root, "ScarletNX.app/Contents/Frameworks/libvulkan.1.dylib");
        char* app_icd = make(root, "ScarletNX.app/Contents/Frameworks/libkosmickrisp_icd.json");
        picks(&(SnxVulkanPlaces){.program = app, .prefixes = {brew, empty}, .sdks = sdks}, "kosmickrisp", app_loader,
              app_icd, "KosmicKrisp");
        free(app_loader);
        free(app_icd);
    }

    /* a value that is no driver */
    refused(&(SnxVulkanPlaces){.program = app}, "vulkan", "vulkan is not auto, moltenvk or kosmickrisp", NULL);

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
    puts("vulkan_driver_pick: auto, moltenvk and kosmickrisp found in the app, the prefixes and the SDKs");
    return 0;
}
