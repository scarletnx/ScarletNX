/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's side of the iOS and macOS apps (docs/APPLE.md): the folders, the game the menu plays, and the windows
   SDL's give dist/apple/Frontend.swift to present its pickers and share sheet over */
#include "gui/apple.h"
#include "gui/i18n.h"
#include "gui/log.h"
#include <SDL3/SDL.h>
#include <TargetConditionals.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <time.h>
#import <Foundation/Foundation.h>

/* dist/apple/Frontend.swift. window is the SDL window's UIWindow or NSWindow; done gets the path picked, NULL for none */
extern bool snx_apple_pick(void* window, bool folder, void (*done)(void* data, const char* path), void* data);
extern bool snx_apple_share(void* window, const char* path);

#if TARGET_OS_IPHONE
extern size_t os_proc_available_memory(void); /* <os/proc.h>, which not every SDK header set declares the same way */
static void sysctl_line(const char* name) {
    char text[256] = "?";
    size_t n = sizeof text;
    if (sysctlbyname(name, text, &n, NULL, 0)) snprintf(text, sizeof text, "(unavailable)");
    fprintf(stderr, "  %s = %s\n", name, text);
}
static void sysctl_number(const char* name) {
    unsigned long long v = 0;
    size_t n = sizeof v;
    if (sysctlbyname(name, &v, &n, NULL, 0))
        fprintf(stderr, "  %s = (unavailable)\n", name);
    else
        fprintf(stderr, "  %s = %llu\n", name, n == 4 ? (unsigned long long)(unsigned)v : v);
}
static void limit_line(const char* name, int resource) {
    struct rlimit r;
    if (getrlimit(resource, &r))
        fprintf(stderr, "  rlimit %s = (unavailable)\n", name);
    else
        fprintf(stderr, "  rlimit %s = soft %llu hard %llu\n", name, (unsigned long long)r.rlim_cur,
                (unsigned long long)r.rlim_max);
}
/* the entitlements the signed app carries, from the provisioning profile in the bundle: which of the ones that matter
   to a game the sideloading tool gave this app (a profile is a signed plist, its keys readable in the file) */
static void entitlement_lines(void) {
    static const char* const keys[] = {"get-task-allow", "com.apple.developer.kernel.extended-virtual-addressing",
                                       "com.apple.developer.kernel.increased-memory-limit",
                                       "com.apple.developer.kernel.increased-debugging-memory-limit",
                                       "com.apple.security.application-groups", "dynamic-codesigning"};
    NSData* data = [NSData dataWithContentsOfFile:[NSBundle.mainBundle.bundlePath
                                                      stringByAppendingPathComponent:@"embedded.mobileprovision"]];
    size_t i;
    if (!data) {
        fprintf(stderr, "entitlements: no embedded.mobileprovision in the bundle\n");
        return;
    }
    fprintf(stderr, "entitlements in the provisioning profile (%lu bytes):\n", (unsigned long)data.length);
    for (i = 0; i < sizeof keys / sizeof *keys; ++i)
        fprintf(stderr, "  %s: %s\n", keys[i], memmem(data.bytes, data.length, keys[i], strlen(keys[i])) ? "yes" : "no");
}
#endif

void gui_apple_stats(char* out, size_t size) {
#if TARGET_OS_IPHONE
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    struct rusage usage;
    unsigned long long footprint = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        footprint = info.phys_footprint >> 20;
    if (getrusage(RUSAGE_SELF, &usage)) memset(&usage, 0, sizeof usage);
    snprintf(out, size,
             "resources: footprint %llu MiB, may still take %llu MiB, cpu %.1f s user %.1f s system, thermal %ld, "
             "low power %d",
             footprint, (unsigned long long)(os_proc_available_memory() >> 20),
             usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6, usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6,
             (long)NSProcessInfo.processInfo.thermalState, (int)NSProcessInfo.processInfo.lowPowerModeEnabled);
#else
    snprintf(out, size, "resources: (not iOS)");
#endif
}

