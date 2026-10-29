/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The log (<game>.log in the state folder, the runs before in logs/ beside it, and stderr) and fatal errors */
#include "nx/runtime/platform/log.h"
#include "common/utilities/common.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

_Thread_local FILE* tls_log_redirect;

#define LINE_MAX_ 2048
#define EARLY_MAX 4096
#define LOG_RUNS  5          /* the runs' logs kept: this one's in <game>.log, the ones before in logs/ */
#define LOG_HEAD  (64 << 10) /* what a cut keeps of the run's start (log_cut_nl), to a line's end */

/* Initialised statically: threads log from the first moments of the run, and a lazy init would race */
static PMutex g_log_lock = PMUTEX_INIT;
/* Everything below is written under g_log_lock. The few fields crash mode reads without it are accessed atomically */
static FILE* g_log;        /* atomic for crash mode's reads */
#if !defined(_WIN32)
static int g_log_fd = -1; /* g_log's descriptor, for crash mode's write() */
#endif
static int g_log_gave_up;  /* the log could not be created: the console has it all */
static char g_early[EARLY_MAX]; /* lines from before the log exists, written at its top */
static size_t g_early_len;
static int g_early_full; /* a line did not fit: no more are kept, so the saved part has no gap */
static u64 g_log_t0;     /* plat_ns() of the first line, atomic */
static char g_log_path[1400];
static u64 g_log_size;       /* what the log holds, about (text mode's \r not counted): a cut when past log_max */
static int g_log_cuts;       /* the cuts made; one that failed stops them */
static long g_log_head = -1; /* the run's start a cut keeps, from the first: the line saying what went comes after it */
static int g_log_full;       /* log_max in MiB from the first cut until log_full_take, atomic */
/* written by every line, apart from the flag every line reads */
static _Alignas(64) int g_mid_line; /* the last text written did not end its line, atomic */
static _Alignas(64) int g_crashing; /* log_crash_mode was called, atomic */

void log_crash_mode(void) {
    __atomic_store_n(&g_crashing, 1, __ATOMIC_SEQ_CST);
    tls_log_redirect = NULL;
}

static size_t log_compose(char* out, size_t cap, const char* text);
/* a name log_keep_run gives a run's log: YYYY-MM-DD_HH-MM-SS, -N when two share a second, .log */
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
/* The run before this one: its log (path) into logs/ beside it, named by when it was last written, and logs/ down to
   the LOG_RUNS - 1 newest. What went wrong into why, a line for the new log, "" when nothing did */
static void log_keep_run(const char* path, char* why, size_t size) {
    char dir[1400], dated[1500], stamp[32], **names = NULL;
    SnxU64 when = snx_file_time(path);
    time_t t = (time_t)when;
    struct tm tm;
    size_t count = 0, i, runs = 0;
    int n;
    why[0] = 0;
    if (!when) return; /* the first run */
    snprintf(dir, sizeof dir, "%slogs", g_statedir);
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
        if (!run_log_name(names[i]) || ++runs < LOG_RUNS) continue;
        snprintf(dated, sizeof dated, "%s/%s", dir, names[i]);
        if (remove(dated) && !why[0])
            snprintf(why, size, "log: cannot delete %s (%s), past the %d runs' logs kept\n", dated, strerror(errno),
                     LOG_RUNS);
    }
    snx_free_strings(names, count);
}

/* The log is made once the game's code module is loaded (g_mod), so a run that never gets that far (a mistyped
   --game, a dump given instead of a game) leaves no log behind */
static void log_create_nl(void) {
    char path[1400], why[1700], out[LINE_MAX_ + 32];
    FILE* file;
    if (g_log || g_log_gave_up || !g_mod) return;
    snprintf(path, sizeof path, "%s%s.log", g_statedir, g_exename);
    log_keep_run(path, why, sizeof why);
    file = fopen(path, "w");
    if (!file) {
        g_log_gave_up = 1;
        fprintf(stderr, "Could not create the log %s, so it only goes to the console.\n", path);
    } else {
        snprintf(g_log_path, sizeof g_log_path, "%s", path);
        if (g_early_len) fwrite(g_early, 1, g_early_len, file);
        g_log_size = g_early_len;
        if (why[0]) {
            g_log_size += log_compose(out, sizeof out, why);
            fputs(out, file);
            fputs(why, stderr);
        }
        fflush(file);
#if !defined(_WIN32)
        __atomic_store_n(&g_log_fd, fileno(file), __ATOMIC_RELEASE);
#endif
        __atomic_store_n(&g_log, file, __ATOMIC_RELEASE);
    }
    g_early_full = 1; /* either way nothing more is kept */
}

/* the "[seconds] " stamp when text starts a line, then the text, into out */
static size_t log_compose(char* out, size_t cap, const char* text) {
    size_t n = 0, len = strlen(text);
    u64 t0 = __atomic_load_n(&g_log_t0, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&g_mid_line, __ATOMIC_RELAXED)) {
        int w;
        if (!t0) {
            t0 = plat_ns();
            __atomic_store_n(&g_log_t0, t0 ? t0 : 1, __ATOMIC_RELAXED);
        }
        w = snprintf(out, cap, "[%7.2f] ", (double)(plat_ns() - t0) / 1e9);
        n = w > 0 && (size_t)w < cap ? (size_t)w : 0;
    }
    if (len > cap - 1 - n) len = cap - 1 - n;
    memcpy(out + n, text, len);
    n += len;
    out[n] = 0;
    if (len) __atomic_store_n(&g_mid_line, text[len - 1] != '\n', __ATOMIC_RELAXED);
    return n;
}

