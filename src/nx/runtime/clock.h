/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's clock (clock.c): the host's monotonic time, counted from the run's start, less the time paused */
#ifndef SNX_CLOCK_H
#define SNX_CLOCK_H
#include "common/cpu/aarch64/cpu.h"

void clock_start(void); /* the run's start: guest time 0 */
u64 g_t0_ns(void);      /* plat_ns() at guest time 0: clock_start's, moved on by the pauses since */
u64 guest_ns(void);     /* nanoseconds since clock_start, less the time paused */
/* a game pause (pause.c): guest time stops at clock_pause, and goes on from there at clock_resume, so the game sees no
   gap. The frontend's thread calls both */
void clock_pause(void);
void clock_resume(void);
void clock_set(u64 ns); /* a save state's load: guest time ns from here */
/* the same time as the console's system tick (svcGetSystemTick, CNTPCT_EL0): 19.2 MHz, or an Ounce's 31.25 MHz after
   clock_ounce */
u64 guest_ticks(void);
void clock_ounce(void); /* an Ounce game's tick, set once before the game's first thread */
/* the system tick's rate: freq ticks a second, counted as step_ticks ticks every step_ns nanoseconds (19.2 MHz: 192
   every 10 us; 31.25 MHz: 125 every 4 us), whole steps, then the rest. NCE's counter stubs count the same way */
typedef struct {
    u32 freq, step_ns, step_ticks;
} ClockRate;
ClockRate clock_rate(void);
/* The graphics unit's timer counts 384 ticks every 625 nanoseconds (614.4 MHz): the unit of a report's timestamp and of
   nvhost-ctrl-gpu's GET_GPU_TIME, which the driver turns into nanoseconds by one ratio. With n = q * 625 + r,
   n * 384 / 625 is q * 384 + r * 384 / 625 exactly (r * 384 can't carry), and never overflows where n * 384 would; the
   same split converts back */
#define SNX_GPU_TICKS_NUM 384u
#define SNX_GPU_TICKS_DEN 625u
static inline u64 snx_gpu_ticks_of_ns(u64 ns) {
    return ns / SNX_GPU_TICKS_DEN * SNX_GPU_TICKS_NUM + ns % SNX_GPU_TICKS_DEN * SNX_GPU_TICKS_NUM / SNX_GPU_TICKS_DEN;
}
static inline u64 snx_gpu_ns_of_ticks(u64 ticks) {
    return ticks / SNX_GPU_TICKS_NUM * SNX_GPU_TICKS_DEN +
           ticks % SNX_GPU_TICKS_NUM * SNX_GPU_TICKS_DEN / SNX_GPU_TICKS_NUM;
}
u64 guest_gpu_ticks(void); /* the guest clock in the graphics unit's ticks */

/* The guest clock in the host's virtual counter (CNTVCT_EL0), as NCE's stubs read it with no outcall (nce_patch.c):
   the clock's nanoseconds are cnt * mul_hi + (cnt * mul_lo >> 64) - origin, and words is the address of its two words,
   frozen and then start (clock.c). freq: the counter's, in Hz (CNTFRQ_EL0) */
typedef struct {
    u64 words, mul_lo, mul_hi, origin, freq;
} ClockCounter;
/* from here guest time follows the host's counter, going on from where it is: on an AArch64 host 0 with *out filled.
   -1 elsewhere, or for a frequency of 0 or past 1 GHz, which out->freq then holds */
int clock_from_counter(ClockCounter* out);

#endif
