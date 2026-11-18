/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_host_page (common/memory/host.c) when threads make the first call together, as the workers of the 3DS compile
   do: each gets the page the host has. A thread that read a lookup still under way got 0, a host with no views, and
   compiled its blocks for the checked memory model of a host that is flat */
#include "common/memory/host.h"
#include <stdio.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#define THREADS 64

static _Atomic int g_arrived, g_open;
static size_t g_page[THREADS];

static void yield_thread(void) {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}
static size_t os_page(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}
static void first_call(size_t* page) {
    atomic_fetch_add_explicit(&g_arrived, 1, memory_order_acq_rel);
    while (!atomic_load_explicit(&g_open, memory_order_acquire)) yield_thread();
    *page = snx_host_page();
}
#ifdef _WIN32
static DWORD WINAPI worker(LPVOID page) {
    first_call((size_t*)page);
    return 0;
}
#else
static void* worker(void* page) {
    first_call((size_t*)page);
    return NULL;
}
#endif

/* a count of the threads at the gate, not a sleep, says they all wait there before the gate opens */
static void open_gate(void) {
    while (atomic_load_explicit(&g_arrived, memory_order_acquire) < THREADS) yield_thread();
    atomic_store_explicit(&g_open, 1, memory_order_release);
}

int main(void) {
#ifdef _WIN32
    HANDLE thread[THREADS];
    for (int i = 0; i < THREADS; ++i) {
        thread[i] = CreateThread(NULL, 0, worker, &g_page[i], 0, NULL);
        if (!thread[i]) return 1;
    }
    open_gate();
    WaitForMultipleObjects(THREADS, thread, TRUE, INFINITE);
#else
    pthread_t thread[THREADS];
    for (int i = 0; i < THREADS; ++i)
        if (pthread_create(&thread[i], NULL, worker, &g_page[i])) return 1;
    open_gate();
    for (int i = 0; i < THREADS; ++i) pthread_join(thread[i], NULL);
#endif
    size_t expected = os_page();
    size_t page = snx_host_page();
    int wrong = 0;
    for (int i = 0; i < THREADS; ++i) wrong += g_page[i] != expected;
    if (!expected || page != expected || wrong) {
        fprintf(stderr,
                "host_page_threads: %d of %d threads read a page other than the OS page of %zu bytes, the main thread "
                "read %zu\n",
                wrong, THREADS, expected, page);
        return 1;
    }
    printf("host_page_threads: ok, %zu byte pages\n", expected);
    return 0;
}
