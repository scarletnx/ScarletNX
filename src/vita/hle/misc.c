/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The small system libraries: ScePower (a charged battery on mains power), SceAppUtil, SceSysmodule (every module is
   there: the runtime answers their functions), SceRtc, SceAppMgr, SceAppUtil's save data (in savedata0:, io.c). The
   network is net.c's */
#include "vita/hle/hle.h"
#include "common/utilities/endian.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCE_FONT_ERROR_HANDLER_OPEN_FAILED 0x80460005u
#define SCE_RTC_ERROR_INVALID_POINTER 0x80251001u

static uint32_t zero(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
static uint32_t one(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 1;
}
static uint32_t battery_percent(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 100;
}
static uint32_t battery_life_time(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 300; /* minutes */
}
static uint32_t arm_clock(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 444;
}
/* sceAppUtilSystemParamGetInt(id, *value): the system's language (1, English), enter button (1, cross), date and
   time formats, the time zone */
static uint32_t system_param_int(VitaProcess* p, VitaThread* t) {
    uint32_t id = vita_arg(p, t, 0), out = vita_arg(p, t, 1), value = 0;
    switch (id) {
        case 1: value = 1; break; /* SCE_SYSTEM_PARAM_ID_LANG: English (United States) */
        case 2: value = 1; break; /* SCE_SYSTEM_PARAM_ID_ENTER_BUTTON: cross */
        default: value = 0; break;
    }
    if (out) vita_write32(p, out, value);
    return 0;
}
static uint32_t system_param_string(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 1), size = vita_arg(p, t, 2);
    static const char name[] = "Player";
    if (out && size) {
        size_t n = size < sizeof name ? size - 1 : sizeof name - 1;
        memcpy(vita_ptr(p, out), name, n);
        ((char*)vita_ptr(p, out))[n] = 0;
    }
    return 0;
}
/* sceRtcGetTime_t(const SceDateTime*, time_t*): the date (year, month, day, hour, minute, second as 16 bits, then
   microseconds) as the seconds since 1970, a 32-bit time_t as SceRtc has it */
