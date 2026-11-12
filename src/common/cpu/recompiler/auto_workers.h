/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The threads of a recompile whose count is left open (the setting aot_threads at 0, Auto): they open one at a time,
   as common.h's SnxAotGovernor decides from what the units take while they compile. nx/loader/module_llvm.c and
   module_arm.c run their queue's worker through it; a test gives it readings of its own */
#ifndef SNX_RECOMPILER_AUTO_WORKERS_H
#define SNX_RECOMPILER_AUTO_WORKERS_H
#include "common/cpu/recompiler/compile_thread.h"
#include "common/platform/time.h"
#include "common/utilities/common.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    void (*worker)(void* arg); /* compiles the queue's units until none is left, as on a thread of a fixed count */
    /* the queue's part of a reading, read under its lock: active, done, left (0 once it has failed too) and held */
    void (*state)(void* arg, SnxAotReading* r);
    void* arg;
    unsigned most; /* the threads the cores give */
    /* the system's part of a reading and the words of its pressure signal: NULL for snx_aot_read, a test's own */
    void (*read)(void* arg, SnxAotReading* r, char* pressure, size_t size);
    void (*say)(void* arg, const char* line); /* a line of the control: NULL for stdout */
    int quiet;                                /* no line at all */
    unsigned tick_ms;                         /* 0 for SNX_AOT_TICK_MS, a test's shorter one */
} SnxAutoWorkers;

#ifdef _WIN32
static inline DWORD WINAPI snx_auto_workers_main(LPVOID p) {
    const SnxAutoWorkers* w = (const SnxAutoWorkers*)p;
    w->worker(w->arg);
    return 0;
}
#else
static inline void* snx_auto_workers_main(void* p) {
    const SnxAutoWorkers* w = (const SnxAutoWorkers*)p;
    w->worker(w->arg);
    return NULL;
}
#endif

static inline void snx_auto_workers_say(const SnxAutoWorkers* w, const char* line) {
    if (w->quiet || !line[0]) return;
    if (w->say) {
        w->say(w->arg, line);
        return;
    }
    puts(line);
    fflush(stdout);
}

/* w->worker on 1 thread, then on one more for each tick the control opens one, until no unit is left to take, and
   returns once every thread has. The threads it opened, the first among them. This thread compiles nothing: it is the
   control's; when the system starts no thread at all, it runs the worker itself, and none opens after */
static inline unsigned snx_run_workers_auto(const SnxAutoWorkers* w) {
    SnxAotGovernor g;
    SnxAotReading r = {0};
    char line[768], pressure[256];
    unsigned created = 0, i, tick = w->tick_ms ? w->tick_ms : SNX_AOT_TICK_MS;
    unsigned most = w->most ? w->most : 1u;
#ifdef _WIN32
    HANDLE* threads = (HANDLE*)snx_alloc(most * sizeof(*threads));
#else
    pthread_t* threads = (pthread_t*)snx_alloc(most * sizeof(*threads));
#endif
    if (w->read)
        w->read(w->arg, &r, pressure, sizeof pressure);
    else
        snx_aot_read(&r, pressure, sizeof pressure);
    snx_aot_governor_start(&g, most, r.footprint, pressure, line, sizeof line);
    snx_auto_workers_say(w, line);
    for (;;) {
        int opens = !created; /* the first thread; then the ones a tick opens */
        if (created) {
            /* a tick, cut short once no unit is left: the last threads are only waited for */
            for (i = 0; i < 10; ++i) {
                plat_sleep_ns((int64_t)tick * 100000);
                w->state(w->arg, &r);
                if (!r.left) break;
            }
            if (!r.left) break;
            if (w->read)
                w->read(w->arg, &r, pressure, sizeof pressure);
            else
                snx_aot_read(&r, pressure, sizeof pressure);
            opens = snx_aot_governor_tick(&g, &r, pressure, line, sizeof line);
            snx_auto_workers_say(w, line);
        }
        if (!opens) continue;
#ifdef _WIN32
        threads[created] = snx_compile_thread(snx_auto_workers_main, (void*)w);
        if (threads[created]) {
            ++created;
            continue;
        }
#else
        errno = snx_compile_thread(&threads[created], snx_auto_workers_main, (void*)w); /* pthread's error, or 0 */
        if (!errno) {
            ++created;
            continue;
        }
#endif
        /* the system starts no more threads: the ones open go on, and the control has nothing left to open */
        snx_os_error(pressure, sizeof pressure);
        if (!created) {
            snprintf(line, sizeof line,
                     "aot_threads=0 (Auto): the system starts no thread (%s): the units compile on this one", pressure);
            snx_auto_workers_say(w, line);
            w->worker(w->arg);
            break;
        }
        snprintf(line, sizeof line, "aot_threads=0 (Auto): stays on %u thread%s of %u: the system starts no more (%s)",
                 created, created == 1u ? "" : "s", g.most, pressure);
        snx_auto_workers_say(w, line);
        g.threads = g.most = created;
    }
    for (i = 0; i < created; ++i) {
#ifdef _WIN32
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
    free(threads);
    return created ? created : 1u;
}

#endif
