/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's clock: real time since the run started, in nanoseconds and in the console's system ticks, less the time
   the game spent paused */
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/plat.h"
#include <stddef.h>

/* The guest time a pause stopped at plus 1 (0 while none is in force), and the clock's reading at guest time 0, moved
   on by each pause's length as it ends. A resume stores start before it clears frozen, so a reader that sees no pause
   sees the start that goes with it. NCE's counter stubs read both words, frozen first, as guest_ns does (clock.h) */
static struct ClockWords {
    u64 frozen, start;
    ClockCounter counter; /* mul_hi 0: the clock is plat_ns. Set once, mul_hi last */
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
    u64 hi = __atomic_load_n(&g_clock.counter.mul_hi, __ATOMIC_ACQUIRE);
    if (hi) {
        __extension__ typedef unsigned __int128 u128;
        u64 cnt = counter_read();
        return cnt * hi + (u64)(((u128)cnt * g_clock.counter.mul_lo) >> 64) - g_clock.counter.origin;
    }
#endif
    return plat_ns();
}

void clock_start(void) { __atomic_store_n(&g_clock.start, clock_now(), __ATOMIC_RELEASE); }
u64 g_t0_ns(void) { return __atomic_load_n(&g_clock.start, __ATOMIC_ACQUIRE); }
u64 guest_ns(void) {
    u64 f = __atomic_load_n(&g_clock.frozen, __ATOMIC_ACQUIRE);
    return f ? f - 1 : clock_now() - __atomic_load_n(&g_clock.start, __ATOMIC_ACQUIRE);
}
void clock_pause(void) {
    if (!__atomic_load_n(&g_clock.frozen, __ATOMIC_RELAXED))
        __atomic_store_n(&g_clock.frozen, guest_ns() + 1, __ATOMIC_RELEASE);
}
void clock_resume(void) {
    u64 f = __atomic_load_n(&g_clock.frozen, __ATOMIC_RELAXED);
    if (!f) return;
    __atomic_store_n(&g_clock.start, clock_now() - (f - 1), __ATOMIC_RELEASE);
    __atomic_store_n(&g_clock.frozen, 0, __ATOMIC_RELEASE);
}

/* a save state's load: guest time is the state's, stopped there while the game is paused */
void clock_set(u64 ns) {
    if (__atomic_load_n(&g_clock.frozen, __ATOMIC_RELAXED))
        __atomic_store_n(&g_clock.frozen, ns + 1, __ATOMIC_RELEASE);
    else
        __atomic_store_n(&g_clock.start, clock_now() - ns, __ATOMIC_RELEASE);
}

int clock_from_counter(ClockCounter* out) {
#if defined(__aarch64__)
    __extension__ typedef unsigned __int128 u128;
    u64 freq, cnt, ns;
    ClockCounter c;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    out->freq = freq;
    if (!freq || freq > 1000000000u) return -1; /* a counter of 1 GHz at most: mul_hi is never 0 */
    c.freq = freq;
    c.words = (u64)(uintptr_t)&g_clock;
    c.mul_hi = 1000000000u / freq;
    c.mul_lo = (u64)(((u128)(1000000000u % freq) << 64) / freq);
    /* the counter's nanoseconds start where plat_ns is now, so guest time goes on from where it is: the counter first,
       so its clock is ahead of plat_ns by the time between the reads, and a reader of both never goes back */
    cnt = counter_read();
    ns = plat_ns();
    c.origin = cnt * c.mul_hi + (u64)(((u128)cnt * c.mul_lo) >> 64) - ns;
    g_clock.counter.words = c.words;
    g_clock.counter.mul_lo = c.mul_lo;
    g_clock.counter.origin = c.origin;
    __atomic_store_n(&g_clock.counter.mul_hi, c.mul_hi,
                     __ATOMIC_RELEASE); /* last: a reader that sees it has the rest */
    *out = c;
    return 0;
#else
    out->freq = 0;
    return -1;
#endif
}

/* 19.2 MHz is 192 ticks every 10 us: whole 10 us steps, then the rest, so neither part overflows or rounds the way
   a double would after a few hours */
u64 guest_ticks(void) {
    u64 ns = guest_ns();
    return ns / 10000u * 192u + ns % 10000u * 192u / 10000u;
}
