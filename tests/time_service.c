/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* time.c's clocks and time zones, its handlers called directly: the system clock against the host's time, calendar
   times in a fixed-offset zone and back, the zone names, the shared memory the SDK reads, and the spans between
   snapshots. The kernel objects and the guest clock are stand-ins here */
#include "nx/hle/services/time.c"
#include "nx/hle/hle.c"

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

u8* g_mem;
static u64 test_ns = 5000000000ull; /* the guest clock: 5 s in */
u64 guest_ns(void) { return test_ns; }
u64 g_t0_ns(void) { return 1000000000000ull; }
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
void kobj_ref(KObj* o) { o->refs++; }
static u8 shm_host[0x1000];
static KShmem shm_obj;
KShmem* kshmem_new(u64 size, const char* name) {
    CHECK(size == sizeof shm_host);
    kobj_set_name(&shm_obj.o, name);
    shm_obj.o.type = KO_SHMEM;
    shm_obj.size = size;
    shm_obj.host = shm_host;
    return &shm_obj;
}
u8* kshmem_ptr(KShmem* s) { return s->host; }
u32 h_new(KObj* o) {
    (void)o;
    return 0x100;
}
int new_event(u32* event, const char* name, int signaled) {
    (void)name, (void)signaled;
    *event = 0x101;
    return 0;
}
/* what the dispatchers reach, which the test does not run */
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    SvcObj* o = calloc(1, sizeof *o);
    CHECK(o != NULL);
    o->name = name;
    o->dispatch = dispatch;
    o->refs = 1;
    return o;
}
#include "svcobj_data.h"
#include "kobj_ids.h" /* the kernel objects the stubs make, as ids */
static void test_free(SvcObj* o) {
    test_svcobj_data_free(o);
    free(o);
}

/* time:u, an application's, may write no clock, setting or location, nor may the clocks and time zone service it hands
   out; time:a and time:s take each write, which changes nothing here */
static void test_permissions(void) {
    SvcObj* sessions[2];
    SvcObj *user, *steady, *zones, *network;
    nn_time_SystemClockContext context;
    nn_time_LocationName name;
    int i, expected;
    memset(&context, 0, sizeof context);
    memset(&name, 0, sizeof name);
    sessions[0] = new_time_user();
    sessions[1] = new_time();
    for (i = 0; i < 2; i++) {
        expected = i ? 0 : (int)TIME_PERMISSION_DENIED;
        CHECK(nn_timesrv_detail_service_IStaticService_GetStandardUserSystemClock(sessions[i], &user) == 0);
        CHECK(nn_timesrv_detail_service_IStaticService_GetStandardNetworkSystemClock(sessions[i], &network) == 0);
        CHECK(nn_timesrv_detail_service_IStaticService_GetStandardSteadyClock(sessions[i], &steady) == 0);
        CHECK(nn_timesrv_detail_service_IStaticService_GetTimeZoneService(sessions[i], &zones) == 0);
        CHECK(nn_timesrv_detail_service_ISystemClock_SetCurrentTime(user, 0) == expected);
        CHECK(nn_timesrv_detail_service_ISystemClock_SetSystemClockContext(user, &context) == expected);
        CHECK(nn_timesrv_detail_service_ISystemClock_SetCurrentTime(network, 0) == expected);
        CHECK(nn_timesrv_detail_service_ISteadyClock_SetTestOffset(steady, 0) == expected);
        CHECK(nn_timesrv_detail_service_ITimeZoneService_SetDeviceLocationName(zones, &name) == expected);
        CHECK(nn_timesrv_detail_service_IStaticService_SetStandardSteadyClockInternalOffset(sessions[i], 0) ==
              expected);
        CHECK(nn_timesrv_detail_service_IStaticService_SetStandardUserSystemClockAutomaticCorrectionEnabled(
                  sessions[i], 1) == expected);
        test_free(user);
        test_free(network);
        test_free(steady);
        test_free(zones);
        test_free(sessions[i]);
    }
}
void out_obj(IpcCtx* c, SvcObj* o) { (void)c, (void)o; }
void out_copy_h(IpcCtx* c, u32 h) { (void)c, (void)h; }
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 0;
}
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 0;
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 0;
}
void svc_register(const char* name, SvcCtor ctor) { (void)name, (void)ctor; }

static IpcBuf buf(u64 addr, u64 size) {
    IpcBuf b;
    b.addr = addr;
    b.size = size;
    b.flags = 0;
    return b;
}
static nn_time_CalendarTime calendar(int y, int mo, int d, int h, int mi, int s) {
    nn_time_CalendarTime c;
    memset(&c, 0, sizeof c);
    c.year = (i16)y;
    c.month = (i8)mo;
    c.day = (i8)d;
    c.hour = (i8)h;
    c.minute = (i8)mi;
    c.second = (i8)s;
    return c;
}
static i64 rd64(const u8* p) { return (i64)snx_ld_le64(p); }