/* days since 1970 of a civil date (Howard Hinnant's days_from_civil) */
static int64_t days_from_civil(int64_t year, int64_t month, int64_t day) {
    int64_t y = year - (month <= 2), era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
/* a SceDateTime's seconds since 1970 */
static int64_t date_seconds(VitaProcess* p, uint32_t date) {
    const unsigned char* b = vita_ptr(p, date);
    int64_t f[6];
    for (unsigned i = 0; i < 6; ++i) f[i] = snx_ld_le16(b + i * 2);
    return days_from_civil(f[0], f[1], f[2]) * 86400 + f[3] * 3600 + f[4] * 60 + f[5];
}
static uint32_t rtc_get_time_t(VitaProcess* p, VitaThread* t) {
    uint32_t date = vita_arg(p, t, 0), out = vita_arg(p, t, 1);
    if (!date || !out) return SCE_RTC_ERROR_INVALID_POINTER;
    vita_write32(p, out, (uint32_t)date_seconds(p, date));
    return 0;
}
static uint32_t rtc_get_time64_t(VitaProcess* p, VitaThread* t) {
    uint32_t date = vita_arg(p, t, 0), out = vita_arg(p, t, 1);
    if (!date || !out) return SCE_RTC_ERROR_INVALID_POINTER;
    vita_write64(p, out, (uint64_t)date_seconds(p, date));
    return 0;
}
/* SceRtcTick: microseconds since 0001-01-01 */
#define UNIX_EPOCH_TICK 62135596800000000ull
static uint32_t rtc_get_tick(VitaProcess* p, VitaThread* t) {
    uint32_t date = vita_arg(p, t, 0), out = vita_arg(p, t, 1);
    if (!date || !out) return SCE_RTC_ERROR_INVALID_POINTER;
    vita_write64(p, out, UNIX_EPOCH_TICK + (uint64_t)(date_seconds(p, date) * 1000000) + vita_read32(p, date + 12));
    return 0;
}
static uint32_t rtc_get_current_tick(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 0);
    if (!out) return SCE_RTC_ERROR_INVALID_POINTER;
    struct timespec now;
#if defined(__ANDROID__) && __ANDROID_API__ < 29
    clock_gettime(CLOCK_REALTIME, &now);
#else
    timespec_get(&now, TIME_UTC);
#endif
    vita_write64(p, out, UNIX_EPOCH_TICK + (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u);
    return 0;
}
static uint32_t rtc_get_tick_resolution(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 1000000;
}
/* sceRtcGetDayOfWeek(year, month, day): 0 for Sunday */
static uint32_t rtc_get_day_of_week(VitaProcess* p, VitaThread* t) {
    int64_t days = days_from_civil(vita_arg(p, t, 0), vita_arg(p, t, 1), vita_arg(p, t, 2));
    return (uint32_t)(((days + 4) % 7 + 7) % 7); /* 1970-01-01 was a Thursday */
}
/* sceRtcSetTime_t(SceDateTime*, time_t): the date of a 32-bit time_t, UTC */
static uint32_t rtc_set_time_t(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 0);
    if (!out) return SCE_RTC_ERROR_INVALID_POINTER;
    time_t seconds = (time_t)(int32_t)vita_arg(p, t, 1);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    uint16_t f[6] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_mday,
                     (uint16_t)tm.tm_hour,          (uint16_t)tm.tm_min,       (uint16_t)tm.tm_sec};
    unsigned char* b = vita_ptr(p, out);
    for (unsigned i = 0; i < 6; ++i) snx_st_le16(b + i * 2, f[i]);
    vita_write32(p, out + 12, 0);
    return 0;
}
/* a SceDateTime from seconds since 1970 and microseconds, UTC */
static void date_from_seconds(VitaProcess* p, uint32_t out, int64_t seconds, uint32_t microseconds) {
    int64_t days = seconds >= 0 ? seconds / 86400 : -((-seconds + 86399) / 86400), rest = seconds - days * 86400;
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153, day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
    int64_t year = yoe + era * 400 + (month <= 2);
    uint16_t f[6] = {(uint16_t)year, (uint16_t)month, (uint16_t)day, (uint16_t)(rest / 3600), (uint16_t)(rest / 60 % 60),
                     (uint16_t)(rest % 60)};
    unsigned char* b = vita_ptr(p, out);
    for (unsigned i = 0; i < 6; ++i) snx_st_le16(b + i * 2, f[i]);
    vita_write32(p, out + 12, microseconds);
}
/* sceRtcSetTick(SceDateTime*, const SceRtcTick*) */
static uint32_t rtc_set_tick(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 0), tick = vita_arg(p, t, 1);
    if (!out || !tick) return SCE_RTC_ERROR_INVALID_POINTER;
    int64_t since = (int64_t)(((uint64_t)vita_read32(p, tick + 4) << 32 | vita_read32(p, tick)) - UNIX_EPOCH_TICK);
    int64_t seconds = since >= 0 ? since / 1000000 : -((-since + 999999) / 1000000);
    date_from_seconds(p, out, seconds, (uint32_t)(since - seconds * 1000000));
    return 0;
}
/* sceRtcSetTime64_t(SceDateTime*, uint64_t time) */
static uint32_t rtc_set_time64_t(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 0);
    if (!out) return SCE_RTC_ERROR_INVALID_POINTER;
    date_from_seconds(p, out, (int64_t)((uint64_t)vita_arg(p, t, 3) << 32 | vita_arg(p, t, 2)), 0);
    return 0;
}
/* sceRtcGetAccumulativeTime(): microseconds since the process started, 64 bits */
static uint32_t rtc_get_accumulative_time(VitaProcess* p, VitaThread* t) {
    return vita_return64(t, (vita_now_ns() - p->start_ns) / 1000u);
}
/* SceDateTime (16 bytes: year, month, day, hour, minute, second as 16 bits, then microseconds) of now, local time */
static void date_time_now(unsigned char* b) {
    struct timespec now;
#if defined(__ANDROID__) && __ANDROID_API__ < 29
    clock_gettime(CLOCK_REALTIME, &now);
#else
    timespec_get(&now, TIME_UTC);
#endif
    time_t seconds = now.tv_sec;
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    uint16_t f[6] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_mday,
                     (uint16_t)tm.tm_hour,          (uint16_t)tm.tm_min,       (uint16_t)tm.tm_sec};
    uint32_t us = (uint32_t)(now.tv_nsec / 1000);
    for (unsigned i = 0; i < 6; ++i) snx_st_le16(b + i * 2, f[i]);
    snx_st_le32(b + 12, us);
}
static uint32_t rtc_current_local(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 0);
    if (!out) return SCE_RTC_ERROR_INVALID_POINTER;
    date_time_now(vita_ptr(p, out));
    return 0;
}
/* sceMotionGetState(SceMotionState*): a console at rest, level and face up: gravity alone on the accelerometer, the
   identity orientation */
