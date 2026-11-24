/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the native bridge keeps for a host thread (android/bridge/bridge.c's guest stack, hle_android.c's looper,
   jni.c's JNIEnv) ends with the thread, whoever made it: a JVM's thread, one of a pool's, ends with no call into the
   bridge */
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include <assert.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* the HLE's function name, called as guest code calls it */
static uint64_t call(const char* name, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    static _Thread_local CPU cpu;
    const HleSymbol* s = hle_find(name);
    if (!s || !s->handler) {
        fprintf(stderr, "android_thread_end: no function %s\n", name);
        exit(1);
    }
    cpu.x[0] = a, cpu.x[1] = b, cpu.x[2] = c, cpu.x[3] = d;
    s->handler(&cpu, s->data);
    return cpu.x[0];
}
#if defined(__linux__)
static size_t open_files(void) {
    size_t n = 0;
    DIR* d = opendir("/proc/self/fd");
    if (!d) return 0;
    while (readdir(d)) ++n;
    closedir(d);
    return n;
}
static size_t vm_kib(void) {
    size_t kib = 0;
    char line[128];
    FILE* f = fopen("/proc/self/status", "r");
    while (f && fgets(line, sizeof line, f)) sscanf(line, "VmSize: %zu", &kib);
    if (f) fclose(f);
    return kib;
}
#endif
static void run(void* (*fn)(void*), void* arg) {
    pthread_t t;
    assert(!pthread_create(&t, NULL, fn, arg));
    assert(!pthread_join(t, NULL));
}

#if defined(__linux__)
/* a thread with guest state: 8 MiB of guest stack */
static void* guest_state(void* p) {
    (void)p;
    assert(bridge_cpu() == bridge_cpu());
    return NULL;
}
static void* looper(void* p) {
    *(uint64_t*)p = call("ALooper_prepare", 0, 0, 0, 0);
    assert(*(uint64_t*)p && *(uint64_t*)p == call("ALooper_forThread", 0, 0, 0, 0));
    return NULL;
}
static void* kept_looper(void* p) {
    looper(p);
    call("ALooper_acquire", *(uint64_t*)p, 0, 0, 0);
    return NULL;
}

#endif

int main(void) {
    char error[256] = "";
    assert(bridge_init(NULL, error, sizeof error));
#if defined(__linux__)
    run(guest_state, NULL); /* the first one's own allocations */
    size_t vm = vm_kib();
    for (int i = 0; i < 64; ++i) run(guest_state, NULL);
    size_t grown = vm_kib() - vm;
    printf("android_thread_end: 64 threads with guest state grew the process by %zu KiB\n", grown);
    assert(grown < 128 * 1024); /* 64 guest stacks of 8 MiB are 512 MiB */

    uint64_t l = 0;
    run(looper, &l);
    size_t files = open_files();
    for (int i = 0; i < 32; ++i) run(looper, &l);
    assert(open_files() == files);
    run(kept_looper, &l); /* acquired: the set outlives its thread */
    assert(open_files() == files + 1);
    call("ALooper_release", l, 0, 0, 0);
    assert(open_files() == files);
#endif
    puts("android_thread_end: ok");
    return 0;
}
