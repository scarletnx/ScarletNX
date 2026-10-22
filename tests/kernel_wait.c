/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The handle table, events and svcWaitSynchronization (kernel.c and sync.c) as the console answers them: zero handles
   is a sleep, more than 64 is OutOfRange, a handle that names nothing waitable (a closed one, a pseudo-handle, the
   writable end of an event) fails the whole wait, the lowest signaled index wins, then a timeout of 0, then a pending
   cancel, and every object keeps a reference only for the wait. The host side is stubbed: a wait runs what the test
   scripts for "another thread" and then moves a fake clock past its deadline */
#include "nx/hle/kernel/kernel.c"
#include "nx/hle/kernel/sync.c"
#include "nx/hle/hle.c"
#include <stdarg.h>
#include <stdio.h>

u8* g_mem;
u64 g_as_size = AS_MAX; /* the reservation's size, mem.c's, which this test does not link */
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
void fatal(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    abort();
}
static KThread g_self;
KThread* kthread_cur(void) { return &g_self; }
void core_leave(KThread* t) { (void)t; }
void core_set_prio(KThread* t, int prio) { t->prio = prio; }
static u64 g_now;
u64 plat_ns(void) { return g_now; }
u64 guest_ns(void) { return g_now; } /* the waits' deadlines: no pause here */
void pc_broadcast(PCond* c) { (void)c; }
/* what another thread does while the caller waits, and what the caller's wait looked like then */
static void (*g_during)(KThread* t);
static const char* g_seen_name;
static u64 g_seen_tid;
static int g_waits;
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    (void)c;
    (void)m;
    g_waits++;
    g_seen_name = g_self.wait_name;
    g_seen_tid = g_self.wait_tid;
    if (g_during) {
        void (*f)(KThread*) = g_during;
        g_during = NULL;
        f(&g_self);
    } else if (timeout_ns >= 0)
        g_now += (u64)timeout_ns;
    else
        fatal("kernel_wait.c: a wait with no deadline and nothing to end it\n");
    return 0;
}

static unsigned checks, failures;
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "kernel_wait.c:%d: %s\n", __LINE__, #expr);                                                \
        }                                                                                                              \
    } while (0)

#define HANDLES 0x100ull /* where the guest's handle array is */
/* little-endian, as the guest writes them */
static void put_handles(const u32* h, int n) {
    int i;
    for (i = 0; i < n; i++) snx_st_le32(g_mem + HANDLES + 4 * (u64)i, h[i]);
}
static u32 kernel_timeouted_wait(u32 n, i64 timeout, u32* idx) {
    *idx = 77;
    return svc_wait_sync(&g_self, HANDLES, n, timeout, idx);
}

static KEvent* g_ev[2];
static void signal_second(KThread* t) {
    (void)t;
    kevent_signal(g_ev[1]);
}
static void cancel_it(KThread* t) {
    if (t->wait_kind == 1 && !t->woken) wake(t, R_CANCELLED); /* svc.c's CancelSynchronization */
}
static int thread_done(KObj* o) { return ((KThread*)o)->state == 2; }
/* the writable end of an svcCreateEvent pair: no signaled test, so it can't be waited on */
static KObj g_wevent = {KO_WEVENT, 1, NULL, NULL, NULL, 0, 0, "writable event"};

void pm_init(PMutex* m) { (void)m; }