/* Crash mode: no lock and no stdio buffers (the crashed thread may own either), straight to the descriptors */
static void log_crash_write(const char* text) {
    char out[LINE_MAX_ + 32];
    size_t n = log_compose(out, sizeof out, text);
#if defined(_WIN32)
    /* the report runs in an exception filter, on the thread that crashed; stdio's locks are recursive */
    FILE* file = __atomic_load_n(&g_log, __ATOMIC_ACQUIRE);
    (void)n;
    if (file) {
        fputs(out, file);
        fflush(file);
    }
    fputs(text, stderr);
    fflush(stderr);
#else
    int fd = __atomic_load_n(&g_log_fd, __ATOMIC_ACQUIRE);
    ssize_t r;
    if (fd >= 0) r = write(fd, out, n);
    r = write(2, text, strlen(text));
    (void)r;
#endif
}

/* The log past log_max (limit bytes): the run's start (its first LOG_HEAD bytes, to a line's end, the same at each
   cut), a line saying what went, and the last lines, half the limit in all, so a long run's log ends where it does. Moved in place through
   a second handle, in binary (Windows' text mode would move a "\r\n" by halves); the first cut for the frontend's
   toast. A crash's line written while a cut runs may land in what the cut drops. Under g_log_lock */
static void log_cut_nl(long limit) {
    static char buf[1 << 16];
#if defined(_WIN32)
    static const char eol[] = "\r\n";
#else
    static const char eol[] = "\n";
#endif
    char mark[320];
    long size, head = 0, from, to, n;
    size_t got, i;
    int moved;
    u64 t0 = __atomic_load_n(&g_log_t0, __ATOMIC_RELAXED);
    FILE* f;
    fflush(g_log);
    f = fopen(g_log_path, "r+b");
    if (!f || fseek(f, 0, SEEK_END) || (size = ftell(f)) <= limit / 2) {
        if (!f) {
            g_log_cuts = -1;
            snprintf(mark, sizeof mark, "log: cannot cut %s past log_max=%ld MiB (%s): it grows on\n", g_log_path,
                     limit >> 20, strerror(errno));
            fputs(mark, g_log);
            fputs(mark, stderr);
        } else
            fclose(f);
        g_log_size = 0;
        return;
    }
    if (g_log_head < 0) {
        rewind(f);
        got = fread(buf, 1, (size_t)(size < LOG_HEAD ? size : LOG_HEAD), f);
        for (i = got; i > 0; --i)
            if (buf[i - 1] == '\n') break;
        g_log_head = (long)i;
    }
    head = g_log_head;
    from = size - (limit / 2 - head);
    if (!fseek(f, from, SEEK_SET) && (got = fread(buf, 1, sizeof buf, f)) > 0) { /* to the next line's start */
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
        size_t want = (size_t)(size - from < (long)sizeof buf ? size - from : (long)sizeof buf);
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
        g_log_cuts = -1;
        to = size;
        snprintf(mark, sizeof mark, "log: cutting %s past log_max=%ld MiB failed (%s): it grows on\n", g_log_path,
                 limit >> 20, strerror(errno));
        fputs(mark, stderr);
    }
    fclose(f);
    fseek(g_log, 0, SEEK_END); /* the descriptor's offset too, which crash mode writes at */
    g_log_size = (u64)to;
    if (!moved)
        fputs(mark, g_log);
    else if (!g_log_cuts++)
        __atomic_store_n(&g_log_full, (int)(limit >> 20), __ATOMIC_RELEASE);
}

int log_full_take(void) { return __atomic_exchange_n(&g_log_full, 0, __ATOMIC_ACQ_REL); }

void logf_(int lvl, const char* fmt, ...) {
    char text[LINE_MAX_], out[LINE_MAX_ + 32];
    va_list ap;
    int w;
    size_t n;
    va_start(ap, fmt);
    w = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    /* a cut line still ends with its newline, so the next one starts on its own */
    if ((size_t)w >= sizeof text) text[sizeof text - 2] = '\n';
    if (tls_log_redirect) {
        fputs(text, tls_log_redirect);
        return;
    }
    if (__atomic_load_n(&g_crashing, __ATOMIC_SEQ_CST)) {
        log_crash_write(text);
        return;
    }
    snx_tracy_log(lvl, text);
    pm_lock(&g_log_lock);
    log_create_nl();
    n = log_compose(out, sizeof out, text);
    if (g_log) {
        long limit = (long)__atomic_load_n(&g_settings.log_size_limit, __ATOMIC_RELAXED) << 20;
        fputs(out, g_log);
        fflush(g_log);
        g_log_size += n;
        if (g_log_cuts >= 0 && g_log_size > (u64)limit) log_cut_nl(limit);
        if (lvl <= 1 || g_settings.log_level >= 2) {
            fputs(text, stderr); /* the console gets the line without its time stamp, which only the log file keeps */
            fflush(stderr);
        }
    } else {
        fputs(text, stderr);
        fflush(stderr);
        if (!g_early_full && !g_log_gave_up) {
            if (n <= sizeof g_early - g_early_len) {
                memcpy(g_early + g_early_len, out, n);
                g_early_len += n;
            } else
                g_early_full = 1;
        }
    }
    pm_unlock(&g_log_lock);
}

void log_error_line(const char* line) {
    WARN("%s\n", line);
}

void fatal(const char* fmt, ...) {
    char msg[LINE_MAX_];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    tls_log_redirect = NULL; /* a failure while writing threads.txt belongs in the log */
    logf_(0, "FATAL: %s\n", msg);
#if !defined(_WIN32)
    /* from the crash handler: a box or a flush could wait on a lock the crashed code holds */
    if (__atomic_load_n(&g_crashing, __ATOMIC_SEQ_CST)) _exit(1);
#endif
    /* everywhere, since a player started from a desktop entry sees no console and no log */
    win_fatal_box(msg);
    plat_exit_now(1);
}
