/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's log and its fatal error, both in log.c */
#ifndef SNX_LOG_H
#define SNX_LOG_H
#include <stdio.h>
#include <stdatomic.h>

/* the .ini's log key: 1 normal, 2 also the LOGV lines (and every line on stderr), 3 the profiler's counters as well
   (gpuprof.c, and the CPU's per resource in renderer.c) */
#include "snx_settings.h" /* g_settings.log_level, the .ini's log */

/* lvl 0 is a warning, 1 a normal line, 2 a verbose one. Checked like printf */
[[gnu::format(printf, 2, 3)]] void logf_(int lvl, const char* fmt, ...);
#define WARN(...) logf_(0, __VA_ARGS__)
#define LOG(...)  logf_(1, __VA_ARGS__)
#define LOGV(...)                                                                                                      \
    do {                                                                                                               \
        if (g_settings.log_level >= 2) logf_(2, __VA_ARGS__);                                                          \
    } while (0)

/* Say something at most n times per call site, for a message a game could otherwise trigger every frame. Each site
   counts in its own static int (relaxed: only the count matters), and stops adding once past the limit */
#define SNX_LOG_AT_MOST_(said, n, lvl, ...)                                                                            \
    do {                                                                                                               \
        static _Atomic int said;                                                                                       \
        if (atomic_load_explicit(&said, memory_order_relaxed) < (n) &&                                                 \
            atomic_fetch_add_explicit(&said, 1, memory_order_relaxed) < (n))                                           \
            logf_(lvl, __VA_ARGS__);                                                                                   \
    } while (0)
#define LOG_LIMIT(n, ...)  SNX_LOG_AT_MOST_(log_limit_said_, n, 1, __VA_ARGS__)
#define WARN_LIMIT(n, ...) SNX_LOG_AT_MOST_(warn_limit_said_, n, 0, __VA_ARGS__)

/* The log and the console written through to the file system, for a moment the process may not live past (a low
   memory warning). Each line is in the file at its end already (logf_ flushes it, and stderr has no buffer): this
   also gives stdout's line not ended yet, and asks the system to put the files on the disk. Any thread */
void log_sync(void);

/* snx_error's sink (common.h) for the runtime: the line as a warning, in the log and on stderr */
void log_error_line(const char* line);

/* Logs "FATAL: ..." and ends the process with status, after the error box where there is one */
[[gnu::format(printf, 2, 3), noreturn]] void fatal_code(int status, const char* fmt, ...);
/* fatal_code with status 1 */
[[gnu::format(printf, 1, 2), noreturn]] void fatal(const char* fmt, ...);

/* log.c's thread state, this thread's */
typedef struct {
    /* While set, the calling thread's lines go to this file and nowhere else (the main loop's threads.txt dump). It
       is per thread so the others keep writing the log during the dump */
    FILE* log_redirect;
} PlatTls;
extern _Thread_local PlatTls t_plat;

/* log_max in MiB once, after the log first went past it and was cut (the oldest lines after the run's start gone), for
   the frontend to say over the game; 0 before that and after it was taken */
int log_full_take(void);

/* Called by the crash handler before its report: from now on lines bypass the log's lock and stdio buffering, which
   the crashed code may hold, and this thread's redirect is dropped */
void log_crash_mode(void);

#endif
