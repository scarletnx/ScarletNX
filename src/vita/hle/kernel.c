/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceLibKernel, SceThreadmgr, SceSysmem and SceProcessmgr: threads, their synchronization, memory blocks and time, as
   the vitasdk headers (MIT) declare them */
#include "vita/hle/hle.h"
#include "vita/hle/hle.hook.h"
#include "common/utilities/ascii.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "snx_settings.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SCE_KERNEL_ERROR_WAIT_DELETE      0x80028006u
#define SCE_KERNEL_ERROR_ILLEGAL_THREAD_ID 0x80028022u
#define SCE_KERNEL_ERROR_ALREADY_SENT     0x80028121u
#define SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID  0x80028101u
#define SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID 0x80028141u
#define SCE_KERNEL_ERROR_UNKNOWN_EVF_ID   0x800280e1u
#define SCE_KERNEL_ERROR_UNKNOWN_COND_ID  0x800281a1u
#define SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK_TYPE 0x80024b01u
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW 0x09408060u
#define MUTEX_RECURSIVE 0x02u
#define RW_LOCK_RECURSIVE 0x02u
#define SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID   0x800281e1u
#define SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID     0x800281a1u
#define SCE_KERNEL_ERROR_RW_LOCK_RECURSIVE    0x800281e2u
#define SCE_KERNEL_ERROR_RW_LOCK_NOT_OWNED    0x800281e4u
#define SCE_KERNEL_ERROR_RW_LOCK_UNLOCK_UDF   0x800281e5u
#define EVF_WAITMODE_OR        0x01u
#define EVF_WAITMODE_CLEAR_ALL 0x02u
#define EVF_WAITMODE_CLEAR_PAT 0x04u

static void name_of(VitaProcess* p, uint32_t address, char out[32]) { vita_string(p, address, out, 32); }

/* ---- threads ---- */
uint32_t vita_kernel_create_thread(VitaProcess* p, VitaThread* t, uint32_t name_address, uint32_t entry,
                                   uint32_t priority, uint32_t stack_size, uint32_t attr) {
    (void)t;
    char name[32];
    name_of(p, name_address, name);
    int32_t uid = vita_thread_new(p, name, entry, priority, stack_size, attr);
    return (uint32_t)uid;
}
uint32_t vita_kernel_start_thread(VitaProcess* p, VitaThread* t, uint32_t thread_id, uint32_t size, uint32_t block) {
    (void)t;
    VitaThread* target = vita_thread(p, thread_id);
    uint32_t copy = 0;
    if (!target) return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    if (target->state != VITA_THREAD_DORMANT) return SCE_KERNEL_ERROR_NOT_DORMANT;
    /* the argument block, copied to the top of the thread's stack */
    if (block && size && size < target->stack_size / 2) {
        copy = (target->stack + target->stack_size - size) & ~15u;
        memmove(vita_ptr(p, copy), vita_ptr(p, block), size);
    }
    vita_thread_start(p, target, size, copy);
    return 0;
}
uint32_t vita_kernel_exit_thread(VitaProcess* p, VitaThread* t, uint32_t status) {
    if (vita_in_module_start(t)) {
        t->cpu.r[0] = status;
        vita_hle_call_returned(p, t);
        return t->cpu.r[0];
    }
    vita_thread_end(p, t, (int32_t)status);
    return 0;
}
static void release_thread(VitaProcess* p, VitaThread* t) {
    vita_free(p, t->stack);
    if (t->module_tls) vita_free(p, t->module_tls);
    vita_object_delete(p, t->uid);
    t->state = VITA_THREAD_FREE;
}
uint32_t vita_kernel_exit_delete_thread(VitaProcess* p, VitaThread* t, int32_t status) {
    vita_thread_end(p, t, status);
    /* no waiter can see its status once it is gone: one waiting for its end is told it ended */
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        VitaThread* w = &p->threads[i];
        if (w->state == VITA_THREAD_WAITING && w->wait.kind == VITA_WAIT_THREAD_END && w->wait.object == t->uid) {
            if (w->wait.out) vita_write32(p, w->wait.out, (uint32_t)t->exit_status);
            w->cpu.r[0] = 0;
            w->state = VITA_THREAD_READY;
        }
    }
    release_thread(p, t);
    return 0;
}
uint32_t vita_kernel_delete_thread(VitaProcess* p, VitaThread* t, uint32_t thread_id) {
    (void)t;
    VitaThread* target = vita_thread(p, thread_id);
    if (!target) return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    if (target->state != VITA_THREAD_DORMANT && target->state != VITA_THREAD_DEAD)
        return SCE_KERNEL_ERROR_NOT_DORMANT;
    release_thread(p, target);
    return 0;
}
uint32_t vita_kernel_wait_thread_end(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t status, uint32_t timeout) {
    (void)t;
    if (!vita_thread(p, uid)) return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_THREAD_END, .object = uid, .out = status,
                            .timeout = timeout, .deadline = vita_deadline(p, timeout)});
    return 0;
}
uint32_t vita_kernel_get_thread_id(VitaProcess* p, VitaThread* t) {
    (void)p;
    return t->uid;
}
uint32_t vita_kernel_delay_thread(VitaProcess* p, VitaThread* t, uint32_t usec) {
    (void)t;
    uint64_t delay = (uint64_t)usec * 1000u;
    /* a delay of 0 gives the core away, as the console's scheduler does */
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_DELAY, .deadline = plat_ns() + (delay ? delay : 1)});
    return 0;
}
uint32_t vita_kernel_get_thread_current_priority(VitaProcess* p, VitaThread* t) {
    (void)p;
    return t->priority;
}
uint32_t vita_kernel_change_thread_priority(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t priority) {
    VitaThread* target = uid ? vita_thread(p, uid) : t;
    if (!target) return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    target->priority = priority;
    return 0;
}
uint32_t vita_kernel_check_thread_stack(VitaProcess* p, VitaThread* t) {
    (void)p;
    return t->cpu.r[13] - t->stack; /* what is left */
}
uint32_t vita_kernel_get_tls_addr(VitaProcess* p, VitaThread* t, uint32_t key) {
    (void)p;
    /* the slots are the first half of the thread's TLS area, the TLS register points at the second */
    return key < VITA_TLS_SIZE / 8 ? t->tls + key * 4 : 0;
}
uint32_t vita_kernel_exit_process(VitaProcess* p, VitaThread* t, int32_t exit_code) {
    (void)t;
    p->exited = true;
    p->exit_code = exit_code;
    return 0;
}
uint32_t vita_kernel_get_process_id(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0x10005;
}
/* sceKernelGetProcessParam(): the SceProcessParam block the main module exports (NID 0x70fba1e7). libc.suprx's
   module_start calls this to reach the SceLibcParam (heap size, malloc replacement, main thread's stack and
   priority), so returning 0 here aborts libc's init and, through vita_module_start_next's chain, the whole run.
   p->image.process_param is the same block, already relocated into guest memory (image.c's exports() records it
   from the export with NID 0x70fba1e7, and process.c already reads it for the main thread's stack and priority).
   Vita3K's sceKernelGetProcessParam returns that same block. */
