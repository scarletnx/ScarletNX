/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The line the window titles carry: the branch and commit the build read, this compiler's version, this host; and the
   logs' line, with the day that commit was made. */
#include "common/utilities/build_info.h"
#include "snx_commit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

int main(void) {
    char want[256], logged[256];
    const char *os, *arch, *day = SNX_COMMIT_DATE;
#if defined(_WIN32)
    os = "Windows";
#elif defined(__linux__)
    os = "Linux";
#elif defined(__FreeBSD__)
    os = "FreeBSD";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    os = "iOS";
#elif defined(__APPLE__)
    os = "macOS";
#else
    os = "unknown OS";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    arch = "arm64";
#elif defined(__mips64) && defined(__MIPSEL__)
    arch = "mips64el";
#elif defined(__mips64)
    arch = "mips64";
#elif defined(__s390x__)
    arch = "s390x";
#else
    arch = NULL;
#endif
#if defined(__clang__)
    snprintf(want, sizeof want, "ScarletNX %s %s (clang %d.%d.%d, %s %s)", SNX_BRANCH, SNX_COMMIT, __clang_major__,
             __clang_minor__, __clang_patchlevel__, os, arch ? arch : "");
    snprintf(logged, sizeof logged, "ScarletNX %s %s, committed %s (clang %d.%d.%d, %s %s)", SNX_BRANCH, SNX_COMMIT,
             day, __clang_major__, __clang_minor__, __clang_patchlevel__, os, arch ? arch : "");
#else
    snprintf(want, sizeof want, "ScarletNX %s %s (gcc %d.%d.%d, %s %s)", SNX_BRANCH, SNX_COMMIT, __GNUC__,
             __GNUC_MINOR__, __GNUC_PATCHLEVEL__, os, arch ? arch : "");
    snprintf(logged, sizeof logged, "ScarletNX %s %s, committed %s (gcc %d.%d.%d, %s %s)", SNX_BRANCH, SNX_COMMIT, day,
             __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__, os, arch ? arch : "");
#endif
    printf("%s\n%s\n", snx_build_info(), snx_build_log_line());
    {
        /* the run's line: the epoch, a leap day's last second, a year's first, a day before 1970 */
        static const struct {
            long long now;
            const char* line;
        } runs[] = {{0, "run: started 1970-01-01 00:00:00 UTC"},
                    {1835481599LL, "run: started 2028-02-29 23:59:59 UTC"},
                    {1798761600LL, "run: started 2027-01-01 00:00:00 UTC"},
                    {1793793600LL, "run: started 2026-11-04 12:00:00 UTC"},
                    {-1, "run: started 1969-12-31 23:59:59 UTC"}};
        for (size_t i = 0; i < sizeof runs / sizeof *runs; i++) {
            char run[SNX_RUN_LOG_LINE];
            snx_run_log_line(run, runs[i].now);
            if (strcmp(run, runs[i].line)) {
                fprintf(stderr, "build_info: %lld gives \"%s\", not \"%s\"\n", runs[i].now, run, runs[i].line);
                return 1;
            }
        }
    }
    assert(!arch || !strcmp(snx_build_info(), want));
    assert(!arch || !strcmp(snx_build_log_line(), logged));
    /* a day, YYYY-MM-DD, or "unknown" */
    assert(!strcmp(day, "unknown") ||
           (strlen(day) == 10 && day[4] == '-' && day[7] == '-' && strspn(day, "0123456789") == 4 &&
            strspn(day + 5, "0123456789") == 2 && strspn(day + 8, "0123456789") == 2));
    assert(strlen(SNX_COMMIT) >= 7); /* a hash, or "unknown" */
    assert(!strcmp(snx_build_commit(), SNX_COMMIT));
    assert(SNX_BRANCH[0]); /* a branch, "detached" or "unknown" */
    puts("build info: commit, compiler and host in the window titles' line, and its commit's day in the logs' passed");
    return 0;
}
