/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The short timed waits of plat_wait_word (src/common/platform/thread.c's short_wait): a wait of SHORT_WAIT_NS or less
   watches the word with the CPU's UMWAIT or MWAITX, or spins, and does not go to the system. It lasts its time when
   the word stays, ends at once when the word already differs, and ends early when another thread changes the word.
   A longer wait, and one of no time or without limit, is the system's */
#include "common/platform/thread.c"
#include "common/platform/time.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

static int g_fail;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "plat_short_wait: %s\n", what);
        g_fail = 1;
    }
}

static _Atomic uint32_t g_word, g_waiting;
/* waits up to the longest short wait for g_word to leave 0: its time in nanoseconds in g_waiting's place */
static _Atomic uint64_t g_took;
static void waiter(void* arg) {
    (void)arg;
    atomic_store_explicit(&g_waiting, 1, memory_order_release);
    uint64_t a = plat_ns();
    while (atomic_load_explicit(&g_word, memory_order_acquire) == 0 && plat_ns() - a < 1000000000u)
        plat_wait_word(&g_word, 0, SHORT_WAIT_NS);
    atomic_store_explicit(&g_took, plat_ns() - a, memory_order_release);
}

int main(void) {
    _Atomic uint32_t w = 7;
    static const int64_t times[] = {1000, 20000, SHORT_WAIT_NS};
#if defined(__x86_64__) || defined(__i386__)
    printf("plat_short_wait: the CPU's watch: %s\n",
           watch_kind() == WATCH_UMWAIT ? "UMWAIT" : watch_kind() == WATCH_MWAITX ? "MWAITX" : "a spin");
#endif
    /* the word stays: each wait lasts its time, and ends within 5 ms of it on a loaded host */
    for (size_t i = 0; i < sizeof times / sizeof times[0]; i++) {
        uint64_t a = plat_ns();
        check(short_wait(&w, 7, times[i]), "a short wait went to the system");
        uint64_t took = plat_ns() - a;
        check(took >= (uint64_t)times[i], "a short wait ended before its time with the word unchanged");
        check(took < (uint64_t)times[i] + 5000000u, "a short wait lasted 5 ms past its time");
    }
    /* the word differs already: no wait */
    uint64_t a = plat_ns();
    check(short_wait(&w, 8, SHORT_WAIT_NS), "a short wait for another value went to the system");
    check(plat_ns() - a < (uint64_t)SHORT_WAIT_NS, "a wait for a value the word does not hold lasted its time");
    /* the system's: longer, no time, or without limit */
    check(!short_wait(&w, 7, SHORT_WAIT_NS + 1), "a wait longer than the short ones did not go to the system");
    check(!short_wait(&w, 7, 0), "a wait of no time was a short wait");
    check(!short_wait(&w, 7, -1), "a wait without limit was a short wait");
    /* another thread changes the word: the waiter's loop of short waits ends soon after, and well before its
       second's limit */
    check(!plat_thread(waiter, NULL, (size_t)256 << 10), "no thread");
    while (!atomic_load_explicit(&g_waiting, memory_order_acquire)) plat_yield();
    plat_sleep_ns(200000);
    atomic_store_explicit(&g_word, 1, memory_order_release);
    plat_wake_word(&g_word);
    while (!atomic_load_explicit(&g_took, memory_order_acquire)) plat_yield();
    check(atomic_load_explicit(&g_took, memory_order_acquire) < 100000000u,
          "the waiter did not see the word change within 100 ms");
    if (!g_fail) puts("plat_short_wait: ok");
    return g_fail;
}
