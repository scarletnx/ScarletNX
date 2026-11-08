/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_BIONIC_SYNC_H
#define SNX_ANDROID_BIONIC_SYNC_H
/* bionic's synchronization objects on LP64, in bionic's own sizes inside the guest's storage, with no host object
   (bionic_sync.c). The sizes and the static initializers are those of bionic's public headers (libc/include/pthread.h,
   bits/pthread_types.h, semaphore.h): pthread_mutex_t 40 bytes, pthread_cond_t 48, pthread_rwlock_t 56,
   pthread_barrier_t 32, pthread_spinlock_t 8, sem_t 16, and pthread_mutexattr_t, pthread_condattr_t and
   pthread_rwlockattr_t a long, pthread_barrierattr_t an int. PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER and
   PTHREAD_RWLOCK_INITIALIZER are zero; PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP is 0x4000 in the first int and
   PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP 0x8000 (the type in bits 14 and 15). What else is in the storage is this
   file's: only these functions read it, and a copy of an object that no thread holds or waits on works.

   A wait sleeps on the futex that the runtime gives (BionicFutex): in the bridge, the GNU ABI kernel's, so a guest
   signal runs its handler during the wait. The functions return bionic's error numbers (Linux's), positive, and the
   semaphores 0 or -1 with the number in *error. No function returns EINTR but sem_wait and its timed forms, as POSIX
   has it */
#include <stdint.h>

/* an absolute time on a clock: CLOCK_REALTIME 0 or CLOCK_MONOTONIC 1 (Linux's numbers) */
typedef struct {
    int64_t seconds, nanoseconds;
    int clock;
} BionicTime;

typedef struct {
    /* until *word is not value, a wake, deadline (NULL: none) or a signal: 0, or -ETIMEDOUT, -EAGAIN, -EINTR, -EINVAL
       (Linux's numbers) */
    int (*wait)(void* context, uint32_t* word, uint32_t value, const BionicTime* deadline);
    /* up to count waiters on word */
    void (*wake)(void* context, uint32_t* word, int count);
    /* the calling thread's id, never 0 */
    uint32_t (*tid)(void* context);
    void* context;
} BionicFutex;

/* bionic's mutex types and process-sharing values */
enum {
    BIONIC_MUTEX_NORMAL = 0,
    BIONIC_MUTEX_RECURSIVE = 1,
    BIONIC_MUTEX_ERRORCHECK = 2,
    BIONIC_PROCESS_PRIVATE = 0,
    BIONIC_PROCESS_SHARED = 1,
    BIONIC_BARRIER_SERIAL_THREAD = -1,
    BIONIC_SEM_VALUE_MAX = 0x3fffffff
};

/* the attributes (a long or an int in the guest): the address of the guest's object */
int bionic_mutexattr_init(void* attr);
int bionic_mutexattr_settype(void* attr, int type);
int bionic_mutexattr_gettype(const void* attr, int* type);
int bionic_mutexattr_setpshared(void* attr, int pshared);
int bionic_mutexattr_getpshared(const void* attr, int* pshared);
int bionic_condattr_init(void* attr);
int bionic_condattr_setclock(void* attr, int clock);
int bionic_condattr_getclock(const void* attr, int* clock);
int bionic_condattr_setpshared(void* attr, int pshared);
int bionic_condattr_getpshared(const void* attr, int* pshared);
int bionic_rwlockattr_init(void* attr);
int bionic_rwlockattr_setpshared(void* attr, int pshared);
int bionic_rwlockattr_getpshared(const void* attr, int* pshared);
int bionic_rwlockattr_setkind(void* attr, int kind);
int bionic_rwlockattr_getkind(const void* attr, int* kind);
int bionic_barrierattr_init(void* attr);
int bionic_barrierattr_setpshared(void* attr, int pshared);
int bionic_barrierattr_getpshared(const void* attr, int* pshared);

/* attr NULL for the defaults */
int bionic_mutex_init(void* mutex, const void* attr);
int bionic_mutex_destroy(void* mutex);
int bionic_mutex_lock(const BionicFutex* f, void* mutex);
int bionic_mutex_trylock(const BionicFutex* f, void* mutex);
int bionic_mutex_timedlock(const BionicFutex* f, void* mutex, const BionicTime* deadline);
int bionic_mutex_unlock(const BionicFutex* f, void* mutex);

int bionic_cond_init(void* cond, const void* attr);
int bionic_cond_destroy(void* cond);
int bionic_cond_signal(const BionicFutex* f, void* cond);
int bionic_cond_broadcast(const BionicFutex* f, void* cond);
/* deadline NULL: no timeout; its clock -1: the cond's own (pthread_cond_timedwait) */
int bionic_cond_wait(const BionicFutex* f, void* cond, void* mutex, const BionicTime* deadline);

int bionic_rwlock_init(void* rwlock, const void* attr);
int bionic_rwlock_destroy(void* rwlock);
int bionic_rwlock_rdlock(const BionicFutex* f, void* rwlock, const BionicTime* deadline);
int bionic_rwlock_tryrdlock(const BionicFutex* f, void* rwlock);
int bionic_rwlock_wrlock(const BionicFutex* f, void* rwlock, const BionicTime* deadline);
int bionic_rwlock_trywrlock(const BionicFutex* f, void* rwlock);
int bionic_rwlock_unlock(const BionicFutex* f, void* rwlock);

int bionic_spin_init(void* lock, int pshared);
int bionic_spin_destroy(void* lock);
int bionic_spin_lock(void* lock);
int bionic_spin_trylock(void* lock);
int bionic_spin_unlock(void* lock);

int bionic_barrier_init(void* barrier, const void* attr, unsigned count);
int bionic_barrier_destroy(void* barrier);
/* 0, or BIONIC_BARRIER_SERIAL_THREAD for one thread of each round */
int bionic_barrier_wait(const BionicFutex* f, void* barrier);

/* 0, or -1 with *error */
int bionic_sem_init(void* sem, int pshared, unsigned value, int* error);
int bionic_sem_destroy(void* sem, int* error);
int bionic_sem_post(const BionicFutex* f, void* sem, int* error);
int bionic_sem_wait(const BionicFutex* f, void* sem, const BionicTime* deadline, int* error);
int bionic_sem_trywait(void* sem, int* error);
int bionic_sem_getvalue(void* sem, int* value, int* error);
#endif
