/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The clocks, the time zones and the snapshots' comparisons are ported from Eden
   (src/core/hle/service/psc/time/static.cpp, common.h, errors.h, clocks/standard_steady_clock_core.cpp,
   src/core/hle/service/glue/time/manager.cpp), through the runtime's time:u service that this file replaces:
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project (GPL-2.0-or-later) */
/* nn::time, native (time.hook). The SDK's clock and time zone functions, answered here with no time:u session and no
   shared memory, as nn/fs.c answers nn::fs. The behavior is that of a clean-room spec of the SDK's functions (their
   checks, their results, which failures stop the game), never the SDK's code.

   The SDK keeps its own state behind Initialize: a count and a mode, the service objects, a client of the time
   service's shared memory, and the adjustable clocks' contexts. Every function that reads that state is hooked here,
   with Initialize, so the SDK's state stays empty and no SDK code is left that would open a session. The functions
   that need no state (the calendar's arithmetic, ToCalendarTimeInUtc, the snapshot's getters) run their own code.

   The clocks: the steady clock is the guest clock from a base the runtime starts at (the host's monotonic time), with
   a random source id. The user, network, local and ephemeral network clocks share one context, the host's time at the
   start on that source. The time zone is a fixed offset, the host's at the start (no daylight saving), and the rules
   the game gets are this file's own: the SDK only hands them back */
#include "nx/nn/time.h"
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/hle.h"
#include "nx/hle/result.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "common/platform/thread.h"
#include "common/utilities/endian.h"
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TIME_NS                   1000000000ll
#define TIME_INITIAL_YEAR         2019 /* set:sys's time!standard_user_clock_initial_year on the 12.1.0 set reports */
#define TIME_AUTOMATIC_CORRECTION 1    /* the console's default: the clock is set from the network */

/* the SDK's own Result for a function called before Initialize: the time module (116), description 0 */
#define TIME_NOT_INITIALIZED 0x74u

/* the host's side, which a save state never holds */
static struct {
    PMutex lock; /* Initialize*, Finalize and IsInitialized, as the SDK's init lock */
} g_nntime;

