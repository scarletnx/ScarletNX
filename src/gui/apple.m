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
#if TARGET_OS_IPHONE
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#import <MetricKit/MetricKit.h>
#endif

/* dist/apple/Frontend.swift. window is the SDL window's UIWindow or NSWindow; done gets the path picked, NULL for none */
extern bool snx_apple_pick(void* window, bool folder, void (*done)(void* data, const char* path), void* data,
                           void (*log)(const char* line));
extern bool snx_apple_share(void* window, const char* path);
/* plat_posix.c: the runtime's power watcher marks a change, which its main thread takes */
extern void plat_power_mark_changed(void);
/* plat_posix.c's weak watcher entry, which this file fills on iOS */
int snx_apple_power_watch(void);

/* the host's power source, for TV mode Auto: iOS's battery state notification. macOS's is plat_posix.c's IOKit one */
#if TARGET_OS_IPHONE
static id g_power_observer;
int snx_apple_power_watch(void) {
    if (g_power_observer) return 0;
    UIDevice* device = UIDevice.currentDevice;
    device.batteryMonitoringEnabled = YES;
    g_power_observer = [NSNotificationCenter.defaultCenter addObserverForName:UIDeviceBatteryStateDidChangeNotification
                                                                     object:nil
                                                                      queue:nil
                                                                 usingBlock:^(NSNotification* note) {
        (void)note;
        plat_power_mark_changed();
    }];
    return g_power_observer ? 0 : -1;
}
#else
/* macOS: the watcher is plat_posix.c's, which the runtime links */
int snx_apple_power_watch(void) { return -1; }
#endif

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

/* MetricKit's payloads: what the system saw of the app's runs, a crash, a hang or an exit for memory among them, which
   the app itself cannot log. Each goes as JSON into logs/metrickit-YYYY-MM-DD_HHMMSS.json, named by the end of the time
   it covers, with -2, -3 and on for a different payload of the same name. A payload that a file already holds (iOS
   gives the past ones again at each start) is not written again. A line in the console log names the file, or the
   file and why it was not written, and what it holds. The system calls the subscriber on a queue of its own, and it
   can give the payloads of a run at a later start */
static NSURL* metrics_folder;
/* the file of json for the payload that ends at end, written when it is not there yet: its name for the payload's
   line, or that it was not written, with the file and the system's reason, since the payload is the exit's only
   record */
static NSString* metrics_write_one(NSData* json, NSDate* end) {
    NSDateFormatter* format = [[NSDateFormatter alloc] init];
    format.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    format.dateFormat = @"yyyy-MM-dd_HHmmss";
    NSString* stem = [@"metrickit-" stringByAppendingString:[format stringFromDate:end ? end : [NSDate date]]];
    if (!json) return [stem stringByAppendingString:@" not written: the payload gave no JSON"];
    if (!metrics_folder) return [stem stringByAppendingString:@" not written: no logs folder"];
    for (int n = 1; n < 100; n++) {
        NSString* name = n == 1 ? [stem stringByAppendingString:@".json"]
                                : [stem stringByAppendingFormat:@"-%d.json", n];
        NSURL* file = [metrics_folder URLByAppendingPathComponent:name];
        NSData* there = [NSData dataWithContentsOfURL:file];
        if ([there isEqualToData:json]) return name;
        if (there) continue;
        NSError* error = nil;
        if ([json writeToURL:file options:NSDataWritingAtomic error:&error]) return name;
        return [NSString stringWithFormat:@"not written to %@: %@", file.path,
                                          error ? error.localizedDescription : @"the system gave no reason"];
    }
    return [NSString stringWithFormat:@"not written: %@.json to %@-99.json in %@ all hold other payloads", stem, stem,
                                      metrics_folder.path];
}
static NSString* metrics_write(NSData* json, NSDate* end) {
    @synchronized(NSFileManager.class) { /* the subscriber's queue and the past payloads of the main thread */
        return metrics_write_one(json, end);
    }
}
/* ", N type" for N diagnostics of type, nothing for none */
static void metrics_count(NSMutableString* line, NSArray* diagnostics, const char* type) {
    if (diagnostics.count) [line appendFormat:@", %lu %s", (unsigned long)diagnostics.count, type];
}
static void metrics_diagnostic(MXDiagnosticPayload* payload, const char* when) {
    NSString* file = metrics_write(payload.JSONRepresentation, payload.timeStampEnd);
    NSMutableString* line = [NSMutableString stringWithFormat:@"metrickit: %s diagnostic payload %@", when, file];
    metrics_count(line, payload.crashDiagnostics, "crash");
    metrics_count(line, payload.hangDiagnostics, "hang");
    metrics_count(line, payload.cpuExceptionDiagnostics, "CPU exception");
    metrics_count(line, payload.diskWriteExceptionDiagnostics, "disk write exception");
    metrics_count(line, payload.appLaunchDiagnostics, "app launch");
    fprintf(stderr, "%s\n", line.UTF8String);
}
/* MetricKit has no diagnostic for an exit for memory: its count is in a metric payload's exit metrics */
static void metrics_metric(MXMetricPayload* payload, const char* when) {
    NSString* file = metrics_write(payload.JSONRepresentation, payload.timeStampEnd);
    MXAppExitMetric* exits = payload.applicationExitMetrics;
    fprintf(stderr,
            "metrickit: %s metric payload %s, memory exit: %lu at the memory limit in the foreground, %lu at the "
            "memory limit and %lu for memory pressure in the background\n",
            when, file.UTF8String,
            (unsigned long)exits.foregroundExitData.cumulativeMemoryResourceLimitExitCount,
            (unsigned long)exits.backgroundExitData.cumulativeMemoryResourceLimitExitCount,
            (unsigned long)exits.backgroundExitData.cumulativeMemoryPressureExitCount);
}
@interface SnxMetrics : NSObject <MXMetricManagerSubscriber>
@end
@implementation SnxMetrics
- (void)didReceiveMetricPayloads:(NSArray<MXMetricPayload*>*)payloads {
    for (MXMetricPayload* payload in payloads) metrics_metric(payload, "new");
}
- (void)didReceiveDiagnosticPayloads:(NSArray<MXDiagnosticPayload*>*)payloads {
    for (MXDiagnosticPayload* payload in payloads) metrics_diagnostic(payload, "new");
}
@end
/* the subscriber, kept for the whole process (a static reference, so that the metric manager need not keep it); and
   the payloads that the system gave before this start */
