/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the log's lines on the process's resources compute, apart from the reads, so a test drives it: main.c's loop
   writes them at log level 2. The floors of free memory, each said once, and the threads with the most time on the
   processors in a window, from snx_threads */
#ifndef SNX_RESOURCES_H
#define SNX_RESOURCES_H
#include "common/utilities/common.h"
#include <stddef.h>

/* the floors of the memory the system still gives the process (snx_memory_available), in MiB, from the highest */
#define MEMORY_FLOORS 4
extern const unsigned g_memory_floors[MEMORY_FLOORS];
/* the lowest floor that left bytes are under of those from *next on, *next moved past every floor they are under; 0,
   and *next as it was, when they are under none of them. *next starts at 0, so each floor is said once a run, and a
   fall past several at once says the lowest */
unsigned memory_floor_crossed(SnxU64 left, unsigned* next);

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