uint32_t vita_kernel_get_process_param(VitaProcess* p, VitaThread* t) {
    static unsigned shown;
    (void)t;
    uint32_t pp = p->image.process_param;
    if (shown < 1) {
        printf("vita: sceKernelGetProcessParam() -> 0x%08" PRIx32 "\n", pp);
        if (pp && vita_mapped(p, pp, 0x20)) {
            printf("vita:   size=%u magic=0x%08x ver=%u name=0x%08x stacksize=0x%08x prio=0x%08x "
                   "libcparam=0x%08x userproc=0x%08x\n",
                   vita_read32(p, pp),      vita_read32(p, pp + 4),  vita_read32(p, pp + 8),
                   vita_read32(p, pp + 0xC), vita_read32(p, pp + 0x10), vita_read32(p, pp + 0x14),
                   vita_read32(p, pp + 0x18), vita_read32(p, pp + 0x1C));
        }
        ++shown;
    }
    return pp;
}

/* ---- semaphores ---- */
uint32_t vita_kernel_create_sema(VitaProcess* p, VitaThread* t, uint32_t name_address, uint32_t attr, int32_t count,
                                 int32_t max) {
    (void)t;
    char name[32];
    VitaObject* o;
    name_of(p, name_address, name);
    uint32_t uid = vita_object_new(p, VITA_OBJ_SEMA, name, &o);
    if (!uid) return SCE_KERNEL_ERROR_NO_MEMORY;
    o->u.sema.attr = attr;
    o->u.sema.count = count;
    o->u.sema.max = max;
    return uid;
}
uint32_t vita_kernel_wait_sema(VitaProcess* p, VitaThread* t, uint32_t uid, int32_t need, uint32_t timeout) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_SEMA);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    if (o->u.sema.count >= need) {
        o->u.sema.count -= need;
        return 0;
    }
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_SEMA, .object = uid, .a = (uint32_t)need, .b = o->u.sema.cancelled,
                            .timeout = timeout, .deadline = vita_deadline(p, timeout)});
    return 0;
}
uint32_t vita_kernel_poll_sema(VitaProcess* p, VitaThread* t, uint32_t uid, int32_t need) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_SEMA);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    if (o->u.sema.count < need) return SCE_KERNEL_ERROR_SEMA_ZERO;
    o->u.sema.count -= need;
    return 0;
}
uint32_t vita_kernel_signal_sema(VitaProcess* p, VitaThread* t, uint32_t uid, int32_t signal) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_SEMA);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    /* the count and the signal are any 32 bits: their sum is taken wide, and one past the most or past a count's own
       range is the overflow */
    int64_t sum = (int64_t)o->u.sema.count + signal;
    if ((o->u.sema.max > 0 && sum > o->u.sema.max) || sum > INT32_MAX || sum < INT32_MIN)
        return SCE_KERNEL_ERROR_SEMA_OVF;
    o->u.sema.count = (int32_t)sum;
    return 0;
}
/* the threads waiting on the semaphore uid */
static uint32_t sema_waiters(VitaProcess* p, uint32_t uid) {
    uint32_t n = 0;
    for (unsigned i = 0; i < VITA_THREADS; ++i)
        if (p->threads[i].state == VITA_THREAD_WAITING && p->threads[i].wait.kind == VITA_WAIT_SEMA &&
            p->threads[i].wait.object == uid)
            ++n;
    return n;
}
/* sceKernelGetSemaInfo(uid, *info, size): SceKernelSemaInfo (size, UID, name, attribute, the initial count, the
   count now, the most, the threads waiting; 0x3c bytes). The initial count is not kept: the count now stands for it */
uint32_t vita_kernel_get_sema_info(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t at, uint32_t size) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_SEMA);
    unsigned char* info;
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    if (!at || size < 0x3c) return SCE_KERNEL_ERROR_ERROR;
    if (!vita_mapped(p, at, 0x3c)) return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    info = vita_ptr(p, at);
    memset(info, 0, 0x3c);
    snx_st_le32(info, 0x3c);
    snx_st_le32(info + 4, uid);
    snprintf((char*)info + 8, 32, "%s", o->name);
    snx_st_le32(info + 0x28, o->u.sema.attr);
    snx_st_le32(info + 0x2c, (uint32_t)o->u.sema.count);
    snx_st_le32(info + 0x30, (uint32_t)o->u.sema.count);
    snx_st_le32(info + 0x34, (uint32_t)o->u.sema.max);
    snx_st_le32(info + 0x38, sema_waiters(p, uid));
    return 0;
}
/* sceKernelCancelSema(uid, count, *waiting): the waiting threads' waits end with SCE_KERNEL_ERROR_WAIT_CANCEL, the
   count set when it is not negative, and how many were waiting written */