static void put_float(VitaProcess* p, uint32_t address, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    vita_write32(p, address, bits);
}
static uint32_t motion_get_state(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 0);
    if (!out) return 0x80360200u; /* SCE_MOTION_ERROR_NULL_PARAMETER */
    uint64_t us = (vita_now_ns() - p->start_ns) / 1000u;
    memset(vita_ptr(p, out), 0, 0xf8);
    vita_write32(p, out, (uint32_t)us);
    put_float(p, out + 12, -1.0f);         /* acceleration's z, in g */
    put_float(p, out + 52, 1.0f);          /* the quaternion's w */
    for (unsigned i = 0; i < 4; ++i) {     /* the rotation and NED matrices */
        put_float(p, out + 56 + i * 20, 1.0f);
        put_float(p, out + 120 + i * 20, 1.0f);
    }
    put_float(p, out + 196, 1.0f);         /* basic orientation: face up */
    vita_write64(p, out + 200, us);
    vita_write32(p, out + 244, 2);         /* SCE_MOTION_MAG_STABLE */
    return 0;
}
/* sceAppMgrReceiveSystemEvent and sceAppUtilReceiveAppEvent: no event comes */
static uint32_t receive_system_event(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0x80802013u; /* SCE_APPMGR_ERROR_STATE */
}
static uint32_t receive_app_event(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 0);
    if (!out) return 0x80100600u; /* SCE_APPUTIL_ERROR_PARAMETER */
    vita_write32(p, out, 0); /* no event */
    return 0;
}
/* sceAppUtilAppParamGetInt(id, *value): the game's full version (SCE_APPUTIL_APPPARAM_ID_SKU_FLAG 0, SKU 3) */
static uint32_t app_param_get_int(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 1);
    if (!out || vita_arg(p, t, 0)) return 0x80100600u;
    vita_write32(p, out, 3); /* SCE_APPUTIL_APPPARAM_SKU_FLAG_FULL */
    return 0;
}
/* ---- SceAppUtil's save data, in savedata0: (VitaProcess.savedata) ----
   A slot is the game's description of a save (SceAppUtilSaveDataSlotParam, 0x34c bytes: status, title, subtitle,
   detail, icon path, user parameter, size, modified time at 0x30c), kept in savedata0:sce_sys/slot<ID>.bin, out of
   the game's own files. The data are the game's files in savedata0: */
