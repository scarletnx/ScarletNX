/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The display's EDR headroom, which macOS alone reports (plat.h). AppKit is asked on the window's own thread, about
   once a second: it changes with the display's brightness, and the last works of an SDR-to-HDR lift use all of it. */
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"

#if defined(__APPLE__)
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>

void plat_display_headroom_poll(void) {
    NSScreen* s = [NSScreen mainScreen];
    float h = s ? (float)s.maximumExtendedDynamicRangeColorComponentValue : 1.0f;
    if (!(h >= 1.0f)) h = 1.0f;
    if (h > 16.0f) h = 16.0f;
    u32 pct = (u32)(h * 100.0f + 0.5f), was = __atomic_load_n(&g_display_headroom_pct, __ATOMIC_RELAXED);
    __atomic_store_n(&g_display_headroom_pct, pct, __ATOMIC_RELAXED);
    if (s) {
        float p = (float)s.maximumPotentialExtendedDynamicRangeColorComponentValue;
        if (!(p >= 1.0f)) p = 1.0f;
        if (p > 16.0f) p = 16.0f;
        u32 pot = (u32)(p * 100.0f + 0.5f), waspot = __atomic_load_n(&g_display_edr_potential_pct, __ATOMIC_RELAXED);
        __atomic_store_n(&g_display_edr_potential_pct, pot, __ATOMIC_RELAXED);
        if (pot != waspot) LOG("video: the display's EDR potential is %.2f\n", (double)pot / 100.0);
    }
    if (pct != was) LOG("video: the display's EDR headroom is %.2f\n", (double)pct / 100.0);
}
#else
void plat_display_headroom_poll(void) {}
#endif
#endif
