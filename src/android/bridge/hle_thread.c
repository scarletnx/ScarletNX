/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's pthreads as HLE. A guest thread is a host thread with its own CPU state (bridge_cpu). Its attributes are
   bionic's layout (below), its keys, pthread_once, join and detach the host's. The mutexes, condition variables,
   rwlocks, spinlocks, barriers and semaphores are bionic's own layout too (hle_sync.c). What takes a guest function
   (a thread's start, a key's destructor, pthread_once) runs it through the bridge */
#if defined(__linux__)
#define _GNU_SOURCE /* pthread_getattr_np, pthread_setname_np */
#endif
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/hle_signal.h"
#include "common/utilities/endian.h"
#include "gnuabi/abi/errno/errno.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

/* ---- pthread_attr_t in bionic's LP64 layout (bits/pthread_types.h): flags (u32), stack base, stack size, guard
   size, sched_policy (i32), sched_priority (i32), 16 reserved bytes. Only these functions and pthread_create read it,
   on every host: glibc's own is another layout, and libthr's and winpthreads' a pointer ---- */
enum {
    BA_FLAGS = 0, BA_STACK = 8, BA_STACK_SIZE = 16, BA_GUARD = 24, BA_POLICY = 32, BA_PRIORITY = 36, BA_SIZE = 56,
    BA_DETACHED = 1, BA_EXPLICIT_SCHED = 4, /* flag bits */
    B_CREATE_DETACHED = 1, B_EXPLICIT_SCHED = 1, B_SCOPE_SYSTEM = 0, B_SCOPE_PROCESS = 1, B_SCHED_RR = 2
};
/* the host stack of a thread the bridge starts: it runs host code only, the guest's own stack is the bridge's */
enum { HOST_STACK = 1 << 20 };

