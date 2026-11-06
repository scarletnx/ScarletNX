/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The process's threads as common.c lists them (snx_threads), which the log's threads line takes (runtime/main.c):
   on Linux, Android, macOS, iOS, Windows and FreeBSD every thread once, each with an id of its own and its time on
   the processors, so the one thread that spun shows the time it spun and the idle ones nearly none; a list cut at
   max still counts them all. Each thread shows the name it gave itself (snx_thread_name), as much of it as the
   system keeps: one with a space and a parenthesis, one past every system's room. Elsewhere none */
#include "common/utilities/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define IDLE    4
#define SPIN_NS 300000000u /* what the spinning thread spends on the processors, its own clock's count */
/* the bytes of a name the system keeps */
#if defined(__linux__)
#define KEEP 15
#elif defined(__FreeBSD__)
#define KEEP 19
#else
#define KEEP 63
#endif
/* the idle threads', then the spinning one's; the first is 70 bytes */
static const char* const g_names[IDLE + 1] = {"idle 0, with a name of seventy bytes: past the room of every system...",
                                              "idle 1", "idle 2", "idle 3", "snx (spin) 1, a long name"};

static struct {
    pthread_mutex_t lock;
    pthread_cond_t moved;
    int ready, done; /* the threads named, and the spinning one done too; main done listing */
} g_test = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};

/* one more thread ready, then waits until main is done listing */
static void ready_then_wait(void) {
    pthread_mutex_lock(&g_test.lock);
    g_test.ready++;
    pthread_cond_broadcast(&g_test.moved);
    while (!g_test.done) pthread_cond_wait(&g_test.moved, &g_test.lock);
    pthread_mutex_unlock(&g_test.lock);
}
static void* idle(void* index) {
    snx_thread_name(g_names[(intptr_t)index]);
    ready_then_wait();
    return NULL;
}
static void* spin(void* index) {
    SnxU64 clock = snx_thread_clock(), ns = 0;
    double give_up = snx_seconds() + 20; /* a loaded machine gives the thread its time slower */
    volatile unsigned turns = 0;
    snx_thread_name(g_names[(intptr_t)index]);
    assert(clock);
    while (!snx_thread_cpu_ns(clock, &ns) && ns < SPIN_NS && snx_seconds() < give_up) ++turns;
    assert(ns >= SPIN_NS);
    snx_thread_clock_close(clock);
    ready_then_wait();
    return NULL;
}

int main(void) {
    SnxThreadInfo list[64], cut[3];
    pthread_t threads[IDLE + 1];
    int n, busy = 0;
    SnxU64 most = 0;
    int named = 1;
#ifdef _WIN32
    named = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription") != NULL; /* 10 1607 on */
#endif
    assert(strlen(g_names[0]) == 70);
    for (int i = 0; i < IDLE + 1; i++)
        assert(!pthread_create(&threads[i], NULL, i < IDLE ? idle : spin, (void*)(intptr_t)i));
    pthread_mutex_lock(&g_test.lock);
    while (g_test.ready < IDLE + 1) pthread_cond_wait(&g_test.moved, &g_test.lock);
    pthread_mutex_unlock(&g_test.lock);

    n = snx_threads(list, 64);
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(__FreeBSD__)
    /* main, the idle ones and the spinning one, and any the runtime library started */
    assert(n >= IDLE + 2 && n <= 64);
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < i; k++) assert(list[i].id != list[k].id);
        if (list[i].cpu_ns > most) most = list[i].cpu_ns;
        if (list[i].cpu_ns >= SPIN_NS - SPIN_NS / 10) busy++; /* Linux counts in 10 ms ticks */
        printf("thread %llx: %.3f s, \"%s\"\n", (unsigned long long)list[i].id, (double)list[i].cpu_ns / 1e9,
               list[i].name);
    }
    assert(busy == 1 && most >= SPIN_NS - SPIN_NS / 10);
    /* each name once, as much of it as the system keeps; the busy thread is the one named for spinning */
    for (int t = 0; named && t < IDLE + 1; t++) {
        char kept[KEEP + 1];
        int found = 0;
        snprintf(kept, sizeof kept, "%s", g_names[t]);
        for (int i = 0; i < n; i++)
            if (!strcmp(list[i].name, kept)) {
                found++;
                assert(t < IDLE || list[i].cpu_ns == most);
            }
        if (found != 1) fprintf(stderr, "\"%s\" listed %d times\n", kept, found);
        assert(found == 1);
    }
    if (!named) printf("threads: this Windows names no thread (no SetThreadDescription)\n");
    /* a list with room for two counts every thread and writes two */
    memset(cut, 0x5a, sizeof cut);
    assert(snx_threads(cut, 2) >= IDLE + 2);
    for (size_t b = 0; b < sizeof cut[2]; b++) assert(((unsigned char*)&cut[2])[b] == 0x5a);
    printf("threads: %d listed, the busiest %.3f s on the processors\n", n, (double)most / 1e9);
#else
    (void)busy;
    (void)most;
    (void)cut;
    (void)named;
    assert(n == -1);
#endif
    pthread_mutex_lock(&g_test.lock);
    g_test.done = 1;
    pthread_cond_broadcast(&g_test.moved);
    pthread_mutex_unlock(&g_test.lock);
    for (int i = 0; i < IDLE + 1; i++) pthread_join(threads[i], NULL);
    return 0;
}
