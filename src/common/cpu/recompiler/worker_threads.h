/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The lock of a recompile's queue of units and the threads that run its worker: the AArch32 AOT
   compilers' (3ds, vita and nds aot/compiler.c), and the library's (library.c) */
#ifndef SNX_RECOMPILER_WORKER_THREADS_H
#define SNX_RECOMPILER_WORKER_THREADS_H
#include "common/cpu/recompiler/compile_thread.h"
#include <stdlib.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

/* a queue's lock */
typedef struct {
#ifdef _WIN32
    CRITICAL_SECTION section;
#else
    pthread_mutex_t mutex;
#endif
} SnxWorkLock;

static inline void snx_work_lock_init(SnxWorkLock* l) {
#ifdef _WIN32
    InitializeCriticalSection(&l->section);
#else
    pthread_mutex_init(&l->mutex, NULL);
#endif
}
static inline void snx_work_lock_destroy(SnxWorkLock* l) {
#ifdef _WIN32
    DeleteCriticalSection(&l->section);
#else
    pthread_mutex_destroy(&l->mutex);
#endif
}
static inline void snx_work_lock(SnxWorkLock* l) {
#ifdef _WIN32
    EnterCriticalSection(&l->section);
#else
    pthread_mutex_lock(&l->mutex);
#endif
}
static inline void snx_work_unlock(SnxWorkLock* l) {
#ifdef _WIN32
    LeaveCriticalSection(&l->section);
#else
    pthread_mutex_unlock(&l->mutex);
#endif
}

typedef struct {
    void (*worker)(void* arg);
    void* arg;
} SnxWorkers;

#ifdef _WIN32
static inline DWORD WINAPI snx_workers_main(LPVOID p) {
    const SnxWorkers* w = (const SnxWorkers*)p;
    w->worker(w->arg);
    return 0;
}
#else
static inline void* snx_workers_main(void* p) {
    const SnxWorkers* w = (const SnxWorkers*)p;
    w->worker(w->arg);
    return NULL;
}
#endif

/* worker(arg) on jobs threads (at least one), the caller's among them, returning once every one has returned. Fewer
   run when the system makes no more threads */
static inline void snx_run_workers(unsigned jobs, void (*worker)(void* arg), void* arg) {
    SnxWorkers w = {worker, arg};
    unsigned created = 0;
#ifdef _WIN32
    HANDLE* threads = (HANDLE*)calloc(jobs ? jobs : 1, sizeof(*threads));
#else
    pthread_t* threads = (pthread_t*)calloc(jobs ? jobs : 1, sizeof(*threads));
#endif
    for (unsigned i = 1; threads && i < jobs; ++i) {
#ifdef _WIN32
        HANDLE h = snx_compile_thread(snx_workers_main, &w);
        if (!h) break;
        threads[created++] = h;
#else
        if (snx_compile_thread(&threads[created], snx_workers_main, &w)) break;
        ++created;
#endif
    }
    worker(arg);
    for (unsigned i = 0; i < created; ++i) {
#ifdef _WIN32
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
    free(threads);
}

#endif