#define SCE_APPUTIL_ERROR_PARAMETER             0x80100600u
#define SCE_APPUTIL_ERROR_SAVEDATA_SLOT_EXISTS    0x80100640u
#define SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND 0x80100641u
#define SCE_APPUTIL_ERROR_SAVEDATA_NO_SPACE_FS    0x80100643u
#define SAVEDATA_SLOTS        0x100u
#define SLOT_PARAM_SIZE       0x34cu
#define SLOT_PARAM_MODIFIED   0x30cu
#define SAVE_MODE_FILE_TRUNCATE 1u
#define SAVE_MODE_DIRECTORY     2u
#define SAVE_FILE_MOST        (256u << 20) /* a save file larger than the Vita's card holds is refused */
/* the host path of a save data path (relative to savedata0:, or a device's), NULL for one leaving savedata0: */
static char* save_path(VitaProcess* p, uint32_t address) {
    char path[512];
    if (!vita_string(p, address, path, sizeof path)) return NULL;
    if (strchr(path, ':')) return vita_io_host_path(p, address);
    for (const char* q = path; *q;) {
        const char* slash = strchr(q, '/');
        size_t n = slash ? (size_t)(slash - q) : strlen(q);
        if (n == 2 && q[0] == '.' && q[1] == '.') return NULL;
        if (!slash) break;
        q = slash + 1;
    }
    const char* rest = path;
    while (*rest == '/') ++rest;
    return snx_path_join(p->savedata, rest);
}
static char* slot_path(VitaProcess* p, uint32_t id) { return snx_format("%s/sce_sys/slot%u.bin", p->savedata, id); }
static bool slot_write(VitaProcess* p, uint32_t id, const unsigned char* param) {
    char* path = slot_path(p, id);
    char* folder = path ? snx_dirname(path) : NULL;
    bool ok = folder && !snx_mkdirs(folder) && !snx_write_file(path, param, SLOT_PARAM_SIZE);
    free(folder);
    free(path);
    return ok;
}
/* the slot's parameters into param (SLOT_PARAM_SIZE bytes): false for a slot the game hasn't made */
static bool slot_read(VitaProcess* p, uint32_t id, unsigned char* param) {
    char* path = slot_path(p, id);
    size_t size = 0;
    unsigned char* bytes = path ? snx_read_file(path, &size) : NULL;
    bool ok = bytes && size == SLOT_PARAM_SIZE;
    if (ok) memcpy(param, bytes, SLOT_PARAM_SIZE);
    free(bytes);
    free(path);
    return ok;
}
/* whether the game has made slot id (the save data dialog's isExist) */
bool vita_savedata_slot_exists(VitaProcess* p, uint32_t id) {
    unsigned char param[SLOT_PARAM_SIZE];
    return id < SAVEDATA_SLOTS && slot_read(p, id, param);
}
/* sceAppUtilSaveDataSlotCreate(id, const SceAppUtilSaveDataSlotParam*, mount point) */
static uint32_t save_data_slot_create(VitaProcess* p, VitaThread* t) {
    uint32_t id = vita_arg(p, t, 0), param = vita_arg(p, t, 1);
    unsigned char old[SLOT_PARAM_SIZE];
    if (id >= SAVEDATA_SLOTS || !param || !vita_mapped(p, param, SLOT_PARAM_SIZE)) return SCE_APPUTIL_ERROR_PARAMETER;
    if (slot_read(p, id, old)) return SCE_APPUTIL_ERROR_SAVEDATA_SLOT_EXISTS;
    unsigned char copy[SLOT_PARAM_SIZE];
    memcpy(copy, vita_ptr(p, param), SLOT_PARAM_SIZE);
    date_time_now(copy + SLOT_PARAM_MODIFIED);
    return slot_write(p, id, copy) ? 0 : SCE_APPUTIL_ERROR_SAVEDATA_NO_SPACE_FS;
}
/* sceAppUtilSaveDataSlotDelete(id, mount point) */
static uint32_t save_data_slot_delete(VitaProcess* p, VitaThread* t) {
    uint32_t id = vita_arg(p, t, 0);
    if (id >= SAVEDATA_SLOTS) return SCE_APPUTIL_ERROR_PARAMETER;
    char* path = slot_path(p, id);
    bool there = path && snx_is_file(path);
    if (there) remove(path);
    free(path);
    return there ? 0 : SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND;
}
/* sceAppUtilSaveDataSlotGetParam(id, SceAppUtilSaveDataSlotParam*, mount point) */
static uint32_t save_data_slot_get_param(VitaProcess* p, VitaThread* t) {
    uint32_t id = vita_arg(p, t, 0), param = vita_arg(p, t, 1);
    unsigned char copy[SLOT_PARAM_SIZE];
    if (id >= SAVEDATA_SLOTS || !param || !vita_mapped(p, param, SLOT_PARAM_SIZE)) return SCE_APPUTIL_ERROR_PARAMETER;
    if (!slot_read(p, id, copy)) return SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND;
    memset(copy, 0, 4); /* status: the slot is fine */
    memcpy(vita_ptr(p, param), copy, SLOT_PARAM_SIZE);
    return 0;
}
/* sceAppUtilSaveDataSlotSetParam(id, const SceAppUtilSaveDataSlotParam*, mount point) */
static uint32_t save_data_slot_set_param(VitaProcess* p, VitaThread* t) {
    uint32_t id = vita_arg(p, t, 0), param = vita_arg(p, t, 1);
    unsigned char copy[SLOT_PARAM_SIZE];
    if (id >= SAVEDATA_SLOTS || !param || !vita_mapped(p, param, SLOT_PARAM_SIZE)) return SCE_APPUTIL_ERROR_PARAMETER;
    if (!slot_read(p, id, copy)) return SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND;
    memcpy(copy, vita_ptr(p, param), SLOT_PARAM_SIZE);
    return slot_write(p, id, copy) ? 0 : SCE_APPUTIL_ERROR_SAVEDATA_NO_SPACE_FS;
}
/* size bytes of data at offset in the file at path, made when missing: the bytes after them kept, unless truncate */
static bool save_file(const char* path, const unsigned char* data, uint32_t size, uint64_t offset, bool truncate) {
    if (offset > SAVE_FILE_MOST || size > SAVE_FILE_MOST - offset) return false;
    size_t old_size = 0;
    unsigned char* old = snx_is_file(path) ? snx_read_file(path, &old_size) : NULL;
    size_t end = (size_t)offset + size, total = truncate || old_size < end ? end : old_size;
    unsigned char* bytes = calloc(total ? total : 1, 1);
    bool ok = bytes != NULL;
    if (ok) {
        if (old) memcpy(bytes, old, old_size < total ? old_size : total); /* none before the first save */
        if (data) memcpy(bytes + offset, data, size);
        char* folder = snx_dirname(path);
        ok = folder && !snx_mkdirs(folder) && !snx_write_file(path, bytes, total);
        free(folder);
    }
    free(bytes);
    free(old);
    return ok;
}
/* sceAppUtilSaveDataDataSave(SceAppUtilSaveDataFileSlot* slot, SceAppUtilSaveDataFile* files, count, mount point,
   SceSize* required KiB): each file (0x40 bytes: path, buffer, size, offset at 0x10, mode at 0x18) written, a
   directory made; then the slot's parameters, the ones given (slot: id, parameters) or those it has, with now for
   their modified time */
