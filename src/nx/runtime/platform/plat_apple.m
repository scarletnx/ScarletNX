/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* plat_apple.h's activity, which only an Objective-C caller can take. NSActivityUserInitiated says the user asked for
   this work, NSActivityLatencyCritical that a deadline the user can feel depends on it, and NSActivityIdleSystemSleepDisabled
   that the display may not sleep under it: those three are what a title's own runs are, and between them they keep App
   Nap and the timer coalescing beside it away from the game. */
#import <Foundation/Foundation.h>

#include "plat_apple.h"
#include "nx/runtime/platform/log.h"

static id g_title_activity;

void plat_apple_title_begin(void) {
    if (g_title_activity) return;
    g_title_activity = [[NSProcessInfo processInfo]
        beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical | NSActivityIdleSystemSleepDisabled)
                          reason:@"a title is playing"];
    LOG("platform: a title's activity is taken: user initiated, latency critical, no idle sleep (App Nap stays off)\n");
}

void plat_apple_title_end(void) {
    if (!g_title_activity) return;
    [[NSProcessInfo processInfo] endActivity:g_title_activity];
    g_title_activity = nil;
}