uint32_t vita_kernel_cancel_sema(VitaProcess* p, VitaThread* t, uint32_t uid, int32_t count, uint32_t waiting) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_SEMA);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    if (waiting && vita_mapped(p, waiting, 4)) vita_write32(p, waiting, sema_waiters(p, uid));
    if (count >= 0) o->u.sema.count = count;
    ++o->u.sema.cancelled;
    return 0;
}
static uint32_t delete_object(VitaProcess* p, uint32_t uid, uint32_t type, uint32_t unknown) {
    if (!vita_object(p, uid, type)) return unknown;
    vita_object_delete(p, uid);
    return 0;
}
uint32_t vita_kernel_delete_sema(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    return delete_object(p, uid, VITA_OBJ_SEMA, SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID);
}

/* ---- mutexes: a mutex's UID, or an LwMutex's work area whose first word holds its UID ---- */
static uint32_t new_mutex(VitaProcess* p, uint32_t name_address, uint32_t attr, int32_t count, uint32_t work) {
    char name[32];
    VitaObject* o;
    name_of(p, name_address, name);
    uint32_t uid = vita_object_new(p, VITA_OBJ_MUTEX, name, &o);
    if (!uid) return 0;
    o->u.mutex.attr = attr;
    o->u.mutex.count = count;
    o->u.mutex.work = work;
    o->u.mutex.owner = count > 0 ? p->current->uid : 0;
    return uid;
}
uint32_t vita_kernel_create_mutex(VitaProcess* p, VitaThread* t, uint32_t name_address, uint32_t attr, int32_t count) {
    (void)t;
    uint32_t uid = new_mutex(p, name_address, attr, count, 0);
    return uid ? uid : SCE_KERNEL_ERROR_NO_MEMORY;
}
uint32_t vita_kernel_create_lw_mutex(VitaProcess* p, VitaThread* t, uint32_t work, uint32_t name_address, uint32_t attr,
                                     int32_t count) {
    (void)t;
    uint32_t uid = new_mutex(p, name_address, attr, count, work);
    if (!uid) return SCE_KERNEL_ERROR_NO_MEMORY;
    vita_write32(p, work, uid);
    return 0;
}
static VitaObject* lw_mutex(VitaProcess* p, uint32_t work, uint32_t* uid) {
    *uid = work ? vita_read32(p, work) : 0;
    VitaObject* o = vita_object(p, *uid, VITA_OBJ_MUTEX);
    return o && o->u.mutex.work == work ? o : NULL;
}
/* a lock now: 0, or the error, or 1 when the caller must wait */
static uint32_t try_lock(VitaThread* t, VitaObject* o, int32_t count) {
    if (count <= 0) return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    if (!o->u.mutex.owner) {
        o->u.mutex.owner = t->uid;
        o->u.mutex.count = count;
        return 0;
    }
    if (o->u.mutex.owner == t->uid) {
        if (!(o->u.mutex.attr & MUTEX_RECURSIVE)) return SCE_KERNEL_ERROR_MUTEX_RECURSIVE;
        o->u.mutex.count += count;
        return 0;
    }
    return 1;
}
static uint32_t lock(VitaProcess* p, VitaThread* t, uint32_t uid, VitaObject* o, int32_t count, uint32_t timeout,
                     uint32_t unknown) {
    if (!o) return unknown;
    uint32_t r = try_lock(t, o, count);
    if (r != 1) return r;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_MUTEX, .object = uid, .a = (uint32_t)count, .timeout = timeout,
                            .deadline = vita_deadline(p, timeout)});
    return 0;
}
static uint32_t try_lock_call(VitaThread* t, VitaObject* o, int32_t count, uint32_t unknown, uint32_t busy) {
    if (!o) return unknown;
    uint32_t r = try_lock(t, o, count);
    return r == 1 ? busy : r;
}
/* A mutex its owner lets go of goes to the thread waiting for it longest (or, with SCE_KERNEL_ATTR_THREAD_PRIO, the
   most urgent), as the console's kernel hands it over: left free, the thread that let go would take it again before
   a waiter ran, and one that locks and unlocks it in a loop would keep the others waiting forever. The waiter's wait
   is met (wait.b) with the mutex its own */