static uint32_t save_data_data_save(VitaProcess* p, VitaThread* t) {
    uint32_t slot = vita_arg(p, t, 0), files = vita_arg(p, t, 1), count = vita_arg(p, t, 2);
    uint32_t required = vita_arg(p, t, 4);
    /* the slot (id, parameters), the files and the given parameters in the game's memory */
    if ((slot && !vita_mapped(p, slot, 8)) ||
        (count && (count > UINT32_MAX / 0x40 || !vita_mapped(p, files, count * 0x40))))
        return SCE_APPUTIL_ERROR_PARAMETER;
    uint32_t id = slot ? vita_read32(p, slot) : 0, given = slot ? vita_read32(p, slot + 4) : 0;
    if (id >= SAVEDATA_SLOTS || (given && !vita_mapped(p, given, SLOT_PARAM_SIZE)) ||
        (required && !vita_mapped(p, required, 4)))
        return SCE_APPUTIL_ERROR_PARAMETER;
    if (required) vita_write32(p, required, 0); /* there is room */
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t f = files + i * 0x40, buffer = vita_read32(p, f + 4), size = vita_read32(p, f + 8),
                 mode = vita_read32(p, f + 0x18);
        uint64_t offset = vita_read32(p, f + 0x10) | (uint64_t)vita_read32(p, f + 0x14) << 32;
        if (buffer && mode != SAVE_MODE_DIRECTORY && !vita_mapped(p, buffer, size)) return SCE_APPUTIL_ERROR_PARAMETER;
        char* path = save_path(p, vita_read32(p, f));
        bool ok = path && (mode == SAVE_MODE_DIRECTORY
                               ? !snx_mkdirs(path)
                               : save_file(path, buffer ? vita_ptr(p, buffer) : NULL, buffer ? size : 0, offset,
                                           mode == SAVE_MODE_FILE_TRUNCATE));
        free(path);
        if (!ok) return path ? SCE_APPUTIL_ERROR_SAVEDATA_NO_SPACE_FS : SCE_APPUTIL_ERROR_PARAMETER;
    }
    if (!slot) return 0;
    unsigned char param[SLOT_PARAM_SIZE];
    if (given)
        memcpy(param, vita_ptr(p, given), SLOT_PARAM_SIZE);
    else if (!slot_read(p, id, param))
        return 0;
    date_time_now(param + SLOT_PARAM_MODIFIED);
    return slot_write(p, id, param) ? 0 : SCE_APPUTIL_ERROR_SAVEDATA_NO_SPACE_FS;
}
/* sceAppUtilSaveDataDataRemove(SceAppUtilSaveDataFileSlot* slot, SceAppUtilSaveDataRemoveItem* items, count, mount
   point): each item's file or folder (0x2c bytes: path, mode), and the slot unless the first's mode keeps it
   (SCE_APPUTIL_SAVEDATA_DATA_REMOVE_MODE_NO_SLOT, 1) */
