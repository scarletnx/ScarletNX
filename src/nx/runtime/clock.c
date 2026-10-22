/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's clock: real time since the run started, in nanoseconds and in the console's system ticks, less the time
   the game spent paused */
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/plat.h"

/* plat_ns() at guest time 0, moved on by each pause's length as it ends, and the guest time a pause stopped at plus
   1 (0 while none is in force). A resume stores start before it clears frozen, so a reader that sees no pause sees the
   start that goes with it */
static struct {
    u64 start, frozen;
} g_clock;

void clock_start(void) { __atomic_store_n(&g_clock.start, plat_ns(), __ATOMIC_RELEASE); }
u64 g_t0_ns(void) { return __atomic_load_n(&g_clock.start, __ATOMIC_ACQUIRE); }
u64 guest_ns(void) {
    u64 f = __atomic_load_n(&g_clock.frozen, __ATOMIC_ACQUIRE);
    return f ? f - 1 : plat_ns() - __atomic_load_n(&g_clock.start, __ATOMIC_ACQUIRE);
}
void clock_pause(void) {
    if (!__atomic_load_n(&g_clock.frozen, __ATOMIC_RELAXED))
        __atomic_store_n(&g_clock.frozen, guest_ns() + 1, __ATOMIC_RELEASE);
}
void clock_resume(void) {
    u64 f = __atomic_load_n(&g_clock.frozen, __ATOMIC_RELAXED);
    if (!f) return;
    __atomic_store_n(&g_clock.start, plat_ns() - (f - 1), __ATOMIC_RELEASE);
    __atomic_store_n(&g_clock.frozen, 0, __ATOMIC_RELEASE);
}

/* a save state's load: guest time is the state's, stopped there while the game is paused */
void clock_set(u64 ns) {
    if (__atomic_load_n(&g_clock.frozen, __ATOMIC_RELAXED))
        __atomic_store_n(&g_clock.frozen, ns + 1, __ATOMIC_RELEASE);
    else
        __atomic_store_n(&g_clock.start, plat_ns() - ns, __ATOMIC_RELEASE);
}

/* 19.2 MHz is 192 ticks every 10 us: whole 10 us steps, then the rest, so neither part overflows or rounds the way
   a double would after a few hours */
u64 guest_ticks(void) {
    u64 ns = guest_ns();
    return ns / 10000u * 192u + ns % 10000u * 192u / 10000u;
}
