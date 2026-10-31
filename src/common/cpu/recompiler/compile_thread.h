/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A thread that runs LLVM's passes over a unit (workers.h, nx/loader/module_llvm.c and module_arm.c, vita/aot, the
   JIT service): LLVM recurses with the function it works on, a frame per block in MemoryDependence's walk of a load's
   predecessors, and a game's biggest functions overflowed the stack a thread gets by default (1 MiB on Android and
   Windows, 8 MiB from glibc). The stack is a reservation: a thread uses the pages it touches */
#ifndef SNX_RECOMPILER_COMPILE_THREAD_H
#define SNX_RECOMPILER_COMPILE_THREAD_H
#include <stddef.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#define SNX_RECOMPILER_STACK ((size_t)64 << 20)

#ifdef _WIN32
/* the thread, NULL when it did not start (GetLastError says why) */
static inline HANDLE snx_compile_thread(LPTHREAD_START_ROUTINE fn, void* arg) {
    return CreateThread(NULL, SNX_RECOMPILER_STACK, fn, arg, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
}
#else
/* 0, or the error pthread gives */
static inline int snx_compile_thread(pthread_t* t, void* (*fn)(void*), void* arg) {
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc) return rc;
    rc = pthread_attr_setstacksize(&attr, SNX_RECOMPILER_STACK);
    if (!rc) rc = pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}
#endif

#endif