static uint32_t save_data_data_remove(VitaProcess* p, VitaThread* t) {
    uint32_t slot = vita_arg(p, t, 0), items = vita_arg(p, t, 1), count = vita_arg(p, t, 2);
    /* the slot (its id) and the items in the game's memory, as DataSave's */
    if ((slot && !vita_mapped(p, slot, 4)) ||
        (count && (count > UINT32_MAX / 0x2c || !vita_mapped(p, items, count * 0x2c))) ||
        (slot && vita_read32(p, slot) >= SAVEDATA_SLOTS))
        return SCE_APPUTIL_ERROR_PARAMETER;
    for (uint32_t i = 0; i < count; ++i) {
        char* path = save_path(p, vita_read32(p, items + i * 0x2c));
        if (!path) return SCE_APPUTIL_ERROR_PARAMETER;
        if (snx_path_exists(path)) snx_remove_tree(path);
        free(path);
    }
    if (slot && (!count || vita_read32(p, items + 4) != 1)) {
        char* path = slot_path(p, vita_read32(p, slot));
        if (path) remove(path);
        free(path);
    }
    return 0;
}
/* ---- SceAppUtil's add-on content: ux0:addcont/<title ID>/<its ID> on the console, the game folder's ux0/ here ----
   sceAppUtilDrmOpen(const SceAppUtilDrmAddcontId* id, mount point) opens the license of the add-on id (its 20 byte
   ID, a game's online pass among them): ENOENT when it isn't installed, which a game takes for not owning it. Its
   sceAppUtilDrmClose closes it, NOT_MOUNTED for one that isn't there */