static unsigned char* attr_of(CPU* c, unsigned n) { return (unsigned char*)(uintptr_t)c->x[n]; }
static void flag(unsigned char* a, uint32_t bit, bool on) {
    uint32_t flags = snx_ld_le32(a + BA_FLAGS);
    snx_st_le32(a + BA_FLAGS, on ? flags | bit : flags & ~bit);
}
static void g_attr_init(CPU* c, void* data) {
    (void)data;
    unsigned char a[BA_SIZE] = {0};
    snx_st_le64(a + BA_STACK_SIZE, 1u << 20); /* bionic's default stack: 1 MiB */
    snx_st_le64(a + BA_GUARD, 4096);
    memcpy(PTR(0), a, sizeof a);
    X(0) = 0;
}
static void g_attr_destroy(CPU* c, void* data) {
    (void)data;
    X(0) = 0;
}
static void g_attr_setdetachstate(CPU* c, void* data) {
    (void)data;
    if (X(1) > B_CREATE_DETACHED) {
        X(0) = GNU_EINVAL;
        return;
    }
    flag(attr_of(c, 0), BA_DETACHED, X(1) == B_CREATE_DETACHED);
    X(0) = 0;
}
static void g_attr_getdetachstate(CPU* c, void* data) {
    (void)data;
    snx_st_le32(PTR(1), snx_ld_le32(attr_of(c, 0) + BA_FLAGS) & BA_DETACHED ? B_CREATE_DETACHED : 0);
    X(0) = 0;
}
static void g_attr_setstacksize(CPU* c, void* data) {
    (void)data;
    if (X(1) < 16384) { /* PTHREAD_STACK_MIN */
        X(0) = GNU_EINVAL;
        return;
    }
    snx_st_le64(attr_of(c, 0) + BA_STACK_SIZE, X(1));
    X(0) = 0;
}
static void g_attr_getstacksize(CPU* c, void* data) {
    (void)data;
    snx_st_le64(PTR(1), snx_ld_le64(attr_of(c, 0) + BA_STACK_SIZE));
    X(0) = 0;
}
static void g_attr_setstack(CPU* c, void* data) {
    (void)data;
    if (X(2) < 16384 || (X(1) & 15)) {
        X(0) = GNU_EINVAL;
        return;
    }
    snx_st_le64(attr_of(c, 0) + BA_STACK, X(1));
    snx_st_le64(attr_of(c, 0) + BA_STACK_SIZE, X(2));
    X(0) = 0;
}
static void g_attr_getstack(CPU* c, void* data) {
    (void)data;
    const unsigned char* a = attr_of(c, 0);
    snx_st_le64(PTR(1), snx_ld_le64(a + BA_STACK));
    snx_st_le64(PTR(2), snx_ld_le64(a + BA_STACK_SIZE));
    X(0) = 0;
}
static void g_attr_setguardsize(CPU* c, void* data) {
    (void)data;
    snx_st_le64(attr_of(c, 0) + BA_GUARD, X(1));
    X(0) = 0;
}
static void g_attr_getguardsize(CPU* c, void* data) {
    (void)data;
    snx_st_le64(PTR(1), snx_ld_le64(attr_of(c, 0) + BA_GUARD));
    X(0) = 0;
}
static void g_attr_setschedpolicy(CPU* c, void* data) {
    (void)data;
    if (X(1) > 5 || X(1) == 4) { /* SCHED_OTHER, FIFO, RR, BATCH, IDLE */
        X(0) = GNU_EINVAL;
        return;
    }
    snx_st_le32(attr_of(c, 0) + BA_POLICY, (uint32_t)X(1));
    X(0) = 0;
}
static void g_attr_getschedpolicy(CPU* c, void* data) {
    (void)data;
    snx_st_le32(PTR(1), snx_ld_le32(attr_of(c, 0) + BA_POLICY));
    X(0) = 0;
}
/* struct sched_param: an int */
static void g_attr_setschedparam(CPU* c, void* data) {
    (void)data;
    snx_st_le32(attr_of(c, 0) + BA_PRIORITY, snx_ld_le32(PTR(1)));
    X(0) = 0;
}
static void g_attr_getschedparam(CPU* c, void* data) {
    (void)data;
    snx_st_le32(PTR(1), snx_ld_le32(attr_of(c, 0) + BA_PRIORITY));
    X(0) = 0;
}
static void g_attr_setinheritsched(CPU* c, void* data) {
    (void)data;
    if (X(1) > B_EXPLICIT_SCHED) {
        X(0) = GNU_EINVAL;
        return;
    }
    flag(attr_of(c, 0), BA_EXPLICIT_SCHED, X(1) == B_EXPLICIT_SCHED);
    X(0) = 0;
}
static void g_attr_getinheritsched(CPU* c, void* data) {
    (void)data;
    snx_st_le32(PTR(1), snx_ld_le32(attr_of(c, 0) + BA_FLAGS) & BA_EXPLICIT_SCHED ? B_EXPLICIT_SCHED : 0);
    X(0) = 0;
}
static void g_attr_setscope(CPU* c, void* data) {
    (void)data;
    X(0) = X(1) == B_SCOPE_SYSTEM ? 0 : X(1) == B_SCOPE_PROCESS ? GNU_ENOTSUP : GNU_EINVAL;
}
static void g_attr_getscope(CPU* c, void* data) {
    (void)data;
    snx_st_le32(PTR(1), B_SCOPE_SYSTEM);
    X(0) = 0;
}
/* the host's attributes of a thread the guest creates with a (bionic's): its detach state. The thread runs on the
   guest stack the bridge gives it (bridge_cpu), and its host stack holds host frames alone */
static void host_attributes(uint64_t guest, pthread_attr_t* out) {
    pthread_attr_init(out);
    pthread_attr_setstacksize(out, HOST_STACK);
    if (guest && (snx_ld_le32((const void*)(uintptr_t)guest) & BA_DETACHED))
        pthread_attr_setdetachstate(out, PTHREAD_CREATE_DETACHED);
}
/* the stack of another thread, the host's own: Linux's pthread_getattr_np, FreeBSD's pthread_attr_get_np, macOS's
   pthread_get_stackaddr_np (the top), Windows' none */
static void host_stack_of(pthread_t thread, uint64_t* base, uint64_t* size) {
    *base = *size = 0;
#if defined(__linux__) || defined(__FreeBSD__)
    pthread_attr_t a;
    void* low;
    size_t n;
#if defined(__linux__)
    if (pthread_getattr_np(thread, &a)) return;
#else
    pthread_attr_init(&a);
    if (pthread_attr_get_np(thread, &a)) {
        pthread_attr_destroy(&a);
        return;
    }
#endif
    if (!pthread_attr_getstack(&a, &low, &n)) {
        *base = (uint64_t)(uintptr_t)low;
        *size = n;
    }
    pthread_attr_destroy(&a);
#elif defined(__APPLE__)
    size_t n = pthread_get_stacksize_np(thread);
    *base = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(thread) - n;
    *size = n;
#else
    (void)thread;
#endif
}

