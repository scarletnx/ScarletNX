/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the recompiles that compile their units on several threads share (library.c, nx/loader/module_llvm.c and
   module_arm.c): the lock of their queue, the threads that run its worker, and a unit's IR kept as a file */
#ifndef SNX_RECOMPILER_WORKERS_H
#define SNX_RECOMPILER_WORKERS_H
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/compile_thread.h"
#include "common/utilities/common.h"
#include <llvm-c/Core.h>
#include <stdlib.h>
#include <string.h>
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

/* the unit's IR as <dir>/<name>.ll: 0, or nonzero when it cannot be written */
static inline int snx_save_ir(SnxIr* ir, const char* dir, const char* name) {
    char *text = snx_ir_text(ir), *file = snx_format("%s.ll", name), *path = snx_path_join(dir, file);
    int rc = snx_write_file(path, text, strlen(text));
    LLVMDisposeMessage(text);
    free(file);
    free(path);
    return rc;
}

#endif