#define ATTR_THREAD_PRIO 0x2000u
static void hand_over(VitaProcess* p, uint32_t uid, VitaObject* o) {
    VitaThread* next = NULL;
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        VitaThread* w = &p->threads[i];
        if (w->state != VITA_THREAD_WAITING || w->wait.kind != VITA_WAIT_MUTEX || w->wait.object != uid || w->wait.b)
            continue;
        bool sooner = !next || ((o->u.mutex.attr & ATTR_THREAD_PRIO) && w->priority != next->priority
                                    ? w->priority < next->priority
                                    : w->seq < next->seq);
        if (sooner) next = w;
    }
    if (!next) return;
    o->u.mutex.owner = next->uid, o->u.mutex.count = (int32_t)next->wait.a;
    next->wait.b = 1;
}
static uint32_t unlock(VitaProcess* p, VitaThread* t, uint32_t uid, VitaObject* o, int32_t count, uint32_t unknown,
                       uint32_t unlocked, uint32_t not_owned) {
    if (!o) return unknown;
    if (!o->u.mutex.owner) return unlocked;
    if (o->u.mutex.owner != t->uid) return not_owned;
    if (count <= 0 || count > o->u.mutex.count) return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    if (!(o->u.mutex.count -= count)) {
        o->u.mutex.owner = 0;
        hand_over(p, uid, o);
    }
    return 0;
}
uint32_t vita_kernel_lock_mutex(VitaProcess* p, VitaThread* t, uint32_t uid, int32_t count, uint32_t timeout) {
    return lock(p, t, uid, vita_object(p, uid, VITA_OBJ_MUTEX), count, timeout,
                SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
}
uint32_t vita_kernel_try_lock_mutex(VitaProcess* p, VitaThread* t, uint32_t uid, int32_t count) {
    return try_lock_call(t, vita_object(p, uid, VITA_OBJ_MUTEX), count,
                         SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID, 0x80028145u /* SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN */);
}
uint32_t vita_kernel_unlock_mutex(VitaProcess* p, VitaThread* t, uint32_t uid, int32_t count) {
    return unlock(p, t, uid, vita_object(p, uid, VITA_OBJ_MUTEX), count, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID,
           SCE_KERNEL_ERROR_MUTEX_UNLOCKED, SCE_KERNEL_ERROR_MUTEX_NOT_OWNED);
}
uint32_t vita_kernel_delete_mutex(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    return delete_object(p, uid, VITA_OBJ_MUTEX, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
}
uint32_t vita_kernel_lock_lw_mutex(VitaProcess* p, VitaThread* t, uint32_t work, int32_t count, uint32_t timeout) {
    uint32_t uid;
    VitaObject* o = lw_mutex(p, work, &uid);
    return lock(p, t, uid, o, count, timeout, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
}
uint32_t vita_kernel_try_lock_lw_mutex(VitaProcess* p, VitaThread* t, uint32_t work, int32_t count) {
    uint32_t uid;
    return try_lock_call(t, lw_mutex(p, work, &uid), count, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID,
                         0x80028185u /* SCE_KERNEL_ERROR_LW_MUTEX_FAILED_TO_OWN */);
}
uint32_t vita_kernel_unlock_lw_mutex(VitaProcess* p, VitaThread* t, uint32_t work, int32_t count) {
    uint32_t uid;
    VitaObject* o = lw_mutex(p, work, &uid);
    return unlock(p, t, uid, o, count, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID,
           SCE_KERNEL_ERROR_LW_MUTEX_UNLOCKED, SCE_KERNEL_ERROR_LW_MUTEX_NOT_OWNED);
}
uint32_t vita_kernel_delete_lw_mutex(VitaProcess* p, VitaThread* t, uint32_t work) {
    (void)t;
    uint32_t uid;
    if (!lw_mutex(p, work, &uid)) return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
    vita_object_delete(p, uid);
    vita_write32(p, work, 0);
    return 0;
}

/* ---- event flags ---- */
uint32_t vita_kernel_create_event_flag(VitaProcess* p, VitaThread* t, uint32_t name_address, uint32_t attr,
                                       uint32_t pattern) {
    (void)t;
    char name[32];
    VitaObject* o;
    name_of(p, name_address, name);
    uint32_t uid = vita_object_new(p, VITA_OBJ_EVENT_FLAG, name, &o);
    if (!uid) return SCE_KERNEL_ERROR_NO_MEMORY;
    o->u.event_flag.attr = attr;
    o->u.event_flag.pattern = pattern;
    return uid;
}
/* whether pattern meets the wait's bits in mode, the flag cleared as mode says when it does */
static bool event_met(VitaProcess* p, VitaObject* o, uint32_t bits, uint32_t mode, uint32_t out) {
    uint32_t pattern = o->u.event_flag.pattern;
    bool met = mode & EVF_WAITMODE_OR ? (pattern & bits) != 0 : (pattern & bits) == bits;
    if (!met) return false;
    if (out) vita_write32(p, out, pattern);
    if (mode & EVF_WAITMODE_CLEAR_ALL)
        o->u.event_flag.pattern = 0;
    else if (mode & EVF_WAITMODE_CLEAR_PAT)
        o->u.event_flag.pattern &= ~bits;
    return true;
}
uint32_t vita_kernel_wait_event_flag(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t bits, uint32_t mode,
                                     uint32_t out, uint32_t timeout) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_EVENT_FLAG);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
    if (event_met(p, o, bits, mode, out)) return 0;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_EVENT_FLAG, .object = uid, .a = bits, .b = mode, .out = out,
                            .timeout = timeout, .deadline = vita_deadline(p, timeout)});
    return 0;
}
uint32_t vita_kernel_poll_event_flag(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t bits, uint32_t mode,
                                     uint32_t out) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_EVENT_FLAG);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
    return event_met(p, o, bits, mode, out) ? 0 : SCE_KERNEL_ERROR_EVF_COND;
}
uint32_t vita_kernel_set_event_flag(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t pattern) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_EVENT_FLAG);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
    o->u.event_flag.pattern |= pattern;
    return 0;
}
uint32_t vita_kernel_clear_event_flag(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t pattern) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_EVENT_FLAG);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
    o->u.event_flag.pattern &= pattern;
    return 0;
}
uint32_t vita_kernel_delete_event_flag(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    return delete_object(p, uid, VITA_OBJ_EVENT_FLAG, SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);
}

/* ---- condition variables: the waiter gives up its mutex, waits to be signaled (wait.b), then takes it again ---- */
uint32_t vita_kernel_create_cond(VitaProcess* p, VitaThread* t, uint32_t name_address, uint32_t attr, uint32_t mutex) {
    (void)t, (void)attr;
    char name[32];
    VitaObject* o;
    name_of(p, name_address, name);
    uint32_t uid = vita_object_new(p, VITA_OBJ_COND, name, &o);
    if (!uid) return SCE_KERNEL_ERROR_NO_MEMORY;
    o->u.cond.mutex = mutex;
    return uid;
}
/* timeout is the guest's SceUInt32* (0 for none), as VitaWait.timeout and vita_deadline take it: the wait writes the time
   left back through it */
static uint32_t cond_wait(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t timeout) {
    VitaObject* c = vita_object(p, uid, VITA_OBJ_COND);
    if (!c) return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
    VitaObject* m = vita_object(p, c->u.cond.mutex, VITA_OBJ_MUTEX);
    if (!m || m->u.mutex.owner != t->uid) return SCE_KERNEL_ERROR_MUTEX_NOT_OWNED;
    uint32_t count = (uint32_t)m->u.mutex.count;
    m->u.mutex.owner = 0, m->u.mutex.count = 0;
    hand_over(p, c->u.cond.mutex, m);
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_COND, .object = uid, .a = c->u.cond.mutex, .out = count,
                            .timeout = timeout, .deadline = vita_deadline(p, timeout)});
    return 0;
}
uint32_t vita_kernel_wait_cond(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t timeout) {
    return cond_wait(p, t, uid, timeout);
}
static uint32_t cond_signal(VitaProcess* p, uint32_t uid, bool all) {
    if (!vita_object(p, uid, VITA_OBJ_COND)) return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
    VitaThread* first = NULL;
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        VitaThread* w = &p->threads[i];
        if (w->state != VITA_THREAD_WAITING || w->wait.kind != VITA_WAIT_COND || w->wait.object != uid || w->wait.b)
            continue;
        if (all)
            w->wait.b = 1;
        else if (!first || w->seq < first->seq)
            first = w;
    }
    if (first) first->wait.b = 1;
    return 0;
}
uint32_t vita_kernel_signal_cond_one(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    return cond_signal(p, uid, false);
}
uint32_t vita_kernel_signal_cond_all(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    return cond_signal(p, uid, true);
}
uint32_t vita_kernel_delete_cond(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    return delete_object(p, uid, VITA_OBJ_COND, SCE_KERNEL_ERROR_UNKNOWN_COND_ID);
}

