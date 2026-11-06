/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Serialized, bounded single-core thread contexts; no host threads or JIT.
   SVC register layout from Azahar kernel/svc_wrapper.h (revision in 3DS.md). */
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "3ds/services/gsp.h"
#include "3ds/services/dsp.h"
#include "3ds/services/y2r.h"
#include "3ds/services/file.h"
#include "3ds/services/hid.h"
#include "3ds/services/uds.h"
#include "common/utilities/common.h"
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002 /* Windows 10 1803 and later */
#endif
#else
#include <time.h>
#endif
CtrThread* snx_ctr_thread(SnxCtrProcess* p) { return &p->objects[p->current_thread].data.thread; }
void snx_ctr_object_release(SnxCtrProcess* p, uint32_t id) {
    if (--p->objects[id].refs) return;
    /* the last reference: what the object holds goes with it */
    CtrObject* o = &p->objects[id];
    if (o->type == CTR_OBJ_SHARED && o->data.shared.address) /* the owner range is private read-write again */
        (void)snx_ctr_protect(p->space, o->data.shared.address, o->data.shared.size, SNX_CTR_READ | SNX_CTR_WRITE);
    if (o->type == CTR_OBJ_FILE && !o->data.file.closed) snx_ctr_file_release(p, id);
    if (o->type == CTR_OBJ_SESSION && o->data.session.service == SNX_CTR_SERVICE_GSP) snx_ctr_gsp_session_closed(p, id);
    p->objects[id] = (CtrObject){0};
}
void snx_ctr_thread_block(SnxCtrProcess* p, uint32_t id, uint32_t address) {
    CtrThread* t = snx_ctr_thread(p);
    t->waiting_object = id;
    t->waiting_address = address;
    t->wait_more_count = t->wait_multiple = t->wait_all = 0;
    t->wait_seq = ++p->sched_seq;
    ++p->objects[id].refs;
}
void snx_ctr_thread_ready(SnxCtrProcess* p, uint32_t id) { p->objects[id].data.thread.ready_seq = ++p->sched_seq; }
uint64_t snx_ctr_now_ns(void) { return (uint64_t)(snx_seconds() * 1e9); }
/* Every thread's effective priority: its own, improved to the best effective priority of the threads waiting on a
   mutex it holds, transitively (Azahar Mutex::UpdatePriority and Thread::UpdatePriority). A chain is at most as long
   as the threads, and each round that changes nothing ends it. */
