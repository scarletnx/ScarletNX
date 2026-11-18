/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's clock: real time since the run started, in nanoseconds and in the console's system ticks, less the time
   the game spent paused */
#include "common/utilities/bits.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/plat.h"
#include <stddef.h>
#include <stdatomic.h>

/* The guest time a pause stopped at plus 1 (0 while none is in force), and the clock's reading at guest time 0, moved
   on by each pause's length as it ends. A resume stores start before it clears frozen, so a reader that sees no pause
   sees the start that goes with it. NCE's counter stubs read both words, frozen first, as guest_ns does (clock.h) */
static struct ClockWords {
    _Atomic u64 frozen, start;
    ClockCounter counter; /* mul_hi 0: the clock is plat_ns. Set once, mul_hi last */
    u8 ounce;             /* the system tick is the Ounce's 31.25 MHz */
} g_clock;
typedef char
    snx_check_clock_words[offsetof(struct ClockWords, frozen) == 0 && offsetof(struct ClockWords, start) == 8 ? 1 : -1];

#if defined(__aarch64__)
/* the host's virtual counter, read in order with the code before it as Linux's vDSO reads it, so two reads never go
   backwards */
static u64 counter_read(void) {
    u64 v;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
}
#endif
/* the clock's reading in nanoseconds: plat_ns, or the counter's under NCE, in the arithmetic of its stubs */
static u64 clock_now(void) {
#if defined(__aarch64__)
    /* the counter the NCE stubs read in place (words): a plain object, atomic here */
    u64 hi = atomic_load_explicit((_Atomic u64*)&g_clock.counter.mul_hi, memory_order_acquire);
    if (hi) {
        u64 cnt = counter_read(), top;
        snx_mul_wide(cnt, g_clock.counter.mul_lo, &top);
        return cnt * hi + top - g_clock.counter.origin;
    }
#endif
    return plat_ns();
}

void clock_start(void) { atomic_store_explicit(&g_clock.start, clock_now(), memory_order_release); }
u64 g_t0_ns(void) { return atomic_load_explicit(&g_clock.start, memory_order_acquire); }
u64 guest_ns(void) {
    u64 f = atomic_load_explicit(&g_clock.frozen, memory_order_acquire);
    return f ? f - 1 : clock_now() - atomic_load_explicit(&g_clock.start, memory_order_acquire);
}
void clock_pause(void) {
    if (!atomic_load_explicit(&g_clock.frozen, memory_order_relaxed))
        atomic_store_explicit(&g_clock.frozen, guest_ns() + 1, memory_order_release);
}
void clock_resume(void) {
    u64 f = atomic_load_explicit(&g_clock.frozen, memory_order_relaxed);
    if (!f) return;
    atomic_store_explicit(&g_clock.start, clock_now() - (f - 1), memory_order_release);
    atomic_store_explicit(&g_clock.frozen, 0, memory_order_release);
}

/* a save state's load: guest time is the state's, stopped there while the game is paused */
void clock_set(u64 ns) {
    if (atomic_load_explicit(&g_clock.frozen, memory_order_relaxed))
        atomic_store_explicit(&g_clock.frozen, ns + 1, memory_order_release);
    else
        atomic_store_explicit(&g_clock.start, clock_now() - ns, memory_order_release);
}

int clock_from_counter(ClockCounter* out) {
#if defined(__aarch64__)
    u64 freq, cnt, ns, rem, top;
    ClockCounter c;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    out->freq = freq;
    if (!freq || freq > 1000000000u) return -1; /* a counter of 1 GHz at most: mul_hi is never 0 */
    c.freq = freq;
    c.words = (u64)(uintptr_t)&g_clock;
    c.mul_hi = 1000000000u / freq;
    c.mul_lo = snx_div_wide(1000000000u % freq, 0, freq, &rem);
    /* the counter's nanoseconds start where plat_ns is now, so guest time goes on from where it is: the counter first,
       so its clock is ahead of plat_ns by the time between the reads, and a reader of both never goes back */
    cnt = counter_read();
    ns = plat_ns();
    snx_mul_wide(cnt, c.mul_lo, &top);
    c.origin = cnt * c.mul_hi + top - ns;
    g_clock.counter.words = c.words;
    g_clock.counter.mul_lo = c.mul_lo;
    g_clock.counter.origin = c.origin;
    atomic_store_explicit((_Atomic u64*)&g_clock.counter.mul_hi, c.mul_hi,
                          memory_order_release); /* last: a reader that sees it has the rest */
    *out = c;
    return 0;
#else
    out->freq = 0;
    return -1;
#endif
}

u64 guest_gpu_ticks(void) { return snx_gpu_ticks_of_ns(guest_ns()); }
/* An Ounce SDK's nn::os::GetSystemTickFrequency returns 31,250,000 as a constant (MOVZ and MOVK), and its tick
   conversions use the same constant. It never reads CNTFRQ_EL0. With 19.2 MHz ticks, the game's time ran at 0.61 of
   real time */
void clock_ounce(void) { g_clock.ounce = 1; }
ClockRate clock_rate(void) {
    static const ClockRate switch_rate = {19200000u, 10000u, 192u}, ounce_rate = {31250000u, 4000u, 125u};
    return g_clock.ounce ? ounce_rate : switch_rate;
}
/* whole steps, then the rest, so neither part overflows or rounds the way a double would after a few hours */
u64 guest_ticks(void) {
    ClockRate r = clock_rate();
    u64 ns = guest_ns();
    return ns / r.step_ns * r.step_ticks + ns % r.step_ns * r.step_ticks / r.step_ns;
}
