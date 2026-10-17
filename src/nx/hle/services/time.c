/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/psc/time/static.cpp, shared_memory.h, shared_memory.cpp, common.h,
   errors.h, clocks/standard_steady_clock_core.cpp, src/core/hle/service/glue/time/manager.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project (GPL-2.0-or-later) */
/* time:u, time:a and time:s: the steady clock (the host's monotonic time), the system clocks (the host's time, the
   user's, local, network and ephemeral network clocks all one context), and a time zone of a fixed offset, the
   host's at startup (no daylight saving). The time zone rules the game gets are this file's own: the SDK only hands
   them back */
#include "nx/hle/services/services_int.h"
#include "nx/runtime/clock.h"
#include <openssl/rand.h>
#include <time.h>

#include "hle/services/time.swipc.inc"

#define TIME_CLOCK_MISMATCH      RC(116, 102)
#define TIME_NOT_FOUND           RC(116, 200)
#define TIME_OVERFLOW            RC(116, 201)
#define TIME_INVALID_ARGUMENT    RC(116, 901)
#define TIME_ZONE_OUT_OF_RANGE   RC(116, 902)
#define TIME_ZONE_NOT_FOUND      RC(116, 989)
#define TIME_NS                  1000000000ll
#define TIME_INITIAL_YEAR        2023 /* set:sys's time!standard_user_clock_initial_year, as Eden has it */
#define TIME_AUTOMATIC_CORRECTION 1   /* the console's default: the clock is set from the network */

/* a time zone rule as this file writes it into the game's nn::time::TimeZoneRule */
typedef struct {
    char magic[4]; /* "SNXZ" */
    i32 utc_offset; /* seconds east of UTC */
    char abbreviation[8];
} TimeRule;

static struct {
    KShmem* shm;
    u8 source_id[16];           /* the steady clock's: the console makes a new one when its RTC resets */
    i64 base_ns;                /* the steady clock at the guest clock's start */
    nn_time_SystemClockContext context; /* every system clock's */
    TimeRule rule;              /* the device's */
    char location[0x24];
} tm;