#if !defined(__linux__)
static void g_getspecific(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)pthread_getspecific((pthread_key_t)(uint32_t)X(0));
}
static void g_setspecific(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)pthread_setspecific((pthread_key_t)(uint32_t)X(0), PTR(1));
}
/* pthread_setname_np(thread, name): macOS names the calling thread alone */
static void g_setname(CPU* c, void* data) {
    (void)data;
#if defined(__APPLE__)
    X(0) = pthread_equal((pthread_t)X(0), pthread_self()) ? (uint64_t)pthread_setname_np(STR(1)) : 0;
#elif defined(__FreeBSD__)
    pthread_set_name_np((pthread_t)X(0), STR(1));
    X(0) = 0;
#else
    X(0) = (uint64_t)pthread_setname_np((pthread_t)X(0), STR(1));
#endif
}
/* pthread_setschedparam: an app's priorities are hints, which the host's scheduler (Linux's numbers, other hosts'
   ranges) doesn't take: success, and no change */
static void g_setschedparam(CPU* c, void* data) {
    (void)data;
    X(0) = 0;
}
#endif

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
    host_attributes(X(1), &attr);
    int r = pthread_create(&t, &attr, thread_main, s);
    pthread_attr_destroy(&attr);
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

/* the attributes of a thread in bionic's layout: the calling thread's with its guest stack, which a garbage
   collector scans for roots, another's with its host stack */
static void g_pthread_getattr_np(CPU* c, void* data) {
    (void)data;
    uint64_t base = 0, size = 0;
    if (pthread_equal((pthread_t)X(0), pthread_self())) {
        size_t n;
        bridge_stack(&base, &n);
        size = n;
    } else
        host_stack_of((pthread_t)X(0), &base, &size);
    unsigned char a[BA_SIZE] = {0};
    snx_st_le64(a + BA_STACK, base);
    snx_st_le64(a + BA_STACK_SIZE, size);
    snx_st_le64(a + BA_GUARD, 4096);
    memcpy(PTR(1), a, sizeof a);
    X(0) = 0;
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

#define P(name)          {#name, hle_pass, (void*)name}
#define A(name, adapter) {#name, adapter, NULL}

const HleSymbol hle_thread[] = {
    {"pthread_create", g_pthread_create, NULL},
    {"pthread_exit", g_pthread_exit, NULL},
    {"pthread_getattr_np", g_pthread_getattr_np, NULL},
    {"pthread_key_create", g_pthread_key_create, NULL},
    {"pthread_key_delete", g_pthread_key_delete, NULL},
    {"pthread_once", g_pthread_once, NULL},
    P(pthread_detach), P(pthread_equal), P(pthread_join), P(pthread_self),
    A(pthread_attr_init, g_attr_init), A(pthread_attr_destroy, g_attr_destroy),
    A(pthread_attr_setdetachstate, g_attr_setdetachstate), A(pthread_attr_getdetachstate, g_attr_getdetachstate),
    A(pthread_attr_setstacksize, g_attr_setstacksize), A(pthread_attr_getstacksize, g_attr_getstacksize),
    A(pthread_attr_setstack, g_attr_setstack), A(pthread_attr_getstack, g_attr_getstack),
    A(pthread_attr_setguardsize, g_attr_setguardsize), A(pthread_attr_getguardsize, g_attr_getguardsize),
    A(pthread_attr_setschedpolicy, g_attr_setschedpolicy), A(pthread_attr_getschedpolicy, g_attr_getschedpolicy),
    A(pthread_attr_setschedparam, g_attr_setschedparam), A(pthread_attr_getschedparam, g_attr_getschedparam),
    A(pthread_attr_setinheritsched, g_attr_setinheritsched),
    A(pthread_attr_getinheritsched, g_attr_getinheritsched), A(pthread_attr_setscope, g_attr_setscope),
    A(pthread_attr_getscope, g_attr_getscope),
#if defined(__linux__)
    P(pthread_getspecific), P(pthread_setname_np), P(pthread_setschedparam), P(pthread_setspecific),
#else
    A(pthread_getspecific, g_getspecific), A(pthread_setname_np, g_setname),
    A(pthread_setschedparam, g_setschedparam), A(pthread_setspecific, g_setspecific),
#endif
    {"", NULL, NULL},
};
