/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host CPUs of plat_posix.c on Linux: plat_cpus lists the process's own, plat_thread_cpus keeps the calling thread
   on the ones given, and a thread plat_thread starts runs on all of them again, not only on the share of the game
   thread that started it (thread.c core_pin) */
#include "nx/runtime/platform/plat_posix.c"
#include <stdarg.h>

void log_crash_mode(void) {}
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
static int g_child[CPU_SETSIZE], g_nchild = -1;
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done = PTHREAD_COND_INITIALIZER;
static void child(void* p) {
    (void)p;
    pthread_mutex_lock(&g_lk);
    g_nchild = mine(g_child, CPU_SETSIZE);
    pthread_cond_broadcast(&g_done);
    pthread_mutex_unlock(&g_lk);
}
/* restricted to the first CPU, as a game thread to its core's share, it starts a thread with plat_thread */
static void parent(void* p) {
    int one = g_all[0], got[CPU_SETSIZE], n;
    (void)p;
    plat_thread_cpus(&one, 1);
    n = mine(got, CPU_SETSIZE);
    CHECK(n == 1 && got[0] == one, "plat_thread_cpus(%d): the thread runs on %d CPUs, the first %d", one, n, got[0]);
    if (plat_thread(child, NULL, 0)) fatal("plat_thread failed\n");
}

int main(void) {
    int ids[CPU_SETSIZE], n;
    g_nall = mine(g_all, CPU_SETSIZE);
    n = plat_cpus(ids, CPU_SETSIZE);
    CHECK(n == g_nall && !memcmp(ids, g_all, sizeof *ids * (size_t)(n > 0 ? n : 0)),
          "plat_cpus: %d CPUs, the thread %d", n, g_nall);
    CHECK(plat_cpus(ids, 1) == 1 && ids[0] == g_all[0], "plat_cpus with room for 1: CPU %d", ids[0]);
    if (g_nall < 2) {
        printf("1 host CPU: nothing to restrict, %u checks, %u failed\n", checks, failures);
        return failures != 0;
    }
    if (plat_thread(parent, NULL, 0)) fatal("plat_thread failed\n");
    pthread_mutex_lock(&g_lk);
    while (g_nchild < 0) pthread_cond_wait(&g_done, &g_lk);
    pthread_mutex_unlock(&g_lk);
    CHECK(g_nchild == g_nall && !memcmp(g_child, g_all, sizeof *g_all * (size_t)g_nall),
          "the thread a restricted thread started runs on %d CPUs, not the process's %d", g_nchild, g_nall);
    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