/* ---- LwConds: the work area's first word holds the UID of a cond, whose mutex is the LwMutex given at creation ---- */
static VitaObject* lw_cond(VitaProcess* p, uint32_t work, uint32_t* uid) {
    *uid = work ? vita_read32(p, work) : 0;
    return vita_object(p, *uid, VITA_OBJ_COND);
}
/* sceKernelCreateLwCond(work, name, attr, LwMutex work, options) */
uint32_t vita_kernel_create_lw_cond(VitaProcess* p, VitaThread* t, uint32_t work, uint32_t name_address, uint32_t attr,
                                    uint32_t mutex_work, uint32_t options) {
    (void)t, (void)attr, (void)options;
    uint32_t mutex = mutex_work ? vita_read32(p, mutex_work) : 0;
    if (!vita_object(p, mutex, VITA_OBJ_MUTEX)) return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
    char name[32];
    VitaObject* o;
    name_of(p, name_address, name);
    uint32_t uid = vita_object_new(p, VITA_OBJ_COND, name, &o);
    if (!uid) return SCE_KERNEL_ERROR_NO_MEMORY;
    o->u.cond.mutex = mutex;
    vita_write32(p, work, uid);
    return 0;
}
/* sceKernelWaitLwCond(work, uint32_t* timeout): no timeout pointer waits for good */
uint32_t vita_kernel_wait_lw_cond(VitaProcess* p, VitaThread* t, uint32_t work, uint32_t at) {
    uint32_t uid;
    if (!lw_cond(p, work, &uid)) return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
    return cond_wait(p, t, uid, at && vita_mapped(p, at, 4) ? at : 0);
}
uint32_t vita_kernel_signal_lw_cond(VitaProcess* p, VitaThread* t, uint32_t work) {
    (void)t;
    uint32_t uid;
    return lw_cond(p, work, &uid) ? cond_signal(p, uid, false) : SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
}
uint32_t vita_kernel_signal_lw_cond_all(VitaProcess* p, VitaThread* t, uint32_t work) {
    (void)t;
    uint32_t uid;
    return lw_cond(p, work, &uid) ? cond_signal(p, uid, true) : SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
}
/* sceKernelSignalLwCondTo(work, thread UID): that thread's wait, if it waits on the cond */
uint32_t vita_kernel_signal_lw_cond_to(VitaProcess* p, VitaThread* t, uint32_t work, uint32_t thread_id) {
    (void)t;
    uint32_t uid;
    if (!lw_cond(p, work, &uid)) return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        VitaThread* w = &p->threads[i];
        if (w->uid == thread_id && w->state == VITA_THREAD_WAITING && w->wait.kind == VITA_WAIT_COND &&
            w->wait.object == uid && !w->wait.b)
            w->wait.b = 1;
    }
    return 0;
}
uint32_t vita_kernel_delete_lw_cond(VitaProcess* p, VitaThread* t, uint32_t work) {
    (void)t;
    uint32_t uid;
    if (!lw_cond(p, work, &uid)) return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
    vita_object_delete(p, uid);
    vita_write32(p, work, 0);
    return 0;
}

static uint64_t process_us(VitaProcess* p);
/* ---- timers (counters the game reads as a clock; no event or callback is delivered) ---- */
static uint64_t timer_time(VitaProcess* p, const VitaObject* o) {
    return o->u.timer.accumulated + (o->u.timer.running ? process_us(p) - o->u.timer.started : 0);
}
uint32_t vita_kernel_create_timer(VitaProcess* p, VitaThread* t, uint32_t name_address) {
    (void)t;
    char name[32];
    VitaObject* o;
    name_of(p, name_address, name);
    uint32_t uid = vita_object_new(p, VITA_OBJ_TIMER, name, &o);
    if (!uid) return SCE_KERNEL_ERROR_NO_MEMORY;
    return uid;
}
uint32_t vita_kernel_start_timer(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_TIMER);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID;
    if (o->u.timer.running) return 1;
    o->u.timer.running = 1, o->u.timer.started = process_us(p);
    return 0;
}
uint32_t vita_kernel_stop_timer(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_TIMER);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID;
    if (!o->u.timer.running) return 0;
    o->u.timer.accumulated = timer_time(p, o), o->u.timer.running = 0;
    return 1;
}
uint64_t vita_kernel_get_timer_time_wide(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_TIMER);
    return o ? timer_time(p, o) : 0;
}
/* sceKernelSetTimerTimeWide(uid, u64 time): the time it had comes back */
uint64_t vita_kernel_set_timer_time_wide(VitaProcess* p, VitaThread* t, uint32_t uid, uint64_t time) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_TIMER);
    if (!o) return 0;
    uint64_t before = timer_time(p, o), now = time;
    o->u.timer.accumulated = now, o->u.timer.started = process_us(p);
    return before;
}
uint32_t vita_kernel_delete_timer(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    return delete_object(p, uid, VITA_OBJ_TIMER, SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID);
}

