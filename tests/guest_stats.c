/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The windows of runtime/guest_stats.c and their lines: the first call only starts a window; each SVC counted by its
   number, those past the table together, a host thread's in slot 0; the misses; a window holding only what came in it;
   no add lost when threads count at once, in their own slots and in the one they share; a guest thread's time on the
   processors read from another thread, at least what it spun, the time of one that ended in the window, which the next
   window no longer counts, and one that ended without saying so, said when another takes its slot or its clock goes;
   NCE's outcalls by kind, the hooked functions' by family, a family's index the same at each call with its name and
   the names past the table together; and the lines saying those values, the NCE line only while NCE runs */
#if defined(__linux__)
#define _POSIX_C_SOURCE 200809L /* nanosleep, which C99 has not */
#endif
#include "nx/runtime/guest_stats.h"
#include "common/utilities/common.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the kernel's names, which the lines take: two SVCs and the thread of slot 3 */
const char* svc_name(u32 imm) {
    return imm == 0x18 ? "WaitSynchronization" : imm == 0x1d ? "SignalProcessWideKey" : NULL;
}
u64 kernel_thread_name(u32 ix, char* out, int n) {
    snprintf(out, (size_t)n, "%s", ix == 3 ? "worker" : "");
    return ix == 3 ? 7 : 0;
}

/* the test's state: the lines logged since the last look, and the spinning thread's steps */
static struct {
    char logged[4][2048];
    int lines, failures;
    pthread_mutex_t lock;
    pthread_cond_t moved;
    int step;
    GstatWindow w; /* 4 KiB, off the stack */
    int nce;       /* nce_on's */
    u64 unsaid;    /* the clock of a thread that ends unsaid */
} g_test = {.lock = PTHREAD_MUTEX_INITIALIZER, .moved = PTHREAD_COND_INITIALIZER};

/* nce.c's: whether the game's code runs natively, which the test sets */
int nce_on(void) { return g_test.nce; }

void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    if (g_test.lines == 4) return;
    va_start(ap, fmt);
    vsnprintf(g_test.logged[g_test.lines++], sizeof g_test.logged[0], fmt, ap);
    va_end(ap);
}

static void check(int ok, const char* what) {
    if (ok) return;
    fprintf(stderr, "FAIL: %s\n", what);
    g_test.failures++;
}
/* line i of those logged has text in it */
static void said(int i, const char* text) {
    if (i < g_test.lines && strstr(g_test.logged[i], text)) return;
    fprintf(stderr, "FAIL: line %d says \"%s\": %s", i, text, i < g_test.lines ? g_test.logged[i] : "(none)\n");
    g_test.failures++;
}

/* the calling thread runs until its own time on the processors grew by ns */
static void spin(u64 ns) {
    u64 clock = snx_thread_clock(), from = 0, now = 0;
    volatile u64 sink = 0;
    if (!clock || snx_thread_cpu_ns(clock, &from)) {
        fprintf(stderr, "FAIL: this system gives no thread's time\n");
        exit(1);
    }
    do {
        for (int i = 0; i < 100000; i++) sink += (u64)i;
        snx_thread_cpu_ns(clock, &now);
    } while (now - from < ns);
    snx_thread_clock_close(clock);
}
/* the steps the guest thread and the test take in turn */
static void step_to(int step) {
    pthread_mutex_lock(&g_test.lock);
    g_test.step = step;
    pthread_cond_broadcast(&g_test.moved);
    pthread_mutex_unlock(&g_test.lock);
}
static void step_wait(int step) {
    pthread_mutex_lock(&g_test.lock);
    while (g_test.step < step) pthread_cond_wait(&g_test.moved, &g_test.lock);
    pthread_mutex_unlock(&g_test.lock);
}
/* guest thread 3: 30 ms on the processors, the test's window, 20 ms more, and its end */
static void* guest(void* arg) {
    (void)arg;
    guest_stats_thread_start(3);
    spin(30000000u);
    step_to(1);
    step_wait(2);
    spin(20000000u);
    guest_stats_thread_end(3);
    return NULL;
}

/* four threads at once: each 100000 SVCs in a slot of its own and as many in slot 0, which they share */
#define RACE 100000
static void* race(void* arg) {
    u32 ix = (u32)(size_t)arg;
    for (int i = 0; i < RACE; i++) {
        guest_stats_svc(ix, 0x30);
        guest_stats_svc(0, 0x31);
    }
    return NULL;
}

/* guest thread 4, which ends without guest_stats_thread_end; its clock, for the test to see it go */
static void* unsaid(void* arg) {
    (void)arg;
    guest_stats_thread_start(4);
    g_test.unsaid = snx_thread_clock();
    return NULL;
}