static void metrics_start(NSURL* logs) {
    static SnxMetrics* subscriber;
    MXMetricManager* manager = MXMetricManager.sharedManager;
    metrics_folder = logs;
    subscriber = [[SnxMetrics alloc] init];
    [manager addSubscriber:subscriber];
    NSArray<MXDiagnosticPayload*>* diagnostics = manager.pastDiagnosticPayloads;
    NSArray<MXMetricPayload*>* metrics = manager.pastPayloads;
    fprintf(stderr, "metrickit: subscribed, %lu past diagnostic and %lu past metric payloads\n",
            (unsigned long)diagnostics.count, (unsigned long)metrics.count);
    for (MXDiagnosticPayload* payload in diagnostics) metrics_diagnostic(payload, "past");
    for (MXMetricPayload* payload in metrics) metrics_metric(payload, "past");
}
#endif

void gui_apple_stats(char* out, size_t size) {
#if TARGET_OS_IPHONE
    @autoreleasepool { /* the main loop calls this each second, and the memory pressure queue too */
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    struct rusage usage;
    unsigned long long footprint = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        footprint = info.phys_footprint >> 20;
    if (getrusage(RUSAGE_SELF, &usage)) memset(&usage, 0, sizeof usage);
    /* what Metal holds for the app, which MoltenVK's descriptor pools, pipelines and buffers take from too, and
       VMA's heap budget does not count; past the working set Metal refuses allocations the footprint still has room
       for. The system's default device is the one MoltenVK uses. The main loop and a memory warning's queue both
       call this, so the device is got once for all */
    static id<MTLDevice> metal;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        metal = MTLCreateSystemDefaultDevice();
    });
    snprintf(out, size,
             "resources: footprint %llu MiB, may still take %llu MiB, Metal %llu of %llu MiB, cpu %.1f s user %.1f s "
             "system, thermal %ld, low power %d",
             footprint, (unsigned long long)(os_proc_available_memory() >> 20),
             (unsigned long long)(metal.currentAllocatedSize >> 20),
             (unsigned long long)(metal.recommendedMaxWorkingSetSize >> 20),
             usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6, usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6,
             (long)NSProcessInfo.processInfo.thermalState, (int)NSProcessInfo.processInfo.lowPowerModeEnabled);
    }
#else
    snprintf(out, size, "resources: (not iOS)");
#endif
}

#if TARGET_OS_IPHONE
/* An Objective-C exception that nothing catches: its name, reason and stack go to the console before the abort, and
   then the handler that was there before runs. A crash report of MetricKit went through libobjc and libc++abi to
   abort() with no reason. libc++abi writes the type and what() of a C++ exception to stderr itself */
