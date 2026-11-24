/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the native bridge keeps for a host thread (src/android/bridge/bridge.c, included whole for its statics) ends by
   what the thread's key holds, and by nothing of its thread-locals, which a host may have freed first: another thread,
   which has none of the owner's, ends it here. The guest stacks of both bridges, the looper's set, the guest JNIEnv and
   the current EGL context go, and what the ending thread has of its own stays */
#include "android/bridge/bridge.c"
#include "android/bridge/arm32.h"
#include "android/bridge/hle.h"
#include "android/bridge/jni.h"
#include <assert.h>
#include <dirent.h>
#include <sched.h>

enum { CLIENT_VERSION = 0x3098, NONE = 0x3038, NO_THREAD = 3 /* Linux's ESRCH */ };
static struct {
    uint64_t display, context;
    uint32_t self; /* the owner's 32-bit pthread_t */
    ThreadState* state;
    _Atomic int step;
} g_t;
static void reach(int step) { atomic_store_explicit(&g_t.step, step, memory_order_release); }
static void wait_for(int step) {
    while (atomic_load_explicit(&g_t.step, memory_order_acquire) < step) sched_yield();
}
/* the HLE's function name, called as guest code calls it */
static uint64_t call(const char* name, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3) {
    static _Thread_local CPU cpu;
    const HleSymbol* s = hle_find(name);
    assert(s && s->handler);
    cpu.x[0] = x0, cpu.x[1] = x1, cpu.x[2] = x2, cpu.x[3] = x3;
    s->handler(&cpu, s->data);
    return cpu.x[0];
}
static uint32_t call32(const char* name, const uint32_t* words, unsigned count) {
    return arm32_call(arm32_hle_symbol(name), words, count, NULL);
}
static uint64_t new_context(void) {
    const int32_t attributes[] = {CLIENT_VERSION, 2, NONE};
    uint64_t x = call("eglCreateContext", g_t.display, 0, 0, (uint64_t)(uintptr_t)attributes);
    assert(x);
    return x;
}
#if defined(__linux__)
static size_t open_files(void) {
    size_t n = 0;
    DIR* d = opendir("/proc/self/fd");
    assert(d);
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

static void* owner(void* arg) {
    (void)arg;
    assert(bridge_cpu());
    assert(call("ALooper_prepare", 0, 0, 0, 0));
    assert(call("eglMakeCurrent", g_t.display, 0, 0, g_t.context) == 1);
    assert(bridge_jni_env((JNIEnv*)(uintptr_t)16));
    assert(arm32_cpu());
    g_t.self = call32("pthread_self", NULL, 0);
    g_t.state = pthread_getspecific(b.end_key);
    assert(g_t.state);
    (void)pthread_setspecific(b.end_key, NULL); /* main runs the end in this thread's place */
    reach(1);
    wait_for(2);
    return NULL;
}

int main(void) {
    char error[256] = "";
    assert(bridge_init(NULL, error, sizeof error));
    assert(arm32_init(error, sizeof error));
    g_t.display = call("eglGetDisplay", 0, 0, 0, 0);
    /* main's own, which the owner's end leaves */
    uint64_t mine = new_context();
    assert(call("eglMakeCurrent", g_t.display, 0, 0, mine) == 1);
    const CPU* cpu = bridge_cpu();
    const SnxArmCPU* cpu32 = arm32_cpu();
    uint64_t env = bridge_jni_env((JNIEnv*)(uintptr_t)32);
    g_t.context = new_context();
    pthread_t t;
    assert(!pthread_create(&t, NULL, owner, NULL));
    wait_for(1);
#if defined(__linux__)
    size_t files = open_files(), vm = vm_kib();
#endif
    assert(call32("pthread_kill", (const uint32_t[]){g_t.self, 0}, 2) == 0);
    state_end(g_t.state);
#if defined(__linux__)
    assert(open_files() == files - 1);       /* the looper's set */
    assert(vm_kib() + 8 * 1024 - 256 <= vm); /* the 64-bit guest stack, 8 MiB */
#endif
    assert(call32("pthread_kill", (const uint32_t[]){g_t.self, 0}, 2) == NO_THREAD);
    assert(call("eglDestroyContext", g_t.display, g_t.context, 0, 0) == 1);
    assert(new_context() == g_t.context); /* no thread has it, so it was freed at once */
    assert(call("eglGetCurrentContext", 0, 0, 0, 0) == mine);
    assert(bridge_cpu() == cpu && arm32_cpu() == cpu32 && bridge_jni_env((JNIEnv*)(uintptr_t)32) == env);
    reach(2);
    assert(!pthread_join(t, NULL));
    puts("android_thread_state: ok");
    return 0;
}
