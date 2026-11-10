/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/build_info.h"
#include "snx_commit.h"
#include <stdio.h>

#define SNX_TEXT(x)   #x
#define SNX_NUMBER(x) SNX_TEXT(x)
#if defined(__clang__)
#define SNX_COMPILER                                                                                                   \
    "clang " SNX_NUMBER(__clang_major__) "." SNX_NUMBER(__clang_minor__) "." SNX_NUMBER(__clang_patchlevel__)
#elif defined(__GNUC__)
#define SNX_COMPILER "gcc " SNX_NUMBER(__GNUC__) "." SNX_NUMBER(__GNUC_MINOR__) "." SNX_NUMBER(__GNUC_PATCHLEVEL__)
#else
#define SNX_COMPILER "unknown compiler"
#endif
#if defined(_WIN32)
#define SNX_OS "Windows"
#elif defined(__ANDROID__)
#define SNX_OS "Android"
#elif defined(__linux__)
#define SNX_OS "Linux"
#elif defined(__FreeBSD__)
#define SNX_OS "FreeBSD"
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define SNX_OS "iOS"
#else
#define SNX_OS "macOS"
#endif
#else
#define SNX_OS "unknown OS"
#endif
#if defined(__x86_64__) || defined(_M_X64)
#define SNX_ARCH "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define SNX_ARCH "arm64"
#elif defined(__powerpc64__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define SNX_ARCH "ppc64le"
#elif defined(__powerpc64__) && defined(_CALL_ELF) && _CALL_ELF == 2
#define SNX_ARCH "ppc64 (ELFv2)"
#elif defined(__powerpc64__)
#define SNX_ARCH "ppc64"
#elif defined(__mips64) && defined(__MIPSEL__)
#define SNX_ARCH "mips64el"
#elif defined(__mips64)
#define SNX_ARCH "mips64"
#elif defined(__s390x__)
#define SNX_ARCH "s390x"
#elif defined(__alpha__)
#define SNX_ARCH "alpha"
#else
#define SNX_ARCH "unknown arch"
#endif

const char* snx_build_info(void) {
    return "ScarletNX " SNX_BRANCH " " SNX_COMMIT " (" SNX_COMPILER ", " SNX_OS " " SNX_ARCH ")";
}
const char* snx_build_log_line(void) {
    return "ScarletNX " SNX_BRANCH " " SNX_COMMIT ", committed " SNX_COMMIT_DATE " (" SNX_COMPILER ", " SNX_OS
           " " SNX_ARCH ")";
}
const char* snx_build_commit(void) { return SNX_COMMIT; }
void snx_run_log_line(char out[SNX_RUN_LOG_LINE], long long now) {
    /* the day of the Gregorian calendar from the days since 1970-01-01 (Howard Hinnant's civil_from_days): the C
       library's gmtime keeps one answer for every thread */
    long long days = now >= 0 ? now / 86400 : -((86399 - now) / 86400), z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365,
              doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153, month = mp < 10 ? mp + 3 : mp - 9, second = now - days * 86400;
    snprintf(out, SNX_RUN_LOG_LINE, "run: started %04lld-%02lld-%02lld %02lld:%02lld:%02lld UTC",
             yoe + era * 400 + (month <= 2), month, doy - (153 * mp + 2) / 5 + 1, second / 3600, second / 60 % 60,
             second % 60);
}
