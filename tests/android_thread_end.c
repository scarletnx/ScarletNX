/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the native bridge keeps for a host thread (android/bridge/bridge.c's guest stack, hle_android.c's looper,
   hle_egl.c's current context) ends with the thread, whoever made it: a JVM's thread, one of a pool's, ends with no
   call into the bridge. And an EGL context that a thread has current stays until that thread lets go of it, whoever
   destroys it: freed at once, the thread's next call used a context that was no more */
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include <assert.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { EGL_CONTEXT_CLIENT_VERSION = 0x3098, EGL_NONE = 0x3038, GL_VERSION = 0x1f02, EGL_BAD_CONTEXT = 0x3006 };

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
static struct {
    uint64_t display, context;
    _Atomic int step; /* how far the two threads have got */
} g_t;
static void reach(int step) { atomic_store_explicit(&g_t.step, step, memory_order_release); }
static void wait_for(int step) {
    while (atomic_load_explicit(&g_t.step, memory_order_acquire) < step) sched_yield();
}
static uint64_t new_context(void) {
    const int32_t attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    uint64_t x = call("eglCreateContext", g_t.display, 0, 0, (uint64_t)(uintptr_t)attributes);
    assert(x);
    return x;
}
/* a thread that has the context current while main destroys it, then lets go of it */
static void* holder(void* p) {
    (void)p;
    assert(call("eglMakeCurrent", g_t.display, 0, 0, g_t.context) == 1);
    const char* version = (const char*)(uintptr_t)call("glGetString", GL_VERSION, 0, 0, 0);
    assert(version && strstr(version, "OpenGL ES 3.1"));
    reach(1);    /* current */
    wait_for(2); /* destroyed */
    version = (const char*)(uintptr_t)call("glGetString", GL_VERSION, 0, 0, 0);
    assert(version && strstr(version, "OpenGL ES 3.1"));
    assert(call("eglGetCurrentContext", 0, 0, 0, 0) == g_t.context);
    assert(call("eglMakeCurrent", g_t.display, 0, 0, 0) == 1);
    reach(3); /* let go */
    return NULL;
}
/* a thread that ends with a context current, as the JVM's do */
static void* leaver(void* p) {
    assert(call("eglMakeCurrent", g_t.display, 0, 0, *(uint64_t*)p) == 1);
    return NULL;
}

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
    g_t.display = call("eglGetDisplay", 0, 0, 0, 0);
    assert(g_t.display);
    pthread_t t;
    g_t.context = new_context();
    assert(!pthread_create(&t, NULL, holder, NULL));
    wait_for(1);
    assert(call("eglDestroyContext", g_t.display, g_t.context, 0, 0) == 1);
    /* not a handle any more */
    assert(call("eglMakeCurrent", g_t.display, 0, 0, g_t.context) == 0);
    assert(call("eglGetError", 0, 0, 0, 0) == EGL_BAD_CONTEXT);
    uint64_t other = new_context();
    assert(other != g_t.context); /* the holder's is not given to another */
    reach(2);
    wait_for(3); /* the holder's is free now */
    assert(!pthread_join(t, NULL));
    uint64_t again = new_context();
    assert(again == g_t.context);
    /* a thread that ends with one current lets go of it, so that destroying it after frees it */
    run(leaver, &again);
    assert(call("eglDestroyContext", g_t.display, again, 0, 0) == 1);
    assert(new_context() == again);
    puts("android_thread_end: ok");
    return 0;
}
