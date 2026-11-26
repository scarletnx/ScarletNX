/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/run_log.h"
#include "common/platform/time.h"
#include "common/utilities/common.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

static struct {
    char buf[1 << 16]; /* a cut's moving bytes: the callers cut under their own lock, and a process has one log */
} g_run_log;

/* a name snx_run_log_keep gives a run's log: YYYY-MM-DD_HH-MM-SS, -N when two share a second, .log */
static int run_log_name(const char* name) {
    static const char shape[] = "0000-00-00_00-00-00";
    size_t i, n = strlen(name);
    for (i = 0; i < sizeof shape - 1; ++i)
        if (i >= n || (shape[i] == '0' ? name[i] < '0' || name[i] > '9' : name[i] != shape[i])) return 0;
    return n >= i + 4 && !strcmp(name + n - 4, ".log");
}
/* run_log_name's names oldest first: by the second, then the plain one before its -2, -3...; any other by its bytes */
static int by_name(const void* a, const void* b) {
    const char *x = *(char* const*)a, *y = *(char* const*)b;
    int c = strncmp(x, y, 19);
    if (c || !run_log_name(x) || !run_log_name(y)) return c ? c : strcmp(x, y);
    return (x[19] == '-' ? atoi(x + 20) : 1) - (y[19] == '-' ? atoi(y + 20) : 1);
}
void snx_run_log_keep(const char* path, const char* dir, char* why, size_t size) {
    char dated[1500], stamp[32], **names = NULL;
    SnxU64 when = snx_file_time(path);
    time_t t = (time_t)when;
    struct tm tm;
    size_t count = 0, i, runs = 0;
    int n;
    why[0] = 0;
    if (!when) return; /* the first run */
    if (snx_mkdirs(dir)) {
        snprintf(why, size, "log: cannot make %s (%s): the last run's log is written over\n", dir, strerror(errno));
        return;
    }
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tm);
    snprintf(dated, sizeof dated, "%s/%s.log", dir, stamp);
    for (n = 2; snx_file_time(dated) && n < 100; ++n) snprintf(dated, sizeof dated, "%s/%s-%d.log", dir, stamp, n);
    if (rename(path, dated)) {
        snprintf(why, size, "log: cannot move the last run's log to %s (%s): it is written over\n", dated,
                 strerror(errno));
        return;
    }
    if (snx_list_dir(dir, &names, &count)) return;
    qsort(names, count, sizeof *names, by_name);
    for (i = count; i-- > 0;) {
        if (!run_log_name(names[i]) || ++runs < SNX_RUN_LOGS) continue;
        snprintf(dated, sizeof dated, "%s/%s", dir, names[i]);
        if (remove(dated) && !why[0])
            snprintf(why, size, "log: cannot delete %s (%s), past the %d runs' logs kept\n", dated, strerror(errno),
                     SNX_RUN_LOGS);
    }
    snx_free_strings(names, count);
}

long snx_run_log_cut(const char* path, long limit, uint64_t t0, SnxRunLogCut* cut, char* said, size_t said_size) {
#if defined(_WIN32)
    static const char eol[] = "\r\n";
#else
    static const char eol[] = "\n";
#endif
    char* buf = g_run_log.buf;
    char mark[320];
    long size, head = 0, from, to, n;
    size_t got, i;
    int moved;
    FILE* f = fopen(path, "r+b");
    said[0] = 0;
    cut->last = -1;
    if (!f || fseek(f, 0, SEEK_END) || (size = ftell(f)) <= limit / 2) {
        if (!f) {
            cut->cuts = -1;
            snprintf(said, said_size, "log: cannot cut %s past log_max=%ld MiB (%s): it grows on\n", path, limit >> 20,
                     strerror(errno));
        } else
            fclose(f);
        return 0;
    }
    if (cut->head < 0) {
        rewind(f);
        got = fread(buf, 1, (size_t)(size < SNX_RUN_LOG_HEAD ? size : SNX_RUN_LOG_HEAD), f);
        for (i = got; i > 0; --i)
            if (buf[i - 1] == '\n') break;
        cut->head = (long)i;
    }
    head = cut->head;
    from = size - (limit / 2 - head);
    if (!fseek(f, from, SEEK_SET) &&
        (got = fread(buf, 1, sizeof g_run_log.buf, f)) > 0) { /* to the next line's start */
        for (i = 0; i < got && buf[i] != '\n'; ++i) continue;
        from += (long)(i < got ? i + 1 : got);
    }
    n = snprintf(
        mark, sizeof mark,
        "[%7.2f] log: reached log_max=%ld MiB: %.1f MiB of lines between the run's start and here cut (Debug > "
        "Log size limit raises it)%s",
        (double)(plat_ns() - t0) / 1e9, limit >> 20, (double)(from - head) / (1 << 20), eol);
    to = head + n;
    moved = !fseek(f, head, SEEK_SET) && fwrite(mark, 1, (size_t)n, f) == (size_t)n;
    while (moved && from < size) {
        size_t want = (size_t)(size - from < (long)sizeof g_run_log.buf ? size - from : (long)sizeof g_run_log.buf);
        moved = !fseek(f, from, SEEK_SET) && (got = fread(buf, 1, want, f)) > 0 && !fseek(f, to, SEEK_SET) &&
                fwrite(buf, 1, got, f) == got;
        from += (long)got;
        to += (long)got;
    }
    moved = !fflush(f) && moved;
#if defined(_WIN32)
    moved = moved && !_chsize_s(_fileno(f), to);
#else
    moved = moved && !ftruncate(fileno(f), to);
#endif
    if (!moved) { /* a disk's failure halfway: the lines stay, some twice, and no more cuts are tried */
        cut->cuts = -1;
        to = size;
        snprintf(said, said_size, "log: cutting %s past log_max=%ld MiB failed (%s): it grows on\n", path, limit >> 20,
                 strerror(errno));
    } else
        cut->cuts++;
    fclose(f);
    cut->last = size - to;
    return to;
}