/* ---- reader-writer locks ---- */
uint32_t vita_kernel_create_rw_lock(VitaProcess* p, VitaThread* t, uint32_t name_address, uint32_t attr) {
    (void)t;
    char name[32];
    VitaObject* o;
    name_of(p, name_address, name);
    uint32_t uid = vita_object_new(p, VITA_OBJ_RWLOCK, name, &o);
    if (!uid) return SCE_KERNEL_ERROR_NO_MEMORY;
    o->u.rwlock.attr = attr;
    return uid;
}
/* a lock now: 0, or the error, or 1 when the caller must wait. A writer may also read what it writes */
static uint32_t try_rw_lock(VitaThread* t, VitaObject* o, bool write) {
    if (write) {
        if (o->u.rwlock.writer == t->uid) {
            if (!(o->u.rwlock.attr & RW_LOCK_RECURSIVE)) return SCE_KERNEL_ERROR_RW_LOCK_RECURSIVE;
            ++o->u.rwlock.writes;
            return 0;
        }
        if (o->u.rwlock.writer || o->u.rwlock.reads) return 1;
        o->u.rwlock.writer = t->uid, o->u.rwlock.writes = 1;
        return 0;
    }
    if (o->u.rwlock.writer && o->u.rwlock.writer != t->uid) return 1;
    ++o->u.rwlock.reads;
    return 0;
}
static uint32_t lock_rw_lock(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t timeout, bool write) {
    VitaObject* o = vita_object(p, uid, VITA_OBJ_RWLOCK);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;
    uint32_t r = try_rw_lock(t, o, write);
    if (r != 1) return r;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_RWLOCK, .object = uid, .a = write, .timeout = timeout,
                            .deadline = vita_deadline(p, timeout)});
    return 0;
}
uint32_t vita_kernel_lock_read_rw_lock(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t timeout) {
    return lock_rw_lock(p, t, uid, timeout, false);
}
uint32_t vita_kernel_lock_write_rw_lock(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t timeout) {
    return lock_rw_lock(p, t, uid, timeout, true);
}
uint32_t vita_kernel_unlock_read_rw_lock(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_RWLOCK);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;
    if (!o->u.rwlock.reads) return SCE_KERNEL_ERROR_RW_LOCK_UNLOCK_UDF;
    --o->u.rwlock.reads;
    return 0;
}
uint32_t vita_kernel_unlock_write_rw_lock(VitaProcess* p, VitaThread* t, uint32_t uid) {
    VitaObject* o = vita_object(p, uid, VITA_OBJ_RWLOCK);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;
    if (o->u.rwlock.writer != t->uid) return SCE_KERNEL_ERROR_RW_LOCK_NOT_OWNED;
    if (!--o->u.rwlock.writes) o->u.rwlock.writer = 0;
    return 0;
}
uint32_t vita_kernel_delete_rw_lock(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    return delete_object(p, uid, VITA_OBJ_RWLOCK, SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID);
}

/* sceKernelGetThreadInfo(uid, SceKernelThreadInfo*): uid 0 the calling thread's */
uint32_t vita_kernel_get_thread_info(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t info) {
    VitaThread* target = uid ? vita_thread(p, uid) : t;
    if (!target) return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    if (!info || !vita_mapped(p, info, 0x80)) return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    uint32_t size = vita_read32(p, info);
    if (size < 0x60 || size > 0x80) return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    memset(vita_ptr(p, info + 4), 0, size - 4);
    /* SceKernelThreadStatus: running 1, ready 2, waiting 8, dormant 0x10, dead 0x40 */
    uint32_t status = target == t ? 1 : target->state == VITA_THREAD_READY ? 2 : target->state == VITA_THREAD_WAITING ? 8
                      : target->state == VITA_THREAD_DORMANT ? 0x10 : 0x40;
    vita_write32(p, info + 4, 0x10005);
    memcpy(vita_ptr(p, info + 8), target->name, sizeof target->name);
    const uint32_t fields[] = {target->attr, status, target->entry, target->stack, target->stack_size,
                               target->priority, target->priority, 0, 0, 0, 0};
    for (unsigned i = 0; i < sizeof fields / sizeof *fields; ++i) vita_write32(p, info + 0x28 + i * 4, fields[i]);
    vita_write32(p, info + 0x5c, (uint32_t)target->exit_status);
    return 0;
}
uint32_t vita_kernel_get_thread_cpu_affinity_mask(VitaProcess* p, VitaThread* t, uint32_t thread_id) {
    (void)t;
    return thread_id && !vita_thread(p, thread_id) ? SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID : 0;
}
uint32_t vita_kernel_close_object(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
/* sceKernelWaitSignal(unused, unused, SceUInt32* timeout) and sceKernelSendSignal(thread): a one-bit mailbox on each
   thread (Vita3K's ThreadState::wait_for_signal and send_signal). A signal sent before the wait ends it at once, with
   the timeout untouched. libult's idle workers sleep here, so a call that returned at once made them find an empty
   queue and assert. */
uint32_t vita_kernel_wait_signal(VitaProcess* p, VitaThread* t, uint32_t unused0, uint32_t unused1, uint32_t timeout) {
    (void)unused0, (void)unused1;
    if (t->signal_pending) {
        t->signal_pending = false;
        return 0;
    }
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_SIGNAL, .timeout = timeout,
                            .deadline = vita_deadline(p, timeout)});
    return 0;
}
uint32_t vita_kernel_send_signal(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    if (!uid) return SCE_KERNEL_ERROR_ILLEGAL_THREAD_ID;
    VitaThread* target = vita_thread(p, uid);
    if (!target) return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    if (target->signal_pending) return SCE_KERNEL_ERROR_ALREADY_SENT;
    target->signal_pending = true;
    return 0;
}
/* SceUlobjDbg's two functions (0xd7f0f610, 0xf9c0f5da): libult's debug registration of its user-level objects. Vita3K
   leaves both unimplemented, as the debugger they report to isn't there */
uint32_t vita_kernel_ulobj_dbg(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}