#define SCE_APPUTIL_ERROR_NOT_MOUNTED 0x80100604u
#define SCE_ERROR_ERRNO_ENOENT        0x80010002u
static bool addcont_installed(VitaProcess* p, uint32_t id) {
    char name[20];
    if (!vita_string(p, id, name, sizeof name) || !name[0] || strchr(name, '/') || strchr(name, '\\') ||
        !strcmp(name, "..") || !strcmp(name, "."))
        return false;
    char* path = snx_format("%s/addcont/%s/%s", p->ux0, p->title_id, name);
    bool installed = path && snx_is_dir(path);
    free(path);
    return installed;
}
static uint32_t drm_open(VitaProcess* p, VitaThread* t) {
    uint32_t id = vita_arg(p, t, 0);
    if (!id) return SCE_APPUTIL_ERROR_PARAMETER;
    return addcont_installed(p, id) ? 0 : SCE_ERROR_ERRNO_ENOENT;
}
static uint32_t drm_close(VitaProcess* p, VitaThread* t) {
    uint32_t id = vita_arg(p, t, 0);
    if (!id) return SCE_APPUTIL_ERROR_PARAMETER;
    return addcont_installed(p, id) ? 0 : SCE_APPUTIL_ERROR_NOT_MOUNTED;
}
/* _sceAppMgrGetAppState(SceAppMgrAppState*, size, version): nothing pending, no system screen over the game (the
   state's system and application event counts and isSystemUiOverlaid zero, the rest reserved). A game asks each
   frame, and pauses itself while the system's screen is over it */
#define SCE_APPMGR_ERROR_NULL_POINTER 0x80802016u
#define APP_STATE_SIZE 0x80u
static uint32_t get_app_state(VitaProcess* p, VitaThread* t) {
    uint32_t state = vita_arg(p, t, 0), size = vita_arg(p, t, 1);
    if (!state) return SCE_APPMGR_ERROR_NULL_POINTER;
    memset(vita_ptr(p, state), 0, size < APP_STATE_SIZE ? size : APP_STATE_SIZE);
    return 0;
}
/* sceAppMgrLoadExec: another program in the game's place, which the runtime can't start: the game ends */
static uint32_t load_exec(VitaProcess* p, VitaThread* t) {
    (void)t;
    p->exited = true;
    return 0;
}
/* ScePgf: the console's system fonts, which the runtime doesn't have: sceFontNewLib answers no library and its error */
static uint32_t font_new_lib(VitaProcess* p, VitaThread* t) {
    uint32_t error = vita_arg(p, t, 1);
    if (error) vita_write32(p, error, SCE_FONT_ERROR_HANDLER_OPEN_FAILED);
    return 0;
}
static uint32_t font_fail(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return SCE_FONT_ERROR_HANDLER_OPEN_FAILED;
}

