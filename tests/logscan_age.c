/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The summary's build line (common/utilities/logscan.c): the day the build's commit was made, from the log's build
   line, and the days from it to the run's beginning, which the caller gives as the log's last write (written) less its
   last line's time, by the days of the UTC calendar */
#include "common/utilities/logscan.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOON_20261104 1793793600LL /* 2026-11-04 12:00:00 UTC */

/* the markdown's build line of a log with this build line and a last line at 2.32 s, the log written then */
static void build(const char* line, long long written, const char* want) {
    char log[512], *md, *at, *end;
    SnxLogScan s;
    snprintf(log, sizeof log, "[   0.00] ScarletNX - Some Game\n[   0.00] build: %s\n[   2.32] FATAL: it stopped\n",
             line);
    assert(!snx_logscan(&s, log, strlen(log)));
    s.written = written;
    md = snx_logscan_markdown(&s, "the log", "###", 1, NULL);
    snx_logscan_free(&s);
    at = md ? strstr(md, "- **Build:** ") : NULL;
    end = at ? strchr(at, '\n') : NULL;
    if (!end || (size_t)(end - at) != strlen(want) || memcmp(at, want, strlen(want))) {
        fprintf(stderr, "logscan_age: %s written at %lld gives\n%.*s\nnot\n%s\n", line, written,
                end ? (int)(end - at) : 0, end ? at : "", want);
        exit(1);
    }
    free(md);
}

int main(void) {
    const char* clang = " (clang 23.1.3, Windows x86_64)";
    char line[128];
#define LINE(head) (snprintf(line, sizeof line, "%s%s", head, clang), line)
    build(LINE("ScarletNX main a52fd20, committed 2026-11-02"), NOON_20261104,
          "- **Build:** `main a52fd20`, committed 2026-11-02, 2 days before this run");
    build(LINE("ScarletNX main a52fd20, committed 2026-11-03"), NOON_20261104,
          "- **Build:** `main a52fd20`, committed 2026-11-03, 1 day before this run");
    build(LINE("ScarletNX main a52fd20, committed 2026-11-04"), NOON_20261104,
          "- **Build:** `main a52fd20`, committed 2026-11-04, 0 days before this run");
    /* written 1 s past midnight: the run began 2 s before it, the day before */
    build(LINE("ScarletNX main a52fd20, committed 2026-11-02"), NOON_20261104 - 43200 + 1,
          "- **Build:** `main a52fd20`, committed 2026-11-02, 1 day before this run");
    /* across a year's end and a leap day */
    build(LINE("ScarletNX main a52fd20, committed 2025-12-31"), 1767268800LL,
          "- **Build:** `main a52fd20`, committed 2025-12-31, 1 day before this run");
    build(LINE("ScarletNX main a52fd20, committed 2028-02-28"), 1835524800LL,
          "- **Build:** `main a52fd20`, committed 2028-02-28, 2 days before this run");
    /* a clock that says the run came first: the days as they are, less than none */
    build(LINE("ScarletNX main a52fd20, committed 2026-11-06"), NOON_20261104,
          "- **Build:** `main a52fd20`, committed 2026-11-06, -2 days before this run");
    /* no run time (a log read as text, the game reports'): the day alone */
    build(LINE("ScarletNX main a52fd20-dirty, committed 2026-11-02"), 0,
          "- **Build:** `main a52fd20-dirty`, committed 2026-11-02");
    /* a build without git, as a source archive's: what it has, and no days from a day it doesn't know */
    build(LINE("ScarletNX unknown unknown, committed unknown"), NOON_20261104,
          "- **Build:** `unknown unknown`, committed unknown");
    /* a build before the day was recorded, and a day that is not one: the line whole */
    build(LINE("ScarletNX main a52fd20"), NOON_20261104,
          "- **Build:** `ScarletNX main a52fd20 (clang 23.1.3, Windows x86_64)`");
    build(LINE("ScarletNX main a52fd20, committed 2026-13-02"), NOON_20261104,
          "- **Build:** `ScarletNX main a52fd20, committed 2026-13-02 (clang 23.1.3, Windows x86_64)`");
    /* the log's text stays a code span: a branch's backtick is a quote */
    build(LINE("ScarletNX x`y a52fd20, committed 2026-11-02"), 0, "- **Build:** `x'y a52fd20`, committed 2026-11-02");
    puts("logscan_age: the build's commit day and its days before the run passed");
    return 0;
}
