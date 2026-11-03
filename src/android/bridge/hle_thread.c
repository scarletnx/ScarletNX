/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's pthreads as HLE. A guest thread is a host thread with its own CPU state (bridge_cpu). bionic's pthread
   types on LP64 have the sizes of glibc's on x86-64 and AArch64 (mutex 40, cond 48, rwlock 56, attr 56, sem 32), and
   the guest only passes them to these functions, so most are the host's. What takes a guest function (a thread's
   start, a key's destructor, pthread_once) runs it through the bridge */
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
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

#if !defined(__linux__)
/* ---- bionic's pthread objects on FreeBSD, macOS and Windows, whose own differ in size (macOS's mutex is 64 bytes
   to bionic's 40, its attributes 64 to 56) or in their static initializer (macOS's and winpthreads' are not zero, and
   bionic's recursive one is 0x4000 in the first int). The guest's object keeps the address of a host object at
   offset 8, made at its first use; its first int is bionic's initializer. The attributes are bionic's layout, which
   only these functions read ---- */

/* bionic's mutex types and its initializers' first int */
enum { B_MUTEX_NORMAL = 0, B_MUTEX_RECURSIVE = 1, B_MUTEX_ERRORCHECK = 2, B_CREATE_DETACHED = 1 };
/* bionic's pthread_attr_t: flags, stack base, stack size, guard size, policy, priority */
enum { BA_FLAGS = 0, BA_STACK = 8, BA_STACK_SIZE = 16, BA_GUARD = 24, BA_SIZE = 56, BA_DETACHED = 1 };
/* the host stack of a thread the bridge starts: it runs host code only, the guest's own stack is the bridge's */
enum { HOST_STACK = 1 << 20 };

typedef struct {
    pthread_mutex_t lock;
} HostMutex;
typedef struct {
    pthread_cond_t cond;
    bool monotonic; /* the guest's deadlines are CLOCK_MONOTONIC's: waited for on CLOCK_REALTIME's */
} HostCond;
typedef struct {
    pthread_rwlock_t lock;
} HostRwlock;
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t more;
    uint32_t count;
} HostSem;