bool vita_kernel_wait_try(VitaProcess* p, VitaThread* t, uint64_t now) {
    (void)now;
    VitaWait* w = &t->wait;
    switch (w->kind) {
        case VITA_WAIT_SEMA: {
            VitaObject* o = vita_object(p, w->object, VITA_OBJ_SEMA);
            if (!o) return t->cpu.r[0] = SCE_KERNEL_ERROR_WAIT_DELETE, true;
            if (o->u.sema.cancelled != w->b) return t->cpu.r[0] = SCE_KERNEL_ERROR_WAIT_CANCEL, true;
            if (o->u.sema.count < (int32_t)w->a) return false;
            o->u.sema.count -= (int32_t)w->a;
            return t->cpu.r[0] = 0, true;
        }
        case VITA_WAIT_RWLOCK: {
            VitaObject* o = vita_object(p, w->object, VITA_OBJ_RWLOCK);
            if (!o) return t->cpu.r[0] = SCE_KERNEL_ERROR_WAIT_DELETE, true;
            uint32_t r = try_rw_lock(t, o, w->a);
            if (r == 1) return false;
            return t->cpu.r[0] = r, true;
        }
        case VITA_WAIT_MUTEX: {
            VitaObject* o = vita_object(p, w->object, VITA_OBJ_MUTEX);
            if (!o) return t->cpu.r[0] = SCE_KERNEL_ERROR_WAIT_DELETE, true;
            if (w->b) return t->cpu.r[0] = 0, true; /* handed over by its owner (hand_over) */
            uint32_t r = try_lock(t, o, (int32_t)w->a);
            if (r == 1) return false;
            return t->cpu.r[0] = r, true;
        }
        case VITA_WAIT_THREAD_END: {
            VitaThread* target = vita_thread(p, w->object);
            if (target && target->state != VITA_THREAD_DEAD) return false;
            if (target && w->out) vita_write32(p, w->out, (uint32_t)target->exit_status);
            return t->cpu.r[0] = 0, true;
        }
        case VITA_WAIT_EVENT_FLAG: {
            VitaObject* o = vita_object(p, w->object, VITA_OBJ_EVENT_FLAG);
            if (!o) return t->cpu.r[0] = SCE_KERNEL_ERROR_WAIT_DELETE, true;
            if (!event_met(p, o, w->a, w->b, w->out)) return false;
            return t->cpu.r[0] = 0, true;
        }
        case VITA_WAIT_SIGNAL:
            if (!t->signal_pending) return false;
            t->signal_pending = false;
            return t->cpu.r[0] = 0, true;
        case VITA_WAIT_VBLANK: /* b set: the result is the one the call made before waiting */
            if (p->display.vblanks <= w->a) return false;
            if (!w->b) t->cpu.r[0] = 0;
            return true;
        case VITA_WAIT_COND: {
            if (!w->b) return false;
            VitaObject* m = vita_object(p, w->a, VITA_OBJ_MUTEX);
            if (!m) return t->cpu.r[0] = SCE_KERNEL_ERROR_WAIT_DELETE, true;
            if (m->u.mutex.owner && m->u.mutex.owner != t->uid) return false;
            m->u.mutex.owner = t->uid;
            m->u.mutex.count = (int32_t)w->out;
            return t->cpu.r[0] = 0, true;
        }
        default: return false;
    }
}

/* ---- memory blocks: CDRAM (the GPU's) in its range, every other type in user memory ---- */
uint32_t vita_kernel_alloc_mem_block(VitaProcess* p, VitaThread* t, uint32_t name_address, uint32_t type,
                                     uint32_t size) {
    (void)t;
    char name[32];
    name_of(p, name_address, name);
    bool cdram = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
    uint32_t address = vita_alloc(p, cdram ? &p->cdram : &p->user, size, cdram ? 0x40000 : 0x1000);
    VitaObject* o;
    uint32_t uid = address ? vita_object_new(p, VITA_OBJ_MEMBLOCK, name, &o) : 0;
    if (!uid) {
        if (address) vita_free(p, address);
        printf("vita: no memory for the %u-byte block \"%s\"\n", (unsigned)size, name);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }
    o->u.memblock.address = address, o->u.memblock.size = size, o->u.memblock.type = type;
    return uid;
}
uint32_t vita_kernel_get_mem_block_base(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t i) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_MEMBLOCK);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_UID;
    vita_write32(p, i, o->u.memblock.address);
    return 0;
}
uint32_t vita_kernel_free_mem_block(VitaProcess* p, VitaThread* t, uint32_t uid) {
    (void)t;
    VitaObject* o = vita_object(p, uid, VITA_OBJ_MEMBLOCK);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_UID;
    vita_free(p, o->u.memblock.address);
    vita_object_delete(p, uid);
    return 0;
}
uint32_t vita_kernel_find_mem_block_by_addr(VitaProcess* p, VitaThread* t, uint32_t address) {
    (void)t;
    for (uint32_t i = 0; i < VITA_OBJECTS; ++i) {
        VitaObject* o = &p->objects[i];
        if (o->type == VITA_OBJ_MEMBLOCK && address >= o->u.memblock.address &&
            address - o->u.memblock.address < o->u.memblock.size)
            return 0x40010001u + i * 2;
    }
    return SCE_KERNEL_ERROR_UNKNOWN_UID;
}
uint32_t vita_kernel_get_free_memory_size(VitaProcess* p, VitaThread* t, uint32_t info) {
    (void)t;
    /* SceKernelFreeMemorySizeInfo: size, then the user, CDRAM and physically continuous memory free */
    uint32_t user = VITA_USER_END - VITA_USER_BASE, cdram = VITA_CDRAM_END - VITA_CDRAM_BASE;
    for (unsigned i = 0; i < VITA_ALLOCATIONS; ++i) {
        const VitaAllocation* a = &p->allocations[i];
        if (!a->size) continue;
        if (a->address >= VITA_CDRAM_BASE && a->address < VITA_CDRAM_END)
            cdram -= a->size;
        else
            user -= a->size;
    }
    if (!vita_mapped(p, info, 16)) return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    vita_write32(p, info + 4, user);
    vita_write32(p, info + 8, cdram);
    vita_write32(p, info + 12, 0x1000000);
    return 0;
}

