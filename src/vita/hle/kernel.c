/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceLibKernel, SceThreadmgr, SceSysmem and SceProcessmgr: threads, their synchronization, memory blocks and time, as
   the vitasdk headers (MIT) declare them */
#include "vita/hle/hle.h"
#include "common/utilities/common.h"
#include "snx_settings.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
#define SCE_KERNEL_ERROR_RW_LOCK_RECURSIVE    0x800281e2u
#define SCE_KERNEL_ERROR_RW_LOCK_NOT_OWNED    0x800281e4u
#define SCE_KERNEL_ERROR_RW_LOCK_UNLOCK_UDF   0x800281e5u
#define EVF_WAITMODE_OR        0x01u
#define EVF_WAITMODE_CLEAR_ALL 0x02u
#define EVF_WAITMODE_CLEAR_PAT 0x04u

static uint32_t arg(VitaProcess* p, VitaThread* t, unsigned n) { return vita_arg(p, t, n); }
static void name_of(VitaProcess* p, uint32_t address, char out[32]) { vita_string(p, address, out, 32); }

/* ---- threads ---- */
static uint32_t create_thread(VitaProcess* p, VitaThread* t) {
    char name[32];
    name_of(p, arg(p, t, 0), name);
    int32_t uid = vita_thread_new(p, name, arg(p, t, 1), arg(p, t, 2), arg(p, t, 3), arg(p, t, 4));
    return (uint32_t)uid;
}
static uint32_t start_thread(VitaProcess* p, VitaThread* t) {
    VitaThread* target = vita_thread(p, arg(p, t, 0));
    uint32_t size = arg(p, t, 1), block = arg(p, t, 2), copy = 0;
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
static uint32_t exit_thread(VitaProcess* p, VitaThread* t) {
    if (vita_in_module_start(t)) {
        t->cpu.r[0] = arg(p, t, 0);
        vita_hle_call_returned(p, t);
        return t->cpu.r[0];
    }
    vita_thread_end(p, t, (int32_t)arg(p, t, 0));
    return 0;
}
static void release_thread(VitaProcess* p, VitaThread* t) {
    vita_free(p, t->stack);
    if (t->module_tls) vita_free(p, t->module_tls);
    vita_object_delete(p, t->uid);
    t->state = VITA_THREAD_FREE;
}
static uint32_t exit_delete_thread(VitaProcess* p, VitaThread* t) {
    vita_thread_end(p, t, (int32_t)arg(p, t, 0));
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
static uint32_t delete_thread(VitaProcess* p, VitaThread* t) {
    VitaThread* target = vita_thread(p, arg(p, t, 0));
    if (!target) return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    if (target->state != VITA_THREAD_DORMANT && target->state != VITA_THREAD_DEAD)
        return SCE_KERNEL_ERROR_NOT_DORMANT;
    release_thread(p, target);
    return 0;
}
static uint32_t wait_thread_end(VitaProcess* p, VitaThread* t) {
    uint32_t uid = arg(p, t, 0);
    if (!vita_thread(p, uid)) return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_THREAD_END, .object = uid, .out = arg(p, t, 1),
                            .timeout = arg(p, t, 2), .deadline = vita_deadline(p, arg(p, t, 2))});
    return 0;
}
static uint32_t get_thread_id(VitaProcess* p, VitaThread* t) {
    (void)p;
    return t->uid;
}
static uint32_t delay_thread(VitaProcess* p, VitaThread* t) {
    uint64_t delay = (uint64_t)arg(p, t, 0) * 1000u;
    /* a delay of 0 gives the core away, as the console's scheduler does */
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_DELAY, .deadline = vita_now_ns() + (delay ? delay : 1)});
    return 0;
}
static uint32_t get_thread_current_priority(VitaProcess* p, VitaThread* t) {
    (void)p;
    return t->priority;
}
static uint32_t change_thread_priority(VitaProcess* p, VitaThread* t) {
    uint32_t uid = arg(p, t, 0);
    VitaThread* target = uid ? vita_thread(p, uid) : t;
    if (!target) return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    target->priority = arg(p, t, 1);
    return 0;
}
static uint32_t check_thread_stack(VitaProcess* p, VitaThread* t) {
    (void)p;
    return t->cpu.r[13] - t->stack; /* what is left */
}
static uint32_t get_tls_addr(VitaProcess* p, VitaThread* t) {
    uint32_t key = arg(p, t, 0);
    /* the slots are the first half of the thread's TLS area, the TLS register points at the second */
    return key < VITA_TLS_SIZE / 8 ? t->tls + key * 4 : 0;
}
static uint32_t exit_process(VitaProcess* p, VitaThread* t) {
    p->exited = true;
    p->exit_code = (int32_t)arg(p, t, 0);
    return 0;
}
static uint32_t get_process_id(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0x10005;
}
/* sceKernelGetProcessParam(): the SceProcessParam block the main module exports (NID 0x70fba1e7). libc.suprx's
   module_start calls this to reach the SceLibcParam (heap size, malloc replacement, main thread's stack and
   priority), so returning 0 here aborts libc's init and, through vita_module_start_next's chain, the whole run.
   p->image.process_param is the same block, already relocated into guest memory (image.c's exports() records it
   from the export with NID 0x70fba1e7, and process.c already reads it for the main thread's stack and priority).
   Vita3K's sceKernelGetProcessParam returns that same block. */
