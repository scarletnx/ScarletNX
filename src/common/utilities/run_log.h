/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A run's log, as the Switch's runtime (nx/runtime/platform/log.c) and the consoles' (game_log.c) keep it: the runs
   before in logs/ beside it, and the cut past log_max that keeps the run's start and its last lines */
#ifndef SNX_RUN_LOG_H
#define SNX_RUN_LOG_H
#include <stddef.h>
#include <stdint.h>

#define SNX_RUN_LOGS     5          /* the runs' logs kept: this one's, the ones before in logs/ */
#define SNX_RUN_LOG_HEAD (64 << 10) /* what a cut keeps of the run's start, to a line's end */

/* The run before this one: its log (path) into dir, named by when it was last written (YYYY-MM-DD_HH-MM-SS.log, -N
   when two share a second), and dir down to the SNX_RUN_LOGS - 1 newest. What went wrong into why, a line for the new
   log, "" when nothing did */
void snx_run_log_keep(const char* path, const char* dir, char* why, size_t size);

/* a log's cuts over its run: head the run's start they keep (-1 before the first), cuts the ones made (-1 once one
   failed, which stops them), last the bytes the last one took out (-1 when it found nothing to cut) */
typedef struct {
    long head;
    int cuts;
    long last;
} SnxRunLogCut;
#define SNX_RUN_LOG_CUT_INIT {-1, 0, -1}

/* The log at path past limit bytes: the run's start (its first SNX_RUN_LOG_HEAD bytes, to a line's end, the same at
   each cut), a line saying what went, and the last lines, half the limit in all, so a long run's log ends where it
   does. Moved in place through a handle of its own, in binary (Windows' text mode would move a "\r\n" by halves); the
   caller's own handle flushed before, and at the file's end after (appending, or moved there). The line's stamp is
   plat_ns() from t0, the log's first line's. The bytes the log holds then: 0 when it held half the limit or less, to be
   counted again from there. A failure's line into said ("" for none), for the caller to write to the log and the
   terminal: the log grows on. Under the caller's lock; one log in a process */
long snx_run_log_cut(const char* path, long limit, uint64_t t0, SnxRunLogCut* cut, char* said, size_t size);
#endif