void gui_apple_log_start(void) {
#if TARGET_OS_IPHONE
    @autoreleasepool {
        NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                                inDomains:NSUserDomainMask].firstObject;
        NSURL* logs = [documents URLByAppendingPathComponent:@"logs" isDirectory:YES];
        const char* path = NULL;
        char today[64];
        gui_log_console_name(today, sizeof today); /* the day it starts on: gui_log's lines go there too */
        if (logs) {
            [NSFileManager.defaultManager createDirectoryAtURL:logs withIntermediateDirectories:YES attributes:nil error:NULL];
            path = [logs URLByAppendingPathComponent:@(today)].path.fileSystemRepresentation;
        }
        time_t now = time(NULL);
        if (!path || !freopen(path, "a", stderr)) return;
        freopen(path, "a", stdout);
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        fprintf(stderr, "\n===== ScarletNX starts, pid %ld, %s", (long)getpid(), ctime(&now));
        fprintf(stderr, "device:\n");
        sysctl_line("hw.machine");
        sysctl_line("hw.model");
        sysctl_line("kern.osversion");
        sysctl_line("kern.osrelease");
        sysctl_number("hw.memsize");
        sysctl_number("hw.pagesize");
        sysctl_number("hw.ncpu");
        sysctl_number("hw.logicalcpu");
        sysctl_number("hw.optional.arm.FEAT_LSE");
        sysctl_number("vm.pagesize");
        sysctl_number("kern.maxfilesperproc");
        fprintf(stderr, "system: iOS %s, physical memory %llu MiB, thermal state %ld, low power %d\n",
                NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String,
                (unsigned long long)(NSProcessInfo.processInfo.physicalMemory >> 20),
                (long)NSProcessInfo.processInfo.thermalState, (int)NSProcessInfo.processInfo.lowPowerModeEnabled);
        fprintf(stderr, "limits:\n");
        limit_line("AS", RLIMIT_AS);
        limit_line("DATA", RLIMIT_DATA);
        limit_line("RSS", RLIMIT_RSS);
        limit_line("NOFILE", RLIMIT_NOFILE);
        {
            task_vm_info_data_t info;
            mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
            if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
                fprintf(stderr, "  task address space: %#llx to %#llx (%llu GiB), virtual size %llu MiB, footprint %llu MiB\n",
                        (unsigned long long)info.min_address, (unsigned long long)info.max_address,
                        (unsigned long long)((info.max_address - info.min_address) >> 30),
                        (unsigned long long)(info.virtual_size >> 20), (unsigned long long)(info.phys_footprint >> 20));
            else
                fprintf(stderr, "  task address space: (unavailable)\n");
        }
        fprintf(stderr, "  available memory before the first allocation: %llu MiB\n",
                (unsigned long long)(os_proc_available_memory() >> 20));
        entitlement_lines();
        fprintf(stderr, "app: %s\n", NSBundle.mainBundle.bundleIdentifier.UTF8String);
        fprintf(stderr, "documents: %s\n", documents.fileSystemRepresentation);
    }
#endif
}

void gui_apple_start(void) {
#if TARGET_OS_IPHONE
    @autoreleasepool {
        NSFileManager* files = NSFileManager.defaultManager;
        NSURL* documents = [files URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
        /* what the player sees: dumps to put a game's .nsp or .xci in, keys for prod.keys, recompiled for what the app
           makes of them, logs for what it prints */
        for (NSString* name in @[ @"dumps", @"keys", @"recompiled", @"logs" ])
            [files createDirectoryAtURL:[documents URLByAppendingPathComponent:name isDirectory:YES]
                withIntermediateDirectories:YES
                                 attributes:nil
                                      error:NULL];
        if (documents) chdir(documents.fileSystemRepresentation);
    }
#endif
}

/* the window AppKit or UIKit has under the SDL one */
static void* native_window(SDL_Window* window) {
    SDL_PropertiesID properties = window ? SDL_GetWindowProperties(window) : 0;
#if TARGET_OS_IPHONE
    return properties ? SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL) : NULL;
#else
    return properties ? SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL) : NULL;
#endif
}

static SDL_DialogFileCallback pick_done;
static void* pick_data;
/* the picker's answer, as an SDL dialog gives it: a list of the one path, empty when none was picked */
static void picked(void* data, const char* path) {
    const char* list[2] = {path, NULL};
    SDL_DialogFileCallback done = pick_done;
    (void)data;
    pick_done = NULL;
    if (done) done(pick_data, list, -1);
}
int gui_apple_pick(SDL_Window* window, int folder, SDL_DialogFileCallback done, void* data, char* message,
                   size_t size) {
    void* native = native_window(window);
    pick_done = done;
    pick_data = data;
    if (native && snx_apple_pick(native, folder != 0, picked, NULL)) return 1;
    pick_done = NULL;
    snprintf(message, size, "%s",
             native ? snx_tr("The document picker did not open.") : snx_tr("No window to show the picker over."));
    return 0;
}

int gui_apple_share(SDL_Window* window, const char* path, char* message, size_t size) {
    void* native = native_window(window);
    if (native && snx_apple_share(native, path)) return 1;
    snprintf(message, size, snx_tr("The log could not be shown: it is at %s"), path);
    return 0;
}

static char** played;
static int played_count;
void gui_apple_play(const char* const* argv) {
    int n = 0, i;
    while (argv[n]) ++n;
    for (i = 0; i < played_count; ++i) free(played[i]);
    free(played);
    played_count = 0;
    played = (char**)calloc((size_t)n + 1, sizeof(*played));
    if (!played) return;
    for (i = 0; i < n; ++i) played[i] = strdup(argv[i]);
    played_count = n;
}
char** gui_apple_played(int* argc) {
    *argc = played_count;
    return played_count ? played : NULL;
}