/* ============================================================ the calendar */
/* days since 1970-01-01 of a proleptic Gregorian date, and back (an era of 400 years is 146097 days) */
static i64 days_from_civil(i64 y, int m, int d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(i64 z, i64* y, int* m, int* d) {
    z += 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe = z - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}
static i64 floor_div(i64 a, i64 b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

/* nn::time::CalendarTime, and nn::time::CalendarAdditionalInfo */
typedef struct {
    i64 year;
    int month, day, hour, minute, second;
} Calendar;
typedef struct {
    i32 day_of_week; /* 0 Sunday */
    i32 day_of_year; /* 0 based */
    char zone[8];
    i32 utc_offset;
} CalendarInfo;

/* a posix time in a rule's zone */
static u32 time_to_calendar(i64 posix, const TimeRule* r, Calendar* cal, CalendarInfo* info) {
    i64 t = posix + r->utc_offset, days = floor_div(t, 86400), secs = t - days * 86400;
    civil_from_days(days, &cal->year, &cal->month, &cal->day);
    R_UNLESS(cal->year >= -32768 && cal->year <= 32767, SNX_RC_TIME_OUT_OF_RANGE);
    cal->hour = (int)(secs / 3600);
    cal->minute = (int)(secs / 60 % 60);
    cal->second = (int)(secs % 60);
    info->day_of_week = (i32)(days - floor_div(days + 4, 7) * 7 + 4); /* 1970-01-01 was a Thursday */
    info->day_of_year = (i32)(days - days_from_civil(cal->year, 1, 1));
    memcpy(info->zone, r->abbreviation, 8);
    info->utc_offset = r->utc_offset;
    R_SUCCEED();
}
/* a local time back to the one posix time it is, in a zone without daylight saving */
static u32 calendar_to_time(const Calendar* cal, const TimeRule* r, i64* posix) {
    R_UNLESS(cal->month >= 1 && cal->month <= 12 && cal->day >= 1 && cal->day <= 31 && cal->hour >= 0 &&
                 cal->hour <= 23 && cal->minute >= 0 && cal->minute <= 59 && cal->second >= 0 && cal->second <= 59,
             SNX_RC_TIME_INVALID_POINTER);
    *posix = days_from_civil(cal->year, cal->month, cal->day) * 86400 + cal->hour * 3600 + cal->minute * 60 +
             cal->second - r->utc_offset;
    R_SUCCEED();
}

/* a CalendarTime in the game's memory: s16 year, then month, day, hour, minute and second, and a reserved byte */
static void put_calendar(u8* p, const Calendar* cal) {
    snx_st_le16(p, (u16)cal->year);
    p[2] = (u8)cal->month;
    p[3] = (u8)cal->day;
    p[4] = (u8)cal->hour;
    p[5] = (u8)cal->minute;
    p[6] = (u8)cal->second;
    p[7] = 0;
}
static Calendar get_calendar(const u8* p) {
    Calendar cal = {0};
    cal.year = (i16)snx_ld_le16(p);
    cal.month = (i8)p[2];
    cal.day = (i8)p[3];
    cal.hour = (i8)p[4];
    cal.minute = (i8)p[5];
    cal.second = (i8)p[6];
    return cal;
}
/* a CalendarAdditionalInfo, 0x18 bytes: the day of the week (s32 in the game's struct, a byte in a snapshot's), the
   day of the year, the zone's name, whether it is daylight saving time (s32 in the game's, a byte in a snapshot's)
   and the offset from UTC. Both forms have the same bytes for a day of the week of 0 to 6 and no daylight saving */
static void put_info(u8* p, const CalendarInfo* info) {
    memset(p, 0, 0x18);
    snx_st_le32(p, (u32)info->day_of_week);
    snx_st_le32(p + 4, (u32)info->day_of_year);
    memcpy(p + 8, info->zone, 8);
    snx_st_le32(p + 0x14, (u32)info->utc_offset);
}

/* ============================================================ time zones */
/* the zones the device can be set to: UTC (0), then the whole hours from 12 west (1) to 14 east (27) of UTC, by
   their tz names, whose sign is the other way round as POSIX has it: Etc/GMT+12 to Etc/GMT-14 */
#define TIME_ZONES 28
static void zone_name(int i, char out[0x24]) {
    memset(out, 0, 0x24);
    if (!i)
        strcpy(out, "UTC");
    else
        snprintf(out, 0x24, "Etc/GMT%+d", 13 - i);
}
static void make_rule(i32 offset, TimeRule* r) {
    *r = (TimeRule){0};
    memcpy(r->magic, "SNXZ", 4);
    r->utc_offset = offset;
    if (!offset)
        strcpy(r->abbreviation, "UTC");
    else if (offset % 3600)
        snprintf(r->abbreviation, sizeof r->abbreviation, "%+03d%02d", (int)(offset / 3600),
                 (int)((offset < 0 ? -offset : offset) / 60 % 60));
    else
        snprintf(r->abbreviation, sizeof r->abbreviation, "%+03d", (int)(offset / 3600));
}
/* the rule of a zone by its name: the device's, UTC or Etc/GMT+N */
static u32 find_rule(const char* name, TimeRule* r) {
    if (!strncmp(name, g_hle.time.location, sizeof g_hle.time.location)) {
        *r = g_hle.time.rule;
        R_SUCCEED();
    }
    if (!strcmp(name, "Etc/UTC") || !strcmp(name, "GMT") || !strcmp(name, "Etc/GMT") || !strcmp(name, "UTC")) {
        make_rule(0, r);
        R_SUCCEED();
    }
    for (int i = 1; i < TIME_ZONES; i++) {
        char z[0x24];
        zone_name(i, z);
        if (!strcmp(name, z)) {
            make_rule((i32)(i - 13) * 3600, r);
            R_SUCCEED();
        }
    }
    R_THROW(SNX_RC_TIME_NOT_FOUND);
}
/* a rule in the game's memory, which only this file's LoadTimeZoneRule makes */
static u32 read_rule(const CPU* c, u64 a, TimeRule* r) {
    const u8* p = hook_host(c, a);
    memcpy(r->magic, p, 4);
    r->utc_offset = (i32)snx_ld_le32(p + 4);
    memcpy(r->abbreviation, p + 8, 8);
    return memcmp(r->magic, "SNXZ", 4) ? SNX_RC_TIME_INVALID_POINTER : 0;
}

/* ============================================================ the clocks */
static i64 steady_ns(void) { return g_hle.time.base_ns + (i64)guest_ns(); }
/* the steady clock's time point now, in seconds */
static TimeContext steady_point(void) {
    TimeContext p = {0};
    p.value = steady_ns() / TIME_NS;
    memcpy(p.source_id, g_hle.time.source_id, 16);
    return p;
}
/* every system clock's context: taken at the start, on the steady clock's source */
static TimeContext system_context(void) {
    TimeContext c = {0};
    c.offset = g_hle.time.context_offset;
    c.value = g_hle.time.context_start;
    memcpy(c.source_id, g_hle.time.source_id, 16);
    return c;
}
/* a system clock's time from its context and the steady clock now, when they count from the same source */
static u32 context_time(const TimeContext* c, const TimeContext* now, i64* t) {
    R_UNLESS(!memcmp(c->source_id, now->source_id, 16), SNX_RC_TIME_OFFSET_INVALID);
    *t = c->offset + now->value;
    R_SUCCEED();
}

/* a SteadyClockTimePoint (s64 seconds, the 16 byte source id) and a SystemClockContext (s64 offset, then a time
   point) in the game's memory */
static void put_point(u8* p, const TimeContext* t) {
    snx_st_le64(p, (u64)t->value);
    memcpy(p + 8, t->source_id, 16);
}
static void put_context(u8* p, const TimeContext* c) {
    snx_st_le64(p, (u64)c->offset);
    put_point(p + 8, c);
}

void nntime_init(void) {
    pm_init(&g_nntime.lock);
    if (RAND_bytes(g_hle.time.source_id, 16) != 1) memcpy(g_hle.time.source_id, "ScarletNX steady", 16);
    g_hle.time.source_id[6] = (u8)(0x40 | (g_hle.time.source_id[6] & 0x0f)); /* a version 4 UUID */
    g_hle.time.source_id[8] = (u8)(0x80 | (g_hle.time.source_id[8] & 0x3f));
    g_hle.time.base_ns = (i64)g_t0_ns();

    time_t now = time(NULL);
    TimeContext start = steady_point();
    g_hle.time.context_offset = (i64)now - start.value;
    g_hle.time.context_start = start.value;

    /* the host's offset from UTC now, in minutes, as a fixed one */
    struct tm* lt = localtime(&now);
    i64 offset = 0;
    if (lt)
        offset = days_from_civil(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday) * 86400 + lt->tm_hour * 3600 +
                 lt->tm_min * 60 + lt->tm_sec - (i64)now;
    offset = (offset + (offset < 0 ? -30 : 30)) / 60 * 60;
    make_rule((i32)offset, &g_hle.time.rule);
    /* its name, for whole hours; the others have none here, and are called UTC with their own offset */
    zone_name(offset && !(offset % 3600) && offset >= -12 * 3600 && offset <= 14 * 3600 ? (int)(offset / 3600) + 13 : 0,
              g_hle.time.location);
    LOG("time: the time zone is %s (UTC%s)\n", g_hle.time.location, g_hle.time.rule.abbreviation);
}

/* ============================================================ the snapshots */
/* nn::time::ClockSnapshot's fields */
#define SNAP_SIZE             0xD0
#define SNAP_USER_CONTEXT     0x00
#define SNAP_NETWORK_CONTEXT  0x20
#define SNAP_USER_TIME        0x40
#define SNAP_NETWORK_TIME     0x48
#define SNAP_USER_CALENDAR    0x50
#define SNAP_NETWORK_CALENDAR 0x58
#define SNAP_USER_INFO        0x60
#define SNAP_NETWORK_INFO     0x78
#define SNAP_STEADY           0x90
#define SNAP_LOCATION         0xA8
#define SNAP_AUTOMATIC        0xCC
#define SNAP_TYPE             0xCD

/* a snapshot of the clocks from their contexts, of a type: 1 from the standard clocks, 2 from the adjustable ones */
static u32 snapshot(u8 s[SNAP_SIZE], u8 type, const TimeContext* user, const TimeContext* network) {
    memset(s, 0, SNAP_SIZE);
    put_context(s + SNAP_USER_CONTEXT, user);
    put_context(s + SNAP_NETWORK_CONTEXT, network);
    TimeContext now = steady_point();
    put_point(s + SNAP_STEADY, &now);
    s[SNAP_AUTOMATIC] = TIME_AUTOMATIC_CORRECTION;
    memcpy(s + SNAP_LOCATION, g_hle.time.location, sizeof g_hle.time.location);
    i64 user_time;
    R_TRY(context_time(user, &now, &user_time));
    snx_st_le64(s + SNAP_USER_TIME, (u64)user_time);
    Calendar cal = {0};
    CalendarInfo info = {0};
    R_TRY(time_to_calendar(user_time, &g_hle.time.rule, &cal, &info));
    put_calendar(s + SNAP_USER_CALENDAR, &cal);
    put_info(s + SNAP_USER_INFO, &info);
    i64 network_time;
    if (R_FAILED(context_time(network, &now, &network_time))) network_time = 0;
    snx_st_le64(s + SNAP_NETWORK_TIME, (u64)network_time);
    R_TRY(time_to_calendar(network_time, &g_hle.time.rule, &cal, &info));
    put_calendar(s + SNAP_NETWORK_CALENDAR, &cal);
    put_info(s + SNAP_NETWORK_INFO, &info);
    s[SNAP_TYPE] = type;
    R_SUCCEED();
}
static int same_source(const u8* a, const u8* b) { return !memcmp(a + 8, b + 8, 16); } /* two time points' ids */

/* how far the user moved the user clock between two snapshots, in ns: none when the network corrected it */
static i64 user_difference(const u8* sa, const u8* sb) {
    i64 oa = (i64)snx_ld_le64(sa + SNAP_USER_CONTEXT), ob = (i64)snx_ld_le64(sb + SNAP_USER_CONTEXT);
    if (!memcmp(sa + SNAP_USER_CONTEXT, sb + SNAP_USER_CONTEXT, 0x20) ||
        !same_source(sa + SNAP_USER_CONTEXT + 8, sb + SNAP_USER_CONTEXT + 8))
        return 0;
    if (!sa[SNAP_AUTOMATIC] || !sb[SNAP_AUTOMATIC]) return (i64)((u64)ob - (u64)oa) * TIME_NS;
    if (same_source(sa + SNAP_NETWORK_CONTEXT + 8, sa + SNAP_STEADY) ||
        same_source(sb + SNAP_NETWORK_CONTEXT + 8, sb + SNAP_STEADY))
        return 0;
    return (i64)((u64)ob - (u64)oa) * TIME_NS;
}
/* the time between two snapshots, by the steady clock when it counted both, else by the network clock */
static u32 span_between(const u8* sa, const u8* sb, i64* span) {
    i64 ta, tb;
    if (same_source(sa + SNAP_STEADY, sb + SNAP_STEADY)) {
        ta = (i64)snx_ld_le64(sa + SNAP_STEADY);
        tb = (i64)snx_ld_le64(sb + SNAP_STEADY);
    } else {
        ta = (i64)snx_ld_le64(sa + SNAP_NETWORK_TIME);
        tb = (i64)snx_ld_le64(sb + SNAP_NETWORK_TIME);
        R_UNLESS(ta && tb, SNX_RC_TIME_NOT_COMPARABLE);
    }
    R_UNLESS(!((tb > 0 && ta < tb - 0x7fffffffffffffffll) || (tb < 0 && ta > tb + 0x7fffffffffffffffll)),
             SNX_RC_TIME_OVERFLOWED);
    i64 s = tb - ta;
    R_UNLESS(s <= 0x7fffffffffffffffll / TIME_NS && s >= -0x7fffffffffffffffll / TIME_NS, SNX_RC_TIME_OVERFLOWED);
    *span = s * TIME_NS;
    R_SUCCEED();
}

/* ============================================================ the hooks */
/* the SDK stops the game on these */
SNX_NORETURN static void precondition(const char* function, const char* what) {
    fatal("nn::time::%s: %s, which the SDK stops the game on", function, what);
}
SNX_NORETURN static void result_abort(const char* function, u32 r) {
    fatal("nn::time::%s failed with result 0x%x (error %04u-%04u), which the SDK stops the game on", function, r,
          2000 + (r & 0x1ff), (r >> 9) & 0x1fff);
}
static int initialized(void) { return __atomic_load_n(&g_hle.time.count, __ATOMIC_ACQUIRE) != 0; }
static void need_initialized(const char* function) {
    if (!initialized()) precondition(function, "nn::time is not initialized");
}

/* ---- Initialize, Finalize */

/* every Initialize* in one: a later call must give the mode of the first */
static u32 initialize(u32 mode) {
    pm_lock(&g_nntime.lock);
    if (g_hle.time.count) {
        if (g_hle.time.mode != mode) {
            pm_unlock(&g_nntime.lock);
            fatal("nn::time::Initialize* of mode %u after one of mode %u, which the SDK stops the game on", mode,
                  g_hle.time.mode);
        }
        g_hle.time.count++;
        pm_unlock(&g_nntime.lock);
        R_SUCCEED();
    }
    if (!g_hle.time.adjusted) { /* the first Initialize of the run: the adjustable clocks take the standard ones' */
        g_hle.time.adjustable_user = system_context();
        g_hle.time.adjustable_network = system_context();
        g_hle.time.adjusted = 1;
    }
    g_hle.time.mode = mode;
    __atomic_store_n(&g_hle.time.count, 1u, __ATOMIC_RELEASE);
    pm_unlock(&g_nntime.lock);
    R_SUCCEED();
}
u32 nntime_initialize(CPU* c) {
    (void)c;
    return initialize(1);
}
u32 nntime_initialize_for_menu(CPU* c) {
    (void)c;
    return initialize(2);
}
u32 nntime_initialize_for_system(CPU* c) {
    (void)c;
    return initialize(3);
}
u32 nntime_initialize_for_repair(CPU* c) {
    (void)c;
    return initialize(4);
}
u32 nntime_initialize_for_system_user(CPU* c) {
    (void)c;
    return initialize(5);
}
u32 nntime_finalize(CPU* c) {
    (void)c;
    pm_lock(&g_nntime.lock);
    if (g_hle.time.count) __atomic_store_n(&g_hle.time.count, g_hle.time.count - 1, __ATOMIC_RELEASE);
    pm_unlock(&g_nntime.lock);
    R_SUCCEED();
}
int nntime_is_initialized(CPU* c) {
    (void)c;
    pm_lock(&g_nntime.lock);
    int r = g_hle.time.count != 0;
    pm_unlock(&g_nntime.lock);
    return r;
}

/* ---- the standard clocks */

/* the user or network clock's time now: they share one context */
static i64 system_now(void) {
    TimeContext ctx = system_context(), now = steady_point();
    i64 t = 0;
    (void)context_time(&ctx, &now, &t); /* one source: never fails */
    return t;
}
u32 nntime_user_get_current_time(CPU* c, u64 out) {
    R_UNLESS(initialized(), TIME_NOT_INITIALIZED);
    snx_st_le64(hook_host(c, out), (u64)system_now());
    R_SUCCEED();
}
i64 nntime_user_now(CPU* c) {
    (void)c;
    return initialized() ? system_now() : 0;
}
u32 nntime_network_get_current_time(CPU* c, u64 out) { return nntime_user_get_current_time(c, out); }
i64 nntime_network_now(CPU* c) { return nntime_user_now(c); }
static u32 get_context_out(CPU* c, u64 out) {
    R_UNLESS(initialized(), TIME_NOT_INITIALIZED);
    TimeContext ctx = system_context();
    put_context(hook_host(c, out), &ctx);
    R_SUCCEED();
}
u32 nntime_user_get_context(CPU* c, u64 out) { return get_context_out(c, out); }
u32 nntime_network_get_context(CPU* c, u64 out) { return get_context_out(c, out); }

i64 nntime_steady_now(CPU* c) {
    (void)c;
    return initialized() ? steady_point().value : 0;
}
u32 nntime_steady_get_current_time_point(CPU* c, u64 out) {
    R_UNLESS(initialized(), TIME_NOT_INITIALIZED);
    TimeContext now = steady_point();
    put_point(hook_host(c, out), &now);
    R_SUCCEED();
}

/* ---- the time zone */

void nntime_get_device_location_name(CPU* c, u64 out) {
    if (!out) precondition("GetDeviceLocationName", "the output is null");
    need_initialized("GetDeviceLocationName");
    memcpy(hook_host(c, out), g_hle.time.location, sizeof g_hle.time.location);
}
u32 nntime_load_time_zone_rule(CPU* c, u64 rule, u64 name) {
    R_UNLESS(initialized(), TIME_NOT_INITIALIZED);
    char z[0x25];
    memcpy(z, hook_host(c, name), 0x24);
    z[0x24] = 0;
    TimeRule r;
    R_TRY(find_rule(z, &r));
    u8* p = hook_host(c, rule);
    memset(p, 0, 0x4000);
    memcpy(p, r.magic, 4);
    snx_st_le32(p + 4, (u32)r.utc_offset);
    memcpy(p + 8, r.abbreviation, 8);
    R_SUCCEED();
}
/* a posix time to a calendar time in a zone: the CalendarAdditionalInfo only when the game asked for it */
static u32 to_calendar(CPU* c, u64 cal, u64 info, u64 time, const TimeRule* r) {
    Calendar ct;
    CalendarInfo ci;
    R_TRY(time_to_calendar((i64)snx_ld_le64(hook_host(c, time)), r, &ct, &ci));
    put_calendar(hook_host(c, cal), &ct);
    if (info) put_info(hook_host(c, info), &ci);
    R_SUCCEED();
}
u32 nntime_to_calendar_time(CPU* c, u64 cal, u64 info, u64 time) {
    R_UNLESS(initialized(), TIME_NOT_INITIALIZED);
    return to_calendar(c, cal, info, time, &g_hle.time.rule);
}
u32 nntime_to_calendar_time_rule(CPU* c, u64 cal, u64 info, u64 time, u64 rule) {
    R_UNLESS(initialized(), TIME_NOT_INITIALIZED);
    TimeRule r;
    R_TRY(read_rule(c, rule, &r));
    return to_calendar(c, cal, info, time, &r);
}

/* ---- the snapshots */

/* a context that counts on the steady clock: an adjustable clock before its first Adjust has none */
static int on_steady(const TimeContext* ctx) { return !memcmp(ctx->source_id, g_hle.time.source_id, 16); }
/* the adjustable network clock's context, which takes the standard one's again when it no longer counts */
static TimeContext adjustable_network(void) {
    if (!on_steady(&g_hle.time.adjustable_network)) g_hle.time.adjustable_network = system_context();
    return g_hle.time.adjustable_network;
}

void nntime_snapshot_create_standard(CPU* c, u64 s) {
    need_initialized("ClockSnapshot::CreateWithStandardSystemClock");
    TimeContext ctx = system_context();
    u8 snap[SNAP_SIZE];
    u32 r = snapshot(snap, 1, &ctx, &ctx);
    if (r) result_abort("ClockSnapshot::CreateWithStandardSystemClock", r);
    memcpy(hook_host(c, s), snap, SNAP_SIZE);
}
i64 nntime_user_difference(CPU* c, u64 a, u64 b) {
    need_initialized("CalculateStandardUserSystemClockDifferenceByUser");
    return user_difference(hook_host(c, a), hook_host(c, b));
}
int nntime_span_between(CPU* c, u64 out, u64 a, u64 b) {
    need_initialized("CalculateSpanBetween");
    i64 span;
    if (span_between(hook_host(c, a), hook_host(c, b), &span)) return 0;
    snx_st_le64(hook_host(c, out), (u64)span);
    return 1;
}
void nntime_snapshot_create_adjustable(CPU* c, u64 s) {
    static const char* const function = "ClockSnapshot::CreateWithAdjustableSystemClock";
    need_initialized(function);
    TimeContext user = g_hle.time.adjustable_user, network = adjustable_network();
    u8 snap[SNAP_SIZE];
    u32 r = snapshot(snap, 2, &user, &network);
    if (r) result_abort(function, r);
    memcpy(hook_host(c, s), snap, SNAP_SIZE);
}

/* ---- the timespecs: the C library's clock_gettime(CLOCK_REALTIME) calls the user clock's through a pointer */

/* a struct timespec of the guest (s64 seconds, s64 nanoseconds) of a steady clock time X in ns, plus a context's
   offset in seconds: the truncating division, so a negative X gives negative nanoseconds */
static void put_timespec(u8* p, i64 offset, i64 x) {
    snx_st_le64(p, (u64)(offset + x / TIME_NS));
    snx_st_le64(p + 8, (u64)(x - x / TIME_NS * TIME_NS));
}
u32 nntime_steady_get_current_timespec(CPU* c, u64 out) {
    R_UNLESS(initialized(), TIME_NOT_INITIALIZED);
    put_timespec(hook_host(c, out), 0, steady_ns());
    R_SUCCEED();
}
u32 nntime_user_get_current_timespec(CPU* c, u64 out) {
    R_UNLESS(initialized(), TIME_NOT_INITIALIZED);
    put_timespec(hook_host(c, out), g_hle.time.context_offset, steady_ns());
    R_SUCCEED();
}
u32 nntime_adjustable_user_get_current_timespec(CPU* c, u64 out) {
    need_initialized("GetAdjustableUserSystemClockCurrentTimespec");
    R_UNLESS(on_steady(&g_hle.time.adjustable_user), SNX_RC_TIME_OFFSET_INVALID);
    put_timespec(hook_host(c, out), g_hle.time.adjustable_user.offset, steady_ns());
    R_SUCCEED();
}

/* ---- the adjustable clocks: a standard clock's context, taken at the first Initialize or the last Adjust */

static i64 adjusted_now(const TimeContext* ctx) { return ctx->offset + steady_point().value; }
void nntime_adjustable_user_adjust(CPU* c) {
    (void)c;
    if (!initialized()) result_abort("AdjustableUserSystemClock::Adjust", TIME_NOT_INITIALIZED);
    g_hle.time.adjustable_user = system_context();
}
u32 nntime_adjustable_user_get_current_time(CPU* c, u64 out) {
    need_initialized("AdjustableUserSystemClock::GetCurrentTime");
    R_UNLESS(on_steady(&g_hle.time.adjustable_user), SNX_RC_TIME_OFFSET_INVALID);
    snx_st_le64(hook_host(c, out), (u64)adjusted_now(&g_hle.time.adjustable_user));
    R_SUCCEED();
}
void nntime_adjustable_user_get_context(CPU* c, u64 out) {
    put_context(hook_host(c, out), &g_hle.time.adjustable_user);
}
i64 nntime_adjustable_user_now(CPU* c) {
    (void)c;
    need_initialized("AdjustableUserSystemClock::now");
    return on_steady(&g_hle.time.adjustable_user) ? adjusted_now(&g_hle.time.adjustable_user) : 0;
}
void nntime_adjustable_network_adjust(CPU* c) {
    (void)c;
    if (!initialized()) result_abort("AdjustableNetworkSystemClock::Adjust", TIME_NOT_INITIALIZED);
    g_hle.time.adjustable_network = system_context();
}
u32 nntime_adjustable_network_get_current_time(CPU* c, u64 out) {
    need_initialized("AdjustableNetworkSystemClock::GetCurrentTime");
    TimeContext ctx = adjustable_network();
    snx_st_le64(hook_host(c, out), (u64)adjusted_now(&ctx));
    R_SUCCEED();
}
void nntime_adjustable_network_get_context(CPU* c, u64 out) {
    need_initialized("AdjustableNetworkSystemClock::GetSystemClockContext");
    TimeContext ctx = adjustable_network();
    put_context(hook_host(c, out), &ctx);
}
i64 nntime_adjustable_network_now(CPU* c) {
    (void)c;
    need_initialized("AdjustableNetworkSystemClock::now");
    TimeContext ctx = adjustable_network();
    return adjusted_now(&ctx);
}

/* ---- the ephemeral network clock, and the clocks an application may not set (time:u has no write permission) */

u32 nntime_ephemeral_get_current_time(CPU* c, u64 out) { return nntime_user_get_current_time(c, out); }
i64 nntime_ephemeral_now(CPU* c) { return nntime_user_now(c); }
u32 nntime_ephemeral_get_context(CPU* c, u64 out) { return get_context_out(c, out); }
u32 nntime_set_clock(CPU* c, u64 value) {
    (void)c, (void)value;
    R_UNLESS(initialized(), TIME_NOT_INITIALIZED);
    R_THROW(SNX_RC_TIME_NO_CAPABILITY);
}

/* ---- the settings of the clocks */

int nntime_is_automatic_correction_enabled(CPU* c) {
    (void)c;
    need_initialized("IsStandardUserSystemClockAutomaticCorrectionEnabled");
    return TIME_AUTOMATIC_CORRECTION;
}
void nntime_set_automatic_correction_enabled(CPU* c, int enabled) {
    (void)c, (void)enabled;
    need_initialized("SetStandardUserSystemClockAutomaticCorrectionEnabled");
    result_abort("SetStandardUserSystemClockAutomaticCorrectionEnabled", SNX_RC_TIME_NO_CAPABILITY);
}
/* automatic correction was on from the start: its time is that of the clocks' context */
void nntime_automatic_correction_updated_time(CPU* c, u64 out) {
    need_initialized("detail::GetStandardUserSystemClockAutomaticCorrectionUpdatedTime");
    TimeContext ctx = system_context();
    put_point(hook_host(c, out), &ctx);
}
/* the network clock was set when the runtime started, well within the accuracy the console asks of it (30 days) */
int nntime_is_accuracy_sufficient(CPU* c) {
    (void)c;
    need_initialized("StandardNetworkSystemClock::IsAccuracySufficient");
    return 1;
}
i32 nntime_initial_year(CPU* c) {
    (void)c;
    need_initialized("GetStandardUserSystemClockInitialYear");
    return TIME_INITIAL_YEAR;
}

/* ---- the time zones */

i32 nntime_total_location_name_count(CPU* c) {
    (void)c;
    need_initialized("GetTotalLocationNameCount");
    return TIME_ZONES;
}
void nntime_load_location_name_list(CPU* c, u64 count, u64 out, i32 max, i32 index) {
    static const char* const function = "LoadLocationNameList";
    need_initialized(function);
    if (index < 0) result_abort(function, SNX_RC_TIME_INVALID_POINTER);
    i32 n = 0;
    while (n < max && index + n < TIME_ZONES) {
        zone_name(index + n, (char*)hook_host(c, out + (u64)n * 0x24));
        n++;
    }
    snx_st_le32(hook_host(c, count), (u32)n);
}
/* the time zone data of firmware 12.1.0, the one set:sys reports */
void nntime_get_time_zone_rule_version(CPU* c, u64 out) {
    need_initialized("GetTimeZoneRuleVersion");
    u8* p = hook_host(c, out);
    memset(p, 0, 0x10);
    memcpy(p, "2021a", 5);
}
/* the location was set when the runtime started, at the steady clock time the clocks' context was taken */
void nntime_get_device_location_name_and_updated_time(CPU* c, u64 name, u64 point) {
    need_initialized("detail::GetDeviceLocationNameAndUpdatedTime");
    memcpy(hook_host(c, name), g_hle.time.location, sizeof g_hle.time.location);
    TimeContext ctx = system_context();
    put_point(hook_host(c, point), &ctx);
}
void nntime_set_device_location_name(CPU* c, u64 name) {
    (void)c, (void)name;
    need_initialized("SetDeviceLocationName");
    result_abort("SetDeviceLocationName", SNX_RC_TIME_NO_CAPABILITY);
}
/* a local time to the posix times it is: one, in a zone without daylight saving. The SDK asks for at most 2 and
   checks nothing; the times and the count go straight into the game's memory */
static u32 to_posix(CPU* c, u64 count, u64 out, i32 max, u64 cal, const TimeRule* r) {
    R_UNLESS((max < 2 ? max : 2) >= 1, SNX_RC_TIME_INVALID_POINTER);
    Calendar ct = get_calendar(hook_host(c, cal));
    i64 t;
    R_TRY(calendar_to_time(&ct, r, &t));
    snx_st_le64(hook_host(c, out), (u64)t);
    snx_st_le32(hook_host(c, count), 1);
    R_SUCCEED();
}
u32 nntime_to_posix_time(CPU* c, u64 count, u64 out, i32 max, u64 cal) {
    R_UNLESS(initialized(), TIME_NOT_INITIALIZED);
    return to_posix(c, count, out, max, cal, &g_hle.time.rule);
}
u32 nntime_to_posix_time_rule(CPU* c, u64 count, u64 out, i32 max, u64 cal, u64 rule) {
    R_UNLESS(initialized(), TIME_NOT_INITIALIZED);
    TimeRule r;
    R_TRY(read_rule(c, rule, &r));
    return to_posix(c, count, out, max, cal, &r);
}