static NSUncaughtExceptionHandler* g_next_uncaught;
static void uncaught(NSException* e) {
    const char* name = e.name.UTF8String;
    const char* reason = e.reason.UTF8String;
    fprintf(stderr, "uncaught exception: %s: %s\n", name ? name : "?", reason ? reason : "(no reason)");
    for (NSString* frame in e.callStackSymbols) fprintf(stderr, "  %s\n", frame.UTF8String);
    if (g_next_uncaught) g_next_uncaught(e);
}
/* logs/.running, there from a start until exit(): one there at a start is a start that did not end with exit() */
static char g_running[1024];
static void running_end(void) { unlink(g_running); }
/* the last start's mark, said and then made again for this start, with exit() removing it */
static void running_line(NSURL* logs) {
    const char* path = [logs URLByAppendingPathComponent:@".running"].path.fileSystemRepresentation;
    char last[128] = {0};
    if (!path || snprintf(g_running, sizeof g_running, "%s", path) >= (int)sizeof g_running) return;
    FILE* f = fopen(g_running, "r");
    if (f) {
        if (!fgets(last, sizeof last, f)) last[0] = 0;
        last[strcspn(last, "\n")] = 0;
        fclose(f);
        fprintf(stderr,
                "last start: %s did not end with exit(). The system ended it: a crash, its memory limit (jetsam), or a "
                "close from the app switcher\n",
                last[0] ? last : "(no mark)");
    } else
        fprintf(stderr, "last start: ended with exit(), or none before\n");
    f = fopen(g_running, "w");
    if (!f) return;
    time_t now = time(NULL);
    char when[32];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "pid %ld, started %s\n", (long)getpid(), when);
    fclose(f);
    atexit(running_end);
}
#endif

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
        g_next_uncaught = NSGetUncaughtExceptionHandler();
        NSSetUncaughtExceptionHandler(uncaught);
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
        /* a host such as LiveContainer runs the app in its own process, with the app's bundle in another place: the
           process's program then is not the bundle's */
        NSString* bundle = NSBundle.mainBundle.bundlePath.stringByResolvingSymlinksInPath;
        NSString* program = NSProcessInfo.processInfo.arguments.firstObject.stringByResolvingSymlinksInPath;
        fprintf(stderr, "  bundle %s\n  program %s\n", bundle.fileSystemRepresentation,
                program.length ? program.fileSystemRepresentation : "(none)");
        if (bundle.length && program.length && ![program hasPrefix:[bundle stringByAppendingString:@"/"]])
            fprintf(stderr, "host: the app runs inside another app (LiveContainer or similar), as its program is not in "
                            "its bundle. The document picker can need a fix of that app (LiveContainer: Fix File "
                            "Picker)\n");
        fprintf(stderr, "documents: %s\n", documents.fileSystemRepresentation);
        if (logs) metrics_start(logs);
        NSDictionary* room = [documents resourceValuesForKeys:@[
            NSURLVolumeAvailableCapacityForImportantUsageKey, NSURLVolumeTotalCapacityKey
        ]
                                                        error:NULL];
        fprintf(stderr, "disk: %lld MiB free for the app of %lld MiB\n",
                [room[NSURLVolumeAvailableCapacityForImportantUsageKey] longLongValue] >> 20,
                [room[NSURLVolumeTotalCapacityKey] longLongValue] >> 20);
        running_line(logs);
    }
#endif
}

/* the last thermal and power states said, so a notification with no change says nothing */
static long g_thermal = -1;
static int g_low_power = -1;
static void state_line(void) {
    long thermal = (long)NSProcessInfo.processInfo.thermalState;
    int low = NSProcessInfo.processInfo.lowPowerModeEnabled;
    static const char* const names[] = {"nominal", "fair", "serious", "critical"};
    if (thermal == g_thermal && low == g_low_power) return;
    g_thermal = thermal;
    g_low_power = low;
    gui_log("state: thermal %s, low power mode %s", thermal >= 0 && thermal < 4 ? names[thermal] : "?",
            low ? "on" : "off");
}
/* the observers, kept while the process lives */
static id g_thermal_observer, g_low_power_observer;
void gui_apple_state_watch(void) {
    if (g_thermal_observer) return;
    state_line();
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    void (^changed)(NSNotification*) = ^(NSNotification* n) {
      (void)n;
      state_line();
    };
    g_thermal_observer = [center addObserverForName:NSProcessInfoThermalStateDidChangeNotification
                                             object:nil
                                              queue:NSOperationQueue.mainQueue
                                         usingBlock:changed];
    g_low_power_observer = [center addObserverForName:NSProcessInfoPowerStateDidChangeNotification
                                               object:nil
                                                queue:NSOperationQueue.mainQueue
                                           usingBlock:changed];
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
/* a step of the picker, which the Swift side says */
static void pick_log(const char* line) { gui_log("%s", line); }
int gui_apple_pick(SDL_Window* window, int folder, SDL_DialogFileCallback done, void* data, int log, char* message,
                   size_t size) {
    void* native = native_window(window);
    pick_done = done;
    pick_data = data;
    if (native && snx_apple_pick(native, folder != 0, picked, NULL, log ? pick_log : NULL)) return 1;
    if (log && !native) gui_log("picker: not opened: SDL gives no UIWindow");
    pick_done = NULL;
    snprintf(message, size, "%s",
             native ? snx_tr("The document picker did not open.") : snx_tr("No window to show the picker over."));
    return 0;
}

int gui_apple_share(SDL_Window* window, const char* path) {
    void* native = native_window(window);
    return native && snx_apple_share(native, path);
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