void snx_ctr_thread_priorities(SnxCtrProcess* p, uint32_t* priority) {
    for (uint32_t i = 1; i < CTR_OBJECTS; ++i)
        priority[i] = p->objects[i].type == CTR_OBJ_THREAD ? p->objects[i].data.thread.priority : 64;
    for (uint32_t round = 0; round < CTR_THREAD_LIMIT; ++round) {
        bool changed = false;
        for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
            const CtrThread* t = &p->objects[i].data.thread;
            if (p->objects[i].type != CTR_OBJ_THREAD || t->dead || !t->waiting_object) continue;
            for (uint32_t j = 0; j <= t->wait_more_count; ++j) {
                const CtrObject* m = &p->objects[j ? t->wait_more[j - 1] : t->waiting_object];
                if (m->type != CTR_OBJ_MUTEX || !m->data.mutex.depth || m->data.mutex.owner == i) continue;
                if (priority[i] < priority[m->data.mutex.owner]) {
                    priority[m->data.mutex.owner] = priority[i];
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }
}
uint32_t snx_ctr_thread_priority(SnxCtrProcess* p, uint32_t id) {
    uint32_t priority[CTR_OBJECTS];
    snx_ctr_thread_priorities(p, priority);
    return priority[id];
}
static uint32_t tls(SnxCtrProcess* p, uint32_t* address) {
    unsigned char zero[4096] = {0};
    for (uint32_t slot = 0; slot < CTR_THREAD_LIMIT; ++slot) {
        uint32_t va = SNX_CTR_INITIAL_TLS + slot * 512;
        bool used = false;
        for (uint32_t i = 1; i < CTR_OBJECTS; ++i)
            if (p->objects[i].type == CTR_OBJ_THREAD && !p->objects[i].data.thread.dead &&
                p->objects[i].data.thread.context.tpidruro == va)
                used = true;
        if (used) continue;
        uint32_t page = va & ~4095u;
        /* a new TLS page comes from the BASE region, not the application's pool (Azahar's
           Process::AllocateThreadLocalStorage): a game that commits all of its pool still starts threads */
        if (snx_ctr_query(p->space, page, NULL, NULL) != SNX_CTR_MEM_OK) {
            uint32_t physical = snx_ctr_shared_pages(p, 4096);
            if (!physical || snx_ctr_map(p->space, page, physical, 4096, 3) != SNX_CTR_MEM_OK) return 0xd86007f3u;
            if (snx_ctr_write_ram(p->space, page, zero, sizeof zero) != SNX_CTR_MEM_OK) {
                (void)snx_ctr_unmap(p->space, page, 4096);
                return 0xe0e01bf5u;
            }
            *address = va;
            return 0;
        }
        if (snx_ctr_write_ram(p->space, va, zero, 512) != SNX_CTR_MEM_OK) return 0xe0e01bf5u;
        *address = va;
        return 0;
    }
    return 0xc860180cu; /* bounded implementation thread/TLS quota */
}
uint32_t snx_ctr_thread_create(SnxCtrProcess* p, const uint32_t* r, uint32_t* out) {
    /* Azahar svc.cpp CreateThread: priority r0 (lowest 63, and no better than the APPLICATION limit's 0x18), entry
       r1, argument r2, stack top r3, processor r4: -2 is the process's ideal processor and -1 any, both core 0; 0 to 3
       are taken as asked. The console refuses core 3 to an application, and core 2 unless its ExHeader allows it
       (0xD9001BEA); Azahar accepts both (its TODO in svc.cpp says so), and so does this, following Azahar. Every
       core's threads share the one serialized scheduler: a syscore thread runs whenever its priority allows, without
       core 1's CPU-time limit. */
    *out = 0;
    if (r[0] > 63) return 0xe0e01bfdu;                 /* ResultOutOfRange */
    if (r[0] < CTR_PRIORITY_LIMIT) return 0xd9001beau; /* ResultNotAuthorized */
    int32_t processor = (int32_t)r[4];
    if (processor < -2 || processor > 3) return 0xe0e01bfdu;
    unsigned char instruction[4];
    uint32_t entry = r[1] & ~1u, width = r[1] & 1 ? 2 : 4;
    if ((entry & (width - 1)) || snx_ctr_fetch(p->space, entry, instruction, width) != SNX_CTR_MEM_OK)
        return 0xe0e01bf5u;
    if ((r[3] & 7) || r[3] < 4 || snx_ctr_read_ram(p->space, r[3] - 4, instruction, 4) != SNX_CTR_MEM_OK)
        return 0xe0e01bf5u;
    unsigned permissions;
    if (snx_ctr_query(p->space, r[3] - 4, NULL, &permissions) != SNX_CTR_MEM_OK || !(permissions & SNX_CTR_WRITE))
        return 0xe0e01bf5u;
    uint32_t result = snx_ctr_object_create(p, CTR_OBJ_THREAD, out);
    if (result) return result;
    uint32_t id = snx_ctr_handle_object(p, *out), address;
    result = tls(p, &address);
    if (result) {
        snx_ctr_handle_close(p, *out);
        *out = 0;
        return result;
    }
    CtrThread* t = &p->objects[id].data.thread;
    t->context.r[0] = r[2];
    t->context.r[13] = r[3];
    t->context.r[15] = entry;
    t->context.cpsr = 0x10 | (r[1] & 1 ? SNX_ARM_T : 0);
    t->context.tpidruro = address;
    /* default NaN, flush-to-zero and round toward zero, as Azahar's svcCreateThread sets every new thread's FPSCR */
    t->context.fpscr = 0x03c00000u;
    t->context.rt = p->cpu.state.cpu.rt;
    t->priority = r[0];
    t->core = processor < 0 ? 0 : (uint32_t)processor; /* -2 the process's ideal processor (0), -1 any: core 0 */
    snx_ctr_thread_ready(p, id);
    ++p->objects[id].refs; /* live thread owns a root, even if its handle closes */
    return 0;
}
static bool ready(SnxCtrProcess* p, uint32_t id, uint32_t thread) {
    CtrObject* o = &p->objects[id];
    if (o->type == CTR_OBJ_EVENT) return !!o->data.event.signaled;
    if (o->type == CTR_OBJ_TIMER) return !!o->data.timer.signaled;
    if (o->type == CTR_OBJ_THREAD) return !!o->data.thread.dead;
    if (o->type == CTR_OBJ_SEMAPHORE) return !!o->data.semaphore.count;
    return o->type == CTR_OBJ_MUTEX && (!o->data.mutex.depth || o->data.mutex.owner == thread);
}
static void acquire(SnxCtrProcess* p, uint32_t id, uint32_t thread) {
    CtrObject* o = &p->objects[id];
    if (o->type == CTR_OBJ_EVENT && o->data.event.reset_type == CTR_RESET_ONESHOT) o->data.event.signaled = 0;
    if (o->type == CTR_OBJ_TIMER && o->data.timer.reset_type == CTR_RESET_ONESHOT) o->data.timer.signaled = 0;
    if (o->type == CTR_OBJ_SEMAPHORE && o->data.semaphore.count) --o->data.semaphore.count;
    if (o->type == CTR_OBJ_MUTEX) {
        o->data.mutex.owner = thread;
        ++o->data.mutex.depth;
    }
}
/* Wait-capable kernel objects (Azahar's WaitObjects that exist here); a process stops the run, waiting on one is not
   modelled */
bool snx_ctr_waitable(uint32_t type) {
    return type == CTR_OBJ_EVENT || type == CTR_OBJ_MUTEX || type == CTR_OBJ_THREAD || type == CTR_OBJ_SEMAPHORE ||
           type == CTR_OBJ_TIMER;
}
SnxCtrSvcResult snx_ctr_wait_many(SnxCtrProcess* p, uint32_t* r) {
    /* WaitSynchronizationN (Azahar svc.cpp): nanoseconds r0 low, r4 high, handles at r1, count r2, wait_all r3 (any
       nonzero), the index out at r1. A negative count is out of range; this model reads at most 64 handles. */
    if ((int32_t)r[2] < 0) {
        r[0] = 0xe0e01bfdu; /* ResultOutOfRange */
        r[1] = UINT32_MAX;
        return SNX_CTR_SVC_RETURN;
    }
    if (r[2] > 64) return SNX_CTR_SVC_UNSUPPORTED;
    bool wait_all = !!r[3];
    if (!r[2]) {
        /* no handles: waiting for all of them is done at once; waiting for any lasts until the deadline */
        r[1] = UINT32_MAX;
        if (wait_all || (!r[0] && !r[4])) {
            r[0] = wait_all ? 0u : CTR_TIMED_OUT;
            return SNX_CTR_SVC_RETURN;
        }
        CtrThread* t = snx_ctr_thread(p);
        snx_ctr_thread_deadline(p, r[0], r[4]);
        if (!t->wake_ns) t->wake_ns = UINT64_MAX; /* -1: never */
        t->wait_nothing = 1;
        return SNX_CTR_SVC_BLOCKED;
    }
    unsigned char bytes[256];
    uint32_t ids[64], count = r[2], found = UINT32_MAX;
    if (snx_ctr_read_ram(p->space, r[1], bytes, count * 4) != SNX_CTR_MEM_OK) {
        r[0] = 0xd8e007f6u; /* ResultInvalidPointer */
        r[1] = UINT32_MAX;
        return SNX_CTR_SVC_RETURN;
    }
    bool all = true;
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char* b = bytes + i * 4;
        uint32_t handle = snx_ld_le32(b);
        ids[i] = snx_ctr_handle_object(p, handle);
        uint32_t type = ids[i] ? p->objects[ids[i]].type : CTR_OBJ_FREE;
        if (type == CTR_OBJ_PROCESS) return SNX_CTR_SVC_UNSUPPORTED;
        if (!snx_ctr_waitable(type)) { /* no object, or one that cannot be waited on */
            r[0] = 0xd8e007f7u;
            r[1] = UINT32_MAX;
            return SNX_CTR_SVC_RETURN;
        }
        if (type == CTR_OBJ_MUTEX && p->objects[ids[i]].data.mutex.depth == UINT32_MAX) return SNX_CTR_SVC_UNSUPPORTED;
        bool available = ready(p, ids[i], p->current_thread);
        if (available && found == UINT32_MAX) found = i;
        all = all && available;
    }
    /* a handle given twice is waited on and acquired once per occurrence, as Azahar's object list does */
    if (wait_all ? all : found != UINT32_MAX) {
        for (uint32_t i = 0; i < count; ++i)
            if (wait_all || i == found) acquire(p, ids[i], p->current_thread);
        r[1] = wait_all ? UINT32_MAX : found;
        r[0] = 0;
        return SNX_CTR_SVC_RETURN;
    }
    if (!r[0] && !r[4]) {
        r[0] = CTR_TIMED_OUT;
        r[1] = UINT32_MAX;
        return SNX_CTR_SVC_RETURN;
    }
    CtrThread* t = snx_ctr_thread(p);
    for (uint32_t i = 0; i < count; ++i) ++p->objects[ids[i]].refs;
    t->waiting_object = ids[0];
    t->wait_multiple = 1;
    t->wait_all = wait_all;
    t->wait_seq = ++p->sched_seq;
    t->wait_more_count = count - 1;
    for (uint32_t i = 1; i < count; ++i) t->wait_more[i - 1] = ids[i];
    snx_ctr_thread_deadline(p, r[0], r[4]); /* nanoseconds: r0 low, r4 high */
    return SNX_CTR_SVC_BLOCKED;
}
void snx_ctr_wake_waiters(SnxCtrProcess* p, uint32_t object) {
    /* Highest (effective) priority eligible waiter first, the one waiting longest among equals, as Azahar's wait lists
       keep them. One-shot events and mutexes are acquired on completion; later waiters recheck the resulting state. */
    uint32_t priority[CTR_OBJECTS];
    snx_ctr_thread_priorities(p, priority);
    for (;;) {
        uint32_t best = 0, index = 0;
        for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
            if (p->objects[i].type != CTR_OBJ_THREAD) continue;
            CtrThread* t = &p->objects[i].data.thread;
            if (!t->waiting_object || t->dead) continue;
            bool includes = t->waiting_object == object, all = true;
            uint32_t first = UINT32_MAX;
            for (uint32_t j = 0; j <= t->wait_more_count; ++j) {
                uint32_t id = j ? t->wait_more[j - 1] : t->waiting_object;
                includes = includes || id == object;
                bool available = ready(p, id, i);
                if (available && first == UINT32_MAX) first = j;
                all = all && available;
            }
            if (!includes || (t->wait_all ? !all : first == UINT32_MAX)) continue;
            if (!best || priority[i] < priority[best] ||
                (priority[i] == priority[best] && t->wait_seq < p->objects[best].data.thread.wait_seq)) {
                best = i;
                index = first;
            }
        }
        if (!best) return;
        CtrThread* t = &p->objects[best].data.thread;
        for (uint32_t j = 0; j <= t->wait_more_count; ++j)
            if (t->wait_all || j == index) acquire(p, j ? t->wait_more[j - 1] : t->waiting_object, best);
        t->context.r[0] = 0;
        if (t->wait_multiple) t->context.r[1] = t->wait_all ? UINT32_MAX : index;
        t->context.exit = t->context.syndrome = 0;
        for (uint32_t j = 0; j <= t->wait_more_count; ++j)
            snx_ctr_object_release(p, j ? t->wait_more[j - 1] : t->waiting_object);
        t->waiting_object = t->waiting_address = t->wait_more_count = t->wait_multiple = t->wait_all = 0;
        t->wake_ns = 0;
        snx_ctr_thread_ready(p, best);
    }
}
static uint64_t now_ns(void) { return snx_ctr_now_ns(); }
void snx_ctr_thread_deadline(SnxCtrProcess* p, uint32_t low, uint32_t high) {
    /* As Azahar's Thread::WakeAfterDelay: -1 schedules no wakeup; other negative delays are already due */
    int64_t ns = (int64_t)((uint64_t)high << 32 | low);
    uint64_t now = now_ns();
    CtrThread* t = snx_ctr_thread(p);
    if (ns == -1)
        t->wake_ns = 0;
    else if (ns <= 0)
        t->wake_ns = now ? now : 1;
    else
        t->wake_ns = (uint64_t)ns > UINT64_MAX - 1 - now ? UINT64_MAX - 1 : now + (uint64_t)ns;
}
/* A waiting thread's deadline passed: its wait ends with Timeout, -1 as WaitSynchronizationN's index (Azahar's
   SVC_SyncCallback), and the objects it waited on lose the references the wait held */
