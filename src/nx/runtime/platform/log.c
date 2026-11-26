/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The log (<game>.log in the state folder, the runs before in logs/ beside it, and stderr) and fatal errors */
#include "nx/runtime/platform/log.h"
#include "common/utilities/common.h"
#include "common/utilities/run_log.h"
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
#include <stdatomic.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

_Thread_local PlatTls t_plat;

#define LINE_MAX_ 2048
#define EARLY_MAX 4096

/* Initialised statically: threads log from the first moments of the run, and a lazy init would race */
static PMutex g_log_lock = PMUTEX_INIT;
/* Everything below is written under g_log_lock. The few fields crash mode reads without it are accessed atomically */
static FILE* _Atomic g_log; /* atomic for crash mode's reads */
#if !defined(_WIN32)
static _Atomic int g_log_fd = -1; /* g_log's descriptor, for crash mode's write() */
#endif
static int g_log_gave_up;  /* the log could not be created: the console has it all */
static char g_early[EARLY_MAX]; /* lines from before the log exists, written at its top */
static size_t g_early_len;
static int g_early_full; /* a line did not fit: no more are kept, so the saved part has no gap */
static _Atomic u64 g_log_t0; /* plat_ns() of the first line, atomic */
static char g_log_path[1400];
static u64 g_log_size;       /* what the log holds, about (text mode's \r not counted): a cut when past log_max */
static SnxRunLogCut g_log_cut = SNX_RUN_LOG_CUT_INIT; /* the cuts past log_max (run_log.h) */
static _Atomic int g_log_full; /* log_max in MiB from the first cut until log_full_take, atomic */
/* written by every line, apart from the flag every line reads */
static _Alignas(64) _Atomic int g_mid_line; /* the last text written did not end its line, atomic */
static _Alignas(64) _Atomic int g_crashing; /* log_crash_mode was called, atomic */

void log_crash_mode(void) {
    atomic_store_explicit(&g_crashing, 1, memory_order_seq_cst);
    t_plat.log_redirect = NULL;
}

static size_t log_compose(char* out, size_t cap, const char* text);
/* The run before this one: its log (path) into logs/ beside it (run_log.h). What went wrong into why, "" when
   nothing */
static void log_keep_run(const char* path, char* why, size_t size) {
    char dir[1400];
    snprintf(dir, sizeof dir, "%slogs", g_statedir);
    snx_run_log_keep(path, dir, why, size);
}

/* The log is made once the game's code module is loaded (g_mod), so a run that never gets that far (a mistyped
   --game, a dump given instead of a game) leaves no log behind */
static void log_create_nl(void) {
    char path[1400], why[1700], out[LINE_MAX_ + 32], *full, *where;
    FILE* file;
    if (atomic_load_explicit(&g_log, memory_order_relaxed) || g_log_gave_up || !g_mod) return;
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
        /* whatever the log level: the launcher's Share Log reads it from the run's output */
        full = snx_absolute(path);
        where = snx_format(SNX_LOG_WRITTEN "%s\n", full ? full : path);
        if (where) {
            g_log_size += log_compose(out, sizeof out, where);
            fputs(out, file);
            fputs(where, stderr);
            fflush(stderr);
        }
        free(where);
        free(full);
        fflush(file);
#if !defined(_WIN32)
        atomic_store_explicit(&g_log_fd, fileno(file), memory_order_release);
#endif
        atomic_store_explicit(&g_log, file, memory_order_release);
    }
    g_early_full = 1; /* either way nothing more is kept */
}

/* the "[seconds] " stamp when text starts a line, then the text, into out */
static size_t log_compose(char* out, size_t cap, const char* text) {
    size_t n = 0, len = strlen(text);
    u64 t0 = atomic_load_explicit(&g_log_t0, memory_order_relaxed);
    if (!atomic_load_explicit(&g_mid_line, memory_order_relaxed)) {
        int w;
        if (!t0) {
            t0 = plat_ns();
            atomic_store_explicit(&g_log_t0, t0 ? t0 : 1, memory_order_relaxed);
        }
        w = snprintf(out, cap, "[%7.2f] ", (double)(plat_ns() - t0) / 1e9);
        n = w > 0 && (size_t)w < cap ? (size_t)w : 0;
    }
    if (len > cap - 1 - n) len = cap - 1 - n;
    memcpy(out + n, text, len);
    n += len;
    out[n] = 0;
    if (len) atomic_store_explicit(&g_mid_line, text[len - 1] != '\n', memory_order_relaxed);
    return n;
}

/* Crash mode: no lock and no stdio buffers (the crashed thread may own either), straight to the descriptors */
static void log_crash_write(const char* text) {
    char out[LINE_MAX_ + 32];
    size_t n = log_compose(out, sizeof out, text);
#if defined(_WIN32)
    /* the report runs in an exception filter, on the thread that crashed; stdio's locks are recursive */
    FILE* file = atomic_load_explicit(&g_log, memory_order_acquire);
    (void)n;
    if (file) {
        fputs(out, file);
        fflush(file);
    }
    fputs(text, stderr);
    fflush(stderr);
#else
    int fd = atomic_load_explicit(&g_log_fd, memory_order_acquire);
    ssize_t r;
    if (fd >= 0) r = write(fd, out, n);
    r = write(2, text, strlen(text));
    (void)r;
#endif
}

/* The log past log_max (limit bytes), cut in place (run_log.h); the first cut for the frontend's toast. A crash's
   line written while a cut runs may land in what the cut drops. Under g_log_lock */