void pm_init(PMutex* m) { (void)m; }

int main(void) {
    hle_init();
    nn_time_PosixTime now;
    nn_time_SystemClockContext ctx;
    nn_time_SteadyClockTimePoint tp;
    nn_time_LocationName name;
    nn_time_CalendarTime cal;
    nn_time_sf_CalendarAdditionalInfo info;
    i32 count;
    i64 t, span;
    u32 counter;
    u8 ctx_le[0x20];
    time_t host;
    char z[0x25];

    g_mem = calloc(1, 0x40000);
    CHECK(g_mem != NULL);
    time_init();

    /* the system clock is the host's time, counting with the steady clock */
    host = time(NULL);
    CHECK(nn_timesrv_detail_service_ISystemClock_GetCurrentTime(NULL, &now) == 0);
    CHECK(now >= (i64)host - 1 && now <= (i64)host + 1);
    test_ns += 10ull * 1000000000ull;
    CHECK(nn_timesrv_detail_service_ISystemClock_GetCurrentTime(NULL, &t) == 0 && t == now + 10);
    CHECK(nn_timesrv_detail_service_ISystemClock_GetSystemClockContext(NULL, &ctx) == 0);
    CHECK(nn_timesrv_detail_service_ISteadyClock_GetCurrentTimePoint(NULL, &tp) == 0);
    CHECK(tp.value == (1000000000000ll + 15000000000ll) / 1000000000ll);
    CHECK(!memcmp(tp.source_id.v, ctx.timestamp.source_id.v, 16) && (tp.source_id.v[6] >> 4) == 4);
    CHECK(ctx.offset + tp.value == t);

    /* the shared memory: the steady clock's base in ns and the contexts, each in the copy its counter picks */
    counter = snx_ld_le32(shm_host);
    CHECK(counter == 1 && rd64(shm_host + 8 + 0x18) == 1000000000000ll);
    CHECK(!memcmp(shm_host + 8 + 0x18 + 8, tp.source_id.v, 16));
    counter = snx_ld_le32(shm_host + 0x38);
    put_context(ctx_le, &ctx); /* the context as the game reads it */
    CHECK(counter == 1 && !memcmp(shm_host + 0x40 + 0x20, ctx_le, sizeof ctx_le));
    CHECK(!memcmp(shm_host + 0x88 + 0x20, ctx_le, sizeof ctx_le));
    CHECK(shm_host[0xCC + 1] == 1);

    /* zones: UTC, then Etc/GMT+12 (UTC-12) to Etc/GMT-14 (UTC+14) */
    CHECK(nn_timesrv_detail_service_ITimeZoneService_GetTotalLocationNameCount(NULL, &count) == 0 && count == 28);
    CHECK(nn_timesrv_detail_service_ITimeZoneService_LoadLocationNameList(NULL, 0, buf(0x1000, 3 * 0x24), &count) ==
          0);
    CHECK(count == 3 && !strcmp((char*)g_mem + 0x1000, "UTC") && !strcmp((char*)g_mem + 0x1024, "Etc/GMT+12") &&
          !strcmp((char*)g_mem + 0x1048, "Etc/GMT+11"));
    CHECK(nn_timesrv_detail_service_ITimeZoneService_LoadLocationNameList(NULL, 26, buf(0x1000, 4 * 0x24), &count) ==
              0 &&
          count == 2 && !strcmp((char*)g_mem + 0x1024, "Etc/GMT-14"));
    CHECK(nn_timesrv_detail_service_ITimeZoneService_GetDeviceLocationName(NULL, &name) == 0);
    memcpy(z, name.v, 0x24);
    z[0x24] = 0;
    CHECK(!strncmp(z, "UTC", 3) || !strncmp(z, "Etc/GMT", 7));
    /* the same location, set at the steady clock time of the user clock's context */
    memset(&name, 0, sizeof name);
    memset(&tp, 0, sizeof tp);
    CHECK(nn_timesrv_detail_service_ITimeZoneService_GetDeviceLocationNameAndUpdatedTime(NULL, &name, &tp) == 0);
    CHECK(!memcmp(name.v, z, 0x24) && tp.value == ctx.timestamp.value &&
          !memcmp(tp.source_id.v, ctx.timestamp.source_id.v, 16));

    /* Tokyo's offset: 2024-02-29 15:04:05 UTC is 2024-03-01 00:04:05 there, a Friday, day 60 of a leap year */
    memset(&name, 0, sizeof name);
    strcpy((char*)name.v, "Etc/GMT-9");
    CHECK(nn_timesrv_detail_service_ITimeZoneService_LoadTimeZoneRule(NULL, &name, buf(0x10000, 0x4000)) == 0);
    CHECK(nn_timesrv_detail_service_ITimeZoneService_ToCalendarTime(NULL, 1709219045, buf(0x10000, 0x4000), &cal,
                                                                     &info) == 0);
    CHECK(cal.year == 2024 && cal.month == 3 && cal.day == 1 && cal.hour == 0 && cal.minute == 4 && cal.second == 5);
    CHECK(info.day_of_week == 5 && info.day_of_year == 60 && info.utc_offset == 9 * 3600 &&
          !strcmp((char*)info.time_zone_name, "+09"));
    cal = calendar(2024, 3, 1, 0, 4, 5);
    CHECK(nn_timesrv_detail_service_ITimeZoneService_ToPosixTime(NULL, &cal, buf(0x10000, 0x4000), buf(0x20000, 80),
                                                                  &count) == 0);
    CHECK(count == 1 && rd64(g_mem + 0x20000) == 1709219045);
    /* before 1970, west of UTC: 1969-12-31 23:59:59 UTC is 19:59:59 in UTC-4, a Wednesday */
    strcpy((char*)name.v, "Etc/GMT+4");
    CHECK(nn_timesrv_detail_service_ITimeZoneService_LoadTimeZoneRule(NULL, &name, buf(0x10000, 0x4000)) == 0);
    CHECK(nn_timesrv_detail_service_ITimeZoneService_ToCalendarTime(NULL, -1, buf(0x10000, 0x4000), &cal, &info) ==
          0);
    CHECK(cal.year == 1969 && cal.month == 12 && cal.day == 31 && cal.hour == 19 && cal.second == 59);
    CHECK(info.day_of_week == 3 && info.day_of_year == 364 && info.utc_offset == -4 * 3600);
    strcpy((char*)name.v, "Mars/Olympus_Mons");
    CHECK(nn_timesrv_detail_service_ITimeZoneService_LoadTimeZoneRule(NULL, &name, buf(0x10000, 0x4000)) ==
          (int)TIME_ZONE_NOT_FOUND);
    CHECK(nn_timesrv_detail_service_ITimeZoneService_ToCalendarTime(NULL, 0, buf(0x30000, 0x4000), &cal, &info) ==
          (int)TIME_INVALID_ARGUMENT); /* not a rule this service wrote */

    /* snapshots: their user time and calendar, and the span between two by the steady clock */
    CHECK(nn_timesrv_detail_service_IStaticService_GetClockSnapshot(NULL, 0, buf(0x2000, 0xd0)) == 0);
    test_ns += 90ull * 1000000000ull;
    CHECK(nn_timesrv_detail_service_IStaticService_GetClockSnapshot(NULL, 0, buf(0x2100, 0xd0)) == 0);
    CHECK(rd64(g_mem + 0x2000 + SNAP_USER_TIME) == t && rd64(g_mem + 0x2100 + SNAP_USER_TIME) == t + 90);
    CHECK(g_mem[0x2000 + SNAP_AUTOMATIC] == 1 && !memcmp(g_mem + 0x2000 + SNAP_LOCATION, z, 0x24));
    CHECK(nn_timesrv_detail_service_IStaticService_CalculateSpanBetween(NULL, buf(0x2000, 0xd0), buf(0x2100, 0xd0),
                                                                         &span) == 0);
    CHECK(span == 90ll * 1000000000ll);
    CHECK(nn_timesrv_detail_service_IStaticService_CalculateStandardUserSystemClockDifferenceByUser(
              NULL, buf(0x2000, 0xd0), buf(0x2100, 0xd0), &span) == 0 &&
          span == 0);
    CHECK(nn_timesrv_detail_service_IStaticService_CalculateMonotonicSystemClockBaseTimePoint(NULL, &ctx, &t) == 0);
    CHECK(t == ctx.offset + (1000000000000ll + (i64)test_ns) / 1000000000ll - (i64)(test_ns / 1000000000ull));

    /* what the 12.1.0 firmware set:sys reports has: its time zone data's version, its initial year */
    {
        nn_time_TimeZoneRuleVersion version;
        i32 year;
        CHECK(nn_timesrv_detail_service_ITimeZoneService_GetTimeZoneRuleVersion(NULL, &version) == 0 &&
              !strcmp((char*)version.v, "2021a"));
        CHECK(nn_timesrv_detail_service_IStaticService_GetStandardUserSystemClockInitialYear(NULL, &year) == 0 &&
              year == 2019);
    }

    test_permissions();
    free(g_mem);
    printf("time_service: %u checks passed\n", checks);
    return 0;
}
