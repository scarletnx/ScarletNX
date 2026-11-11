/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NX_NN_TIME_H
#define SNX_NX_NN_TIME_H
#include "common/cpu/aarch64/cpu.h"

/* a time zone rule as nn/time.c writes it into the game's nn::time::TimeZoneRule, which the SDK never reads: only
   the native ToCalendarTime takes it back */
typedef struct {
    char magic[4];  /* "SNXZ" */
    i32 utc_offset; /* seconds east of UTC */
    char abbreviation[8];
} TimeRule;

/* a nn::time::SystemClockContext: the offset of a system clock from the steady clock, and the steady clock's time
   point (s, its source) the offset was taken at */
typedef struct {
    i64 offset;
    i64 value;
    u8 source_id[16];
} TimeContext;

/* The native nn::time's state (g_hle.time, which a save state holds): the clocks, the device's time zone, and what
   the SDK's own nn::time keeps between calls (its init count and mode, and the adjustable clocks' contexts) */
typedef struct {
    u8 source_id[16]; /* the steady clock's: the console makes a new one when its RTC resets */
    i64 base_ns;      /* the steady clock at the guest clock's start */
    /* every system clock's context: its offset, and the steady clock's time (s) it was taken at */
    i64 context_offset, context_start;
    TimeRule rule; /* the device's */
    char location[0x24];
    u32 count;    /* Initialize* calls less Finalize calls */
    u32 mode;     /* the Initialize* of the first of them: 1 Initialize, 2 ForMenu, 3 ForSystem, 4 ForRepair, 5
                     ForSystemUser */
    u32 adjusted; /* the first Initialize took the adjustable clocks' contexts, which nothing clears */
    TimeContext adjustable_user, adjustable_network;
} HleTime;

/* the clocks and the time zone, the host's at startup: before hook_install */
void nntime_init(void);
#endif
