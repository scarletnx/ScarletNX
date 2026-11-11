/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native nn::time (src/nx/nn/time.c), its handlers called as the hooks call them, on a stand-in CPU over a guest
   buffer: Initialize's count and mode, the results before Initialize, the clocks against the host's time, calendar
   times in a fixed-offset zone, the snapshots and the spans between them, and the calls the SDK stops the game on.
   The guest clock, the lock and the log are stand-ins here */
#include "nx/nn/time.c"
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

Hle g_hle;
static u64 test_ns = 5000000000ull; /* the guest clock: 5 s in */
u64 guest_ns(void) { return test_ns; }
u64 g_t0_ns(void) { return 1000000000000ull; }
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }

/* fatal: the game stops, which a check expects */
static jmp_buf stopped;
static int expect_stop;
void fatal(const char* fmt, ...) {
    va_list a;
    if (!expect_stop) {
        va_start(a, fmt);
        vfprintf(stderr, fmt, a);
        va_end(a);
        fputc('\n', stderr);
        exit(1);
    }
    longjmp(stopped, 1);
}
#define CHECK_STOPS(call)                                                                                              \
    do {                                                                                                               \
        expect_stop = 1;                                                                                               \
        int stopped_ = setjmp(stopped);                                                                                \
        if (!stopped_) {                                                                                               \
            call;                                                                                                      \
        }                                                                                                              \
        expect_stop = 0;                                                                                               \
        CHECK(stopped_ == 1);                                                                                          \
    } while (0)

static CPU cpu;
static u8 mem[0x40000];
static i64 rd64(u64 a) { return (i64)snx_ld_le64(mem + a); }

