/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the log's lines on the process's resources compute, apart from the reads, so a test drives it: main.c's loop
   writes them at log level 2. The threads with the most time on the processors in a window, from snx_threads */
#ifndef SNX_RESOURCES_H
#define SNX_RESOURCES_H
#include "common/utilities/common.h"
#include <stddef.h>

#define THREADS_SEEN 512 /* the threads a window keeps the time of */
typedef struct {
    struct {
        SnxU64 id, ns;
    } seen[THREADS_SEEN]; /* each thread's time on the processors at the window's start */
    unsigned n;
    int started;
    SnxU64 at_ns; /* the window's start */
} ThreadsWindow;
/* the "threads: " line of the window that ends at now_ns into line: the three threads with the most time on the
   processors in it, by name ("?" for a thread without one), of the n that the system listed, whose first
   THREADS_SEEN are in now. A thread the window did not have started in it, so all its time is the window's, unless
   the window was full; one with less time than the window had is a new thread that took an ended one's id. The next
   window starts at now_ns. 0 at the first call, which only starts one */
int threads_line_text(ThreadsWindow* w, const SnxThreadInfo* now, unsigned n, SnxU64 now_ns, char* line, size_t size);
#endif