static void log_cut_nl(long limit) {
    SNX_ZONE_T(z_cut, "plat log cut", "log_max reached");
    char mark[320];
    int before = g_log_cut.cuts;
    fflush(atomic_load_explicit(&g_log, memory_order_relaxed));
    g_log_size = (u64)snx_run_log_cut(g_log_path, limit, atomic_load_explicit(&g_log_t0, memory_order_relaxed),
                                      &g_log_cut, mark, sizeof mark);
    if (g_log_cut.last >= 0) {
        fseek(atomic_load_explicit(&g_log, memory_order_relaxed), 0,
              SEEK_END); /* the descriptor's offset too, which crash mode writes at */
        SNX_PLOT("plat log cut bytes", (double)g_log_cut.last); /* the bytes saved; 0 when the cut failed */
    }
    if (mark[0]) {
        fputs(mark, atomic_load_explicit(&g_log, memory_order_relaxed));
        fputs(mark, stderr);
    } else if (!before && g_log_cut.cuts == 1)
        atomic_store_explicit(&g_log_full, (int)(limit >> 20), memory_order_release);
}

int log_full_take(void) { return atomic_exchange_explicit(&g_log_full, 0, memory_order_acq_rel); }

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
    if (t_plat.log_redirect) {
        fputs(text, t_plat.log_redirect);
        return;
    }
    if (atomic_load_explicit(&g_crashing, memory_order_seq_cst)) {
        log_crash_write(text);
        return;
    }
    snx_tracy_log(lvl, text);
    pm_lock(&g_log_lock);
    log_create_nl();
    n = log_compose(out, sizeof out, text);
    if (atomic_load_explicit(&g_log, memory_order_relaxed)) {
        long limit = (long)atomic_load_explicit((_Atomic int*)&g_settings.log_size_limit, memory_order_relaxed) << 20;
        fputs(out, atomic_load_explicit(&g_log, memory_order_relaxed));
        fflush(atomic_load_explicit(&g_log, memory_order_relaxed));
        g_log_size += n;
        if (g_log_cut.cuts >= 0 && g_log_size > (u64)limit) log_cut_nl(limit);
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

void log_sync(void) {
    if (atomic_load_explicit(&g_crashing, memory_order_seq_cst))
        return; /* crash mode's lines skip stdio, and its locks */
    pm_lock(&g_log_lock);
    if (atomic_load_explicit(&g_log, memory_order_relaxed))
        fflush(atomic_load_explicit(&g_log, memory_order_relaxed)); /* logf_ flushed each line already */
    pm_unlock(&g_log_lock);
    fflush(stdout); /* line buffered, so only a line not ended yet waits in it */
    fflush(stderr);
#if defined(_WIN32)
    {
        /* FlushFileBuffers, outside the lock as below; the log stays open to the end of the run */
        FILE* file = atomic_load_explicit(&g_log, memory_order_acquire);
        if (file) (void)_commit(_fileno(file));
    }
#else
    {
        /* outside the lock, since the disk can take a while. A pipe or a terminal refuses, which is no matter */
        int fd = atomic_load_explicit(&g_log_fd, memory_order_acquire);
        if (fd >= 0) (void)fsync(fd);
        (void)fsync(1);
        (void)fsync(2);
    }
#endif
}

void log_error_line(const char* line) {
    WARN("%s\n", line);
}

/* the fatal error e, its message in it: logged, its panel, and the process's end with status */
[[noreturn]] static void fatal_error(int status, SnxError* e) {
    const char* msg = e->message;
    t_plat.log_redirect = NULL; /* a failure while writing threads.txt belongs in the log */
    logf_(0, "FATAL: %s\n", msg);
    snx_tracy_fatal(msg); /* the message pane says why the trace ends here */
#if !defined(_WIN32)
    /* from the crash handler: a box or a flush could wait on a lock the crashed code holds. The panel's own process
       takes no lock of this one's (win_fatal_crash) */
    if (atomic_load_explicit(&g_crashing, memory_order_seq_cst)) {
        win_fatal_crash(e);
        _exit(status);
    }
#endif
    /* everywhere, since a player started from a desktop entry sees no console and no log. A remake's too: a game
       an app or a shortcut started has no driver to recompile it, and one the driver started recompiles once
       the box is closed */
    /* Close the client so a connected capture writes the trace up to this point. From the crash handler there is
       neither time nor safety for it: _exit above leaves the socket, and the capture writes what it has. */
    if (!atomic_load_explicit(&g_crashing, memory_order_seq_cst)) snx_tracy_shutdown();
    win_fatal_box(e);
    plat_exit_now(status);
}
/* An error of the kind given has, its message fmt's: on the stack, as a crash's (whose thread may have no heap left) */
#define FATAL_ERROR(status, given, fmt)                                                                                \
    do {                                                                                                               \
        SnxError e_ = *(given);                                                                                        \
        va_list ap_;                                                                                                   \
        va_start(ap_, fmt);                                                                                            \
        vsnprintf(e_.message, sizeof e_.message, fmt, ap_);                                                            \
        va_end(ap_);                                                                                                   \
        if (e_.kind == SNX_ERROR_INTERNAL) e_.kind = snx_error_kind_of_message(e_.message);                            \
        fatal_error(status, &e_);                                                                                      \
    } while (0)

void fatal_code(int status, const char* fmt, ...) { FATAL_ERROR(status, &(SnxError){0}, fmt); }
void fatal(const char* fmt, ...) { FATAL_ERROR(1, &(SnxError){0}, fmt); }
void fatal_as(int kind, const char* fmt, ...) { FATAL_ERROR(1, &(SnxError){.kind = kind}, fmt); }
void fatal_given(int status, const SnxError* given, const char* fmt, ...) { FATAL_ERROR(status, given, fmt); }