static uint64_t* object_slot(uint64_t guest) {
    return (uint64_t*)(uintptr_t)(guest + 8);
}
/* the host object of a guest object: made by make at the first use, by one thread when several race */
static void* host_object(uint64_t guest, void* (*make)(uint64_t guest), void (*drop)(void* object)) {
    uint64_t* slot = object_slot(guest);
    uint64_t have = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (have) return (void*)(uintptr_t)have;
    void* made = make(guest);
    if (!made) return NULL;
    uint64_t expected = 0;
    if (__atomic_compare_exchange_n(slot, &expected, (uint64_t)(uintptr_t)made, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
        return made;
    drop(made);
    return (void*)(uintptr_t)expected;
}

static void* make_mutex_of(int type) {
    HostMutex* m = malloc(sizeof *m);
    if (!m) return NULL;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, type == B_MUTEX_RECURSIVE    ? PTHREAD_MUTEX_RECURSIVE
                                  : type == B_MUTEX_ERRORCHECK ? PTHREAD_MUTEX_ERRORCHECK
                                                               : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(&m->lock, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}
/* a mutex that its static initializer made: its type is bits 14 and 15 of the first int */
static void* make_mutex(uint64_t guest) {
    uint32_t first;
    memcpy(&first, (const void*)(uintptr_t)guest, sizeof first);
    return make_mutex_of((int)(first >> 14 & 3));
}
static void drop_mutex(void* object) {
    pthread_mutex_destroy(&((HostMutex*)object)->lock);
    free(object);
}
static pthread_mutex_t* mutex_of(uint64_t guest) {
    HostMutex* m = host_object(guest, make_mutex, drop_mutex);
    return m ? &m->lock : NULL;
}
static void* make_cond_of(bool monotonic) {
    HostCond* k = malloc(sizeof *k);
    if (!k) return NULL;
    pthread_cond_init(&k->cond, NULL);
    k->monotonic = monotonic;
    return k;
}
static void* make_cond(uint64_t guest) {
    (void)guest;
    return make_cond_of(false);
}
static void drop_cond(void* object) {
    pthread_cond_destroy(&((HostCond*)object)->cond);
    free(object);
}
static void* make_rwlock(uint64_t guest) {
    (void)guest;
    HostRwlock* r = malloc(sizeof *r);
    if (r) pthread_rwlock_init(&r->lock, NULL);
    return r;
}
static void drop_rwlock(void* object) {
    pthread_rwlock_destroy(&((HostRwlock*)object)->lock);
    free(object);
}
static pthread_rwlock_t* rwlock_of(uint64_t guest) {
    HostRwlock* r = host_object(guest, make_rwlock, drop_rwlock);
    return r ? &r->lock : NULL;
}
/* the object gone: the guest's pointer to it cleared, as a destroyed object is the initializer's again */
static void destroy_object(uint64_t guest, void (*drop)(void* object)) {
    uint64_t have = __atomic_exchange_n(object_slot(guest), 0, __ATOMIC_ACQ_REL);
    if (have) drop((void*)(uintptr_t)have);
}
static void install_object(uint64_t guest, void* made, void (*drop)(void* object)) {
    uint64_t old = __atomic_exchange_n(object_slot(guest), (uint64_t)(uintptr_t)made, __ATOMIC_ACQ_REL);
    if (old) drop((void*)(uintptr_t)old);
}

/* the guest's attributes: a long, its low bits the type (mutexattr) or 1 for CLOCK_MONOTONIC (condattr) */
static void attr_set(uint64_t address, uint64_t value) {
    memcpy((void*)(uintptr_t)address, &value, sizeof value);
}
static uint64_t attr_get(uint64_t address) {
    uint64_t value = 0;
    if (address) memcpy(&value, (const void*)(uintptr_t)address, sizeof value);
    return value;
}
static void g_attr_zero(CPU* c, void* data) {
    (void)data;
    attr_set(X(0), 0);
    X(0) = 0;
}
static void g_attr_nothing(CPU* c, void* data) {
    (void)data;
    X(0) = 0;
}
static void g_mutexattr_settype(CPU* c, void* data) {
    (void)data;
    if (X(1) > B_MUTEX_ERRORCHECK) {
        X(0) = EINVAL;
        return;
    }
    attr_set(X(0), X(1));
    X(0) = 0;
}
static void g_condattr_setclock(CPU* c, void* data) {
    (void)data;
    if (X(1) > 1) { /* CLOCK_REALTIME 0 and CLOCK_MONOTONIC 1 */
        X(0) = EINVAL;
        return;
    }
    attr_set(X(0), X(1));
    X(0) = 0;
}

static void g_mutex_init(CPU* c, void* data) {
    (void)data;
    void* m = make_mutex_of((int)(attr_get(X(1)) & 3));
    if (!m) {
        X(0) = ENOMEM;
        return;
    }
    memset(PTR(0), 0, 8);
    install_object(X(0), m, drop_mutex);
    X(0) = 0;
}
static void g_mutex_destroy(CPU* c, void* data) {
    (void)data;
    destroy_object(X(0), drop_mutex);
    X(0) = 0;
}
static void g_mutex_lock(CPU* c, void* data) {
    (void)data;
    pthread_mutex_t* m = mutex_of(X(0));
    X(0) = m ? (uint64_t)pthread_mutex_lock(m) : ENOMEM;
}
static void g_mutex_trylock(CPU* c, void* data) {
    (void)data;
    pthread_mutex_t* m = mutex_of(X(0));
    X(0) = m ? (uint64_t)pthread_mutex_trylock(m) : ENOMEM;
}
static void g_mutex_unlock(CPU* c, void* data) {
    (void)data;
    pthread_mutex_t* m = mutex_of(X(0));
    X(0) = m ? (uint64_t)pthread_mutex_unlock(m) : EPERM;
}

static void g_cond_init(CPU* c, void* data) {
    (void)data;
    void* k = make_cond_of((attr_get(X(1)) & 1) != 0);
    if (!k) {
        X(0) = ENOMEM;
        return;
    }
    install_object(X(0), k, drop_cond);
    X(0) = 0;
}
static void g_cond_destroy(CPU* c, void* data) {
    (void)data;
    destroy_object(X(0), drop_cond);
    X(0) = 0;
}
static void g_cond_signal(CPU* c, void* data) {
    (void)data;
    HostCond* k = host_object(X(0), make_cond, drop_cond);
    X(0) = k ? (uint64_t)pthread_cond_signal(&k->cond) : ENOMEM;
}
static void g_cond_broadcast(CPU* c, void* data) {
    (void)data;
    HostCond* k = host_object(X(0), make_cond, drop_cond);
    X(0) = k ? (uint64_t)pthread_cond_broadcast(&k->cond) : ENOMEM;
}
static void g_cond_wait(CPU* c, void* data) {
    (void)data;
    HostCond* k = host_object(X(0), make_cond, drop_cond);
    pthread_mutex_t* m = mutex_of(X(1));
    X(0) = k && m ? (uint64_t)pthread_cond_wait(&k->cond, m) : ENOMEM;
}
/* a guest deadline (timespec of two 64-bit fields, on the cond's clock) as CLOCK_REALTIME's */
static struct timespec deadline_of(uint64_t address, bool monotonic) {
    int64_t s, ns;
    memcpy(&s, (const void*)(uintptr_t)address, sizeof s);
    memcpy(&ns, (const char*)(uintptr_t)address + 8, sizeof ns);
    if (monotonic) {
        struct timespec now_m, now_r;
        clock_gettime(CLOCK_MONOTONIC, &now_m);
        clock_gettime(CLOCK_REALTIME, &now_r);
        int64_t left = (s - (int64_t)now_m.tv_sec) * 1000000000 + (ns - now_m.tv_nsec);
        if (left < 0) left = 0;
        int64_t at = (int64_t)now_r.tv_nsec + left % 1000000000;
        s = (int64_t)now_r.tv_sec + left / 1000000000 + at / 1000000000;
        ns = at % 1000000000;
    }
    struct timespec t = {(time_t)s, (long)ns};
    return t;
}
static void g_cond_timedwait(CPU* c, void* data) {
    (void)data;
    HostCond* k = host_object(X(0), make_cond, drop_cond);
    pthread_mutex_t* m = mutex_of(X(1));
    if (!k || !m) {
        X(0) = ENOMEM;
        return;
    }
    struct timespec t = deadline_of(X(2), k->monotonic);
    X(0) = (uint64_t)pthread_cond_timedwait(&k->cond, m, &t);
}

static void g_rwlock_init(CPU* c, void* data) {
    (void)data;
    void* r = make_rwlock(0);
    if (!r) {
        X(0) = ENOMEM;
        return;
    }
    install_object(X(0), r, drop_rwlock);
    X(0) = 0;
}
static void g_rwlock_destroy(CPU* c, void* data) {
    (void)data;
    destroy_object(X(0), drop_rwlock);
    X(0) = 0;
}
static void g_rwlock_rdlock(CPU* c, void* data) {
    (void)data;
    pthread_rwlock_t* r = rwlock_of(X(0));
    X(0) = r ? (uint64_t)pthread_rwlock_rdlock(r) : ENOMEM;
}
static void g_rwlock_wrlock(CPU* c, void* data) {
    (void)data;
    pthread_rwlock_t* r = rwlock_of(X(0));
    X(0) = r ? (uint64_t)pthread_rwlock_wrlock(r) : ENOMEM;
}
static void g_rwlock_unlock(CPU* c, void* data) {
    (void)data;
    pthread_rwlock_t* r = rwlock_of(X(0));
    X(0) = r ? (uint64_t)pthread_rwlock_unlock(r) : EPERM;
}

/* sem_t: a mutex, a condition and the count (macOS has no unnamed semaphores). 0, or -1 and errno */
static void* make_sem(uint64_t guest) {
    (void)guest;
    HostSem* s = malloc(sizeof *s);
    if (!s) return NULL;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->more, NULL);
    s->count = 0;
    return s;
}
static void drop_sem(void* object) {
    HostSem* s = object;
    pthread_cond_destroy(&s->more);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
static void g_sem_init(CPU* c, void* data) {
    (void)data;
    HostSem* s = make_sem(0);
    if (!s) {
        hle_set_errno(GNU_ENOMEM);
        X(0) = (uint64_t)-1;
        return;
    }
    s->count = (uint32_t)X(2);
    install_object(X(0), s, drop_sem);
    X(0) = 0;
}
static void g_sem_destroy(CPU* c, void* data) {
    (void)data;
    destroy_object(X(0), drop_sem);
    X(0) = 0;
}
static void g_sem_post(CPU* c, void* data) {
    (void)data;
    HostSem* s = host_object(X(0), make_sem, drop_sem);
    if (!s) {
        hle_set_errno(GNU_EINVAL);
        X(0) = (uint64_t)-1;
        return;
    }
    pthread_mutex_lock(&s->lock);
    s->count++;
    pthread_cond_signal(&s->more);
    pthread_mutex_unlock(&s->lock);
    X(0) = 0;
}
/* sem_wait and sem_timedwait (an absolute CLOCK_REALTIME deadline) */
static void sem_take(CPU* c, bool timed) {
    HostSem* s = host_object(X(0), make_sem, drop_sem);
    if (!s) {
        hle_set_errno(GNU_EINVAL);
        X(0) = (uint64_t)-1;
        return;
    }
    struct timespec t = {0, 0};
    if (timed) t = deadline_of(X(1), false);
    int r = 0;
    pthread_mutex_lock(&s->lock);
    while (!s->count && !r) r = timed ? pthread_cond_timedwait(&s->more, &s->lock, &t) : pthread_cond_wait(&s->more, &s->lock);
    if (s->count) {
        s->count--;
        r = 0;
    }
    pthread_mutex_unlock(&s->lock);
    if (r) hle_set_errno(GNU_ETIMEDOUT);
    X(0) = r ? (uint64_t)-1 : 0;
}
static void g_sem_wait(CPU* c, void* data) {
    (void)data;
    sem_take(c, false);
}
static void g_sem_timedwait(CPU* c, void* data) {
    (void)data;
    sem_take(c, true);
}

/* pthread_attr_t in bionic's layout */
static void g_attr_init(CPU* c, void* data) {
    (void)data;
    unsigned char a[BA_SIZE] = {0};
    snx_st_le64(a + BA_STACK_SIZE, 1u << 20); /* bionic's default stack: 1 MiB */
    snx_st_le64(a + BA_GUARD, 4096);
    memcpy(PTR(0), a, sizeof a);
    X(0) = 0;
}
static void g_attr_setdetachstate(CPU* c, void* data) {
    (void)data;
    unsigned char* a = PTR(0);
    uint32_t flags = snx_ld_le32(a + BA_FLAGS);
    snx_st_le32(a + BA_FLAGS, X(1) == B_CREATE_DETACHED ? flags | BA_DETACHED : flags & ~(uint32_t)BA_DETACHED);
    X(0) = 0;
}
static void g_attr_setstacksize(CPU* c, void* data) {
    (void)data;
    if (X(1) < 16384) {
        X(0) = EINVAL;
        return;
    }
    snx_st_le64((unsigned char*)PTR(0) + BA_STACK_SIZE, X(1));
    X(0) = 0;
}
static void g_attr_getstack(CPU* c, void* data) {
    (void)data;
    const unsigned char* a = PTR(0);
    snx_st_le64(PTR(1), snx_ld_le64(a + BA_STACK));
    snx_st_le64(PTR(2), snx_ld_le64(a + BA_STACK_SIZE));
    X(0) = 0;
}
static void g_attr_getguardsize(CPU* c, void* data) {
    (void)data;
    snx_st_le64(PTR(1), snx_ld_le64((const unsigned char*)PTR(0) + BA_GUARD));
    X(0) = 0;
}
/* the host's attributes of a thread the guest creates with a (bionic's): its detach state */
static void host_attributes(uint64_t guest, pthread_attr_t* out) {
    pthread_attr_init(out);
    pthread_attr_setstacksize(out, HOST_STACK);
    if (guest && (snx_ld_le32((const void*)(uintptr_t)guest) & BA_DETACHED))
        pthread_attr_setdetachstate(out, PTHREAD_CREATE_DETACHED);
}
/* the stack of another thread, the host's own: FreeBSD's pthread_attr_get_np, macOS's
   pthread_get_stackaddr_np (the top), Windows' none */
static void host_stack_of(pthread_t thread, uint64_t* base, uint64_t* size) {
    *base = *size = 0;
#if defined(__FreeBSD__)
    pthread_attr_t a;
    pthread_attr_init(&a);
    void* low;
    size_t n;
    if (!pthread_attr_get_np(thread, &a) && !pthread_attr_getstack(&a, &low, &n)) {
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
#if defined(__linux__)
    pthread_attr_t* a = PTR(1);
    if (a) {
        memcpy(&attr, a, sizeof attr);
        pthread_attr_setstacksize(&attr, 1u << 20);
    }
#else
    host_attributes(X(1), &attr);
    pthread_attr_t* a = &attr;
#endif
    int r = pthread_create(&t, a ? &attr : NULL, thread_main, s);
#if !defined(__linux__)
    pthread_attr_destroy(&attr);
#endif
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
#if defined(__linux__)
    pthread_attr_t* attr = PTR(1);
    int r = pthread_getattr_np((pthread_t)X(0), attr);
    if (!r && pthread_equal((pthread_t)X(0), pthread_self())) {
        uint64_t base;
        size_t size;
        bridge_stack(&base, &size);
        pthread_attr_setstack(attr, (void*)(uintptr_t)base, size);
    }
    X(0) = (uint64_t)r;
#else
    /* bionic's attributes (its layout): the calling thread's guest stack, another's host stack */
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
#endif
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
#if defined(__linux__)
    P(pthread_attr_destroy), P(pthread_attr_getguardsize), P(pthread_attr_getstack), P(pthread_attr_init),
    P(pthread_attr_setdetachstate), P(pthread_attr_setstacksize), P(pthread_cond_broadcast), P(pthread_cond_destroy),
    P(pthread_cond_init), P(pthread_cond_signal), P(pthread_cond_timedwait), P(pthread_cond_wait),
    P(pthread_condattr_destroy), P(pthread_condattr_init), P(pthread_condattr_setclock), P(pthread_getspecific),
    P(pthread_mutex_destroy), P(pthread_mutex_init), P(pthread_mutex_lock), P(pthread_mutex_trylock),
    P(pthread_mutex_unlock), P(pthread_mutexattr_destroy), P(pthread_mutexattr_init), P(pthread_mutexattr_settype),
    P(pthread_rwlock_destroy), P(pthread_rwlock_init), P(pthread_rwlock_rdlock), P(pthread_rwlock_unlock),
    P(pthread_rwlock_wrlock), P(pthread_setname_np), P(pthread_setschedparam), P(pthread_setspecific),
    P(sem_init), P(sem_destroy), P(sem_post), P(sem_wait), P(sem_timedwait),
#else
    A(pthread_attr_destroy, g_attr_nothing), A(pthread_attr_getguardsize, g_attr_getguardsize),
    A(pthread_attr_getstack, g_attr_getstack), A(pthread_attr_init, g_attr_init),
    A(pthread_attr_setdetachstate, g_attr_setdetachstate), A(pthread_attr_setstacksize, g_attr_setstacksize),
    A(pthread_cond_broadcast, g_cond_broadcast), A(pthread_cond_destroy, g_cond_destroy),
    A(pthread_cond_init, g_cond_init), A(pthread_cond_signal, g_cond_signal),
    A(pthread_cond_timedwait, g_cond_timedwait), A(pthread_cond_wait, g_cond_wait),
    A(pthread_condattr_destroy, g_attr_nothing), A(pthread_condattr_init, g_attr_zero),
    A(pthread_condattr_setclock, g_condattr_setclock), A(pthread_getspecific, g_getspecific),
    A(pthread_mutex_destroy, g_mutex_destroy), A(pthread_mutex_init, g_mutex_init),
    A(pthread_mutex_lock, g_mutex_lock), A(pthread_mutex_trylock, g_mutex_trylock),
    A(pthread_mutex_unlock, g_mutex_unlock), A(pthread_mutexattr_destroy, g_attr_nothing),
    A(pthread_mutexattr_init, g_attr_zero), A(pthread_mutexattr_settype, g_mutexattr_settype),
    A(pthread_rwlock_destroy, g_rwlock_destroy), A(pthread_rwlock_init, g_rwlock_init),
    A(pthread_rwlock_rdlock, g_rwlock_rdlock), A(pthread_rwlock_unlock, g_rwlock_unlock),
    A(pthread_rwlock_wrlock, g_rwlock_wrlock), A(pthread_setname_np, g_setname),
    A(pthread_setschedparam, g_setschedparam), A(pthread_setspecific, g_setspecific),
    A(sem_init, g_sem_init), A(sem_destroy, g_sem_destroy), A(sem_post, g_sem_post), A(sem_wait, g_sem_wait),
    A(sem_timedwait, g_sem_timedwait),
#endif
    {"", NULL, NULL},
};
