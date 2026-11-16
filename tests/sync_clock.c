/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sync.c's clocks against the game's (runtime/clock.c): steady_clock::now is guest time, which a game pause stops and a
   save state's load sets, as the SDK's own steady_clock reads it; system_clock::now is the host's real time */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "common/platform/time.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/clock.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* the kernel's side of a wait, which these calls never make */
KThread* kthread_cur(void) { return NULL; }
void core_leave(KThread* t) { (void)t; }
void core_enter(KThread* t) { (void)t; }
void kthread_block(KThread* t) { (void)t; }
void kthread_unblock(KThread* t) { (void)t; }
void fatal(const char* format, ...) {
    (void)format;
    abort();
}

int main(void) {
    CPU c = {0};
    clock_start();
    plat_sleep_ns(20000000);
    i64 before = sync_steady_now(&c);
    assert(before >= 20000000 && before < 1000000000ll); /* time since the run's start, not the host's uptime */
    /* paused for 200 ms: the game's steady_clock stands still, and goes on from there */
    clock_pause();
    i64 frozen = sync_steady_now(&c);
    plat_sleep_ns(200000000);
    assert(sync_steady_now(&c) == frozen && frozen >= before);
    clock_resume();
    i64 after = sync_steady_now(&c);
    assert(after >= frozen && after - frozen < 150000000);
    /* a save state's load: the state's guest time */
    clock_set(5000000000ull);
    i64 set = sync_steady_now(&c);
    assert(set >= 5000000000ll && set < 5150000000ll);
    /* system_clock stays the host's real time, in microseconds */
    i64 unix_us = plat_unix_ns() / 1000, now_us = sync_system_now(&c);
    assert(now_us >= unix_us && now_us - unix_us < 1000000);
    puts("PASS sync.c's clocks: steady_clock the game's (stopped by a pause, set by a state's load), system_clock the "
         "host's");
    return 0;
}