/* ============================================================ the calendar */
/* days since 1970-01-01 of a proleptic Gregorian date, and back (an era of 400 years is 146097 days) */
static i64 days_from_civil(i64 y, int m, int d) {
    i64 era;
    i64 yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(i64 z, i64* y, int* m, int* d) {
    i64 era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}
static i64 floor_div(i64 a, i64 b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

/* a posix time in a rule's zone */
static int time_to_calendar(i64 posix, const TimeRule* r, nn_time_CalendarTime* cal,
                            nn_time_sf_CalendarAdditionalInfo* info) {
    i64 t = posix + r->utc_offset, days = floor_div(t, 86400), secs = t - days * 86400, y;
    int m, d;
    civil_from_days(days, &y, &m, &d);
    if (y < -32768 || y > 32767) return TIME_ZONE_OUT_OF_RANGE;
    cal->year = (i16)y;
    cal->month = (i8)m;
    cal->day = (i8)d;
    cal->hour = (i8)(secs / 3600);
    cal->minute = (i8)(secs / 60 % 60);
    cal->second = (i8)(secs % 60);
    info->day_of_week = (i32)(days - floor_div(days + 4, 7) * 7 + 4); /* 0 Sunday: 1970-01-01 was a Thursday */
    info->day_of_year = (i32)(days - days_from_civil(y, 1, 1));
    memcpy(info->time_zone_name, r->abbreviation, 8);
    info->is_dst = 0;
    info->utc_offset = r->utc_offset;
    return 0;
}
static int calendar_to_time(const nn_time_CalendarTime* cal, const TimeRule* r, i64* posix) {
    if (cal->month < 1 || cal->month > 12 || cal->day < 1 || cal->day > 31 || cal->hour < 0 || cal->hour > 23 ||
        cal->minute < 0 || cal->minute > 59 || cal->second < 0 || cal->second > 59)
        return TIME_INVALID_ARGUMENT;
    *posix = days_from_civil(cal->year, cal->month, cal->day) * 86400 + cal->hour * 3600 + cal->minute * 60 +
             cal->second - r->utc_offset;
    return 0;
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
        sprintf(out, "Etc/GMT%+d", 13 - i);
}
static void make_rule(i32 offset, TimeRule* r) {
    memset(r, 0, sizeof *r);
    memcpy(r->magic, "SNXZ", 4);
    r->utc_offset = offset;
    if (!offset)
        strcpy(r->abbreviation, "UTC");
    else if (offset % 3600)
        sprintf(r->abbreviation, "%+03d%02d", (int)(offset / 3600), (int)((offset < 0 ? -offset : offset) / 60 % 60));
    else
        sprintf(r->abbreviation, "%+03d", (int)(offset / 3600));
}
/* the rule of a zone by its name: the device's, UTC or Etc/GMT+N */
static int find_rule(const char* name, TimeRule* r) {
    int i;
    char z[0x24];
    if (!strncmp(name, tm.location, sizeof tm.location)) {
        *r = tm.rule;
        return 0;
    }
    if (!strcmp(name, "Etc/UTC") || !strcmp(name, "GMT") || !strcmp(name, "Etc/GMT") || !strcmp(name, "UTC")) {
        make_rule(0, r);
        return 0;
    }
    for (i = 1; i < TIME_ZONES; i++) {
        zone_name(i, z);
        if (!strcmp(name, z)) {
            make_rule((i32)(i - 13) * 3600, r);
            return 0;
        }
    }
    return TIME_ZONE_NOT_FOUND;
}
static int read_rule(IpcBuf b, TimeRule* r) {
    if (b.size < sizeof *r) return TIME_INVALID_ARGUMENT;
    memcpy(r, GP(b.addr), sizeof *r);
    return memcmp(r->magic, "SNXZ", 4) ? TIME_INVALID_ARGUMENT : 0;
}

/* ============================================================ the clocks */
static i64 steady_ns(void) { return tm.base_ns + (i64)guest_ns(); }
static void steady_point(nn_time_SteadyClockTimePoint* p) {
    p->value = steady_ns() / TIME_NS;
    memcpy(p->source_id.v, tm.source_id, 16);
}
/* a system clock's time from its context and the steady clock now, when they count from the same source */
static int context_time(const nn_time_SystemClockContext* c, const nn_time_SteadyClockTimePoint* now, i64* t) {
    if (memcmp(c->timestamp.source_id.v, now->source_id.v, 16)) return TIME_CLOCK_MISMATCH;
    *t = c->offset + now->value;
    return 0;
}

/* one of the shared memory's values the SDK reads without locks: a counter, and two copies the counter's parity
   picks between, at value bytes in */
static void shm_write(u32 at, u32 value, const void* v, u32 size) {
    u8* p = kshmem_ptr(tm.shm) + at;
    u32 counter;
    memcpy(&counter, p, 4);
    counter++;
    memcpy(p + value + (counter % 2) * size, v, size);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(p, &counter, 4);
}

void time_init(void) {
    time_t now = time(NULL);
    struct tm* lt = localtime(&now);
    nn_time_SteadyClockTimePoint start;
    u8 adjustment[0x38], automatic = TIME_AUTOMATIC_CORRECTION;
    i64 offset = 0, zero = 0;

    if (RAND_bytes(tm.source_id, 16) != 1) memcpy(tm.source_id, "ScarletNX steady", 16);
    tm.source_id[6] = (u8)(0x40 | (tm.source_id[6] & 0x0f)); /* a version 4 UUID */
    tm.source_id[8] = (u8)(0x80 | (tm.source_id[8] & 0x3f));
    tm.base_ns = (i64)g_t0_ns();

    steady_point(&start);
    tm.context.offset = (i64)now - start.value;
    tm.context.timestamp = start;

    /* the host's offset from UTC now, in minutes, as a fixed one */
    if (lt)
        offset = days_from_civil(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday) * 86400 + lt->tm_hour * 3600 +
                 lt->tm_min * 60 + lt->tm_sec - (i64)now;
    offset = (offset + (offset < 0 ? -30 : 30)) / 60 * 60;
    make_rule((i32)offset, &tm.rule);
    /* its name, for whole hours; the others have none here, and are called UTC with their own offset */
    zone_name(offset && !(offset % 3600) && offset >= -12 * 3600 && offset <= 14 * 3600 ? (int)(offset / 3600) + 13 : 0,
              tm.location);

    /* the steady clock's base (ns), the system clock contexts, automatic correction, and the continuous adjustment
       newer SDKs compute the steady clock from (Eden's StandardSteadyClockCore: no drift) */
    tm.shm = kshmem_new(0x1000, "time");
    kobj_ref(&tm.shm->o);
    {
        nn_time_SteadyClockTimePoint base;
        base.value = tm.base_ns;
        memcpy(base.source_id.v, tm.source_id, 16);
        shm_write(0x00, 8, &base, sizeof base);
    }
    shm_write(0x38, 8, &tm.context, sizeof tm.context);
    shm_write(0x80, 8, &tm.context, sizeof tm.context);
    shm_write(0xC8, 4, &automatic, 1);
    memset(adjustment, 0, sizeof adjustment);
    offset = (i64)guest_ns();
    memcpy(adjustment, &offset, 8); /* the uptime it was set at */
    memcpy(adjustment + 8, &zero, 8); /* no drift: scale 0, shift 0 */
    memcpy(adjustment + 0x18, &tm.base_ns, 8); /* the lower and upper bounds */
    memcpy(adjustment + 0x20, &tm.base_ns, 8);
    memcpy(adjustment + 0x28, tm.source_id, 16);
    shm_write(0xD0, 8, adjustment, sizeof adjustment);
    LOG("time: the time zone is %s (UTC%s)\n", tm.location, tm.rule.abbreviation);
}

/* ============================================================ ISystemClock */
static int nn_timesrv_detail_service_ISystemClock_GetCurrentTime(SvcObj* self, nn_time_PosixTime* time) {
    nn_time_SteadyClockTimePoint now;
    (void)self;
    steady_point(&now);
    return context_time(&tm.context, &now, time);
}
static int nn_timesrv_detail_service_ISystemClock_GetSystemClockContext(SvcObj* self,
                                                                        nn_time_SystemClockContext* context) {
    (void)self;
    *context = tm.context;
    return 0;
}
/* the clock is never set: the event never signals */
static int nn_timesrv_detail_service_ISystemClock_GetOperationEventReadableHandle(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "time clock operation", 0);
}

/* ============================================================ ISteadyClock */
static int nn_timesrv_detail_service_ISteadyClock_GetCurrentTimePoint(SvcObj* self,
                                                                      nn_time_SteadyClockTimePoint* time_point) {
    (void)self;
    steady_point(time_point);
    return 0;
}

/* ============================================================ ITimeZoneService */
static int nn_timesrv_detail_service_ITimeZoneService_GetDeviceLocationName(SvcObj* self, nn_time_LocationName* name) {
    (void)self;
    memcpy(name->v, tm.location, sizeof name->v);
    return 0;
}
static int nn_timesrv_detail_service_ITimeZoneService_GetTotalLocationNameCount(SvcObj* self, i32* count) {
    (void)self;
    *count = TIME_ZONES;
    return 0;
}
static int nn_timesrv_detail_service_ITimeZoneService_LoadLocationNameList(SvcObj* self, i32 index, IpcBuf names,
                                                                          i32* count) {
    i32 n = 0;
    (void)self;
    if (index < 0) return TIME_INVALID_ARGUMENT;
    while (index + n < TIME_ZONES && (u64)(n + 1) * 0x24 <= names.size) {
        char z[0x24];
        zone_name(index + n, z);
        memcpy(GP(names.addr + (u64)n * 0x24), z, 0x24);
        n++;
    }
    *count = n;
    return 0;
}
static int nn_timesrv_detail_service_ITimeZoneService_LoadTimeZoneRule(SvcObj* self, const nn_time_LocationName* name,
                                                                      IpcBuf rule) {
    char z[0x25];
    TimeRule r;
    int rc;
    (void)self;
    memcpy(z, name->v, 0x24);
    z[0x24] = 0;
    rc = find_rule(z, &r);
    if (rc) return rc;
    if (rule.size < sizeof r) return TIME_INVALID_ARGUMENT;
    memset(GP(rule.addr), 0, (size_t)rule.size);
    memcpy(GP(rule.addr), &r, sizeof r);
    return 0;
}
static int nn_timesrv_detail_service_ITimeZoneService_GetTimeZoneRuleVersion(SvcObj* self,
                                                                            nn_time_TimeZoneRuleVersion* version) {
    (void)self;
    memcpy(version->v, "2024a", 6);
    return 0;
}
static int nn_timesrv_detail_service_ITimeZoneService_GetDeviceLocationNameOperationEventReadableHandle(SvcObj* self,
                                                                                                      u32* event) {
    (void)self;
    return new_event(event, "time zone operation", 0);
}
static int nn_timesrv_detail_service_ITimeZoneService_ToCalendarTime(SvcObj* self, nn_time_PosixTime time, IpcBuf rule,
                                                                    nn_time_CalendarTime* calendar,
                                                                    nn_time_sf_CalendarAdditionalInfo* info) {
    TimeRule r;
    int rc = read_rule(rule, &r);
    (void)self;
    return rc ? rc : time_to_calendar(time, &r, calendar, info);
}
static int nn_timesrv_detail_service_ITimeZoneService_ToCalendarTimeWithMyRule(
    SvcObj* self, nn_time_PosixTime time, nn_time_CalendarTime* calendar, nn_time_sf_CalendarAdditionalInfo* info) {
    (void)self;
    return time_to_calendar(time, &tm.rule, calendar, info);
}
/* a local time is one posix time in a zone without daylight saving */
static int to_posix(const nn_time_CalendarTime* calendar, const TimeRule* r, IpcBuf times, i32* count) {
    i64 t;
    int rc = calendar_to_time(calendar, r, &t);
    if (rc) return rc;
    if (times.size < 8) return TIME_INVALID_ARGUMENT;
    memcpy(GP(times.addr), &t, 8);
    *count = 1;
    return 0;
}
static int nn_timesrv_detail_service_ITimeZoneService_ToPosixTime(SvcObj* self, const nn_time_CalendarTime* calendar,
                                                                 IpcBuf rule, IpcBuf times, i32* count) {
    TimeRule r;
    int rc = read_rule(rule, &r);
    (void)self;
    return rc ? rc : to_posix(calendar, &r, times, count);
}
static int nn_timesrv_detail_service_ITimeZoneService_ToPosixTimeWithMyRule(SvcObj* self,
                                                                           const nn_time_CalendarTime* calendar,
                                                                           IpcBuf times, i32* count) {
    (void)self;
    return to_posix(calendar, &tm.rule, times, count);
}

/* ============================================================ IStaticService */
/* the user's, network, local and ephemeral network clocks are one context here */
static SvcObj* new_clock(void) { return nn_timesrv_detail_service_ISystemClock_new(0); }
static int nn_timesrv_detail_service_IStaticService_GetStandardUserSystemClock(SvcObj* self, SvcObj** clock) {
    (void)self;
    *clock = new_clock();
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetStandardNetworkSystemClock(SvcObj* self, SvcObj** clock) {
    (void)self;
    *clock = new_clock();
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetStandardSteadyClock(SvcObj* self, SvcObj** clock) {
    (void)self;
    *clock = nn_timesrv_detail_service_ISteadyClock_new(0);
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetTimeZoneService(SvcObj* self, SvcObj** service) {
    (void)self;
    *service = nn_timesrv_detail_service_ITimeZoneService_new(0);
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetStandardLocalSystemClock(SvcObj* self, SvcObj** clock) {
    (void)self;
    *clock = new_clock();
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetEphemeralNetworkSystemClock(SvcObj* self, SvcObj** clock) {
    (void)self;
    *clock = new_clock();
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetSharedMemoryNativeHandle(SvcObj* self, u32* shared_memory) {
    (void)self;
    *shared_memory = h_new(&tm.shm->o);
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetStandardNetworkClockOperationEventReadableHandle(SvcObj* self,
                                                                                                       u32* event) {
    (void)self;
    return new_event(event, "time network clock operation", 0);
}
static int nn_timesrv_detail_service_IStaticService_GetEphemeralNetworkClockOperationEventReadableHandle(SvcObj* self,
                                                                                                        u32* event) {
    (void)self;
    return new_event(event, "time ephemeral clock operation", 0);
}
static int nn_timesrv_detail_service_IStaticService_IsStandardUserSystemClockAutomaticCorrectionEnabled(SvcObj* self,
                                                                                                       u8* enabled) {
    (void)self;
    *enabled = TIME_AUTOMATIC_CORRECTION;
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetStandardUserSystemClockInitialYear(SvcObj* self, i32* year) {
    (void)self;
    *year = TIME_INITIAL_YEAR;
    return 0;
}
/* the network clock was set when the runtime started, well within the accuracy the console asks of it (30 days) */
static int nn_timesrv_detail_service_IStaticService_IsStandardNetworkSystemClockAccuracySufficient(SvcObj* self,
                                                                                                  u8* sufficient) {
    (void)self;
    *sufficient = 1;
    return 0;
}
/* the context's time when the steady clock started counting */
static int nn_timesrv_detail_service_IStaticService_CalculateMonotonicSystemClockBaseTimePoint(
    SvcObj* self, const nn_time_SystemClockContext* context, i64* time) {
    nn_time_SteadyClockTimePoint now;
    (void)self;
    steady_point(&now);
    if (memcmp(now.source_id.v, context->timestamp.source_id.v, 16)) return TIME_CLOCK_MISMATCH;
    *time = context->offset + now.value - (i64)(guest_ns() / TIME_NS);
    return 0;
}

/* nn::time::sf::ClockSnapshot's fields (time.swipc) */
#define SNAP_USER_CONTEXT    0x00
#define SNAP_NETWORK_CONTEXT 0x20
#define SNAP_USER_TIME       0x40
#define SNAP_NETWORK_TIME    0x48
#define SNAP_USER_CALENDAR   0x50
#define SNAP_NETWORK_CALENDAR 0x58
#define SNAP_USER_INFO       0x60
#define SNAP_NETWORK_INFO    0x78
#define SNAP_STEADY          0x90
#define SNAP_LOCATION        0xA8
#define SNAP_AUTOMATIC       0xCC
#define SNAP_TYPE            0xCD

static int snapshot(u8 type, const nn_time_SystemClockContext* user, const nn_time_SystemClockContext* network,
                    IpcBuf out) {
    u8 s[0xd0];
    nn_time_SteadyClockTimePoint now;
    nn_time_CalendarTime cal;
    nn_time_sf_CalendarAdditionalInfo info;
    i64 user_time, network_time;
    int rc;
    memset(s, 0, sizeof s);
    memcpy(s + SNAP_USER_CONTEXT, user, sizeof *user);
    memcpy(s + SNAP_NETWORK_CONTEXT, network, sizeof *network);
    steady_point(&now);
    memcpy(s + SNAP_STEADY, &now, sizeof now);
    s[SNAP_AUTOMATIC] = TIME_AUTOMATIC_CORRECTION;
    memcpy(s + SNAP_LOCATION, tm.location, sizeof tm.location);
    rc = context_time(user, &now, &user_time);
    if (rc) return rc;
    memcpy(s + SNAP_USER_TIME, &user_time, 8);
    memset(&cal, 0, sizeof cal);
    memset(&info, 0, sizeof info);
    rc = time_to_calendar(user_time, &tm.rule, &cal, &info);
    if (rc) return rc;
    memcpy(s + SNAP_USER_CALENDAR, &cal, sizeof cal);
    memcpy(s + SNAP_USER_INFO, &info, sizeof info);
    if (context_time(network, &now, &network_time)) network_time = 0;
    memcpy(s + SNAP_NETWORK_TIME, &network_time, 8);
    memset(&cal, 0, sizeof cal);
    memset(&info, 0, sizeof info);
    rc = time_to_calendar(network_time, &tm.rule, &cal, &info);
    if (rc) return rc;
    memcpy(s + SNAP_NETWORK_CALENDAR, &cal, sizeof cal);
    memcpy(s + SNAP_NETWORK_INFO, &info, sizeof info);
    s[SNAP_TYPE] = type;
    memcpy(GP(out.addr), s, (size_t)(out.size < sizeof s ? out.size : sizeof s));
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetClockSnapshot(SvcObj* self, u8 type, IpcBuf out) {
    (void)self;
    return snapshot(type, &tm.context, &tm.context, out);
}
static int nn_timesrv_detail_service_IStaticService_GetClockSnapshotFromSystemClockContext(
    SvcObj* self, u8 type, const nn_time_SystemClockContext* user_context,
    const nn_time_SystemClockContext* network_context, IpcBuf out) {
    (void)self;
    return snapshot(type, user_context, network_context, out);
}

static void read_snapshot(IpcBuf b, u8 s[0xd0]) {
    memset(s, 0, 0xd0);
    memcpy(s, GP(b.addr), (size_t)(b.size < 0xd0 ? b.size : 0xd0));
}
static int same_source(const u8* a, const u8* b) { return !memcmp(a + 8, b + 8, 16); } /* two time points' ids */

/* how far the user moved the user clock between two snapshots: none when the network corrected it */
static int nn_timesrv_detail_service_IStaticService_CalculateStandardUserSystemClockDifferenceByUser(
    SvcObj* self, IpcBuf a, IpcBuf b, nn_TimeSpanType* difference) {
    u8 sa[0xd0], sb[0xd0];
    i64 oa, ob;
    (void)self;
    read_snapshot(a, sa);
    read_snapshot(b, sb);
    memcpy(&oa, sa + SNAP_USER_CONTEXT, 8);
    memcpy(&ob, sb + SNAP_USER_CONTEXT, 8);
    if (!memcmp(sa + SNAP_USER_CONTEXT, sb + SNAP_USER_CONTEXT, 0x20) ||
        !same_source(sa + SNAP_USER_CONTEXT + 8, sb + SNAP_USER_CONTEXT + 8))
        *difference = 0;
    else if (!sa[SNAP_AUTOMATIC] || !sb[SNAP_AUTOMATIC])
        *difference = (ob - oa) * TIME_NS;
    else if (same_source(sa + SNAP_NETWORK_CONTEXT + 8, sa + SNAP_STEADY) ||
             same_source(sb + SNAP_NETWORK_CONTEXT + 8, sb + SNAP_STEADY))
        *difference = 0;
    else
        *difference = (ob - oa) * TIME_NS;
    return 0;
}
/* the time between two snapshots, by the steady clock when it counted both, else by the network clock */
static int nn_timesrv_detail_service_IStaticService_CalculateSpanBetween(SvcObj* self, IpcBuf a, IpcBuf b,
                                                                       nn_TimeSpanType* span) {
    u8 sa[0xd0], sb[0xd0];
    i64 ta, tb, s;
    (void)self;
    read_snapshot(a, sa);
    read_snapshot(b, sb);
    if (same_source(sa + SNAP_STEADY, sb + SNAP_STEADY)) {
        memcpy(&ta, sa + SNAP_STEADY, 8);
        memcpy(&tb, sb + SNAP_STEADY, 8);
        if ((tb > 0 && ta < tb - 0x7fffffffffffffffll) || (tb < 0 && ta > tb + 0x7fffffffffffffffll))
            return TIME_OVERFLOW;
    } else {
        memcpy(&ta, sa + SNAP_NETWORK_TIME, 8);
        memcpy(&tb, sb + SNAP_NETWORK_TIME, 8);
        if (!ta || !tb) return TIME_NOT_FOUND;
    }
    s = tb - ta;
    if (s > 0x7fffffffffffffffll / TIME_NS || s < -0x7fffffffffffffffll / TIME_NS) return TIME_OVERFLOW;
    *span = s * TIME_NS;
    return 0;
}

SvcObj* new_time(void) { return nn_timesrv_detail_service_IStaticService_new(0); }
