/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The os_sync family's locks (src/nx/nn/os_sync.c) hand a lock to the oldest thread waiting on its word when its
   holder lets go of it, as the console's kernel hands a mutex to a waiter (svcArbitrateUnlock): the thread that let
   go and takes it again at once waits for its turn. Freed instead, it went back to that thread before the waiter
   ran. Host threads stand for the game's, each with a thread record of its own; the kernel's side is the word's hand
   over (sync.c's svc_arbitrate_unlock_to) with no thread waiting in the kernel */
#include "nx/nn/os_sync.c"
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Hle g_hle;
int g_lock_futex;
[[noreturn]] static void unreached(const char* what) {
    fprintf(stderr, "os_sync_handoff: %s was called\n", what);
    abort();
}
ClockRate clock_rate(void) { unreached("clock_rate"); }
void core_yield(KThread* t, int migrate) { (void)t, (void)migrate, unreached("core_yield"); }
void fatal(const char* fmt, ...) { unreached(fmt); }
u64 guest_ticks(void) { unreached("guest_ticks"); }
void hook_wait_begin(void) {}
void hook_wait_end(void) {}
u64 kernel_find_symbol_exact(const char* name) { unreached(name); }
void kthread_block(KThread* t) { (void)t, unreached("kthread_block"); }
void kthread_unblock(KThread* t) { (void)t, unreached("kthread_unblock"); }
static _Thread_local KThread* t_self;
KThread* kthread_cur(void) { return t_self; }
u32 svc_signal_addr(u64 addr, u32 type, i32 value, i32 count) {
    (void)addr, (void)type, (void)value, (void)count, unreached("svc_signal_addr");
}
u32 svc_wait_addr(KThread* t, u64 addr, u32 type, i64 value, i64 timeout) {
    (void)t, (void)addr, (void)type, (void)value, (void)timeout, unreached("svc_wait_addr");
}
static u8 g_guest[0x100];
/* the kernel's side with nobody waiting in the kernel: to the tag given, keeping the waiters bit, else freed */
u32 svc_arbitrate_unlock(u64 addr) { (void)addr, unreached("svc_arbitrate_unlock"); }
u32 svc_arbitrate_unlock_to(u64 addr, u32 tag, int* handed) {
    _Atomic u32* p = (_Atomic u32*)(void*)(g_guest + addr);
    *handed = tag != 0;
    atomic_store(p, snx_le32(tag ? tag | HANDLE_WAIT : 0u));
    if (tag)
        plat_wake_word(p);
    else
        plat_wake_word_one(p);
    return 0;
}

enum { LOCK = 0x40 };
static KThread g_threads[3];
static char g_order[8];
static _Atomic int g_norder;
static void note(char who) { g_order[atomic_fetch_add(&g_norder, 1)] = who; }
static CPU g_cpu[3];
/* B: waits on the lock A holds, holds it a while, lets go */
static void* run_b(void* arg) {
    (void)arg;
    t_self = &g_threads[1];
    os_cs_enter(&g_cpu[1], LOCK);
    note('B');
    usleep(20000);
    os_cs_leave(&g_cpu[1], LOCK);
    return NULL;
}
int main(void) {
    for (int i = 0; i < 3; i++) {
        g_cpu[i].mem = g_guest;
        g_threads[i].own_handle = 0x100 + (u32)i;
    }
    g_lock_futex = 1;
    t_self = &g_threads[0];
    /* A holds it; B comes to wait on its word */
    os_cs_enter(&g_cpu[0], LOCK);
    assert(os_cs_mine(&g_cpu[0], LOCK));
    pthread_t b;
    assert(!pthread_create(&b, NULL, run_b, NULL));
    for (int i = 0; i < 2000 && hand_first(LOCK) != 0x101u; i++) usleep(1000);
    assert(hand_first(LOCK) == 0x101u);
    /* A lets go and takes it again at once: B, waiting first, has it first */
    os_cs_leave(&g_cpu[0], LOCK);
    os_cs_enter(&g_cpu[0], LOCK);
    note('A');
    os_cs_leave(&g_cpu[0], LOCK);
    assert(!pthread_join(b, NULL));
    assert(g_norder == 2 && g_order[0] == 'B' && g_order[1] == 'A');
    assert(!hand_first(LOCK) && snx_ld_le32(g_guest + LOCK) == 0);
    /* a lock nobody waits on is free at once, and nobody stays queued */
    os_cs_enter(&g_cpu[0], LOCK);
    os_cs_leave(&g_cpu[0], LOCK);
    assert(snx_ld_le32(g_guest + LOCK) == 0 && !hand_first(LOCK));
    puts("os_sync_handoff: a lock goes to the thread waiting on it before the one that let it go takes it again");
    return 0;
}