static void time_out(SnxCtrProcess* p, uint32_t id) {
    CtrThread* t = &p->objects[id].data.thread;
    t->context.r[0] = CTR_TIMED_OUT;
    if (t->wait_multiple) t->context.r[1] = UINT32_MAX;
    t->context.exit = t->context.syndrome = 0;
    for (uint32_t j = 0; j <= t->wait_more_count; ++j)
        snx_ctr_object_release(p, j ? t->wait_more[j - 1] : t->waiting_object);
    t->waiting_object = t->waiting_address = t->wait_more_count = t->wait_multiple = t->wait_all = 0;
    t->wake_ns = 0;
    snx_ctr_thread_ready(p, id);
}
void snx_ctr_timer_signal(SnxCtrProcess* p, uint32_t id, uint64_t now) {
    /* Azahar kernel/timer.cpp, Timer::Signal: signaled, every waiter woken, a pulse cleared again at once; an interval
       keeps the period from the deadline, not from when it was seen. A deadline more than a period behind (the host
       was busy) restarts from now instead of firing the missed periods back to back. */
    CtrObject* o = &p->objects[id];
    o->data.timer.signaled = 1;
    snx_ctr_wake_waiters(p, id);
    if (o->data.timer.reset_type == CTR_RESET_PULSE) o->data.timer.signaled = 0;
    if (!o->data.timer.interval) {
        o->data.timer.deadline = 0;
        return;
    }
    uint64_t interval = o->data.timer.interval, deadline = o->data.timer.deadline;
    uint64_t next = interval > UINT64_MAX - deadline ? UINT64_MAX : deadline + interval; /* saturating */
    o->data.timer.deadline = next > now ? next : now > UINT64_MAX - interval ? UINT64_MAX : now + interval;
}
SnxCtrSvcResult snx_ctr_thread_exit(SnxCtrProcess* p) {
    /* Azahar Thread::Stop: the thread is dead, which satisfies its waiters, and the mutexes it holds are released;
       its TLS slot is free again (tls() skips dead threads). The live thread's root reference goes. */
    uint32_t id = p->current_thread;
    CtrThread* t = snx_ctr_thread(p);
    t->dead = 1;
    t->wake_ns = 0;
    for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
        CtrObject* o = &p->objects[i];
        if (o->type != CTR_OBJ_MUTEX || o->data.mutex.owner != id || !o->data.mutex.depth) continue;
        o->data.mutex.owner = o->data.mutex.depth = 0;
        snx_ctr_wake_waiters(p, i);
    }
    snx_ctr_wake_waiters(p, id);
    snx_ctr_object_release(p, id);
    for (uint32_t i = 1; i < CTR_OBJECTS; ++i)
        if (p->objects[i].type == CTR_OBJ_THREAD && !p->objects[i].data.thread.dead) return SNX_CTR_SVC_BLOCKED;
    return SNX_CTR_SVC_EXIT; /* the process's last thread */
}
static void sleep_until(uint64_t deadline) {
    uint64_t now = now_ns();
    if (deadline <= now) return;
#if defined(_WIN32)
    /* Sleep() rounds to the system tick (15.6 ms unless raised), far longer than a 4.9 ms audio frame: a high-resolution
       waitable timer (Windows 10 1803 and later) waits the exact time, Sleep() remains where it is missing */
    static HANDLE timer;
    static bool tried;
    if (!tried) {
        tried = true;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((deadline - now) / 100); /* relative, in 100 ns units */
    if (timer && due.QuadPart && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        Sleep((DWORD)((deadline - now) / 1000000 ? (deadline - now) / 1000000 : 1));
#else
    struct timespec ts = {(time_t)((deadline - now) / 1000000000), (long)((deadline - now) % 1000000000)};
    nanosleep(&ts, NULL);
#endif
}
/* The cores take turns, each scheduling a slice of its own as Azahar's per-core schedulers do (a slice ends at an SVC
   or after SNX_CTR_POLL_BLOCKS blocks): a busy thread on the system core no longer keeps a waiting one of the
   application core from running, as it never does on the console. A core with no thread to run gives its turn to the
   next. On the core whose turn it is: the runnable thread of best effective priority (inheritance from mutex waiters
   included). Among equals the core's own last one keeps it (Azahar's PopNextReadyThread: only a strictly better
   priority takes it), else the one runnable longest (ready_seq: Azahar's per-priority ready queue, a preempted thread keeping its place at the front). Time is
   part of it: a sleeping thread is runnable again at its deadline, a waiting one times out at its own, timers fire,
   and the display's vblank and the DSP's audio frame are delivered when due, which wakes the threads waiting on GSP's
   and the DSP's interrupt events. A thread that yielded runs only when no other can. When no thread can run but a
   deadline is ahead, the host sleeps until the first of them; only a process with nothing left to wake for is
   blocked. Waking from a vblank or an audio frame that leaves nothing runnable returns SNX_CTR_SCHED_IDLE, so a
   process that only waits on the display and the DSP still lets its caller see the window close; a thread's or a
   timer's deadline is waited for here. The dispatcher also calls this every SNX_CTR_POLL_BLOCKS blocks, so a woken
   thread of better priority preempts one that makes no SVC, as an interrupt would. */
SnxCtrSchedule snx_ctr_schedule(SnxCtrProcess* p) {
    /* The current thread's registers go to its context first: a vblank already due wakes the threads waiting on
       GSP's events, the current one included, by editing their saved contexts, and the thread chosen below always
       resumes from its context. Only the plain CPU prefix is saved. The checked adapter/callback context never
       moves. A context switch or an idle wait loses exclusives, the current thread carrying on keeps them. A thread
       that exited has no context to keep: its object may already be free. */
    uint32_t current = p->current_thread, exclusive = p->cpu.state.cpu.exclusive_valid;
    p->cpu.state.cpu.exclusive_valid = 0;
    if (p->objects[current].type == CTR_OBJ_THREAD && !p->objects[current].data.thread.dead)
        snx_ctr_thread(p)->context = p->cpu.state.cpu;
    bool slept_for_device = false;
    uint32_t priority[CTR_OBJECTS];
    for (;;) {
        uint64_t now = now_ns(), earliest = UINT64_MAX;
        snx_ctr_uds_poll(p);
        if (p->gsp.shared_object && now >= p->gsp.next_vblank_ns) snx_ctr_gsp_vblank(p);
        if (p->dsp.core && now >= p->dsp.next_frame_ns) snx_ctr_dsp_frame(p, now);
        if (p->y2r.done_ns && now >= p->y2r.done_ns) snx_ctr_y2r_done(p);
        if (now >= snx_ctr_hid_deadline(p)) snx_ctr_hid_update(p, now);
        if (now >= p->time_update_ns) snx_ctr_shared_page_time(p);
        for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
            CtrObject* o = &p->objects[i];
            if (o->type != CTR_OBJ_TIMER || !o->data.timer.deadline) continue;
            if (o->data.timer.deadline <= now) snx_ctr_timer_signal(p, i, now);
            if (o->data.timer.deadline && o->data.timer.deadline < earliest) earliest = o->data.timer.deadline;
        }
        /* deadlines first: sleeps end and waits time out, which makes those threads runnable */
        for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
            if (p->objects[i].type != CTR_OBJ_THREAD) continue;
            CtrThread* t = &p->objects[i].data.thread;
            if (t->dead || !t->wake_ns) continue;
            if (t->wake_ns > now) {
                if (t->wake_ns < earliest) earliest = t->wake_ns;
                continue;
            }
            if (t->waiting_object)
                time_out(p, i);
            else {
                if (t->wait_nothing) { /* WaitSynchronizationN on no handles: Timeout, index -1 */
                    t->context.r[0] = CTR_TIMED_OUT;
                    t->context.r[1] = UINT32_MAX;
                    t->wait_nothing = 0;
                }
                snx_ctr_thread_ready(p, i);
            }
            t->wake_ns = 0;
        }
        snx_ctr_thread_priorities(p, priority);
        uint32_t next = 0, core = 0;
        for (uint32_t turn = 0; turn < 4 && !next; ++turn) {
            core = (p->next_core + turn) & 3;
            uint32_t yielded = 0, own = p->core_thread[core];
            for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
                if (p->objects[i].type != CTR_OBJ_THREAD) continue;
                CtrThread* t = &p->objects[i].data.thread;
                if (t->dead || t->wake_ns || t->waiting_object || (t->core & 3) != core) continue;
                if (t->yield) {
                    t->yield = 0;
                    if (!yielded || priority[i] < priority[yielded]) yielded = i;
                    continue;
                }
                bool better = !next || priority[i] < priority[next];
                if (!better && priority[i] == priority[next])
                    better = next != own && (i == own || t->ready_seq < p->objects[next].data.thread.ready_seq);
                if (better) next = i;
            }
            if (!next) next = yielded;
        }
        if (next) {
            p->next_core = (core + 1) & 3;
            p->core_thread[core] = next;
            p->current_thread = next;
            /* the space's flat view is the core's, not a thread's: it stays as a thread's registers come in */
            uint8_t* mem = p->cpu.state.cpu.mem;
            uint64_t mem_size = p->cpu.state.cpu.mem_size;
            p->cpu.state.cpu = snx_ctr_thread(p)->context;
            p->cpu.state.cpu.mem = mem;
            p->cpu.state.cpu.mem_size = mem_size;
            if (next == current) p->cpu.state.cpu.exclusive_valid = exclusive;
            return SNX_CTR_SCHED_RUN;
        }
        if (slept_for_device) return SNX_CTR_SCHED_IDLE;
        uint64_t deadline = earliest;
        if (p->uds && now + 1000000ull < deadline) deadline = now + 1000000ull;
        if (p->gsp.shared_object && p->gsp.next_vblank_ns < deadline) deadline = p->gsp.next_vblank_ns;
        if (p->dsp.core && p->dsp.next_frame_ns < deadline) deadline = p->dsp.next_frame_ns;
        if (p->y2r.done_ns && p->y2r.done_ns < deadline) deadline = p->y2r.done_ns;
        if (snx_ctr_hid_deadline(p) < deadline) deadline = snx_ctr_hid_deadline(p);
        /* nothing to wake for (a thread sleeping forever included) */
        if (deadline >= UINT64_MAX - 1) return SNX_CTR_SCHED_BLOCKED;
        slept_for_device = deadline < earliest;
        exclusive = 0;
        uint64_t before = now_ns();
        sleep_until(deadline);
        p->idle_ns += now_ns() - before;
        ++p->idle_waits;
    }
}