int main(void) {
    hle_init();
    u32 idx, h[3], hw, ht;
    int refs;
    g_mem = calloc(1, 0x1000);
    if (!g_mem) return 1;
    g_self.o.type = KO_THREAD;
    g_self.o.is_signaled = thread_done;
    g_self.id = 0x60;
    g_ev[0] = kevent_new("first");
    g_ev[1] = kevent_new("second");
    h[0] = h_new(&g_ev[0]->o);
    h[1] = h_new(&g_ev[1]->o);
    CHECK(h[0] && h[1] && h[0] != h[1] && g_ev[0]->o.refs == 1);

    /* no handles: a sleep that never reads the array (the address is junk), named "no handles" in the dump */
    CHECK(svc_wait_sync(&g_self, 0xFFFFFFFFFFFFull, 0, 0, &idx) == R_TIMEOUT);
    g_waits = 0;
    CHECK(svc_wait_sync(&g_self, 0xFFFFFFFFFFFFull, 0, 1000, &idx) == R_TIMEOUT && g_waits == 1);
    CHECK(g_seen_name && !strcmp(g_seen_name, "no handles") && g_self.wait_kind == 0);
    g_during = cancel_it;
    CHECK(svc_wait_sync(&g_self, 0, 0, -1, &idx) == R_CANCELLED && !g_self.cancel);

    /* more than 64 handles, and handles that name nothing to wait on: the whole wait fails, no reference kept */
    CHECK(kernel_timeouted_wait(65, 0, &idx) == R_OUTOFRANGE);
    hw = h_new(&g_wevent);
    ht = h_new(&g_self.o);
    {
        u32 bad[4] = {0x12345, H_CUR_THREAD, 0, 0};
        int i;
        bad[2] = hw;
        bad[3] = ht + (1u << 15); /* the thread's slot, an older linear id */
        for (i = 0; i < 4; i++) {
            u32 two[2];
            two[0] = h[0];
            two[1] = bad[i];
            put_handles(two, 2);
            refs = g_ev[0]->o.refs;
            CHECK(kernel_timeouted_wait(2, 0, &idx) == R_BADHANDLE && g_ev[0]->o.refs == refs);
        }
    }

    /* the lowest signaled index wins; with none, a timeout of 0 times out ahead of a pending cancel */
    put_handles(h, 2);
    kevent_signal(g_ev[1]);
    CHECK(kernel_timeouted_wait(2, -1, &idx) == 0 && idx == 1);
    kevent_signal(g_ev[0]);
    CHECK(kernel_timeouted_wait(2, -1, &idx) == 0 && idx == 0);
    kevent_clear(g_ev[0]);
    kevent_clear(g_ev[1]);
    g_self.cancel = 1;
    CHECK(kernel_timeouted_wait(2, 0, &idx) == R_TIMEOUT && g_self.cancel == 1);
    CHECK(kernel_timeouted_wait(2, 1000, &idx) == R_CANCELLED && g_self.cancel == 0);

    /* signaled while waiting: the index of the object that ended the wait; references only for the wait */
    refs = g_ev[1]->o.refs;
    g_during = signal_second;
    CHECK(kernel_timeouted_wait(2, -1, &idx) == 0 && idx == 1 && g_ev[1]->o.refs == refs && !g_ev[1]->o.nwait && !g_ev[0]->o.nwait);
    kevent_clear(g_ev[1]);
    g_during = cancel_it;
    CHECK(kernel_timeouted_wait(2, -1, &idx) == R_CANCELLED && idx == ~0u && !g_self.cancel);
    CHECK(kernel_timeouted_wait(2, 500, &idx) == R_TIMEOUT && idx == ~0u);

    /* a join: the thread first among the handles is named in the dump */
    {
        u32 two[2];
        two[0] = ht;
        two[1] = h[0];
        put_handles(two, 2);
        CHECK(kernel_timeouted_wait(2, 1000, &idx) == R_TIMEOUT && g_seen_tid == 0x60 && !g_self.wait_tid);
        g_self.state = 2; /* it has exited */
        CHECK(kernel_timeouted_wait(2, 1000, &idx) == 0 && idx == 0);
        g_self.state = 0;
    }

    /* the pseudo-handles: the process resolves to its permanent object, neither can be closed */
    {
        KObj* p = h_get_ref(H_CUR_PROC, KO_PROCESS);
        CHECK(p && p->type == KO_PROCESS && h_get_ref(H_CUR_THREAD, KO_THREAD) == &g_self.o);
        kobj_unref(p);
        kobj_unref(&g_self.o);
        CHECK(h_close(H_CUR_PROC) == R_BADHANDLE && h_close(H_CUR_THREAD) == R_BADHANDLE);
        CHECK(h_get_ref(H_CUR_PROC, 0) == p && p->refs == 2);
        kobj_unref(p);
    }

    /* a closed handle is stale, and the object goes with its last reference */
    kobj_ref(&g_ev[0]->o);
    CHECK(h_close(h[0]) == 0 && g_ev[0]->o.refs == 1 && h_close(h[0]) == R_BADHANDLE);
    put_handles(h, 1);
    CHECK(kernel_timeouted_wait(1, 0, &idx) == R_BADHANDLE);
    kobj_unref(&g_ev[0]->o);

    printf("kernel_wait: %u checks, %u failed\n", checks, failures);
    free(g_mem);
    return failures != 0;
}
