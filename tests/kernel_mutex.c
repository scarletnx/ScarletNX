/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Priority inheritance in the kernel's mutex arbitration (sync.c), as the console's kernel does it: a mutex's owner
   runs at the best priority among its waiters, up a chain of owners that wait on mutexes themselves, until the mutex
   is handed over. The rest of the kernel is stubbed: a wait returns at once with the thread still queued, so each
   step's priorities can be read */
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
u64 plat_ns(void) { return 0; }
void kobj_ref(KObj* o) { (void)o; }
void kobj_unref(KObj* o) { (void)o; }
/* kernel.c's, for the threads a join waits on: signaled once exited */
int kobj_waitable(const KObj* o) { return o->type == KO_EVENT || o->type == KO_THREAD || o->type == KO_PROCESS; }
int kobj_signaled(const KObj* o) { return o->type == KO_THREAD && ((const KThread*)o)->state == 2; }
u32 kobj_id(const KObj* o) { return (u32)(size_t)o; } /* tells the test's objects apart */
void waiter_add(KObj* o, KThread* t) {
    (void)o;
    (void)t;
}
void waiter_del(KObj* o, KThread* t) {
    (void)o;
    (void)t;
}
static unsigned g_slept;
static u64 g_joining;
static KThread* g_ends[2]; /* the threads a join's sleep sees exit */
int sleep_nl(KThread* t, i64 timeout) {
    (void)timeout;
    g_slept++;
    g_joining = t->wait_tid;
    for (int i = 0; i < 2; i++)
        if (g_ends[i]) g_ends[i]->state = 2;
    return 1;
}
/* the word at g_watch when a thread is woken */
static u64 g_watch;
static u32 g_watched = ~0u;
void wake(KThread* t, int result) {
    t->woken = 1;
    t->wait_result = result;
    if (g_watch) g_watched = snx_ld_le32(g_mem + g_watch);
}
void core_set_prio(KThread* t, int prio) { t->prio = prio; }

enum { SNX_KMT_THREADS = 6, SNX_KMT_HANDLE = 0x100 };
/* the threads are the pool's first slots, where the kernel's lists find them (kt_at) */
#define snx_kmt_thread (g_hle.thread.threads)
KObj* h_get_nl(u32 h) {
    u32 i = h - SNX_KMT_HANDLE;
    return i < SNX_KMT_THREADS ? &snx_kmt_thread[i].o : NULL;
}
static u32 handle(KThread* t) { return SNX_KMT_HANDLE + (u32)(t - snx_kmt_thread); }
/* ends once the waiter sleeps, like a joined thread */

static unsigned checks, failures;
#define CHECK(expr, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "kernel_mutex.c:%d: %s: ", __LINE__, #expr);                                               \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

/* the priorities the cases start from */
static const int start_prio[] = {46, 44, 45, 40, 50, 30};
/* fresh threads at these base priorities, nobody waiting */
static KThread* reset(const int* prio) {
    memset(snx_kmt_thread, 0, sizeof snx_kmt_thread);
    memset(g_mem, 0, 0x1000);
    g_hle.sync.mwait = 0;
    for (int i = 0; i < SNX_KMT_THREADS; i++) {
        snx_kmt_thread[i].o.type = KO_THREAD;
        snx_kmt_thread[i].prio = snx_kmt_thread[i].base_prio = prio[i];
    }
    return snx_kmt_thread;
}
/* guest words are little-endian, as sync.c reads and writes them */
static u32 word(u64 addr) { return snx_ld_le32(g_mem + addr); }
/* t blocks on the mutex at addr, which owner holds: the word names owner with the waiters bit, as the SDK leaves it
   before it asks the kernel */
static void lock(KThread* t, KThread* owner, u64 addr) {
    u32 v = handle(owner) | HANDLE_WAIT;
    snx_st_le32(g_mem + addr, v);
    CHECK(svc_arbitrate_lock(t, handle(owner), addr, handle(t)) == 0, "lock");
}

void pm_init(PMutex* m) { (void)m; }