const VitaHle vita_hle_misc[] = {
    {0x2085d15du, "scePowerGetBatteryLifePercent", battery_percent},
    {0x8efb3fa2u, "scePowerGetBatteryLifeTime", battery_life_time},
    {0x1e490401u, "scePowerIsBatteryCharging", one},
    {0x87440f5eu, "scePowerIsPowerOnline", one},
    {0x74db5ae5u, "scePowerSetArmClockFrequency", zero},
    {0xabc6f88fu, "scePowerGetArmClockFrequency", arm_clock},
    {0xdaffe671u, "sceAppUtilInit", zero},
    {0xb220b00bu, "sceAppUtilShutdown", zero},
    {0xee85804du, "sceAppUtilPhotoMount", zero},
    {0x9651b941u, "sceAppUtilPhotoUmount", zero},
    {0xb5edcbffu, "sceAppUtilMusicMount", zero},
    {0xa7fe1bf7u, "sceAppUtilMusicUmount", zero},
    {0x5dfb9ca0u, "sceAppUtilSystemParamGetInt", system_param_int},
    {0x6e6aa267u, "sceAppUtilSystemParamGetString", system_param_string},
    {0x79a0160au, "sceSysmoduleLoadModule", zero},
    {0x31d87805u, "sceSysmoduleUnloadModule", zero},
    {0x53099b7au, "sceSysmoduleIsLoaded", zero},
    {0x8de6feb7u, "sceRtcGetTime_t", rtc_get_time_t},
    {0x0572eddcu, "sceRtcGetCurrentClockLocalTime", rtc_current_local},
    {0xc995de02u, "sceRtcGetTime64_t", rtc_get_time64_t},
    {0xf2b238e2u, "sceRtcGetTick", rtc_get_tick},
    {0x23f79274u, "sceRtcGetCurrentTick", rtc_get_current_tick},
    {0xcddd25feu, "sceRtcGetCurrentNetworkTick", rtc_get_current_tick},
    {0x811313b3u, "sceRtcGetTickResolution", rtc_get_tick_resolution},
    {0x2f3531ebu, "sceRtcGetDayOfWeek", rtc_get_day_of_week},
    {0x3a332f81u, "sceRtcSetTime_t", rtc_set_time_t},
    {0xcd89f464u, "sceRtcSetTick", rtc_set_tick},
    {0xa6c36b6au, "sceRtcSetTime64_t", rtc_set_time64_t},
    {0x258be8ecu, "sceRtcGetAccumulativeTime", rtc_get_accumulative_time},
    {0x3ce187b6u, "scePowerSetConfigurationMode", zero},
    {0x28034ac9u, "sceMotionStartSampling", zero},
    {0x0fd2cda2u, "sceMotionReset", zero},
    {0xbdb32767u, "sceMotionGetState", motion_get_state},
    {0x10b5765fu, "sceAppMgrReceiveSystemEvent", receive_system_event},
    {0xfff8f7f0u, "sceAppMgrIsGameProgram", one},
    {0xafceab96u, "sceAppMgrAcquireBgmPort", zero},
    {0xb18370c2u, "sceTouchEnableTouchForce", zero},
    {0x76e674d1u, "sceScreenShotEnable", zero},
    {0x50ae9ff9u, "sceScreenShotDisable", zero},
    {0x7061665bu, "sceScreenShotSetOverlayImage", zero},
    {0xcc18fbaeu, "sceKernelChangeThreadVfpException", zero},
    {0x9379e2d5u, "_sceFiosKernelOverlayGetList", zero},
    {0xb02e0b26u, "_sceFiosKernelOverlayGetRecommendedScheduler", zero},
    {0x3e9172eau, "_sceFiosKernelOverlayThreadSetDisabled", zero},
    {0xee0dbed9u, "sceAppUtilReceiveAppEvent", receive_app_event},
    {0xcd7fd67au, "sceAppUtilAppParamGetInt", app_param_get_int},
    {0x7e8fe96au, "sceAppUtilSaveDataSlotCreate", save_data_slot_create},
    {0x266a7646u, "sceAppUtilSaveDataSlotDelete", save_data_slot_delete},
    {0x93f0d89fu, "sceAppUtilSaveDataSlotGetParam", save_data_slot_get_param},
    {0x98630136u, "sceAppUtilSaveDataSlotSetParam", save_data_slot_set_param},
    {0x607647bau, "sceAppUtilSaveDataDataSave", save_data_data_save},
    {0x2db7be3bu, "sceAppUtilDrmOpen", drm_open},
    {0x6a140498u, "sceAppUtilDrmClose", drm_close},
    {0xd1c6ab8eu, "sceAppUtilSaveDataDataRemove", save_data_data_remove},
    {0xe6774abcu, "sceAppMgrLoadExec", load_exec},
    {0x5e86319au, "_sceAppMgrGetAppState", get_app_state},
    {0x210c0046u, "__sceAppMgrGetAppState", get_app_state},
    {0x1055aba3u, "sceFontNewLib", font_new_lib},
    {0x07ee1733u, "sceFontDoneLib", zero},
    {0xbd2dfcffu, "sceFontOpen", font_new_lib},
    {0x4a7293e9u, "sceFontClose", zero},
    {0x6fd1ba65u, "sceFontGetCharInfo", font_fail},
    {0xab45aad3u, "sceFontGetCharGlyphImage", font_fail},
    {0, NULL, NULL},
};