/* the counts of the windows below, from threads 1 and 2 and a host thread */
static void count(void) {
    for (int i = 0; i < 3; i++) guest_stats_svc(1, 0x18);
    for (int i = 0; i < 3; i++) guest_stats_svc(2, 0x18);
    guest_stats_svc(1, 0x1d);
    guest_stats_svc(0, 0x7f);
    guest_stats_svc(1, 0x1234);
    guest_stats_svc(2, SNX_SVC_COUNT);
    for (int i = 0; i < 5; i++) guest_stats_add(1, GSTAT_MISS);
}

int main(void) {
    GstatWindow* w = &g_test.w;
    pthread_t thread;

    count(); /* before the first window: never in one */
    check(guest_stats_window(1000, 1, w) == -1, "the first call only starts a window");

    count();
    check(!guest_stats_window(1000 + 2000000000u, 121, w), "the second call ends one");
    check(w->ns == 2000000000u && w->presents == 120, "the window's length and presents");
    check(w->svcs == 10, "10 SVCs in the window");
    check(w->svc[0x18] == 6 && w->svc[0x1d] == 1 && w->svc[0x7f] == 1, "each SVC by its number, slot 0's too");
    check(w->svc[SNX_SVC_COUNT] == 2, "the numbers past the table together");
    check(w->count[GSTAT_MISS] == 5, "5 misses");
    check(w->threads == 0 && w->cpu_ns == 0, "no guest thread ran");

    check(!guest_stats_window(1000 + 3000000000u, 181, w), "a third window");
    check(w->svcs == 0 && w->count[GSTAT_MISS] == 0 && w->presents == 60, "nothing came in it");

    count();
    g_test.lines = 0;
    guest_stats_log(1000 + 5000000000u, 301);
    check(g_test.lines == 2, "two lines a window");
    said(0, "cpu: 120 presents in 2.00 s (60.0 fps): 0 of the game's threads ran 0.00 s on the host's processors (0% "
            "of one, 0.00 ms a present), the whole process ");
    said(0, "; the busiest: none\n");
    said(1, "cpu: in those 2.00 s, 10 SVCs (5 a second): 0x18 WaitSynchronization 6, past 0xff 2, 0x1d "
            "SignalProcessWideKey 1, 0x7f 1; 5 calls the dispatch table had no code for (RtAPI.miss)\n");

    /* no add lost, in a thread's own slot or in the one they share */
    {
        pthread_t racer[4];
        guest_stats_window(1000 + 5500000000u, 301, w);
        for (size_t i = 0; i < 4; i++) pthread_create(&racer[i], NULL, race, (void*)(i + 10));
        for (int i = 0; i < 4; i++) pthread_join(racer[i], NULL);
        guest_stats_window(1000 + 5600000000u, 301, w);
        check(w->svc[0x30] == 4 * RACE && w->svc[0x31] == 4 * RACE, "every add of four threads at once");
    }

    /* a guest thread's time, read by this thread */
    guest_stats_window(1000 + 6000000000u, 301, w);
    if (pthread_create(&thread, NULL, guest, NULL)) {
        fprintf(stderr, "FAIL: no thread\n");
        return 1;
    }
    step_wait(1);
    check(!guest_stats_window(1000 + 7000000000u, 361, w), "the guest thread's window");
    check(w->thread_ns[3] >= 30000000u, "at least the 30 ms it spun");
    check(w->cpu_ns == w->thread_ns[3] && w->threads == 1 && w->no_clock == 0, "it alone ran, with a clock");
    check(w->process_ns >= w->thread_ns[3], "the process ran at least as long");
    fprintf(stderr, "thread 3: %llu ns, the process %llu ns\n", (unsigned long long)w->thread_ns[3],
            (unsigned long long)w->process_ns);
    step_to(2);
    pthread_join(thread, NULL);
    check(!guest_stats_window(1000 + 8000000000u, 421, w), "the window it ended in");
    check(w->thread_ns[3] >= 20000000u && w->threads == 1, "the 20 ms more it spun before it ended");
    check(!guest_stats_window(1000 + 9000000000u, 481, w), "the window after");
    check(w->thread_ns[3] == 0 && w->threads == 0, "an ended thread counts no more");

    /* a slot taken again from a thread that ended without saying so: said, on any system */
    if (pthread_create(&thread, NULL, unsaid, NULL)) {
        fprintf(stderr, "FAIL: no thread\n");
        return 1;
    }
    pthread_join(thread, NULL);
    g_test.lines = 0;
    guest_stats_thread_start(4);
    guest_stats_thread_end(4);
    check(g_test.lines == 1, "the slot taken again said");
    said(0, "cpu: the thread before in slot 4 ended without guest_stats_thread_end: its 0.00 s on the processors up to "
            "the window's start count, and what it ran after them does not\n");

    /* a thread that ends without saying so: once the system lets go of it its clock reads no more, which the log says
       once (the thread ran in the window, its time there uncounted). Linux only: the other systems may keep a handle
       or a port name that reads on, or names another thread */
#if defined(__linux__)
    u64 ns;
    if (pthread_create(&thread, NULL, unsaid, NULL)) {
        fprintf(stderr, "FAIL: no thread\n");
        return 1;
    }
    pthread_join(thread, NULL);
    for (int i = 0; i < 200 && !snx_thread_cpu_ns(g_test.unsaid, &ns); i++) {
        struct timespec ms10 = {0, 10000000};
        nanosleep(&ms10, NULL);
    }
#endif
    g_test.lines = 0;
    check(!guest_stats_window(1000 + 9500000000u, 481, w), "the window a thread ended unsaid in");
#if defined(__linux__)
    check(g_test.lines == 1 && w->threads == 1 && w->no_clock == 0, "said, and it ran in the window, with a clock");
    said(0, "cpu: the clock of the thread in slot 4 reads no more (it ended without guest_stats_thread_end): its 0.00 "
            "s on the processors up to the window's start count, and what it ran after them does not\n");
#endif

    /* the line of a window that a thread ran in names it */
    guest_stats_thread_start(3);
    spin(10000000u);
    g_test.lines = 0;
    guest_stats_log(1000 + 10500000000u, 541);
    guest_stats_thread_end(3);
    check(g_test.lines == 2, "an ended clock said only once");
    said(0, "cpu: 60 presents in 1.00 s (60.0 fps): 1 of the game's threads ran 0.0");
    said(0, "; the busiest: thread 7 'worker' 0.0");

    /* NCE's outcalls, a window of their own */
    check(guest_stats_family("libc") == 0 && guest_stats_family("nvn queue") == 1, "two families");
    check(guest_stats_family("libc") == 0, "a family again: its index");
    check(!strcmp(guest_stats_family_name(1), "nvn queue"), "a family's name");
    guest_stats_window(1000 + 11000000000u, 541, w);
    for (int i = 0; i < 4; i++) guest_stats_hook(1, 0);
    for (int i = 0; i < 2; i++) guest_stats_hook(2, 1);
    for (int i = 0; i < 3; i++) guest_stats_add(1, GSTAT_NCE_SVC);
    for (int i = 0; i < 7; i++) guest_stats_add(2, GSTAT_NCE_TICKS);
    guest_stats_add(1, GSTAT_NCE_TRAP);
    guest_stats_add(1, GSTAT_NCE_RUN);
    for (int i = 0; i < 2; i++) guest_stats_add(2, GSTAT_NCE_KICK);
    g_test.nce = 1;
    g_test.lines = 0;
    guest_stats_log(1000 + 12000000000u, 601);
    check(g_test.lines == 3, "three lines a window under NCE");
    said(2, "nce: in those 1.00 s, 17 outcalls (17 a second): 3 SVCs, 7 counter reads, 6 calls of hooked functions (by "
            "family: libc 4, nvn queue 2), 1 returns of native runs; 1 native runs started, 2 kicks\n");
    g_test.lines = 0;
    guest_stats_log(1000 + 13000000000u, 661);
    said(2, "nce: in those 1.00 s, 0 outcalls (0 a second): 0 SVCs, 0 counter reads, 0 calls of hooked functions (by "
            "family: none), 0 returns of native runs; 0 native runs started, 0 kicks\n");

    /* the families past the table: the last index, named so */
    for (unsigned i = 2; i < GSTAT_FAMILIES - 1; i++) {
        char name[16];
        snprintf(name, sizeof name, "family %u", i);
        check(guest_stats_family(name) == i, "a family of its own");
    }
    check(guest_stats_family("one more") == GSTAT_FAMILIES - 1, "the first past the table");
    check(guest_stats_family("and another") == GSTAT_FAMILIES - 1, "the second past it");
    check(!strcmp(guest_stats_family_name(GSTAT_FAMILIES - 1), "others"), "their name");
    check(guest_stats_family("family 30") == 30, "one in the table still");

    if (g_test.failures) return 1;
    printf("guest_stats: all checks passed\n");
    return 0;
}