int main(void) {
    hle_init();
    g_mem = calloc(1, 0x1000);
    if (!g_mem) return 1;
    enum { SNX_KMT_M = 0x100, SNX_KMT_M2 = 0x200, SNX_KMT_CV = 0x300 };

    /* the owner, at 46, takes the priority of a waiter at 44 while it holds the mutex, and gives it back when it
       hands the mutex over */
    KThread* t = reset((const int[]){46, 44, 45, 40, 50, 30});
    KThread *a = &t[0], *b = &t[1];
    lock(b, a, SNX_KMT_M);
    CHECK(a->prio == 44 && a->base_prio == 46, "owner at %d (base %d) with a waiter at 44", a->prio, a->base_prio);
    CHECK(svc_arbitrate_unlock(SNX_KMT_M) == 0 && b->woken && word(SNX_KMT_M) == handle(b),
          "handed to the waiter: word %x", word(SNX_KMT_M));
    CHECK(a->prio == 46, "owner back at %d after the handover", a->prio);

    /* of two waiters the best takes the mutex, and the other then waits for it: its priority goes with it */
    t = reset((const int[]){46, 44, 45, 40, 50, 30});
    a = &t[0], b = &t[1];
    KThread* e = &t[3];
    lock(b, a, SNX_KMT_M);
    lock(e, a, SNX_KMT_M);
    CHECK(a->prio == 40, "owner at %d with waiters at 44 and 40", a->prio);
    svc_arbitrate_unlock(SNX_KMT_M);
    CHECK(e->woken && !b->woken && word(SNX_KMT_M) == (handle(e) | HANDLE_WAIT), "40 takes it, 44 still waits: word %x",
          word(SNX_KMT_M));
    CHECK(b->lock_owner == kt_ix(e) && a->prio == 46 && e->prio == 40, "44 now waits for 40; old owner at %d", a->prio);

    /* a chain: the owner waits on a mutex of its own, so its owner inherits too */
    t = reset((const int[]){46, 44, 45, 40, 50, 30});
    a = &t[0], b = &t[1];
    KThread* d = &t[4];
    lock(a, d, SNX_KMT_M2);
    CHECK(d->prio == 46, "the second owner at %d with the first owner (46) waiting", d->prio);
    lock(b, a, SNX_KMT_M);
    CHECK(a->prio == 44 && d->prio == 44, "44 reaches both owners: %d and %d", a->prio, d->prio);

    /* SetThreadPriority on an owner moves its base, not what it inherits */
    t = reset((const int[]){46, 44, 45, 40, 50, 30});
    a = &t[0], b = &t[1];
    lock(b, a, SNX_KMT_M);
    prio_set_base_nl(a, 50);
    CHECK(a->prio == 44 && a->base_prio == 50, "base 50 under a waiter at 44: runs at %d", a->prio);
    svc_arbitrate_unlock(SNX_KMT_M);
    CHECK(a->prio == 50, "at its new base after the handover: %d", a->prio);

    /* a condition variable signalled while another thread holds the mutex: the woken waiter queues on the mutex and
       its owner inherits its priority */
    t = reset((const int[]){46, 44, 45, 40, 50, 30});
    a = &t[0], b = &t[1];
    u32 mine = handle(b);
    snx_st_le32(g_mem + SNX_KMT_M, mine);
    CHECK(svc_wait_cv(b, SNX_KMT_M, SNX_KMT_CV, handle(b), -1) == 0, "wait on the condition variable");
    u32 held = handle(a);
    snx_st_le32(g_mem + SNX_KMT_M, held);
    svc_signal_cv(SNX_KMT_CV, 1);
    CHECK(b->wait_kind == 2 && b->lock_owner == kt_ix(a) && a->prio == 44, "signalled into a held mutex: owner at %d",
          a->prio);

    /* the condition variable is marked before its mutex changes hands */
    t = reset(start_prio);
    a = &t[0], b = &t[1];
    lock(b, a, SNX_KMT_M);
    g_watch = SNX_KMT_CV;
    CHECK(svc_wait_cv(a, SNX_KMT_M, SNX_KMT_CV, handle(a), -1) == 0, "wait on the condition variable");
    CHECK(b->woken && word(SNX_KMT_M) == handle(b) && g_watched == 1,
          "the mutex went to its waiter (word %x) with the condition variable at %u", word(SNX_KMT_M), g_watched);
    g_watch = 0;

    /* a join: the thread dump names the thread waited for */
    t = reset(start_prio);
    a = &t[0], b = &t[1];
    b->id = 82;
    g_ends[0] = b;
    u32 joined = handle(b), at = 9;
    snx_st_le32(g_mem + 0x400, joined);
    g_slept = 0;
    CHECK(svc_wait_sync(a, 0x400, 1, -1, &at) == 0 && at == 0 && g_slept == 1, "join: index %u after %u sleeps", at,
          g_slept);
    CHECK(g_joining == 82, "the joining thread names thread %lu, not 82", (unsigned long)g_joining);

    /* a join on several objects names the thread when it is the first of them */
    t = reset(start_prio);
    a = &t[0], b = &t[1];
    b->id = 83;
    g_ends[0] = b;
    g_ends[1] = &t[2];
    u32 two[2] = {handle(b), handle(&t[2])};
    snx_st_le32(g_mem + 0x400, two[0]), snx_st_le32(g_mem + 0x404, two[1]);
    g_slept = 0;
    CHECK(svc_wait_sync(a, 0x400, 2, -1, &at) == 0 && at == 0 && g_joining == 83, "join of two: thread %lu",
          (unsigned long)g_joining);
    g_ends[0] = g_ends[1] = NULL;

    /* ArbitrateLock answers as the console: a word that changed meanwhile succeeds without a wait, an owner handle
       that names no thread (the current-thread pseudo-handle, a closed handle) is InvalidHandle, and the address must
       be word-aligned */
    t = reset(start_prio);
    a = &t[0], b = &t[1];
    g_slept = 0;
    u32 v = handle(a);
    snx_st_le32(g_mem + SNX_KMT_M, v); /* no waiters bit: it was unlocked and taken again */
    CHECK(svc_arbitrate_lock(b, handle(a), SNX_KMT_M, handle(b)) == 0 && !g_slept && !g_hle.sync.mwait, "word changed");
    v = 0xFFFF8000u | HANDLE_WAIT;
    snx_st_le32(g_mem + SNX_KMT_M, v);
    CHECK(svc_arbitrate_lock(b, 0xFFFF8000u, SNX_KMT_M, handle(b)) == R_BADHANDLE && !g_slept, "pseudo-handle owner");
    v = 0x999u | HANDLE_WAIT;
    snx_st_le32(g_mem + SNX_KMT_M, v);
    CHECK(svc_arbitrate_lock(b, 0x999u, SNX_KMT_M, handle(b)) == R_BADHANDLE && !g_slept, "closed handle owner");
    CHECK(svc_arbitrate_lock(b, handle(a), SNX_KMT_M + 2, handle(b)) == R_BADADDR, "unaligned mutex");

    /* a condition variable waiter signalled into a mutex whose word names no thread is woken with InvalidState */
    t = reset(start_prio);
    a = &t[0], b = &t[1];
    memset(g_hle.sync.cvwait, 0, sizeof g_hle.sync.cvwait);
    mine = handle(b);
    snx_st_le32(g_mem + SNX_KMT_M, mine);
    CHECK(svc_wait_cv(b, SNX_KMT_M, SNX_KMT_CV, handle(b), -1) == 0, "wait on the condition variable");
    v = 0x999u;
    snx_st_le32(g_mem + SNX_KMT_M, v);
    b->woken = 0;
    svc_signal_cv(SNX_KMT_CV, 1);
    CHECK(b->woken && b->wait_result == R_BADSTATE && !g_hle.sync.mwait, "woken with %x", b->wait_result);

    /* SignalToAddress type 2 (SignalAndModifyByWaitingCountIfEqual): the new value as the console computes it, by
       whether this signal leaves waiters behind */
    {
        static const struct {
            int waiters, count;
            u32 now;
        } k[] = {{0, 1, 6}, {2, 1, 5}, {2, 2, 4}, {2, 3, 4}, {2, -1, 3}, {0, -1, 6}};
        enum { SNX_KMT_ARB = 0x500 };
        for (unsigned i = 0; i < sizeof k / sizeof *k; i++) {
            u32 five = 5;
            t = reset(start_prio);
            memset(g_hle.sync.arbwait, 0, sizeof g_hle.sync.arbwait);
            snx_st_le32(g_mem + SNX_KMT_ARB, five);
            for (int w = 0; w < k[i].waiters; w++)
                CHECK(svc_wait_addr(&t[w], SNX_KMT_ARB, 0, 6, -1) == 0, "waiter %d", w);
            for (int w = 0; w < k[i].waiters; w++) t[w].woken = 0;
            CHECK(svc_signal_addr(SNX_KMT_ARB, 2, 5, k[i].count) == 0 && word(SNX_KMT_ARB) == k[i].now,
                  "%d waiters, count %d: word %u, not %u", k[i].waiters, k[i].count, word(SNX_KMT_ARB), k[i].now);
            CHECK(!k[i].waiters || t[1].woken, "the best waiter (44) is woken");
            CHECK(!k[i].waiters || t[0].woken == (k[i].count != 1), "the other one only when the count allows");
        }
        u32 seven = 7;
        t = reset(start_prio);
        memset(g_hle.sync.arbwait, 0, sizeof g_hle.sync.arbwait);
        snx_st_le32(g_mem + SNX_KMT_ARB, seven);
        CHECK(svc_wait_addr(&t[0], SNX_KMT_ARB, 2, 7, -1) == 0, "wait if equal");
        t[0].woken = 0;
        CHECK(svc_signal_addr(SNX_KMT_ARB, 2, 5, 1) == R_BADSTATE && !t[0].woken && word(SNX_KMT_ARB) == 7,
              "another value: InvalidState, nobody woken");
    }

    printf("%u checks, %u failed\n", checks, failures);
    free(g_mem);
    return failures != 0;
}
