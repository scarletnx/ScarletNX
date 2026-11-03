/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's pthreads as HLE. A guest thread is a host thread with its own CPU state (bridge_cpu). bionic's pthread
   types on LP64 have the sizes of glibc's on x86-64 and AArch64 (mutex 40, cond 48, rwlock 56, attr 56, sem 32), and
   the guest only passes them to these functions, so most are the host's. What takes a guest function (a thread's
   start, a key's destructor, pthread_once) runs it through the bridge */
#define _GNU_SOURCE
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/hle_signal.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- threads ---- */

typedef struct {
    uint64_t start, arg;
    uint64_t mask; /* the creator's guest signal mask, which the thread starts with */
} Start;

/* the keys with a guest destructor, which a guest thread runs as it ends, as bionic does */
enum { MAX_KEYS = 1024 };
static uint64_t destructors[MAX_KEYS];

static void run_destructors(void) {
    /* as POSIX: up to PTHREAD_DESTRUCTOR_ITERATIONS rounds while a value remains */
    for (int round = 0; round < 4; ++round) {
        bool any = false;
        for (unsigned k = 0; k < MAX_KEYS; ++k) {
            uint64_t d = __atomic_load_n(&destructors[k], __ATOMIC_ACQUIRE);
            if (!d) continue;
            void* v = pthread_getspecific((pthread_key_t)k);
            if (!v) continue;
            pthread_setspecific((pthread_key_t)k, NULL);
            uint64_t args[1] = {(uint64_t)(uintptr_t)v};
            bridge_call(d, args, 1);
            any = true;
        }
        if (!any) break;
    }
}

static void* thread_main(void* p) {
    Start s = *(Start*)p;
    free(p);
    signals_inherit(s.mask);
    uint64_t args[1] = {s.arg};
    uint64_t result = bridge_call(s.start, args, 1);
    run_destructors();
    bridge_thread_end();
    return (void*)(uintptr_t)result;
}

static void g_pthread_create(CPU* c, void* data) {
    (void)data;
    Start* s = malloc(sizeof *s);
    if (!s) {
        X(0) = EAGAIN;
        return;
    }
    s->start = X(2);
    s->arg = X(3);
    s->mask = signals_mask();
    pthread_t t;
    /* the host thread's own stack runs host code only: the guest's is the bridge's, so the attribute's size is the
       host's least */
    pthread_attr_t attr;
    pthread_attr_t* a = PTR(1);
    if (a) {
        memcpy(&attr, a, sizeof attr);
        pthread_attr_setstacksize(&attr, 1u << 20);
    }
    int r = pthread_create(&t, a ? &attr : NULL, thread_main, s);
    if (r) free(s);
    else memcpy(PTR(0), &t, sizeof t);
    X(0) = (uint64_t)r;
}

static void g_pthread_exit(CPU* c, void* data) {
    (void)data;
    run_destructors();
    void* v = PTR(0);
    bridge_thread_end();
    pthread_exit(v);
}

/* the attributes of a thread: the calling thread's with its guest stack, which a garbage collector scans for roots */
static void g_pthread_getattr_np(CPU* c, void* data) {
    (void)data;
    pthread_attr_t* attr = PTR(1);
    int r = pthread_getattr_np((pthread_t)X(0), attr);
    if (!r && pthread_equal((pthread_t)X(0), pthread_self())) {
        uint64_t base;
        size_t size;
        bridge_stack(&base, &size);
        pthread_attr_setstack(attr, (void*)(uintptr_t)base, size);
    }
    X(0) = (uint64_t)r;
}

/* ---- keys ---- */

static void g_pthread_key_create(CPU* c, void* data) {
    (void)data;
    pthread_key_t k;
    int r = pthread_key_create(&k, NULL);
    if (!r && k >= MAX_KEYS) {
        pthread_key_delete(k);
        r = EAGAIN;
    }
    if (!r) {
        __atomic_store_n(&destructors[k], X(1), __ATOMIC_RELEASE);
        int32_t key = (int32_t)k;
        memcpy(PTR(0), &key, sizeof key);
    }
    X(0) = (uint64_t)r;
}
static void g_pthread_key_delete(CPU* c, void* data) {
    (void)data;
    if (X(0) < MAX_KEYS) __atomic_store_n(&destructors[X(0)], 0, __ATOMIC_RELEASE);
    X(0) = (uint64_t)pthread_key_delete((pthread_key_t)X(0));
}

/* ---- pthread_once: bionic's pthread_once_t is an int, 0 before. 1 runs it, 2 done ---- */

static void g_pthread_once(CPU* c, void* data) {
    (void)data;
    int* once = PTR(0);
    int expected = 0;
    if (__atomic_compare_exchange_n(once, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        bridge_call(X(1), NULL, 0);
        __atomic_store_n(once, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != 2) sched_yield();
    }
    X(0) = 0;
}

#define P(name) {#name, hle_pass, (void*)name}

const HleSymbol hle_thread[] = {
    {"pthread_create", g_pthread_create, NULL},
    {"pthread_exit", g_pthread_exit, NULL},
    {"pthread_getattr_np", g_pthread_getattr_np, NULL},
    {"pthread_key_create", g_pthread_key_create, NULL},
    {"pthread_key_delete", g_pthread_key_delete, NULL},
    {"pthread_once", g_pthread_once, NULL},
    P(pthread_attr_destroy), P(pthread_attr_getguardsize), P(pthread_attr_getstack), P(pthread_attr_init),
    P(pthread_attr_setdetachstate), P(pthread_attr_setstacksize), P(pthread_cond_broadcast), P(pthread_cond_destroy),
    P(pthread_cond_init), P(pthread_cond_signal), P(pthread_cond_timedwait), P(pthread_cond_wait),
    P(pthread_condattr_destroy), P(pthread_condattr_init), P(pthread_condattr_setclock), P(pthread_detach),
    P(pthread_equal), P(pthread_getspecific), P(pthread_join), P(pthread_mutex_destroy), P(pthread_mutex_init),
    P(pthread_mutex_lock), P(pthread_mutex_trylock), P(pthread_mutex_unlock), P(pthread_mutexattr_destroy),
    P(pthread_mutexattr_init), P(pthread_mutexattr_settype), P(pthread_rwlock_destroy), P(pthread_rwlock_init),
    P(pthread_rwlock_rdlock), P(pthread_rwlock_unlock), P(pthread_rwlock_wrlock), P(pthread_self),
    P(pthread_setname_np), P(pthread_setschedparam), P(pthread_setspecific),
    {"", NULL, NULL},
};