/* ---- time: the process's in microseconds, the wall clock's as the C library asks ---- */
static uint64_t process_us(VitaProcess* p) { return (plat_ns() - p->start_ns) / 1000u; }
uint64_t vita_kernel_get_process_time_wide(VitaProcess* p, VitaThread* t) {
    (void)t;
    return process_us(p);
}
uint32_t vita_kernel_get_process_time_low(VitaProcess* p, VitaThread* t) {
    (void)t;
    return (uint32_t)process_us(p);
}
uint32_t vita_kernel_libc_gettimeofday(VitaProcess* p, VitaThread* t, uint32_t tv, uint32_t address) {
    (void)t;
    int64_t now = plat_unix_ns();
    if (tv) {
        vita_write32(p, tv, (uint32_t)(now / 1000000000));
        vita_write32(p, tv + 4, (uint32_t)(now % 1000000000 / 1000));
    }
    if (address) vita_write64(p, address, 0); /* UTC, no daylight saving */
    return 0;
}
uint32_t vita_kernel_power_tick(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}

/* ---- modules: the game's own, the libraries merged into it, and the ones the game loads by path ---- */
#define MODULE_UID_BASE        0x40020000u
#define MODULE_INFO_SIZE       0x1b8u
#define SCE_ERROR_ERRNO_ENOENT 0x80010002u
#define SCE_KERNEL_ERROR_MODULEMGR_NO_MOD 0x8002d082u

static VitaLoadedModule* module_of(VitaProcess* p, uint32_t uid) {
    return uid >= MODULE_UID_BASE && uid - MODULE_UID_BASE < p->image.module_count
               ? &p->image.modules[uid - MODULE_UID_BASE]
               : NULL;
}
static bool same_name(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (snx_to_upper((unsigned char)*a) != snx_to_upper((unsigned char)*b)) return false;
    return *a == *b;
}
/* the module's start has run: its result is the load's unless it failed */
static uint32_t module_started(VitaProcess* p, VitaThread* t, void* context) {
    uint32_t uid = MODULE_UID_BASE + (uint32_t)(uintptr_t)context;
    (void)p;
    return (int32_t)t->cpu.r[0] < 0 ? t->cpu.r[0] : uid;
}
/* sceKernelLoadStartModule(path, args, argp, flags, option, *result): a module the game ships, found by its file's
   name among those loaded with it (nothing is loaded at run time: the code was compiled with the game's), started
   once; any other file is not there */
uint32_t vita_kernel_load_start_module(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t args, uint32_t argp,
                                       uint32_t flags, uint32_t option, uint32_t result) {
    (void)flags, (void)option;
    char path[256];
    const char* leaf = path;
    if (!vita_string(p, address, path, sizeof path)) return SCE_ERROR_ERRNO_ENOENT;
    for (const char* c = path; *c; ++c)
        if (*c == '/' || *c == ':' || *c == '\\') leaf = c + 1;
    for (unsigned i = 1; i < p->image.module_count; ++i) {
        VitaLoadedModule* m = &p->image.modules[i];
        if (!same_name(m->leaf, leaf)) continue;
        printf("vita: sceKernelLoadStartModule(\"%s\") -> module %u%s\n", path, i, p->module_started[i] ? "" : ", started");
        if (p->module_started[i]) return MODULE_UID_BASE + i;
        uint32_t start_args[2] = {args, argp};
        p->module_started[i] = true;
        if (result) snx_st_le32(vita_ptr(p, result), 0);
        vita_hle_guest_call(p, t, m->start, start_args, 2, module_started, (void*)(uintptr_t)i);
        return 0;
    }
    printf("vita: sceKernelLoadStartModule(\"%s\") -> no such file\n", path);
    return SCE_ERROR_ERRNO_ENOENT;
}
/* sceKernelGetModuleList(flags, *ids, *count): the started modules, the game's last */
uint32_t vita_kernel_get_module_list(VitaProcess* p, VitaThread* t, uint32_t flags, uint32_t ids, uint32_t count) {
    (void)t, (void)flags;
    uint32_t room, n = 0;
    if (!ids || !count) return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
    room = snx_ld_le32(vita_ptr(p, count));
    for (unsigned i = 1; i <= p->image.module_count && n < room; ++i) {
        unsigned module = i % p->image.module_count; /* 1, 2, ... then 0 */
        if (!p->module_started[module]) continue;
        snx_st_le32(vita_ptr(p, ids + 4 * n++), MODULE_UID_BASE + module);
    }
    snx_st_le32(vita_ptr(p, count), n);
    return 0;
}
/* sceKernelGetModuleInfo(uid, *info): SceKernelModuleInfo, 0x1b8 bytes, with its segments' places */
uint32_t vita_kernel_get_module_info(VitaProcess* p, VitaThread* t, uint32_t uid, uint32_t at) {
    (void)t;
    VitaLoadedModule* m = module_of(p, uid);
    unsigned char* info;
    if (!m || !at) return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
    if (!vita_mapped(p, at, MODULE_INFO_SIZE)) return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    info = vita_ptr(p, at);
    memset(info, 0, MODULE_INFO_SIZE);
    snx_st_le32(info, MODULE_INFO_SIZE);
    snx_st_le32(info + 4, uid);
    snprintf((char*)info + 0xc, 28, "%s", m->name);
    snx_st_le32(info + 0x2c, m->start);
    if (uid == MODULE_UID_BASE)
        snprintf((char*)info + 0x54, 256, "app0:eboot.bin");
    else
        snprintf((char*)info + 0x54, 256, "app0:sce_module/%s", m->leaf);
    for (unsigned k = 0; k < 4 && k < m->segment_count; ++k) {
        const VitaSegment* s = &p->image.segment[m->first_segment + k];
        unsigned char* out = info + 0x154 + 0x18 * k;
        snx_st_le32(out, 0x18);
        snx_st_le32(out + 4, s->flags);
        snx_st_le32(out + 8, s->address);
        snx_st_le32(out + 12, s->size);
        snx_st_le32(out + 16, s->size);
    }
    return 0;
}
