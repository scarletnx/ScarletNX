/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The calling thread at the system's low priority (common.c's snx_thread_priority_low), which the pipeline workers
   past the first two take, read back from the system. On Linux and Android the thread's nice goes SNX_THREAD_LOW_NICE
   above its own, and only that thread's: the main thread and a thread started after keep theirs. A thread at nice 19
   already stays there, and why names that nice. On Windows the thread runs below normal, on macOS and iOS in the
   utility QoS class; on FreeBSD in the idle class, or the system refuses it to a user without the privilege and why
   names the class the thread stays in; elsewhere it is refused */
#include "common/utilities/common.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <errno.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/rtprio.h>
#endif

static int g_failures;
#define CHECK(ok, ...)                                                                                                 \
    do {                                                                                                               \
        if (!(ok)) {                                                                                                   \
            fprintf(stderr, "thread_priority.c:%d: %s: ", __LINE__, #ok);                                              \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* the calling thread's priority as the system says it: its nice on Linux, its thread priority on Windows, its QoS
   class on Apple systems, its rtprio class on FreeBSD (or 0 where none is read) */
static int priority_now(void) {
#if defined(_WIN32)
    return GetThreadPriority(GetCurrentThread());
#elif defined(__linux__)
    errno = 0;
    int nice = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    return nice == -1 && errno ? 1000 : nice;
#elif defined(__APPLE__)
    return (int)qos_class_self();
#elif defined(__FreeBSD__)
    struct rtprio now = {0, 0};
    return rtprio_thread(RTP_LOOKUP, 0, &now) ? -1 : RTP_PRIO_BASE(now.type);
#else
    return 0;
#endif
}

typedef struct {
    int at_19; /* set its nice to 19 first (Linux) */
    int lower; /* call snx_thread_priority_low */
    int before, after, result;
    char why[256];
} Run;
static void* run(void* arg) {
    Run* r = arg;
#if defined(__linux__)
    if (r->at_19) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
#endif
    r->before = priority_now();
    r->why[0] = 0;
    r->result = r->lower ? snx_thread_priority_low(r->why, sizeof r->why) : 0;
    r->after = priority_now();
    return NULL;
}
static Run start(int lower, int at_19) {
    Run r = {at_19, lower, 0, 0, 0, ""};
    pthread_t t;
    if (pthread_create(&t, NULL, run, &r) || pthread_join(t, NULL)) {
        fprintf(stderr, "thread_priority.c: a thread did not start\n");
        g_failures++;
    }
    return r;
}

int main(void) {
    int main_before = priority_now();
    Run low = start(1, 0), plain = start(0, 0);
    printf("main at %d; lowered thread %d to %d (%s, %s), a thread after at %d\n", main_before, low.before, low.after,
           low.result ? "refused" : "done", snx_thread_priority_low_name(), plain.after);
    CHECK(priority_now() == main_before, "main is at %d after a thread lowered itself, at %d before", priority_now(),
          main_before);
    CHECK(plain.after == main_before, "a thread started after runs at %d, main at %d", plain.after, main_before);
#if defined(__linux__)
    {
        /* SNX_THREAD_LOW_NICE above main's, 19 at most; a test run at nice 19 has nowhere lower */
        int want = main_before + SNX_THREAD_LOW_NICE < 19 ? main_before + SNX_THREAD_LOW_NICE : 19;
        CHECK(low.before == main_before && low.result == (main_before < 19 ? 0 : -1) && low.after == want,
              "the thread went from nice %d to %d (%d: %s), nice %d expected", low.before, low.after, low.result,
              low.why, want);
        CHECK(!strcmp(snx_thread_priority_low_name(), "nice +10"), "its name is \"%s\"",
              snx_thread_priority_low_name());
        /* a thread at 19 already: refused, with the nice it runs at */
        Run at_19 = start(1, 1);
        CHECK(at_19.before == 19 && at_19.result == -1 && at_19.after == 19 &&
                  strstr(at_19.why, "nice 19 is the lowest there is; it runs at nice 19"),
              "a thread at nice %d: %d, nice %d after, \"%s\"", at_19.before, at_19.result, at_19.after, at_19.why);
    }
#elif defined(_WIN32)
    CHECK(low.result == 0 && low.after == THREAD_PRIORITY_BELOW_NORMAL,
          "the thread runs at %d (%d: %s), below normal (%d) expected", low.after, low.result, low.why,
          THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
    CHECK(low.result == 0 && low.after == (int)QOS_CLASS_UTILITY,
          "the thread runs in QoS class %#x (%d: %s), utility (%#x) expected", (unsigned)low.after, low.result, low.why,
          (unsigned)QOS_CLASS_UTILITY);
#elif defined(__FreeBSD__)
    /* the idle class needs root, the idletime group with mac_priority or security.bsd.unprivileged_idprio=1 */
    if (low.result == 0)
        CHECK(low.after == RTP_PRIO_IDLE, "the thread runs in rtprio class %d, the idle class (%d) expected", low.after,
              RTP_PRIO_IDLE);
    else
        CHECK(low.after == RTP_PRIO_NORMAL && strstr(low.why, "rtprio_thread idle 0") &&
                  strstr(low.why, "it runs in the timeshare class"),
              "refused: class %d after, \"%s\"", low.after, low.why);
    printf("FreeBSD: the idle class %s%s\n", low.result ? "refused: " : "taken", low.why);
#else
    CHECK(low.result == -1 && low.why[0], "%d: \"%s\": this system has none, a refusal expected", low.result, low.why);
#endif
    if (g_failures) return 1;
    printf("thread_priority: ok\n");
    return 0;
}
