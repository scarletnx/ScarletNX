/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Preloaded by the jemalloc test (tests/jemalloc.cmake): a pthread_cond_init that allocates before it does its work,
   as the AppImage's cross-libc-dlopen.so does. A malloc whose first call initialises a condition variable enters
   itself here and waits on its own lock */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>

/* Where the allocation goes: a pointer a compiler cannot see unused, or it drops the malloc and free together */
static void* volatile g_jemalloc_preload_kept;

int pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    union {
        void* object;
        int (*function)(pthread_cond_t*, const pthread_condattr_t*);
    } next;
    g_jemalloc_preload_kept = malloc(1u << 20);
    free(g_jemalloc_preload_kept);
    next.object = dlsym(RTLD_NEXT, "pthread_cond_init");
    return next.function != NULL ? next.function(cond, attr) : 0;
}
