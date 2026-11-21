/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host CPUs on Linux (common/platform/thread_spawn.c, with plat_posix.c's hooks): plat_cpus lists the process's
   own and plat_cpu_count counts them, plat_thread_cpus keeps the calling thread on the ones given, and a thread
   plat_thread starts runs on all of them again, not only on the share of the game thread that started it (thread.c
   core_pin). A thread a library starts with pthread_create takes its starter's CPUs: plat_thread_cpus_widen gives the
   starter all of them first, and what it gives back puts the starter back on its own */
#include "nx/runtime/platform/plat_posix.c"
#include <stdarg.h>

void log_crash_mode(void) {}
/* nce.c, whose NCE no game here is of: the crash handler gives it the signal's context */
void nce_crash_context(const void* context, const void* fault) {
    (void)context;
    (void)fault;
}
void crash_report(void* host_pc, void* fault, int sig) {
    (void)host_pc;
    (void)fault;
    (void)sig;
}
void fatal(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    abort();
}
void logf_(int lvl, const char* fmt, ...) {
    va_list a;
    (void)lvl;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
}

static unsigned checks, failures;
#define CHECK(expr, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "plat_cpus.c:%d: %s: ", __LINE__, #expr);                                                  \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

/* the CPUs the calling thread may run on, as plat_cpus lists them */
static int mine(int* ids, int max) {
    cpu_set_t s;
    int c, n = 0;
    if (sched_getaffinity(0, sizeof s, &s)) return -1;
    for (c = 0; c < CPU_SETSIZE && n < max; c++)
        if (CPU_ISSET(c, &s)) ids[n++] = c;
    return n;
}
static int g_all[CPU_SETSIZE], g_nall;
static int g_child[CPU_SETSIZE], g_nchild = -1, g_parent_done;
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done = PTHREAD_COND_INITIALIZER;
static void child(void* p) {
    (void)p;
    pthread_mutex_lock(&g_lk);
    g_nchild = mine(g_child, CPU_SETSIZE);
    pthread_cond_broadcast(&g_done);
    pthread_mutex_unlock(&g_lk);
}
/* a thread a library starts (pthread_create, as FFmpeg's workers): the CPUs it runs on */
static int g_lib[CPU_SETSIZE], g_nlib;
static void* lib_child(void* p) {
    (void)p;
    g_nlib = mine(g_lib, CPU_SETSIZE);
    return NULL;
}
static int lib_thread(void) {
    pthread_t t;
    g_nlib = -1;
    if (pthread_create(&t, NULL, lib_child, NULL)) fatal("pthread_create failed\n");
    pthread_join(t, NULL);
    return g_nlib;
}
/* restricted to the first CPU, as a game thread to its core's share, it starts a thread as a library does, before and
   after widening, then a thread with plat_thread */
static void parent(void* p) {
    int one = g_all[0], got[CPU_SETSIZE], saved[CPU_SETSIZE], n, k;
    (void)p;
    plat_thread_cpus(&one, 1);
    n = mine(got, CPU_SETSIZE);
    CHECK(n == 1 && got[0] == one, "plat_thread_cpus(%d): the thread runs on %d CPUs, the first %d", one, n, got[0]);
    CHECK(lib_thread() == 1 && g_lib[0] == one, "a library's thread took %d CPUs of its starter's 1", g_nlib);
    k = plat_thread_cpus_widen(saved, CPU_SETSIZE);
    n = mine(got, CPU_SETSIZE);
    CHECK(k == 1 && saved[0] == one && n == g_nall, "widened: %d CPUs kept (the first %d), the thread on %d of %d", k,
          saved[0], n, g_nall);
    CHECK(lib_thread() == g_nall && !memcmp(g_lib, g_all, sizeof *g_all * (size_t)g_nall),
          "a library's thread started widened runs on %d CPUs, not the process's %d", g_nlib, g_nall);
    plat_thread_cpus(saved, k);
    n = mine(got, CPU_SETSIZE);
    CHECK(n == 1 && got[0] == one, "given back: the thread runs on %d CPUs, the first %d", n, got[0]);
    if (plat_thread(child, NULL, 0)) fatal("plat_thread failed\n");
    /* plat_thread returns only once its attributes are destroyed, and glibc keeps the affinity's cpuset in them: the
       process waits for that here, as its leak check at exit would read them as a leak while it runs */
    pthread_mutex_lock(&g_lk);
    g_parent_done = 1;
    pthread_cond_broadcast(&g_done);
    pthread_mutex_unlock(&g_lk);
}

int main(void) {
    int ids[CPU_SETSIZE], n;
    g_nall = mine(g_all, CPU_SETSIZE);
    n = plat_cpus(ids, CPU_SETSIZE);
    CHECK(n == g_nall && !memcmp(ids, g_all, sizeof *ids * (size_t)(n > 0 ? n : 0)),
          "plat_cpus: %d CPUs, the thread %d", n, g_nall);
    CHECK(plat_cpus(ids, 1) == 1 && ids[0] == g_all[0], "plat_cpus with room for 1: CPU %d", ids[0]);
    CHECK(plat_cpu_count() == g_nall, "plat_cpu_count: %d CPUs, the thread %d", plat_cpu_count(), g_nall);
    CHECK(plat_thread_cpus_widen(ids, CPU_SETSIZE) == 0, "a thread on all of them has nothing to give back");
    if (g_nall < 2) {
        printf("1 host CPU: nothing to restrict, %u checks, %u failed\n", checks, failures);
        return failures != 0;
    }
    if (plat_thread(parent, NULL, 0)) fatal("plat_thread failed\n");
    pthread_mutex_lock(&g_lk);
    while (g_nchild < 0 || !g_parent_done) pthread_cond_wait(&g_done, &g_lk);
    pthread_mutex_unlock(&g_lk);
    CHECK(g_nchild == g_nall && !memcmp(g_child, g_all, sizeof *g_all * (size_t)g_nall),
          "the thread a restricted thread started runs on %d CPUs, not the process's %d", g_nchild, g_nall);
    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