static uint32_t get_process_param(VitaProcess* p, VitaThread* t) {
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
static uint32_t create_sema(VitaProcess* p, VitaThread* t) {
    char name[32];
    VitaObject* o;
    name_of(p, arg(p, t, 0), name);
    uint32_t uid = vita_object_new(p, VITA_OBJ_SEMA, name, &o);
    if (!uid) return SCE_KERNEL_ERROR_NO_MEMORY;
    o->u.sema.attr = arg(p, t, 1);
    o->u.sema.count = (int32_t)arg(p, t, 2);
    o->u.sema.max = (int32_t)arg(p, t, 3);
    return uid;
}
static uint32_t wait_sema(VitaProcess* p, VitaThread* t) {
    uint32_t uid = arg(p, t, 0);
    VitaObject* o = vita_object(p, uid, VITA_OBJ_SEMA);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    int32_t need = (int32_t)arg(p, t, 1);
    if (o->u.sema.count >= need) {
        o->u.sema.count -= need;
        return 0;
    }
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_SEMA, .object = uid, .a = (uint32_t)need, .timeout = arg(p, t, 2),
                            .deadline = vita_deadline(p, arg(p, t, 2))});
    return 0;
}
static uint32_t poll_sema(VitaProcess* p, VitaThread* t) {
    VitaObject* o = vita_object(p, arg(p, t, 0), VITA_OBJ_SEMA);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    int32_t need = (int32_t)arg(p, t, 1);
    if (o->u.sema.count < need) return SCE_KERNEL_ERROR_SEMA_ZERO;
    o->u.sema.count -= need;
    return 0;
}
static uint32_t signal_sema(VitaProcess* p, VitaThread* t) {
    VitaObject* o = vita_object(p, arg(p, t, 0), VITA_OBJ_SEMA);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    int32_t n = (int32_t)arg(p, t, 1);
    if (o->u.sema.max > 0 && o->u.sema.count + n > o->u.sema.max) return SCE_KERNEL_ERROR_SEMA_OVF;
    o->u.sema.count += n;
    return 0;
}
static uint32_t delete_object(VitaProcess* p, VitaThread* t, uint32_t type, uint32_t unknown) {
    uint32_t uid = arg(p, t, 0);
    if (!vita_object(p, uid, type)) return unknown;
    vita_object_delete(p, uid);
    return 0;
}
static uint32_t delete_sema(VitaProcess* p, VitaThread* t) {
    return delete_object(p, t, VITA_OBJ_SEMA, SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID);
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
static uint32_t create_mutex(VitaProcess* p, VitaThread* t) {
    uint32_t uid = new_mutex(p, arg(p, t, 0), arg(p, t, 1), (int32_t)arg(p, t, 2), 0);
    return uid ? uid : SCE_KERNEL_ERROR_NO_MEMORY;
}
static uint32_t create_lw_mutex(VitaProcess* p, VitaThread* t) {
    uint32_t work = arg(p, t, 0);
    uint32_t uid = new_mutex(p, arg(p, t, 1), arg(p, t, 2), (int32_t)arg(p, t, 3), work);
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
static uint32_t lock(VitaProcess* p, VitaThread* t, uint32_t uid, VitaObject* o, uint32_t unknown) {
    if (!o) return unknown;
    int32_t count = (int32_t)arg(p, t, 1);
    uint32_t r = try_lock(t, o, count);
    if (r != 1) return r;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_MUTEX, .object = uid, .a = (uint32_t)count, .timeout = arg(p, t, 2),
                            .deadline = vita_deadline(p, arg(p, t, 2))});
    return 0;
}
static uint32_t try_lock_call(VitaProcess* p, VitaThread* t, VitaObject* o, uint32_t unknown, uint32_t busy) {
    if (!o) return unknown;
    uint32_t r = try_lock(t, o, (int32_t)arg(p, t, 1));
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
static uint32_t unlock(VitaProcess* p, VitaThread* t, uint32_t uid, VitaObject* o, uint32_t unknown,
                       uint32_t unlocked, uint32_t not_owned) {
    if (!o) return unknown;
    int32_t count = (int32_t)arg(p, t, 1);
    if (!o->u.mutex.owner) return unlocked;
    if (o->u.mutex.owner != t->uid) return not_owned;
    if (count <= 0 || count > o->u.mutex.count) return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    if (!(o->u.mutex.count -= count)) {
        o->u.mutex.owner = 0;
        hand_over(p, uid, o);
    }
    return 0;
}
static uint32_t lock_mutex(VitaProcess* p, VitaThread* t) {
    uint32_t uid = arg(p, t, 0);
    return lock(p, t, uid, vita_object(p, uid, VITA_OBJ_MUTEX), SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
}
static uint32_t try_lock_mutex(VitaProcess* p, VitaThread* t) {
    return try_lock_call(p, t, vita_object(p, arg(p, t, 0), VITA_OBJ_MUTEX), SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID,
                  0x80028145u /* SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN */);
}
static uint32_t unlock_mutex(VitaProcess* p, VitaThread* t) {
    uint32_t uid = arg(p, t, 0);
    return unlock(p, t, uid, vita_object(p, uid, VITA_OBJ_MUTEX), SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID,
           SCE_KERNEL_ERROR_MUTEX_UNLOCKED, SCE_KERNEL_ERROR_MUTEX_NOT_OWNED);
}
static uint32_t delete_mutex(VitaProcess* p, VitaThread* t) {
    return delete_object(p, t, VITA_OBJ_MUTEX, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
}
static uint32_t lock_lw_mutex(VitaProcess* p, VitaThread* t) {
    uint32_t uid;
    VitaObject* o = lw_mutex(p, arg(p, t, 0), &uid);
    return lock(p, t, uid, o, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
}
static uint32_t try_lock_lw_mutex(VitaProcess* p, VitaThread* t) {
    uint32_t uid;
    return try_lock_call(p, t, lw_mutex(p, arg(p, t, 0), &uid), SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID,
                  0x80028185u /* SCE_KERNEL_ERROR_LW_MUTEX_FAILED_TO_OWN */);
}
static uint32_t unlock_lw_mutex(VitaProcess* p, VitaThread* t) {
    uint32_t uid;
    VitaObject* o = lw_mutex(p, arg(p, t, 0), &uid);
    return unlock(p, t, uid, o, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID,
           SCE_KERNEL_ERROR_LW_MUTEX_UNLOCKED, SCE_KERNEL_ERROR_LW_MUTEX_NOT_OWNED);
}
static uint32_t delete_lw_mutex(VitaProcess* p, VitaThread* t) {
    uint32_t uid;
    if (!lw_mutex(p, arg(p, t, 0), &uid)) return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
    vita_object_delete(p, uid);
    vita_write32(p, arg(p, t, 0), 0);
    return 0;
}

/* ---- event flags ---- */
static uint32_t create_event_flag(VitaProcess* p, VitaThread* t) {
    char name[32];
    VitaObject* o;
    name_of(p, arg(p, t, 0), name);
    uint32_t uid = vita_object_new(p, VITA_OBJ_EVENT_FLAG, name, &o);
    if (!uid) return SCE_KERNEL_ERROR_NO_MEMORY;
    o->u.event_flag.attr = arg(p, t, 1);
    o->u.event_flag.pattern = arg(p, t, 2);
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
static uint32_t wait_event_flag(VitaProcess* p, VitaThread* t) {
    uint32_t uid = arg(p, t, 0), bits = arg(p, t, 1), mode = arg(p, t, 2), out = arg(p, t, 3);
    VitaObject* o = vita_object(p, uid, VITA_OBJ_EVENT_FLAG);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
    if (event_met(p, o, bits, mode, out)) return 0;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_EVENT_FLAG, .object = uid, .a = bits, .b = mode, .out = out,
                            .timeout = arg(p, t, 4), .deadline = vita_deadline(p, arg(p, t, 4))});
    return 0;
}
static uint32_t poll_event_flag(VitaProcess* p, VitaThread* t) {
    VitaObject* o = vita_object(p, arg(p, t, 0), VITA_OBJ_EVENT_FLAG);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
    return event_met(p, o, arg(p, t, 1), arg(p, t, 2), arg(p, t, 3)) ? 0 : SCE_KERNEL_ERROR_EVF_COND;
}
static uint32_t set_event_flag(VitaProcess* p, VitaThread* t) {
    VitaObject* o = vita_object(p, arg(p, t, 0), VITA_OBJ_EVENT_FLAG);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
    o->u.event_flag.pattern |= arg(p, t, 1);
    return 0;
}
static uint32_t clear_event_flag(VitaProcess* p, VitaThread* t) {
    VitaObject* o = vita_object(p, arg(p, t, 0), VITA_OBJ_EVENT_FLAG);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
    o->u.event_flag.pattern &= arg(p, t, 1);
    return 0;
}
static uint32_t delete_event_flag(VitaProcess* p, VitaThread* t) {
    return delete_object(p, t, VITA_OBJ_EVENT_FLAG, SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);
}

/* ---- condition variables: the waiter gives up its mutex, waits to be signaled (wait.b), then takes it again ---- */
static uint32_t create_cond(VitaProcess* p, VitaThread* t) {
    char name[32];
    VitaObject* o;
    name_of(p, arg(p, t, 0), name);
    uint32_t uid = vita_object_new(p, VITA_OBJ_COND, name, &o);
    if (!uid) return SCE_KERNEL_ERROR_NO_MEMORY;
    o->u.cond.mutex = arg(p, t, 2);
    return uid;
}
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
static uint32_t wait_cond(VitaProcess* p, VitaThread* t) { return cond_wait(p, t, arg(p, t, 0), arg(p, t, 1)); }
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
static uint32_t signal_cond_one(VitaProcess* p, VitaThread* t) { return cond_signal(p, arg(p, t, 0), false); }
static uint32_t signal_cond_all(VitaProcess* p, VitaThread* t) { return cond_signal(p, arg(p, t, 0), true); }
static uint32_t delete_cond(VitaProcess* p, VitaThread* t) {
    return delete_object(p, t, VITA_OBJ_COND, SCE_KERNEL_ERROR_UNKNOWN_COND_ID);
}

/* ---- LwConds: the work area's first word holds the UID of a cond, whose mutex is the LwMutex given at creation ---- */
static VitaObject* lw_cond(VitaProcess* p, uint32_t work, uint32_t* uid) {
    *uid = work ? vita_read32(p, work) : 0;
    return vita_object(p, *uid, VITA_OBJ_COND);
}
/* sceKernelCreateLwCond(work, name, attr, LwMutex work, options) */
static uint32_t create_lw_cond(VitaProcess* p, VitaThread* t) {
    uint32_t work = arg(p, t, 0), mutex_work = arg(p, t, 3);
    uint32_t mutex = mutex_work ? vita_read32(p, mutex_work) : 0;
    if (!vita_object(p, mutex, VITA_OBJ_MUTEX)) return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
    char name[32];
    VitaObject* o;
    name_of(p, arg(p, t, 1), name);
    uint32_t uid = vita_object_new(p, VITA_OBJ_COND, name, &o);
    if (!uid) return SCE_KERNEL_ERROR_NO_MEMORY;
    o->u.cond.mutex = mutex;
    vita_write32(p, work, uid);
    return 0;
}
/* sceKernelWaitLwCond(work, uint32_t* timeout): no timeout pointer waits for good */
static uint32_t wait_lw_cond(VitaProcess* p, VitaThread* t) {
    uint32_t uid, at = arg(p, t, 1);
    if (!lw_cond(p, arg(p, t, 0), &uid)) return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
    return cond_wait(p, t, uid, at && vita_mapped(p, at, 4) ? vita_read32(p, at) : 0);
}
static uint32_t signal_lw_cond(VitaProcess* p, VitaThread* t) {
    uint32_t uid;
    return lw_cond(p, arg(p, t, 0), &uid) ? cond_signal(p, uid, false) : SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
}
static uint32_t signal_lw_cond_all(VitaProcess* p, VitaThread* t) {
    uint32_t uid;
    return lw_cond(p, arg(p, t, 0), &uid) ? cond_signal(p, uid, true) : SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
}
/* sceKernelSignalLwCondTo(work, thread UID): that thread's wait, if it waits on the cond */
static uint32_t signal_lw_cond_to(VitaProcess* p, VitaThread* t) {
    uint32_t uid;
    if (!lw_cond(p, arg(p, t, 0), &uid)) return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        VitaThread* w = &p->threads[i];
        if (w->uid == arg(p, t, 1) && w->state == VITA_THREAD_WAITING && w->wait.kind == VITA_WAIT_COND &&
            w->wait.object == uid && !w->wait.b)
            w->wait.b = 1;
    }
    return 0;
}
static uint32_t delete_lw_cond(VitaProcess* p, VitaThread* t) {
    uint32_t uid;
    if (!lw_cond(p, arg(p, t, 0), &uid)) return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
    vita_object_delete(p, uid);
    vita_write32(p, arg(p, t, 0), 0);
    return 0;
}

/* ---- reader-writer locks ---- */
static uint32_t create_rw_lock(VitaProcess* p, VitaThread* t) {
    char name[32];
    VitaObject* o;
    name_of(p, arg(p, t, 0), name);
    uint32_t uid = vita_object_new(p, VITA_OBJ_RWLOCK, name, &o);
    if (!uid) return SCE_KERNEL_ERROR_NO_MEMORY;
    o->u.rwlock.attr = arg(p, t, 1);
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
static uint32_t lock_rw_lock(VitaProcess* p, VitaThread* t, bool write) {
    uint32_t uid = arg(p, t, 0);
    VitaObject* o = vita_object(p, uid, VITA_OBJ_RWLOCK);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;
    uint32_t r = try_rw_lock(t, o, write);
    if (r != 1) return r;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_RWLOCK, .object = uid, .a = write, .timeout = arg(p, t, 1),
                            .deadline = vita_deadline(p, arg(p, t, 1))});
    return 0;
}
static uint32_t lock_read_rw_lock(VitaProcess* p, VitaThread* t) { return lock_rw_lock(p, t, false); }
static uint32_t lock_write_rw_lock(VitaProcess* p, VitaThread* t) { return lock_rw_lock(p, t, true); }
static uint32_t unlock_read_rw_lock(VitaProcess* p, VitaThread* t) {
    VitaObject* o = vita_object(p, arg(p, t, 0), VITA_OBJ_RWLOCK);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;
    if (!o->u.rwlock.reads) return SCE_KERNEL_ERROR_RW_LOCK_UNLOCK_UDF;
    --o->u.rwlock.reads;
    return 0;
}
static uint32_t unlock_write_rw_lock(VitaProcess* p, VitaThread* t) {
    VitaObject* o = vita_object(p, arg(p, t, 0), VITA_OBJ_RWLOCK);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;
    if (o->u.rwlock.writer != t->uid) return SCE_KERNEL_ERROR_RW_LOCK_NOT_OWNED;
    if (!--o->u.rwlock.writes) o->u.rwlock.writer = 0;
    return 0;
}
static uint32_t delete_rw_lock(VitaProcess* p, VitaThread* t) {
    return delete_object(p, t, VITA_OBJ_RWLOCK, SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID);
}

/* sceKernelGetThreadInfo(uid, SceKernelThreadInfo*): uid 0 the calling thread's */
static uint32_t get_thread_info(VitaProcess* p, VitaThread* t) {
    uint32_t uid = arg(p, t, 0), info = arg(p, t, 1);
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
static uint32_t get_thread_cpu_affinity_mask(VitaProcess* p, VitaThread* t) {
    return arg(p, t, 0) && !vita_thread(p, arg(p, t, 0)) ? SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID : 0;
}
static uint32_t close_object(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
/* sceKernelWaitSignal(unused, unused, SceUInt32* timeout) and sceKernelSendSignal(thread): a one-bit mailbox on each
   thread (Vita3K's ThreadState::wait_for_signal and send_signal). A signal sent before the wait ends it at once, with
   the timeout untouched. libult's idle workers sleep here, so a call that returned at once made them find an empty
   queue and assert. */
static uint32_t wait_signal(VitaProcess* p, VitaThread* t) {
    if (t->signal_pending) {
        t->signal_pending = false;
        return 0;
    }
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_SIGNAL, .timeout = arg(p, t, 2),
                            .deadline = vita_deadline(p, arg(p, t, 2))});
    return 0;
}
static uint32_t send_signal(VitaProcess* p, VitaThread* t) {
    uint32_t uid = arg(p, t, 0);
    if (!uid) return SCE_KERNEL_ERROR_ILLEGAL_THREAD_ID;
    VitaThread* target = vita_thread(p, uid);
    if (!target) return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    if (target->signal_pending) return SCE_KERNEL_ERROR_ALREADY_SENT;
    target->signal_pending = true;
    return 0;
}
/* SceUlobjDbg's two functions (0xd7f0f610, 0xf9c0f5da): libult's debug registration of its user-level objects. Vita3K
   leaves both unimplemented, as the debugger they report to isn't there */
static uint32_t ulobj_dbg(VitaProcess* p, VitaThread* t) {
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
static uint32_t alloc_mem_block(VitaProcess* p, VitaThread* t) {
    char name[32];
    name_of(p, arg(p, t, 0), name);
    uint32_t type = arg(p, t, 1), size = arg(p, t, 2);
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
static uint32_t get_mem_block_base(VitaProcess* p, VitaThread* t) {
    VitaObject* o = vita_object(p, arg(p, t, 0), VITA_OBJ_MEMBLOCK);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_UID;
    vita_write32(p, arg(p, t, 1), o->u.memblock.address);
    return 0;
}
static uint32_t free_mem_block(VitaProcess* p, VitaThread* t) {
    uint32_t uid = arg(p, t, 0);
    VitaObject* o = vita_object(p, uid, VITA_OBJ_MEMBLOCK);
    if (!o) return SCE_KERNEL_ERROR_UNKNOWN_UID;
    vita_free(p, o->u.memblock.address);
    vita_object_delete(p, uid);
    return 0;
}
static uint32_t find_mem_block_by_addr(VitaProcess* p, VitaThread* t) {
    uint32_t address = arg(p, t, 0);
    for (uint32_t i = 0; i < VITA_OBJECTS; ++i) {
        VitaObject* o = &p->objects[i];
        if (o->type == VITA_OBJ_MEMBLOCK && address >= o->u.memblock.address &&
            address - o->u.memblock.address < o->u.memblock.size)
            return 0x40010001u + i * 2;
    }
    return SCE_KERNEL_ERROR_UNKNOWN_UID;
}
static uint32_t get_free_memory_size(VitaProcess* p, VitaThread* t) {
    /* SceKernelFreeMemorySizeInfo: size, then the user, CDRAM and physically continuous memory free */
    uint32_t info = arg(p, t, 0), user = VITA_USER_END - VITA_USER_BASE, cdram = VITA_CDRAM_END - VITA_CDRAM_BASE;
    for (unsigned i = 0; i < VITA_ALLOCATIONS; ++i) {
        const VitaAllocation* a = &p->allocations[i];
        if (!a->size) continue;
        if (a->address >= VITA_CDRAM_BASE && a->address < VITA_CDRAM_END)
            cdram -= a->size;
        else
            user -= a->size;
    }
    vita_write32(p, info + 4, user);
    vita_write32(p, info + 8, cdram);
    vita_write32(p, info + 12, 0x1000000);
    return 0;
}

/* ---- time: the process's in microseconds, the wall clock's as the C library asks ---- */
static uint64_t process_us(VitaProcess* p) { return (vita_now_ns() - p->start_ns) / 1000u; }
static uint32_t get_process_time_wide(VitaProcess* p, VitaThread* t) { return vita_return64(t, process_us(p)); }
static uint32_t get_process_time_low(VitaProcess* p, VitaThread* t) {
    (void)t;
    return (uint32_t)process_us(p);
}
static uint32_t libc_gettimeofday(VitaProcess* p, VitaThread* t) {
    uint32_t tv = arg(p, t, 0);
    struct timespec now;
#if defined(__ANDROID__) && __ANDROID_API__ < 29
    clock_gettime(CLOCK_REALTIME, &now);
#else
    timespec_get(&now, TIME_UTC);
#endif
    if (tv) {
        vita_write32(p, tv, (uint32_t)now.tv_sec);
        vita_write32(p, tv + 4, (uint32_t)(now.tv_nsec / 1000));
    }
    if (arg(p, t, 1)) vita_write64(p, arg(p, t, 1), 0); /* UTC, no daylight saving */
    return 0;
}
static uint32_t power_tick(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}

const VitaHle vita_hle_kernel[] = {
    {0xc5c11ee7u, "sceKernelCreateThread", create_thread},
    {0xf08de149u, "sceKernelStartThread", start_thread},
    {0x0c8a38e1u, "sceKernelExitThread", exit_thread},
    {0x1d17decfu, "sceKernelExitDeleteThread", exit_delete_thread},
    {0x1bbde3d9u, "sceKernelDeleteThread", delete_thread},
    {0xddb395a9u, "sceKernelWaitThreadEnd", wait_thread_end},
    {0x0fb972f9u, "sceKernelGetThreadId", get_thread_id},
    {0x4b675d05u, "sceKernelDelayThread", delay_thread},
    {0x9c0180e1u, "sceKernelDelayThreadCB", delay_thread},
    {0xc53a9848u, "sceKernelGetThreadCurrentPriority", get_thread_current_priority},
    {0xbd0139f2u, "sceKernelChangeThreadPriority", change_thread_priority},
    {0x84092fd0u, "sceKernelCheckThreadStack", check_thread_stack},
    {0xb295eb61u, "sceKernelGetTLSAddr", get_tls_addr},
    {0x7595d9aau, "sceKernelExitProcess", exit_process},
    {0x9dcb4b7au, "sceKernelGetProcessId", get_process_id},
    {0x2be3e066u, "sceKernelGetProcessParam", get_process_param},
    {0x1bd67366u, "sceKernelCreateSema", create_sema},
    {0x0c7b834bu, "sceKernelWaitSema", wait_sema},
    {0x174692b4u, "sceKernelWaitSemaCB", wait_sema},
    {0x866ef048u, "sceKernelPollSema", poll_sema},
    {0xe6b761d1u, "sceKernelSignalSema", signal_sema},
    {0xdb32948au, "sceKernelDeleteSema", delete_sema},
    {0xed53334au, "sceKernelCreateMutex", create_mutex},
    {0x1d8d7945u, "sceKernelLockMutex", lock_mutex},
    {0x72fc1f54u, "sceKernelTryLockMutex", try_lock_mutex},
    {0x1a372ec8u, "sceKernelUnlockMutex", unlock_mutex},
    {0xcb78710du, "sceKernelDeleteMutex", delete_mutex},
    {0xda6ec8efu, "sceKernelCreateLwMutex", create_lw_mutex},
    {0x46e7be7bu, "sceKernelLockLwMutex", lock_lw_mutex},
    {0xa6a2c915u, "sceKernelTryLockLwMutex", try_lock_lw_mutex},
    {0x120afc8cu, "sceKernelUnlockLwMutex2", unlock_lw_mutex},
    {0x91fa6614u, "sceKernelUnlockLwMutex", unlock_lw_mutex},
    {0x244e76d2u, "sceKernelDeleteLwMutex", delete_lw_mutex},
    {0x8516d040u, "sceKernelCreateEventFlag", create_event_flag},
    {0x83c0e2afu, "sceKernelWaitEventFlag", wait_event_flag},
    {0x1fbb0fe1u, "sceKernelPollEventFlag", poll_event_flag},
    {0xec94dff7u, "sceKernelSetEventFlag", set_event_flag},
    {0x4cb87ca7u, "sceKernelClearEventFlag", clear_event_flag},
    {0x5840162cu, "sceKernelDeleteEventFlag", delete_event_flag},
    {0x50572fdau, "sceKernelCreateCond", create_cond},
    {0xc88d44adu, "sceKernelWaitCond", wait_cond},
    {0x6ed2e2dcu, "sceKernelSignalCond", signal_cond_one},
    {0xc2e7ac22u, "sceKernelSignalCondAll", signal_cond_all},
    {0x879e6ebdu, "sceKernelDeleteCond", delete_cond},
    {0x48c7eae6u, "sceKernelCreateLwCond", create_lw_cond},
    {0xe1878282u, "sceKernelWaitLwCond", wait_lw_cond},
    {0x3ac63b9au, "sceKernelSignalLwCond", signal_lw_cond},
    {0xe5241a0cu, "sceKernelSignalLwCondAll", signal_lw_cond_all},
    {0xfc1a48ebu, "sceKernelSignalLwCondTo", signal_lw_cond_to},
    {0x721f6cb3u, "sceKernelDeleteLwCond", delete_lw_cond},
    {0x8fa54b07u, "sceKernelWaitLwCondCB", wait_lw_cond},
    {0x3148c6b6u, "sceKernelLockLwMutexCB", lock_lw_mutex},
    {0x2bdaa524u, "sceKernelLockMutexCB", lock_mutex},
    {0x4ce42ce2u, "sceKernelWaitCondCB", wait_cond},
    {0xe737b1dfu, "sceKernelWaitEventFlagCB", wait_event_flag},
    {0x2d4a62b7u, "sceKernelLockReadRWLockCB", lock_read_rw_lock},
    {0xa4777082u, "sceKernelLockWriteRWLockCB", lock_write_rw_lock},
    {0xc54941edu, "sceKernelWaitThreadEndCB", wait_thread_end},
    {0xb9d5ebdeu, "sceKernelAllocMemBlock", alloc_mem_block},
    {0xb8ef5818u, "sceKernelGetMemBlockBase", get_mem_block_base},
    {0xa91e15eeu, "sceKernelFreeMemBlock", free_mem_block},
    {0xa33b99d1u, "sceKernelFindMemBlockByAddr", find_mem_block_by_addr},
    {0x87cc580bu, "sceKernelGetFreeMemorySize", get_free_memory_size},
    {0xb110c123u, "sceKernelGetProcessTimeWide", get_process_time_wide},
    {0x4c4672bfu, "sceKernelGetProcessTime", get_process_time_wide},
    {0xe9f973b1u, "sceKernelGetProcessTimeLow", get_process_time_low},
    {0xf4ee4fa9u, "sceKernelGetSystemTimeWide", get_process_time_wide},
    {0x4b879059u, "sceKernelLibcGettimeofday", libc_gettimeofday},
    {0x2252890cu, "sceKernelPowerTick", power_tick},
    {0x8667951du, "sceKernelCreateRWLock", create_rw_lock},
    {0xfa670f0fu, "sceKernelLockReadRWLock", lock_read_rw_lock},
    {0x67a187bbu, "sceKernelLockWriteRWLock", lock_write_rw_lock},
    {0x3ef91145u, "sceKernelUnlockReadRWLock", unlock_read_rw_lock},
    {0xb4151397u, "sceKernelUnlockWriteRWLock", unlock_write_rw_lock},
    {0xe73649cau, "sceKernelDeleteRWLock", delete_rw_lock},
    {0x8d9c5461u, "sceKernelGetThreadInfo", get_thread_info},
    {0xf1ae5654u, "sceKernelGetThreadCpuAffinityMask", get_thread_cpu_affinity_mask},
    {0x03e23af6u, "sceKernelCloseMutex", close_object},
    {0xadca94e5u, "sceKernelWaitSignal", wait_signal},
    {0x24460bb3u, "sceKernelWaitSignalCB", wait_signal},
    {0xd4c367b2u, "sceKernelSendSignal", send_signal},
    {0xd7f0f610u, "SceUlobjDbg_D7F0F610", ulobj_dbg},
    {0xf9c0f5dau, "SceUlobjDbg_F9C0F5DA", ulobj_dbg},
    {0, NULL, NULL},
};