int main(void) {
    cpu.mem = mem;
    CPU* c = &cpu;
    nntime_init();

    /* before Initialize: the clocks answer 0x74 (or 0) and leave their output, the others stop the game */
    snx_st_le64(mem + 0x100, 77);
    CHECK(!nntime_is_initialized(c));
    CHECK(nntime_user_get_current_time(c, 0x100) == 0x74 && rd64(0x100) == 77);
    CHECK(nntime_network_get_current_time(c, 0x100) == 0x74 && nntime_user_now(c) == 0 && nntime_steady_now(c) == 0);
    CHECK(nntime_steady_get_current_time_point(c, 0x100) == 0x74 && nntime_user_get_context(c, 0x100) == 0x74);
    CHECK(nntime_load_time_zone_rule(c, 0x10000, 0x200) == 0x74 && nntime_to_calendar_time(c, 0x300, 0, 0x100) == 0x74);
    CHECK_STOPS(nntime_get_device_location_name(c, 0x200));
    CHECK_STOPS(nntime_snapshot_create_standard(c, 0x2000));
    CHECK_STOPS(nntime_span_between(c, 0x100, 0x2000, 0x2100));
    CHECK_STOPS(nntime_user_difference(c, 0x2000, 0x2100));

    /* Initialize counts, Finalize to zero ends it, and a later Initialize of another mode stops the game */
    CHECK(nntime_initialize(c) == 0 && nntime_initialize(c) == 0 && nntime_is_initialized(c));
    CHECK_STOPS(nntime_initialize_for_menu(c));
    CHECK(nntime_finalize(c) == 0 && nntime_is_initialized(c));
    CHECK(nntime_finalize(c) == 0 && !nntime_is_initialized(c) && nntime_finalize(c) == 0);
    CHECK(nntime_initialize_for_menu(c) == 0 && nntime_finalize(c) == 0);
    CHECK(nntime_initialize(c) == 0);

    /* the user and network clocks are the host's time, counting with the steady clock */
    i64 host = (i64)time(NULL);
    CHECK(nntime_user_get_current_time(c, 0x100) == 0);
    i64 now = rd64(0x100);
    CHECK(now >= host - 1 && now <= host + 1 && nntime_user_now(c) == now && nntime_network_now(c) == now);
    test_ns += 10ull * 1000000000ull;
    CHECK(nntime_network_get_current_time(c, 0x100) == 0 && rd64(0x100) == now + 10);
    CHECK(nntime_user_get_context(c, 0x400) == 0 && nntime_network_get_context(c, 0x420) == 0);
    CHECK(!memcmp(mem + 0x400, mem + 0x420, 0x20));
    CHECK(nntime_steady_get_current_time_point(c, 0x440) == 0);
    CHECK(rd64(0x440) == (1000000000000ll + 15000000000ll) / 1000000000ll && nntime_steady_now(c) == rd64(0x440));
    CHECK(!memcmp(mem + 0x448, mem + 0x410, 16) && (mem[0x448 + 6] >> 4) == 4); /* one source, a v4 UUID */
    CHECK(rd64(0x400) + rd64(0x440) == now + 10);

    /* the device's zone: UTC or a whole hour's Etc/GMT */
    nntime_get_device_location_name(c, 0x200);
    CHECK(!strncmp((char*)mem + 0x200, "UTC", 3) || !strncmp((char*)mem + 0x200, "Etc/GMT", 7));
    CHECK_STOPS(nntime_get_device_location_name(c, 0));

    /* Tokyo's offset: 2024-02-29 15:04:05 UTC is 2024-03-01 00:04:05 there, a Friday, day 60 of a leap year */
    memset(mem + 0x500, 0, 0x24);
    strcpy((char*)mem + 0x500, "Etc/GMT-9");
    CHECK(nntime_load_time_zone_rule(c, 0x10000, 0x500) == 0);
    snx_st_le64(mem + 0x100, 1709219045);
    memset(mem + 0x600, 0xee, 0x20);
    CHECK(nntime_to_calendar_time_rule(c, 0x300, 0x600, 0x100, 0x10000) == 0);
    CHECK(snx_ld_le16(mem + 0x300) == 2024 && mem[0x302] == 3 && mem[0x303] == 1 && mem[0x304] == 0 &&
          mem[0x305] == 4 && mem[0x306] == 5 && mem[0x307] == 0);
    CHECK(snx_ld_le32(mem + 0x600) == 5 && snx_ld_le32(mem + 0x604) == 60 && !strcmp((char*)mem + 0x608, "+09"));
    CHECK(snx_ld_le32(mem + 0x610) == 0 && (i32)snx_ld_le32(mem + 0x614) == 9 * 3600 && mem[0x618] == 0xee);
    /* no CalendarAdditionalInfo asked for: none written */
    memset(mem + 0x600, 0xee, 0x18);
    CHECK(nntime_to_calendar_time_rule(c, 0x300, 0, 0x100, 0x10000) == 0 && mem[0x600] == 0xee);
    /* before 1970, west of UTC: 1969-12-31 23:59:59 UTC is 19:59:59 in UTC-4, a Wednesday */
    strcpy((char*)mem + 0x500, "Etc/GMT+4");
    CHECK(nntime_load_time_zone_rule(c, 0x10000, 0x500) == 0);
    snx_st_le64(mem + 0x100, (u64)-1);
    CHECK(nntime_to_calendar_time_rule(c, 0x300, 0x600, 0x100, 0x10000) == 0);
    CHECK(snx_ld_le16(mem + 0x300) == 1969 && mem[0x302] == 12 && mem[0x303] == 31 && mem[0x304] == 19);
    CHECK(snx_ld_le32(mem + 0x600) == 3 && snx_ld_le32(mem + 0x604) == 364);
    strcpy((char*)mem + 0x500, "Mars/Olympus_Mons");
    CHECK(nntime_load_time_zone_rule(c, 0x10000, 0x500) == SNX_RC_TIME_NOT_FOUND);
    CHECK(nntime_to_calendar_time_rule(c, 0x300, 0x600, 0x100, 0x30000) == SNX_RC_TIME_INVALID_POINTER);
    CHECK(nntime_to_calendar_time(c, 0x300, 0x600, 0x100) == 0); /* the device's rule */

    /* snapshots: their user time and calendar, and the span between two by the steady clock */
    i64 t = nntime_user_now(c);
    nntime_snapshot_create_standard(c, 0x2000);
    test_ns += 90ull * 1000000000ull;
    nntime_snapshot_create_standard(c, 0x2100);
    CHECK(rd64(0x2000 + SNAP_USER_TIME) == t && rd64(0x2100 + SNAP_USER_TIME) == t + 90);
    CHECK(mem[0x2000 + SNAP_AUTOMATIC] == 1 && mem[0x2000 + SNAP_TYPE] == 1);
    CHECK(!memcmp(mem + 0x2000 + SNAP_LOCATION, mem + 0x200, 0x24));
    CHECK(nntime_span_between(c, 0x100, 0x2000, 0x2100) && rd64(0x100) == 90ll * 1000000000ll);
    CHECK(nntime_user_difference(c, 0x2000, 0x2100) == 0);
    /* two snapshots of other steady clocks and no network time: not comparable, and the output stays */
    memcpy(mem + 0x2200, mem + 0x2100, SNAP_SIZE);
    mem[0x2200 + SNAP_STEADY + 8] ^= 1;
    snx_st_le64(mem + 0x2200 + SNAP_NETWORK_TIME, 0);
    snx_st_le64(mem + 0x100, 5);
    CHECK(!nntime_span_between(c, 0x100, 0x2000, 0x2200) && rd64(0x100) == 5);

    /* the timespecs: the user clock's is the C library's realtime clock, its seconds those of the user clock */
    CHECK(nntime_user_get_current_timespec(c, 0x700) == 0 && rd64(0x700) == nntime_user_now(c));
    CHECK(rd64(0x708) == (i64)((1000000000000ull + test_ns) % 1000000000ull));
    CHECK(nntime_steady_get_current_timespec(c, 0x710) == 0 && rd64(0x710) == nntime_steady_now(c));
    CHECK(nntime_adjustable_user_get_current_timespec(c, 0x720) == 0 && rd64(0x720) == rd64(0x700));

    /* the adjustable clocks: the context of the first Initialize, counting with the steady clock */
    CHECK(nntime_adjustable_user_get_current_time(c, 0x100) == 0 && rd64(0x100) == nntime_user_now(c));
    CHECK(nntime_adjustable_user_now(c) == nntime_adjustable_network_now(c));
    nntime_adjustable_user_get_context(c, 0x740);
    nntime_adjustable_network_get_context(c, 0x760);
    CHECK(!memcmp(mem + 0x740, mem + 0x400, 0x20) && !memcmp(mem + 0x760, mem + 0x400, 0x20));
    nntime_snapshot_create_adjustable(c, 0x2300);
    CHECK(mem[0x2300 + SNAP_TYPE] == 2 && rd64(0x2300 + SNAP_USER_TIME) == nntime_user_now(c));

    /* the posix times of a local time: one, in a zone without daylight saving */
    strcpy((char*)mem + 0x500, "Etc/GMT-9");
    CHECK(nntime_load_time_zone_rule(c, 0x10000, 0x500) == 0);
    u8 cal[8] = {0xe8, 0x07, 3, 1, 0, 4, 5, 0}; /* 2024-03-01 00:04:05 */
    memcpy(mem + 0x800, cal, 8);
    CHECK(nntime_to_posix_time_rule(c, 0x810, 0x820, 2, 0x800, 0x10000) == 0);
    CHECK(snx_ld_le32(mem + 0x810) == 1 && rd64(0x820) == 1709219045);
    CHECK(nntime_to_posix_time_rule(c, 0x810, 0x820, 0, 0x800, 0x10000) == SNX_RC_TIME_INVALID_POINTER);
    mem[0x802] = 13;
    CHECK(nntime_to_posix_time(c, 0x810, 0x820, 2, 0x800) == SNX_RC_TIME_INVALID_POINTER);

    /* the zones: UTC, then Etc/GMT+12 (UTC-12) to Etc/GMT-14 (UTC+14) */
    CHECK(nntime_total_location_name_count(c) == 28);
    nntime_load_location_name_list(c, 0x900, 0x1000, 3, 0);
    CHECK(snx_ld_le32(mem + 0x900) == 3 && !strcmp((char*)mem + 0x1000, "UTC") &&
          !strcmp((char*)mem + 0x1024, "Etc/GMT+12") && !strcmp((char*)mem + 0x1048, "Etc/GMT+11"));
    nntime_load_location_name_list(c, 0x900, 0x1000, 4, 26);
    CHECK(snx_ld_le32(mem + 0x900) == 2 && !strcmp((char*)mem + 0x1024, "Etc/GMT-14"));
    nntime_get_time_zone_rule_version(c, 0x940);
    CHECK(!strcmp((char*)mem + 0x940, "2021a"));
    nntime_get_device_location_name_and_updated_time(c, 0x960, 0x990);
    CHECK(!memcmp(mem + 0x960, mem + 0x200, 0x24) && !memcmp(mem + 0x990, mem + 0x408, 0x18));

    /* what an application may not change: the clocks answer NoCapability, the rest stops the game */
    CHECK(nntime_set_clock(c, 0x100) == SNX_RC_TIME_NO_CAPABILITY);
    CHECK_STOPS(nntime_set_device_location_name(c, 0x200));
    CHECK_STOPS(nntime_set_automatic_correction_enabled(c, 0));
    CHECK(nntime_is_automatic_correction_enabled(c) && nntime_is_accuracy_sufficient(c) && nntime_initial_year(c) == 2019);

    /* after Finalize to zero: the clocks answer 0x74 again, the adjustable ones stop the game */
    CHECK(nntime_finalize(c) == 0 && !nntime_is_initialized(c));
    CHECK(nntime_user_get_current_timespec(c, 0x700) == 0x74 && nntime_set_clock(c, 0x100) == 0x74);
    CHECK_STOPS(nntime_adjustable_user_now(c));
    CHECK_STOPS(nntime_adjustable_user_adjust(c));

    printf("nn_time: %u checks passed\n", checks);
    return 0;
}
