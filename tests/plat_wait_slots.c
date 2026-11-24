/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The waits that the hang report reads (pm_waits, src/common/platform/thread.c): more threads than it has slots for
   come and go, each waiting on a condition variable once, and the report still finds the one that waits now. A slot
   belongs to a thread from its first wait, so without one that ends giving it back the report would be blind to every
   thread after the first PM_WAITS of them (512) */
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

enum { THREADS = 600, WAITS = 600 };
static struct {
    PMutex m;
    PCond c;
    int go; /* the threads up to this one may end, under m */
    _Atomic int done;
} g_t = {.m = PMUTEX_INIT};

static void body(void* arg) {
    int n = (int)(intptr_t)arg;
    pm_lock(&g_t.m);
    while (g_t.go <= n) pc_wait(&g_t.c, &g_t.m, -1);
    pm_unlock(&g_t.m);
    atomic_fetch_add_explicit(&g_t.done, 1, memory_order_release);
}
/* the waits with m that the report shows */
static int waiting(void) {
    static PmWait w[WAITS];
    int n = pm_waits(w, WAITS), on = 0;
    for (int i = 0; i < n; i++) on += w[i].m == &g_t.m && w[i].cond;
    return on;
}

int main(void) {
    pc_init(&g_t.c);
    for (int i = 0; i < THREADS; i++) {
        assert(!plat_thread(body, (void*)(intptr_t)i, (size_t)256 << 10));
        uint64_t t0 = plat_ns(), limit = 5ull * 1000000000;
        while (!waiting() && plat_ns() - t0 < limit) plat_yield();
        if (!waiting()) {
            fprintf(stderr, "plat_wait_slots: thread %d waits, and the report does not show it\n", i);
            return 1;
        }
        pm_lock(&g_t.m);
        g_t.go = i + 1;
        pc_broadcast(&g_t.c);
        pm_unlock(&g_t.m);
        while (atomic_load_explicit(&g_t.done, memory_order_acquire) <= i) plat_yield();
    }
    puts("plat_wait_slots: ok");
    return 0;
}
